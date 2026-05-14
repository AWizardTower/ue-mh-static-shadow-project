// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowValidateCellSplitCommandlet.h"

#include "MHShadowCellDataAsset.h"
#include "MHShadowDataAsset.h"
#include "MHShadowWorldDataAsset.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"

namespace
{
static bool ParseStringParam(const FString& Params, const TCHAR* Key, FString& OutValue)
{
	if (FParse::Value(*Params, Key, OutValue))
	{
		OutValue.TrimStartAndEndInline();
		return !OutValue.IsEmpty();
	}
	return false;
}

static int32 ParseIntParam(const FString& Params, const TCHAR* Key, int32 DefaultValue)
{
	int32 Value = DefaultValue;
	FParse::Value(*Params, Key, Value);
	return Value;
}

static FString ToObjectPath(const FString& AssetPath)
{
	if (AssetPath.Contains(TEXT(".")))
	{
		return AssetPath;
	}

	const FString AssetName = FPackageName::GetShortName(AssetPath);
	return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

static int32 GetChildIndex(const FIntVector4& ChildIndices, int32 ChildSlot)
{
	switch (ChildSlot)
	{
	case 0:
		return ChildIndices.X;
	case 1:
		return ChildIndices.Y;
	case 2:
		return ChildIndices.Z;
	default:
		return ChildIndices.W;
	}
}

static bool SampleRepresentativeDepthFromTile(
	const TArray<FMHShadowNode>& Nodes,
	const FMHShadowTile& Tile,
	int32 LocalX,
	int32 LocalY,
	int32 TileSize,
	float& OutDepth)
{
	OutDepth = 1.0f;
	if (Tile.RootNodeIndex < 0 || Tile.NodeCount <= 0 || TileSize <= 0 || !Nodes.IsValidIndex(Tile.RootNodeIndex))
	{
		return false;
	}

	int32 NodeIndex = Tile.RootNodeIndex;
	FIntPoint NodeMin(0, 0);
	int32 NodeSize = TileSize;
	bool bHasDepth = false;

	for (int32 Step = 0; Step < 32; ++Step)
	{
		if (!Nodes.IsValidIndex(NodeIndex) || NodeIndex < Tile.NodeOffset || NodeIndex >= Tile.NodeOffset + Tile.NodeCount)
		{
			return bHasDepth;
		}

		const FMHShadowNode& Node = Nodes[NodeIndex];
		if (Node.bHasRepresentativeDepth)
		{
			OutDepth = Node.RepresentativeDepth;
			bHasDepth = true;
		}

		const bool bHasChildren = Node.ChildIndices.X >= 0
			|| Node.ChildIndices.Y >= 0
			|| Node.ChildIndices.Z >= 0
			|| Node.ChildIndices.W >= 0;
		if (!bHasChildren || NodeSize <= 1)
		{
			return bHasDepth;
		}

		const int32 ChildSize = FMath::Max(NodeSize / 2, 1);
		const int32 ChildX = (LocalX - NodeMin.X) >= ChildSize ? 1 : 0;
		const int32 ChildY = (LocalY - NodeMin.Y) >= ChildSize ? 1 : 0;
		const int32 ChildSlot = ChildY * 2 + ChildX;
		const int32 ChildNodeIndex = GetChildIndex(Node.ChildIndices, ChildSlot);
		if (ChildNodeIndex < Tile.NodeOffset || ChildNodeIndex >= Tile.NodeOffset + Tile.NodeCount)
		{
			return bHasDepth;
		}

		NodeIndex = ChildNodeIndex;
		NodeMin.X += ChildX * ChildSize;
		NodeMin.Y += ChildY * ChildSize;
		NodeSize = ChildSize;
	}

	return bHasDepth;
}

static FMatrix MakeMatrixFromRows(const FVector4& Row0, const FVector4& Row1, const FVector4& Row2, const FVector4& Row3)
{
	return FMatrix(
		FPlane(Row0.X, Row0.Y, Row0.Z, Row0.W),
		FPlane(Row1.X, Row1.Y, Row1.Z, Row1.W),
		FPlane(Row2.X, Row2.Y, Row2.Z, Row2.W),
		FPlane(Row3.X, Row3.Y, Row3.Z, Row3.W));
}

struct FTileAssignmentSample
{
	FVector2D UV = FVector2D::ZeroVector;
	FIntPoint LocalSample = FIntPoint::ZeroValue;
	bool bHasDepth = false;
	float RepresentativeDepth = 1.0f;
	FVector WorldPosition = FVector::ZeroVector;
};

static FTileAssignmentSample SampleTileAssignment(const FMHShadowClipmapLevel& Level, const FMHShadowTile& Tile, const TArray<FMHShadowNode>& Nodes)
{
	FTileAssignmentSample Sample;
	const int32 LocalCenter = FMath::Clamp(Level.TileSize / 2, 0, FMath::Max(0, Level.TileSize - 1));
	Sample.LocalSample = FIntPoint(LocalCenter, LocalCenter);
	Sample.bHasDepth = SampleRepresentativeDepthFromTile(Nodes, Tile, LocalCenter, LocalCenter, Level.TileSize, Sample.RepresentativeDepth);

	Sample.UV = FVector2D(
		(static_cast<double>(Tile.TexelRect.X + Tile.TexelRect.Z) * 0.5) / FMath::Max(1, Level.Resolution.X),
		(static_cast<double>(Tile.TexelRect.Y + Tile.TexelRect.W) * 0.5) / FMath::Max(1, Level.Resolution.Y));
	const FMatrix WorldToShadow = MakeMatrixFromRows(Level.WorldToShadowRow0, Level.WorldToShadowRow1, Level.WorldToShadowRow2, Level.WorldToShadowRow3);
	const FMatrix ShadowToWorld = WorldToShadow.Inverse();
	const FVector4 ShadowPosition(Sample.UV.X, Sample.UV.Y, Sample.RepresentativeDepth, 1.0);
	const FVector4 WorldPosition = ShadowToWorld.TransformFVector4(ShadowPosition);
	const double W = FMath::Abs(WorldPosition.W) > UE_SMALL_NUMBER ? WorldPosition.W : 1.0;
	Sample.WorldPosition = FVector(WorldPosition.X / W, WorldPosition.Y / W, WorldPosition.Z / W);
	return Sample;
}

static FIntPoint GetCellCoord(const FVector& WorldPosition, float CellSize)
{
	return FIntPoint(
		FMath::FloorToInt(WorldPosition.X / CellSize),
		FMath::FloorToInt(WorldPosition.Y / CellSize));
}

static FString GetValidationOutputDir()
{
	const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("CellSplitValidation"));
	IFileManager::Get().MakeDirectory(*OutputDir, true);
	return OutputDir;
}
}

UMHShadowValidateCellSplitCommandlet::UMHShadowValidateCellSplitCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowValidateCellSplitCommandlet::Main(const FString& Params)
{
	FString SourcePath = TEXT("/Game/MHShadow/Baked/MHShadowData_LightmassDual_LargeCacheStress_4096_Clipmap_Final");
	FString WorldPath = TEXT("/Game/MHShadow/Baked/LargeCacheStressCells/MHShadowWorld_LargeCacheStress");
	ParseStringParam(Params, TEXT("Source="), SourcePath);
	ParseStringParam(Params, TEXT("World="), WorldPath);
	const bool bValidateAssignment = ParseIntParam(Params, TEXT("ValidateAssignment="), 1) != 0;
	const int32 SampleCount = FMath::Max(0, ParseIntParam(Params, TEXT("SampleCount="), 256));

	UMHShadowDataAsset* SourceAsset = LoadObject<UMHShadowDataAsset>(nullptr, *ToObjectPath(SourcePath));
	UMHShadowWorldDataAsset* WorldAsset = LoadObject<UMHShadowWorldDataAsset>(nullptr, *ToObjectPath(WorldPath));
	if (!SourceAsset || !WorldAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load source/world for cell validation. Source=%s loaded=%d World=%s loaded=%d"),
			*SourcePath,
			SourceAsset ? 1 : 0,
			*WorldPath,
			WorldAsset ? 1 : 0);
		return 1;
	}

	TArray<const UMHShadowCellDataAsset*> Cells;
	Cells.Reserve(WorldAsset->CellAssets.Num());
	int64 LargestCellBytes = 0;
	TMap<int32, const UMHShadowCellDataAsset*> TileOwnerByGlobalIndex;
	TSet<int32> DuplicateTileOwners;
	for (const TSoftObjectPtr<UMHShadowCellDataAsset>& CellRef : WorldAsset->CellAssets)
	{
		UMHShadowCellDataAsset* Cell = CellRef.LoadSynchronous();
		if (!Cell)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to load MH shadow cell asset: %s"), *CellRef.ToSoftObjectPath().ToString());
			return 1;
		}
		Cells.Add(Cell);
		LargestCellBytes = FMath::Max(LargestCellBytes, Cell->EstimatedCompressedBytes);
		for (const int32 GlobalTileIndex : Cell->GlobalClipmapTileIndices)
		{
			if (TileOwnerByGlobalIndex.Contains(GlobalTileIndex))
			{
				DuplicateTileOwners.Add(GlobalTileIndex);
			}
			else
			{
				TileOwnerByGlobalIndex.Add(GlobalTileIndex, Cell);
			}
		}
	}

	FMHShadowCellFlattenedData Flattened;
	FString FlattenError;
	if (!WorldAsset->BuildFlattenedData(Cells, Flattened, &FlattenError))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to flatten cell split for validation: %s"), *FlattenError);
		return 1;
	}

	const int32 SourceTileCount = SourceAsset->ClipmapTiles.Num();
	const bool bBasicShapeValid = SourceTileCount > 0
		&& SourceTileCount == SourceAsset->ClipmapPageTable.Num()
		&& SourceTileCount == Flattened.ClipmapTiles.Num()
		&& SourceTileCount == Flattened.ClipmapPageTable.Num()
		&& SourceAsset->ClipmapLevels.Num() == Flattened.ClipmapLevels.Num();
	if (!bBasicShapeValid)
	{
		UE_LOG(LogTemp, Error, TEXT("Cell split shape mismatch: sourceTiles=%d sourcePageTable=%d flatTiles=%d flatPageTable=%d sourceLevels=%d flatLevels=%d"),
			SourceAsset->ClipmapTiles.Num(),
			SourceAsset->ClipmapPageTable.Num(),
			Flattened.ClipmapTiles.Num(),
			Flattened.ClipmapPageTable.Num(),
			SourceAsset->ClipmapLevels.Num(),
			Flattened.ClipmapLevels.Num());
		return 1;
	}

	constexpr float Tolerance = 1.0e-6f;
	int32 MissingTiles = 0;
	int32 MismatchTexels = 0;
	int32 CheckedTexels = 0;
	int32 MissingRepresentativeTexels = 0;
	double MaxAbsError = 0.0;
	TArray<FString> ErrorRows;
	ErrorRows.Reserve(64);

	for (int32 TileIndex = 0; TileIndex < SourceTileCount; ++TileIndex)
	{
		const FMHShadowTile& SourceTile = SourceAsset->ClipmapTiles[TileIndex];
		const FMHShadowTile& FlatTile = Flattened.ClipmapTiles[TileIndex];
		if (FlatTile.RootNodeIndex < 0 || FlatTile.NodeCount <= 0 || Flattened.ClipmapPageTable[TileIndex] != TileIndex)
		{
			++MissingTiles;
			if (ErrorRows.Num() < 64)
			{
				ErrorRows.Add(FString::Printf(TEXT("MissingTile,%d,%d,%d,0,0,0"), TileIndex, SourceTile.TileCoord.X, SourceTile.TileCoord.Y));
			}
			continue;
		}

		const int32 TileSize = WorldAsset->TileSize;
		for (int32 LocalY = 0; LocalY < TileSize; ++LocalY)
		{
			for (int32 LocalX = 0; LocalX < TileSize; ++LocalX)
			{
				float SourceDepth = 1.0f;
				float FlatDepth = 1.0f;
				const bool bSourceHasDepth = SampleRepresentativeDepthFromTile(SourceAsset->ClipmapNodes, SourceTile, LocalX, LocalY, TileSize, SourceDepth);
				const bool bFlatHasDepth = SampleRepresentativeDepthFromTile(Flattened.ClipmapNodes, FlatTile, LocalX, LocalY, TileSize, FlatDepth);
				++CheckedTexels;
				if (bSourceHasDepth != bFlatHasDepth)
				{
					++MissingRepresentativeTexels;
				}
				const double AbsError = FMath::Abs(static_cast<double>(SourceDepth) - static_cast<double>(FlatDepth));
				MaxAbsError = FMath::Max(MaxAbsError, AbsError);
				if (bSourceHasDepth != bFlatHasDepth || AbsError > Tolerance)
				{
					++MismatchTexels;
					if (ErrorRows.Num() < 64)
					{
						ErrorRows.Add(FString::Printf(
							TEXT("Mismatch,%d,%d,%d,%d,%d,%.9f,%.9f"),
							TileIndex,
							SourceTile.TileCoord.X,
							SourceTile.TileCoord.Y,
							LocalX,
							LocalY,
							SourceDepth,
							FlatDepth));
					}
				}
			}
		}
	}

	int32 CheckedAssignmentTiles = 0;
	int32 AssignmentMismatchTiles = 0;
	int32 MissingTileOwner = 0;
	int32 DuplicateTileOwner = 0;
	int32 DepthlessTiles = 0;
	TSet<FIntPoint> CellsTouched;
	FString AssignmentCsv = TEXT("Status,GlobalTileIndex,Level,LocalTileIndex,TileX,TileY,CenterU,CenterV,SampleLocalX,SampleLocalY,HasDepth,RepresentativeDepth,ExpectedCellX,ExpectedCellY,ActualCellX,ActualCellY,WorldX,WorldY,WorldZ,CellAsset\n");
	FString SampleCsv = AssignmentCsv;
	int32 SamplesWritten = 0;

	if (bValidateAssignment)
	{
		for (const FMHShadowClipmapLevel& Level : SourceAsset->ClipmapLevels)
		{
			for (int32 LocalTileIndex = 0; LocalTileIndex < Level.TileDataCount; ++LocalTileIndex)
			{
				const int32 GlobalTileIndex = Level.TileOffset + LocalTileIndex;
				if (!SourceAsset->ClipmapTiles.IsValidIndex(GlobalTileIndex))
				{
					continue;
				}

				const FMHShadowTile& SourceTile = SourceAsset->ClipmapTiles[GlobalTileIndex];
				const FTileAssignmentSample Assignment = SampleTileAssignment(Level, SourceTile, SourceAsset->ClipmapNodes);
				const FIntPoint ExpectedCellCoord = GetCellCoord(Assignment.WorldPosition, WorldAsset->CellSize);
				const UMHShadowCellDataAsset* const* OwnerPtr = TileOwnerByGlobalIndex.Find(GlobalTileIndex);
				const UMHShadowCellDataAsset* Owner = OwnerPtr ? *OwnerPtr : nullptr;
				const FIntPoint ActualCellCoord = Owner ? Owner->CellCoord : FIntPoint(MIN_int32, MIN_int32);
				const bool bMissingOwner = Owner == nullptr;
				const bool bDuplicateOwner = DuplicateTileOwners.Contains(GlobalTileIndex);
				const bool bMismatch = bMissingOwner || bDuplicateOwner || ActualCellCoord != ExpectedCellCoord;
				const FString Status = bMissingOwner
					? TEXT("MissingOwner")
					: bDuplicateOwner
						? TEXT("DuplicateOwner")
						: (bMismatch ? TEXT("AssignmentMismatch") : TEXT("OK"));
				const FString CellAssetPath = Owner ? Owner->GetPathName() : FString();

				++CheckedAssignmentTiles;
				DepthlessTiles += Assignment.bHasDepth ? 0 : 1;
				MissingTileOwner += bMissingOwner ? 1 : 0;
				DuplicateTileOwner += bDuplicateOwner ? 1 : 0;
				AssignmentMismatchTiles += bMismatch ? 1 : 0;
				CellsTouched.Add(ExpectedCellCoord);

				const FString Row = FString::Printf(
					TEXT("%s,%d,%d,%d,%d,%d,%.8f,%.8f,%d,%d,%d,%.9f,%d,%d,%d,%d,%.3f,%.3f,%.3f,%s\n"),
					*Status,
					GlobalTileIndex,
					Level.LevelIndex,
					LocalTileIndex,
					SourceTile.TileCoord.X,
					SourceTile.TileCoord.Y,
					Assignment.UV.X,
					Assignment.UV.Y,
					Assignment.LocalSample.X,
					Assignment.LocalSample.Y,
					Assignment.bHasDepth ? 1 : 0,
					Assignment.RepresentativeDepth,
					ExpectedCellCoord.X,
					ExpectedCellCoord.Y,
					ActualCellCoord.X,
					ActualCellCoord.Y,
					Assignment.WorldPosition.X,
					Assignment.WorldPosition.Y,
					Assignment.WorldPosition.Z,
					*CellAssetPath);
				AssignmentCsv += Row;
				if ((SamplesWritten < SampleCount) || bMismatch)
				{
					SampleCsv += Row;
					++SamplesWritten;
				}
			}
		}
	}

	const FString OutputDir = GetValidationOutputDir();
	FString ErrorCsv = TEXT("Type,TileIndex,TileX,TileY,LocalX,LocalY,SourceDepth,FlattenedDepth\n");
	for (const FString& ErrorRow : ErrorRows)
	{
		ErrorCsv += ErrorRow + TEXT("\n");
	}
	FString Csv = TEXT("Metric,Value\n");
	Csv += FString::Printf(TEXT("Cells,%d\n"), Cells.Num());
	Csv += FString::Printf(TEXT("SourceTiles,%d\n"), SourceTileCount);
	Csv += FString::Printf(TEXT("LoadedVirtualPages,%d\n"), Flattened.LoadedVirtualPageCount);
	Csv += FString::Printf(TEXT("UnavailableVirtualPages,%d\n"), Flattened.UnavailableVirtualPageCount);
	Csv += FString::Printf(TEXT("CheckedTexels,%d\n"), CheckedTexels);
	Csv += FString::Printf(TEXT("MissingTiles,%d\n"), MissingTiles);
	Csv += FString::Printf(TEXT("MissingRepresentativeTexels,%d\n"), MissingRepresentativeTexels);
	Csv += FString::Printf(TEXT("MismatchTexels,%d\n"), MismatchTexels);
	Csv += FString::Printf(TEXT("MaxAbsError,%.12f\n"), MaxAbsError);
	Csv += FString::Printf(TEXT("ProviderMemoryBytes,%lld\n"), Flattened.ProviderMemoryBytes);
	Csv += FString::Printf(TEXT("LargestCellBytes,%lld\n"), LargestCellBytes);
	Csv += FString::Printf(TEXT("MonolithicCompressedBytes,%lld\n"), SourceAsset->Stats.CompressedBytes);
	Csv += FString::Printf(TEXT("ValidateAssignment,%d\n"), bValidateAssignment ? 1 : 0);
	Csv += FString::Printf(TEXT("CheckedAssignmentTiles,%d\n"), CheckedAssignmentTiles);
	Csv += FString::Printf(TEXT("AssignmentMismatchTiles,%d\n"), AssignmentMismatchTiles);
	Csv += FString::Printf(TEXT("MissingTileOwner,%d\n"), MissingTileOwner);
	Csv += FString::Printf(TEXT("DuplicateTileOwner,%d\n"), DuplicateTileOwner);
	Csv += FString::Printf(TEXT("DepthlessTiles,%d\n"), DepthlessTiles);
	Csv += FString::Printf(TEXT("CellsTouched,%d\n"), CellsTouched.Num());
	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(OutputDir, TEXT("CellSplitValidation.csv")));
	FFileHelper::SaveStringToFile(ErrorCsv, *FPaths::Combine(OutputDir, TEXT("CellSplitRepresentativeErrors.csv")));
	if (bValidateAssignment)
	{
		FFileHelper::SaveStringToFile(AssignmentCsv, *FPaths::Combine(OutputDir, TEXT("TileAssignmentValidation.csv")));
		FFileHelper::SaveStringToFile(SampleCsv, *FPaths::Combine(OutputDir, TEXT("TileAssignmentSamples.csv")));
	}

	UE_LOG(LogTemp, Display, TEXT("MH cell split validation: source=%s world=%s cells=%d loadedPages=%d unavailablePages=%d checkedTexels=%d missingTiles=%d mismatchTexels=%d maxAbsError=%.12f checkedAssignmentTiles=%d assignmentMismatch=%d missingOwners=%d duplicateOwners=%d depthlessTiles=%d cellsTouched=%d providerBytes=%lld largestCellBytes=%lld monolithicCompressedBytes=%lld csv=%s"),
		*SourcePath,
		*WorldPath,
		Cells.Num(),
		Flattened.LoadedVirtualPageCount,
		Flattened.UnavailableVirtualPageCount,
		CheckedTexels,
		MissingTiles,
		MismatchTexels,
		MaxAbsError,
		CheckedAssignmentTiles,
		AssignmentMismatchTiles,
		MissingTileOwner,
		DuplicateTileOwner,
		DepthlessTiles,
		CellsTouched.Num(),
		Flattened.ProviderMemoryBytes,
		LargestCellBytes,
		SourceAsset->Stats.CompressedBytes,
		*OutputDir);

	const bool bAssignmentValid = !bValidateAssignment || (AssignmentMismatchTiles == 0 && MissingTileOwner == 0 && DuplicateTileOwner == 0);
	return MissingTiles == 0 && MismatchTexels == 0 && Flattened.UnavailableVirtualPageCount == 0 && bAssignmentValid ? 0 : 1;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowSplitToCellsCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MHShadowCellDataAsset.h"
#include "MHShadowDataAsset.h"
#include "MHShadowWorldDataAsset.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

namespace
{
struct FSplitCellBuilder
{
	FIntPoint Coord = FIntPoint::ZeroValue;
	FBox Bounds = FBox(EForceInit::ForceInit);
	TArray<int32> GlobalTileIndices;
	TArray<FMHShadowTile> Tiles;
	TArray<FMHShadowNode> Nodes;
	int32 ValidTileCount = 0;
	int32 TotalValidTexels = 0;
};

static bool ParseStringParam(const FString& Params, const TCHAR* Key, FString& OutValue)
{
	if (FParse::Value(*Params, Key, OutValue))
	{
		OutValue.TrimStartAndEndInline();
		return !OutValue.IsEmpty();
	}
	return false;
}

static float ParseFloatParam(const FString& Params, const TCHAR* Key, float DefaultValue)
{
	float Value = DefaultValue;
	FParse::Value(*Params, Key, Value);
	return Value;
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

static FString SanitizeAssetName(const FString& Name)
{
	FString Result = Name;
	for (TCHAR& Char : Result)
	{
		if (!FChar::IsAlnum(Char) && Char != TEXT('_'))
		{
			Char = TEXT('_');
		}
	}
	return Result;
}

static FString MakeCellAssetName(const FIntPoint& CellCoord)
{
	return FString::Printf(TEXT("MHShadowCell_%d_%d"), CellCoord.X, CellCoord.Y).Replace(TEXT("-"), TEXT("N"));
}

static FString MakeCellObjectPath(const FString& OutputRoot, const FIntPoint& CellCoord)
{
	const FString CellPackagePath = OutputRoot / TEXT("Cells") / MakeCellAssetName(CellCoord);
	return ToObjectPath(CellPackagePath);
}

static bool SaveAsset(UObject* Asset)
{
	if (!Asset)
	{
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	const FString PackageName = Package->GetName();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true);

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
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

static void SetChildIndex(FIntVector4& ChildIndices, int32 ChildSlot, int32 ChildIndex)
{
	switch (ChildSlot)
	{
	case 0:
		ChildIndices.X = ChildIndex;
		break;
	case 1:
		ChildIndices.Y = ChildIndex;
		break;
	case 2:
		ChildIndices.Z = ChildIndex;
		break;
	default:
		ChildIndices.W = ChildIndex;
		break;
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
	FVector4 WorldPosition = ShadowToWorld.TransformFVector4(ShadowPosition);
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

static int64 EstimateCellBytes(const FSplitCellBuilder& Cell)
{
	return static_cast<int64>(Cell.Nodes.Num()) * static_cast<int64>(sizeof(FMHShadowNode))
		+ static_cast<int64>(Cell.Tiles.Num()) * static_cast<int64>(sizeof(FMHShadowTile))
		+ static_cast<int64>(Cell.GlobalTileIndices.Num()) * static_cast<int64>(sizeof(int32));
}

static int64 EstimateRawDualBytes(const UMHShadowDataAsset& SourceAsset)
{
	if (SourceAsset.ClipmapRawIntervals.Num() > 0)
	{
		return static_cast<int64>(SourceAsset.ClipmapRawIntervals.Num()) * static_cast<int64>(sizeof(FVector4f));
	}

	int64 Bytes = 0;
	for (const FMHShadowClipmapLevel& Level : SourceAsset.ClipmapLevels)
	{
		Bytes += static_cast<int64>(FMath::Max(0, Level.Resolution.X))
			* static_cast<int64>(FMath::Max(0, Level.Resolution.Y))
			* static_cast<int64>(sizeof(FVector4f));
	}
	return Bytes;
}

static void AppendTileToCell(
	const UMHShadowDataAsset& SourceAsset,
	const FMHShadowTile& SourceTile,
	int32 GlobalTileIndex,
	const FVector& TileWorldPosition,
	float CellSize,
	FSplitCellBuilder& Cell)
{
	Cell.Bounds += TileWorldPosition;
	Cell.Bounds += FVector(
		(Cell.Coord.X + 1) * CellSize,
		(Cell.Coord.Y + 1) * CellSize,
		TileWorldPosition.Z);

	const int32 NodeLocalOffset = Cell.Nodes.Num();
	for (int32 NodeIndex = SourceTile.NodeOffset; NodeIndex < SourceTile.NodeOffset + SourceTile.NodeCount; ++NodeIndex)
	{
		if (!SourceAsset.ClipmapNodes.IsValidIndex(NodeIndex))
		{
			continue;
		}

		FMHShadowNode Node = SourceAsset.ClipmapNodes[NodeIndex];
		for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
		{
			const int32 OriginalChildIndex = GetChildIndex(Node.ChildIndices, ChildSlot);
			SetChildIndex(Node.ChildIndices, ChildSlot, OriginalChildIndex >= 0 ? OriginalChildIndex - SourceTile.NodeOffset + NodeLocalOffset : INDEX_NONE);
		}
		Node.IntervalIndex = INDEX_NONE;
		Cell.Nodes.Add(Node);
	}

	FMHShadowTile CellTile = SourceTile;
	CellTile.NodeOffset = NodeLocalOffset;
	CellTile.NodeCount = Cell.Nodes.Num() - NodeLocalOffset;
	CellTile.RootNodeIndex = SourceTile.RootNodeIndex >= 0 ? SourceTile.RootNodeIndex - SourceTile.NodeOffset + NodeLocalOffset : INDEX_NONE;
	CellTile.PageIndex = GlobalTileIndex;
	CellTile.bResidentDefault = true;
	Cell.GlobalTileIndices.Add(GlobalTileIndex);
	Cell.Tiles.Add(CellTile);
	Cell.ValidTileCount += SourceTile.ValidTexelCount > 0 ? 1 : 0;
	Cell.TotalValidTexels += SourceTile.ValidTexelCount;
}
}

UMHShadowSplitToCellsCommandlet::UMHShadowSplitToCellsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowSplitToCellsCommandlet::Main(const FString& Params)
{
	FString SourcePath = TEXT("/Game/MHShadow/Baked/MHShadowData_LightmassDual_LargeCacheStress_4096_Clipmap_Final");
	FString OutputRoot = TEXT("/Game/MHShadow/Baked/LargeCacheStressCells");
	ParseStringParam(Params, TEXT("Source="), SourcePath);
	ParseStringParam(Params, TEXT("OutputRoot="), OutputRoot);
	const float CellSize = FMath::Max(1.0f, ParseFloatParam(Params, TEXT("CellSize="), 1000.0f));
	const bool bIncludeRawDebug = ParseIntParam(Params, TEXT("IncludeRawDebug="), 0) != 0;

	UMHShadowDataAsset* SourceAsset = LoadObject<UMHShadowDataAsset>(nullptr, *ToObjectPath(SourcePath));
	if (!SourceAsset || !SourceAsset->IsValidForRendering() || SourceAsset->ClipmapLevels.Num() == 0 || SourceAsset->ClipmapTiles.Num() == 0 || SourceAsset->ClipmapNodes.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid source MH shadow asset for cell split: %s"), *SourcePath);
		return 1;
	}

	OutputRoot.RemoveFromEnd(TEXT("/"));
	const FString OutputStem = SanitizeAssetName(FPackageName::GetShortName(OutputRoot).Replace(TEXT("Cells"), TEXT("")));
	const FString WorldPackagePath = OutputRoot / FString::Printf(TEXT("MHShadowWorld_%s"), *OutputStem);
	const FString WorldObjectPath = ToObjectPath(WorldPackagePath);
	const FString WorldPackageName = FPackageName::ObjectPathToPackageName(WorldObjectPath);
	UMHShadowWorldDataAsset* WorldAsset = LoadObject<UMHShadowWorldDataAsset>(nullptr, *WorldObjectPath);
	UPackage* WorldPackage = WorldAsset ? WorldAsset->GetOutermost() : CreatePackage(*WorldPackageName);
	const FString WorldAssetName = FPackageName::GetShortName(WorldPackagePath);
	if (!WorldAsset)
	{
		WorldAsset = NewObject<UMHShadowWorldDataAsset>(WorldPackage, *WorldAssetName, RF_Public | RF_Standalone | RF_Transactional);
	}
	WorldAsset->Modify();
	WorldAsset->BakeSource = SourceAsset->BakeSource;
	WorldAsset->ProjectionMapping = SourceAsset->ProjectionMapping;
	WorldAsset->Resolution = SourceAsset->Resolution;
	WorldAsset->TileSize = SourceAsset->TileSize;
	WorldAsset->TileCount = SourceAsset->TileCount;
	WorldAsset->DepthBias = SourceAsset->DepthBias;
	WorldAsset->WorldToShadowRow0 = SourceAsset->WorldToShadowRow0;
	WorldAsset->WorldToShadowRow1 = SourceAsset->WorldToShadowRow1;
	WorldAsset->WorldToShadowRow2 = SourceAsset->WorldToShadowRow2;
	WorldAsset->WorldToShadowRow3 = SourceAsset->WorldToShadowRow3;
	WorldAsset->ClipmapLevels = SourceAsset->ClipmapLevels;
	WorldAsset->CellSize = CellSize;
	WorldAsset->SourceMonolithicAsset = FSoftObjectPath(SourceAsset);
	WorldAsset->CellAssets.Reset();

	TMap<FIntPoint, FSplitCellBuilder> Cells;
	FString TileCsv = TEXT("GlobalTileIndex,Level,LocalTileIndex,TileX,TileY,CenterU,CenterV,SampleLocalX,SampleLocalY,HasDepth,RepresentativeDepth,CellX,CellY,WorldX,WorldY,WorldZ,ValidTexels,NodeCount,AssignmentMethod,CellAsset\n");
	int32 TotalVirtualPages = 0;
	int32 NonEmptyVirtualPages = 0;

	for (const FMHShadowClipmapLevel& Level : SourceAsset->ClipmapLevels)
	{
		for (int32 LocalTileIndex = 0; LocalTileIndex < Level.TileDataCount; ++LocalTileIndex)
		{
			const int32 GlobalTileIndex = Level.TileOffset + LocalTileIndex;
			if (!SourceAsset->ClipmapTiles.IsValidIndex(GlobalTileIndex))
			{
				UE_LOG(LogTemp, Error, TEXT("Source clipmap tile index out of range: %d"), GlobalTileIndex);
				return 1;
			}

			const FMHShadowTile& SourceTile = SourceAsset->ClipmapTiles[GlobalTileIndex];
			const FTileAssignmentSample AssignmentSample = SampleTileAssignment(Level, SourceTile, SourceAsset->ClipmapNodes);
			const FIntPoint CellCoord = GetCellCoord(AssignmentSample.WorldPosition, CellSize);
			FSplitCellBuilder& Cell = Cells.FindOrAdd(CellCoord);
			Cell.Coord = CellCoord;
			AppendTileToCell(*SourceAsset, SourceTile, GlobalTileIndex, AssignmentSample.WorldPosition, CellSize, Cell);

			++TotalVirtualPages;
			NonEmptyVirtualPages += SourceTile.ValidTexelCount > 0 ? 1 : 0;
			TileCsv += FString::Printf(
				TEXT("%d,%d,%d,%d,%d,%.8f,%.8f,%d,%d,%d,%.9f,%d,%d,%.3f,%.3f,%.3f,%d,%d,DepthWorldPosition,%s\n"),
				GlobalTileIndex,
				Level.LevelIndex,
				LocalTileIndex,
				SourceTile.TileCoord.X,
				SourceTile.TileCoord.Y,
				AssignmentSample.UV.X,
				AssignmentSample.UV.Y,
				AssignmentSample.LocalSample.X,
				AssignmentSample.LocalSample.Y,
				AssignmentSample.bHasDepth ? 1 : 0,
				AssignmentSample.RepresentativeDepth,
				CellCoord.X,
				CellCoord.Y,
				AssignmentSample.WorldPosition.X,
				AssignmentSample.WorldPosition.Y,
				AssignmentSample.WorldPosition.Z,
				SourceTile.ValidTexelCount,
				SourceTile.NodeCount,
				*MakeCellObjectPath(OutputRoot, CellCoord));
		}
	}

	TArray<FIntPoint> SortedCellCoords;
	Cells.GetKeys(SortedCellCoords);
	SortedCellCoords.Sort([](const FIntPoint& A, const FIntPoint& B)
	{
		return A.Y == B.Y ? A.X < B.X : A.Y < B.Y;
	});

	FIntPoint MinCoord(MAX_int32, MAX_int32);
	FIntPoint MaxCoord(MIN_int32, MIN_int32);
	int32 CellIndex = 0;
	int64 TotalCompressedBytes = 0;
	int64 LargestCellBytes = 0;
	FString SplitCsv = TEXT("CellIndex,CellX,CellY,TileCount,ValidTileCount,ValidTexels,Nodes,EstimatedBytes,Asset\n");

	for (const FIntPoint& CellCoord : SortedCellCoords)
	{
		FSplitCellBuilder& Cell = Cells[CellCoord];
		MinCoord.X = FMath::Min(MinCoord.X, CellCoord.X);
		MinCoord.Y = FMath::Min(MinCoord.Y, CellCoord.Y);
		MaxCoord.X = FMath::Max(MaxCoord.X, CellCoord.X);
		MaxCoord.Y = FMath::Max(MaxCoord.Y, CellCoord.Y);

		const FString CellAssetName = MakeCellAssetName(CellCoord);
		const FString CellPackagePath = OutputRoot / TEXT("Cells") / CellAssetName;
		const FString CellObjectPath = ToObjectPath(CellPackagePath);
		UMHShadowCellDataAsset* CellAsset = LoadObject<UMHShadowCellDataAsset>(nullptr, *CellObjectPath);
		UPackage* CellPackage = CellAsset ? CellAsset->GetOutermost() : CreatePackage(*FPackageName::ObjectPathToPackageName(CellObjectPath));
		if (!CellAsset)
		{
			CellAsset = NewObject<UMHShadowCellDataAsset>(CellPackage, *CellAssetName, RF_Public | RF_Standalone | RF_Transactional);
		}
		CellAsset->Modify();
		const int64 CellEstimatedBytes = EstimateCellBytes(Cell);
		CellAsset->WorldData = TSoftObjectPtr<UMHShadowWorldDataAsset>(FSoftObjectPath(WorldAsset));
		CellAsset->CellCoord = CellCoord;
		CellAsset->CellIndex = CellIndex;
		CellAsset->WorldBounds = Cell.Bounds;
		CellAsset->GlobalClipmapTileIndices = MoveTemp(Cell.GlobalTileIndices);
		CellAsset->ClipmapTiles = MoveTemp(Cell.Tiles);
		CellAsset->ClipmapNodes = MoveTemp(Cell.Nodes);
		CellAsset->ValidTileCount = Cell.ValidTileCount;
		CellAsset->TotalValidTexels = Cell.TotalValidTexels;
		CellAsset->EstimatedCompressedBytes = CellEstimatedBytes;
		TotalCompressedBytes += CellAsset->EstimatedCompressedBytes;
		LargestCellBytes = FMath::Max(LargestCellBytes, CellAsset->EstimatedCompressedBytes);

		CellPackage->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(CellAsset);
		if (!SaveAsset(CellAsset))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to save MH shadow cell asset: %s"), *CellObjectPath);
			return 1;
		}

		WorldAsset->CellAssets.Add(TSoftObjectPtr<UMHShadowCellDataAsset>(FSoftObjectPath(CellAsset)));
		SplitCsv += FString::Printf(
			TEXT("%d,%d,%d,%d,%d,%d,%d,%lld,%s\n"),
			CellIndex,
			CellCoord.X,
			CellCoord.Y,
			CellAsset->GlobalClipmapTileIndices.Num(),
			CellAsset->ValidTileCount,
			CellAsset->TotalValidTexels,
			CellAsset->ClipmapNodes.Num(),
			CellAsset->EstimatedCompressedBytes,
			*CellObjectPath);
		++CellIndex;
	}

	if (SortedCellCoords.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("Cell split produced no cells."));
		return 1;
	}

	WorldAsset->CellMinCoord = MinCoord;
	WorldAsset->CellCount = FIntPoint(MaxCoord.X - MinCoord.X + 1, MaxCoord.Y - MinCoord.Y + 1);
	WorldAsset->TotalVirtualPages = TotalVirtualPages;
	WorldAsset->TotalRawDualBytes = EstimateRawDualBytes(*SourceAsset);
	WorldAsset->TotalCompressedBytes = TotalCompressedBytes;

	WorldPackage->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(WorldAsset);
	if (!SaveAsset(WorldAsset))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save MH shadow world asset: %s"), *WorldObjectPath);
		return 1;
	}

	const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("CellSplit"), OutputStem);
	IFileManager::Get().MakeDirectory(*OutputDir, true);
	FFileHelper::SaveStringToFile(TileCsv, *FPaths::Combine(OutputDir, TEXT("TileToCell.csv")));
	SplitCsv += TEXT("\nMetric,Value\n");
	SplitCsv += FString::Printf(TEXT("Cells,%d\n"), SortedCellCoords.Num());
	SplitCsv += FString::Printf(TEXT("CellGrid,%dx%d\n"), WorldAsset->CellCount.X, WorldAsset->CellCount.Y);
	SplitCsv += FString::Printf(TEXT("CellSize,%.3f\n"), CellSize);
	SplitCsv += FString::Printf(TEXT("TotalVirtualPages,%d\n"), TotalVirtualPages);
	SplitCsv += FString::Printf(TEXT("NonEmptyVirtualPages,%d\n"), NonEmptyVirtualPages);
	SplitCsv += FString::Printf(TEXT("TotalRawDualBytes,%lld\n"), WorldAsset->TotalRawDualBytes);
	SplitCsv += FString::Printf(TEXT("TotalCompressedBytes,%lld\n"), TotalCompressedBytes);
	SplitCsv += FString::Printf(TEXT("LargestCellBytes,%lld\n"), LargestCellBytes);
	SplitCsv += FString::Printf(TEXT("IncludeRawDebug,%d\n"), bIncludeRawDebug ? 1 : 0);
	FFileHelper::SaveStringToFile(SplitCsv, *FPaths::Combine(OutputDir, TEXT("SplitStats.csv")));

	UE_LOG(LogTemp, Display, TEXT("Split MH shadow asset into cells: source=%s world=%s outputRoot=%s cells=%d grid=%dx%d totalPages=%d compressedBytes=%lld largestCell=%lld csv=%s"),
		*SourcePath,
		*WorldObjectPath,
		*OutputRoot,
		SortedCellCoords.Num(),
		WorldAsset->CellCount.X,
		WorldAsset->CellCount.Y,
		TotalVirtualPages,
		TotalCompressedBytes,
		LargestCellBytes,
		*OutputDir);
	return 0;
}

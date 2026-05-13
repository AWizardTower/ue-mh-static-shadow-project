// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowValidateCompressionCommandlet.h"

#include "MHShadowDataAsset.h"
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

static bool SampleRepresentativeDepth(const UMHShadowDataAsset& Asset, int32 X, int32 Y, float& OutDepth)
{
	OutDepth = 1.0f;
	if (Asset.Nodes.Num() == 0 || Asset.Resolution.X <= 0 || Asset.Resolution.Y <= 0)
	{
		return false;
	}

	const bool bUseTiledData = Asset.TileSize > 0
		&& Asset.TileCount.X > 0
		&& Asset.TileCount.Y > 0
		&& Asset.Tiles.Num() == Asset.TileCount.X * Asset.TileCount.Y;

	int32 NodeIndex = 0;
	FIntPoint NodeMin(0, 0);
	int32 NodeSize = Asset.Resolution.X;
	int32 NodeOffset = 0;
	int32 NodeCount = Asset.Nodes.Num();
	if (bUseTiledData)
	{
		const int32 TileX = X / Asset.TileSize;
		const int32 TileY = Y / Asset.TileSize;
		const int32 TileIndex = TileY * Asset.TileCount.X + TileX;
		if (!Asset.Tiles.IsValidIndex(TileIndex))
		{
			return false;
		}

		const FMHShadowTile& Tile = Asset.Tiles[TileIndex];
		NodeIndex = Tile.RootNodeIndex;
		NodeSize = Asset.TileSize;
		NodeOffset = Tile.NodeOffset;
		NodeCount = Tile.NodeCount;
		X -= Tile.TexelRect.X;
		Y -= Tile.TexelRect.Y;
	}

	if (!Asset.Nodes.IsValidIndex(NodeIndex))
	{
		return false;
	}

	bool bHasDepth = false;

	for (int32 Step = 0; Step < 32; ++Step)
	{
		if (!Asset.Nodes.IsValidIndex(NodeIndex) || NodeIndex < NodeOffset || NodeIndex >= NodeOffset + NodeCount)
		{
			return bHasDepth;
		}

		const FMHShadowNode& Node = Asset.Nodes[NodeIndex];
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
		const int32 ChildX = (X - NodeMin.X) >= ChildSize ? 1 : 0;
		const int32 ChildY = (Y - NodeMin.Y) >= ChildSize ? 1 : 0;
		const int32 ChildSlot = ChildY * 2 + ChildX;
		const int32 ChildNodeIndex = GetChildIndex(Node.ChildIndices, ChildSlot);
		if (ChildNodeIndex < NodeOffset || ChildNodeIndex >= NodeOffset + NodeCount)
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

static bool SampleRepresentativeDepthFromTile(
	const TArray<FMHShadowNode>& Nodes,
	const FMHShadowTile& Tile,
	int32 TileSize,
	int32 LocalX,
	int32 LocalY,
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

static FString GetValidationOutputDir()
{
	const FString OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("CompressionValidation"));
	IFileManager::Get().MakeDirectory(*OutputDir, true);
	return OutputDir;
}

struct FTileValidationStats
{
	int32 RawTexels = 0;
	int32 ValidTexels = 0;
	int32 EmptyTexels = 0;
	int32 RepresentedTexels = 0;
	int32 MissingTexels = 0;
	int32 MismatchTexels = 0;
};
}

UMHShadowValidateCompressionCommandlet::UMHShadowValidateCompressionCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowValidateCompressionCommandlet::Main(const FString& Params)
{
	FString AssetPath = TEXT("/Game/MHShadow/Baked/MHShadowData_LightmassDual_BakeTest");
	ParseStringParam(Params, TEXT("Asset="), AssetPath);

	UMHShadowDataAsset* Asset = LoadObject<UMHShadowDataAsset>(nullptr, *ToObjectPath(AssetPath));
	if (!Asset)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load MH shadow data asset '%s'."), *AssetPath);
		return 1;
	}

	const int32 ExpectedTexelCount = Asset->Resolution.X * Asset->Resolution.Y;
	const bool bHasTiledData = Asset->TileSize > 0
		&& Asset->TileCount.X > 0
		&& Asset->TileCount.Y > 0
		&& Asset->Tiles.Num() == Asset->TileCount.X * Asset->TileCount.Y;
	if (Asset->RawIntervals.Num() != ExpectedTexelCount || Asset->Resolution.X <= 0 || Asset->Resolution.Y <= 0 || (!bHasTiledData && Asset->Resolution.X != Asset->Resolution.Y))
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid MH shadow asset for compression validation: %s resolution=%dx%d rawIntervals=%d expected=%d"),
			*AssetPath,
			Asset->Resolution.X,
			Asset->Resolution.Y,
			Asset->RawIntervals.Num(),
			ExpectedTexelCount);
		return 1;
	}

	constexpr float Tolerance = 1.0e-5f;
	int32 ValidRawTexels = 0;
	int32 EmptyRawTexels = 0;
	int32 RepresentedTexels = 0;
	int32 MissingTexels = 0;
	int32 MismatchTexels = 0;
	double ShrinkSum = 0.0;
	float MaxShrink = 0.0f;
	TArray<FString> ErrorRows;
	ErrorRows.Reserve(64);
	TArray<FTileValidationStats> TileStats;
	if (bHasTiledData)
	{
		TileStats.SetNum(Asset->Tiles.Num());
	}

	for (int32 Y = 0; Y < Asset->Resolution.Y; ++Y)
	{
		for (int32 X = 0; X < Asset->Resolution.X; ++X)
		{
			const int32 TexelIndex = Y * Asset->Resolution.X + X;
			const FMHShadowDepthInterval& RawInterval = Asset->RawIntervals[TexelIndex];
			const float ExpectedMin = RawInterval.bValid ? RawInterval.MinDepth : 1.0f;
			const float ExpectedMax = RawInterval.bValid ? RawInterval.MaxDepth : 1.0f;

			ValidRawTexels += RawInterval.bValid ? 1 : 0;
			EmptyRawTexels += RawInterval.bValid ? 0 : 1;
			FTileValidationStats* CurrentTileStats = nullptr;
			if (bHasTiledData)
			{
				const int32 TileX = X / Asset->TileSize;
				const int32 TileY = Y / Asset->TileSize;
				const int32 TileIndex = TileY * Asset->TileCount.X + TileX;
				CurrentTileStats = TileStats.IsValidIndex(TileIndex) ? &TileStats[TileIndex] : nullptr;
				if (CurrentTileStats)
				{
					++CurrentTileStats->RawTexels;
					CurrentTileStats->ValidTexels += RawInterval.bValid ? 1 : 0;
					CurrentTileStats->EmptyTexels += RawInterval.bValid ? 0 : 1;
				}
			}

			float RepresentativeDepth = 1.0f;
			if (!SampleRepresentativeDepth(*Asset, X, Y, RepresentativeDepth))
			{
				++MissingTexels;
				if (CurrentTileStats)
				{
					++CurrentTileStats->MissingTexels;
				}
				if (ErrorRows.Num() < 64)
				{
					ErrorRows.Add(FString::Printf(TEXT("Missing,%d,%d,%d,%.9f,%.9f,NaN"), X, Y, TexelIndex, ExpectedMin, ExpectedMax));
				}
				continue;
			}

			const bool bInsideInterval = RepresentativeDepth >= ExpectedMin - Tolerance && RepresentativeDepth <= ExpectedMax + Tolerance;
			if (!bInsideInterval)
			{
				++MismatchTexels;
				if (CurrentTileStats)
				{
					++CurrentTileStats->MismatchTexels;
				}
				if (ErrorRows.Num() < 64)
				{
					ErrorRows.Add(FString::Printf(TEXT("Mismatch,%d,%d,%d,%.9f,%.9f,%.9f"), X, Y, TexelIndex, ExpectedMin, ExpectedMax, RepresentativeDepth));
				}
				continue;
			}

			++RepresentedTexels;
			if (CurrentTileStats)
			{
				++CurrentTileStats->RepresentedTexels;
			}
			if (RawInterval.bValid)
			{
				const float IntervalWidth = FMath::Max(0.0f, ExpectedMax - ExpectedMin);
				const float RepresentativeShrink = FMath::Max(RepresentativeDepth - ExpectedMin, ExpectedMax - RepresentativeDepth);
				const float Shrink = FMath::Max(0.0f, IntervalWidth - RepresentativeShrink);
				ShrinkSum += Shrink;
				MaxShrink = FMath::Max(MaxShrink, Shrink);
			}
		}
	}

	int32 ClipmapLevelCount = 0;
	int32 ClipmapRawTexels = 0;
	int32 ClipmapRepresentedTexels = 0;
	int32 ClipmapMissingTexels = 0;
	int32 ClipmapMismatchTexels = 0;
	FString ClipmapCsv;
	ClipmapCsv += TEXT("LevelIndex,ResolutionX,ResolutionY,TileCountX,TileCountY,RawTexels,RepresentedTexels,MissingTexels,MismatchTexels\n");
	for (const FMHShadowClipmapLevel& Level : Asset->ClipmapLevels)
	{
		const bool bLevelValid =
			Level.Resolution.X > 0
			&& Level.Resolution.Y > 0
			&& Level.TileSize > 0
			&& Level.TileCount.X > 0
			&& Level.TileCount.Y > 0
			&& Level.RawIntervalOffset >= 0
			&& Level.RawIntervalOffset + Level.RawIntervalCount <= Asset->ClipmapRawIntervals.Num()
			&& Level.TileOffset >= 0
			&& Level.TileOffset + Level.TileDataCount <= Asset->ClipmapTiles.Num();
		if (!bLevelValid)
		{
			++ClipmapMissingTexels;
			ClipmapCsv += FString::Printf(TEXT("%d,%d,%d,%d,%d,0,0,1,0\n"),
				Level.LevelIndex,
				Level.Resolution.X,
				Level.Resolution.Y,
				Level.TileCount.X,
				Level.TileCount.Y);
			continue;
		}

		++ClipmapLevelCount;
		int32 LevelRawTexels = 0;
		int32 LevelRepresentedTexels = 0;
		int32 LevelMissingTexels = 0;
		int32 LevelMismatchTexels = 0;
		for (int32 Y = 0; Y < Level.Resolution.Y; ++Y)
		{
			for (int32 X = 0; X < Level.Resolution.X; ++X)
			{
				const int32 LocalTexelIndex = Y * Level.Resolution.X + X;
				const int32 RawIndex = Level.RawIntervalOffset + LocalTexelIndex;
				const FMHShadowDepthInterval& RawInterval = Asset->ClipmapRawIntervals[RawIndex];
				const float ExpectedMin = RawInterval.bValid ? RawInterval.MinDepth : 1.0f;
				const float ExpectedMax = RawInterval.bValid ? RawInterval.MaxDepth : 1.0f;
				const int32 TileX = X / Level.TileSize;
				const int32 TileY = Y / Level.TileSize;
				const int32 LocalTileIndex = TileY * Level.TileCount.X + TileX;
				const int32 TileIndex = Level.TileOffset + LocalTileIndex;

				++LevelRawTexels;
				float RepresentativeDepth = 1.0f;
				if (!Asset->ClipmapTiles.IsValidIndex(TileIndex)
					|| !SampleRepresentativeDepthFromTile(
						Asset->ClipmapNodes,
						Asset->ClipmapTiles[TileIndex],
						Level.TileSize,
						X - Asset->ClipmapTiles[TileIndex].TexelRect.X,
						Y - Asset->ClipmapTiles[TileIndex].TexelRect.Y,
						RepresentativeDepth))
				{
					++LevelMissingTexels;
					continue;
				}

				const bool bInsideInterval = RepresentativeDepth >= ExpectedMin - Tolerance && RepresentativeDepth <= ExpectedMax + Tolerance;
				if (!bInsideInterval)
				{
					++LevelMismatchTexels;
					continue;
				}

				++LevelRepresentedTexels;
			}
		}

		ClipmapRawTexels += LevelRawTexels;
		ClipmapRepresentedTexels += LevelRepresentedTexels;
		ClipmapMissingTexels += LevelMissingTexels;
		ClipmapMismatchTexels += LevelMismatchTexels;
		ClipmapCsv += FString::Printf(TEXT("%d,%d,%d,%d,%d,%d,%d,%d,%d\n"),
			Level.LevelIndex,
			Level.Resolution.X,
			Level.Resolution.Y,
			Level.TileCount.X,
			Level.TileCount.Y,
			LevelRawTexels,
			LevelRepresentedTexels,
			LevelMissingTexels,
			LevelMismatchTexels);
	}

	FString Csv;
	Csv += TEXT("Metric,Value\n");
	Csv += FString::Printf(TEXT("Asset,%s\n"), *AssetPath);
	Csv += FString::Printf(TEXT("Resolution,%dx%d\n"), Asset->Resolution.X, Asset->Resolution.Y);
	Csv += FString::Printf(TEXT("RawTexels,%d\n"), ExpectedTexelCount);
	Csv += FString::Printf(TEXT("ValidRawTexels,%d\n"), ValidRawTexels);
	Csv += FString::Printf(TEXT("EmptyRawTexels,%d\n"), EmptyRawTexels);
	Csv += FString::Printf(TEXT("RepresentedTexels,%d\n"), RepresentedTexels);
	Csv += FString::Printf(TEXT("MissingTexels,%d\n"), MissingTexels);
	Csv += FString::Printf(TEXT("MismatchTexels,%d\n"), MismatchTexels);
	Csv += FString::Printf(TEXT("Nodes,%d\n"), Asset->Nodes.Num());
	Csv += FString::Printf(TEXT("Intervals,%d\n"), Asset->Intervals.Num());
	Csv += FString::Printf(TEXT("RawBytes,%lld\n"), Asset->Stats.RawBytes);
	Csv += FString::Printf(TEXT("CompressedBytes,%lld\n"), Asset->Stats.CompressedBytes);
	Csv += FString::Printf(TEXT("CompressionRatio,%.9f\n"), Asset->Stats.CompressionRatio);
	Csv += FString::Printf(TEXT("TileSize,%d\n"), Asset->TileSize);
	Csv += FString::Printf(TEXT("TileCount,%dx%d\n"), Asset->TileCount.X, Asset->TileCount.Y);
	Csv += FString::Printf(TEXT("ClipmapLevels,%d\n"), ClipmapLevelCount);
	Csv += FString::Printf(TEXT("ClipmapRawTexels,%d\n"), ClipmapRawTexels);
	Csv += FString::Printf(TEXT("ClipmapRepresentedTexels,%d\n"), ClipmapRepresentedTexels);
	Csv += FString::Printf(TEXT("ClipmapMissingTexels,%d\n"), ClipmapMissingTexels);
	Csv += FString::Printf(TEXT("ClipmapMismatchTexels,%d\n"), ClipmapMismatchTexels);
	Csv += FString::Printf(TEXT("AverageShrink,%.9f\n"), ValidRawTexels > 0 ? ShrinkSum / static_cast<double>(ValidRawTexels) : 0.0);
	Csv += FString::Printf(TEXT("MaxShrink,%.9f\n"), MaxShrink);
	Csv += TEXT("\nErrorType,X,Y,TexelIndex,ExpectedMin,ExpectedMax,RepresentativeDepth\n");
	for (const FString& ErrorRow : ErrorRows)
	{
		Csv += ErrorRow;
		Csv += TEXT("\n");
	}

	const FString SafeName = FPackageName::GetShortName(AssetPath);
	const FString CsvPath = FPaths::Combine(GetValidationOutputDir(), SafeName + TEXT("_CompressionValidation.csv"));
	FFileHelper::SaveStringToFile(Csv, *CsvPath);

	int32 WorstTileIndex = INDEX_NONE;
	int32 WorstTileMismatch = 0;
	FString TileCsv;
	TileCsv += TEXT("TileIndex,TileX,TileY,PageIndex,NodeOffset,NodeCount,RawTexels,ValidTexels,EmptyTexels,RepresentedTexels,MissingTexels,MismatchTexels,CompressionRatio\n");
	if (bHasTiledData)
	{
		for (int32 TileIndex = 0; TileIndex < Asset->Tiles.Num(); ++TileIndex)
		{
			const FMHShadowTile& Tile = Asset->Tiles[TileIndex];
			const FTileValidationStats& Stats = TileStats[TileIndex];
			if (Stats.MismatchTexels > WorstTileMismatch)
			{
				WorstTileMismatch = Stats.MismatchTexels;
				WorstTileIndex = TileIndex;
			}

			TileCsv += FString::Printf(
				TEXT("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.9f\n"),
				TileIndex,
				Tile.TileCoord.X,
				Tile.TileCoord.Y,
				Tile.PageIndex,
				Tile.NodeOffset,
				Tile.NodeCount,
				Stats.RawTexels,
				Stats.ValidTexels,
				Stats.EmptyTexels,
				Stats.RepresentedTexels,
				Stats.MissingTexels,
				Stats.MismatchTexels,
				Tile.CompressionRatio);
		}
	}
	const FString TileCsvPath = FPaths::Combine(GetValidationOutputDir(), SafeName + TEXT("_TileCompressionValidation.csv"));
	FFileHelper::SaveStringToFile(TileCsv, *TileCsvPath);
	const FString ClipmapCsvPath = FPaths::Combine(GetValidationOutputDir(), SafeName + TEXT("_ClipmapCompressionValidation.csv"));
	FFileHelper::SaveStringToFile(ClipmapCsv, *ClipmapCsvPath);

	UE_LOG(LogTemp, Display, TEXT("MH compression validation: %s"), *AssetPath);
	UE_LOG(LogTemp, Display, TEXT("RawTexels=%d Valid=%d Empty=%d Represented=%d Missing=%d Mismatch=%d Tiles=%dx%d Nodes=%d Ratio=%.6f"),
		ExpectedTexelCount,
		ValidRawTexels,
		EmptyRawTexels,
		RepresentedTexels,
		MissingTexels,
		MismatchTexels,
		Asset->TileCount.X,
		Asset->TileCount.Y,
		Asset->Nodes.Num(),
		Asset->Stats.CompressionRatio);
	UE_LOG(LogTemp, Display, TEXT("Compression validation CSV: %s"), *CsvPath);
	UE_LOG(LogTemp, Display, TEXT("Tile compression validation CSV: %s WorstTile=%d WorstTileMismatch=%d"),
		*TileCsvPath,
		WorstTileIndex,
		WorstTileMismatch);
	UE_LOG(LogTemp, Display, TEXT("Clipmap compression validation: Levels=%d RawTexels=%d Represented=%d Missing=%d Mismatch=%d CSV=%s"),
		ClipmapLevelCount,
		ClipmapRawTexels,
		ClipmapRepresentedTexels,
		ClipmapMissingTexels,
		ClipmapMismatchTexels,
		*ClipmapCsvPath);

	if (MissingTexels != 0 || MismatchTexels != 0 || ClipmapMissingTexels != 0 || ClipmapMismatchTexels != 0 || Asset->Stats.CompressionRatio >= 1.0f)
	{
		UE_LOG(LogTemp, Error, TEXT("MH compression validation failed."));
		return 1;
	}

	UE_LOG(LogTemp, Display, TEXT("MH compression validation passed."));
	return 0;
}

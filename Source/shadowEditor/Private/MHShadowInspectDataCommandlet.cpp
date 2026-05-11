// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowInspectDataCommandlet.h"

#include "MHShadowDataAsset.h"
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
}

UMHShadowInspectDataCommandlet::UMHShadowInspectDataCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowInspectDataCommandlet::Main(const FString& Params)
{
	FString AssetPath = TEXT("/Game/MHShadow/Baked/MHShadowData_Test");
	ParseStringParam(Params, TEXT("Asset="), AssetPath);
	FString Expectation;
	ParseStringParam(Params, TEXT("Expect="), Expectation);

	UMHShadowDataAsset* Asset = LoadObject<UMHShadowDataAsset>(nullptr, *ToObjectPath(AssetPath));
	if (!Asset)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load MH shadow data asset '%s'."), *AssetPath);
		return 1;
	}

	const int32 ExpectedPreviewPixels = Asset->Resolution.X * Asset->Resolution.Y;
	TSet<FColor> UniqueColors;
	int32 NonWhitePixels = 0;
	int32 ValidRawIntervals = 0;
	int32 ThinFallbackIntervals = 0;
	int32 UnpairedIntervals = 0;
	int32 MultiHitIntervals = 0;
	int32 PairedIntervals = 0;
	int32 EmptyTiles = 0;
	int32 WorstTileIndex = INDEX_NONE;
	float MinTileRatio = TNumericLimits<float>::Max();
	float MaxTileRatio = 0.0f;
	double TileRatioSum = 0.0;
	float MaxThickness = 0.0f;
	double ThicknessSum = 0.0;
	for (const FColor& Color : Asset->DebugIntervalPreview)
	{
		UniqueColors.Add(Color);
		if (Color != FColor::White)
		{
			++NonWhitePixels;
		}
	}
	for (int32 Index = 0; Index < Asset->RawIntervals.Num(); ++Index)
	{
		const FMHShadowDepthInterval& Interval = Asset->RawIntervals[Index];
		if (Interval.bValid)
		{
			++ValidRawIntervals;
			const uint8 Flags = Asset->RawIntervalFlags.IsValidIndex(Index) ? Asset->RawIntervalFlags[Index] : 0;
			const float Thickness = FMath::Max(0.0f, Interval.MaxDepth - Interval.MinDepth);
			MaxThickness = FMath::Max(MaxThickness, Thickness);
			ThicknessSum += Thickness;
			if ((Flags & ((1 << 1) | (1 << 2))) == 0)
			{
				++PairedIntervals;
			}
		}
	}
	for (uint8 Flags : Asset->RawIntervalFlags)
	{
		ThinFallbackIntervals += (Flags & (1 << 1)) != 0 ? 1 : 0;
		UnpairedIntervals += (Flags & (1 << 2)) != 0 ? 1 : 0;
		MultiHitIntervals += (Flags & (1 << 3)) != 0 ? 1 : 0;
	}
	for (int32 TileIndex = 0; TileIndex < Asset->Tiles.Num(); ++TileIndex)
	{
		const FMHShadowTile& Tile = Asset->Tiles[TileIndex];
		EmptyTiles += Tile.ValidTexelCount == 0 ? 1 : 0;
		MinTileRatio = FMath::Min(MinTileRatio, Tile.CompressionRatio);
		if (Tile.CompressionRatio > MaxTileRatio)
		{
			MaxTileRatio = Tile.CompressionRatio;
			WorstTileIndex = TileIndex;
		}
		TileRatioSum += Tile.CompressionRatio;
	}
	const float AvgTileRatio = Asset->Tiles.Num() > 0 ? static_cast<float>(TileRatioSum / static_cast<double>(Asset->Tiles.Num())) : 0.0f;

	bool bOk = true;
	const bool bHasTiledData = Asset->TileCount.X > 0
		&& Asset->TileCount.Y > 0
		&& Asset->Tiles.Num() == Asset->TileCount.X * Asset->TileCount.Y
		&& Asset->PageTable.Num() == Asset->Tiles.Num();
	bOk &= Asset->IsValidForRendering();
	bOk &= Asset->Resolution.X > 0 && Asset->Resolution.Y > 0;
	bOk &= Asset->Stats.RawTexelCount == ExpectedPreviewPixels;
	bOk &= Asset->Stats.ValidTexelCount > 0;
	bOk &= Asset->RawIntervals.Num() == ExpectedPreviewPixels;
	bOk &= ValidRawIntervals == Asset->Stats.ValidTexelCount;
	bOk &= Asset->Stats.NodeCount > 0 || Asset->RawIntervals.Num() == ExpectedPreviewPixels;
	bOk &= Asset->Stats.IntervalCount > 0 || Asset->RawIntervals.Num() == ExpectedPreviewPixels;
	bOk &= Asset->Stats.RawBytes > 0;
	bOk &= Asset->Stats.CompressedBytes > 0 || Asset->Nodes.Num() == 0;
	bOk &= Asset->DebugIntervalPreview.Num() == ExpectedPreviewPixels;
	bOk &= NonWhitePixels > 0;
	bOk &= UniqueColors.Num() > 1;
	bOk &= !Asset->LightZAxis.IsNearlyZero();
	bOk &= Asset->LightSpaceMax.X > Asset->LightSpaceMin.X;
	bOk &= Asset->LightSpaceMax.Y > Asset->LightSpaceMin.Y;
	bOk &= Asset->MaxLightDepth > Asset->MinLightDepth;
	if (Asset->BakeSource == EMHShadowBakeSource::LightmassDual)
	{
		bOk &= bHasTiledData;
		bOk &= Asset->TileSize > 0;
		bOk &= (Asset->Resolution.X % Asset->TileSize) == 0;
		bOk &= (Asset->Resolution.Y % Asset->TileSize) == 0;
	}
	if (Expectation.Equals(TEXT("ClosedCube"), ESearchCase::IgnoreCase))
	{
		bOk &= ValidRawIntervals > 0;
		bOk &= ValidRawIntervals < ExpectedPreviewPixels;
		bOk &= PairedIntervals > 0;
		bOk &= ThinFallbackIntervals < ValidRawIntervals;
		bOk &= MaxThickness > 0.005f;
	}

	UE_LOG(LogTemp, Display, TEXT("MH shadow data inspect: %s"), *AssetPath);
	UE_LOG(LogTemp, Display, TEXT("Resolution=%dx%d ValidTexels=%d Nodes=%d Intervals=%d RawBytes=%lld CompressedBytes=%lld Ratio=%.6f BakeSeconds=%.3f"),
		Asset->Resolution.X,
		Asset->Resolution.Y,
		Asset->Stats.ValidTexelCount,
		Asset->Stats.NodeCount,
		Asset->Stats.IntervalCount,
		Asset->Stats.RawBytes,
		Asset->Stats.CompressedBytes,
		Asset->Stats.CompressionRatio,
		Asset->Stats.BakeSeconds);
	UE_LOG(LogTemp, Display, TEXT("DebugPreviewPixels=%d NonWhitePixels=%d UniqueColors=%d LightDepth=[%.3f, %.3f]"),
		Asset->DebugIntervalPreview.Num(),
		NonWhitePixels,
		UniqueColors.Num(),
		Asset->MinLightDepth,
		Asset->MaxLightDepth);
	UE_LOG(LogTemp, Display, TEXT("RawIntervals=%d ValidRawIntervals=%d Paired=%d AvgThickness=%.6f MaxThickness=%.6f"),
		Asset->RawIntervals.Num(),
		ValidRawIntervals,
		PairedIntervals,
		ValidRawIntervals > 0 ? static_cast<float>(ThicknessSum / static_cast<double>(ValidRawIntervals)) : 0.0f,
		MaxThickness);
	UE_LOG(LogTemp, Display, TEXT("Source=%d ProjectionMapping=%d RawFlags=%d ThinFallback=%d Unpaired=%d MultiHit=%d"),
		static_cast<int32>(Asset->BakeSource),
		static_cast<int32>(Asset->ProjectionMapping),
		Asset->RawIntervalFlags.Num(),
		ThinFallbackIntervals,
		UnpairedIntervals,
		MultiHitIntervals);
	UE_LOG(LogTemp, Display, TEXT("Tiles=%dx%d TileSize=%d TileAssets=%d PageTable=%d EmptyTiles=%d TileRatio[min=%.6f avg=%.6f max=%.6f worst=%d]"),
		Asset->TileCount.X,
		Asset->TileCount.Y,
		Asset->TileSize,
		Asset->Tiles.Num(),
		Asset->PageTable.Num(),
		EmptyTiles,
		Asset->Tiles.Num() > 0 ? MinTileRatio : 0.0f,
		AvgTileRatio,
		MaxTileRatio,
		WorstTileIndex);
	if (Expectation.Equals(TEXT("ClosedCube"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogTemp, Display, TEXT("ClosedCube expectation: Empty=%d Paired=%d ThinFallback=%d Valid=%d MaxThickness=%.6f"),
			ExpectedPreviewPixels - ValidRawIntervals,
			PairedIntervals,
			ThinFallbackIntervals,
			ValidRawIntervals,
			MaxThickness);
	}
	UE_LOG(LogTemp, Display, TEXT("LightSpaceMin=(%.3f, %.3f) LightSpaceMax=(%.3f, %.3f)"),
		Asset->LightSpaceMin.X,
		Asset->LightSpaceMin.Y,
		Asset->LightSpaceMax.X,
		Asset->LightSpaceMax.Y);

	if (!bOk)
	{
		UE_LOG(LogTemp, Error, TEXT("MH shadow data inspect failed."));
		return 1;
	}

	UE_LOG(LogTemp, Display, TEXT("MH shadow data inspect passed."));
	return 0;
}

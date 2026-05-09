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
	for (const FColor& Color : Asset->DebugIntervalPreview)
	{
		UniqueColors.Add(Color);
		if (Color != FColor::White)
		{
			++NonWhitePixels;
		}
	}
	for (const FMHShadowDepthInterval& Interval : Asset->RawIntervals)
	{
		if (Interval.bValid)
		{
			++ValidRawIntervals;
		}
	}

	bool bOk = true;
	bOk &= Asset->IsValidForRendering();
	bOk &= Asset->Resolution.X > 0 && Asset->Resolution.X == Asset->Resolution.Y;
	bOk &= Asset->Stats.RawTexelCount == ExpectedPreviewPixels;
	bOk &= Asset->Stats.ValidTexelCount > 0;
	bOk &= Asset->RawIntervals.Num() == ExpectedPreviewPixels;
	bOk &= ValidRawIntervals == Asset->Stats.ValidTexelCount;
	bOk &= Asset->Stats.NodeCount > 0;
	bOk &= Asset->Stats.IntervalCount > 0;
	bOk &= Asset->Stats.RawBytes > 0;
	bOk &= Asset->Stats.CompressedBytes > 0;
	bOk &= Asset->DebugIntervalPreview.Num() == ExpectedPreviewPixels;
	bOk &= NonWhitePixels > 0;
	bOk &= UniqueColors.Num() > 1;
	bOk &= !Asset->LightZAxis.IsNearlyZero();
	bOk &= Asset->LightSpaceMax.X > Asset->LightSpaceMin.X;
	bOk &= Asset->LightSpaceMax.Y > Asset->LightSpaceMin.Y;
	bOk &= Asset->MaxLightDepth > Asset->MinLightDepth;

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
	UE_LOG(LogTemp, Display, TEXT("RawIntervals=%d ValidRawIntervals=%d"),
		Asset->RawIntervals.Num(),
		ValidRawIntervals);
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

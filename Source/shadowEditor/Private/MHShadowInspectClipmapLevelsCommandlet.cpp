// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowInspectClipmapLevelsCommandlet.h"

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

static int64 LevelRawBytes(const FMHShadowClipmapLevel& Level)
{
	return static_cast<int64>(Level.RawIntervalCount) * static_cast<int64>(sizeof(float) * 2);
}

static int64 LevelCompressedBytes(const FMHShadowClipmapLevel& Level)
{
	return static_cast<int64>(Level.NodeCount) * static_cast<int64>(sizeof(FMHShadowNode))
		+ static_cast<int64>(Level.TileDataCount) * static_cast<int64>(sizeof(FMHShadowTile))
		+ static_cast<int64>(Level.PageTableCount) * static_cast<int64>(sizeof(int32));
}
}

UMHShadowInspectClipmapLevelsCommandlet::UMHShadowInspectClipmapLevelsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowInspectClipmapLevelsCommandlet::Main(const FString& Params)
{
	FString AssetPath;
	if (!ParseStringParam(Params, TEXT("Asset="), AssetPath))
	{
		UE_LOG(LogTemp, Error, TEXT("Missing Asset=/Game/... argument."));
		return 1;
	}

	UMHShadowDataAsset* Asset = LoadObject<UMHShadowDataAsset>(nullptr, *ToObjectPath(AssetPath));
	if (!Asset)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load MH shadow data asset: %s"), *AssetPath);
		return 1;
	}

	const FString StatsDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("ClipmapInspect"));
	IFileManager::Get().MakeDirectory(*StatsDir, true);
	const FString SafeName = FPackageName::GetShortName(FPackageName::ObjectPathToPackageName(ToObjectPath(AssetPath)));

	FString Csv = TEXT("Level,ResolutionX,ResolutionY,TileSize,TileCountX,TileCountY,TexelWorldSizeX,TexelWorldSizeY,RawIntervals,Tiles,Nodes,RawBytes,CompressedBytes,CompressionRatio,WorldToShadowRow0,WorldToShadowRow1,WorldToShadowRow2,WorldToShadowRow3\n");
	UE_LOG(LogTemp, Display, TEXT("MH clipmap inspect: %s baseResolution=%dx%d baseTileSize=%d clipmapLevels=%d"),
		*Asset->GetPathName(),
		Asset->Resolution.X,
		Asset->Resolution.Y,
		Asset->TileSize,
		Asset->ClipmapLevels.Num());

	for (const FMHShadowClipmapLevel& Level : Asset->ClipmapLevels)
	{
		const int64 RawBytes = LevelRawBytes(Level);
		const int64 CompressedBytes = LevelCompressedBytes(Level);
		const double Ratio = RawBytes > 0 ? static_cast<double>(CompressedBytes) / static_cast<double>(RawBytes) : 1.0;
		UE_LOG(LogTemp, Display, TEXT("Level=%d res=%dx%d tileSize=%d tiles=%dx%d texelWorld=(%.4f,%.4f) raw=%d nodes=%d ratio=%.6f"),
			Level.LevelIndex,
			Level.Resolution.X,
			Level.Resolution.Y,
			Level.TileSize,
			Level.TileCount.X,
			Level.TileCount.Y,
			Level.TexelWorldSize.X,
			Level.TexelWorldSize.Y,
			Level.RawIntervalCount,
			Level.NodeCount,
			Ratio);

		Csv += FString::Printf(
			TEXT("%d,%d,%d,%d,%d,%d,%.6f,%.6f,%d,%d,%d,%lld,%lld,%.9f,\"%s\",\"%s\",\"%s\",\"%s\"\n"),
			Level.LevelIndex,
			Level.Resolution.X,
			Level.Resolution.Y,
			Level.TileSize,
			Level.TileCount.X,
			Level.TileCount.Y,
			Level.TexelWorldSize.X,
			Level.TexelWorldSize.Y,
			Level.RawIntervalCount,
			Level.TileDataCount,
			Level.NodeCount,
			RawBytes,
			CompressedBytes,
			Ratio,
			*Level.WorldToShadowRow0.ToString(),
			*Level.WorldToShadowRow1.ToString(),
			*Level.WorldToShadowRow2.ToString(),
			*Level.WorldToShadowRow3.ToString());
	}

	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(StatsDir, SafeName + TEXT("_ClipmapLevelSummary.csv")));
	UE_LOG(LogTemp, Display, TEXT("MH clipmap inspect wrote %s"), *FPaths::Combine(StatsDir, SafeName + TEXT("_ClipmapLevelSummary.csv")));
	return 0;
}

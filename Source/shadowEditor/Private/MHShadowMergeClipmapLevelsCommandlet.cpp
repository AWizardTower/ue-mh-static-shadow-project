// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowMergeClipmapLevelsCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "MHShadowDataAsset.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

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

static TArray<FString> SplitInputs(const FString& Inputs)
{
	TArray<FString> Result;
	Inputs.ParseIntoArray(Result, TEXT(","), true);
	for (FString& Input : Result)
	{
		Input.TrimStartAndEndInline();
	}
	Result.RemoveAll([](const FString& Value) { return Value.IsEmpty(); });
	return Result;
}

static TArray<FString> ParseIndexedInputs(const FString& Params)
{
	TArray<FString> Result;
	for (int32 InputIndex = 0; InputIndex < 32; ++InputIndex)
	{
		FString InputPath;
		if (ParseStringParam(Params, *FString::Printf(TEXT("Input%d="), InputIndex), InputPath))
		{
			Result.Add(InputPath);
		}
	}
	return Result;
}

static bool ParseBoolParam(const FString& Params, const TCHAR* Key, bool bDefault)
{
	int32 Value = bDefault ? 1 : 0;
	if (FParse::Value(*Params, Key, Value))
	{
		return Value != 0;
	}
	return bDefault;
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

static bool SaveShadowDataAsset(const FString& ObjectPath, UMHShadowDataAsset* Asset)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
	if (!Package)
	{
		return false;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
}

static int64 EstimateCompressedBytes(const UMHShadowDataAsset& Asset)
{
	return static_cast<int64>(Asset.Nodes.Num() + Asset.ClipmapNodes.Num()) * static_cast<int64>(sizeof(FMHShadowNode))
		+ static_cast<int64>(Asset.Tiles.Num() + Asset.ClipmapTiles.Num()) * static_cast<int64>(sizeof(FMHShadowTile))
		+ static_cast<int64>(Asset.PageTable.Num() + Asset.ClipmapPageTable.Num()) * static_cast<int64>(sizeof(int32))
		+ static_cast<int64>(Asset.ClipmapLevels.Num()) * static_cast<int64>(sizeof(FMHShadowClipmapLevel));
}

static const TArray<FMHShadowDepthInterval>* GetSourceRawIntervals(const UMHShadowDataAsset& SourceAsset, const FMHShadowClipmapLevel& SourceLevel)
{
	return SourceLevel.RawIntervalCount > 0 && SourceAsset.ClipmapRawIntervals.IsValidIndex(SourceLevel.RawIntervalOffset)
		? &SourceAsset.ClipmapRawIntervals
		: &SourceAsset.RawIntervals;
}

static bool AppendLevelFromAsset(
	UMHShadowDataAsset& OutputAsset,
	const UMHShadowDataAsset& SourceAsset,
	int32 OutputLevelIndex,
	bool bIncludeRawDebug,
	int64& InOutRawTexelCount,
	int64& InOutValidTexelCount,
	FString& OutError)
{
	FMHShadowClipmapLevel SourceLevel;
	if (SourceAsset.ClipmapLevels.Num() > 0)
	{
		SourceLevel = SourceAsset.ClipmapLevels[0];
	}
	else
	{
		SourceLevel.LevelIndex = 0;
		SourceLevel.Resolution = SourceAsset.Resolution;
		SourceLevel.TileSize = SourceAsset.TileSize;
		SourceLevel.TileCount = SourceAsset.TileCount;
		SourceLevel.RawIntervalOffset = 0;
		SourceLevel.RawIntervalCount = SourceAsset.RawIntervals.Num();
		SourceLevel.TileOffset = 0;
		SourceLevel.TileDataCount = SourceAsset.Tiles.Num();
		SourceLevel.PageTableOffset = 0;
		SourceLevel.PageTableCount = SourceAsset.PageTable.Num();
		SourceLevel.NodeOffset = 0;
		SourceLevel.NodeCount = SourceAsset.Nodes.Num();
		const FVector2D LightExtent = SourceAsset.LightSpaceMax - SourceAsset.LightSpaceMin;
		SourceLevel.TexelWorldSize = FVector2D(
			SourceAsset.Resolution.X > 0 ? FMath::Abs(LightExtent.X) / static_cast<double>(SourceAsset.Resolution.X) : 0.0,
			SourceAsset.Resolution.Y > 0 ? FMath::Abs(LightExtent.Y) / static_cast<double>(SourceAsset.Resolution.Y) : 0.0);
		SourceLevel.WorldToShadowRow0 = SourceAsset.WorldToShadowRow0;
		SourceLevel.WorldToShadowRow1 = SourceAsset.WorldToShadowRow1;
		SourceLevel.WorldToShadowRow2 = SourceAsset.WorldToShadowRow2;
		SourceLevel.WorldToShadowRow3 = SourceAsset.WorldToShadowRow3;
	}

	const TArray<FMHShadowDepthInterval>* SourceRawIntervals = GetSourceRawIntervals(SourceAsset, SourceLevel);
	const TArray<FMHShadowNode>& SourceNodes = SourceAsset.ClipmapLevels.Num() > 0 ? SourceAsset.ClipmapNodes : SourceAsset.Nodes;
	const TArray<FMHShadowTile>& SourceTiles = SourceAsset.ClipmapLevels.Num() > 0 ? SourceAsset.ClipmapTiles : SourceAsset.Tiles;

	if (!SourceRawIntervals)
	{
		OutError = TEXT("source raw interval array is null");
		return false;
	}
	if (SourceLevel.RawIntervalCount <= 0 || SourceLevel.TileDataCount <= 0 || SourceLevel.NodeCount <= 0)
	{
		OutError = FString::Printf(TEXT("source level has invalid counts raw=%d tiles=%d nodes=%d"), SourceLevel.RawIntervalCount, SourceLevel.TileDataCount, SourceLevel.NodeCount);
		return false;
	}
	if (!SourceRawIntervals->IsValidIndex(SourceLevel.RawIntervalOffset) || !SourceRawIntervals->IsValidIndex(SourceLevel.RawIntervalOffset + SourceLevel.RawIntervalCount - 1))
	{
		OutError = TEXT("source level raw interval range is out of bounds");
		return false;
	}
	if (!SourceTiles.IsValidIndex(SourceLevel.TileOffset) || !SourceTiles.IsValidIndex(SourceLevel.TileOffset + SourceLevel.TileDataCount - 1))
	{
		OutError = TEXT("source level tile range is out of bounds");
		return false;
	}
	if (!SourceNodes.IsValidIndex(SourceLevel.NodeOffset) || !SourceNodes.IsValidIndex(SourceLevel.NodeOffset + SourceLevel.NodeCount - 1))
	{
		OutError = TEXT("source level node range is out of bounds");
		return false;
	}

	InOutRawTexelCount += SourceLevel.RawIntervalCount;
	if (SourceAsset.Stats.ValidTexelCount > 0)
	{
		InOutValidTexelCount += SourceAsset.Stats.ValidTexelCount;
	}
	else
	{
		for (int32 IntervalIndex = 0; IntervalIndex < SourceLevel.RawIntervalCount; ++IntervalIndex)
		{
			InOutValidTexelCount += (*SourceRawIntervals)[SourceLevel.RawIntervalOffset + IntervalIndex].bValid ? 1 : 0;
		}
	}

	FMHShadowClipmapLevel OutputLevel = SourceLevel;
	OutputLevel.LevelIndex = OutputLevelIndex;
	OutputLevel.RawIntervalOffset = bIncludeRawDebug ? OutputAsset.ClipmapRawIntervals.Num() : 0;
	OutputLevel.RawIntervalCount = bIncludeRawDebug ? SourceLevel.RawIntervalCount : 0;
	OutputLevel.TileOffset = OutputAsset.ClipmapTiles.Num();
	OutputLevel.PageTableOffset = OutputAsset.ClipmapPageTable.Num();
	OutputLevel.NodeOffset = OutputAsset.ClipmapNodes.Num();

	if (bIncludeRawDebug)
	{
		OutputAsset.ClipmapRawIntervals.Append(SourceRawIntervals->GetData() + SourceLevel.RawIntervalOffset, SourceLevel.RawIntervalCount);
	}

	const int32 NodeOffsetDelta = OutputLevel.NodeOffset - SourceLevel.NodeOffset;
	const int32 IntervalOffsetDelta = OutputAsset.Intervals.Num();
	if (bIncludeRawDebug)
	{
		OutputAsset.Intervals.Append(SourceAsset.Intervals);
	}

	for (int32 NodeIndex = 0; NodeIndex < SourceLevel.NodeCount; ++NodeIndex)
	{
		FMHShadowNode Node = SourceNodes[SourceLevel.NodeOffset + NodeIndex];
		for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
		{
			const int32 ChildIndex = GetChildIndex(Node.ChildIndices, ChildSlot);
			SetChildIndex(Node.ChildIndices, ChildSlot, ChildIndex >= 0 ? ChildIndex + NodeOffsetDelta : INDEX_NONE);
		}
		if (Node.IntervalIndex >= 0)
		{
			Node.IntervalIndex = bIncludeRawDebug ? Node.IntervalIndex + IntervalOffsetDelta : INDEX_NONE;
		}
		OutputAsset.ClipmapNodes.Add(Node);
	}

	for (int32 TileIndex = 0; TileIndex < SourceLevel.TileDataCount; ++TileIndex)
	{
		FMHShadowTile Tile = SourceTiles[SourceLevel.TileOffset + TileIndex];
		const int32 GlobalTileIndex = OutputLevel.TileOffset + TileIndex;
		Tile.NodeOffset += NodeOffsetDelta;
		Tile.RootNodeIndex += NodeOffsetDelta;
		Tile.PageIndex = GlobalTileIndex;
		Tile.bResidentDefault = true;
		OutputAsset.ClipmapTiles.Add(Tile);
		OutputAsset.ClipmapPageTable.Add(GlobalTileIndex);
	}

	OutputLevel.NodeCount = OutputAsset.ClipmapNodes.Num() - OutputLevel.NodeOffset;
	OutputLevel.TileDataCount = SourceLevel.TileDataCount;
	OutputLevel.PageTableCount = SourceLevel.TileDataCount;
	OutputAsset.ClipmapLevels.Add(OutputLevel);
	return true;
}
}

UMHShadowMergeClipmapLevelsCommandlet::UMHShadowMergeClipmapLevelsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowMergeClipmapLevelsCommandlet::Main(const FString& Params)
{
	FString InputsValue;
	TArray<FString> InputPaths;
	if (ParseStringParam(Params, TEXT("Inputs="), InputsValue))
	{
		InputPaths = SplitInputs(InputsValue);
	}

	TArray<FString> IndexedInputPaths = ParseIndexedInputs(Params);
	if (IndexedInputPaths.Num() > 0)
	{
		InputPaths = MoveTemp(IndexedInputPaths);
	}
	const bool bIncludeRawDebug = ParseBoolParam(Params, TEXT("IncludeRawDebug="), false);

	if (InputPaths.Num() == 0)
	{
		UE_LOG(LogTemp, Error, TEXT("Missing Inputs=/Game/.../L0,/Game/.../L1,... or Input0=/Game/.../L0 Input1=/Game/.../L1 ..."));
		return 1;
	}

	FString OutputPath = TEXT("/Game/MHShadow/Baked/MHShadowData_LightmassDual_LargeCacheStress_RealClipmap");
	ParseStringParam(Params, TEXT("Output="), OutputPath);
	const FString OutputObjectPath = ToObjectPath(OutputPath);
	const FString OutputPackageName = FPackageName::ObjectPathToPackageName(OutputObjectPath);
	if (!FPackageName::IsValidLongPackageName(OutputPackageName))
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid output asset path: %s"), *OutputPath);
		return 1;
	}

	TArray<UMHShadowDataAsset*> InputAssets;
	UE_LOG(LogTemp, Display, TEXT("MH real clipmap merge input count: %d"), InputPaths.Num());
	for (const FString& InputPath : InputPaths)
	{
		UE_LOG(LogTemp, Display, TEXT("MH real clipmap merge input: %s"), *InputPath);
		UMHShadowDataAsset* InputAsset = LoadObject<UMHShadowDataAsset>(nullptr, *ToObjectPath(InputPath));
		if (!InputAsset)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to load clipmap level asset: %s"), *InputPath);
			return 1;
		}
		InputAssets.Add(InputAsset);
	}

	UPackage* Package = CreatePackage(*OutputPackageName);
	Package->FullyLoad();
	const FString AssetName = FPackageName::GetShortName(OutputPackageName);
	UMHShadowDataAsset* OutputAsset = NewObject<UMHShadowDataAsset>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!OutputAsset)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create output asset: %s"), *OutputObjectPath);
		return 1;
	}

	const UMHShadowDataAsset* First = InputAssets[0];
	OutputAsset->BakeSource = First->BakeSource;
	OutputAsset->ProjectionMapping = First->ProjectionMapping;
	OutputAsset->Resolution = First->Resolution;
	OutputAsset->TileSize = First->TileSize;
	OutputAsset->TileCount = First->TileCount;
	OutputAsset->WorldToShadowRow0 = First->WorldToShadowRow0;
	OutputAsset->WorldToShadowRow1 = First->WorldToShadowRow1;
	OutputAsset->WorldToShadowRow2 = First->WorldToShadowRow2;
	OutputAsset->WorldToShadowRow3 = First->WorldToShadowRow3;
	OutputAsset->LightOrigin = First->LightOrigin;
	OutputAsset->LightXAxis = First->LightXAxis;
	OutputAsset->LightYAxis = First->LightYAxis;
	OutputAsset->LightZAxis = First->LightZAxis;
	OutputAsset->LightSpaceMin = First->LightSpaceMin;
	OutputAsset->LightSpaceMax = First->LightSpaceMax;
	OutputAsset->MinLightDepth = First->MinLightDepth;
	OutputAsset->MaxLightDepth = First->MaxLightDepth;
	OutputAsset->RawIntervals.Reset();
	OutputAsset->RawIntervalFlags.Reset();
	OutputAsset->Intervals.Reset();
	OutputAsset->Nodes.Reset();
	OutputAsset->Tiles.Reset();
	OutputAsset->PageTable.Reset();
	OutputAsset->DebugIntervalPreview.Reset();

	OutputAsset->ClipmapLevels.Reset();
	OutputAsset->ClipmapRawIntervals.Reset();
	OutputAsset->ClipmapNodes.Reset();
	OutputAsset->ClipmapTiles.Reset();
	OutputAsset->ClipmapPageTable.Reset();

	int64 TotalSourceRawTexels = 0;
	int64 TotalSourceValidTexels = 0;
	UE_LOG(LogTemp, Display, TEXT("MH real clipmap merge IncludeRawDebug=%d"), bIncludeRawDebug ? 1 : 0);
	for (int32 LevelIndex = 0; LevelIndex < InputAssets.Num(); ++LevelIndex)
	{
		FString Error;
		if (!AppendLevelFromAsset(*OutputAsset, *InputAssets[LevelIndex], LevelIndex, bIncludeRawDebug, TotalSourceRawTexels, TotalSourceValidTexels, Error))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to append real clipmap level %d from %s: %s"), LevelIndex, *InputPaths[LevelIndex], *Error);
			return 1;
		}
	}

	OutputAsset->Stats.RawTexelCount = TotalSourceRawTexels;
	OutputAsset->Stats.ValidTexelCount = TotalSourceValidTexels;
	OutputAsset->Stats.NodeCount = OutputAsset->ClipmapNodes.Num();
	OutputAsset->Stats.IntervalCount = OutputAsset->Intervals.Num();
	OutputAsset->Stats.RawBytes = TotalSourceRawTexels * static_cast<int64>(sizeof(float) * 2);
	OutputAsset->Stats.CompressedBytes = EstimateCompressedBytes(*OutputAsset);
	OutputAsset->Stats.CompressionRatio = OutputAsset->Stats.RawBytes > 0
		? static_cast<float>(static_cast<double>(OutputAsset->Stats.CompressedBytes) / static_cast<double>(OutputAsset->Stats.RawBytes))
		: 1.0f;

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(OutputAsset);
	if (!SaveShadowDataAsset(OutputObjectPath, OutputAsset))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save merged real clipmap asset: %s"), *OutputObjectPath);
		return 1;
	}

	const FString StatsDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("RealClipmap"));
	IFileManager::Get().MakeDirectory(*StatsDir, true);
	FString Csv = TEXT("Level,Input,ResolutionX,ResolutionY,TileSize,TileCountX,TileCountY,TexelWorldSizeX,TexelWorldSizeY,RawIntervals,Tiles,Nodes,WorldToShadowRow0,WorldToShadowRow1,WorldToShadowRow2,WorldToShadowRow3\n");
	for (int32 LevelIndex = 0; LevelIndex < OutputAsset->ClipmapLevels.Num(); ++LevelIndex)
	{
		const FMHShadowClipmapLevel& Level = OutputAsset->ClipmapLevels[LevelIndex];
		Csv += FString::Printf(
			TEXT("%d,%s,%d,%d,%d,%d,%d,%.6f,%.6f,%d,%d,%d,\"%s\",\"%s\",\"%s\",\"%s\"\n"),
			LevelIndex,
			InputPaths.IsValidIndex(LevelIndex) ? *InputPaths[LevelIndex] : TEXT(""),
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
			*Level.WorldToShadowRow0.ToString(),
			*Level.WorldToShadowRow1.ToString(),
			*Level.WorldToShadowRow2.ToString(),
			*Level.WorldToShadowRow3.ToString());
	}
	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(StatsDir, AssetName + TEXT("_ClipmapLevelSummary.csv")));

	UE_LOG(LogTemp, Display, TEXT("Merged %d real clipmap levels into %s raw=%d tiles=%d nodes=%d ratio=%.6f"),
		OutputAsset->ClipmapLevels.Num(),
		*OutputObjectPath,
		OutputAsset->ClipmapRawIntervals.Num(),
		OutputAsset->ClipmapTiles.Num(),
		OutputAsset->ClipmapNodes.Num(),
		OutputAsset->Stats.CompressionRatio);
	return 0;
}

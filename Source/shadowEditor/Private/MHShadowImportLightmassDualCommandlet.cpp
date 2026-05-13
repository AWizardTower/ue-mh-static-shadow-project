// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowImportLightmassDualCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "MHShadowCompressor.h"
#include "MHShadowDataAsset.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/Archive.h"
#include "UObject/SavePackage.h"

namespace
{
enum EMHDualShadowFlags : uint8
{
	MHDSF_Valid = 1 << 0,
	MHDSF_ThinFallback = 1 << 1,
	MHDSF_Unpaired = 1 << 2,
	MHDSF_MultiHit = 1 << 3,
	MHDSF_Empty = 1 << 4,
};

struct FMHDualShadowMapFileData
{
	FMatrix44f WorldToLight = FMatrix44f::Identity;
	int32 ShadowMapSizeX = 0;
	int32 ShadowMapSizeY = 0;
	int32 ValidTexelCount = 0;
	int32 EmptyTexelCount = 0;
	int32 ThinFallbackTexelCount = 0;
	int32 UnpairedTexelCount = 0;
	int32 MultiHitTexelCount = 0;
	int32 PairedTexelCount = 0;
	float AverageThickness = 0.0f;
	float MaxThickness = 0.0f;
	float BakeSeconds = 0.0f;
	FVector4f LightSpaceBoundsMin = FVector4f::Zero();
	FVector4f LightSpaceBoundsMax = FVector4f::Zero();
	FVector4f WorldImportanceBoundsMin = FVector4f::Zero();
	FVector4f WorldImportanceBoundsMax = FVector4f::Zero();
	FVector4f LightSpaceRect = FVector4f::Zero();
	float TexelWorldSizeX = 0.0f;
	float TexelWorldSizeY = 0.0f;
};

struct FMHDualShadowMapFileDataV3
{
	FMatrix44f WorldToLight = FMatrix44f::Identity;
	int32 ShadowMapSizeX = 0;
	int32 ShadowMapSizeY = 0;
	int32 ValidTexelCount = 0;
	int32 EmptyTexelCount = 0;
	int32 ThinFallbackTexelCount = 0;
	int32 UnpairedTexelCount = 0;
	int32 MultiHitTexelCount = 0;
	float AverageThickness = 0.0f;
	float MaxThickness = 0.0f;
	float BakeSeconds = 0.0f;
	FVector4f LightSpaceBoundsMin = FVector4f::Zero();
	FVector4f LightSpaceBoundsMax = FVector4f::Zero();
};

struct FMHDualShadowMapFileSample
{
	FFloat16 FrontDepth;
	FFloat16 BackDepth;
	uint8 Flags = MHDSF_Empty;
	uint8 Padding[3] = {0, 0, 0};
};

struct FMHDualShadowMapFileDebugRay
{
	int32 TexelX = 0;
	int32 TexelY = 0;
	int32 FirstHitIndex = 0;
	int32 HitCount = 0;
	float FrontDepth = 0.0f;
	float BackDepth = 0.0f;
	uint8 Flags = MHDSF_Empty;
	uint8 Padding[3] = {0, 0, 0};
};

struct FMHDualShadowMapFileDebugHit
{
	int32 RayIndex = 0;
	int32 HitIndex = 0;
	int32 ElementIndex = INDEX_NONE;
	uint8 Flags = 0;
	uint8 Padding[3] = {0, 0, 0};
	float Distance = 0.0f;
	float NormalizedDepth = 0.0f;
	FVector4f WorldPosition = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);
	FVector4f WorldNormal = FVector4f(0.0f, 0.0f, 1.0f, 0.0f);
	FGuid MeshGuid;
	FGuid ObjectGuid;
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

static FString ToObjectPath(const FString& AssetPath)
{
	if (AssetPath.Contains(TEXT(".")))
	{
		return AssetPath;
	}

	const FString AssetName = FPackageName::GetShortName(AssetPath);
	return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

static FString GetAssetStemFromObjectPath(const FString& ObjectPath)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	return FPackageName::GetShortName(PackageName);
}

static FString GetLightmassDualOutputDir(const FString& OutputObjectPath)
{
	const FString DebugDir = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("MHShadow"),
		TEXT("LightmassDual"),
		GetAssetStemFromObjectPath(OutputObjectPath));
	IFileManager::Get().MakeDirectory(*DebugDir, true);
	return DebugDir;
}

static bool FindLatestLightmassDualFile(FString& OutFile)
{
	TArray<FString> Files;
	const FString NewSearchDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("LightmassDual"), TEXT("Sidecar"));
	IFileManager::Get().FindFilesRecursive(Files, *NewSearchDir, TEXT("*.mhdual"), true, false);
	if (Files.Num() == 0)
	{
		const FString LegacySearchDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("Lightmass"));
		IFileManager::Get().FindFilesRecursive(Files, *LegacySearchDir, TEXT("*.mhdual"), true, false);
	}
	if (Files.Num() == 0)
	{
		return false;
	}

	Files.Sort([](const FString& A, const FString& B)
	{
		return IFileManager::Get().GetTimeStamp(*A) > IFileManager::Get().GetTimeStamp(*B);
	});
	OutFile = Files[0];
	return true;
}

static bool LoadLightmassDualFile(
	const FString& File,
	FGuid& OutLightGuid,
	FMHDualShadowMapFileData& OutData,
	TArray<FMHDualShadowMapFileSample>& OutSamples,
	TArray<FMHDualShadowMapFileDebugRay>& OutDebugRays,
	TArray<FMHDualShadowMapFileDebugHit>& OutDebugHits)
{
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*File));
	if (!Reader)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to open MH dual shadow sidecar: %s"), *File);
		return false;
	}

	uint32 Magic = 0;
	int32 Version = 0;
	*Reader << Magic;
	*Reader << Version;
	*Reader << OutLightGuid;
	if (Magic != 0x5344484D || (Version != 3 && Version != 4))
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid MH dual shadow sidecar header: %s Magic=0x%08x Version=%d. Rebuild lighting to regenerate a v3/v4 sidecar."), *File, Magic, Version);
		return false;
	}

	if (Version == 3)
	{
		FMHDualShadowMapFileDataV3 LegacyData;
		Reader->Serialize(&LegacyData, sizeof(LegacyData));
		OutData.WorldToLight = LegacyData.WorldToLight;
		OutData.ShadowMapSizeX = LegacyData.ShadowMapSizeX;
		OutData.ShadowMapSizeY = LegacyData.ShadowMapSizeY;
		OutData.ValidTexelCount = LegacyData.ValidTexelCount;
		OutData.EmptyTexelCount = LegacyData.EmptyTexelCount;
		OutData.ThinFallbackTexelCount = LegacyData.ThinFallbackTexelCount;
		OutData.UnpairedTexelCount = LegacyData.UnpairedTexelCount;
		OutData.MultiHitTexelCount = LegacyData.MultiHitTexelCount;
		OutData.PairedTexelCount = FMath::Max(0, LegacyData.ValidTexelCount - LegacyData.ThinFallbackTexelCount - LegacyData.UnpairedTexelCount);
		OutData.AverageThickness = LegacyData.AverageThickness;
		OutData.MaxThickness = LegacyData.MaxThickness;
		OutData.BakeSeconds = LegacyData.BakeSeconds;
		OutData.LightSpaceBoundsMin = LegacyData.LightSpaceBoundsMin;
		OutData.LightSpaceBoundsMax = LegacyData.LightSpaceBoundsMax;
		OutData.LightSpaceRect = FVector4f(LegacyData.LightSpaceBoundsMin.X, LegacyData.LightSpaceBoundsMin.Y, LegacyData.LightSpaceBoundsMax.X, LegacyData.LightSpaceBoundsMax.Y);
	}
	else
	{
		Reader->Serialize(&OutData, sizeof(OutData));
	}
	int32 SampleCount = 0;
	*Reader << SampleCount;
	if (SampleCount <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("MH dual shadow sidecar has no samples: %s"), *File);
		return false;
	}

	OutSamples.Empty(SampleCount);
	OutSamples.AddZeroed(SampleCount);
	Reader->Serialize(OutSamples.GetData(), OutSamples.GetTypeSize() * SampleCount);

	int32 DebugRayCount = 0;
	*Reader << DebugRayCount;
	if (DebugRayCount < 0)
	{
		UE_LOG(LogTemp, Error, TEXT("MH dual shadow sidecar has invalid debug ray count: %s count=%d"), *File, DebugRayCount);
		return false;
	}
	OutDebugRays.Empty(DebugRayCount);
	OutDebugRays.AddZeroed(DebugRayCount);
	if (DebugRayCount > 0)
	{
		Reader->Serialize(OutDebugRays.GetData(), OutDebugRays.GetTypeSize() * DebugRayCount);
	}

	int32 DebugHitCount = 0;
	*Reader << DebugHitCount;
	if (DebugHitCount < 0)
	{
		UE_LOG(LogTemp, Error, TEXT("MH dual shadow sidecar has invalid debug hit count: %s count=%d"), *File, DebugHitCount);
		return false;
	}
	OutDebugHits.Empty(DebugHitCount);
	OutDebugHits.AddZeroed(DebugHitCount);
	if (DebugHitCount > 0)
	{
		Reader->Serialize(OutDebugHits.GetData(), OutDebugHits.GetTypeSize() * DebugHitCount);
	}
	return !Reader->IsError();
}

static FColor HeatColor(float T)
{
	T = FMath::Clamp(T, 0.0f, 1.0f);
	const uint8 R = static_cast<uint8>(FMath::Clamp(T * 255.0f, 0.0f, 255.0f));
	const uint8 G = static_cast<uint8>(FMath::Clamp((1.0f - FMath::Abs(T - 0.5f) * 2.0f) * 255.0f, 0.0f, 255.0f));
	const uint8 B = static_cast<uint8>(FMath::Clamp((1.0f - T) * 255.0f, 0.0f, 255.0f));
	return FColor(R, G, B, 255);
}

static FColor FlagColor(uint8 Flags)
{
	if ((Flags & MHDSF_MultiHit) != 0)
	{
		return FColor::Red;
	}
	if ((Flags & MHDSF_Unpaired) != 0)
	{
		return FColor(180, 0, 255, 255);
	}
	if ((Flags & MHDSF_ThinFallback) != 0)
	{
		return FColor::Yellow;
	}
	if ((Flags & MHDSF_Valid) != 0)
	{
		return FColor::Black;
	}
	return FColor::White;
}

static bool SavePng(const FString& Path, int32 SizeX, int32 SizeY, const TArray<FColor>& Pixels)
{
	TArray64<uint8> PngData;
	FImageUtils::PNGCompressImageArray(
		SizeX,
		SizeY,
		TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()),
		PngData);
	return FFileHelper::SaveArrayToFile(PngData, *Path);
}

static void WriteLightmassDualDebugOutputs(
	const FString& OutputObjectPath,
	const FMHDualShadowMapFileData& Data,
	const TArray<FMHShadowDepthInterval>& Intervals,
	const TArray<uint8>& Flags)
{
	const int32 SizeX = Data.ShadowMapSizeX;
	const int32 SizeY = Data.ShadowMapSizeY;
	const int32 TexelCount = SizeX * SizeY;
	if (TexelCount <= 0 || Intervals.Num() != TexelCount || Flags.Num() != TexelCount)
	{
		return;
	}

	TArray<FColor> FrontDepth;
	TArray<FColor> BackDepth;
	TArray<FColor> Thickness;
	TArray<FColor> Validity;
	TArray<FColor> Difficulty;
	FrontDepth.SetNumUninitialized(TexelCount);
	BackDepth.SetNumUninitialized(TexelCount);
	Thickness.SetNumUninitialized(TexelCount);
	Validity.SetNumUninitialized(TexelCount);
	Difficulty.SetNumUninitialized(TexelCount);
	TArray<FColor> Coverage;
	Coverage.SetNumUninitialized(TexelCount);

	for (int32 Y = 0; Y < SizeY; ++Y)
	{
		for (int32 X = 0; X < SizeX; ++X)
		{
			const int32 Index = Y * SizeX + X;
			const FMHShadowDepthInterval& Interval = Intervals[Index];
			const uint8 U = SizeX > 1 ? static_cast<uint8>(FMath::RoundToInt(255.0f * X / static_cast<float>(SizeX - 1))) : 0;
			const uint8 V = SizeY > 1 ? static_cast<uint8>(FMath::RoundToInt(255.0f * Y / static_cast<float>(SizeY - 1))) : 0;
			const uint8 Checker = ((X / 16 + Y / 16) & 1) != 0 ? 48 : 0;
			Coverage[Index] = FColor(U, V, static_cast<uint8>(128 + Checker), 255);
			if (Interval.bValid)
			{
				const uint8 Front = static_cast<uint8>(FMath::Clamp(Interval.MinDepth * 255.0f, 0.0f, 255.0f));
				const uint8 Back = static_cast<uint8>(FMath::Clamp(Interval.MaxDepth * 255.0f, 0.0f, 255.0f));
				const float ThicknessValue = FMath::Max(0.0f, Interval.MaxDepth - Interval.MinDepth);
				FrontDepth[Index] = FColor(Front, Front, Front, 255);
				BackDepth[Index] = FColor(Back, Back, Back, 255);
				Thickness[Index] = HeatColor(ThicknessValue * 32.0f);

				float Gradient = 0.0f;
				if (X + 1 < SizeX && Intervals[Index + 1].bValid)
				{
					Gradient += FMath::Abs(Interval.MinDepth - Intervals[Index + 1].MinDepth);
				}
				if (Y + 1 < SizeY && Intervals[Index + SizeX].bValid)
				{
					Gradient += FMath::Abs(Interval.MinDepth - Intervals[Index + SizeX].MinDepth);
				}
				Difficulty[Index] = HeatColor(Gradient * 64.0f + ThicknessValue * 8.0f);
			}
			else
			{
				FrontDepth[Index] = FColor::White;
				BackDepth[Index] = FColor::White;
				Thickness[Index] = FColor::Black;
				Difficulty[Index] = FColor::Black;
			}

			Validity[Index] = FlagColor(Flags[Index]);
		}
	}

	const FString SafeName = GetAssetStemFromObjectPath(OutputObjectPath);
	const FString DebugDir = GetLightmassDualOutputDir(OutputObjectPath);
	SavePng(FPaths::Combine(DebugDir, SafeName + TEXT("_LightmassDual_Coverage.png")), SizeX, SizeY, Coverage);
	SavePng(FPaths::Combine(DebugDir, SafeName + TEXT("_LightmassDual_FrontDepth.png")), SizeX, SizeY, FrontDepth);
	SavePng(FPaths::Combine(DebugDir, SafeName + TEXT("_LightmassDual_BackDepth.png")), SizeX, SizeY, BackDepth);
	SavePng(FPaths::Combine(DebugDir, SafeName + TEXT("_LightmassDual_Thickness.png")), SizeX, SizeY, Thickness);
	SavePng(FPaths::Combine(DebugDir, SafeName + TEXT("_LightmassDual_ValidityFlags.png")), SizeX, SizeY, Validity);
	SavePng(FPaths::Combine(DebugDir, SafeName + TEXT("_LightmassDual_CompressionDifficulty.png")), SizeX, SizeY, Difficulty);
}

static void WriteLightmassDualStatsCsv(
	const FString& OutputObjectPath,
	const FMHDualShadowMapFileData& Data,
	const UMHShadowDataAsset& Asset,
	const FGuid& LightGuid,
	const FString& SourceFile)
{
	const FString SafeName = GetAssetStemFromObjectPath(OutputObjectPath);
	const FString StatsDir = GetLightmassDualOutputDir(OutputObjectPath);

	const FString Csv =
		TEXT("Source,LightGuid,SourceFile,Resolution,RawTexels,ValidTexels,EmptyTexels,PairedTexels,ThinFallbackTexels,UnpairedTexels,MultiHitTexels,AverageThickness,MaxThickness,Nodes,Intervals,RawBytes,CompressedBytes,CompressionRatio,BakeSeconds\n")
		+ FString::Printf(
			TEXT("LightmassDual,%s,%s,%dx%d,%d,%d,%d,%d,%d,%d,%d,%.8f,%.8f,%d,%d,%lld,%lld,%.6f,%.3f\n"),
			*LightGuid.ToString(EGuidFormats::DigitsWithHyphens),
			*SourceFile,
			Data.ShadowMapSizeX,
			Data.ShadowMapSizeY,
			Asset.Stats.RawTexelCount,
			Asset.Stats.ValidTexelCount,
			Data.EmptyTexelCount,
			Data.PairedTexelCount,
			Data.ThinFallbackTexelCount,
			Data.UnpairedTexelCount,
			Data.MultiHitTexelCount,
			Data.AverageThickness,
			Data.MaxThickness,
			Asset.Stats.NodeCount,
			Asset.Stats.IntervalCount,
			Asset.Stats.RawBytes,
			Asset.Stats.CompressedBytes,
			Asset.Stats.CompressionRatio,
			Asset.Stats.BakeSeconds);

	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(StatsDir, SafeName + TEXT("_LightmassDual_Stats.csv")));

	const FMatrix Matrix(Data.WorldToLight);
	FString BoundsCsv;
	BoundsCsv += TEXT("Key,X,Y,Z,W\n");
	BoundsCsv += FString::Printf(TEXT("Resolution,%d,%d,0,0\n"), Data.ShadowMapSizeX, Data.ShadowMapSizeY);
	BoundsCsv += FString::Printf(TEXT("LightSpaceBoundsMin,%.9f,%.9f,%.9f,%.9f\n"), Data.LightSpaceBoundsMin.X, Data.LightSpaceBoundsMin.Y, Data.LightSpaceBoundsMin.Z, Data.LightSpaceBoundsMin.W);
	BoundsCsv += FString::Printf(TEXT("LightSpaceBoundsMax,%.9f,%.9f,%.9f,%.9f\n"), Data.LightSpaceBoundsMax.X, Data.LightSpaceBoundsMax.Y, Data.LightSpaceBoundsMax.Z, Data.LightSpaceBoundsMax.W);
	BoundsCsv += FString::Printf(TEXT("WorldImportanceBoundsMin,%.9f,%.9f,%.9f,%.9f\n"), Data.WorldImportanceBoundsMin.X, Data.WorldImportanceBoundsMin.Y, Data.WorldImportanceBoundsMin.Z, Data.WorldImportanceBoundsMin.W);
	BoundsCsv += FString::Printf(TEXT("WorldImportanceBoundsMax,%.9f,%.9f,%.9f,%.9f\n"), Data.WorldImportanceBoundsMax.X, Data.WorldImportanceBoundsMax.Y, Data.WorldImportanceBoundsMax.Z, Data.WorldImportanceBoundsMax.W);
	BoundsCsv += FString::Printf(TEXT("LightSpaceRect,%.9f,%.9f,%.9f,%.9f\n"), Data.LightSpaceRect.X, Data.LightSpaceRect.Y, Data.LightSpaceRect.Z, Data.LightSpaceRect.W);
	BoundsCsv += FString::Printf(TEXT("TexelWorldSize,%.9f,%.9f,0,0\n"), Data.TexelWorldSizeX, Data.TexelWorldSizeY);
	BoundsCsv += FString::Printf(TEXT("WorldToShadowRow0,%.9f,%.9f,%.9f,%.9f\n"), Matrix.M[0][0], Matrix.M[0][1], Matrix.M[0][2], Matrix.M[0][3]);
	BoundsCsv += FString::Printf(TEXT("WorldToShadowRow1,%.9f,%.9f,%.9f,%.9f\n"), Matrix.M[1][0], Matrix.M[1][1], Matrix.M[1][2], Matrix.M[1][3]);
	BoundsCsv += FString::Printf(TEXT("WorldToShadowRow2,%.9f,%.9f,%.9f,%.9f\n"), Matrix.M[2][0], Matrix.M[2][1], Matrix.M[2][2], Matrix.M[2][3]);
	BoundsCsv += FString::Printf(TEXT("WorldToShadowRow3,%.9f,%.9f,%.9f,%.9f\n"), Matrix.M[3][0], Matrix.M[3][1], Matrix.M[3][2], Matrix.M[3][3]);
	FFileHelper::SaveStringToFile(BoundsCsv, *FPaths::Combine(StatsDir, SafeName + TEXT("_LightmassDual_TileInfo.csv")));
}

static void WriteLightmassDualDepthStatsCsv(
	const FString& OutputObjectPath,
	const TArray<FMHShadowDepthInterval>& Intervals,
	const TArray<uint8>& Flags)
{
	const FString SafeName = GetAssetStemFromObjectPath(OutputObjectPath);
	const FString StatsDir = GetLightmassDualOutputDir(OutputObjectPath);

	int32 ValidCount = 0;
	int32 EmptyCount = 0;
	int32 ThinCount = 0;
	int32 UnpairedCount = 0;
	int32 MultiHitCount = 0;
	int32 PairedCount = 0;
	int32 OrderedCount = 0;
	int32 InvertedCount = 0;
	int32 NonFiniteCount = 0;
	double FrontSum = 0.0;
	double BackSum = 0.0;
	double ThicknessSum = 0.0;
	float FrontMin = TNumericLimits<float>::Max();
	float FrontMax = TNumericLimits<float>::Lowest();
	float BackMin = TNumericLimits<float>::Max();
	float BackMax = TNumericLimits<float>::Lowest();
	float ThicknessMin = TNumericLimits<float>::Max();
	float ThicknessMax = TNumericLimits<float>::Lowest();

	for (int32 Index = 0; Index < Intervals.Num(); ++Index)
	{
		const uint8 SampleFlags = Flags.IsValidIndex(Index) ? Flags[Index] : 0;
		EmptyCount += (SampleFlags & MHDSF_Empty) ? 1 : 0;
		ThinCount += (SampleFlags & MHDSF_ThinFallback) ? 1 : 0;
		UnpairedCount += (SampleFlags & MHDSF_Unpaired) ? 1 : 0;
		MultiHitCount += (SampleFlags & MHDSF_MultiHit) ? 1 : 0;

		const FMHShadowDepthInterval& Interval = Intervals[Index];
		if (!Interval.bValid)
		{
			continue;
		}

		const float Front = Interval.MinDepth;
		const float Back = Interval.MaxDepth;
		if (!FMath::IsFinite(Front) || !FMath::IsFinite(Back))
		{
			++NonFiniteCount;
			continue;
		}

		++ValidCount;
		PairedCount += (SampleFlags & (MHDSF_ThinFallback | MHDSF_Unpaired)) == 0 ? 1 : 0;
		const float Thickness = Back - Front;
		if (Front <= Back)
		{
			++OrderedCount;
		}
		else
		{
			++InvertedCount;
		}

		FrontMin = FMath::Min(FrontMin, Front);
		FrontMax = FMath::Max(FrontMax, Front);
		BackMin = FMath::Min(BackMin, Back);
		BackMax = FMath::Max(BackMax, Back);
		ThicknessMin = FMath::Min(ThicknessMin, Thickness);
		ThicknessMax = FMath::Max(ThicknessMax, Thickness);
		FrontSum += Front;
		BackSum += Back;
		ThicknessSum += Thickness;
	}

	const auto SafeAverage = [](double Sum, int32 Count)
	{
		return Count > 0 ? static_cast<float>(Sum / static_cast<double>(Count)) : 0.0f;
	};
	if (ValidCount == 0)
	{
		FrontMin = FrontMax = BackMin = BackMax = ThicknessMin = ThicknessMax = 0.0f;
	}

	FString Csv;
	Csv += TEXT("Key,Value\n");
	Csv += FString::Printf(TEXT("Texels,%d\n"), Intervals.Num());
	Csv += FString::Printf(TEXT("ValidTexels,%d\n"), ValidCount);
	Csv += FString::Printf(TEXT("EmptyFlagTexels,%d\n"), EmptyCount);
	Csv += FString::Printf(TEXT("PairedFlagTexels,%d\n"), PairedCount);
	Csv += FString::Printf(TEXT("ThinFallbackFlagTexels,%d\n"), ThinCount);
	Csv += FString::Printf(TEXT("UnpairedFlagTexels,%d\n"), UnpairedCount);
	Csv += FString::Printf(TEXT("MultiHitFlagTexels,%d\n"), MultiHitCount);
	Csv += FString::Printf(TEXT("OrderedIntervals,%d\n"), OrderedCount);
	Csv += FString::Printf(TEXT("InvertedIntervals,%d\n"), InvertedCount);
	Csv += FString::Printf(TEXT("NonFiniteIntervals,%d\n"), NonFiniteCount);
	Csv += FString::Printf(TEXT("FrontMin,%.9f\n"), FrontMin);
	Csv += FString::Printf(TEXT("FrontMax,%.9f\n"), FrontMax);
	Csv += FString::Printf(TEXT("FrontAvg,%.9f\n"), SafeAverage(FrontSum, ValidCount));
	Csv += FString::Printf(TEXT("BackMin,%.9f\n"), BackMin);
	Csv += FString::Printf(TEXT("BackMax,%.9f\n"), BackMax);
	Csv += FString::Printf(TEXT("BackAvg,%.9f\n"), SafeAverage(BackSum, ValidCount));
	Csv += FString::Printf(TEXT("ThicknessMin,%.9f\n"), ThicknessMin);
	Csv += FString::Printf(TEXT("ThicknessMax,%.9f\n"), ThicknessMax);
	Csv += FString::Printf(TEXT("ThicknessAvg,%.9f\n"), SafeAverage(ThicknessSum, ValidCount));
	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(StatsDir, SafeName + TEXT("_LightmassDual_DepthStats.csv")));
}

static void WriteLightmassDualHitSequenceCsv(
	const FString& OutputObjectPath,
	const TArray<FMHDualShadowMapFileDebugRay>& DebugRays,
	const TArray<FMHDualShadowMapFileDebugHit>& DebugHits)
{
	const FString SafeName = GetAssetStemFromObjectPath(OutputObjectPath);
	const FString StatsDir = GetLightmassDualOutputDir(OutputObjectPath);

	FString RayCsv;
	RayCsv += TEXT("RayIndex,TexelX,TexelY,FirstHitIndex,HitCount,FrontDepth,BackDepth,Flags\n");
	for (int32 RayIndex = 0; RayIndex < DebugRays.Num(); ++RayIndex)
	{
		const FMHDualShadowMapFileDebugRay& Ray = DebugRays[RayIndex];
		RayCsv += FString::Printf(
			TEXT("%d,%d,%d,%d,%d,%.9f,%.9f,%u\n"),
			RayIndex,
			Ray.TexelX,
			Ray.TexelY,
			Ray.FirstHitIndex,
			Ray.HitCount,
			Ray.FrontDepth,
			Ray.BackDepth,
			static_cast<uint32>(Ray.Flags));
	}
	FFileHelper::SaveStringToFile(RayCsv, *FPaths::Combine(StatsDir, SafeName + TEXT("_LightmassDual_DebugRays.csv")));

	FString HitCsv;
	HitCsv += TEXT("RayIndex,TexelX,TexelY,HitIndex,ElementIndex,Distance,NormalizedDepth,WorldX,WorldY,WorldZ,NormalX,NormalY,NormalZ,MeshGuid,ObjectGuid,Flags\n");
	for (const FMHDualShadowMapFileDebugHit& Hit : DebugHits)
	{
		const FMHDualShadowMapFileDebugRay* Ray = DebugRays.IsValidIndex(Hit.RayIndex) ? &DebugRays[Hit.RayIndex] : nullptr;
		HitCsv += FString::Printf(
			TEXT("%d,%d,%d,%d,%d,%.6f,%.9f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%s,%s,%u\n"),
			Hit.RayIndex,
			Ray ? Ray->TexelX : INDEX_NONE,
			Ray ? Ray->TexelY : INDEX_NONE,
			Hit.HitIndex,
			Hit.ElementIndex,
			Hit.Distance,
			Hit.NormalizedDepth,
			Hit.WorldPosition.X,
			Hit.WorldPosition.Y,
			Hit.WorldPosition.Z,
			Hit.WorldNormal.X,
			Hit.WorldNormal.Y,
			Hit.WorldNormal.Z,
			*Hit.MeshGuid.ToString(EGuidFormats::DigitsWithHyphens),
			*Hit.ObjectGuid.ToString(EGuidFormats::DigitsWithHyphens),
			static_cast<uint32>(Hit.Flags));
	}
	FFileHelper::SaveStringToFile(HitCsv, *FPaths::Combine(StatsDir, SafeName + TEXT("_LightmassDual_HitSequence.csv")));
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

static int64 GetCompressedNodeBytes(int32 NodeCount)
{
	return static_cast<int64>(NodeCount) * static_cast<int64>(sizeof(int32) * 4 + sizeof(float) * 3 + sizeof(uint32));
}

static TArray<FMHShadowDepthInterval> DownsampleIntervals2x(
	const TArray<FMHShadowDepthInterval>& SourceIntervals,
	FIntPoint SourceResolution,
	FIntPoint& OutResolution)
{
	OutResolution = FIntPoint(SourceResolution.X / 2, SourceResolution.Y / 2);
	TArray<FMHShadowDepthInterval> Downsampled;
	Downsampled.SetNum(OutResolution.X * OutResolution.Y);

	for (int32 Y = 0; Y < OutResolution.Y; ++Y)
	{
		for (int32 X = 0; X < OutResolution.X; ++X)
		{
			FMHShadowDepthInterval& Out = Downsampled[Y * OutResolution.X + X];
			bool bAnyValid = false;
			float MinDepth = 1.0f;
			float MaxDepth = 0.0f;
			for (int32 OffsetY = 0; OffsetY < 2; ++OffsetY)
			{
				for (int32 OffsetX = 0; OffsetX < 2; ++OffsetX)
				{
					const int32 SourceX = X * 2 + OffsetX;
					const int32 SourceY = Y * 2 + OffsetY;
					const int32 SourceIndex = SourceY * SourceResolution.X + SourceX;
					if (!SourceIntervals.IsValidIndex(SourceIndex) || !SourceIntervals[SourceIndex].bValid)
					{
						continue;
					}

					bAnyValid = true;
					MinDepth = FMath::Min(MinDepth, SourceIntervals[SourceIndex].MinDepth);
					MaxDepth = FMath::Max(MaxDepth, SourceIntervals[SourceIndex].MaxDepth);
				}
			}

			Out.MinDepth = bAnyValid ? MinDepth : 1.0f;
			Out.MaxDepth = bAnyValid ? MaxDepth : 1.0f;
			Out.bValid = bAnyValid;
		}
	}

	return Downsampled;
}

static bool AppendCompressedClipmapLevel(
	UMHShadowDataAsset& Asset,
	const TArray<FMHShadowDepthInterval>& LevelIntervals,
	FIntPoint LevelResolution,
	int32 LevelIndex,
	int32 TileSize,
	FVector2D TexelWorldSize,
	double& InOutCompressionSeconds,
	int64& InOutCompressedBytes)
{
	if (LevelResolution.X <= 0 || LevelResolution.Y <= 0 || TileSize <= 0)
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid clipmap level %d resolution=%dx%d tileSize=%d."),
			LevelIndex,
			LevelResolution.X,
			LevelResolution.Y,
			TileSize);
		return false;
	}
	if ((LevelResolution.X % TileSize) != 0 || (LevelResolution.Y % TileSize) != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("Clipmap level %d resolution=%dx%d must be divisible by TileSize=%d."),
			LevelIndex,
			LevelResolution.X,
			LevelResolution.Y,
			TileSize);
		return false;
	}
	if (LevelIntervals.Num() != LevelResolution.X * LevelResolution.Y)
	{
		UE_LOG(LogTemp, Error, TEXT("Clipmap level %d expected %d intervals, got %d."),
			LevelIndex,
			LevelResolution.X * LevelResolution.Y,
			LevelIntervals.Num());
		return false;
	}

	FMHShadowClipmapLevel Level;
	Level.LevelIndex = LevelIndex;
	Level.Resolution = LevelResolution;
	Level.TileSize = TileSize;
	Level.TileCount = FIntPoint(LevelResolution.X / TileSize, LevelResolution.Y / TileSize);
	Level.RawIntervalOffset = Asset.ClipmapRawIntervals.Num();
	Level.RawIntervalCount = LevelIntervals.Num();
	Level.TileOffset = Asset.ClipmapTiles.Num();
	Level.TileDataCount = Level.TileCount.X * Level.TileCount.Y;
	Level.PageTableOffset = Asset.ClipmapPageTable.Num();
	Level.PageTableCount = Level.TileDataCount;
	Level.NodeOffset = Asset.ClipmapNodes.Num();
	Level.TexelWorldSize = TexelWorldSize;
	Level.WorldToShadowRow0 = Asset.WorldToShadowRow0;
	Level.WorldToShadowRow1 = Asset.WorldToShadowRow1;
	Level.WorldToShadowRow2 = Asset.WorldToShadowRow2;
	Level.WorldToShadowRow3 = Asset.WorldToShadowRow3;

	Asset.ClipmapRawIntervals.Append(LevelIntervals);
	Asset.ClipmapTiles.Reserve(Asset.ClipmapTiles.Num() + Level.TileDataCount);
	Asset.ClipmapPageTable.Reserve(Asset.ClipmapPageTable.Num() + Level.TileDataCount);

	for (int32 TileY = 0; TileY < Level.TileCount.Y; ++TileY)
	{
		for (int32 TileX = 0; TileX < Level.TileCount.X; ++TileX)
		{
			FMHShadowCompressionInput TileInput;
			TileInput.Resolution = FIntPoint(TileSize, TileSize);
			TileInput.TexelIntervals.SetNumUninitialized(TileSize * TileSize);

			int32 TileValidTexels = 0;
			for (int32 LocalY = 0; LocalY < TileSize; ++LocalY)
			{
				const int32 SourceY = TileY * TileSize + LocalY;
				for (int32 LocalX = 0; LocalX < TileSize; ++LocalX)
				{
					const int32 SourceX = TileX * TileSize + LocalX;
					const int32 SourceIndex = SourceY * LevelResolution.X + SourceX;
					const int32 LocalIndex = LocalY * TileSize + LocalX;
					TileInput.TexelIntervals[LocalIndex] = LevelIntervals[SourceIndex];
					TileValidTexels += LevelIntervals[SourceIndex].bValid ? 1 : 0;
				}
			}

			FMHShadowCompressionOutput TileOutput;
			FString CompressionError;
			if (!FMHShadowCompressor::Compress(TileInput, TileOutput, &CompressionError))
			{
				UE_LOG(LogTemp, Error, TEXT("Clipmap level %d tile compression failed at tile=(%d,%d): %s"),
					LevelIndex,
					TileX,
					TileY,
					*CompressionError);
				return false;
			}

			const int32 NodeOffset = Asset.ClipmapNodes.Num();
			const int32 IntervalOffset = Asset.Intervals.Num();
			for (FMHShadowNode Node : TileOutput.Nodes)
			{
				for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
				{
					const int32 LocalChildIndex = GetChildIndex(Node.ChildIndices, ChildSlot);
					SetChildIndex(Node.ChildIndices, ChildSlot, LocalChildIndex >= 0 ? LocalChildIndex + NodeOffset : INDEX_NONE);
				}
				if (Node.IntervalIndex >= 0)
				{
					Node.IntervalIndex += IntervalOffset;
				}
				Asset.ClipmapNodes.Add(Node);
			}
			Asset.Intervals.Append(TileOutput.Intervals);

			const int32 LocalTileIndex = TileY * Level.TileCount.X + TileX;
			const int32 GlobalTileIndex = Level.TileOffset + LocalTileIndex;
			FMHShadowTile Tile;
			Tile.TileCoord = FIntPoint(TileX, TileY);
			Tile.TexelRect = FIntVector4(
				TileX * TileSize,
				TileY * TileSize,
				(TileX + 1) * TileSize,
				(TileY + 1) * TileSize);
			Tile.NodeOffset = NodeOffset;
			Tile.NodeCount = TileOutput.Nodes.Num();
			Tile.RootNodeIndex = NodeOffset;
			Tile.PageIndex = GlobalTileIndex;
			Tile.bResidentDefault = true;
			Tile.RawTexelCount = TileSize * TileSize;
			Tile.ValidTexelCount = TileValidTexels;
			Tile.CompressedNodeCount = TileOutput.Nodes.Num();
			Tile.CompressionRatio = TileOutput.Stats.CompressionRatio;
			Asset.ClipmapTiles.Add(Tile);
			Asset.ClipmapPageTable.Add(GlobalTileIndex);

			InOutCompressionSeconds += TileOutput.Stats.BakeSeconds;
			InOutCompressedBytes += GetCompressedNodeBytes(TileOutput.Nodes.Num());
		}
	}

	Level.NodeCount = Asset.ClipmapNodes.Num() - Level.NodeOffset;
	Asset.ClipmapLevels.Add(Level);
	return true;
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
		if (!Nodes.IsValidIndex(NodeIndex))
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

static void BuildLightmassDualTileData(
	const FString& OutputObjectPath,
	UMHShadowDataAsset& Asset)
{
	const FString SafeName = GetAssetStemFromObjectPath(OutputObjectPath);
	const FString StatsDir = GetLightmassDualOutputDir(OutputObjectPath);
	const int32 TileCount = Asset.Tiles.Num();

	int32 EmptyTileCount = 0;
	int32 WorstTileIndex = INDEX_NONE;
	float MinRatio = TNumericLimits<float>::Max();
	float MaxRatio = 0.0f;
	double RatioSum = 0.0;
	FString Csv;
	Csv += TEXT("TileIndex,TileX,TileY,PageIndex,TexelMinX,TexelMinY,TexelMaxX,TexelMaxY,RawTexels,ValidTexels,NodeOffset,NodeCount,RootNodeIndex,CompressionRatio,Resident\n");

	for (int32 TileIndex = 0; TileIndex < TileCount; ++TileIndex)
	{
		const FMHShadowTile& Tile = Asset.Tiles[TileIndex];
		EmptyTileCount += Tile.ValidTexelCount == 0 ? 1 : 0;
		MinRatio = FMath::Min(MinRatio, Tile.CompressionRatio);
		if (Tile.CompressionRatio > MaxRatio)
		{
			MaxRatio = Tile.CompressionRatio;
			WorstTileIndex = TileIndex;
		}
		RatioSum += Tile.CompressionRatio;

		Csv += FString::Printf(
			TEXT("%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.9f,%d\n"),
			TileIndex,
			Tile.TileCoord.X,
			Tile.TileCoord.Y,
			Tile.PageIndex,
			Tile.TexelRect.X,
			Tile.TexelRect.Y,
			Tile.TexelRect.Z,
			Tile.TexelRect.W,
			Tile.RawTexelCount,
			Tile.ValidTexelCount,
			Tile.NodeOffset,
			Tile.NodeCount,
			Tile.RootNodeIndex,
			Tile.CompressionRatio,
			Tile.bResidentDefault ? 1 : 0);
	}

	const float AvgRatio = TileCount > 0 ? static_cast<float>(RatioSum / static_cast<double>(TileCount)) : 1.0f;
	Csv += TEXT("\nMetric,Value\n");
	Csv += FString::Printf(TEXT("TileSize,%d\n"), Asset.TileSize);
	Csv += FString::Printf(TEXT("TileCountX,%d\n"), Asset.TileCount.X);
	Csv += FString::Printf(TEXT("TileCountY,%d\n"), Asset.TileCount.Y);
	Csv += FString::Printf(TEXT("EmptyTileCount,%d\n"), EmptyTileCount);
	Csv += FString::Printf(TEXT("MinTileRatio,%.9f\n"), TileCount > 0 ? MinRatio : 1.0f);
	Csv += FString::Printf(TEXT("MaxTileRatio,%.9f\n"), MaxRatio);
	Csv += FString::Printf(TEXT("AvgTileRatio,%.9f\n"), AvgRatio);
	Csv += FString::Printf(TEXT("WorstTileIndex,%d\n"), WorstTileIndex);
	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(StatsDir, SafeName + TEXT("_TileStats.csv")));

	const int32 TexelCount = Asset.Resolution.X * Asset.Resolution.Y;
	TArray<FColor> PhysicalAtlasPreview;
	TArray<FColor> PageTablePreview;
	PhysicalAtlasPreview.SetNumUninitialized(TexelCount);
	PageTablePreview.SetNumUninitialized(TexelCount);

	for (int32 Y = 0; Y < Asset.Resolution.Y; ++Y)
	{
		for (int32 X = 0; X < Asset.Resolution.X; ++X)
		{
			const int32 TileX = X / Asset.TileSize;
			const int32 TileY = Y / Asset.TileSize;
			const int32 TileIndex = TileY * Asset.TileCount.X + TileX;
			const int32 PixelIndex = Y * Asset.Resolution.X + X;
			float Depth = 1.0f;
			if (Asset.Tiles.IsValidIndex(TileIndex))
			{
				const FMHShadowTile& Tile = Asset.Tiles[TileIndex];
				SampleRepresentativeDepthFromTile(Asset.Nodes, Tile, X - Tile.TexelRect.X, Y - Tile.TexelRect.Y, Asset.TileSize, Depth);
				const uint8 R = static_cast<uint8>((Tile.TileCoord.X * 53) & 255);
				const uint8 G = static_cast<uint8>((Tile.TileCoord.Y * 97) & 255);
				const uint8 B = static_cast<uint8>((Tile.PageIndex * 29) & 255);
				PageTablePreview[PixelIndex] = Tile.bResidentDefault ? FColor(R, G, B, 255) : FColor::Red;
			}
			else
			{
				PageTablePreview[PixelIndex] = FColor::Red;
			}

			const uint8 Gray = static_cast<uint8>(FMath::Clamp(Depth * 255.0f, 0.0f, 255.0f));
			PhysicalAtlasPreview[PixelIndex] = FColor(Gray, Gray, Gray, 255);
		}
	}

	SavePng(FPaths::Combine(StatsDir, SafeName + TEXT("_PhysicalAtlasPreview.png")), Asset.Resolution.X, Asset.Resolution.Y, PhysicalAtlasPreview);
	SavePng(FPaths::Combine(StatsDir, SafeName + TEXT("_PageTablePreview.png")), Asset.Resolution.X, Asset.Resolution.Y, PageTablePreview);
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
}

UMHShadowImportLightmassDualCommandlet::UMHShadowImportLightmassDualCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowImportLightmassDualCommandlet::Main(const FString& Params)
{
	FString FilePath;
	if (!ParseStringParam(Params, TEXT("File="), FilePath) && !FindLatestLightmassDualFile(FilePath))
	{
		UE_LOG(LogTemp, Error, TEXT("No MH dual shadow sidecar file found. Pass File=... or build lighting with bGenerateMHDualShadowMap=True."));
		return 1;
	}

	FString OutputPath = TEXT("/Game/MHShadow/Baked/MHShadowData_LightmassDual");
	ParseStringParam(Params, TEXT("Output="), OutputPath);
	const FString OutputObjectPath = ToObjectPath(OutputPath);

	FGuid LightGuid;
	FMHDualShadowMapFileData FileData;
	TArray<FMHDualShadowMapFileSample> FileSamples;
	TArray<FMHDualShadowMapFileDebugRay> DebugRays;
	TArray<FMHDualShadowMapFileDebugHit> DebugHits;
	if (!LoadLightmassDualFile(FilePath, LightGuid, FileData, FileSamples, DebugRays, DebugHits))
	{
		return 1;
	}

	const int32 ExpectedTexelCount = FileData.ShadowMapSizeX * FileData.ShadowMapSizeY;
	if (FileData.ShadowMapSizeX <= 0 || FileData.ShadowMapSizeY <= 0 || FileSamples.Num() != ExpectedTexelCount)
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid MH dual shadow sidecar dimensions: %dx%d samples=%d"),
			FileData.ShadowMapSizeX,
			FileData.ShadowMapSizeY,
			FileSamples.Num());
		return 1;
	}

	const int32 ImportTileSize = FMath::Max(1, FMath::RoundToInt(ParseFloatParam(Params, TEXT("TileSize="), 128.0f)));
	if (!FMath::IsPowerOfTwo(ImportTileSize))
	{
		UE_LOG(LogTemp, Error, TEXT("LightmassDual tiled MH import requires power-of-two TileSize, got %d."), ImportTileSize);
		return 1;
	}
	if ((FileData.ShadowMapSizeX % ImportTileSize) != 0 || (FileData.ShadowMapSizeY % ImportTileSize) != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("LightmassDual tiled MH import requires resolution %dx%d to be divisible by TileSize=%d."),
			FileData.ShadowMapSizeX,
			FileData.ShadowMapSizeY,
			ImportTileSize);
		return 1;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(OutputObjectPath);
	const FString AssetName = GetAssetStemFromObjectPath(OutputObjectPath);
	UPackage* Package = CreatePackage(*PackageName);
	if (UObject* ExistingAsset = StaticFindObject(nullptr, Package, *AssetName))
	{
		ExistingAsset->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
	}
	UMHShadowDataAsset* Asset = NewObject<UMHShadowDataAsset>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Asset->BakeSource = EMHShadowBakeSource::LightmassDual;
	Asset->ProjectionMapping = EMHShadowProjectionMapping::LightmassWorldToShadowMatrix;
	Asset->Resolution = FIntPoint(FileData.ShadowMapSizeX, FileData.ShadowMapSizeY);
	Asset->TileSize = ImportTileSize;
	Asset->TileCount = FIntPoint(FileData.ShadowMapSizeX / ImportTileSize, FileData.ShadowMapSizeY / ImportTileSize);
	Asset->DepthBias = ParseFloatParam(Params, TEXT("DepthBias="), 0.001f);
	Asset->LightOrigin = FVector::ZeroVector;
	Asset->LightXAxis = FVector::ForwardVector;
	Asset->LightYAxis = FVector::RightVector;
	Asset->LightZAxis = FVector::UpVector;
	Asset->LightSpaceMin = FVector2D::ZeroVector;
	Asset->LightSpaceMax = FVector2D(1.0, 1.0);
	Asset->MinLightDepth = 0.0f;
	Asset->MaxLightDepth = 1.0f;
	Asset->WorldToShadowRow0 = FVector4(FileData.WorldToLight.M[0][0], FileData.WorldToLight.M[0][1], FileData.WorldToLight.M[0][2], FileData.WorldToLight.M[0][3]);
	Asset->WorldToShadowRow1 = FVector4(FileData.WorldToLight.M[1][0], FileData.WorldToLight.M[1][1], FileData.WorldToLight.M[1][2], FileData.WorldToLight.M[1][3]);
	Asset->WorldToShadowRow2 = FVector4(FileData.WorldToLight.M[2][0], FileData.WorldToLight.M[2][1], FileData.WorldToLight.M[2][2], FileData.WorldToLight.M[2][3]);
	Asset->WorldToShadowRow3 = FVector4(FileData.WorldToLight.M[3][0], FileData.WorldToLight.M[3][1], FileData.WorldToLight.M[3][2], FileData.WorldToLight.M[3][3]);

	Asset->RawIntervals.Empty(ExpectedTexelCount);
	Asset->RawIntervals.AddDefaulted(ExpectedTexelCount);
	Asset->RawIntervalFlags.Empty(ExpectedTexelCount);
	Asset->RawIntervalFlags.AddZeroed(ExpectedTexelCount);
	Asset->DebugIntervalPreview.Empty(ExpectedTexelCount);
	Asset->DebugIntervalPreview.AddDefaulted(ExpectedTexelCount);

	int32 ValidIntervalCount = 0;
	int32 PairedIntervalCount = 0;
	for (int32 Index = 0; Index < ExpectedTexelCount; ++Index)
	{
		const FMHDualShadowMapFileSample& FileSample = FileSamples[Index];
		const bool bValid = (FileSample.Flags & MHDSF_Valid) != 0;
		FMHShadowDepthInterval& Interval = Asset->RawIntervals[Index];
		Interval.MinDepth = FileSample.FrontDepth.GetFloat();
		Interval.MaxDepth = FileSample.BackDepth.GetFloat();
		Interval.bValid = bValid;
		Asset->RawIntervalFlags[Index] = FileSample.Flags;
		Asset->DebugIntervalPreview[Index] = bValid ? FlagColor(FileSample.Flags) : FColor::White;
		if (bValid)
		{
			++ValidIntervalCount;
			if ((FileSample.Flags & (MHDSF_ThinFallback | MHDSF_Unpaired)) == 0)
			{
				++PairedIntervalCount;
			}
		}
	}
	FileData.PairedTexelCount = PairedIntervalCount;

	Asset->Intervals.Reset();
	Asset->Nodes.Reset();
	Asset->Tiles.Reset();
	Asset->PageTable.Reset();
	Asset->Tiles.Reserve(Asset->TileCount.X * Asset->TileCount.Y);
	Asset->PageTable.Reserve(Asset->TileCount.X * Asset->TileCount.Y);

	double TotalCompressionSeconds = 0.0;
	int64 TotalCompressedBytes = 0;
	bool bTileCompressionSucceeded = true;
	for (int32 TileY = 0; TileY < Asset->TileCount.Y && bTileCompressionSucceeded; ++TileY)
	{
		for (int32 TileX = 0; TileX < Asset->TileCount.X && bTileCompressionSucceeded; ++TileX)
		{
			FMHShadowCompressionInput TileInput;
			TileInput.Resolution = FIntPoint(Asset->TileSize, Asset->TileSize);
			TileInput.TexelIntervals.SetNumUninitialized(Asset->TileSize * Asset->TileSize);

			int32 TileValidTexels = 0;
			for (int32 LocalY = 0; LocalY < Asset->TileSize; ++LocalY)
			{
				const int32 SourceY = TileY * Asset->TileSize + LocalY;
				for (int32 LocalX = 0; LocalX < Asset->TileSize; ++LocalX)
				{
					const int32 SourceX = TileX * Asset->TileSize + LocalX;
					const int32 SourceIndex = SourceY * Asset->Resolution.X + SourceX;
					const int32 LocalIndex = LocalY * Asset->TileSize + LocalX;
					TileInput.TexelIntervals[LocalIndex] = Asset->RawIntervals[SourceIndex];
					TileValidTexels += Asset->RawIntervals[SourceIndex].bValid ? 1 : 0;
				}
			}

			FMHShadowCompressionOutput TileOutput;
			FString CompressionError;
			if (!FMHShadowCompressor::Compress(TileInput, TileOutput, &CompressionError))
			{
				UE_LOG(LogTemp, Error, TEXT("LightmassDual tile compression failed at tile=(%d,%d): %s"), TileX, TileY, *CompressionError);
				bTileCompressionSucceeded = false;
				break;
			}

			const int32 NodeOffset = Asset->Nodes.Num();
			const int32 IntervalOffset = Asset->Intervals.Num();
			for (FMHShadowNode Node : TileOutput.Nodes)
			{
				for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
				{
					const int32 LocalChildIndex = GetChildIndex(Node.ChildIndices, ChildSlot);
					SetChildIndex(Node.ChildIndices, ChildSlot, LocalChildIndex >= 0 ? LocalChildIndex + NodeOffset : INDEX_NONE);
				}
				if (Node.IntervalIndex >= 0)
				{
					Node.IntervalIndex += IntervalOffset;
				}
				Asset->Nodes.Add(Node);
			}
			Asset->Intervals.Append(TileOutput.Intervals);

			const int32 TileIndex = TileY * Asset->TileCount.X + TileX;
			FMHShadowTile Tile;
			Tile.TileCoord = FIntPoint(TileX, TileY);
			Tile.TexelRect = FIntVector4(
				TileX * Asset->TileSize,
				TileY * Asset->TileSize,
				(TileX + 1) * Asset->TileSize,
				(TileY + 1) * Asset->TileSize);
			Tile.NodeOffset = NodeOffset;
			Tile.NodeCount = TileOutput.Nodes.Num();
			Tile.RootNodeIndex = NodeOffset;
			Tile.PageIndex = TileIndex;
			Tile.bResidentDefault = true;
			Tile.RawTexelCount = Asset->TileSize * Asset->TileSize;
			Tile.ValidTexelCount = TileValidTexels;
			Tile.CompressedNodeCount = TileOutput.Nodes.Num();
			Tile.CompressionRatio = TileOutput.Stats.CompressionRatio;
			Asset->Tiles.Add(Tile);
			Asset->PageTable.Add(Tile.PageIndex);

			TotalCompressionSeconds += TileOutput.Stats.BakeSeconds;
			TotalCompressedBytes += GetCompressedNodeBytes(TileOutput.Nodes.Num());
		}
	}

	if (!bTileCompressionSucceeded)
	{
		return 1;
	}

	Asset->Stats.RawTexelCount = ExpectedTexelCount;
	Asset->Stats.ValidTexelCount = ValidIntervalCount;
	Asset->Stats.NodeCount = Asset->Nodes.Num();
	Asset->Stats.IntervalCount = Asset->Intervals.Num();
	Asset->Stats.RawBytes = static_cast<int64>(ExpectedTexelCount) * static_cast<int64>(sizeof(float) * 2);
	Asset->Stats.CompressedBytes =
		TotalCompressedBytes
		+ static_cast<int64>(Asset->Tiles.Num()) * static_cast<int64>(sizeof(FMHShadowTile))
		+ static_cast<int64>(Asset->PageTable.Num()) * static_cast<int64>(sizeof(int32));
	Asset->Stats.CompressionRatio = Asset->Stats.RawBytes > 0
		? static_cast<float>(static_cast<double>(Asset->Stats.CompressedBytes) / static_cast<double>(Asset->Stats.RawBytes))
		: 1.0f;
	Asset->Stats.BakeSeconds = TotalCompressionSeconds;
	Asset->Stats.BakeSeconds = FileData.BakeSeconds;

	const int32 RequestedClipmapLevels = FMath::Clamp(FMath::RoundToInt(ParseFloatParam(Params, TEXT("ClipmapLevels="), 1.0f)), 0, 8);
	const bool bDeriveClipmapLevels = FMath::RoundToInt(ParseFloatParam(Params, TEXT("ClipmapDerived="), 1.0f)) != 0;
	Asset->ClipmapLevels.Reset();
	Asset->ClipmapRawIntervals.Reset();
	Asset->ClipmapNodes.Reset();
	Asset->ClipmapTiles.Reset();
	Asset->ClipmapPageTable.Reset();
	if (RequestedClipmapLevels > 0)
	{
		TArray<FMHShadowDepthInterval> LevelIntervals = Asset->RawIntervals;
		FIntPoint LevelResolution = Asset->Resolution;
		double ClipmapCompressionSeconds = 0.0;
		int64 ClipmapCompressedBytes = 0;
		for (int32 LevelIndex = 0; LevelIndex < RequestedClipmapLevels; ++LevelIndex)
		{
			if (LevelIndex > 0)
			{
				if (!bDeriveClipmapLevels || LevelResolution.X < ImportTileSize * 2 || LevelResolution.Y < ImportTileSize * 2)
				{
					break;
				}

				FIntPoint DownsampledResolution;
				LevelIntervals = DownsampleIntervals2x(LevelIntervals, LevelResolution, DownsampledResolution);
				LevelResolution = DownsampledResolution;
			}

			const float TexelScale = static_cast<float>(1 << LevelIndex);
			if (!AppendCompressedClipmapLevel(
				*Asset,
				LevelIntervals,
				LevelResolution,
				LevelIndex,
				ImportTileSize,
				FVector2D(FileData.TexelWorldSizeX * TexelScale, FileData.TexelWorldSizeY * TexelScale),
				ClipmapCompressionSeconds,
				ClipmapCompressedBytes))
			{
				return 1;
			}
		}

		UE_LOG(LogTemp, Display, TEXT("Built MH clipmap data: requestedLevels=%d builtLevels=%d clipmapTiles=%d clipmapNodes=%d clipmapRawIntervals=%d derived=%d clipmapCompressSeconds=%.3f clipmapCompressedBytes=%lld"),
			RequestedClipmapLevels,
			Asset->ClipmapLevels.Num(),
			Asset->ClipmapTiles.Num(),
			Asset->ClipmapNodes.Num(),
			Asset->ClipmapRawIntervals.Num(),
			bDeriveClipmapLevels ? 1 : 0,
			ClipmapCompressionSeconds,
			ClipmapCompressedBytes);
	}

	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(Asset);
	if (!SaveShadowDataAsset(OutputObjectPath, Asset))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save LightmassDual MH shadow asset: %s"), *OutputObjectPath);
		return 1;
	}

	WriteLightmassDualDebugOutputs(OutputObjectPath, FileData, Asset->RawIntervals, Asset->RawIntervalFlags);
	WriteLightmassDualStatsCsv(OutputObjectPath, FileData, *Asset, LightGuid, FilePath);
	WriteLightmassDualDepthStatsCsv(OutputObjectPath, Asset->RawIntervals, Asset->RawIntervalFlags);
	WriteLightmassDualHitSequenceCsv(OutputObjectPath, DebugRays, DebugHits);
	BuildLightmassDualTileData(OutputObjectPath, *Asset);

	UE_LOG(LogTemp, Display, TEXT("Imported LightmassDual MH shadow asset: %s Source=%s Resolution=%dx%d TileSize=%d Tiles=%dx%d Valid=%d Nodes=%d Ratio=%.6f ClipmapLevels=%d ClipmapTiles=%d ClipmapNodes=%d Flags Paired=%d Thin=%d Unpaired=%d MultiHit=%d DebugRays=%d DebugHits=%d"),
		*OutputObjectPath,
		*FilePath,
		Asset->Resolution.X,
		Asset->Resolution.Y,
		Asset->TileSize,
		Asset->TileCount.X,
		Asset->TileCount.Y,
		Asset->Stats.ValidTexelCount,
		Asset->Stats.NodeCount,
		Asset->Stats.CompressionRatio,
		Asset->ClipmapLevels.Num(),
		Asset->ClipmapTiles.Num(),
		Asset->ClipmapNodes.Num(),
		FileData.PairedTexelCount,
		FileData.ThinFallbackTexelCount,
		FileData.UnpairedTexelCount,
		FileData.MultiHitTexelCount,
		DebugRays.Num(),
		DebugHits.Num());
	return 0;
}

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
	if (Magic != 0x5344484D || Version != 3)
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid MH dual shadow sidecar header: %s Magic=0x%08x Version=%d. Rebuild lighting to regenerate the v3 sidecar."), *File, Magic, Version);
		return false;
	}

	Reader->Serialize(&OutData, sizeof(OutData));
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
		TEXT("Source,LightGuid,SourceFile,Resolution,RawTexels,ValidTexels,EmptyTexels,ThinFallbackTexels,UnpairedTexels,MultiHitTexels,AverageThickness,MaxThickness,Nodes,Intervals,RawBytes,CompressedBytes,CompressionRatio,BakeSeconds\n")
		+ FString::Printf(
			TEXT("LightmassDual,%s,%s,%dx%d,%d,%d,%d,%d,%d,%d,%.8f,%.8f,%d,%d,%lld,%lld,%.6f,%.3f\n"),
			*LightGuid.ToString(EGuidFormats::DigitsWithHyphens),
			*SourceFile,
			Data.ShadowMapSizeX,
			Data.ShadowMapSizeY,
			Asset.Stats.RawTexelCount,
			Asset.Stats.ValidTexelCount,
			Data.EmptyTexelCount,
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
	Asset->TileSize = 128;
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
		}
	}

	FMHShadowCompressionInput CompressionInput;
	CompressionInput.Resolution = Asset->Resolution;
	CompressionInput.TexelIntervals = Asset->RawIntervals;

	FMHShadowCompressionOutput CompressionOutput;
	FString CompressionError;
	if (FMHShadowCompressor::Compress(CompressionInput, CompressionOutput, &CompressionError))
	{
		Asset->Intervals = MoveTemp(CompressionOutput.Intervals);
		Asset->Nodes = MoveTemp(CompressionOutput.Nodes);
		Asset->Stats = CompressionOutput.Stats;
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("LightmassDual asset kept raw-only because MH compression was skipped: %s"), *CompressionError);
		Asset->Intervals.Empty();
		Asset->Nodes.Empty();
		Asset->Stats.RawTexelCount = ExpectedTexelCount;
		Asset->Stats.ValidTexelCount = ValidIntervalCount;
		Asset->Stats.NodeCount = 0;
		Asset->Stats.IntervalCount = 0;
		Asset->Stats.RawBytes = static_cast<int64>(ExpectedTexelCount) * static_cast<int64>(sizeof(float) * 2 + sizeof(uint8));
		Asset->Stats.CompressedBytes = 0;
		Asset->Stats.CompressionRatio = 0.0f;
	}
	Asset->Stats.BakeSeconds = FileData.BakeSeconds;

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

	UE_LOG(LogTemp, Display, TEXT("Imported LightmassDual MH shadow asset: %s Source=%s Resolution=%dx%d Valid=%d Nodes=%d Flags Thin=%d Unpaired=%d MultiHit=%d DebugRays=%d DebugHits=%d"),
		*OutputObjectPath,
		*FilePath,
		Asset->Resolution.X,
		Asset->Resolution.Y,
		Asset->Stats.ValidTexelCount,
		Asset->Stats.NodeCount,
		FileData.ThinFallbackTexelCount,
		FileData.UnpairedTexelCount,
		FileData.MultiHitTexelCount,
		DebugRays.Num(),
		DebugHits.Num());
	return 0;
}

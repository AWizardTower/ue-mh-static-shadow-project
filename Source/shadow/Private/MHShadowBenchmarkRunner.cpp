// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowBenchmarkRunner.h"

#include "Camera/CameraActor.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "MHShadowComponent.h"
#include "MHShadowCellComponent.h"
#include "MHShadowCellDataAsset.h"
#include "MHShadowDataAsset.h"
#include "MHShadowWorldComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHShadowBenchmark, Log, All);

namespace
{
	constexpr double DiffThreshold = 0.05;
	const TCHAR* const GRealClipmapTunedRestoredDepthBiasAdd = TEXT("0.01");

	FString CsvEscape(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\""), TEXT("\"\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	float Luminance01(const FColor& Color)
	{
		return (0.2126f * float(Color.R) + 0.7152f * float(Color.G) + 0.0722f * float(Color.B)) / 255.0f;
	}

	TArray<FString> ParseCsvFields(const FString& Line)
	{
		TArray<FString> Fields;
		FString Current;
		bool bInQuotes = false;
		for (int32 Index = 0; Index < Line.Len(); ++Index)
		{
			const TCHAR Ch = Line[Index];
			if (Ch == TCHAR('"'))
			{
				if (bInQuotes && Index + 1 < Line.Len() && Line[Index + 1] == TCHAR('"'))
				{
					Current.AppendChar(TCHAR('"'));
					++Index;
				}
				else
				{
					bInQuotes = !bInQuotes;
				}
			}
			else if (Ch == TCHAR(',') && !bInQuotes)
			{
				Fields.Add(Current);
				Current.Reset();
			}
			else
			{
				Current.AppendChar(Ch);
			}
		}
		Fields.Add(Current);
		return Fields;
	}

	bool FindNewestRuntimeStatsFile(const FString& Prefix, FString& OutPath)
	{
		const FString RuntimeStatsDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("RuntimeStats"));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(RuntimeStatsDir, Prefix + TEXT("*.csv")), true, false);
		FDateTime NewestTime = FDateTime::MinValue();
		FString NewestPath;
		for (const FString& File : Files)
		{
			const FString FullPath = FPaths::IsRelative(File) ? FPaths::Combine(RuntimeStatsDir, File) : File;
			const FDateTime Timestamp = IFileManager::Get().GetTimeStamp(*FullPath);
			if (Timestamp > NewestTime)
			{
				NewestTime = Timestamp;
				NewestPath = FullPath;
			}
		}
		if (NewestPath.IsEmpty())
		{
			return false;
		}
		OutPath = NewestPath;
		return true;
	}

	void DestroyStaleBenchmarkCameras(UWorld* World)
	{
		if (!World)
		{
			return;
		}

		for (TActorIterator<ACameraActor> It(World); It; ++It)
		{
			ACameraActor* CameraActor = *It;
			if (CameraActor && CameraActor->GetName().StartsWith(TEXT("MHShadowBenchmarkCamera")) && !CameraActor->IsActorBeingDestroyed())
			{
				CameraActor->Destroy();
			}
		}
	}

	int32 ToInt(const TArray<FString>& Fields, int32 Index)
	{
		return Fields.IsValidIndex(Index) ? FCString::Atoi(*Fields[Index]) : 0;
	}

	uint64 ToUInt64(const TArray<FString>& Fields, int32 Index)
	{
		return Fields.IsValidIndex(Index) ? FCString::Strtoui64(*Fields[Index], nullptr, 10) : 0;
	}

	double ToDouble(const TArray<FString>& Fields, int32 Index)
	{
		return Fields.IsValidIndex(Index) ? FCString::Atod(*Fields[Index]) : 0.0;
	}

	FString FindCommandValue(const TArray<FString>& Commands, const TCHAR* CVarName, const TCHAR* DefaultValue = TEXT("-"))
	{
		const FString Prefix = FString(CVarName) + TEXT(" ");
		FString LastValue;
		for (const FString& Command : Commands)
		{
			if (Command.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				FString Value = Command.RightChop(Prefix.Len());
				Value.TrimStartAndEndInline();
				LastValue = Value;
			}
		}
		return LastValue.IsEmpty() ? FString(DefaultValue) : LastValue;
	}

	FString JoinCommandsForCsv(const TArray<FString>& Commands)
	{
		FString Joined;
		for (int32 Index = 0; Index < Commands.Num(); ++Index)
		{
			if (Index > 0)
			{
				Joined += TEXT("; ");
			}
			Joined += Commands[Index];
		}
		return Joined;
	}

	void UpsertCommand(TArray<FString>& Commands, const TCHAR* CVarName, const TCHAR* Value)
	{
		const FString Prefix = FString(CVarName) + TEXT(" ");
		const FString NewCommand = Prefix + Value;
		for (FString& Command : Commands)
		{
			if (Command.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				Command = NewCommand;
				return;
			}
		}
		Commands.Add(NewCommand);
	}

	int32 FindCommandInt(const TArray<FString>& Commands, const TCHAR* CVarName, int32 DefaultValue)
	{
		const FString Value = FindCommandValue(Commands, CVarName, TEXT(""));
		return Value.IsEmpty() ? DefaultValue : FCString::Atoi(*Value);
	}
}

FMHShadowBenchmarkRunner::FMHShadowBenchmarkRunner() = default;

FMHShadowBenchmarkRunner::~FMHShadowBenchmarkRunner()
{
	StopBenchmark();
}

void FMHShadowBenchmarkRunner::StartBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::BakeTest);
}

void FMHShadowBenchmarkRunner::StartLargeCacheStressBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::LargeCacheStress);
}

void FMHShadowBenchmarkRunner::StartClipmapRegressionBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::ClipmapRegression);
}

void FMHShadowBenchmarkRunner::StartClipmapDegenerationBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::ClipmapDegeneration);
}

void FMHShadowBenchmarkRunner::StartCellProviderRegressionBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::CellProviderRegression);
}

void FMHShadowBenchmarkRunner::StartRealClipmapRegressionBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::RealClipmapRegression);
}

void FMHShadowBenchmarkRunner::StartCacheStrategyRegressionBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::CacheStrategyRegression);
}

void FMHShadowBenchmarkRunner::StartQualityRegressionBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::QualityRegression);
}

void FMHShadowBenchmarkRunner::StartSoftShadowRegressionBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::SoftShadowRegression);
}

void FMHShadowBenchmarkRunner::StartLightingIntegrationRegressionBenchmark()
{
	StartBenchmarkInternal(EBenchmarkProfile::LightingIntegrationRegression);
}

void FMHShadowBenchmarkRunner::StartBenchmarkInternal(EBenchmarkProfile Profile)
{
	if (bRunning)
	{
		UE_LOG(LogMHShadowBenchmark, Warning, TEXT("Benchmark is already running."));
		return;
	}

	UWorld* World = GetBenchmarkWorld();
	if (!World || !GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		UE_LOG(LogMHShadowBenchmark, Error, TEXT("MH benchmark needs a running PIE/Standalone game viewport. Start Play or Standalone, then run MHShadow.Benchmark.Run."));
		return;
	}

	BuildBenchmarkPlan(Profile);
	NormalizeHardShadowCaptureCommands();

	if ((Profile == EBenchmarkProfile::LargeCacheStress || Profile == EBenchmarkProfile::ClipmapRegression || Profile == EBenchmarkProfile::ClipmapDegeneration || Profile == EBenchmarkProfile::RealClipmapRegression || Profile == EBenchmarkProfile::CacheStrategyRegression || Profile == EBenchmarkProfile::QualityRegression || Profile == EBenchmarkProfile::SoftShadowRegression || Profile == EBenchmarkProfile::LightingIntegrationRegression) && !EnsureLargeCacheStressData(*World))
	{
		return;
	}

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("Benchmark"), FString::Printf(TEXT("%s_%s"), *ActiveProfileName, *Timestamp));
	IFileManager::Get().MakeDirectory(*OutputDir, true);

	CaptureRows.Reset();
	DiffRows.Reset();
	StabilityRows.Reset();
	CaptureStatsRows.Reset();
	HardShadowSanityRows.Reset();
	AtlasBaselineByCamera.Reset();
	ClipmapFirstByCamera.Reset();
	CaptureLumaByKey.Reset();
	AtlasSizeByCamera.Reset();
	ClipmapSizeByCamera.Reset();
	CaptureSizeByKey.Reset();
	RouteStartFrameByCapture.Reset();
	ProviderRouteStatsByCapture.Reset();

	BenchmarkStartTime = FPlatformTime::Seconds();
	CurrentStepIndex = INDEX_NONE;
	RemainingSettleFrames = 0;
	bRunning = true;

	Exec(World, TEXT("r.Shadow.MHStatic.Stats.Reset 1"));
	Exec(World, TEXT("r.Shadow.MHStatic.Stats.Enable 1"));
	Exec(World, TEXT("r.Shadow.MHStatic.Stats.CSV 1"));
	Exec(World, (Profile == EBenchmarkProfile::ClipmapRegression || Profile == EBenchmarkProfile::ClipmapDegeneration || Profile == EBenchmarkProfile::CellProviderRegression || Profile == EBenchmarkProfile::RealClipmapRegression || Profile == EBenchmarkProfile::CacheStrategyRegression || Profile == EBenchmarkProfile::QualityRegression || Profile == EBenchmarkProfile::SoftShadowRegression || Profile == EBenchmarkProfile::LightingIntegrationRegression) ? TEXT("r.Shadow.MHStatic.Stats.CSVEveryNFrames 1") : (Profile == EBenchmarkProfile::LargeCacheStress ? TEXT("r.Shadow.MHStatic.Stats.CSVEveryNFrames 2") : TEXT("r.Shadow.MHStatic.Stats.CSVEveryNFrames 10")));
	Exec(World, TEXT("r.Shadow.MHStatic.Feedback.Enable 1"));
	Exec(World, TEXT("r.Shadow.MHStatic.Cache.Enable 1"));
	Exec(World, TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"));
	Exec(World, TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"));
	Exec(World, Profile == EBenchmarkProfile::LargeCacheStress ? TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 8") : TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 64"));
	Exec(World, Profile == EBenchmarkProfile::BakeTest || Profile == EBenchmarkProfile::RealClipmapRegression || Profile == EBenchmarkProfile::CacheStrategyRegression || Profile == EBenchmarkProfile::QualityRegression || Profile == EBenchmarkProfile::SoftShadowRegression || Profile == EBenchmarkProfile::LightingIntegrationRegression ? TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3") : TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4"));
	Exec(World, TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"));
	Exec(World, TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"));
	Exec(World, TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"));
	Exec(World, TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3"));
	Exec(World, TEXT("r.Shadow.MHStatic.Clipmap.SingleLevelUseBaseData 1"));

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FMHShadowBenchmarkRunner::Tick));
	UE_LOG(LogMHShadowBenchmark, Display, TEXT("MH benchmark started. Output: %s"), *OutputDir);
}

void FMHShadowBenchmarkRunner::CaptureCurrent()
{
	UWorld* World = GetBenchmarkWorld();
	if (!World || !GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		UE_LOG(LogMHShadowBenchmark, Error, TEXT("No game viewport available for MHShadow.Benchmark.CaptureCurrent."));
		return;
	}

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString ManualDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("Benchmark"), TEXT("Manual"), Timestamp);
	IFileManager::Get().MakeDirectory(*ManualDir, true);

	FIntPoint Size = FIntPoint::ZeroValue;
	TArray<float> Luma;
	const FString Filename = FPaths::Combine(ManualDir, TEXT("CurrentViewport.png"));
	if (CaptureViewport(Filename, Size, Luma))
	{
		Exec(World, TEXT("r.Shadow.MHStatic.Dump"));
		Exec(World, TEXT("r.Shadow.MHStatic.Stats.Dump"));
		UE_LOG(LogMHShadowBenchmark, Display, TEXT("Captured current viewport: %s (%dx%d)"), *Filename, Size.X, Size.Y);
	}
}

void FMHShadowBenchmarkRunner::StopBenchmark()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	if (bRunning)
	{
		bRunning = false;
		UE_LOG(LogMHShadowBenchmark, Warning, TEXT("MH benchmark stopped before completion."));
	}

	DestroyBenchmarkCamera();
}

bool FMHShadowBenchmarkRunner::Tick(float DeltaTime)
{
	if (!bRunning)
	{
		return false;
	}

	if (RemainingSettleFrames > 0)
	{
		--RemainingSettleFrames;
		return true;
	}

	if (CurrentStepIndex >= 0 && Steps.IsValidIndex(CurrentStepIndex))
	{
		const FBenchmarkStep& Step = Steps[CurrentStepIndex];
		const FCameraSpec& Camera = Cameras[Step.CameraIndex];
		const FCaptureSpec& Capture = Captures[Step.CaptureIndex];
		const FString Filename = FPaths::Combine(OutputDir, FString::Printf(TEXT("%02d_%s_%s.png"), CurrentStepIndex + 1, *MakeSafeName(Camera.Name), *MakeSafeName(Capture.Name)));

		FIntPoint Size = FIntPoint::ZeroValue;
		TArray<float> Luma;
		if (CaptureViewport(Filename, Size, Luma))
		{
			RecordCapture(Step, Filename, Size, Luma);
		}
		else
		{
			UE_LOG(LogMHShadowBenchmark, Error, TEXT("Failed to capture benchmark step %d (%s / %s)."), CurrentStepIndex + 1, *Camera.Name, *Capture.Name);
		}
	}

	StartNextStep();
	return true;
}

void FMHShadowBenchmarkRunner::BuildBenchmarkPlan(EBenchmarkProfile Profile)
{
	if (Profile == EBenchmarkProfile::ClipmapRegression)
	{
		BuildClipmapRegressionPlan();
	}
	else if (Profile == EBenchmarkProfile::ClipmapDegeneration)
	{
		BuildClipmapDegenerationPlan();
	}
	else if (Profile == EBenchmarkProfile::CellProviderRegression)
	{
		BuildCellProviderRegressionPlan();
	}
	else if (Profile == EBenchmarkProfile::RealClipmapRegression)
	{
		BuildRealClipmapRegressionPlan();
	}
	else if (Profile == EBenchmarkProfile::CacheStrategyRegression)
	{
		BuildCacheStrategyRegressionPlan();
	}
	else if (Profile == EBenchmarkProfile::QualityRegression)
	{
		BuildQualityRegressionPlan();
	}
	else if (Profile == EBenchmarkProfile::SoftShadowRegression)
	{
		BuildSoftShadowRegressionPlan();
	}
	else if (Profile == EBenchmarkProfile::LightingIntegrationRegression)
	{
		BuildLightingIntegrationRegressionPlan();
	}
	else if (Profile == EBenchmarkProfile::LargeCacheStress)
	{
		BuildLargeCacheStressPlan();
	}
	else
	{
		BuildBakeTestPlan();
	}
}

void FMHShadowBenchmarkRunner::NormalizeHardShadowCaptureCommands()
{
	if (ActiveProfileName == TEXT("QualityRegression") || ActiveProfileName == TEXT("SoftShadowRegression"))
	{
		return;
	}

	for (FCaptureSpec& Capture : Captures)
	{
		if (!IsHardShadowSanityCapture(Capture))
		{
			continue;
		}

		if (Capture.Name.Contains(TEXT("ZeroRestoredBias"))
			|| Capture.Name.Contains(TEXT("DefaultRestoredBias"))
			|| Capture.Name.Contains(TEXT("SuppressionCheck")))
		{
			continue;
		}

		UpsertCommand(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), TEXT("0"));
		UpsertCommand(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale"), TEXT("0"));
		UpsertCommand(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd"), GRealClipmapTunedRestoredDepthBiasAdd);
	}
}

bool FMHShadowBenchmarkRunner::IsHardShadowSanityCapture(const FCaptureSpec& Capture) const
{
	const int32 Debug = FindCommandInt(Capture.Commands, TEXT("r.Shadow.MHStatic.Debug"), 0);
	const int32 Source = FindCommandInt(Capture.Commands, TEXT("r.Shadow.MHStatic.Source"), -1);
	return Debug == 1 && (Source == 2 || Source == 4 || Source == 5 || Source == 6);
}

FString FMHShadowBenchmarkRunner::MakeHardShadowSanityStatus(const FCaptureSpec& Capture, double MinVisibility, double MaxVisibility, double NearWhitePercent, double NearBlackPercent, FString& OutDetails) const
{
	const double VisibilityRange = MaxVisibility - MinVisibility;
	const bool bNearlyFlat = VisibilityRange < 0.10;
	const bool bMostlyWhite = NearWhitePercent > 98.0;
	const bool bMostlyBlack = NearBlackPercent > 98.0;

	OutDetails = FString::Printf(TEXT("range=%.6f nearWhite=%.3f nearBlack=%.3f"), VisibilityRange, NearWhitePercent, NearBlackPercent);

	if (Capture.Name.Contains(TEXT("DefaultRestoredBias")) || Capture.Name.Contains(TEXT("SuppressionCheck")))
	{
		if (bMostlyWhite || bNearlyFlat)
		{
			return TEXT("BiasSuppressedShadow");
		}
		return TEXT("DefaultBiasStillHasShadow");
	}

	if (bMostlyWhite)
	{
		return TEXT("LikelyNoCastShadow");
	}

	if (bMostlyBlack)
	{
		return TEXT("LikelyFullyShadowed");
	}

	if (bNearlyFlat)
	{
		return TEXT("LowVisibilityRange");
	}

	return TEXT("OK");
}

void FMHShadowBenchmarkRunner::BuildBakeTestPlan()
{
	ActiveProfileName = TEXT("BakeTest");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("Overview"), FVector(-1100.0, -900.0, 820.0), FVector(0.0, 0.0, 120.0) });
	Cameras.Add({ TEXT("NearContact"), FVector(-360.0, -420.0, 260.0), FVector(20.0, -20.0, 90.0) });
	Cameras.Add({ TEXT("WallCorner"), FVector(-780.0, -620.0, 380.0), FVector(-120.0, -80.0, 130.0) });
	Cameras.Add({ TEXT("ClipmapTransition"), FVector(1150.0, -950.0, 520.0), FVector(0.0, 0.0, 120.0) });
	Cameras.Add({ TEXT("FarView"), FVector(-2050.0, 1300.0, 980.0), FVector(0.0, 0.0, 120.0) });

	Captures.Add({
		TEXT("UEOriginal"),
		{
			TEXT("r.Shadow.MHStatic.Enable 0"),
			TEXT("r.Shadow.MHStatic.Debug 0")
		},
		8,
		false,
		false,
		false,
		false,
		TEXT("UE renderer baseline with MH disabled")
	});

	Captures.Add({
		TEXT("AtlasBaseline"),
		{
			TEXT("r.Shadow.MHStatic.Enable 1"),
			TEXT("r.Shadow.MHStatic.Mode 3"),
			TEXT("r.Shadow.MHStatic.Source 2"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
			TEXT("r.Shadow.MHStatic.Restored.EnablePCF 0")
		},
		8,
		true,
		false,
		false,
		false,
		TEXT("Golden hard-shadow visibility from full restored atlas")
	});

	Captures.Add({
		TEXT("RawFront"),
		{
			TEXT("r.Shadow.MHStatic.Source 1"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0")
		},
		6,
		false,
		true,
		false,
		false,
		TEXT("Uncompressed front-depth baseline")
	});

	Captures.Add({
		TEXT("MHTree"),
		{
			TEXT("r.Shadow.MHStatic.Source 0"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0")
		},
		6,
		false,
		true,
		false,
		false,
		TEXT("Direct compressed MH representative-depth sampling")
	});

	Captures.Add({
		TEXT("VirtualPageAtlas"),
		{
			TEXT("r.Shadow.MHStatic.Source 4"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0")
		},
		6,
		false,
		true,
		false,
		false,
		TEXT("All-resident virtual page atlas")
	});

	Captures.Add({
		TEXT("LimitedCache"),
		{
			TEXT("r.Shadow.MHStatic.Source 5"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.Reset 1"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1")
		},
		22,
		false,
		true,
		false,
		false,
		TEXT("Limited physical page cache after feedback has time to fill pages")
	});

	Captures.Add({
		TEXT("Clipmap"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.Reset 1"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3")
		},
		22,
		false,
		true,
		false,
		false,
		TEXT("Virtual clipmap hard-shadow result")
	});

	Captures.Add({
		TEXT("ClipmapRepeat"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1")
		},
		8,
		false,
		false,
		true,
		false,
		TEXT("Repeat capture for same-camera temporal stability")
	});

	Captures.Add({
		TEXT("ClipmapLevelDebug"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 18")
		},
		6,
		false,
		false,
		false,
		false,
		TEXT("Clipmap level visualization")
	});

	Captures.Add({
		TEXT("ClipmapFallbackDebug"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 20")
		},
		6,
		false,
		false,
		false,
		false,
		TEXT("Clipmap miss/fallback visualization")
	});

	for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
	{
		for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::BuildLargeCacheStressPlan()
{
	ActiveProfileName = TEXT("LargeCacheStress");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("OverviewHigh"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });
	Cameras.Add({ TEXT("NorthWestLow"), FVector(-4300.0, -4300.0, 720.0), FVector(-2500.0, -2500.0, 160.0) });
	Cameras.Add({ TEXT("NorthSweep"), FVector(-2400.0, -5600.0, 680.0), FVector(900.0, -2600.0, 150.0) });
	Cameras.Add({ TEXT("CenterA"), FVector(-1700.0, -1800.0, 560.0), FVector(800.0, 900.0, 160.0) });
	Cameras.Add({ TEXT("CenterB"), FVector(1300.0, -2200.0, 540.0), FVector(-900.0, 1300.0, 160.0) });
	Cameras.Add({ TEXT("EastRun"), FVector(5600.0, -1300.0, 700.0), FVector(2600.0, 1300.0, 170.0) });
	Cameras.Add({ TEXT("FarCorner"), FVector(5600.0, 5600.0, 940.0), FVector(2500.0, 2500.0, 180.0) });
	Cameras.Add({ TEXT("SouthDiag"), FVector(2400.0, 5700.0, 720.0), FVector(-1000.0, 1500.0, 160.0) });
	Cameras.Add({ TEXT("WestRun"), FVector(-5600.0, 1200.0, 700.0), FVector(-2500.0, -900.0, 160.0) });
	Cameras.Add({ TEXT("ContactA"), FVector(-2500.0, 300.0, 320.0), FVector(-1800.0, 600.0, 130.0) });
	Cameras.Add({ TEXT("ContactB"), FVector(800.0, 1800.0, 360.0), FVector(1300.0, 2200.0, 130.0) });
	Cameras.Add({ TEXT("ReturnOverview"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });

	Captures.Add({
		TEXT("AtlasBaseline"),
		{
			TEXT("r.Shadow.MHStatic.Enable 1"),
			TEXT("r.Shadow.MHStatic.Mode 3"),
			TEXT("r.Shadow.MHStatic.Source 2"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
			TEXT("r.Shadow.MHStatic.Restored.EnablePCF 0")
		},
		10,
		true,
		false,
		false,
		false,
		TEXT("Full restored atlas hard-shadow baseline for stress scene")
	});

	Captures.Add({
		TEXT("VirtualPageAtlas"),
		{
			TEXT("r.Shadow.MHStatic.Source 4"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0")
		},
		8,
		false,
		true,
		false,
		false,
		TEXT("All-resident virtual page atlas sanity check")
	});

	Captures.Add({
		TEXT("LimitedCache_4x4"),
		{
			TEXT("r.Shadow.MHStatic.Source 5"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 4"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 4"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 8"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Limited page cache stress route with 16 physical pages")
	});

	Captures.Add({
		TEXT("LimitedCache_8x8"),
		{
			TEXT("r.Shadow.MHStatic.Source 5"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Limited page cache comparison route with 64 physical pages")
	});

	Captures.Add({
		TEXT("Clipmap_4x4"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 4"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 4"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 8"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Virtual clipmap stress route with 16 physical pages")
	});

	Captures.Add({
		TEXT("Clipmap"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Virtual clipmap comparison route with 64 physical pages")
	});

	Captures.Add({
		TEXT("ClipmapRepeat"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1")
		},
		8,
		false,
		false,
		true,
		false,
		TEXT("Repeat capture for Source=6 8x8 temporal stability")
	});

	Captures.Add({
		TEXT("ClipmapLevelDebug"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 18")
		},
		6,
		false,
		false,
		false,
		false,
		TEXT("Clipmap level visualization")
	});

	Captures.Add({
		TEXT("ClipmapFallbackDebug"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 20")
		},
		6,
		false,
		false,
		false,
		false,
		TEXT("Clipmap miss/fallback visualization")
	});

	for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
	{
		for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::BuildClipmapDegenerationPlan()
{
	ActiveProfileName = TEXT("ClipmapDegeneration");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("OverviewHigh"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });
	Cameras.Add({ TEXT("NorthWestLow"), FVector(-4300.0, -4300.0, 720.0), FVector(-2500.0, -2500.0, 160.0) });
	Cameras.Add({ TEXT("NorthSweep"), FVector(-2400.0, -5600.0, 680.0), FVector(900.0, -2600.0, 150.0) });
	Cameras.Add({ TEXT("CenterA"), FVector(-1700.0, -1800.0, 560.0), FVector(800.0, 900.0, 160.0) });
	Cameras.Add({ TEXT("CenterB"), FVector(1300.0, -2200.0, 540.0), FVector(-900.0, 1300.0, 160.0) });
	Cameras.Add({ TEXT("EastRun"), FVector(5600.0, -1300.0, 700.0), FVector(2600.0, 1300.0, 170.0) });
	Cameras.Add({ TEXT("FarCorner"), FVector(5600.0, 5600.0, 940.0), FVector(2500.0, 2500.0, 180.0) });
	Cameras.Add({ TEXT("SouthDiag"), FVector(2400.0, 5700.0, 720.0), FVector(-1000.0, 1500.0, 160.0) });
	Cameras.Add({ TEXT("WestRun"), FVector(-5600.0, 1200.0, 700.0), FVector(-2500.0, -900.0, 160.0) });
	Cameras.Add({ TEXT("ContactA"), FVector(-2500.0, 300.0, 320.0), FVector(-1800.0, 600.0, 130.0) });
	Cameras.Add({ TEXT("ContactB"), FVector(800.0, 1800.0, 360.0), FVector(1300.0, 2200.0, 130.0) });
	Cameras.Add({ TEXT("ReturnOverview"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });

	Captures.Add({
		TEXT("AtlasBaseline"),
		{
			TEXT("r.Shadow.MHStatic.Enable 1"),
			TEXT("r.Shadow.MHStatic.Mode 3"),
			TEXT("r.Shadow.MHStatic.Source 2"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
			TEXT("r.Shadow.MHStatic.Restored.EnablePCF 0")
		},
		10,
		true,
		false,
		false,
		false,
		TEXT("Full restored atlas hard-shadow baseline")
	});

	Captures.Add({
		TEXT("LimitedCache_8x8_LongSettle"),
		{
			TEXT("r.Shadow.MHStatic.Source 5"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1")
		},
		30,
		false,
		true,
		false,
		true,
		TEXT("Source=5 long-settle 8x8 cache baseline")
	});

	Captures.Add({
		TEXT("ClipmapSingleLevel_8x8_BaseCompat_LongSettle"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.SingleLevelUseBaseData 1")
		},
		30,
		false,
		true,
		false,
		true,
		TEXT("Source=6 single-level runtime path reusing base tile/node/page data")
	});

	Captures.Add({
		TEXT("ClipmapSingleLevel_8x8_Independent"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.SingleLevelUseBaseData 0")
		},
		30,
		false,
		true,
		false,
		true,
		TEXT("Old Source=6 single-level path using independently built clipmap level-0 data")
	});

	Captures.Add({
		TEXT("LimitedCache_8x8_Debug9"),
		{
			TEXT("r.Shadow.MHStatic.Source 5"),
			TEXT("r.Shadow.MHStatic.Debug 9"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1")
		},
		30,
		false,
		false,
		false,
		true,
		TEXT("Source=5 physical atlas depth debug")
	});

	Captures.Add({
		TEXT("ClipmapSingleLevel_8x8_BaseCompat_Debug9"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 9"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.SingleLevelUseBaseData 1")
		},
		30,
		false,
		false,
		false,
		true,
		TEXT("Source=6 single-level base-compatible physical atlas depth debug")
	});

	PairwiseSpecs.Add({ TEXT("ClipmapSingleLevel_8x8_BaseCompat_LongSettle"), TEXT("LimitedCache_8x8_LongSettle") });
	PairwiseSpecs.Add({ TEXT("ClipmapSingleLevel_8x8_Independent"), TEXT("LimitedCache_8x8_LongSettle") });
	PairwiseSpecs.Add({ TEXT("ClipmapSingleLevel_8x8_BaseCompat_Debug9"), TEXT("LimitedCache_8x8_Debug9") });

	for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
	{
		for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::BuildCellProviderRegressionPlan()
{
	ActiveProfileName = TEXT("CellProviderRegression");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("OverviewHigh"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });
	Cameras.Add({ TEXT("CenterA"), FVector(-1700.0, -1800.0, 560.0), FVector(800.0, 900.0, 160.0) });
	Cameras.Add({ TEXT("EastRun"), FVector(5600.0, -1300.0, 700.0), FVector(2600.0, 1300.0, 170.0) });
	Cameras.Add({ TEXT("FarCorner"), FVector(5600.0, 5600.0, 940.0), FVector(2500.0, 2500.0, 180.0) });
	Cameras.Add({ TEXT("ContactA"), FVector(-2500.0, 300.0, 320.0), FVector(-1800.0, 600.0, 130.0) });
	Cameras.Add({ TEXT("ReturnOverview"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });

	const TArray<FString> StableCaptureCommands = {
		TEXT("r.ScreenPercentage 100"),
		TEXT("r.PostProcessAAQuality 0"),
		TEXT("r.AntiAliasingMethod 0"),
		TEXT("r.TemporalAA.Upsampling 0"),
		TEXT("r.MotionBlurQuality 0"),
		TEXT("r.EyeAdaptationQuality 0")
	};

	TArray<FString> Source6AllResidentCommands = StableCaptureCommands;
	Source6AllResidentCommands.Append({
		TEXT("r.Shadow.MHStatic.Enable 1"),
		TEXT("r.Shadow.MHStatic.Mode 3"),
		TEXT("r.Shadow.MHStatic.Source 6"),
		TEXT("r.Shadow.MHStatic.Debug 1"),
		TEXT("r.Shadow.MHStatic.DepthTest 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
		TEXT("r.Shadow.MHStatic.Cache.Enable 0"),
		TEXT("r.Shadow.MHStatic.Feedback.Enable 0"),
		TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4"),
		TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
		TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
		TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
	});

	FCaptureSpec MonolithicAllResident;
	MonolithicAllResident.Name = TEXT("Monolithic_Source6_AllResident");
	MonolithicAllResident.Commands = Source6AllResidentCommands;
	MonolithicAllResident.SettleFrames = 12;
	MonolithicAllResident.bResetCacheAtRouteStart = true;
	MonolithicAllResident.Notes = TEXT("Monolithic Source=6 all-resident baseline for cell-provider equivalence.");
	MonolithicAllResident.ProviderMode = EShadowProviderMode::Monolithic;
	Captures.Add(MonolithicAllResident);

	FCaptureSpec CellAllResident;
	CellAllResident.Name = TEXT("CellProvider_AllLoaded_Source6");
	CellAllResident.Commands = Source6AllResidentCommands;
	CellAllResident.SettleFrames = 12;
	CellAllResident.bResetCacheAtRouteStart = true;
	CellAllResident.Notes = TEXT("Cell-provider Source=6 all-cells-loaded all-resident route. Should match monolithic baseline.");
	CellAllResident.ProviderMode = EShadowProviderMode::CellProvider;
	Captures.Add(CellAllResident);

	FCaptureSpec CellPartialDisabled;
	CellPartialDisabled.Name = TEXT("CellProvider_PartialDisabled_Source6");
	CellPartialDisabled.Commands = Source6AllResidentCommands;
	CellPartialDisabled.SettleFrames = 12;
	CellPartialDisabled.bResetCacheAtRouteStart = true;
	CellPartialDisabled.Notes = TEXT("Cell-provider partial-loaded route; every fourth cell is disabled to validate unavailable page accounting.");
	CellPartialDisabled.ProviderMode = EShadowProviderMode::CellProvider;
	CellPartialDisabled.CellDisableModulo = 4;
	CellPartialDisabled.CellDisableRemainder = 0;
	Captures.Add(CellPartialDisabled);

	FCaptureSpec MonolithicDebug9;
	MonolithicDebug9.Name = TEXT("Monolithic_Debug9");
	MonolithicDebug9.Commands = Source6AllResidentCommands;
	MonolithicDebug9.Commands.Add(TEXT("r.Shadow.MHStatic.Debug 9"));
	MonolithicDebug9.SettleFrames = 6;
	MonolithicDebug9.Notes = TEXT("Monolithic Source=6 physical atlas depth debug.");
	MonolithicDebug9.ProviderMode = EShadowProviderMode::Monolithic;
	Captures.Add(MonolithicDebug9);

	FCaptureSpec CellDebug9;
	CellDebug9.Name = TEXT("CellProvider_Debug9");
	CellDebug9.Commands = Source6AllResidentCommands;
	CellDebug9.Commands.Add(TEXT("r.Shadow.MHStatic.Debug 9"));
	CellDebug9.SettleFrames = 6;
	CellDebug9.Notes = TEXT("Cell-provider Source=6 physical atlas depth debug.");
	CellDebug9.ProviderMode = EShadowProviderMode::CellProvider;
	Captures.Add(CellDebug9);

	FCaptureSpec CellLimitedCache;
	CellLimitedCache.Name = TEXT("CellProvider_8x8Cache_Source6");
	CellLimitedCache.Commands = StableCaptureCommands;
	CellLimitedCache.Commands.Append({
		TEXT("r.Shadow.MHStatic.Enable 1"),
		TEXT("r.Shadow.MHStatic.Mode 3"),
		TEXT("r.Shadow.MHStatic.Source 6"),
		TEXT("r.Shadow.MHStatic.Debug 1"),
		TEXT("r.Shadow.MHStatic.DepthTest 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
		TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
		TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
		TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
		TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
		TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4"),
		TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
		TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
		TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
	});
	CellLimitedCache.SettleFrames = 12;
	CellLimitedCache.bResetCacheAtRouteStart = true;
	CellLimitedCache.Notes = TEXT("Cell-provider Source=6 limited 8x8 cache route for cache/runtime stats.");
	CellLimitedCache.ProviderMode = EShadowProviderMode::CellProvider;
	Captures.Add(CellLimitedCache);

	FCaptureSpec CellLevelDebug;
	CellLevelDebug.Name = TEXT("CellProvider_LevelDebug");
	CellLevelDebug.Commands = CellLimitedCache.Commands;
	CellLevelDebug.Commands.Add(TEXT("r.Shadow.MHStatic.Debug 18"));
	CellLevelDebug.SettleFrames = 6;
	CellLevelDebug.Notes = TEXT("Cell-provider clipmap level visualization.");
	CellLevelDebug.ProviderMode = EShadowProviderMode::CellProvider;
	Captures.Add(CellLevelDebug);

	FCaptureSpec CellFallbackDebug;
	CellFallbackDebug.Name = TEXT("CellProvider_FallbackDebug");
	CellFallbackDebug.Commands = CellLimitedCache.Commands;
	CellFallbackDebug.Commands.Add(TEXT("r.Shadow.MHStatic.Debug 20"));
	CellFallbackDebug.SettleFrames = 6;
	CellFallbackDebug.Notes = TEXT("Cell-provider miss/fallback visualization.");
	CellFallbackDebug.ProviderMode = EShadowProviderMode::CellProvider;
	Captures.Add(CellFallbackDebug);

	PairwiseSpecs.Add({ TEXT("CellProvider_AllLoaded_Source6"), TEXT("Monolithic_Source6_AllResident") });
	PairwiseSpecs.Add({ TEXT("CellProvider_Debug9"), TEXT("Monolithic_Debug9") });

	for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
	{
		for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::BuildRealClipmapRegressionPlan()
{
	ActiveProfileName = TEXT("RealClipmapRegression");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("OverviewHigh"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });
	Cameras.Add({ TEXT("CenterA"), FVector(-1700.0, -1800.0, 560.0), FVector(800.0, 900.0, 160.0) });
	Cameras.Add({ TEXT("EastRun"), FVector(5600.0, -1300.0, 700.0), FVector(2600.0, 1300.0, 170.0) });
	Cameras.Add({ TEXT("FarCorner"), FVector(5600.0, 5600.0, 940.0), FVector(2500.0, 2500.0, 180.0) });
	Cameras.Add({ TEXT("ContactA"), FVector(-2500.0, 300.0, 320.0), FVector(-1800.0, 600.0, 130.0) });
	Cameras.Add({ TEXT("ReturnOverview"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });

	const TArray<FString> StableCaptureCommands = {
		TEXT("r.ScreenPercentage 100"),
		TEXT("r.PostProcessAAQuality 0"),
		TEXT("r.AntiAliasingMethod 0"),
		TEXT("r.TemporalAA.Upsampling 0"),
		TEXT("r.MotionBlurQuality 0"),
		TEXT("r.EyeAdaptationQuality 0")
	};

	TArray<FString> RealClipmapAllResident = StableCaptureCommands;
	RealClipmapAllResident.Append({
		TEXT("r.Shadow.MHStatic.Enable 1"),
		TEXT("r.Shadow.MHStatic.Mode 3"),
		TEXT("r.Shadow.MHStatic.Source 6"),
		TEXT("r.Shadow.MHStatic.Debug 1"),
		TEXT("r.Shadow.MHStatic.DepthTest 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
		TEXT("r.Shadow.MHStatic.Cache.Enable 0"),
		TEXT("r.Shadow.MHStatic.Feedback.Enable 0"),
		TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3"),
		TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
		TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
		TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
	});

	FCaptureSpec MonolithicAllResident;
	MonolithicAllResident.Name = TEXT("RealClipmap_Monolithic_AllResident");
	MonolithicAllResident.Commands = RealClipmapAllResident;
	MonolithicAllResident.SettleFrames = 12;
	MonolithicAllResident.bAtlasBaseline = true;
	MonolithicAllResident.bResetCacheAtRouteStart = true;
	MonolithicAllResident.Notes = TEXT("Merged real multi-range clipmap asset, monolithic provider, all resident.");
	MonolithicAllResident.ProviderMode = EShadowProviderMode::Monolithic;
	Captures.Add(MonolithicAllResident);

	FCaptureSpec ZeroBiasHardBaseline = MonolithicAllResident;
	ZeroBiasHardBaseline.Name = TEXT("Source6_ZeroRestoredBias_HardBaseline");
	ZeroBiasHardBaseline.bAtlasBaseline = false;
	ZeroBiasHardBaseline.bCompareToAtlas = true;
	UpsertCommand(ZeroBiasHardBaseline.Commands, TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), TEXT("0"));
	UpsertCommand(ZeroBiasHardBaseline.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale"), TEXT("0"));
	UpsertCommand(ZeroBiasHardBaseline.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd"), TEXT("0"));
	ZeroBiasHardBaseline.Notes = TEXT("Explicit zero-restored-bias Source=6 hard-shadow baseline used to detect all-lit false passes.");
	Captures.Add(ZeroBiasHardBaseline);

	FCaptureSpec TunedBiasHardBaseline = MonolithicAllResident;
	TunedBiasHardBaseline.Name = TEXT("Source6_TunedBias_HardBaseline");
	TunedBiasHardBaseline.bAtlasBaseline = false;
	TunedBiasHardBaseline.bCompareToAtlas = true;
	UpsertCommand(TunedBiasHardBaseline.Commands, TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), TEXT("0"));
	UpsertCommand(TunedBiasHardBaseline.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale"), TEXT("0"));
	UpsertCommand(TunedBiasHardBaseline.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd"), GRealClipmapTunedRestoredDepthBiasAdd);
	TunedBiasHardBaseline.Notes = TEXT("Tuned Stage 3 hard-shadow baseline: manual bias 0.01 keeps cast shadows while suppressing zero-bias acne; Stage 5 should replace this with level-aware automatic bias.");
	Captures.Add(TunedBiasHardBaseline);

	FCaptureSpec DefaultBiasSuppression = MonolithicAllResident;
	DefaultBiasSuppression.Name = TEXT("Source6_DefaultRestoredBias_SuppressionCheck");
	DefaultBiasSuppression.bAtlasBaseline = false;
	DefaultBiasSuppression.bCompareToAtlas = true;
	DefaultBiasSuppression.Commands = RealClipmapAllResident;
	UpsertCommand(DefaultBiasSuppression.Commands, TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), TEXT("0"));
	UpsertCommand(DefaultBiasSuppression.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale"), TEXT("1"));
	UpsertCommand(DefaultBiasSuppression.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd"), TEXT("0"));
	DefaultBiasSuppression.Notes = TEXT("Old default restored-bias route; large differences from zero-bias indicate bias suppressed cast shadows.");
	Captures.Add(DefaultBiasSuppression);

	FCaptureSpec CellAllResident;
	CellAllResident.Name = TEXT("RealClipmap_CellProvider_AllLoaded");
	CellAllResident.Commands = RealClipmapAllResident;
	CellAllResident.SettleFrames = 12;
	CellAllResident.bResetCacheAtRouteStart = true;
	CellAllResident.Notes = TEXT("Real multi-range clipmap through cell provider; all cells loaded.");
	CellAllResident.ProviderMode = EShadowProviderMode::CellProvider;
	Captures.Add(CellAllResident);

	FCaptureSpec CellCache;
	CellCache.Name = TEXT("RealClipmap_CellProvider_8x8Cache");
	CellCache.Commands = StableCaptureCommands;
	CellCache.Commands.Append({
		TEXT("r.Shadow.MHStatic.Enable 1"),
		TEXT("r.Shadow.MHStatic.Mode 3"),
		TEXT("r.Shadow.MHStatic.Source 6"),
		TEXT("r.Shadow.MHStatic.Debug 1"),
		TEXT("r.Shadow.MHStatic.DepthTest 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
		TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
		TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
		TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
		TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
		TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3"),
		TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
		TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
		TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
	});
	CellCache.SettleFrames = 16;
	CellCache.bResetCacheAtRouteStart = true;
	CellCache.bCompareToAtlas = true;
	CellCache.Notes = TEXT("Real multi-range clipmap through cell provider with limited physical page cache.");
	CellCache.ProviderMode = EShadowProviderMode::CellProvider;
	Captures.Add(CellCache);

	FCaptureSpec LevelDebug = CellCache;
	LevelDebug.Name = TEXT("RealClipmap_LevelDebug");
	LevelDebug.Commands.Add(TEXT("r.Shadow.MHStatic.Debug 18"));
	LevelDebug.SettleFrames = 8;
	LevelDebug.bCompareToAtlas = false;
	LevelDebug.Notes = TEXT("Selected clipmap level visualization.");
	Captures.Add(LevelDebug);

	FCaptureSpec ResolvedDebug = CellCache;
	ResolvedDebug.Name = TEXT("RealClipmap_ResolvedLevelDebug");
	ResolvedDebug.Commands.Add(TEXT("r.Shadow.MHStatic.Debug 21"));
	ResolvedDebug.SettleFrames = 8;
	ResolvedDebug.bCompareToAtlas = false;
	ResolvedDebug.Notes = TEXT("Resolved/fallback clipmap level visualization.");
	Captures.Add(ResolvedDebug);

	FCaptureSpec CoverageDebug = CellCache;
	CoverageDebug.Name = TEXT("RealClipmap_UVCoverageDebug");
	CoverageDebug.Commands.Add(TEXT("r.Shadow.MHStatic.Debug 22"));
	CoverageDebug.SettleFrames = 8;
	CoverageDebug.bCompareToAtlas = false;
	CoverageDebug.Notes = TEXT("Per-level UV coverage visualization for real multi-range matrices.");
	Captures.Add(CoverageDebug);

	FCaptureSpec FallbackDebug = CellCache;
	FallbackDebug.Name = TEXT("RealClipmap_FallbackDebug");
	FallbackDebug.Commands.Add(TEXT("r.Shadow.MHStatic.Debug 20"));
	FallbackDebug.SettleFrames = 8;
	FallbackDebug.bCompareToAtlas = false;
	FallbackDebug.Notes = TEXT("Miss/fallback visualization for real multi-range clipmap.");
	Captures.Add(FallbackDebug);

	PairwiseSpecs.Add({ TEXT("RealClipmap_CellProvider_AllLoaded"), TEXT("RealClipmap_Monolithic_AllResident") });
	PairwiseSpecs.Add({ TEXT("RealClipmap_CellProvider_8x8Cache"), TEXT("RealClipmap_Monolithic_AllResident") });
	PairwiseSpecs.Add({ TEXT("Source6_DefaultRestoredBias_SuppressionCheck"), TEXT("Source6_TunedBias_HardBaseline") });
	PairwiseSpecs.Add({ TEXT("Source6_ZeroRestoredBias_HardBaseline"), TEXT("Source6_TunedBias_HardBaseline") });

	for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
	{
		for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::BuildCacheStrategyRegressionPlan()
{
	ActiveProfileName = TEXT("CacheStrategyRegression");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("OverviewHigh"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });
	Cameras.Add({ TEXT("CenterA"), FVector(-1700.0, -1800.0, 560.0), FVector(800.0, 900.0, 160.0) });
	Cameras.Add({ TEXT("EastRun"), FVector(5600.0, -1300.0, 700.0), FVector(2600.0, 1300.0, 170.0) });
	Cameras.Add({ TEXT("FarCorner"), FVector(5600.0, 5600.0, 940.0), FVector(2500.0, 2500.0, 180.0) });
	Cameras.Add({ TEXT("ContactA"), FVector(-2500.0, 300.0, 320.0), FVector(-1800.0, 600.0, 130.0) });
	Cameras.Add({ TEXT("ReturnOverview"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });

	const TArray<FString> StableCaptureCommands = {
		TEXT("r.ScreenPercentage 100"),
		TEXT("r.PostProcessAAQuality 0"),
		TEXT("r.AntiAliasingMethod 0"),
		TEXT("r.TemporalAA.Upsampling 0"),
		TEXT("r.MotionBlurQuality 0"),
		TEXT("r.EyeAdaptationQuality 0")
	};

	TArray<FString> AllResidentCommands = StableCaptureCommands;
	AllResidentCommands.Append({
		TEXT("r.Shadow.MHStatic.Enable 1"),
		TEXT("r.Shadow.MHStatic.Mode 3"),
		TEXT("r.Shadow.MHStatic.Source 6"),
		TEXT("r.Shadow.MHStatic.Debug 1"),
		TEXT("r.Shadow.MHStatic.DepthTest 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
		TEXT("r.Shadow.MHStatic.Restored.EnablePCF 0"),
		TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale 0"),
		FString::Printf(TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd %s"), GRealClipmapTunedRestoredDepthBiasAdd),
		TEXT("r.Shadow.MHStatic.Cache.Enable 0"),
		TEXT("r.Shadow.MHStatic.Feedback.Enable 0"),
		TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3"),
		TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
		TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
		TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
	});

	FCaptureSpec Baseline;
	Baseline.Name = TEXT("CellProvider_AllLoaded_AllResident");
	Baseline.Commands = AllResidentCommands;
	Baseline.SettleFrames = 12;
	Baseline.bAtlasBaseline = true;
	Baseline.bResetCacheAtRouteStart = true;
	Baseline.Notes = TEXT("Final same-source golden baseline: current CellProvider data with all pages resident and cache disabled.");
	Baseline.ProviderMode = EShadowProviderMode::CellProvider;
	Captures.Add(Baseline);

	auto MakeCacheRoute = [this, &StableCaptureCommands](const TCHAR* Name, int32 CachePages, int32 MaxUploads, int32 PriorityMode, int32 EvictRequested, int32 PrefetchRadius, int32 PrefetchBudget, int32 SettleFrames, const TCHAR* Notes)
	{
		FCaptureSpec Capture;
		Capture.Name = Name;
		Capture.Commands = StableCaptureCommands;
		Capture.Commands.Append({
			TEXT("r.Shadow.MHStatic.Enable 1"),
			TEXT("r.Shadow.MHStatic.Mode 3"),
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
			TEXT("r.Shadow.MHStatic.Restored.EnablePCF 0"),
			TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale 0"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd %s"), GRealClipmapTunedRestoredDepthBiasAdd),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX %d"), CachePages),
			FString::Printf(TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY %d"), CachePages),
			FString::Printf(TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame %d"), MaxUploads),
			FString::Printf(TEXT("r.Shadow.MHStatic.Cache.PriorityMode %d"), PriorityMode),
			TEXT("r.Shadow.MHStatic.Cache.LevelPriorityScale 0.15"),
			TEXT("r.Shadow.MHStatic.Cache.ResidencyBoost 0.25"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Cache.EvictRequested %d"), EvictRequested),
			TEXT("r.Shadow.MHStatic.Cache.EvictHysteresis 0.15"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Cache.PrefetchRadius %d"), PrefetchRadius),
			FString::Printf(TEXT("r.Shadow.MHStatic.Cache.PrefetchBudget %d"), PrefetchBudget),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3"),
			TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
			TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
		});
		Capture.SettleFrames = SettleFrames;
		Capture.bResetCacheAtRouteStart = true;
		Capture.bCompareToAtlas = true;
		Capture.Notes = Notes;
		Capture.ProviderMode = EShadowProviderMode::CellProvider;
		return Capture;
	};

	Captures.Add(MakeCacheRoute(TEXT("Cache8x8_CurrentPolicy"), 8, 16, 0, 0, 0, 0, 16, TEXT("Legacy unordered requested-page cache policy, used as Stage 4 baseline.")));
	Captures.Add(MakeCacheRoute(TEXT("Cache8x8_LowMemoryPriority"), 8, 16, 1, 1, 0, 0, 16, TEXT("Low-memory preset: 8x8 physical pages with pixel-count priority and no prefetch.")));
	Captures.Add(MakeCacheRoute(TEXT("Cache16x16_DisplayPriorityPrefetch"), 16, 32, 1, 1, 1, 32, 16, TEXT("Recommended display preset: 16x16 physical pages with priority, requested-page eviction, and 4-neighbor prefetch.")));
	Captures.Add(MakeCacheRoute(TEXT("Cache16x16_DisplayPriorityPrefetch_LongSettle"), 16, 32, 1, 1, 1, 32, 40, TEXT("Recommended display preset with longer settle to measure convergence.")));

	PairwiseSpecs.Add({ TEXT("Cache8x8_CurrentPolicy"), TEXT("CellProvider_AllLoaded_AllResident") });
	PairwiseSpecs.Add({ TEXT("Cache8x8_LowMemoryPriority"), TEXT("CellProvider_AllLoaded_AllResident") });
	PairwiseSpecs.Add({ TEXT("Cache16x16_DisplayPriorityPrefetch"), TEXT("CellProvider_AllLoaded_AllResident") });
	PairwiseSpecs.Add({ TEXT("Cache16x16_DisplayPriorityPrefetch_LongSettle"), TEXT("CellProvider_AllLoaded_AllResident") });
	PairwiseSpecs.Add({ TEXT("Cache8x8_LowMemoryPriority"), TEXT("Cache8x8_CurrentPolicy") });
	PairwiseSpecs.Add({ TEXT("Cache16x16_DisplayPriorityPrefetch"), TEXT("Cache8x8_LowMemoryPriority") });

	for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
	{
		for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::BuildQualityRegressionPlan()
{
	ActiveProfileName = TEXT("QualityRegression");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("OverviewHigh"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });
	Cameras.Add({ TEXT("CenterA"), FVector(-1700.0, -1800.0, 560.0), FVector(800.0, 900.0, 160.0) });
	Cameras.Add({ TEXT("EastRun"), FVector(5600.0, -1300.0, 700.0), FVector(2600.0, 1300.0, 170.0) });
	Cameras.Add({ TEXT("FarCorner"), FVector(5600.0, 5600.0, 940.0), FVector(2500.0, 2500.0, 180.0) });
	Cameras.Add({ TEXT("ContactA"), FVector(-2500.0, 300.0, 320.0), FVector(-1800.0, 600.0, 130.0) });
	Cameras.Add({ TEXT("ReturnOverview"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });

	const TArray<FString> StableCaptureCommands = {
		TEXT("r.ScreenPercentage 100"),
		TEXT("r.PostProcessAAQuality 0"),
		TEXT("r.AntiAliasingMethod 0"),
		TEXT("r.TemporalAA.Upsampling 0"),
		TEXT("r.MotionBlurQuality 0"),
		TEXT("r.EyeAdaptationQuality 0"),
		TEXT("r.Shadow.MHStatic.Enable 1"),
		TEXT("r.Shadow.MHStatic.Mode 3"),
		TEXT("r.Shadow.MHStatic.Source 6"),
		TEXT("r.Shadow.MHStatic.Debug 1"),
		TEXT("r.Shadow.MHStatic.DepthTest 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
		TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
		TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 16"),
		TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 16"),
		TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 32"),
		TEXT("r.Shadow.MHStatic.Cache.PriorityMode 1"),
		TEXT("r.Shadow.MHStatic.Cache.LevelPriorityScale 0.15"),
		TEXT("r.Shadow.MHStatic.Cache.ResidencyBoost 0.25"),
		TEXT("r.Shadow.MHStatic.Cache.EvictRequested 1"),
		TEXT("r.Shadow.MHStatic.Cache.EvictHysteresis 0.15"),
		TEXT("r.Shadow.MHStatic.Cache.PrefetchRadius 1"),
		TEXT("r.Shadow.MHStatic.Cache.PrefetchBudget 32"),
		TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3"),
		TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
		TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
		TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
	};

	auto MakeQualityRoute = [&StableCaptureCommands](const TCHAR* Name, int32 BiasMode, int32 FilterMode, int32 Kernel, int32 SamplePolicy, float RadiusTexels, const TCHAR* BiasAdd, int32 SettleFrames, const TCHAR* Notes)
	{
		FCaptureSpec Capture;
		Capture.Name = Name;
		Capture.Commands = StableCaptureCommands;
		Capture.Commands.Append({
			TEXT("r.Shadow.MHStatic.Restored.EnablePCF 0"),
			TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale 0"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd %s"), BiasAdd),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.BiasMode %d"), BiasMode),
			TEXT("r.Shadow.MHStatic.Restored.BiasWorldUnits 4"),
			TEXT("r.Shadow.MHStatic.Restored.SlopeBiasScale 1"),
			TEXT("r.Shadow.MHStatic.Restored.SlopeBiasClamp 0.01"),
			TEXT("r.Shadow.MHStatic.Restored.ForceScreenSlope 0"),
			TEXT("r.Shadow.MHStatic.Restored.UEBiasDistribution 1"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.FilterBiasScale %.3f"), FilterMode != 0 ? 0.5f : 0.0f),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.FilterMode %d"), FilterMode),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.PCFKernel %d"), Kernel),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.PCFSamplePolicy %d"), SamplePolicy),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.PCFRadiusTexels %.3f"), RadiusTexels)
		});
		if (FilterMode != 0)
		{
			UpsertCommand(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), TEXT("1"));
		}
		Capture.SettleFrames = SettleFrames;
		Capture.bResetCacheAtRouteStart = true;
		Capture.bCompareToAtlas = true;
		Capture.Notes = Notes;
		Capture.ProviderMode = EShadowProviderMode::CellProvider;
		return Capture;
	};

	FCaptureSpec Hard = MakeQualityRoute(TEXT("Hard_FixedBias_0p01"), 0, 0, 3, 1, 1.0f, GRealClipmapTunedRestoredDepthBiasAdd, 20, TEXT("Manual fixed 0.01 bias hard-shadow baseline for Stage 5 quality comparison."));
	Hard.bAtlasBaseline = true;
	Captures.Add(Hard);
	Captures.Add(MakeQualityRoute(TEXT("Hard_LevelAware_WorldBias"), 1, 0, 3, 1, 1.0f, TEXT("0"), 20, TEXT("Automatic per-level world-space bias, hard compare.")));
	Captures.Add(MakeQualityRoute(TEXT("Hard_OldReceiverPlane"), 2, 0, 3, 1, 1.0f, TEXT("0"), 20, TEXT("Previous receiver-plane slope bias using GBuffer normal, hard compare.")));
	Captures.Add(MakeQualityRoute(TEXT("Hard_UECSMCompatible"), 3, 0, 3, 1, 1.0f, TEXT("0"), 20, TEXT("UE CSM-compatible bias using Directional Light shadow bias and CSM bias CVars.")));
	FCaptureSpec ScreenFallback = MakeQualityRoute(TEXT("Hard_UEReceiverPlane_ScreenFallback"), 2, 0, 3, 1, 1.0f, TEXT("0"), 20, TEXT("Receiver-plane route with screen-depth fallback diagnostics; use only if GBuffer normal is unavailable."));
	UpsertCommand(ScreenFallback.Commands, TEXT("r.Shadow.MHStatic.Restored.ForceScreenSlope"), TEXT("1"));
	Captures.Add(ScreenFallback);
	Captures.Add(MakeQualityRoute(TEXT("PCF3_FixedBias_SameResolvedLevel"), 0, 1, 3, 1, 1.0f, GRealClipmapTunedRestoredDepthBiasAdd, 24, TEXT("Conservative Source=6 3x3 PCF: fixed tuned bias and center-resolved clipmap level.")));
	Captures.Add(MakeQualityRoute(TEXT("PCF3_UECSMCompatible"), 3, 1, 3, 1, 1.0f, TEXT("0"), 24, TEXT("UE CSM-compatible bias plus same-resolved-level 3x3 PCF.")));
	FCaptureSpec ReceiverPCF = MakeQualityRoute(TEXT("PCF3_UEReceiverPlane_SameResolvedLevel"), 2, 1, 3, 1, 1.0f, GRealClipmapTunedRestoredDepthBiasAdd, 24, TEXT("Recommended Stage 5.2 route: fixed 0.01 bias floor plus UE-style receiver-plane same-level 3x3 PCF."));
	Captures.Add(ReceiverPCF);

	FCaptureSpec PCF3Repeat = ReceiverPCF;
	PCF3Repeat.Name = TEXT("PCF3_UEReceiverPlane_SameResolvedLevel_Repeat");
	PCF3Repeat.Notes = TEXT("Repeat capture for UE-style receiver-plane PCF temporal stability.");
	PCF3Repeat.bClipmapStabilityRepeat = true;
	PCF3Repeat.StabilityBaselineName = TEXT("PCF3_UEReceiverPlane_SameResolvedLevel");
	Captures.Add(PCF3Repeat);

	PairwiseSpecs.Add({ TEXT("Hard_LevelAware_WorldBias"), TEXT("Hard_FixedBias_0p01") });
	PairwiseSpecs.Add({ TEXT("Hard_OldReceiverPlane"), TEXT("Hard_FixedBias_0p01") });
	PairwiseSpecs.Add({ TEXT("Hard_UECSMCompatible"), TEXT("Hard_FixedBias_0p01") });
	PairwiseSpecs.Add({ TEXT("Hard_UEReceiverPlane_ScreenFallback"), TEXT("Hard_FixedBias_0p01") });
	PairwiseSpecs.Add({ TEXT("PCF3_FixedBias_SameResolvedLevel"), TEXT("Hard_FixedBias_0p01") });
	PairwiseSpecs.Add({ TEXT("PCF3_UECSMCompatible"), TEXT("Hard_FixedBias_0p01") });
	PairwiseSpecs.Add({ TEXT("PCF3_UECSMCompatible"), TEXT("PCF3_FixedBias_SameResolvedLevel") });
	PairwiseSpecs.Add({ TEXT("PCF3_UEReceiverPlane_SameResolvedLevel"), TEXT("Hard_FixedBias_0p01") });
	PairwiseSpecs.Add({ TEXT("PCF3_UEReceiverPlane_SameResolvedLevel"), TEXT("PCF3_FixedBias_SameResolvedLevel") });

	for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
	{
		for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::BuildSoftShadowRegressionPlan()
{
	ActiveProfileName = TEXT("SoftShadowRegression");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("OverviewHigh"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });
	Cameras.Add({ TEXT("CenterA"), FVector(-1700.0, -1800.0, 560.0), FVector(800.0, 900.0, 160.0) });
	Cameras.Add({ TEXT("EastRun"), FVector(5600.0, -1300.0, 700.0), FVector(2600.0, 1300.0, 170.0) });
	Cameras.Add({ TEXT("FarCorner"), FVector(5600.0, 5600.0, 940.0), FVector(2500.0, 2500.0, 180.0) });
	Cameras.Add({ TEXT("ContactA"), FVector(-2500.0, 300.0, 320.0), FVector(-1800.0, 600.0, 130.0) });
	Cameras.Add({ TEXT("ReturnOverview"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });

	const TArray<FString> StableCaptureCommands = {
		TEXT("r.ScreenPercentage 100"),
		TEXT("r.PostProcessAAQuality 0"),
		TEXT("r.AntiAliasingMethod 0"),
		TEXT("r.TemporalAA.Upsampling 0"),
		TEXT("r.MotionBlurQuality 0"),
		TEXT("r.EyeAdaptationQuality 0"),
		TEXT("r.Shadow.MHStatic.Enable 1"),
		TEXT("r.Shadow.MHStatic.Mode 3"),
		TEXT("r.Shadow.MHStatic.Source 6"),
		TEXT("r.Shadow.MHStatic.Debug 1"),
		TEXT("r.Shadow.MHStatic.DepthTest 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
		TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
		TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
		TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 16"),
		TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 16"),
		TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 32"),
		TEXT("r.Shadow.MHStatic.Cache.PriorityMode 1"),
		TEXT("r.Shadow.MHStatic.Cache.LevelPriorityScale 0.15"),
		TEXT("r.Shadow.MHStatic.Cache.ResidencyBoost 0.25"),
		TEXT("r.Shadow.MHStatic.Cache.EvictRequested 1"),
		TEXT("r.Shadow.MHStatic.Cache.EvictHysteresis 0.15"),
		TEXT("r.Shadow.MHStatic.Cache.PrefetchRadius 1"),
		TEXT("r.Shadow.MHStatic.Cache.PrefetchBudget 32"),
		TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3"),
		TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
		TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
		TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
		TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
	};

	auto MakeSoftRoute = [&StableCaptureCommands](const TCHAR* Name, int32 BiasMode, int32 FilterMode, int32 SettleFrames, bool bRepeat, const TCHAR* Notes)
	{
		FCaptureSpec Capture;
		Capture.Name = Name;
		Capture.Commands = StableCaptureCommands;
		Capture.Commands.Append({
			TEXT("r.Shadow.MHStatic.Restored.EnablePCF 0"),
			TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale 0"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd %s"), GRealClipmapTunedRestoredDepthBiasAdd),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.BiasMode %d"), BiasMode),
			TEXT("r.Shadow.MHStatic.Restored.BiasWorldUnits 4"),
			TEXT("r.Shadow.MHStatic.Restored.SlopeBiasScale 1"),
			TEXT("r.Shadow.MHStatic.Restored.SlopeBiasClamp 0.01"),
			TEXT("r.Shadow.MHStatic.Restored.ForceScreenSlope 0"),
			TEXT("r.Shadow.MHStatic.Restored.FilterBiasScale 0"),
			TEXT("r.Shadow.MHStatic.Restored.CoarseLevelBiasScale 0"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.FilterMode %d"), FilterMode),
			TEXT("r.Shadow.MHStatic.Restored.PCFKernel 3"),
			TEXT("r.Shadow.MHStatic.Restored.PCFSamplePolicy 1"),
			TEXT("r.Shadow.MHStatic.Restored.PCFRadiusTexels 1"),
			TEXT("r.Shadow.MHStatic.Restored.Soft.Mode 1"),
			TEXT("r.Shadow.MHStatic.Restored.Soft.BlockerSamples 8"),
			TEXT("r.Shadow.MHStatic.Restored.Soft.FilterSamples 16"),
			TEXT("r.Shadow.MHStatic.Restored.Soft.BlockerRadiusTexels 4"),
			TEXT("r.Shadow.MHStatic.Restored.Soft.MinRadiusTexels 1"),
			TEXT("r.Shadow.MHStatic.Restored.Soft.MaxRadiusTexels 6"),
			TEXT("r.Shadow.MHStatic.Restored.Soft.PenumbraScale 0.05"),
			TEXT("r.Shadow.MHStatic.Restored.Soft.TransitionScale 4096"),
			TEXT("r.Shadow.MHStatic.Restored.Soft.SourceAngleScale 1")
		});
		if (FilterMode != 0)
		{
			UpsertCommand(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), TEXT("1"));
		}
		Capture.SettleFrames = SettleFrames;
		Capture.bResetCacheAtRouteStart = true;
		Capture.bCompareToAtlas = true;
		Capture.bClipmapStabilityRepeat = bRepeat;
		Capture.Notes = Notes;
		Capture.ProviderMode = EShadowProviderMode::CellProvider;
		return Capture;
	};

	FCaptureSpec Hard = MakeSoftRoute(TEXT("Hard_FixedBias_0p01"), 0, 0, 20, false, TEXT("Manual fixed 0.01 bias hard-shadow baseline for contact-hardening comparison."));
	Hard.bAtlasBaseline = true;
	Captures.Add(Hard);
	Captures.Add(MakeSoftRoute(TEXT("PCF3_UECSMCompatible"), 3, 1, 24, false, TEXT("Stable same-resolved-level 3x3 PCF baseline using UE CSM-compatible bias.")));
	Captures.Add(MakeSoftRoute(TEXT("Soft_UEPCSS_Blocker8_Filter16"), 3, 2, 28, false, TEXT("UE PCSS-compatible contact-hardening route: 8 blocker taps and 16 soft PCF taps.")));
	FCaptureSpec LegacySoft = MakeSoftRoute(TEXT("Soft_LegacyContactHardening"), 2, 2, 28, false, TEXT("Legacy contact-hardening route retained as a visual and metric comparison."));
	UpsertCommand(LegacySoft.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.Mode"), TEXT("0"));
	Captures.Add(LegacySoft);

	FCaptureSpec SoftRepeat = MakeSoftRoute(TEXT("Soft_UEPCSS_Blocker8_Filter16_Repeat"), 3, 2, 28, true, TEXT("Repeat capture for UE PCSS-compatible contact-hardening temporal stability."));
	SoftRepeat.StabilityBaselineName = TEXT("Soft_UEPCSS_Blocker8_Filter16");
	Captures.Add(SoftRepeat);

	PairwiseSpecs.Add({ TEXT("PCF3_UECSMCompatible"), TEXT("Hard_FixedBias_0p01") });
	PairwiseSpecs.Add({ TEXT("Soft_UEPCSS_Blocker8_Filter16"), TEXT("Hard_FixedBias_0p01") });
	PairwiseSpecs.Add({ TEXT("Soft_UEPCSS_Blocker8_Filter16"), TEXT("PCF3_UECSMCompatible") });
	PairwiseSpecs.Add({ TEXT("Soft_UEPCSS_Blocker8_Filter16"), TEXT("Soft_LegacyContactHardening") });

	for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
	{
		for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::BuildLightingIntegrationRegressionPlan()
{
	ActiveProfileName = TEXT("LightingIntegrationRegression");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("OverviewHigh"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });
	Cameras.Add({ TEXT("CenterA"), FVector(-1700.0, -1800.0, 560.0), FVector(800.0, 900.0, 160.0) });
	Cameras.Add({ TEXT("ContactA"), FVector(-2500.0, 300.0, 320.0), FVector(-1800.0, 600.0, 130.0) });

	const TArray<FString> StableCaptureCommands = {
		TEXT("r.ScreenPercentage 100"),
		TEXT("r.PostProcessAAQuality 0"),
		TEXT("r.AntiAliasingMethod 0"),
		TEXT("r.TemporalAA.Upsampling 0"),
		TEXT("r.MotionBlurQuality 0"),
		TEXT("r.EyeAdaptationQuality 0")
	};

	auto MakeSource6Route = [&StableCaptureCommands](const TCHAR* Name, int32 Mode, int32 Debug, int32 FilterMode, int32 BiasMode, const TCHAR* BiasAdd, const TCHAR* Notes)
	{
		FCaptureSpec Capture;
		Capture.Name = Name;
		Capture.Commands = StableCaptureCommands;
		Capture.Commands.Append({
			TEXT("r.Shadow.MHStatic.Enable 1"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Mode %d"), Mode),
			TEXT("r.Shadow.MHStatic.Source 6"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Debug %d"), Debug),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 16"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 16"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 32"),
			TEXT("r.Shadow.MHStatic.Cache.PriorityMode 1"),
			TEXT("r.Shadow.MHStatic.Cache.LevelPriorityScale 0.15"),
			TEXT("r.Shadow.MHStatic.Cache.ResidencyBoost 0.25"),
			TEXT("r.Shadow.MHStatic.Cache.EvictRequested 1"),
			TEXT("r.Shadow.MHStatic.Cache.EvictHysteresis 0.15"),
			TEXT("r.Shadow.MHStatic.Cache.PrefetchRadius 1"),
			TEXT("r.Shadow.MHStatic.Cache.PrefetchBudget 32"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3"),
			TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
			TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3"),
			TEXT("r.Shadow.MHStatic.Restored.EnablePCF 0"),
			TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale 0"),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd %s"), BiasAdd),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.BiasMode %d"), BiasMode),
			FString::Printf(TEXT("r.Shadow.MHStatic.Restored.FilterMode %d"), FilterMode),
			TEXT("r.Shadow.MHStatic.Restored.PCFSamplePolicy 1"),
			TEXT("r.Shadow.MHStatic.Cache.Reset 1")
		});
		Capture.SettleFrames = 24;
		Capture.bResetCacheAtRouteStart = true;
		Capture.Notes = Notes;
		Capture.ProviderMode = EShadowProviderMode::CellProvider;
		return Capture;
	};

	FCaptureSpec UEBaseline;
	UEBaseline.Name = TEXT("UEBaseline_Enable0");
	UEBaseline.Commands = StableCaptureCommands;
	UEBaseline.Commands.Append({
		TEXT("r.Shadow.MHStatic.Enable 0"),
		TEXT("r.Shadow.MHStatic.Debug 0")
	});
	UEBaseline.SettleFrames = 12;
	UEBaseline.Notes = TEXT("Original UE lighting with MH disabled.");
	Captures.Add(UEBaseline);

	FCaptureSpec MaskDebug = MakeSource6Route(TEXT("Mask_Mode3_HardDebug"), 3, 1, 0, 0, GRealClipmapTunedRestoredDepthBiasAdd, TEXT("MH hard visibility mask in debug mode for alignment checks."));
	MaskDebug.bAtlasBaseline = true;
	Captures.Add(MaskDebug);

	Captures.Add(MakeSource6Route(TEXT("Lit_Mode4_DirectLighting_Hard"), 4, 0, 0, 0, GRealClipmapTunedRestoredDepthBiasAdd, TEXT("MH visibility encoded as UE light attenuation and consumed by deferred directional lighting.")));

	FCaptureSpec LitPCF = MakeSource6Route(TEXT("Lit_Mode4_DirectLighting_PCF"), 4, 0, 1, 3, TEXT("0"), TEXT("Lighting integration with the current conservative Source=6 PCF route."));
	UpsertCommand(LitPCF.Commands, TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), TEXT("1"));
	UpsertCommand(LitPCF.Commands, TEXT("r.Shadow.MHStatic.Restored.PCFKernel"), TEXT("3"));
	UpsertCommand(LitPCF.Commands, TEXT("r.Shadow.MHStatic.Restored.PCFRadiusTexels"), TEXT("1"));
	Captures.Add(LitPCF);

	PairwiseSpecs.Add({ TEXT("Lit_Mode4_DirectLighting_Hard"), TEXT("UEBaseline_Enable0") });
	PairwiseSpecs.Add({ TEXT("Lit_Mode4_DirectLighting_PCF"), TEXT("Lit_Mode4_DirectLighting_Hard") });

	for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
	{
		for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::BuildClipmapRegressionPlan()
{
	ActiveProfileName = TEXT("ClipmapRegression");
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();
	PairwiseSpecs.Reset();

	Cameras.Add({ TEXT("OverviewHigh"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });
	Cameras.Add({ TEXT("NorthWestLow"), FVector(-4300.0, -4300.0, 720.0), FVector(-2500.0, -2500.0, 160.0) });
	Cameras.Add({ TEXT("NorthSweep"), FVector(-2400.0, -5600.0, 680.0), FVector(900.0, -2600.0, 150.0) });
	Cameras.Add({ TEXT("CenterA"), FVector(-1700.0, -1800.0, 560.0), FVector(800.0, 900.0, 160.0) });
	Cameras.Add({ TEXT("CenterB"), FVector(1300.0, -2200.0, 540.0), FVector(-900.0, 1300.0, 160.0) });
	Cameras.Add({ TEXT("EastRun"), FVector(5600.0, -1300.0, 700.0), FVector(2600.0, 1300.0, 170.0) });
	Cameras.Add({ TEXT("FarCorner"), FVector(5600.0, 5600.0, 940.0), FVector(2500.0, 2500.0, 180.0) });
	Cameras.Add({ TEXT("SouthDiag"), FVector(2400.0, 5700.0, 720.0), FVector(-1000.0, 1500.0, 160.0) });
	Cameras.Add({ TEXT("WestRun"), FVector(-5600.0, 1200.0, 700.0), FVector(-2500.0, -900.0, 160.0) });
	Cameras.Add({ TEXT("ContactA"), FVector(-2500.0, 300.0, 320.0), FVector(-1800.0, 600.0, 130.0) });
	Cameras.Add({ TEXT("ContactB"), FVector(800.0, 1800.0, 360.0), FVector(1300.0, 2200.0, 130.0) });
	Cameras.Add({ TEXT("ReturnOverview"), FVector(-6200.0, -6200.0, 3600.0), FVector(0.0, 0.0, 180.0) });

	Captures.Add({
		TEXT("AtlasBaseline"),
		{
			TEXT("r.Shadow.MHStatic.Enable 1"),
			TEXT("r.Shadow.MHStatic.Mode 3"),
			TEXT("r.Shadow.MHStatic.Source 2"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.DepthTest 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasScale 0"),
			TEXT("r.Shadow.MHStatic.DepthBiasAdd 0"),
			TEXT("r.Shadow.MHStatic.Restored.EnablePCF 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
		},
		10,
		true,
		false,
		false,
		false,
		TEXT("Full restored atlas hard-shadow baseline for clipmap regression")
	});

	Captures.Add({
		TEXT("LimitedCache_4x4"),
		{
			TEXT("r.Shadow.MHStatic.Source 5"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 4"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 4"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 8"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Source=5 single-level limited cache baseline with 16 physical pages")
	});

	Captures.Add({
		TEXT("ClipmapSingleLevel_4x4"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 4"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 4"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 8"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.SingleLevelUseBaseData 1")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Source=6 degenerated to level 0 with 16 physical pages")
	});

	Captures.Add({
		TEXT("LimitedCache_8x8"),
		{
			TEXT("r.Shadow.MHStatic.Source 5"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Source=5 single-level limited cache baseline with 64 physical pages")
	});

	Captures.Add({
		TEXT("ClipmapSingleLevel_8x8"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.SingleLevelUseBaseData 1")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Source=6 degenerated to level 0 with 64 physical pages")
	});

	Captures.Add({
		TEXT("Clipmap_Default_8x8"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4"),
			TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 900"),
			TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 1.75"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 0"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Current Source=6 default regression route")
	});

	Captures.Add({
		TEXT("Clipmap_FineBias1_8x8"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4"),
			TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 900"),
			TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 1.75"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Biases level choice one level finer")
	});

	Captures.Add({
		TEXT("Clipmap_FineBias1_Fallback1_8x8"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4"),
			TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 900"),
			TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 1.75"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 1")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("One-level finer selection, but fallback may only go one coarser level")
	});

	Captures.Add({
		TEXT("Clipmap_FineBias1_Level0_1600_8x8"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4"),
			TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
			TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 1.75"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("One-level finer selection with larger level-0 distance")
	});

	Captures.Add({
		TEXT("Clipmap_FineBias1_Level0_1600_Scale2_8x8"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4"),
			TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
			TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
		},
		10,
		false,
		true,
		false,
		true,
		TEXT("Candidate tuned route: one-level finer, level0=1600, scale=2")
	});

	Captures.Add({
		TEXT("Clipmap_TunedRepeat"),
		{
			TEXT("r.Shadow.MHStatic.Source 6"),
			TEXT("r.Shadow.MHStatic.Debug 1"),
			TEXT("r.Shadow.MHStatic.Cache.Enable 1"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"),
			TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"),
			TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 16"),
			TEXT("r.Shadow.MHStatic.Feedback.Enable 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 4"),
			TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 1600"),
			TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 2"),
			TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias 1"),
			TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels 3")
		},
		8,
		false,
		false,
		true,
		false,
		TEXT("Repeat tuned route for stability check"),
		TEXT("Clipmap_FineBias1_Level0_1600_Scale2_8x8")
	});

	PairwiseSpecs.Add({ TEXT("ClipmapSingleLevel_4x4"), TEXT("LimitedCache_4x4") });
	PairwiseSpecs.Add({ TEXT("ClipmapSingleLevel_8x8"), TEXT("LimitedCache_8x8") });
	PairwiseSpecs.Add({ TEXT("Clipmap_Default_8x8"), TEXT("LimitedCache_8x8") });
	PairwiseSpecs.Add({ TEXT("Clipmap_FineBias1_8x8"), TEXT("LimitedCache_8x8") });
	PairwiseSpecs.Add({ TEXT("Clipmap_FineBias1_Fallback1_8x8"), TEXT("LimitedCache_8x8") });
	PairwiseSpecs.Add({ TEXT("Clipmap_FineBias1_Level0_1600_8x8"), TEXT("LimitedCache_8x8") });
	PairwiseSpecs.Add({ TEXT("Clipmap_FineBias1_Level0_1600_Scale2_8x8"), TEXT("LimitedCache_8x8") });
	PairwiseSpecs.Add({ TEXT("Clipmap_TunedRepeat"), TEXT("Clipmap_FineBias1_Level0_1600_Scale2_8x8") });

	for (int32 CaptureIndex = 0; CaptureIndex < Captures.Num(); ++CaptureIndex)
	{
		for (int32 CameraIndex = 0; CameraIndex < Cameras.Num(); ++CameraIndex)
		{
			Steps.Add({ CameraIndex, CaptureIndex });
		}
	}
}

void FMHShadowBenchmarkRunner::StartNextStep()
{
	++CurrentStepIndex;
	if (!Steps.IsValidIndex(CurrentStepIndex))
	{
		FinishBenchmark();
		return;
	}

	const FBenchmarkStep& Step = Steps[CurrentStepIndex];
	ApplyStep(Step);
	RemainingSettleFrames = FMath::Max(1, Captures[Step.CaptureIndex].SettleFrames);

	UE_LOG(LogMHShadowBenchmark, Display, TEXT("Benchmark step %d/%d: %s / %s"), CurrentStepIndex + 1, Steps.Num(), *Cameras[Step.CameraIndex].Name, *Captures[Step.CaptureIndex].Name);
}

void FMHShadowBenchmarkRunner::FinishBenchmark()
{
	UWorld* World = GetBenchmarkWorld();
	if (World)
	{
		Exec(World, TEXT("r.Shadow.MHStatic.Stats.Dump"));
	}

	WriteOutputs();

	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	bRunning = false;
	DestroyBenchmarkCamera();
	UE_LOG(LogMHShadowBenchmark, Display, TEXT("MH benchmark complete. Output: %s"), *OutputDir);
}

void FMHShadowBenchmarkRunner::ApplyStep(const FBenchmarkStep& Step)
{
	UWorld* World = GetBenchmarkWorld();
	if (!World)
	{
		return;
	}

	ApplyCamera(Cameras[Step.CameraIndex]);
	if (Step.CameraIndex == 0 && Captures[Step.CaptureIndex].ProviderMode != EShadowProviderMode::Default)
	{
		if (!ConfigureShadowProvider(*World, Captures[Step.CaptureIndex]))
		{
			UE_LOG(LogMHShadowBenchmark, Error, TEXT("Failed to configure shadow provider for capture %s."), *Captures[Step.CaptureIndex].Name);
			return;
		}
	}
	if (Step.CameraIndex == 0 && Captures[Step.CaptureIndex].bResetCacheAtRouteStart)
	{
		Exec(World, TEXT("r.Shadow.MHStatic.Cache.Reset 1"));
		RouteStartFrameByCapture.FindOrAdd(Captures[Step.CaptureIndex].Name) = GetLatestRuntimeStatsFrame();
	}
	for (const FString& Command : Captures[Step.CaptureIndex].Commands)
	{
		Exec(World, Command);
	}
}

void FMHShadowBenchmarkRunner::ApplyCamera(const FCameraSpec& CameraSpec)
{
	UWorld* World = GetBenchmarkWorld();
	if (!World)
	{
		return;
	}

	APlayerController* PlayerController = World->GetFirstPlayerController();
	if (!PlayerController)
	{
		UE_LOG(LogMHShadowBenchmark, Warning, TEXT("No PlayerController found; benchmark camera cannot be applied."));
		return;
	}

	ACameraActor* CameraActor = BenchmarkCamera.Get();
	if (!CameraActor)
	{
		DestroyStaleBenchmarkCameras(World);

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		CameraActor = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), CameraSpec.Location, FRotator::ZeroRotator, SpawnParameters);
		BenchmarkCamera = CameraActor;
#if WITH_EDITOR
		if (CameraActor)
		{
			CameraActor->SetActorLabel(TEXT("MHShadowBenchmarkCamera"));
		}
#endif
	}

	if (CameraActor)
	{
		const FRotator Rotation = (CameraSpec.Target - CameraSpec.Location).Rotation();
		CameraActor->SetActorLocationAndRotation(CameraSpec.Location, Rotation);
		PlayerController->SetViewTarget(CameraActor);
	}
}

void FMHShadowBenchmarkRunner::DestroyBenchmarkCamera()
{
	UWorld* World = nullptr;
	if (ACameraActor* CameraActor = BenchmarkCamera.Get())
	{
		World = CameraActor->GetWorld();
		if (!CameraActor->IsActorBeingDestroyed())
		{
			CameraActor->Destroy();
		}
	}
	BenchmarkCamera.Reset();

	DestroyStaleBenchmarkCameras(World);
}

bool FMHShadowBenchmarkRunner::EnsureLargeCacheStressData(UWorld& World)
{
	UMHShadowDataAsset* StressAsset = LoadActiveBenchmarkDataAsset();
	if (!StressAsset || !StressAsset->IsValidForRendering())
	{
		UE_LOG(LogMHShadowBenchmark, Error, TEXT("%s benchmark needs a valid monolithic MH shadow asset. Build/import/merge data first."), *ActiveProfileName);
		return false;
	}

	int32 ComponentCount = 0;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		for (UActorComponent* Component : Actor->GetComponents())
		{
			UMHShadowComponent* MHComponent = Cast<UMHShadowComponent>(Component);
			if (!MHComponent)
			{
				continue;
			}

			MHComponent->ShadowData = StressAsset;
			MHComponent->RegisterShadowData();
			++ComponentCount;
		}
	}

	if (ComponentCount == 0)
	{
		UE_LOG(LogMHShadowBenchmark, Error, TEXT("LargeCacheStress benchmark found no UMHShadowComponent in map %s."), *World.GetPathName());
		return false;
	}

	UE_LOG(LogMHShadowBenchmark, Display, TEXT("LargeCacheStress benchmark assigned %s to %d MHShadowComponent(s)."), *StressAsset->GetPathName(), ComponentCount);
	return true;
}

UMHShadowDataAsset* FMHShadowBenchmarkRunner::LoadLargeCacheStressDataAsset() const
{
	const TCHAR* FinalPath = TEXT("/Game/MHShadow/Baked/MHShadowData_LightmassDual_LargeCacheStress_4096_Clipmap_Final.MHShadowData_LightmassDual_LargeCacheStress_4096_Clipmap_Final");
	if (UMHShadowDataAsset* StressAsset = LoadObject<UMHShadowDataAsset>(nullptr, FinalPath))
	{
		return StressAsset;
	}

	const TCHAR* FallbackPath = TEXT("/Game/MHShadow/Baked/MHShadowData_LightmassDual_LargeCacheStress_4096_Clipmap.MHShadowData_LightmassDual_LargeCacheStress_4096_Clipmap");
	return LoadObject<UMHShadowDataAsset>(nullptr, FallbackPath);
}

UMHShadowDataAsset* FMHShadowBenchmarkRunner::LoadRealClipmapDataAsset() const
{
	const TCHAR* FinalPath = TEXT("/Game/MHShadow/Baked/MHShadowData_LightmassDual_LargeCacheStress_RealClipmap.MHShadowData_LightmassDual_LargeCacheStress_RealClipmap");
	return LoadObject<UMHShadowDataAsset>(nullptr, FinalPath);
}

UMHShadowDataAsset* FMHShadowBenchmarkRunner::LoadActiveBenchmarkDataAsset() const
{
	return ActiveProfileName == TEXT("RealClipmapRegression") || ActiveProfileName == TEXT("CacheStrategyRegression") || ActiveProfileName == TEXT("QualityRegression") || ActiveProfileName == TEXT("SoftShadowRegression")
		? LoadRealClipmapDataAsset()
		: LoadLargeCacheStressDataAsset();
}

bool FMHShadowBenchmarkRunner::ConfigureShadowProvider(UWorld& World, const FCaptureSpec& Capture)
{
	const EShadowProviderMode ProviderMode = Capture.ProviderMode;
	if (ProviderMode == EShadowProviderMode::Default)
	{
		return true;
	}

	UMHShadowDataAsset* StressAsset = LoadActiveBenchmarkDataAsset();
	if (ProviderMode == EShadowProviderMode::Monolithic && (!StressAsset || !StressAsset->IsValidForRendering()))
	{
		UE_LOG(LogMHShadowBenchmark, Error, TEXT("CellProviderRegression monolithic route needs a valid LargeCacheStress monolithic asset."));
		return false;
	}

	int32 MHComponentCount = 0;
	int32 WorldComponentCount = 0;
	int32 DisabledCellCount = 0;
	int32 ExpectedUnavailablePages = 0;
	TArray<UMHShadowWorldComponent*> BenchmarkWorldComponentsToRegister;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor)
		{
			continue;
		}

		TArray<UMHShadowComponent*> MHComponents;
		Actor->GetComponents<UMHShadowComponent>(MHComponents);
		for (UMHShadowComponent* MHComponent : MHComponents)
		{
			if (!MHComponent)
			{
				continue;
			}

			MHComponent->UnregisterShadowData();
			if (ProviderMode == EShadowProviderMode::Monolithic)
			{
				MHComponent->ShadowData = StressAsset;
				MHComponent->RegisterShadowData();
			}
			else
			{
				MHComponent->ShadowData = nullptr;
			}
			++MHComponentCount;
		}

		TArray<UMHShadowCellComponent*> CellComponents;
		Actor->GetComponents<UMHShadowCellComponent>(CellComponents);
		for (UMHShadowCellComponent* CellComponent : CellComponents)
		{
			if (!CellComponent)
			{
				continue;
			}

			bool bCellEnabled = ProviderMode == EShadowProviderMode::CellProvider;
			if (bCellEnabled && Capture.CellDisableModulo > 0 && CellComponent->CellData)
			{
				const int32 CellIndex = CellComponent->CellData->CellIndex;
				const int32 NormalizedRemainder = ((Capture.CellDisableRemainder % Capture.CellDisableModulo) + Capture.CellDisableModulo) % Capture.CellDisableModulo;
				const int32 CellRemainder = CellIndex >= 0 ? CellIndex % Capture.CellDisableModulo : INDEX_NONE;
				if (CellIndex >= 0 && CellRemainder == NormalizedRemainder)
				{
					bCellEnabled = false;
					++DisabledCellCount;
					ExpectedUnavailablePages += CellComponent->CellData->GlobalClipmapTileIndices.Num();
				}
			}
			CellComponent->bAutoRegisterCell = bCellEnabled;
		}

		TArray<UMHShadowWorldComponent*> WorldComponents;
		Actor->GetComponents<UMHShadowWorldComponent>(WorldComponents);
		for (UMHShadowWorldComponent* WorldComponent : WorldComponents)
		{
			if (!WorldComponent)
			{
				continue;
			}

			WorldComponent->UnregisterShadowData();
			if (ProviderMode == EShadowProviderMode::Monolithic)
			{
				WorldComponent->bAutoRegisterWorld = false;
			}
			else
			{
				WorldComponent->bAutoRegisterWorld = true;
				BenchmarkWorldComponentsToRegister.Add(WorldComponent);
			}
			++WorldComponentCount;
		}
	}

	if (ProviderMode == EShadowProviderMode::CellProvider)
	{
		for (UMHShadowWorldComponent* WorldComponent : BenchmarkWorldComponentsToRegister)
		{
			if (WorldComponent)
			{
				WorldComponent->RegisterShadowData();
			}
		}
	}

	if (ProviderMode == EShadowProviderMode::Monolithic && MHComponentCount == 0)
	{
		UE_LOG(LogMHShadowBenchmark, Error, TEXT("CellProviderRegression monolithic route found no UMHShadowComponent in map %s."), *World.GetPathName());
		return false;
	}
	if (ProviderMode == EShadowProviderMode::CellProvider && WorldComponentCount == 0)
	{
		UE_LOG(LogMHShadowBenchmark, Error, TEXT("CellProviderRegression cell route found no UMHShadowWorldComponent in map %s."), *World.GetPathName());
		return false;
	}

	ProviderRouteStatsByCapture.FindOrAdd(Capture.Name) = { DisabledCellCount, ExpectedUnavailablePages };
	UE_LOG(LogMHShadowBenchmark, Display, TEXT("CellProviderRegression provider mode=%s mhComponents=%d worldComponents=%d disabledCells=%d expectedUnavailablePages=%d"),
		ProviderMode == EShadowProviderMode::Monolithic ? TEXT("Monolithic") : TEXT("CellProvider"),
		MHComponentCount,
		WorldComponentCount,
		DisabledCellCount,
		ExpectedUnavailablePages);
	return true;
}

void FMHShadowBenchmarkRunner::Exec(UWorld* World, const FString& Command) const
{
	if (GEngine && World)
	{
		GEngine->Exec(World, *Command);
	}
}

UWorld* FMHShadowBenchmarkRunner::GetBenchmarkWorld() const
{
	if (!GEngine)
	{
		return nullptr;
	}

	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World && (Context.WorldType == EWorldType::PIE || Context.WorldType == EWorldType::Game))
		{
			return World;
		}
	}

	return nullptr;
}

bool FMHShadowBenchmarkRunner::CaptureViewport(const FString& Filename, FIntPoint& OutSize, TArray<float>& OutLuma) const
{
	if (!GEngine || !GEngine->GameViewport || !GEngine->GameViewport->Viewport)
	{
		return false;
	}

	FViewport* Viewport = GEngine->GameViewport->Viewport;
	OutSize = Viewport->GetSizeXY();
	if (OutSize.X <= 0 || OutSize.Y <= 0)
	{
		return false;
	}

	TArray<FColor> Pixels;
	FReadSurfaceDataFlags ReadFlags(RCM_UNorm, CubeFace_MAX);
	ReadFlags.SetLinearToGamma(false);
	if (!Viewport->ReadPixels(Pixels, ReadFlags))
	{
		return false;
	}

	if (Pixels.Num() != OutSize.X * OutSize.Y)
	{
		return false;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	FImageView Image(Pixels.GetData(), OutSize.X, OutSize.Y, EGammaSpace::sRGB);
	if (!FImageUtils::SaveImageByExtension(*Filename, Image))
	{
		UE_LOG(LogMHShadowBenchmark, Error, TEXT("Failed to save screenshot: %s"), *Filename);
		return false;
	}

	OutLuma.SetNumUninitialized(Pixels.Num());
	for (int32 Index = 0; Index < Pixels.Num(); ++Index)
	{
		OutLuma[Index] = Luminance01(Pixels[Index]);
	}

	return true;
}

void FMHShadowBenchmarkRunner::RecordCapture(const FBenchmarkStep& Step, const FString& Filename, const FIntPoint& Size, const TArray<float>& Luma)
{
	const FCameraSpec& Camera = Cameras[Step.CameraIndex];
	const FCaptureSpec& Capture = Captures[Step.CaptureIndex];

	double LumaSum = 0.0;
	double MinLuma = Luma.Num() > 0 ? 1.0 : 0.0;
	double MaxLuma = 0.0;
	int32 NonBlackPixels = 0;
	int32 NearWhitePixels = 0;
	int32 NearBlackPixels = 0;
	for (const float Value : Luma)
	{
		LumaSum += Value;
		MinLuma = FMath::Min(MinLuma, double(Value));
		MaxLuma = FMath::Max(MaxLuma, double(Value));
		if (Value > KINDA_SMALL_NUMBER)
		{
			++NonBlackPixels;
		}
		if (Value >= 0.98f)
		{
			++NearWhitePixels;
		}
		if (Value <= 0.02f)
		{
			++NearBlackPixels;
		}
	}
	const double MeanLuma = Luma.Num() > 0 ? LumaSum / double(Luma.Num()) : 0.0;
	const double NonBlackPercent = Luma.Num() > 0 ? 100.0 * double(NonBlackPixels) / double(Luma.Num()) : 0.0;
	const double NearWhitePercent = Luma.Num() > 0 ? 100.0 * double(NearWhitePixels) / double(Luma.Num()) : 0.0;
	const double NearBlackPercent = Luma.Num() > 0 ? 100.0 * double(NearBlackPixels) / double(Luma.Num()) : 0.0;
	if (MaxLuma <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogMHShadowBenchmark, Warning, TEXT("Benchmark capture is fully black: camera=%s route=%s file=%s"),
			*Camera.Name,
			*Capture.Name,
			*Filename);
	}

	CaptureRows.Add({
		Camera.Name,
		Capture.Name,
		Filename,
		Size,
		FPlatformTime::Seconds() - BenchmarkStartTime,
		MeanLuma,
		MinLuma,
		MaxLuma,
		NonBlackPercent,
		MeanLuma,
		MinLuma,
		MaxLuma,
		NearWhitePercent,
		NearBlackPercent,
		Capture.Notes
	});

	if (IsHardShadowSanityCapture(Capture))
	{
		FString Details;
		const FString Status = MakeHardShadowSanityStatus(Capture, MinLuma, MaxLuma, NearWhitePercent, NearBlackPercent, Details);
		HardShadowSanityRows.Add({
			Camera.Name,
			Capture.Name,
			Filename,
			MeanLuma,
			MinLuma,
			MaxLuma,
			NearWhitePercent,
			NearBlackPercent,
			Status,
			Details
		});
		if (Status != TEXT("OK") && Status != TEXT("DefaultBiasStillHasShadow"))
		{
			UE_LOG(LogMHShadowBenchmark, Warning, TEXT("Hard-shadow sanity warning: camera=%s route=%s status=%s %s"),
				*Camera.Name,
				*Capture.Name,
				*Status,
				*Details);
		}
	}

	const FString CaptureKey = MakeCaptureKey(Camera.Name, Capture.Name);
	CaptureLumaByKey.Add(CaptureKey, Luma);
	CaptureSizeByKey.Add(CaptureKey, Size);
	RecordRuntimeStatsForCapture(Camera, Capture, Filename);

	if (Capture.bAtlasBaseline)
	{
		AtlasBaselineByCamera.Add(Camera.Name, Luma);
		AtlasSizeByCamera.Add(Camera.Name, Size);
	}

	if (Capture.Name == TEXT("Clipmap"))
	{
		ClipmapFirstByCamera.Add(Camera.Name, Luma);
		ClipmapSizeByCamera.Add(Camera.Name, Size);
	}

	if (Capture.bCompareToAtlas)
	{
		const TArray<float>* Baseline = AtlasBaselineByCamera.Find(Camera.Name);
		const FIntPoint* BaselineSize = AtlasSizeByCamera.Find(Camera.Name);
		if (Baseline && BaselineSize && *BaselineSize == Size && Baseline->Num() == Luma.Num())
		{
			DiffRows.Add(MakeDiffRow(Camera.Name, Capture.Name, TEXT("AtlasBaseline"), Size, Luma, *Baseline));
		}
		else
		{
			UE_LOG(LogMHShadowBenchmark, Warning, TEXT("No compatible atlas baseline for %s / %s."), *Camera.Name, *Capture.Name);
		}
	}

	if (Capture.bClipmapStabilityRepeat)
	{
		const FString BaselineName = Capture.StabilityBaselineName.IsEmpty() ? TEXT("Clipmap") : Capture.StabilityBaselineName;
		const TArray<float>* First = CaptureLumaByKey.Find(MakeCaptureKey(Camera.Name, BaselineName));
		const FIntPoint* FirstSize = CaptureSizeByKey.Find(MakeCaptureKey(Camera.Name, BaselineName));
		if (First && FirstSize && *FirstSize == Size && First->Num() == Luma.Num())
		{
			StabilityRows.Add(MakeDiffRow(Camera.Name, Capture.Name, BaselineName, Size, Luma, *First));
		}
		else
		{
			UE_LOG(LogMHShadowBenchmark, Warning, TEXT("No compatible baseline capture for stability test: %s / %s."), *Camera.Name, *BaselineName);
		}
	}
}

int32 FMHShadowBenchmarkRunner::InferCaptureSource(const FCaptureSpec& Capture) const
{
	for (int32 CommandIndex = Capture.Commands.Num() - 1; CommandIndex >= 0; --CommandIndex)
	{
		const FString& Command = Capture.Commands[CommandIndex];
		const FString Prefix = TEXT("r.Shadow.MHStatic.Source");
		if (Command.StartsWith(Prefix))
		{
			FString ValueString = Command.RightChop(Prefix.Len()).TrimStartAndEnd();
			return FCString::Atoi(*ValueString);
		}
	}
	return INDEX_NONE;
}

FString FMHShadowBenchmarkRunner::MakeCaptureKey(const FString& Camera, const FString& Source) const
{
	return Camera + TEXT("\n") + Source;
}

uint64 FMHShadowBenchmarkRunner::GetLatestRuntimeStatsFrame() const
{
	TArray<FRuntimeSummaryRow> SummaryRows;
	TArray<FRuntimeLevelRow> LevelRows;
	if (!LoadRuntimeStats(SummaryRows, LevelRows) || SummaryRows.Num() == 0)
	{
		return 0;
	}
	return SummaryRows.Last().Frame;
}

bool FMHShadowBenchmarkRunner::LoadRuntimeStats(TArray<FRuntimeSummaryRow>& OutSummaryRows, TArray<FRuntimeLevelRow>& OutLevelRows) const
{
	OutSummaryRows.Reset();
	OutLevelRows.Reset();

	FString SummaryPath;
	if (FindNewestRuntimeStatsFile(TEXT("MHStaticShadow_RuntimeSummary_"), SummaryPath))
	{
		TArray<FString> Lines;
		if (FFileHelper::LoadFileToStringArray(Lines, *SummaryPath))
		{
			for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
			{
				const TArray<FString> Fields = ParseCsvFields(Lines[LineIndex]);
				if (Fields.Num() < 23)
				{
					continue;
				}

				FRuntimeSummaryRow Row;
				Row.bValid = true;
				Row.Frame = ToUInt64(Fields, 0);
				Row.Source = ToInt(Fields, 3);
				Row.RequestedPages = ToInt(Fields, 4);
				Row.ResidentRequestedPages = ToInt(Fields, 5);
				Row.MissRequestedPages = ToInt(Fields, 6);
				Row.FallbackPossiblePages = ToInt(Fields, 7);
				Row.Uploads = ToInt(Fields, 8);
				Row.Evictions = ToInt(Fields, 9);
				Row.MappedPages = ToInt(Fields, 10);
				Row.VirtualPages = ToInt(Fields, 11);
				Row.PhysicalPages = ToInt(Fields, 12);
				Row.CacheHitRate = ToDouble(Fields, 13);
				Row.MissRate = ToDouble(Fields, 14);
				Row.FallbackRate = ToDouble(Fields, 15);
				Row.CompressionRatio = ToDouble(Fields, 18);
				Row.PhysicalAtlasMemoryRatio = ToDouble(Fields, 21);
				Row.ResidentPageRatio = ToDouble(Fields, 22);
				Row.ProviderMemoryBytes = ToUInt64(Fields, 23);
				Row.LargestProviderCellBytes = ToUInt64(Fields, 24);
				Row.ProviderLoadedCells = ToInt(Fields, 25);
				Row.ProviderUnavailablePages = ToInt(Fields, 26);
				Row.RequestedPixels = ToUInt64(Fields, 27);
				Row.ResidentRequestedPixels = ToUInt64(Fields, 28);
				Row.MissRequestedPixels = ToUInt64(Fields, 29);
				Row.PixelWeightedHitRate = ToDouble(Fields, 30);
				Row.PixelWeightedMissRate = ToDouble(Fields, 31);
				OutSummaryRows.Add(Row);
			}
		}
	}

	FString PerLevelPath;
	if (FindNewestRuntimeStatsFile(TEXT("MHStaticShadow_RuntimePerLevel_"), PerLevelPath))
	{
		TArray<FString> Lines;
		if (FFileHelper::LoadFileToStringArray(Lines, *PerLevelPath))
		{
			for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
			{
				const TArray<FString> Fields = ParseCsvFields(Lines[LineIndex]);
				if (Fields.Num() < 12)
				{
					continue;
				}

				FRuntimeLevelRow Row;
				Row.bValid = true;
				Row.Frame = ToUInt64(Fields, 0);
				Row.Source = ToInt(Fields, 2);
				Row.Level = ToInt(Fields, 3);
				Row.TotalPages = ToInt(Fields, 4);
				Row.RequestedPages = ToInt(Fields, 5);
				Row.ResidentRequestedPages = ToInt(Fields, 6);
				Row.MissRequestedPages = ToInt(Fields, 7);
				Row.FallbackPossiblePages = ToInt(Fields, 8);
				Row.ResidentTotalPages = ToInt(Fields, 9);
				Row.Uploads = ToInt(Fields, 10);
				Row.Evictions = ToInt(Fields, 11);
				Row.RequestedPixels = ToUInt64(Fields, 12);
				Row.ResidentRequestedPixels = ToUInt64(Fields, 13);
				Row.MissRequestedPixels = ToUInt64(Fields, 14);
				OutLevelRows.Add(Row);
			}
		}
	}

	return OutSummaryRows.Num() > 0 || OutLevelRows.Num() > 0;
}

bool FMHShadowBenchmarkRunner::FindLatestRuntimeStatsForSource(int32 Source, FRuntimeSummaryRow& OutSummary, TArray<FRuntimeLevelRow>& OutLevels) const
{
	OutSummary = FRuntimeSummaryRow();
	OutLevels.Reset();

	TArray<FRuntimeSummaryRow> SummaryRows;
	TArray<FRuntimeLevelRow> LevelRows;
	if (!LoadRuntimeStats(SummaryRows, LevelRows))
	{
		return false;
	}

	for (int32 RowIndex = SummaryRows.Num() - 1; RowIndex >= 0; --RowIndex)
	{
		if (Source == INDEX_NONE || SummaryRows[RowIndex].Source == Source)
		{
			OutSummary = SummaryRows[RowIndex];
			break;
		}
	}

	if (!OutSummary.bValid)
	{
		return false;
	}

	for (const FRuntimeLevelRow& LevelRow : LevelRows)
	{
		if (LevelRow.Frame == OutSummary.Frame && LevelRow.Source == OutSummary.Source)
		{
			OutLevels.Add(LevelRow);
		}
	}
	return true;
}

void FMHShadowBenchmarkRunner::RecordRuntimeStatsForCapture(const FCameraSpec& Camera, const FCaptureSpec& Capture, const FString& Filename)
{
	const int32 Source = InferCaptureSource(Capture);
	if (Source == INDEX_NONE)
	{
		return;
	}

	FRuntimeSummaryRow Summary;
	TArray<FRuntimeLevelRow> Levels;
	if (!FindLatestRuntimeStatsForSource(Source, Summary, Levels))
	{
		return;
	}

	TArray<FRuntimeSummaryRow> SummaryRows;
	TArray<FRuntimeLevelRow> IgnoredLevelRows;
	LoadRuntimeStats(SummaryRows, IgnoredLevelRows);
	const uint64 RouteStartFrame = RouteStartFrameByCapture.Contains(Capture.Name) ? RouteStartFrameByCapture[Capture.Name] : 0;
	int32 UploadsSinceRouteStart = 0;
	int32 EvictionsSinceRouteStart = 0;
	for (const FRuntimeSummaryRow& Row : SummaryRows)
	{
		if (Row.Source == Source && Row.Frame > RouteStartFrame && Row.Frame <= Summary.Frame)
		{
			UploadsSinceRouteStart += Row.Uploads;
			EvictionsSinceRouteStart += Row.Evictions;
		}
	}

	FCaptureStatsRow StatsRow;
	StatsRow.Camera = Camera.Name;
	StatsRow.SourceName = Capture.Name;
	StatsRow.Filename = Filename;
	StatsRow.Summary = Summary;
	StatsRow.UploadsSinceRouteStart = UploadsSinceRouteStart;
	StatsRow.EvictionsSinceRouteStart = EvictionsSinceRouteStart;
	if (const FProviderRouteStats* ProviderStats = ProviderRouteStatsByCapture.Find(Capture.Name))
	{
		StatsRow.DisabledCells = ProviderStats->DisabledCells;
		StatsRow.ExpectedUnavailablePages = ProviderStats->ExpectedUnavailablePages;
	}
	StatsRow.Levels = MoveTemp(Levels);
	CaptureStatsRows.Add(MoveTemp(StatsRow));
}

FMHShadowBenchmarkRunner::FDiffRow FMHShadowBenchmarkRunner::MakeDiffRow(const FString& Camera, const FString& Source, const FString& Baseline, const FIntPoint& Size, const TArray<float>& A, const TArray<float>& B) const
{
	FDiffRow Row;
	Row.Camera = Camera;
	Row.Source = Source;
	Row.Baseline = Baseline;
	Row.Size = Size;

	double SumAbs = 0.0;
	double SumSq = 0.0;
	double MaxAbs = 0.0;
	int32 MismatchPixels = 0;

	const int32 Count = FMath::Min(A.Num(), B.Num());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const double AbsDiff = FMath::Abs(double(A[Index]) - double(B[Index]));
		SumAbs += AbsDiff;
		SumSq += AbsDiff * AbsDiff;
		MaxAbs = FMath::Max(MaxAbs, AbsDiff);
		if (AbsDiff > DiffThreshold)
		{
			++MismatchPixels;
		}
	}

	if (Count > 0)
	{
		Row.MeanAbs = SumAbs / double(Count);
		Row.RMSE = FMath::Sqrt(SumSq / double(Count));
		Row.MaxAbs = MaxAbs;
		Row.MismatchPixels = MismatchPixels;
		Row.MismatchPercent = 100.0 * double(MismatchPixels) / double(Count);
	}

	return Row;
}

void FMHShadowBenchmarkRunner::WriteOutputs() const
{
	TArray<FString> SummaryLines;
	SummaryLines.Add(TEXT("Camera,Source,Filename,Width,Height,TimeSeconds,MeanLuma,MinLuma,MaxLuma,NonBlackPercent,MeanVisibility,MinVisibility,MaxVisibility,NearWhitePercent,NearBlackPercent,Notes"));
	for (const FCaptureRow& Row : CaptureRows)
	{
		SummaryLines.Add(FString::Printf(TEXT("%s,%s,%s,%d,%d,%.3f,%.8f,%.8f,%.8f,%.5f,%.8f,%.8f,%.8f,%.5f,%.5f,%s"),
			*CsvEscape(Row.Camera),
			*CsvEscape(Row.Source),
			*CsvEscape(Row.Filename),
			Row.Size.X,
			Row.Size.Y,
			Row.TimeSeconds,
			Row.MeanLuma,
			Row.MinLuma,
			Row.MaxLuma,
			Row.NonBlackPercent,
			Row.MeanVisibility,
			Row.MinVisibility,
			Row.MaxVisibility,
			Row.NearWhitePercent,
			Row.NearBlackPercent,
			*CsvEscape(Row.Notes)));
	}
	FFileHelper::SaveStringArrayToFile(SummaryLines, *FPaths::Combine(OutputDir, TEXT("BenchmarkSummary.csv")));

	TArray<FString> RouteSummaryLines;
	RouteSummaryLines.Add(TEXT("Source,SettleFrames,ResetCacheAtRouteStart,CompareToAtlas,StabilityRepeat,CVarSource,Mode,Debug,RestoredEnablePCF,RestoredDepthBiasScale,RestoredDepthBiasAdd,RestoredBiasMode,RestoredBiasWorldUnits,RestoredSlopeBiasScale,RestoredSlopeBiasClamp,RestoredForceScreenSlope,RestoredFilterBiasScale,RestoredFilterMode,RestoredPCFKernel,RestoredPCFSamplePolicy,RestoredPCFRadiusTexels,SoftMode,SoftBlockerSamples,SoftFilterSamples,SoftBlockerRadiusTexels,SoftMinRadiusTexels,SoftMaxRadiusTexels,SoftPenumbraScale,SoftTransitionScale,SoftSourceAngleScale,CachePagesX,CachePagesY,MaxUploads,CachePriorityMode,CacheLevelPriorityScale,CacheResidencyBoost,CacheEvictRequested,CacheEvictHysteresis,CachePrefetchRadius,CachePrefetchBudget,LevelCount,Level0Distance,DistanceScale,FineLevelBias,FallbackMaxCoarserLevels,SingleLevelUseBaseData,CellDisableModulo,CellDisableRemainder,Commands,Notes"));
	for (const FCaptureSpec& Capture : Captures)
	{
		TArray<FString> Fields;
		Fields.Reserve(50);
		Fields.Add(CsvEscape(Capture.Name));
		Fields.Add(FString::FromInt(Capture.SettleFrames));
		Fields.Add(FString::FromInt(Capture.bResetCacheAtRouteStart ? 1 : 0));
		Fields.Add(FString::FromInt(Capture.bCompareToAtlas ? 1 : 0));
		Fields.Add(FString::FromInt(Capture.bClipmapStabilityRepeat ? 1 : 0));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Source"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Mode"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Debug"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.EnablePCF"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.BiasMode"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.BiasWorldUnits"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.SlopeBiasScale"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.SlopeBiasClamp"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.ForceScreenSlope"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.FilterBiasScale"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.FilterMode"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.PCFKernel"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.PCFSamplePolicy"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.PCFRadiusTexels"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.Mode"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.BlockerSamples"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.FilterSamples"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.BlockerRadiusTexels"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.MinRadiusTexels"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.MaxRadiusTexels"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.PenumbraScale"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.TransitionScale"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Restored.Soft.SourceAngleScale"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.PriorityMode"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.LevelPriorityScale"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.ResidencyBoost"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.EvictRequested"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.EvictHysteresis"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.PrefetchRadius"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Cache.PrefetchBudget"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Clipmap.LevelCount"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels"))));
		Fields.Add(CsvEscape(FindCommandValue(Capture.Commands, TEXT("r.Shadow.MHStatic.Clipmap.SingleLevelUseBaseData"))));
		Fields.Add(FString::FromInt(Capture.CellDisableModulo));
		Fields.Add(FString::FromInt(Capture.CellDisableRemainder));
		Fields.Add(CsvEscape(JoinCommandsForCsv(Capture.Commands)));
		Fields.Add(CsvEscape(Capture.Notes));
		RouteSummaryLines.Add(FString::Join(Fields, TEXT(",")));
	}
	FFileHelper::SaveStringArrayToFile(RouteSummaryLines, *FPaths::Combine(OutputDir, TEXT("BenchmarkRouteSummary.csv")));

	TArray<FString> SanityLines;
	SanityLines.Add(TEXT("Camera,Source,Filename,MeanVisibility,MinVisibility,MaxVisibility,NearWhitePercent,NearBlackPercent,Status,Details"));
	for (const FHardShadowSanityRow& Row : HardShadowSanityRows)
	{
		SanityLines.Add(FString::Printf(TEXT("%s,%s,%s,%.8f,%.8f,%.8f,%.5f,%.5f,%s,%s"),
			*CsvEscape(Row.Camera),
			*CsvEscape(Row.Source),
			*CsvEscape(Row.Filename),
			Row.MeanVisibility,
			Row.MinVisibility,
			Row.MaxVisibility,
			Row.NearWhitePercent,
			Row.NearBlackPercent,
			*CsvEscape(Row.Status),
			*CsvEscape(Row.Details)));
	}
	for (const FCameraSpec& Camera : Cameras)
	{
		const FString ZeroKey = MakeCaptureKey(Camera.Name, TEXT("Source6_ZeroRestoredBias_HardBaseline"));
		const FString TunedKey = MakeCaptureKey(Camera.Name, TEXT("Source6_TunedBias_HardBaseline"));
		const FString DefaultKey = MakeCaptureKey(Camera.Name, TEXT("Source6_DefaultRestoredBias_SuppressionCheck"));
		const TArray<float>* ZeroLuma = CaptureLumaByKey.Find(ZeroKey);
		const TArray<float>* TunedLuma = CaptureLumaByKey.Find(TunedKey);
		const TArray<float>* DefaultLuma = CaptureLumaByKey.Find(DefaultKey);
		const FIntPoint* ZeroSize = CaptureSizeByKey.Find(ZeroKey);
		const FIntPoint* TunedSize = CaptureSizeByKey.Find(TunedKey);
		const FIntPoint* DefaultSize = CaptureSizeByKey.Find(DefaultKey);
		auto AppendBiasDiffWarning = [this, &SanityLines, &Camera](const FString& SourceName, const FString& BaselineName, const FIntPoint& Size, const TArray<float>& SourceLuma, const TArray<float>& BaselineLuma, const TCHAR* Status, const TCHAR* DetailPrefix)
		{
			const FDiffRow BiasDiff = MakeDiffRow(Camera.Name, SourceName, BaselineName, Size, SourceLuma, BaselineLuma);
			if (BiasDiff.MeanAbs > 0.02 || BiasDiff.MismatchPercent > 5.0)
			{
				const FString Details = FString::Printf(TEXT("%s meanAbs=%.6f mismatch=%.5f maxAbs=%.6f"), DetailPrefix, BiasDiff.MeanAbs, BiasDiff.MismatchPercent, BiasDiff.MaxAbs);
				SanityLines.Add(FString::Printf(TEXT("%s,%s,%s,%.8f,%.8f,%.8f,%.5f,%.5f,%s,%s"),
					*CsvEscape(Camera.Name),
					*CsvEscape(SourceName),
					*CsvEscape(FString(TEXT("-"))),
					0.0,
					0.0,
					0.0,
					0.0,
					0.0,
					*CsvEscape(FString(Status)),
					*CsvEscape(Details)));
			}
		};

		if (DefaultLuma && TunedLuma && DefaultSize && TunedSize && *DefaultSize == *TunedSize && DefaultLuma->Num() == TunedLuma->Num())
		{
			AppendBiasDiffWarning(
				TEXT("Source6_DefaultRestoredBias_SuppressionCheck"),
				TEXT("Source6_TunedBias_HardBaseline"),
				*DefaultSize,
				*DefaultLuma,
				*TunedLuma,
				TEXT("BiasSuppressedShadow"),
				TEXT("default-vs-tuned"));
		}

		if (ZeroLuma && TunedLuma && ZeroSize && TunedSize && *ZeroSize == *TunedSize && ZeroLuma->Num() == TunedLuma->Num())
		{
			AppendBiasDiffWarning(
				TEXT("Source6_ZeroRestoredBias_HardBaseline"),
				TEXT("Source6_TunedBias_HardBaseline"),
				*ZeroSize,
				*ZeroLuma,
				*TunedLuma,
				TEXT("ZeroBiasDiffersFromTuned"),
				TEXT("zero-vs-tuned"));
		}
	}
	FFileHelper::SaveStringArrayToFile(SanityLines, *FPaths::Combine(OutputDir, TEXT("HardShadowSanity.csv")));

	if (ActiveProfileName == TEXT("LightingIntegrationRegression"))
	{
		TArray<FString> LitSanityLines;
		LitSanityLines.Add(TEXT("Camera,Source,Filename,MeanLuma,MinLuma,MaxLuma,LumaRange,NonBlackPercent,MeanVisibility,MinVisibility,MaxVisibility,VisibilityRange,NearWhitePercent,NearBlackPercent,Status,Details"));
		for (const FCaptureRow& Row : CaptureRows)
		{
			const double LumaRange = Row.MaxLuma - Row.MinLuma;
			const double VisibilityRange = Row.MaxVisibility - Row.MinVisibility;
			FString Status = TEXT("OK");
			if (Row.Source.Contains(TEXT("Lit_")))
			{
				if (Row.NonBlackPercent < 1.0 || Row.MaxLuma <= 0.02)
				{
					Status = TEXT("LikelyAllBlackLitImage");
				}
				else if (LumaRange < 0.03)
				{
					Status = TEXT("LowLitDynamicRange");
				}
			}
			else if (Row.Source.Contains(TEXT("Mask_")))
			{
				if (VisibilityRange < 0.10)
				{
					Status = TEXT("LowMaskVisibilityRange");
				}
			}
			else if (Row.Source.Contains(TEXT("UEBaseline")))
			{
				Status = Row.NonBlackPercent < 1.0 ? TEXT("LikelyAllBlackUEBaseline") : TEXT("ReferenceOnly");
			}

			const FString Details = FString::Printf(TEXT("lumaRange=%.6f visibilityRange=%.6f nonBlack=%.3f"), LumaRange, VisibilityRange, Row.NonBlackPercent);
			LitSanityLines.Add(FString::Printf(TEXT("%s,%s,%s,%.8f,%.8f,%.8f,%.8f,%.5f,%.8f,%.8f,%.8f,%.8f,%.5f,%.5f,%s,%s"),
				*CsvEscape(Row.Camera),
				*CsvEscape(Row.Source),
				*CsvEscape(Row.Filename),
				Row.MeanLuma,
				Row.MinLuma,
				Row.MaxLuma,
				LumaRange,
				Row.NonBlackPercent,
				Row.MeanVisibility,
				Row.MinVisibility,
				Row.MaxVisibility,
				VisibilityRange,
				Row.NearWhitePercent,
				Row.NearBlackPercent,
				*CsvEscape(Status),
				*CsvEscape(Details)));
		}

		for (const FCameraSpec& Camera : Cameras)
		{
			const FString MaskKey = MakeCaptureKey(Camera.Name, TEXT("Mask_Mode3_HardDebug"));
			const FString LitKey = MakeCaptureKey(Camera.Name, TEXT("Lit_Mode4_DirectLighting_Hard"));
			const TArray<float>* MaskLuma = CaptureLumaByKey.Find(MaskKey);
			const TArray<float>* LitLuma = CaptureLumaByKey.Find(LitKey);
			const FIntPoint* MaskSize = CaptureSizeByKey.Find(MaskKey);
			const FIntPoint* LitSize = CaptureSizeByKey.Find(LitKey);
			if (MaskLuma && LitLuma && MaskSize && LitSize && *MaskSize == *LitSize && MaskLuma->Num() == LitLuma->Num())
			{
				const FDiffRow ApproxAlignment = MakeDiffRow(Camera.Name, TEXT("Lit_Mode4_DirectLighting_Hard"), TEXT("Mask_Mode3_HardDebug"), *LitSize, *LitLuma, *MaskLuma);
				const FString Details = FString::Printf(TEXT("approxLumaMaskDiff meanAbs=%.6f mismatch=%.5f maxAbs=%.6f; visual edge review still required because lit image includes material lighting"), ApproxAlignment.MeanAbs, ApproxAlignment.MismatchPercent, ApproxAlignment.MaxAbs);
				LitSanityLines.Add(FString::Printf(TEXT("%s,%s,%s,%.8f,%.8f,%.8f,%.8f,%.5f,%.8f,%.8f,%.8f,%.8f,%.5f,%.5f,%s,%s"),
					*CsvEscape(Camera.Name),
					*CsvEscape(TEXT("Mode3Mode4ApproxAlignment")),
					*CsvEscape(TEXT("-")),
					0.0,
					0.0,
					0.0,
					0.0,
					0.0,
					0.0,
					0.0,
					0.0,
					0.0,
					0.0,
					0.0,
					*CsvEscape(TEXT("ApproximateOnly")),
					*CsvEscape(Details)));
			}
		}
		FFileHelper::SaveStringArrayToFile(LitSanityLines, *FPaths::Combine(OutputDir, TEXT("LitIntegrationSanity.csv")));
	}

	TArray<FString> DiffLines;
	DiffLines.Add(TEXT("Camera,Source,Baseline,Width,Height,MeanAbs,RMSE,MaxAbs,MismatchPixels,MismatchPercent"));
	for (const FDiffRow& Row : DiffRows)
	{
		DiffLines.Add(FString::Printf(TEXT("%s,%s,%s,%d,%d,%.8f,%.8f,%.8f,%d,%.5f"),
			*CsvEscape(Row.Camera),
			*CsvEscape(Row.Source),
			*CsvEscape(Row.Baseline),
			Row.Size.X,
			Row.Size.Y,
			Row.MeanAbs,
			Row.RMSE,
			Row.MaxAbs,
			Row.MismatchPixels,
			Row.MismatchPercent));
	}
	FFileHelper::SaveStringArrayToFile(DiffLines, *FPaths::Combine(OutputDir, TEXT("HardShadowDiff.csv")));

	TArray<FString> StabilityLines;
	StabilityLines.Add(TEXT("Camera,Source,Baseline,Width,Height,MeanAbs,RMSE,MaxAbs,MismatchPixels,MismatchPercent"));
	for (const FDiffRow& Row : StabilityRows)
	{
		StabilityLines.Add(FString::Printf(TEXT("%s,%s,%s,%d,%d,%.8f,%.8f,%.8f,%d,%.5f"),
			*CsvEscape(Row.Camera),
			*CsvEscape(Row.Source),
			*CsvEscape(Row.Baseline),
			Row.Size.X,
			Row.Size.Y,
			Row.MeanAbs,
			Row.RMSE,
			Row.MaxAbs,
			Row.MismatchPixels,
			Row.MismatchPercent));
	}
	FFileHelper::SaveStringArrayToFile(StabilityLines, *FPaths::Combine(OutputDir, TEXT("Stability.csv")));

	TArray<FString> PairwiseLines;
	PairwiseLines.Add(TEXT("Camera,Source,Baseline,Width,Height,MeanAbs,RMSE,MaxAbs,MismatchPixels,MismatchPercent"));
	struct FPairwiseAggregate
	{
		int32 Count = 0;
		double MeanAbsSum = 0.0;
		double RmseSum = 0.0;
		double MaxAbs = 0.0;
		int64 MismatchPixelsSum = 0;
		double MismatchPercentSum = 0.0;
		double MismatchPercentMax = 0.0;
	};
	TMap<FString, FPairwiseAggregate> PairwiseAggregates;
	for (const FPairwiseSpec& Spec : PairwiseSpecs)
	{
		for (const FCameraSpec& Camera : Cameras)
		{
			const FString SourceKey = MakeCaptureKey(Camera.Name, Spec.Source);
			const FString BaselineKey = MakeCaptureKey(Camera.Name, Spec.Baseline);
			const TArray<float>* SourceLuma = CaptureLumaByKey.Find(SourceKey);
			const TArray<float>* BaselineLuma = CaptureLumaByKey.Find(BaselineKey);
			const FIntPoint* SourceSize = CaptureSizeByKey.Find(SourceKey);
			const FIntPoint* BaselineSize = CaptureSizeByKey.Find(BaselineKey);
			if (!SourceLuma || !BaselineLuma || !SourceSize || !BaselineSize || *SourceSize != *BaselineSize || SourceLuma->Num() != BaselineLuma->Num())
			{
				continue;
			}

			const FDiffRow Row = MakeDiffRow(Camera.Name, Spec.Source, Spec.Baseline, *SourceSize, *SourceLuma, *BaselineLuma);
			FPairwiseAggregate& Aggregate = PairwiseAggregates.FindOrAdd(Spec.Source + TEXT("\t") + Spec.Baseline);
			++Aggregate.Count;
			Aggregate.MeanAbsSum += Row.MeanAbs;
			Aggregate.RmseSum += Row.RMSE;
			Aggregate.MaxAbs = FMath::Max(Aggregate.MaxAbs, Row.MaxAbs);
			Aggregate.MismatchPixelsSum += Row.MismatchPixels;
			Aggregate.MismatchPercentSum += Row.MismatchPercent;
			Aggregate.MismatchPercentMax = FMath::Max(Aggregate.MismatchPercentMax, Row.MismatchPercent);
			PairwiseLines.Add(FString::Printf(TEXT("%s,%s,%s,%d,%d,%.8f,%.8f,%.8f,%d,%.5f"),
				*CsvEscape(Row.Camera),
				*CsvEscape(Row.Source),
				*CsvEscape(Row.Baseline),
				Row.Size.X,
				Row.Size.Y,
				Row.MeanAbs,
				Row.RMSE,
				Row.MaxAbs,
				Row.MismatchPixels,
				Row.MismatchPercent));
		}
	}
	FFileHelper::SaveStringArrayToFile(PairwiseLines, *FPaths::Combine(OutputDir, TEXT("PairwiseDiff.csv")));

	TArray<FString> PairwiseSummaryLines;
	PairwiseSummaryLines.Add(TEXT("Source,Baseline,Count,AvgMeanAbs,AvgRMSE,MaxAbs,TotalMismatchPixels,AvgMismatchPercent,MaxMismatchPercent"));
	for (const FPairwiseSpec& Spec : PairwiseSpecs)
	{
		const FPairwiseAggregate* Aggregate = PairwiseAggregates.Find(Spec.Source + TEXT("\t") + Spec.Baseline);
		if (!Aggregate || Aggregate->Count <= 0)
		{
			continue;
		}

		PairwiseSummaryLines.Add(FString::Printf(TEXT("%s,%s,%d,%.8f,%.8f,%.8f,%lld,%.5f,%.5f"),
			*CsvEscape(Spec.Source),
			*CsvEscape(Spec.Baseline),
			Aggregate->Count,
			Aggregate->MeanAbsSum / double(Aggregate->Count),
			Aggregate->RmseSum / double(Aggregate->Count),
			Aggregate->MaxAbs,
			Aggregate->MismatchPixelsSum,
			Aggregate->MismatchPercentSum / double(Aggregate->Count),
			Aggregate->MismatchPercentMax));
	}
	FFileHelper::SaveStringArrayToFile(PairwiseSummaryLines, *FPaths::Combine(OutputDir, TEXT("PairwiseSummary.csv")));
	if (ActiveProfileName == TEXT("QualityRegression"))
	{
		FFileHelper::SaveStringArrayToFile(PairwiseSummaryLines, *FPaths::Combine(OutputDir, TEXT("QualityPairwiseSummary.csv")));
		FFileHelper::SaveStringArrayToFile(SanityLines, *FPaths::Combine(OutputDir, TEXT("ShadowAcneSanity.csv")));

		TArray<FString> EdgeSoftnessLines;
		EdgeSoftnessLines.Add(TEXT("Camera,Source,Width,Height,MidVisibilityPercent,StrongEdgePercent,MeanGradient"));
		for (const FCaptureRow& Row : CaptureRows)
		{
			const FString CaptureKey = MakeCaptureKey(Row.Camera, Row.Source);
			const TArray<float>* Luma = CaptureLumaByKey.Find(CaptureKey);
			const FIntPoint* Size = CaptureSizeByKey.Find(CaptureKey);
			if (!Luma || !Size || Size->X <= 1 || Size->Y <= 1 || Luma->Num() != Size->X * Size->Y)
			{
				continue;
			}

			int32 MidPixels = 0;
			int32 StrongEdgeSamples = 0;
			double GradientSum = 0.0;
			int32 GradientSamples = 0;
			for (int32 Y = 0; Y < Size->Y; ++Y)
			{
				for (int32 X = 0; X < Size->X; ++X)
				{
					const int32 Index = Y * Size->X + X;
					const float Value = (*Luma)[Index];
					if (Value > 0.15f && Value < 0.85f)
					{
						++MidPixels;
					}
					if (X + 1 < Size->X)
					{
						const double Gradient = FMath::Abs(double((*Luma)[Index + 1] - Value));
						GradientSum += Gradient;
						++GradientSamples;
						if (Gradient > 0.05)
						{
							++StrongEdgeSamples;
						}
					}
					if (Y + 1 < Size->Y)
					{
						const double Gradient = FMath::Abs(double((*Luma)[Index + Size->X] - Value));
						GradientSum += Gradient;
						++GradientSamples;
						if (Gradient > 0.05)
						{
							++StrongEdgeSamples;
						}
					}
				}
			}

			const double PixelCount = double(Size->X) * double(Size->Y);
			const double MidVisibilityPercent = PixelCount > 0.0 ? 100.0 * double(MidPixels) / PixelCount : 0.0;
			const double StrongEdgePercent = GradientSamples > 0 ? 100.0 * double(StrongEdgeSamples) / double(GradientSamples) : 0.0;
			const double MeanGradient = GradientSamples > 0 ? GradientSum / double(GradientSamples) : 0.0;
			EdgeSoftnessLines.Add(FString::Printf(TEXT("%s,%s,%d,%d,%.5f,%.5f,%.8f"),
				*CsvEscape(Row.Camera),
				*CsvEscape(Row.Source),
				Size->X,
				Size->Y,
				MidVisibilityPercent,
				StrongEdgePercent,
				MeanGradient));
		}
		FFileHelper::SaveStringArrayToFile(EdgeSoftnessLines, *FPaths::Combine(OutputDir, TEXT("EdgeSoftness.csv")));
	}

	TArray<FString> CacheStatsLines;
	CacheStatsLines.Add(TEXT("Camera,Source,Filename,StatsFrame,StatsSource,RequestedPages,ResidentRequestedPages,MissRequestedPages,FallbackPossiblePages,CacheHitRate,MissRate,FallbackRate,RequestedPixels,ResidentRequestedPixels,MissRequestedPixels,PixelWeightedHitRate,PixelWeightedMissRate,Uploads,Evictions,UploadsSinceRouteStart,EvictionsSinceRouteStart,MappedPages,VirtualPages,PhysicalPages,CompressionRatio,PhysicalAtlasMemoryRatio,ResidentPageRatio,ProviderMemoryBytes,LargestProviderCellBytes,ProviderLoadedCells,ProviderUnavailablePages,DisabledCells,ExpectedUnavailablePages"));
	for (const FCaptureStatsRow& Row : CaptureStatsRows)
	{
		CacheStatsLines.Add(FString::Printf(TEXT("%s,%s,%s,%llu,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f,%llu,%llu,%llu,%.6f,%.6f,%d,%d,%d,%d,%d,%d,%d,%.8f,%.8f,%.6f,%llu,%llu,%d,%d,%d,%d"),
			*CsvEscape(Row.Camera),
			*CsvEscape(Row.SourceName),
			*CsvEscape(Row.Filename),
			Row.Summary.Frame,
			Row.Summary.Source,
			Row.Summary.RequestedPages,
			Row.Summary.ResidentRequestedPages,
			Row.Summary.MissRequestedPages,
			Row.Summary.FallbackPossiblePages,
			Row.Summary.CacheHitRate,
			Row.Summary.MissRate,
			Row.Summary.FallbackRate,
			Row.Summary.RequestedPixels,
			Row.Summary.ResidentRequestedPixels,
			Row.Summary.MissRequestedPixels,
			Row.Summary.PixelWeightedHitRate,
			Row.Summary.PixelWeightedMissRate,
			Row.Summary.Uploads,
			Row.Summary.Evictions,
			Row.UploadsSinceRouteStart,
			Row.EvictionsSinceRouteStart,
			Row.Summary.MappedPages,
			Row.Summary.VirtualPages,
			Row.Summary.PhysicalPages,
			Row.Summary.CompressionRatio,
			Row.Summary.PhysicalAtlasMemoryRatio,
			Row.Summary.ResidentPageRatio,
			Row.Summary.ProviderMemoryBytes,
			Row.Summary.LargestProviderCellBytes,
			Row.Summary.ProviderLoadedCells,
			Row.Summary.ProviderUnavailablePages,
			Row.DisabledCells,
			Row.ExpectedUnavailablePages));
	}
	FFileHelper::SaveStringArrayToFile(CacheStatsLines, *FPaths::Combine(OutputDir, TEXT("BenchmarkCacheStats.csv")));

	TArray<FString> PerLevelLines;
	PerLevelLines.Add(TEXT("Camera,Source,Filename,StatsFrame,StatsSource,Level,TotalPages,RequestedPages,ResidentRequestedPages,MissRequestedPages,FallbackPossiblePages,ResidentTotalPages,Uploads,Evictions,RequestedPixels,ResidentRequestedPixels,MissRequestedPixels"));
	for (const FCaptureStatsRow& Row : CaptureStatsRows)
	{
		for (const FRuntimeLevelRow& Level : Row.Levels)
		{
			PerLevelLines.Add(FString::Printf(TEXT("%s,%s,%s,%llu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%llu,%llu,%llu"),
				*CsvEscape(Row.Camera),
				*CsvEscape(Row.SourceName),
				*CsvEscape(Row.Filename),
				Row.Summary.Frame,
				Row.Summary.Source,
				Level.Level,
				Level.TotalPages,
				Level.RequestedPages,
				Level.ResidentRequestedPages,
				Level.MissRequestedPages,
				Level.FallbackPossiblePages,
				Level.ResidentTotalPages,
				Level.Uploads,
				Level.Evictions,
				Level.RequestedPixels,
				Level.ResidentRequestedPixels,
				Level.MissRequestedPixels));
		}
	}
	FFileHelper::SaveStringArrayToFile(PerLevelLines, *FPaths::Combine(OutputDir, TEXT("BenchmarkPerLevelStats.csv")));

	TArray<FString> CommandLines;
	for (const FCaptureSpec& Capture : Captures)
	{
		CommandLines.Add(FString::Printf(TEXT("[%s]"), *Capture.Name));
		CommandLines.Append(Capture.Commands);
		CommandLines.Add(TEXT(""));
	}
	FFileHelper::SaveStringArrayToFile(CommandLines, *FPaths::Combine(OutputDir, TEXT("BenchmarkCommands.txt")));

	WriteReadme();
}

void FMHShadowBenchmarkRunner::WriteReadme() const
{
	FString Readme;
	Readme += TEXT("# MH Static Shadow Benchmark\n\n");
	Readme += TEXT("This folder was generated by `MHShadow.Benchmark.Run`.\n\n");
	Readme += TEXT("## What It Measures\n\n");
	Readme += TEXT("- `AtlasBaseline` is the route marked as the golden hard-shadow baseline for this run; final cache reports use CellProvider all-loaded/all-resident so cache routes are compared against the same data source.\n");
	Readme += TEXT("- `HardShadowDiff.csv` compares RawFront, MHTree, VirtualPageAtlas, LimitedCache, and Clipmap against that baseline.\n");
	Readme += TEXT("- `Stability.csv` compares two same-camera Clipmap captures to catch cache or temporal instability.\n");
	Readme += TEXT("- `PairwiseDiff.csv` compares selected cache/clipmap routes directly. Cache strategy runs compare 8x8/16x16 cache routes against `CellProvider_AllLoaded_AllResident`.\n");
	Readme += TEXT("- `PairwiseSummary.csv` aggregates those pairwise comparisons so threshold checks do not require manual spreadsheet grouping.\n");
	Readme += TEXT("- `BenchmarkRouteSummary.csv` records the CVar strategy behind each route, including cache size, clipmap level selection, and single-level base-data mode.\n");
	Readme += TEXT("- `HardShadowSanity.csv` records visibility range and near-white/near-black ratios so an all-lit or all-shadowed capture cannot silently pass.\n");
	Readme += TEXT("- RealClipmap hard-shadow correctness routes currently use `r.Shadow.MHStatic.Restored.DepthBiasAdd 0.01` as a manually tuned Stage 3 bias; Stage 5 should replace this with level-aware automatic bias.\n");
	Readme += TEXT("- `BenchmarkCacheStats.csv` and `BenchmarkPerLevelStats.csv` attach renderer runtime cache stats to each screenshot.\n");
	Readme += TEXT("- Cache strategy runs include pixel-weighted hit/miss metrics, so a route can improve visual error even when page-count miss remains high.\n");
	Readme += TEXT("- Recommended display cache preset: `16x16 + Priority + Prefetch` (`Cache16x16_DisplayPriorityPrefetch`).\n");
	Readme += TEXT("- Low-memory cache preset: `8x8 + Priority + no Prefetch` (`Cache8x8_LowMemoryPriority`).\n");
	Readme += TEXT("- Quality regression runs add `QualityPairwiseSummary.csv`, `ShadowAcneSanity.csv`, and `EdgeSoftness.csv` for UE-style bias / stable PCF comparisons.\n");
	Readme += TEXT("- Soft-shadow regression runs compare hard, stable PCF, and contact-hardening routes; `Debug 27/28/29` visualize blocker validity, penumbra radius, and soft sample validity.\n");
	Readme += TEXT("- Lighting integration runs add `LitIntegrationSanity.csv` to verify Mode 4 produces a normal lit image while Mode 3 remains the mask/debug baseline.\n");
	Readme += TEXT("- Runtime page/cache statistics are written by the renderer stats system under `Saved/MHShadow/RuntimeStats` when stats CSV is enabled.\n\n");
	Readme += TEXT("## Useful Follow-up Commands\n\n");
	Readme += TEXT("```text\n");
	Readme += TEXT("MHShadow.Benchmark.CaptureCurrent\n");
	Readme += TEXT("MHShadow.Benchmark.RunClipmapRegression\n");
	Readme += TEXT("MHShadow.Benchmark.RunClipmapDegeneration\n");
	Readme += TEXT("MHShadow.Benchmark.RunLargeCacheStress\n");
	Readme += TEXT("MHShadow.Benchmark.RunCellProviderRegression\n");
	Readme += TEXT("MHShadow.Benchmark.RunCacheStrategyRegression\n");
	Readme += TEXT("MHShadow.Benchmark.RunQualityRegression\n");
	Readme += TEXT("MHShadow.Benchmark.RunSoftShadowRegression\n");
	Readme += TEXT("MHShadow.Benchmark.RunLightingIntegrationRegression\n");
	Readme += TEXT("MHShadow.Preset.LitClipmap16\n");
	Readme += TEXT("r.Shadow.MHStatic.Stats.Dump\n");
	Readme += TEXT("stat gpu\n");
	Readme += TEXT("```\n");
	FFileHelper::SaveStringToFile(Readme, *FPaths::Combine(OutputDir, TEXT("README_Benchmark.md")));
}

FString FMHShadowBenchmarkRunner::MakeSafeName(const FString& Name) const
{
	FString Result = Name;
	const TCHAR* InvalidChars = TEXT("\\/:*?\"<>| .");
	for (int32 Index = 0; InvalidChars[Index] != TCHAR('\0'); ++Index)
	{
		Result.ReplaceCharInline(InvalidChars[Index], TCHAR('_'));
	}
	return Result;
}

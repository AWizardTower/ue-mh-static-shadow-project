// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowBenchmarkRunner.h"

#include "Camera/CameraActor.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

DEFINE_LOG_CATEGORY_STATIC(LogMHShadowBenchmark, Log, All);

namespace
{
	constexpr double DiffThreshold = 0.05;

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
}

FMHShadowBenchmarkRunner::FMHShadowBenchmarkRunner() = default;

FMHShadowBenchmarkRunner::~FMHShadowBenchmarkRunner()
{
	StopBenchmark();
}

void FMHShadowBenchmarkRunner::StartBenchmark()
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

	BuildBenchmarkPlan();

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	OutputDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"), TEXT("Benchmark"), Timestamp);
	IFileManager::Get().MakeDirectory(*OutputDir, true);

	CaptureRows.Reset();
	DiffRows.Reset();
	StabilityRows.Reset();
	AtlasBaselineByCamera.Reset();
	ClipmapFirstByCamera.Reset();
	AtlasSizeByCamera.Reset();
	ClipmapSizeByCamera.Reset();

	BenchmarkStartTime = FPlatformTime::Seconds();
	CurrentStepIndex = INDEX_NONE;
	RemainingSettleFrames = 0;
	bRunning = true;

	Exec(World, TEXT("r.Shadow.MHStatic.Stats.Enable 1"));
	Exec(World, TEXT("r.Shadow.MHStatic.Stats.CSV 1"));
	Exec(World, TEXT("r.Shadow.MHStatic.Stats.CSVEveryNFrames 10"));
	Exec(World, TEXT("r.Shadow.MHStatic.Feedback.Enable 1"));
	Exec(World, TEXT("r.Shadow.MHStatic.Cache.Enable 1"));
	Exec(World, TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX 8"));
	Exec(World, TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY 8"));
	Exec(World, TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame 64"));
	Exec(World, TEXT("r.Shadow.MHStatic.Clipmap.LevelCount 3"));
	Exec(World, TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance 300"));
	Exec(World, TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale 1.5"));

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

void FMHShadowBenchmarkRunner::BuildBenchmarkPlan()
{
	Cameras.Reset();
	Captures.Reset();
	Steps.Reset();

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
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.Name = TEXT("MHShadowBenchmarkCamera");
		SpawnParameters.ObjectFlags |= RF_Transient;
		CameraActor = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), CameraSpec.Location, FRotator::ZeroRotator, SpawnParameters);
		BenchmarkCamera = CameraActor;
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
	if (ACameraActor* CameraActor = BenchmarkCamera.Get())
	{
		if (!CameraActor->IsActorBeingDestroyed())
		{
			CameraActor->Destroy();
		}
	}
	BenchmarkCamera.Reset();
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

	CaptureRows.Add({
		Camera.Name,
		Capture.Name,
		Filename,
		Size,
		FPlatformTime::Seconds() - BenchmarkStartTime,
		Capture.Notes
	});

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
		const TArray<float>* First = ClipmapFirstByCamera.Find(Camera.Name);
		const FIntPoint* FirstSize = ClipmapSizeByCamera.Find(Camera.Name);
		if (First && FirstSize && *FirstSize == Size && First->Num() == Luma.Num())
		{
			StabilityRows.Add(MakeDiffRow(Camera.Name, TEXT("ClipmapRepeat"), TEXT("Clipmap"), Size, Luma, *First));
		}
		else
		{
			UE_LOG(LogMHShadowBenchmark, Warning, TEXT("No compatible clipmap first capture for stability test: %s."), *Camera.Name);
		}
	}
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
	SummaryLines.Add(TEXT("Camera,Source,Filename,Width,Height,TimeSeconds,Notes"));
	for (const FCaptureRow& Row : CaptureRows)
	{
		SummaryLines.Add(FString::Printf(TEXT("%s,%s,%s,%d,%d,%.3f,%s"),
			*CsvEscape(Row.Camera),
			*CsvEscape(Row.Source),
			*CsvEscape(Row.Filename),
			Row.Size.X,
			Row.Size.Y,
			Row.TimeSeconds,
			*CsvEscape(Row.Notes)));
	}
	FFileHelper::SaveStringArrayToFile(SummaryLines, *FPaths::Combine(OutputDir, TEXT("BenchmarkSummary.csv")));

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
	Readme += TEXT("- `AtlasBaseline` is `Source=2`, the full restored atlas hard-shadow baseline.\n");
	Readme += TEXT("- `HardShadowDiff.csv` compares RawFront, MHTree, VirtualPageAtlas, LimitedCache, and Clipmap against that baseline.\n");
	Readme += TEXT("- `Stability.csv` compares two same-camera Clipmap captures to catch cache or temporal instability.\n");
	Readme += TEXT("- Runtime page/cache statistics are written by the renderer stats system under `Saved/MHShadow/RuntimeStats` when stats CSV is enabled.\n\n");
	Readme += TEXT("## Useful Follow-up Commands\n\n");
	Readme += TEXT("```text\n");
	Readme += TEXT("MHShadow.Benchmark.CaptureCurrent\n");
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

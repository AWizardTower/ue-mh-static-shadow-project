// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class ACameraActor;
class UWorld;

class FMHShadowBenchmarkRunner
{
public:
	FMHShadowBenchmarkRunner();
	~FMHShadowBenchmarkRunner();

	void StartBenchmark();
	void CaptureCurrent();
	void StopBenchmark();

private:
	struct FCameraSpec
	{
		FString Name;
		FVector Location = FVector::ZeroVector;
		FVector Target = FVector::ZeroVector;
	};

	struct FCaptureSpec
	{
		FString Name;
		TArray<FString> Commands;
		int32 SettleFrames = 6;
		bool bAtlasBaseline = false;
		bool bCompareToAtlas = false;
		bool bClipmapStabilityRepeat = false;
		FString Notes;
	};

	struct FBenchmarkStep
	{
		int32 CameraIndex = INDEX_NONE;
		int32 CaptureIndex = INDEX_NONE;
	};

	struct FCaptureRow
	{
		FString Camera;
		FString Source;
		FString Filename;
		FIntPoint Size = FIntPoint::ZeroValue;
		double TimeSeconds = 0.0;
		FString Notes;
	};

	struct FDiffRow
	{
		FString Camera;
		FString Source;
		FString Baseline;
		double MeanAbs = 0.0;
		double RMSE = 0.0;
		double MaxAbs = 0.0;
		int32 MismatchPixels = 0;
		double MismatchPercent = 0.0;
		FIntPoint Size = FIntPoint::ZeroValue;
	};

	bool Tick(float DeltaTime);
	void BuildBenchmarkPlan();
	void StartNextStep();
	void FinishBenchmark();
	void ApplyStep(const FBenchmarkStep& Step);
	void ApplyCamera(const FCameraSpec& CameraSpec);
	void DestroyBenchmarkCamera();
	void Exec(UWorld* World, const FString& Command) const;
	UWorld* GetBenchmarkWorld() const;
	bool CaptureViewport(const FString& Filename, FIntPoint& OutSize, TArray<float>& OutLuma) const;
	void RecordCapture(const FBenchmarkStep& Step, const FString& Filename, const FIntPoint& Size, const TArray<float>& Luma);
	FDiffRow MakeDiffRow(const FString& Camera, const FString& Source, const FString& Baseline, const FIntPoint& Size, const TArray<float>& A, const TArray<float>& B) const;
	void WriteOutputs() const;
	void WriteReadme() const;
	FString MakeSafeName(const FString& Name) const;

	TArray<FCameraSpec> Cameras;
	TArray<FCaptureSpec> Captures;
	TArray<FBenchmarkStep> Steps;
	TArray<FCaptureRow> CaptureRows;
	TArray<FDiffRow> DiffRows;
	TArray<FDiffRow> StabilityRows;
	TMap<FString, TArray<float>> AtlasBaselineByCamera;
	TMap<FString, TArray<float>> ClipmapFirstByCamera;
	TMap<FString, FIntPoint> AtlasSizeByCamera;
	TMap<FString, FIntPoint> ClipmapSizeByCamera;

	FString OutputDir;
	double BenchmarkStartTime = 0.0;
	int32 CurrentStepIndex = INDEX_NONE;
	int32 RemainingSettleFrames = 0;
	bool bRunning = false;
	FTSTicker::FDelegateHandle TickerHandle;
	TWeakObjectPtr<ACameraActor> BenchmarkCamera;
};

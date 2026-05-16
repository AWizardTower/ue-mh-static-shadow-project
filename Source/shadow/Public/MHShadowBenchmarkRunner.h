// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class ACameraActor;
class UMHShadowDataAsset;
class UWorld;

class FMHShadowBenchmarkRunner
{
public:
	FMHShadowBenchmarkRunner();
	~FMHShadowBenchmarkRunner();

	void StartBenchmark();
	void StartLargeCacheStressBenchmark();
	void StartClipmapRegressionBenchmark();
	void StartClipmapDegenerationBenchmark();
	void StartCellProviderRegressionBenchmark();
	void StartRealClipmapRegressionBenchmark();
	void CaptureCurrent();
	void StopBenchmark();

private:
	enum class EBenchmarkProfile : uint8
	{
		BakeTest,
		LargeCacheStress,
		ClipmapRegression,
		ClipmapDegeneration,
		CellProviderRegression,
		RealClipmapRegression
	};

	struct FCameraSpec
	{
		FString Name;
		FVector Location = FVector::ZeroVector;
		FVector Target = FVector::ZeroVector;
	};

	enum class EShadowProviderMode : uint8
	{
		Default,
		Monolithic,
		CellProvider
	};

	struct FCaptureSpec
	{
		FString Name;
		TArray<FString> Commands;
		int32 SettleFrames = 6;
		bool bAtlasBaseline = false;
		bool bCompareToAtlas = false;
		bool bClipmapStabilityRepeat = false;
		bool bResetCacheAtRouteStart = false;
		FString Notes;
		FString StabilityBaselineName;
		EShadowProviderMode ProviderMode = EShadowProviderMode::Default;
		int32 CellDisableModulo = 0;
		int32 CellDisableRemainder = 0;
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
		double MeanLuma = 0.0;
		double MinLuma = 0.0;
		double MaxLuma = 0.0;
		double NonBlackPercent = 0.0;
		double MeanVisibility = 0.0;
		double MinVisibility = 0.0;
		double MaxVisibility = 0.0;
		double NearWhitePercent = 0.0;
		double NearBlackPercent = 0.0;
		FString Notes;
	};

	struct FHardShadowSanityRow
	{
		FString Camera;
		FString Source;
		FString Filename;
		double MeanVisibility = 0.0;
		double MinVisibility = 0.0;
		double MaxVisibility = 0.0;
		double NearWhitePercent = 0.0;
		double NearBlackPercent = 0.0;
		FString Status;
		FString Details;
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

	struct FRuntimeSummaryRow
	{
		bool bValid = false;
		uint64 Frame = 0;
		int32 Source = 0;
		int32 RequestedPages = 0;
		int32 ResidentRequestedPages = 0;
		int32 MissRequestedPages = 0;
		int32 FallbackPossiblePages = 0;
		int32 Uploads = 0;
		int32 Evictions = 0;
		int32 MappedPages = 0;
		int32 VirtualPages = 0;
		int32 PhysicalPages = 0;
		double CacheHitRate = 0.0;
		double MissRate = 0.0;
		double FallbackRate = 0.0;
		double CompressionRatio = 0.0;
		double PhysicalAtlasMemoryRatio = 0.0;
		double ResidentPageRatio = 0.0;
		uint64 ProviderMemoryBytes = 0;
		uint64 LargestProviderCellBytes = 0;
		int32 ProviderLoadedCells = 0;
		int32 ProviderUnavailablePages = 0;
	};

	struct FRuntimeLevelRow
	{
		bool bValid = false;
		uint64 Frame = 0;
		int32 Source = 0;
		int32 Level = 0;
		int32 TotalPages = 0;
		int32 RequestedPages = 0;
		int32 ResidentRequestedPages = 0;
		int32 MissRequestedPages = 0;
		int32 FallbackPossiblePages = 0;
		int32 ResidentTotalPages = 0;
		int32 Uploads = 0;
		int32 Evictions = 0;
	};

	struct FCaptureStatsRow
	{
		FString Camera;
		FString SourceName;
		FString Filename;
		FRuntimeSummaryRow Summary;
		int32 UploadsSinceRouteStart = 0;
		int32 EvictionsSinceRouteStart = 0;
		int32 DisabledCells = 0;
		int32 ExpectedUnavailablePages = 0;
		TArray<FRuntimeLevelRow> Levels;
	};

	struct FProviderRouteStats
	{
		int32 DisabledCells = 0;
		int32 ExpectedUnavailablePages = 0;
	};

	struct FPairwiseSpec
	{
		FString Source;
		FString Baseline;
	};

	bool Tick(float DeltaTime);
	void StartBenchmarkInternal(EBenchmarkProfile Profile);
	void BuildBenchmarkPlan(EBenchmarkProfile Profile);
	void BuildBakeTestPlan();
	void BuildLargeCacheStressPlan();
	void BuildClipmapRegressionPlan();
	void BuildClipmapDegenerationPlan();
	void BuildCellProviderRegressionPlan();
	void BuildRealClipmapRegressionPlan();
	void NormalizeHardShadowCaptureCommands();
	bool IsHardShadowSanityCapture(const FCaptureSpec& Capture) const;
	FString MakeHardShadowSanityStatus(const FCaptureSpec& Capture, double MinVisibility, double MaxVisibility, double NearWhitePercent, double NearBlackPercent, FString& OutDetails) const;
	void StartNextStep();
	void FinishBenchmark();
	void ApplyStep(const FBenchmarkStep& Step);
	void ApplyCamera(const FCameraSpec& CameraSpec);
	void DestroyBenchmarkCamera();
	bool EnsureLargeCacheStressData(UWorld& World);
	UMHShadowDataAsset* LoadLargeCacheStressDataAsset() const;
	UMHShadowDataAsset* LoadRealClipmapDataAsset() const;
	UMHShadowDataAsset* LoadActiveBenchmarkDataAsset() const;
	bool ConfigureShadowProvider(UWorld& World, const FCaptureSpec& Capture);
	void Exec(UWorld* World, const FString& Command) const;
	UWorld* GetBenchmarkWorld() const;
	bool CaptureViewport(const FString& Filename, FIntPoint& OutSize, TArray<float>& OutLuma) const;
	void RecordCapture(const FBenchmarkStep& Step, const FString& Filename, const FIntPoint& Size, const TArray<float>& Luma);
	FDiffRow MakeDiffRow(const FString& Camera, const FString& Source, const FString& Baseline, const FIntPoint& Size, const TArray<float>& A, const TArray<float>& B) const;
	int32 InferCaptureSource(const FCaptureSpec& Capture) const;
	FString MakeCaptureKey(const FString& Camera, const FString& Source) const;
	uint64 GetLatestRuntimeStatsFrame() const;
	bool LoadRuntimeStats(TArray<FRuntimeSummaryRow>& OutSummaryRows, TArray<FRuntimeLevelRow>& OutLevelRows) const;
	bool FindLatestRuntimeStatsForSource(int32 Source, FRuntimeSummaryRow& OutSummary, TArray<FRuntimeLevelRow>& OutLevels) const;
	void RecordRuntimeStatsForCapture(const FCameraSpec& Camera, const FCaptureSpec& Capture, const FString& Filename);
	void WriteOutputs() const;
	void WriteReadme() const;
	FString MakeSafeName(const FString& Name) const;

	TArray<FCameraSpec> Cameras;
	TArray<FCaptureSpec> Captures;
	TArray<FBenchmarkStep> Steps;
	TArray<FCaptureRow> CaptureRows;
	TArray<FDiffRow> DiffRows;
	TArray<FDiffRow> StabilityRows;
	TArray<FCaptureStatsRow> CaptureStatsRows;
	TArray<FHardShadowSanityRow> HardShadowSanityRows;
	TArray<FPairwiseSpec> PairwiseSpecs;
	TMap<FString, TArray<float>> AtlasBaselineByCamera;
	TMap<FString, TArray<float>> ClipmapFirstByCamera;
	TMap<FString, TArray<float>> CaptureLumaByKey;
	TMap<FString, FIntPoint> AtlasSizeByCamera;
	TMap<FString, FIntPoint> ClipmapSizeByCamera;
	TMap<FString, FIntPoint> CaptureSizeByKey;
	TMap<FString, uint64> RouteStartFrameByCapture;
	TMap<FString, FProviderRouteStats> ProviderRouteStatsByCapture;

	FString OutputDir;
	FString ActiveProfileName = TEXT("BakeTest");
	double BenchmarkStartTime = 0.0;
	int32 CurrentStepIndex = INDEX_NONE;
	int32 RemainingSettleFrames = 0;
	bool bRunning = false;
	FTSTicker::FDelegateHandle TickerHandle;
	TWeakObjectPtr<ACameraActor> BenchmarkCamera;
};

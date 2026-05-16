// Copyright Epic Games, Inc. All Rights Reserved.

#include "shadow.h"
#include "HAL/IConsoleManager.h"
#include "MHShadowBenchmarkRunner.h"
#include "Modules/ModuleManager.h"

namespace
{
	void SetMHShadowCVarInt(const TCHAR* Name, int32 Value)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Variable->Set(Value, ECVF_SetByConsole);
		}
	}

	void SetMHShadowCVarFloat(const TCHAR* Name, float Value)
	{
		if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			Variable->Set(Value, ECVF_SetByConsole);
		}
	}

	void ApplyCommonSource6Preset()
	{
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Enable"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Mode"), 3);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Source"), 6);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Debug"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Clipmap.LevelCount"), 3);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Clipmap.Level0Distance"), 1600.0f);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Clipmap.DistanceScale"), 2.0f);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Clipmap.FineLevelBias"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Clipmap.FallbackToCoarser"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Clipmap.FallbackMaxCoarserLevels"), 3);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.Enable"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Feedback.Enable"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PriorityMode"), 1);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Cache.LevelPriorityScale"), 0.15f);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Cache.ResidencyBoost"), 0.25f);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.EvictRequested"), 1);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Cache.EvictHysteresis"), 0.15f);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.MaxPageUploadsPerFrame"), 32);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.DepthTest"), 0);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Restored.DepthBiasScale"), 0.0f);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Restored.DepthBiasAdd"), 0.01f);
	}
}

class FShadowModule final : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();

		RunBenchmarkCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.Run"),
			TEXT("Run the MH static shadow screenshot/diff benchmark in PIE or Standalone."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StartBenchmark),
			ECVF_Default);

		CaptureCurrentCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.CaptureCurrent"),
			TEXT("Capture the current MH shadow viewport and dump renderer/cache stats."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::CaptureCurrent),
			ECVF_Default);

		RunLargeCacheStressCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.RunLargeCacheStress"),
			TEXT("Run the large MH static shadow page-cache/clipmap stress benchmark in PIE or Standalone."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StartLargeCacheStressBenchmark),
			ECVF_Default);

		RunClipmapRegressionCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.RunClipmapRegression"),
			TEXT("Run the MH static shadow clipmap regression benchmark with single-level and tuning routes."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StartClipmapRegressionBenchmark),
			ECVF_Default);

		RunClipmapDegenerationCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.RunClipmapDegeneration"),
			TEXT("Run the MH static shadow clipmap single-level degeneration benchmark."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StartClipmapDegenerationBenchmark),
			ECVF_Default);

		RunCellProviderRegressionCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.RunCellProviderRegression"),
			TEXT("Run the MH static shadow cell-provider clipmap regression benchmark in PIE or Standalone."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StartCellProviderRegressionBenchmark),
			ECVF_Default);

		RunRealClipmapRegressionCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.RunRealClipmapRegression"),
			TEXT("Run the MH static shadow real multi-range clipmap regression benchmark in PIE or Standalone."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StartRealClipmapRegressionBenchmark),
			ECVF_Default);

		RunCacheStrategyRegressionCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.RunCacheStrategyRegression"),
			TEXT("Run the MH static shadow page-cache priority/prefetch strategy benchmark in PIE or Standalone."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StartCacheStrategyRegressionBenchmark),
			ECVF_Default);

		RunQualityRegressionCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.RunQualityRegression"),
			TEXT("Run the MH static shadow UE-style bias and stable PCF quality benchmark in PIE or Standalone."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StartQualityRegressionBenchmark),
			ECVF_Default);

		StopBenchmarkCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.Stop"),
			TEXT("Stop the active MH static shadow benchmark."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StopBenchmark),
			ECVF_Default);

		HardClipmap16PresetCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Preset.HardClipmap16"),
			TEXT("Apply the recommended Source=6 hard-shadow preset: 16x16 cache, priority, prefetch, tuned fixed bias."),
			FConsoleCommandDelegate::CreateRaw(this, &FShadowModule::ApplyHardClipmap16Preset),
			ECVF_Default);

		LowMemoryHardClipmap8PresetCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Preset.LowMemoryHardClipmap8"),
			TEXT("Apply the low-memory Source=6 hard-shadow preset: 8x8 cache, priority, no prefetch, tuned fixed bias."),
			FConsoleCommandDelegate::CreateRaw(this, &FShadowModule::ApplyLowMemoryHardClipmap8Preset),
			ECVF_Default);

		QualityClipmap16PresetCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Preset.QualityClipmap16"),
			TEXT("Apply the experimental Source=6 quality preset: 16x16 cache, receiver-plane bias, stable 3x3 PCF."),
			FConsoleCommandDelegate::CreateRaw(this, &FShadowModule::ApplyQualityClipmap16Preset),
			ECVF_Default);
	}

	virtual void ShutdownModule() override
	{
		BenchmarkRunner.StopBenchmark();

		if (RunBenchmarkCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(RunBenchmarkCommand);
			RunBenchmarkCommand = nullptr;
		}
		if (CaptureCurrentCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(CaptureCurrentCommand);
			CaptureCurrentCommand = nullptr;
		}
		if (RunLargeCacheStressCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(RunLargeCacheStressCommand);
			RunLargeCacheStressCommand = nullptr;
		}
		if (RunClipmapRegressionCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(RunClipmapRegressionCommand);
			RunClipmapRegressionCommand = nullptr;
		}
		if (RunClipmapDegenerationCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(RunClipmapDegenerationCommand);
			RunClipmapDegenerationCommand = nullptr;
		}
		if (RunCellProviderRegressionCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(RunCellProviderRegressionCommand);
			RunCellProviderRegressionCommand = nullptr;
		}
		if (RunRealClipmapRegressionCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(RunRealClipmapRegressionCommand);
			RunRealClipmapRegressionCommand = nullptr;
		}
		if (RunCacheStrategyRegressionCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(RunCacheStrategyRegressionCommand);
			RunCacheStrategyRegressionCommand = nullptr;
		}
		if (RunQualityRegressionCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(RunQualityRegressionCommand);
			RunQualityRegressionCommand = nullptr;
		}
		if (StopBenchmarkCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(StopBenchmarkCommand);
			StopBenchmarkCommand = nullptr;
		}
		if (HardClipmap16PresetCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(HardClipmap16PresetCommand);
			HardClipmap16PresetCommand = nullptr;
		}
		if (LowMemoryHardClipmap8PresetCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(LowMemoryHardClipmap8PresetCommand);
			LowMemoryHardClipmap8PresetCommand = nullptr;
		}
		if (QualityClipmap16PresetCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(QualityClipmap16PresetCommand);
			QualityClipmap16PresetCommand = nullptr;
		}

		FDefaultGameModuleImpl::ShutdownModule();
	}

private:
	void ApplyHardClipmap16Preset()
	{
		ApplyCommonSource6Preset();
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX"), 16);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY"), 16);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PrefetchRadius"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PrefetchBudget"), 32);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), 0);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.FilterMode"), 0);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.BiasMode"), 0);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.Reset"), 1);
		UE_LOG(LogTemp, Display, TEXT("Applied MHShadow.Preset.HardClipmap16."));
	}

	void ApplyLowMemoryHardClipmap8Preset()
	{
		ApplyCommonSource6Preset();
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX"), 8);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY"), 8);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PrefetchRadius"), 0);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PrefetchBudget"), 0);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), 0);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.FilterMode"), 0);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.BiasMode"), 0);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.Reset"), 1);
		UE_LOG(LogTemp, Display, TEXT("Applied MHShadow.Preset.LowMemoryHardClipmap8."));
	}

	void ApplyQualityClipmap16Preset()
	{
		ApplyCommonSource6Preset();
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesX"), 16);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PhysicalPagesY"), 16);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PrefetchRadius"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.PrefetchBudget"), 32);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.EnablePCF"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.FilterMode"), 1);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.BiasMode"), 2);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Restored.PCFKernel"), 3);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Restored.PCFRadiusTexels"), 1.0f);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Restored.BiasWorldUnits"), 24.0f);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Restored.SlopeBiasScale"), 1.0f);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Restored.SlopeBiasClamp"), 0.01f);
		SetMHShadowCVarFloat(TEXT("r.Shadow.MHStatic.Restored.FilterBiasScale"), 0.5f);
		SetMHShadowCVarInt(TEXT("r.Shadow.MHStatic.Cache.Reset"), 1);
		UE_LOG(LogTemp, Display, TEXT("Applied MHShadow.Preset.QualityClipmap16."));
	}

	FMHShadowBenchmarkRunner BenchmarkRunner;
	IConsoleObject* RunBenchmarkCommand = nullptr;
	IConsoleObject* CaptureCurrentCommand = nullptr;
	IConsoleObject* RunLargeCacheStressCommand = nullptr;
	IConsoleObject* RunClipmapRegressionCommand = nullptr;
	IConsoleObject* RunClipmapDegenerationCommand = nullptr;
	IConsoleObject* RunCellProviderRegressionCommand = nullptr;
	IConsoleObject* RunRealClipmapRegressionCommand = nullptr;
	IConsoleObject* RunCacheStrategyRegressionCommand = nullptr;
	IConsoleObject* RunQualityRegressionCommand = nullptr;
	IConsoleObject* StopBenchmarkCommand = nullptr;
	IConsoleObject* HardClipmap16PresetCommand = nullptr;
	IConsoleObject* LowMemoryHardClipmap8PresetCommand = nullptr;
	IConsoleObject* QualityClipmap16PresetCommand = nullptr;
};

IMPLEMENT_PRIMARY_GAME_MODULE(FShadowModule, shadow, "shadow");

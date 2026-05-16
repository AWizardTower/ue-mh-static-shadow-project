// Copyright Epic Games, Inc. All Rights Reserved.

#include "shadow.h"
#include "HAL/IConsoleManager.h"
#include "MHShadowBenchmarkRunner.h"
#include "Modules/ModuleManager.h"

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

		StopBenchmarkCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("MHShadow.Benchmark.Stop"),
			TEXT("Stop the active MH static shadow benchmark."),
			FConsoleCommandDelegate::CreateRaw(&BenchmarkRunner, &FMHShadowBenchmarkRunner::StopBenchmark),
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
		if (StopBenchmarkCommand)
		{
			IConsoleManager::Get().UnregisterConsoleObject(StopBenchmarkCommand);
			StopBenchmarkCommand = nullptr;
		}

		FDefaultGameModuleImpl::ShutdownModule();
	}

private:
	FMHShadowBenchmarkRunner BenchmarkRunner;
	IConsoleObject* RunBenchmarkCommand = nullptr;
	IConsoleObject* CaptureCurrentCommand = nullptr;
	IConsoleObject* RunLargeCacheStressCommand = nullptr;
	IConsoleObject* RunClipmapRegressionCommand = nullptr;
	IConsoleObject* RunClipmapDegenerationCommand = nullptr;
	IConsoleObject* RunCellProviderRegressionCommand = nullptr;
	IConsoleObject* RunRealClipmapRegressionCommand = nullptr;
	IConsoleObject* StopBenchmarkCommand = nullptr;
};

IMPLEMENT_PRIMARY_GAME_MODULE(FShadowModule, shadow, "shadow");

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
	IConsoleObject* StopBenchmarkCommand = nullptr;
};

IMPLEMENT_PRIMARY_GAME_MODULE(FShadowModule, shadow, "shadow");

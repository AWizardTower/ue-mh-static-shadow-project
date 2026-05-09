// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCompressor.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
FMHShadowDepthInterval MakeTestInterval(float MinDepth, float MaxDepth)
{
	FMHShadowDepthInterval Interval;
	Interval.MinDepth = MinDepth;
	Interval.MaxDepth = MaxDepth;
	Interval.bValid = true;
	return Interval;
}

int32 GetChildIndex(const FIntVector4& ChildIndices, int32 ChildSlot)
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

const FMHShadowDepthInterval* FindNodeInterval(const FMHShadowCompressionOutput& Output, const FMHShadowNode& Node)
{
	return Output.Intervals.IsValidIndex(Node.IntervalIndex) ? &Output.Intervals[Node.IntervalIndex] : nullptr;
}

const FMHShadowDepthInterval* SampleCompressedTree(const FMHShadowCompressionOutput& Output, FIntPoint Resolution, int32 X, int32 Y)
{
	if (!Output.Nodes.IsValidIndex(0))
	{
		return nullptr;
	}

	int32 NodeIndex = 0;
	FIntPoint NodeMin(0, 0);
	int32 NodeSize = Resolution.X;

	for (int32 Step = 0; Step < 32; ++Step)
	{
		if (!Output.Nodes.IsValidIndex(NodeIndex))
		{
			return nullptr;
		}

		const FMHShadowNode& Node = Output.Nodes[NodeIndex];
		const bool bHasChildren = Node.ChildIndices.X >= 0
			|| Node.ChildIndices.Y >= 0
			|| Node.ChildIndices.Z >= 0
			|| Node.ChildIndices.W >= 0;

		if (!bHasChildren || NodeSize <= 1)
		{
			return FindNodeInterval(Output, Node);
		}

		const int32 ChildSize = FMath::Max(NodeSize / 2, 1);
		const int32 ChildX = (X - NodeMin.X) >= ChildSize ? 1 : 0;
		const int32 ChildY = (Y - NodeMin.Y) >= ChildSize ? 1 : 0;
		const int32 ChildSlot = ChildY * 2 + ChildX;
		const int32 ChildNodeIndex = GetChildIndex(Node.ChildIndices, ChildSlot);

		if (ChildNodeIndex < 0)
		{
			return FindNodeInterval(Output, Node);
		}

		NodeIndex = ChildNodeIndex;
		NodeMin.X += ChildX * ChildSize;
		NodeMin.Y += ChildY * ChildSize;
		NodeSize = ChildSize;
	}

	return nullptr;
}

bool TestIntervalEquals(FAutomationTestBase& Test, const TCHAR* Context, const FMHShadowDepthInterval* Actual, const FMHShadowDepthInterval& Expected)
{
	if (!Actual)
	{
		Test.AddError(FString::Printf(TEXT("%s: expected a valid interval, got none."), Context));
		return false;
	}

	const bool bMatches = Actual->bValid == Expected.bValid
		&& FMath::IsNearlyEqual(Actual->MinDepth, Expected.MinDepth)
		&& FMath::IsNearlyEqual(Actual->MaxDepth, Expected.MaxDepth);
	if (!bMatches)
	{
		Test.AddError(FString::Printf(
			TEXT("%s: expected [%f, %f] valid=%d, got [%f, %f] valid=%d."),
			Context,
			Expected.MinDepth,
			Expected.MaxDepth,
			Expected.bValid ? 1 : 0,
			Actual->MinDepth,
			Actual->MaxDepth,
			Actual->bValid ? 1 : 0));
	}

	return bMatches;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHShadowCompressorUniformTest, "Shadow.MH.Compressor.Uniform2x2CollapsesToRoot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHShadowCompressorUniformTest::RunTest(const FString& Parameters)
{
	FMHShadowCompressionInput Input;
	Input.Resolution = FIntPoint(2, 2);
	Input.TexelIntervals.Init(MakeTestInterval(10.0f, 20.0f), 4);

	FMHShadowCompressionOutput Output;
	FString Error;
	TestTrue(TEXT("Uniform input compresses successfully."), FMHShadowCompressor::Compress(Input, Output, &Error));
	TestEqual(TEXT("Uniform 2x2 input collapses to one node."), Output.Nodes.Num(), 1);
	TestEqual(TEXT("Uniform 2x2 input stores one interval."), Output.Intervals.Num(), 1);

	const FMHShadowDepthInterval Expected = MakeTestInterval(10.0f, 20.0f);
	TestIntervalEquals(*this, TEXT("Sample 0,0"), SampleCompressedTree(Output, Input.Resolution, 0, 0), Expected);
	TestIntervalEquals(*this, TEXT("Sample 1,1"), SampleCompressedTree(Output, Input.Resolution, 1, 1), Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHShadowCompressorMixedTest, "Shadow.MH.Compressor.Mixed2x2PreservesTexels", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHShadowCompressorMixedTest::RunTest(const FString& Parameters)
{
	FMHShadowCompressionInput Input;
	Input.Resolution = FIntPoint(2, 2);
	Input.TexelIntervals = {
		MakeTestInterval(0.0f, 1.0f),
		MakeTestInterval(10.0f, 12.0f),
		MakeTestInterval(20.0f, 25.0f),
		MakeTestInterval(30.0f, 33.0f)
	};

	FMHShadowCompressionOutput Output;
	FString Error;
	TestTrue(TEXT("Mixed input compresses successfully."), FMHShadowCompressor::Compress(Input, Output, &Error));
	TestEqual(TEXT("Mixed 2x2 input keeps four intervals represented."), Output.Intervals.Num(), 4);

	TestIntervalEquals(*this, TEXT("Sample 0,0"), SampleCompressedTree(Output, Input.Resolution, 0, 0), Input.TexelIntervals[0]);
	TestIntervalEquals(*this, TEXT("Sample 1,0"), SampleCompressedTree(Output, Input.Resolution, 1, 0), Input.TexelIntervals[1]);
	TestIntervalEquals(*this, TEXT("Sample 0,1"), SampleCompressedTree(Output, Input.Resolution, 0, 1), Input.TexelIntervals[2]);
	TestIntervalEquals(*this, TEXT("Sample 1,1"), SampleCompressedTree(Output, Input.Resolution, 1, 1), Input.TexelIntervals[3]);
	return true;
}

#endif

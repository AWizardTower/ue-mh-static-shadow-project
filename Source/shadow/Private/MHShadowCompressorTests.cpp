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

FMHShadowDepthInterval MakeEmptyInterval()
{
	return FMHShadowDepthInterval();
}

FMHShadowDepthInterval NormalizeForCompression(const FMHShadowDepthInterval& Interval)
{
	if (!Interval.bValid)
	{
		return MakeTestInterval(1.0f, 1.0f);
	}
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

bool SampleCompressedDepth(const TArray<FMHShadowNode>& Nodes, FIntPoint Resolution, int32 X, int32 Y, float& OutDepth)
{
	if (!Nodes.IsValidIndex(0))
	{
		return false;
	}

	int32 NodeIndex = 0;
	FIntPoint NodeMin(0, 0);
	int32 NodeSize = Resolution.X;
	bool bHasDepth = false;
	OutDepth = 1.0f;

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
		const int32 ChildX = (X - NodeMin.X) >= ChildSize ? 1 : 0;
		const int32 ChildY = (Y - NodeMin.Y) >= ChildSize ? 1 : 0;
		const int32 ChildSlot = ChildY * 2 + ChildX;
		const int32 ChildNodeIndex = GetChildIndex(Node.ChildIndices, ChildSlot);

		if (ChildNodeIndex < 0)
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

bool TestDepthInsideInterval(FAutomationTestBase& Test, const TCHAR* Context, float Depth, const FMHShadowDepthInterval& ExpectedInterval)
{
	const FMHShadowDepthInterval Interval = NormalizeForCompression(ExpectedInterval);
	const bool bInside = Depth >= Interval.MinDepth - KINDA_SMALL_NUMBER && Depth <= Interval.MaxDepth + KINDA_SMALL_NUMBER;
	if (!bInside)
	{
		Test.AddError(FString::Printf(
			TEXT("%s: sampled representative depth %f is outside interval [%f, %f]."),
			Context,
			Depth,
			Interval.MinDepth,
			Interval.MaxDepth));
	}
	return bInside;
}

bool ValidateAllTexels(FAutomationTestBase& Test, const FMHShadowCompressionInput& Input, const FMHShadowCompressionOutput& Output)
{
	bool bOk = true;
	for (int32 Y = 0; Y < Input.Resolution.Y; ++Y)
	{
		for (int32 X = 0; X < Input.Resolution.X; ++X)
		{
			float Depth = 1.0f;
			const bool bSampled = SampleCompressedDepth(Output.Nodes, Input.Resolution, X, Y, Depth);
			if (!bSampled)
			{
				Test.AddError(FString::Printf(TEXT("No representative depth sampled at texel %d,%d."), X, Y));
				bOk = false;
				continue;
			}

			const int32 TexelIndex = Y * Input.Resolution.X + X;
			bOk &= TestDepthInsideInterval(Test, *FString::Printf(TEXT("Texel %d,%d"), X, Y), Depth, Input.TexelIntervals[TexelIndex]);
		}
	}
	return bOk;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHShadowCompressorUniformTest, "Shadow.MH.Compressor.Uniform2x2CollapsesToRoot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHShadowCompressorUniformTest::RunTest(const FString& Parameters)
{
	FMHShadowCompressionInput Input;
	Input.Resolution = FIntPoint(2, 2);
	Input.TexelIntervals.Init(MakeTestInterval(0.25f, 0.75f), 4);

	FMHShadowCompressionOutput Output;
	FString Error;
	TestTrue(TEXT("Uniform input compresses successfully."), FMHShadowCompressor::Compress(Input, Output, &Error));
	TestEqual(TEXT("Uniform 2x2 input collapses to one node."), Output.Nodes.Num(), 1);
	TestTrue(TEXT("Root stores a representative depth."), Output.Nodes[0].bHasRepresentativeDepth);
	TestDepthInsideInterval(*this, TEXT("Root representative"), Output.Nodes[0].RepresentativeDepth, Input.TexelIntervals[0]);
	TestTrue(TEXT("Every texel samples a legal representative depth."), ValidateAllTexels(*this, Input, Output));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHShadowCompressorDisjointTest, "Shadow.MH.Compressor.Disjoint2x2PreservesLegalDepths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHShadowCompressorDisjointTest::RunTest(const FString& Parameters)
{
	FMHShadowCompressionInput Input;
	Input.Resolution = FIntPoint(2, 2);
	Input.TexelIntervals = {
		MakeTestInterval(0.05f, 0.10f),
		MakeTestInterval(0.30f, 0.35f),
		MakeTestInterval(0.55f, 0.60f),
		MakeTestInterval(0.80f, 0.85f)
	};

	FMHShadowCompressionOutput Output;
	FString Error;
	TestTrue(TEXT("Disjoint input compresses successfully."), FMHShadowCompressor::Compress(Input, Output, &Error));
	TestTrue(TEXT("Disjoint input keeps refinement nodes."), Output.Nodes.Num() > 1);
	TestTrue(TEXT("Every texel samples a legal representative depth."), ValidateAllTexels(*this, Input, Output));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHShadowCompressorMixedEmptyTest, "Shadow.MH.Compressor.MixedEmptyTexelsUseFarDepth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHShadowCompressorMixedEmptyTest::RunTest(const FString& Parameters)
{
	FMHShadowCompressionInput Input;
	Input.Resolution = FIntPoint(2, 2);
	Input.TexelIntervals = {
		MakeEmptyInterval(),
		MakeTestInterval(0.20f, 0.30f),
		MakeEmptyInterval(),
		MakeTestInterval(0.60f, 0.70f)
	};

	FMHShadowCompressionOutput Output;
	FString Error;
	TestTrue(TEXT("Mixed empty input compresses successfully."), FMHShadowCompressor::Compress(Input, Output, &Error));
	TestTrue(TEXT("Every texel samples a legal representative depth."), ValidateAllTexels(*this, Input, Output));

	float EmptyDepth = 0.0f;
	TestTrue(TEXT("Empty texel samples a representative depth."), SampleCompressedDepth(Output.Nodes, Input.Resolution, 0, 0, EmptyDepth));
	TestTrue(TEXT("Empty texel samples far depth."), FMath::IsNearlyEqual(EmptyDepth, 1.0f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHShadowCompressorAncestorFallbackTest, "Shadow.MH.Compressor.EmptyInnerNodeFallsBackToAncestorDepth", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHShadowCompressorAncestorFallbackTest::RunTest(const FString& Parameters)
{
	TArray<FMHShadowNode> Nodes;
	Nodes.SetNum(3);

	Nodes[0].ChildIndices = FIntVector4(1, -1, -1, -1);
	Nodes[0].RepresentativeDepth = 0.5f;
	Nodes[0].bHasRepresentativeDepth = true;

	Nodes[1].ChildIndices = FIntVector4(-1, 2, -1, -1);
	Nodes[1].bHasRepresentativeDepth = false;

	Nodes[2].RepresentativeDepth = 0.25f;
	Nodes[2].bHasRepresentativeDepth = true;

	float Depth = 0.0f;
	TestTrue(TEXT("Missing grandchild samples ancestor representative depth."), SampleCompressedDepth(Nodes, FIntPoint(4, 4), 0, 0, Depth));
	TestTrue(TEXT("Ancestor representative depth is returned."), FMath::IsNearlyEqual(Depth, 0.5f));

	TestTrue(TEXT("Present grandchild samples own representative depth."), SampleCompressedDepth(Nodes, FIntPoint(4, 4), 1, 0, Depth));
	TestTrue(TEXT("Grandchild representative depth is returned."), FMath::IsNearlyEqual(Depth, 0.25f));
	return true;
}

#endif

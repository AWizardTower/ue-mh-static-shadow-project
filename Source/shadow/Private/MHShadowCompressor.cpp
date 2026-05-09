// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCompressor.h"

namespace
{
struct FBuildNode
{
	FMHShadowDepthInterval Interval;
	TStaticArray<TUniquePtr<FBuildNode>, 4> Children;
	int32 Level = 0;

	bool HasAnyChild() const
	{
		for (const TUniquePtr<FBuildNode>& Child : Children)
		{
			if (Child)
			{
				return true;
			}
		}
		return false;
	}

	bool IsUniformLeaf() const
	{
		return !HasAnyChild();
	}
};

static bool IntersectIntervals(const FMHShadowDepthInterval& A, const FMHShadowDepthInterval& B, FMHShadowDepthInterval& Out)
{
	if (!A.bValid || !B.bValid)
	{
		return false;
	}

	Out.MinDepth = FMath::Max(A.MinDepth, B.MinDepth);
	Out.MaxDepth = FMath::Min(A.MaxDepth, B.MaxDepth);
	Out.bValid = Out.MinDepth <= Out.MaxDepth;
	return Out.bValid;
}

static bool IntersectSubset(const TStaticArray<FBuildNode*, 4>& Children, uint8 Mask, FMHShadowDepthInterval& Out)
{
	bool bHasFirst = false;
	FMHShadowDepthInterval Accumulated;

	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		if ((Mask & (1u << ChildIndex)) == 0)
		{
			continue;
		}

		FBuildNode* Child = Children[ChildIndex];
		if (!Child || !Child->IsUniformLeaf() || !Child->Interval.bValid)
		{
			return false;
		}

		if (!bHasFirst)
		{
			Accumulated = Child->Interval;
			bHasFirst = true;
		}
		else if (!IntersectIntervals(Accumulated, Child->Interval, Accumulated))
		{
			return false;
		}
	}

	if (!bHasFirst)
	{
		return false;
	}

	Out = Accumulated;
	return true;
}

static int32 CountBits(uint8 Mask)
{
	int32 Count = 0;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		Count += (Mask & (1u << Index)) ? 1 : 0;
	}
	return Count;
}

static TUniquePtr<FBuildNode> BuildNode(const FMHShadowCompressionInput& Input, int32 X, int32 Y, int32 Size, int32 Level)
{
	TUniquePtr<FBuildNode> Node = MakeUnique<FBuildNode>();
	Node->Level = Level;

	if (Size == 1)
	{
		const int32 TexelIndex = Y * Input.Resolution.X + X;
		Node->Interval = Input.TexelIntervals.IsValidIndex(TexelIndex)
			? Input.TexelIntervals[TexelIndex]
			: FMHShadowDepthInterval();
		return Node;
	}

	const int32 HalfSize = Size / 2;
	Node->Children[0] = BuildNode(Input, X, Y, HalfSize, Level + 1);
	Node->Children[1] = BuildNode(Input, X + HalfSize, Y, HalfSize, Level + 1);
	Node->Children[2] = BuildNode(Input, X, Y + HalfSize, HalfSize, Level + 1);
	Node->Children[3] = BuildNode(Input, X + HalfSize, Y + HalfSize, HalfSize, Level + 1);

	TStaticArray<FBuildNode*, 4> ChildPtrs;
	bool bAllUniformEmpty = true;
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		ChildPtrs[ChildIndex] = Node->Children[ChildIndex].Get();
		bAllUniformEmpty &= ChildPtrs[ChildIndex]->IsUniformLeaf() && !ChildPtrs[ChildIndex]->Interval.bValid;
	}

	if (bAllUniformEmpty)
	{
		for (TUniquePtr<FBuildNode>& Child : Node->Children)
		{
			Child.Reset();
		}
		return Node;
	}

	uint8 BestMask = 0;
	FMHShadowDepthInterval BestInterval;
	int32 BestCount = 0;

	for (uint8 Mask = 1; Mask < 16; ++Mask)
	{
		FMHShadowDepthInterval Candidate;
		const int32 CandidateCount = CountBits(Mask);
		if (CandidateCount > BestCount && IntersectSubset(ChildPtrs, Mask, Candidate))
		{
			BestMask = Mask;
			BestInterval = Candidate;
			BestCount = CandidateCount;
		}
	}

	if (BestCount > 0)
	{
		Node->Interval = BestInterval;
		for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
		{
			if ((BestMask & (1u << ChildIndex)) != 0)
			{
				Node->Children[ChildIndex].Reset();
			}
		}
	}

	return Node;
}

static int32 AddInterval(const FMHShadowDepthInterval& Interval, FMHShadowCompressionOutput& Output)
{
	if (!Interval.bValid)
	{
		return INDEX_NONE;
	}

	const int32 NewIndex = Output.Intervals.Num();
	Output.Intervals.Add(Interval);
	return NewIndex;
}

static int32 FlattenNode(const FBuildNode& BuildNode, FMHShadowCompressionOutput& Output)
{
	FMHShadowNode Node;
	Node.Level = BuildNode.Level;
	Node.IntervalIndex = AddInterval(BuildNode.Interval, Output);

	const int32 NodeIndex = Output.Nodes.Num();
	Output.Nodes.Add(Node);

	FIntVector4 ChildIndices(-1, -1, -1, -1);
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		if (BuildNode.Children[ChildIndex])
		{
			const int32 FlattenedChildIndex = FlattenNode(*BuildNode.Children[ChildIndex], Output);
			if (ChildIndex == 0)
			{
				ChildIndices.X = FlattenedChildIndex;
			}
			else if (ChildIndex == 1)
			{
				ChildIndices.Y = FlattenedChildIndex;
			}
			else if (ChildIndex == 2)
			{
				ChildIndices.Z = FlattenedChildIndex;
			}
			else
			{
				ChildIndices.W = FlattenedChildIndex;
			}
		}
	}

	Output.Nodes[NodeIndex].ChildIndices = ChildIndices;
	return NodeIndex;
}
}

bool FMHShadowCompressor::Compress(const FMHShadowCompressionInput& Input, FMHShadowCompressionOutput& Output, FString* OutError)
{
	Output = FMHShadowCompressionOutput();

	if (Input.Resolution.X <= 0 || Input.Resolution.X != Input.Resolution.Y || !FMath::IsPowerOfTwo(Input.Resolution.X))
	{
		if (OutError)
		{
			*OutError = TEXT("MH shadow compression requires a square power-of-two resolution.");
		}
		return false;
	}

	const int32 ExpectedTexels = Input.Resolution.X * Input.Resolution.Y;
	if (Input.TexelIntervals.Num() != ExpectedTexels)
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Expected %d texel intervals, got %d."), ExpectedTexels, Input.TexelIntervals.Num());
		}
		return false;
	}

	double StartSeconds = FPlatformTime::Seconds();
	TUniquePtr<FBuildNode> Root = BuildNode(Input, 0, 0, Input.Resolution.X, 0);
	FlattenNode(*Root, Output);

	int32 ValidTexelCount = 0;
	for (const FMHShadowDepthInterval& Interval : Input.TexelIntervals)
	{
		if (Interval.bValid)
		{
			++ValidTexelCount;
		}
	}

	Output.Stats.RawTexelCount = ExpectedTexels;
	Output.Stats.ValidTexelCount = ValidTexelCount;
	Output.Stats.NodeCount = Output.Nodes.Num();
	Output.Stats.IntervalCount = Output.Intervals.Num();
	Output.Stats.RawBytes = static_cast<int64>(ExpectedTexels) * static_cast<int64>(sizeof(float) * 2);
	Output.Stats.CompressedBytes =
		static_cast<int64>(Output.Nodes.Num()) * static_cast<int64>(sizeof(int32) * 6)
		+ static_cast<int64>(Output.Intervals.Num()) * static_cast<int64>(sizeof(float) * 2);
	Output.Stats.CompressionRatio = Output.Stats.RawBytes > 0
		? static_cast<float>(static_cast<double>(Output.Stats.CompressedBytes) / static_cast<double>(Output.Stats.RawBytes))
		: 1.0f;
	Output.Stats.BakeSeconds = FPlatformTime::Seconds() - StartSeconds;
	return true;
}

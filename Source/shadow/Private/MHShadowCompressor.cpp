// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCompressor.h"

namespace
{
struct FBuildNode
{
	FMHShadowDepthInterval Bounds;
	TStaticArray<TUniquePtr<FBuildNode>, 4> Children;
	float RepresentativeDepth = 1.0f;
	int32 Level = 0;
	bool bHasRepresentativeDepth = false;

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

static FMHShadowDepthInterval MakeFarInterval()
{
	FMHShadowDepthInterval Interval;
	Interval.MinDepth = 1.0f;
	Interval.MaxDepth = 1.0f;
	Interval.bValid = true;
	return Interval;
}

static FMHShadowDepthInterval NormalizeInputInterval(const FMHShadowDepthInterval& Interval)
{
	if (!Interval.bValid)
	{
		return MakeFarInterval();
	}

	FMHShadowDepthInterval Normalized = Interval;
	if (Normalized.MinDepth > Normalized.MaxDepth)
	{
		Swap(Normalized.MinDepth, Normalized.MaxDepth);
	}
	Normalized.MinDepth = FMath::Clamp(Normalized.MinDepth, 0.0f, 1.0f);
	Normalized.MaxDepth = FMath::Clamp(Normalized.MaxDepth, 0.0f, 1.0f);
	Normalized.bValid = true;
	return Normalized;
}

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

static bool ContainsDepth(const FMHShadowDepthInterval& Interval, float Depth)
{
	return Interval.bValid && Interval.MinDepth <= Depth && Interval.MaxDepth >= Depth;
}

static int32 CountContainingIntervals(const TStaticArray<FBuildNode*, 4>& Children, float Depth)
{
	int32 Count = 0;

	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		FBuildNode* Child = Children[ChildIndex];
		if (Child && ContainsDepth(Child->Bounds, Depth))
		{
			++Count;
		}
	}

	return Count;
}

static FMHShadowDepthInterval ComputeBoundsForDepth(const TStaticArray<FBuildNode*, 4>& Children, float Depth)
{
	FMHShadowDepthInterval Bounds;
	Bounds.MinDepth = 0.0f;
	Bounds.MaxDepth = 1.0f;
	Bounds.bValid = true;

	bool bAnyCovered = false;
	for (FBuildNode* Child : Children)
	{
		if (Child && ContainsDepth(Child->Bounds, Depth))
		{
			Bounds.MinDepth = bAnyCovered ? FMath::Max(Bounds.MinDepth, Child->Bounds.MinDepth) : Child->Bounds.MinDepth;
			Bounds.MaxDepth = bAnyCovered ? FMath::Min(Bounds.MaxDepth, Child->Bounds.MaxDepth) : Child->Bounds.MaxDepth;
			bAnyCovered = true;
		}
	}

	if (!bAnyCovered || Bounds.MinDepth > Bounds.MaxDepth)
	{
		Bounds.MinDepth = Depth;
		Bounds.MaxDepth = Depth;
	}

	return Bounds;
}

static void ChooseRepresentativeDepth(const TStaticArray<FBuildNode*, 4>& Children, float& OutDepth, FMHShadowDepthInterval& OutBounds)
{
	FMHShadowDepthInterval Intersection = Children[0]->Bounds;
	for (int32 ChildIndex = 1; ChildIndex < 4; ++ChildIndex)
	{
		FMHShadowDepthInterval NewIntersection;
		if (!IntersectIntervals(Intersection, Children[ChildIndex]->Bounds, NewIntersection))
		{
			Intersection.bValid = false;
			break;
		}
		Intersection = NewIntersection;
	}

	if (Intersection.bValid)
	{
		OutBounds = Intersection;
		OutDepth = (Intersection.MinDepth + Intersection.MaxDepth) * 0.5f;
		return;
	}

	int32 BestCount = -1;
	float BestDepth = Children[0]->Bounds.MinDepth;
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		const float CandidateDepth = Children[ChildIndex]->Bounds.MinDepth;
		const int32 CandidateCount = CountContainingIntervals(Children, CandidateDepth);
		if (CandidateCount > BestCount)
		{
			BestCount = CandidateCount;
			BestDepth = CandidateDepth;
		}
	}

	OutDepth = BestDepth;
	OutBounds = ComputeBoundsForDepth(Children, BestDepth);
}

static TUniquePtr<FBuildNode> BuildNode(const FMHShadowCompressionInput& Input, int32 X, int32 Y, int32 Size, int32 Level)
{
	TUniquePtr<FBuildNode> Node = MakeUnique<FBuildNode>();
	Node->Level = Level;

	if (Size == 1)
	{
		const int32 TexelIndex = Y * Input.Resolution.X + X;
		Node->Bounds = Input.TexelIntervals.IsValidIndex(TexelIndex)
			? NormalizeInputInterval(Input.TexelIntervals[TexelIndex])
			: MakeFarInterval();
		Node->RepresentativeDepth = (Node->Bounds.MinDepth + Node->Bounds.MaxDepth) * 0.5f;
		Node->bHasRepresentativeDepth = true;
		return Node;
	}

	const int32 HalfSize = Size / 2;
	Node->Children[0] = BuildNode(Input, X, Y, HalfSize, Level + 1);
	Node->Children[1] = BuildNode(Input, X + HalfSize, Y, HalfSize, Level + 1);
	Node->Children[2] = BuildNode(Input, X, Y + HalfSize, HalfSize, Level + 1);
	Node->Children[3] = BuildNode(Input, X + HalfSize, Y + HalfSize, HalfSize, Level + 1);

	TStaticArray<FBuildNode*, 4> ChildPtrs;
	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		ChildPtrs[ChildIndex] = Node->Children[ChildIndex].Get();
	}

	ChooseRepresentativeDepth(ChildPtrs, Node->RepresentativeDepth, Node->Bounds);
	Node->bHasRepresentativeDepth = true;

	for (int32 ChildIndex = 0; ChildIndex < 4; ++ChildIndex)
	{
		FBuildNode* Child = Node->Children[ChildIndex].Get();
		if (!Child || !ContainsDepth(Child->Bounds, Node->RepresentativeDepth))
		{
			continue;
		}

		if (Child->HasAnyChild())
		{
			Child->bHasRepresentativeDepth = false;
			Child->RepresentativeDepth = 1.0f;
		}
		else
		{
			Node->Children[ChildIndex].Reset();
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
	Node.RepresentativeDepth = BuildNode.RepresentativeDepth;
	Node.BoundsMinDepth = BuildNode.Bounds.MinDepth;
	Node.BoundsMaxDepth = BuildNode.Bounds.MaxDepth;
	Node.bHasRepresentativeDepth = BuildNode.bHasRepresentativeDepth;
	Node.IntervalIndex = AddInterval(BuildNode.Bounds, Output);

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
		static_cast<int64>(Output.Nodes.Num()) * static_cast<int64>(sizeof(int32) * 4 + sizeof(float) * 3 + sizeof(uint32));
	Output.Stats.CompressionRatio = Output.Stats.RawBytes > 0
		? static_cast<float>(static_cast<double>(Output.Stats.CompressedBytes) / static_cast<double>(Output.Stats.RawBytes))
		: 1.0f;
	Output.Stats.BakeSeconds = FPlatformTime::Seconds() - StartSeconds;
	return true;
}

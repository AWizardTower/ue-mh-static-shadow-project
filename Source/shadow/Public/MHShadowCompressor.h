// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MHShadowTypes.h"

struct FMHShadowCompressionInput
{
	FIntPoint Resolution = FIntPoint::ZeroValue;
	TArray<FMHShadowDepthInterval> TexelIntervals;
};

struct FMHShadowCompressionOutput
{
	TArray<FMHShadowDepthInterval> Intervals;
	TArray<FMHShadowNode> Nodes;
	FMHShadowBakeStats Stats;
};

class SHADOW_API FMHShadowCompressor
{
public:
	static bool Compress(const FMHShadowCompressionInput& Input, FMHShadowCompressionOutput& Output, FString* OutError = nullptr);
};

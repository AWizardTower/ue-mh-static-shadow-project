// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MHShadowTypes.generated.h"

USTRUCT(BlueprintType)
struct FMHShadowDepthInterval
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float MinDepth = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float MaxDepth = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	bool bValid = false;
};

USTRUCT(BlueprintType)
struct FMHShadowNode
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FIntVector4 ChildIndices = FIntVector4(-1, -1, -1, -1);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 IntervalIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 Level = 0;
};

USTRUCT(BlueprintType)
struct FMHShadowBakeStats
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 RawTexelCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 ValidTexelCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 NodeCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 IntervalCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int64 RawBytes = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int64 CompressedBytes = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float CompressionRatio = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	double BakeSeconds = 0.0;
};

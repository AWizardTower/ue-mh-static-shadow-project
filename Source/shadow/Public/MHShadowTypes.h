// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MHShadowTypes.generated.h"

UENUM(BlueprintType)
enum class EMHShadowBakeSource : uint8
{
	CpuTraceDual,
	LightmassStaticDepth,
	LightmassDual,
};

UENUM(BlueprintType)
enum class EMHShadowProjectionMapping : uint8
{
	BasisRectDepth,
	LightmassWorldToShadowMatrix,
};

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

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float RepresentativeDepth = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float BoundsMinDepth = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float BoundsMaxDepth = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	bool bHasRepresentativeDepth = false;
};

USTRUCT(BlueprintType)
struct FMHShadowTile
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FIntPoint TileCoord = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FIntVector4 TexelRect = FIntVector4(0, 0, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 NodeOffset = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 NodeCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 RootNodeIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 PageIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	bool bResidentDefault = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	int32 RawTexelCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	int32 ValidTexelCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	int32 CompressedNodeCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	float CompressionRatio = 1.0f;
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

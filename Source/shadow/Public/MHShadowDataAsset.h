// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MHShadowTypes.h"
#include "MHShadowDataAsset.generated.h"

UCLASS(BlueprintType)
class SHADOW_API UMHShadowDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	EMHShadowBakeSource BakeSource = EMHShadowBakeSource::CpuTraceDual;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	EMHShadowProjectionMapping ProjectionMapping = EMHShadowProjectionMapping::BasisRectDepth;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FIntPoint Resolution = FIntPoint(0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 TileSize = 128;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Tiles")
	FIntPoint TileCount = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float DepthBias = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FVector LightOrigin = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FVector LightXAxis = FVector::ForwardVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FVector LightYAxis = FVector::RightVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FVector LightZAxis = FVector::UpVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FVector2D LightSpaceMin = FVector2D::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FVector2D LightSpaceMax = FVector2D::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float MinLightDepth = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float MaxLightDepth = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Lightmass")
	FVector4 WorldToShadowRow0 = FVector4(1, 0, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Lightmass")
	FVector4 WorldToShadowRow1 = FVector4(0, 1, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Lightmass")
	FVector4 WorldToShadowRow2 = FVector4(0, 0, 1, 0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Lightmass")
	FVector4 WorldToShadowRow3 = FVector4(0, 0, 0, 1);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Debug")
	TArray<FMHShadowDepthInterval> RawIntervals;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Debug")
	TArray<uint8> RawIntervalFlags;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	TArray<FMHShadowDepthInterval> Intervals;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	TArray<FMHShadowNode> Nodes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Tiles")
	TArray<FMHShadowTile> Tiles;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Tiles")
	TArray<int32> PageTable;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FMHShadowBakeStats Stats;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	TArray<FColor> DebugIntervalPreview;

	UFUNCTION(BlueprintCallable, Category = "MH Shadow")
	bool IsValidForRendering() const;
};

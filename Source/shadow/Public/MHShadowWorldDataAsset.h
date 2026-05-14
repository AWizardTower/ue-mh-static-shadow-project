// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MHShadowTypes.h"
#include "MHShadowWorldDataAsset.generated.h"

class UMHShadowCellDataAsset;

struct FMHShadowCellFlattenedData
{
	EMHShadowBakeSource BakeSource = EMHShadowBakeSource::LightmassDual;
	EMHShadowProjectionMapping ProjectionMapping = EMHShadowProjectionMapping::LightmassWorldToShadowMatrix;
	FIntPoint Resolution = FIntPoint::ZeroValue;
	int32 TileSize = 128;
	FIntPoint TileCount = FIntPoint::ZeroValue;
	float DepthBias = 0.001f;
	FVector4 WorldToShadowRow0 = FVector4(1, 0, 0, 0);
	FVector4 WorldToShadowRow1 = FVector4(0, 1, 0, 0);
	FVector4 WorldToShadowRow2 = FVector4(0, 0, 1, 0);
	FVector4 WorldToShadowRow3 = FVector4(0, 0, 0, 1);
	TArray<FMHShadowClipmapLevel> ClipmapLevels;
	TArray<FMHShadowTile> ClipmapTiles;
	TArray<FMHShadowNode> ClipmapNodes;
	TArray<int32> ClipmapPageTable;
	int64 RawDualBytes = 0;
	int32 LoadedCellCount = 0;
	int32 LoadedVirtualPageCount = 0;
	int32 UnavailableVirtualPageCount = 0;
	int64 ProviderMemoryBytes = 0;
	int64 LargestLoadedCellBytes = 0;
};

UCLASS(BlueprintType)
class SHADOW_API UMHShadowWorldDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	EMHShadowBakeSource BakeSource = EMHShadowBakeSource::LightmassDual;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	EMHShadowProjectionMapping ProjectionMapping = EMHShadowProjectionMapping::LightmassWorldToShadowMatrix;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FIntPoint Resolution = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	int32 TileSize = 128;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	FIntPoint TileCount = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	float DepthBias = 0.001f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Lightmass")
	FVector4 WorldToShadowRow0 = FVector4(1, 0, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Lightmass")
	FVector4 WorldToShadowRow1 = FVector4(0, 1, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Lightmass")
	FVector4 WorldToShadowRow2 = FVector4(0, 0, 1, 0);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Lightmass")
	FVector4 WorldToShadowRow3 = FVector4(0, 0, 0, 1);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Clipmap")
	TArray<FMHShadowClipmapLevel> ClipmapLevels;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cells")
	float CellSize = 1000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cells")
	FIntPoint CellMinCoord = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cells")
	FIntPoint CellCount = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cells")
	TArray<TSoftObjectPtr<UMHShadowCellDataAsset>> CellAssets;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Source")
	FSoftObjectPath SourceMonolithicAsset;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	int32 TotalVirtualPages = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	int64 TotalRawDualBytes = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	int64 TotalCompressedBytes = 0;

	UFUNCTION(BlueprintCallable, Category = "MH Shadow|Cells")
	bool IsValidWorldData() const;

	bool BuildFlattenedData(const TArray<const UMHShadowCellDataAsset*>& LoadedCells, FMHShadowCellFlattenedData& OutData, FString* OutError = nullptr) const;
};

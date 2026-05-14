// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "MHShadowTypes.h"
#include "MHShadowCellDataAsset.generated.h"

class UMHShadowWorldDataAsset;

UCLASS(BlueprintType)
class SHADOW_API UMHShadowCellDataAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cell")
	TSoftObjectPtr<UMHShadowWorldDataAsset> WorldData;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cell")
	FIntPoint CellCoord = FIntPoint::ZeroValue;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cell")
	int32 CellIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cell")
	FBox WorldBounds = FBox(EForceInit::ForceInit);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cell")
	TArray<int32> GlobalClipmapTileIndices;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cell")
	TArray<FMHShadowTile> ClipmapTiles;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cell")
	TArray<FMHShadowNode> ClipmapNodes;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	int32 ValidTileCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	int32 TotalValidTexels = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MH Shadow|Stats")
	int64 EstimatedCompressedBytes = 0;

	UFUNCTION(BlueprintCallable, Category = "MH Shadow|Cell")
	bool IsValidCellData() const;
};

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MHShadowBakeVolume.generated.h"

class AActor;
class ADirectionalLight;
class UBoxComponent;

UCLASS(Blueprintable)
class SHADOW_API AMHShadowBakeVolume : public AActor
{
	GENERATED_BODY()

public:
	AMHShadowBakeVolume();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MH Shadow")
	TObjectPtr<UBoxComponent> BoundsComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow")
	TObjectPtr<ADirectionalLight> DirectionalLight = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow", meta = (ClampMin = "2", UIMin = "128", UIMax = "4096"))
	int32 Resolution = 512;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow", meta = (ClampMin = "16", UIMin = "64", UIMax = "512"))
	int32 TileSize = 128;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow", meta = (ClampMin = "0.0"))
	float DepthBias = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow", meta = (ClampMin = "0.0"))
	float TracePadding = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow")
	bool bStaticGeometryOnly = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow")
	FString DefaultOutputAssetPath = TEXT("/Game/MHShadow/Baked/MHShadowData");

	FBox GetBakeBounds() const;
};

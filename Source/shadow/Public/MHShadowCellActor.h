// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MHShadowCellActor.generated.h"

class UMHShadowCellComponent;

UCLASS()
class SHADOW_API AMHShadowCellActor : public AActor
{
	GENERATED_BODY()

public:
	AMHShadowCellActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MH Shadow|Cell")
	TObjectPtr<UMHShadowCellComponent> CellComponent;
};

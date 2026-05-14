// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "MHShadowWorldActor.generated.h"

class UMHShadowWorldComponent;

UCLASS()
class SHADOW_API AMHShadowWorldActor : public AActor
{
	GENERATED_BODY()

public:
	AMHShadowWorldActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "MH Shadow|World")
	TObjectPtr<UMHShadowWorldComponent> WorldComponent;
};

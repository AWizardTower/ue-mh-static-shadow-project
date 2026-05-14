// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCellActor.h"

#include "Components/SceneComponent.h"
#include "MHShadowCellComponent.h"

AMHShadowCellActor::AMHShadowCellActor()
{
	PrimaryActorTick.bCanEverTick = false;
	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));
	RootComponent = SceneRoot;
	CellComponent = CreateDefaultSubobject<UMHShadowCellComponent>(TEXT("MHShadowCell"));
}

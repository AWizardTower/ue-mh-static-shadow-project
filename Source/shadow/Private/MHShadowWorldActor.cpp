// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowWorldActor.h"

#include "Components/SceneComponent.h"
#include "MHShadowWorldComponent.h"

AMHShadowWorldActor::AMHShadowWorldActor()
{
	PrimaryActorTick.bCanEverTick = false;
	USceneComponent* SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("DefaultSceneRoot"));
	RootComponent = SceneRoot;
	WorldComponent = CreateDefaultSubobject<UMHShadowWorldComponent>(TEXT("MHShadowWorld"));
}

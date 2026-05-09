// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowBakeVolume.h"

#include "Components/BoxComponent.h"

AMHShadowBakeVolume::AMHShadowBakeVolume()
{
	PrimaryActorTick.bCanEverTick = false;

	BoundsComponent = CreateDefaultSubobject<UBoxComponent>(TEXT("Bounds"));
	SetRootComponent(BoundsComponent);
	BoundsComponent->SetBoxExtent(FVector(500.0, 500.0, 300.0));
	BoundsComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

FBox AMHShadowBakeVolume::GetBakeBounds() const
{
	if (!BoundsComponent)
	{
		return GetComponentsBoundingBox();
	}

	return FBox::BuildAABB(
		BoundsComponent->GetComponentLocation(),
		BoundsComponent->GetScaledBoxExtent());
}

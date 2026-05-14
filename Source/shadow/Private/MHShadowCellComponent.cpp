// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCellComponent.h"

#include "MHShadowCellDataAsset.h"
#include "MHShadowWorldComponent.h"
#include "UObject/UnrealType.h"

UMHShadowCellComponent::UMHShadowCellComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UMHShadowCellComponent::OnRegister()
{
	Super::OnRegister();
	NotifyCellChanged();
}

void UMHShadowCellComponent::OnUnregister()
{
	NotifyCellChanged();
	Super::OnUnregister();
}

void UMHShadowCellComponent::BeginPlay()
{
	Super::BeginPlay();
	NotifyCellChanged();
}

void UMHShadowCellComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	NotifyCellChanged();
	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void UMHShadowCellComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UMHShadowCellComponent, CellData)
		|| PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UMHShadowCellComponent, bAutoRegisterCell))
	{
		NotifyCellChanged();
	}
}
#endif

bool UMHShadowCellComponent::IsCellProviderActive() const
{
	return bAutoRegisterCell && CellData && CellData->IsValidCellData() && IsRegistered();
}

void UMHShadowCellComponent::NotifyCellChanged()
{
	if (UWorld* World = GetWorld())
	{
		UMHShadowWorldComponent::NotifyCellsChanged(World);
	}
}

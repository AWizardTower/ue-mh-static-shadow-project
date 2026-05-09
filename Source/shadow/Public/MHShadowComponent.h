// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MHShadowComponent.generated.h"

class UMHShadowDataAsset;

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class SHADOW_API UMHShadowComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMHShadowComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow")
	TObjectPtr<UMHShadowDataAsset> ShadowData = nullptr;

	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UFUNCTION(BlueprintCallable, Category = "MH Shadow")
	void RegisterShadowData();

	UFUNCTION(BlueprintCallable, Category = "MH Shadow")
	void UnregisterShadowData();

private:
	uint64 RegistrationId = 0;
	bool bRegisteredWithRenderer = false;
};

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MHShadowWorldComponent.generated.h"

class UMHShadowWorldDataAsset;

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class SHADOW_API UMHShadowWorldComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMHShadowWorldComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow|World")
	TObjectPtr<UMHShadowWorldDataAsset> WorldData = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow|World")
	bool bAutoRegisterWorld = true;

	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UFUNCTION(BlueprintCallable, Category = "MH Shadow|World")
	void RegisterShadowData();

	UFUNCTION(BlueprintCallable, Category = "MH Shadow|World")
	void UnregisterShadowData();

	UFUNCTION(BlueprintCallable, Category = "MH Shadow|World")
	void RebuildFromLoadedCells();

	static void NotifyCellsChanged(UWorld* World);

private:
	uint64 RegistrationId = 0;
	bool bRegisteredWithRenderer = false;
};

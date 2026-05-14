// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "MHShadowCellComponent.generated.h"

class UMHShadowCellDataAsset;

UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class SHADOW_API UMHShadowCellComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UMHShadowCellComponent();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow|Cell")
	TObjectPtr<UMHShadowCellDataAsset> CellData = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "MH Shadow|Cell")
	bool bAutoRegisterCell = true;

	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	UFUNCTION(BlueprintCallable, Category = "MH Shadow|Cell")
	bool IsCellProviderActive() const;

	UFUNCTION(BlueprintCallable, Category = "MH Shadow|Cell")
	void NotifyCellChanged();
};

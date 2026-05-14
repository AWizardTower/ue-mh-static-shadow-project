// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowCreateLargeCacheStressMapCommandlet.generated.h"

UCLASS()
class UMHShadowCreateLargeCacheStressMapCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowCreateLargeCacheStressMapCommandlet();

	virtual int32 Main(const FString& Params) override;
};

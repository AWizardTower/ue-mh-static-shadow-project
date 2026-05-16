// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowCreateClipmapBakeMapsCommandlet.generated.h"

UCLASS()
class UMHShadowCreateClipmapBakeMapsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowCreateClipmapBakeMapsCommandlet();

	virtual int32 Main(const FString& Params) override;
};

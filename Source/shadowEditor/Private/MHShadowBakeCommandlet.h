// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowBakeCommandlet.generated.h"

UCLASS()
class UMHShadowBakeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowBakeCommandlet();
	virtual int32 Main(const FString& Params) override;
};

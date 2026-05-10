// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "MHShadowCreateDualCubeValidationMapCommandlet.generated.h"

UCLASS()
class UMHShadowCreateDualCubeValidationMapCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowCreateDualCubeValidationMapCommandlet();

	virtual int32 Main(const FString& Params) override;
};

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "MHShadowValidateCellSplitCommandlet.generated.h"

UCLASS()
class UMHShadowValidateCellSplitCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowValidateCellSplitCommandlet();

	virtual int32 Main(const FString& Params) override;
};

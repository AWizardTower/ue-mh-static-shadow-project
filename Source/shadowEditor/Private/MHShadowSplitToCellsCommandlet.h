// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "MHShadowSplitToCellsCommandlet.generated.h"

UCLASS()
class UMHShadowSplitToCellsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowSplitToCellsCommandlet();

	virtual int32 Main(const FString& Params) override;
};

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commandlets/Commandlet.h"
#include "MHShadowPopulateCellActorsCommandlet.generated.h"

UCLASS()
class UMHShadowPopulateCellActorsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowPopulateCellActorsCommandlet();

	virtual int32 Main(const FString& Params) override;
};

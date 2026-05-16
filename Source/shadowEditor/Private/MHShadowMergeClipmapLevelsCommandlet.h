// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowMergeClipmapLevelsCommandlet.generated.h"

UCLASS()
class UMHShadowMergeClipmapLevelsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowMergeClipmapLevelsCommandlet();

	virtual int32 Main(const FString& Params) override;
};

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowInspectClipmapLevelsCommandlet.generated.h"

UCLASS()
class UMHShadowInspectClipmapLevelsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowInspectClipmapLevelsCommandlet();

	virtual int32 Main(const FString& Params) override;
};

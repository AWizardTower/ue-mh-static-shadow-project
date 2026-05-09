// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowInspectDataCommandlet.generated.h"

UCLASS()
class UMHShadowInspectDataCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowInspectDataCommandlet();
	virtual int32 Main(const FString& Params) override;
};

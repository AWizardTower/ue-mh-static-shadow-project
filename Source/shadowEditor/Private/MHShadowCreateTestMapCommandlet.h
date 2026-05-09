// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowCreateTestMapCommandlet.generated.h"

UCLASS()
class UMHShadowCreateTestMapCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowCreateTestMapCommandlet();
	virtual int32 Main(const FString& Params) override;
};

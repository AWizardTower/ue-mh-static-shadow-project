// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowImportLightmassDualClipmapCommandlet.generated.h"

UCLASS()
class UMHShadowImportLightmassDualClipmapCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowImportLightmassDualClipmapCommandlet();

	virtual int32 Main(const FString& Params) override;
};

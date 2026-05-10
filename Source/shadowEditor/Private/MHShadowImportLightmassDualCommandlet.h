// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowImportLightmassDualCommandlet.generated.h"

UCLASS()
class UMHShadowImportLightmassDualCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowImportLightmassDualCommandlet();

	virtual int32 Main(const FString& Params) override;
};

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MHShadowValidateCompressionCommandlet.generated.h"

UCLASS()
class UMHShadowValidateCompressionCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UMHShadowValidateCompressionCommandlet();
	virtual int32 Main(const FString& Params) override;
};

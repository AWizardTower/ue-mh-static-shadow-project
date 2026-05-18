// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowImportLightmassDualClipmapCommandlet.h"

#include "MHShadowImportLightmassDualCommandlet.h"

UMHShadowImportLightmassDualClipmapCommandlet::UMHShadowImportLightmassDualClipmapCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowImportLightmassDualClipmapCommandlet::Main(const FString& Params)
{
	return RunMHShadowImportLightmassDualCommandlet(Params);
}

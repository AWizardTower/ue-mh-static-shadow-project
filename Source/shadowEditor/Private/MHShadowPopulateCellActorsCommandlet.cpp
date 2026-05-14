// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowPopulateCellActorsCommandlet.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "MHShadowCellActor.h"
#include "MHShadowCellComponent.h"
#include "MHShadowCellDataAsset.h"
#include "MHShadowComponent.h"
#include "MHShadowWorldActor.h"
#include "MHShadowWorldComponent.h"
#include "MHShadowWorldDataAsset.h"
#include "Misc/PackageName.h"

namespace
{
static bool ParseStringParam(const FString& Params, const TCHAR* Key, FString& OutValue)
{
	if (FParse::Value(*Params, Key, OutValue))
	{
		OutValue.TrimStartAndEndInline();
		return !OutValue.IsEmpty();
	}
	return false;
}

static FString ToObjectPath(const FString& AssetPath)
{
	if (AssetPath.Contains(TEXT(".")))
	{
		return AssetPath;
	}

	const FString AssetName = FPackageName::GetShortName(AssetPath);
	return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

static void SetLabel(AActor* Actor, const FString& Label)
{
#if WITH_EDITOR
	if (Actor)
	{
		Actor->SetActorLabel(Label);
	}
#endif
}

static FVector GetCellActorLocation(const UMHShadowWorldDataAsset& WorldData, const UMHShadowCellDataAsset& CellData)
{
	if (CellData.WorldBounds.IsValid)
	{
		return CellData.WorldBounds.GetCenter();
	}

	return FVector(
		(static_cast<double>(CellData.CellCoord.X) + 0.5) * WorldData.CellSize,
		(static_cast<double>(CellData.CellCoord.Y) + 0.5) * WorldData.CellSize,
		0.0);
}
}

UMHShadowPopulateCellActorsCommandlet::UMHShadowPopulateCellActorsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowPopulateCellActorsCommandlet::Main(const FString& Params)
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Error, TEXT("No editor available for MH shadow cell actor population."));
		return 1;
	}

	FString MapPath = TEXT("/Game/MHShadow/Test/MHShadowLargeCacheStress");
	FString WorldPath = TEXT("/Game/MHShadow/Baked/LargeCacheStressCells/MHShadowWorld_LargeCacheStress");
	ParseStringParam(Params, TEXT("Map="), MapPath);
	ParseStringParam(Params, TEXT("World="), WorldPath);

	UMHShadowWorldDataAsset* WorldData = LoadObject<UMHShadowWorldDataAsset>(nullptr, *ToObjectPath(WorldPath));
	if (!WorldData || !WorldData->IsValidWorldData())
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid MH shadow world data asset: %s"), *WorldPath);
		return 1;
	}

	const FString MapFilename = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	if (!FEditorFileUtils::LoadMap(MapFilename, false, true))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load map for MH cell actor population: %s (%s)"), *MapPath, *MapFilename);
		return 1;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("No editor world after loading map: %s"), *MapPath);
		return 1;
	}

	TArray<AActor*> ActorsToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->IsA<AMHShadowWorldActor>() || Actor->IsA<AMHShadowCellActor>())
		{
			ActorsToDestroy.Add(Actor);
			continue;
		}

		TArray<UMHShadowComponent*> LegacyComponents;
		Actor->GetComponents<UMHShadowComponent>(LegacyComponents);
		for (UMHShadowComponent* LegacyComponent : LegacyComponents)
		{
			if (LegacyComponent)
			{
				LegacyComponent->UnregisterShadowData();
				LegacyComponent->ShadowData = nullptr;
			}
		}
	}

	for (AActor* Actor : ActorsToDestroy)
	{
		World->DestroyActor(Actor);
	}

	FActorSpawnParameters SpawnParams;
	AMHShadowWorldActor* WorldActor = World->SpawnActor<AMHShadowWorldActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (!WorldActor || !WorldActor->WorldComponent)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to spawn MH shadow world actor."));
		return 1;
	}
	SetLabel(WorldActor, TEXT("MH_CellProviderWorld"));
	WorldActor->WorldComponent->WorldData = WorldData;
	WorldActor->WorldComponent->bAutoRegisterWorld = false;

	int32 SpawnedCellActors = 0;
	int64 LoadedProviderBytes = 0;
	for (const TSoftObjectPtr<UMHShadowCellDataAsset>& CellRef : WorldData->CellAssets)
	{
		UMHShadowCellDataAsset* CellData = CellRef.LoadSynchronous();
		if (!CellData || !CellData->IsValidCellData())
		{
			UE_LOG(LogTemp, Warning, TEXT("Skipping invalid MH shadow cell data while populating actors: %s"), *CellRef.ToSoftObjectPath().ToString());
			continue;
		}

		const FString ActorName = FString::Printf(TEXT("MH_CellProvider_%d_%d_%d"), CellData->CellIndex, CellData->CellCoord.X, CellData->CellCoord.Y).Replace(TEXT("-"), TEXT("N"));
		FActorSpawnParameters CellSpawnParams;
		AMHShadowCellActor* CellActor = World->SpawnActor<AMHShadowCellActor>(GetCellActorLocation(*WorldData, *CellData), FRotator::ZeroRotator, CellSpawnParams);
		if (!CellActor || !CellActor->CellComponent)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to spawn MH shadow cell actor for %s"), *CellData->GetPathName());
			return 1;
		}

		SetLabel(CellActor, ActorName);
		CellActor->CellComponent->CellData = CellData;
		CellActor->CellComponent->bAutoRegisterCell = true;
		LoadedProviderBytes += CellData->EstimatedCompressedBytes;
		++SpawnedCellActors;
	}

	WorldActor->WorldComponent->bAutoRegisterWorld = true;
	WorldActor->WorldComponent->RegisterShadowData();
	World->UpdateWorldComponents(true, false);
	World->MarkPackageDirty();

	if (!FEditorFileUtils::SaveMap(World, MapFilename))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save populated MH shadow cell actor map: %s"), *MapFilename);
		return 1;
	}

	UE_LOG(LogTemp, Display, TEXT("Populated MH shadow cell actors: map=%s world=%s cells=%d providerBytes=%lld disabledLegacyComponents=yes"),
		*MapPath,
		*WorldPath,
		SpawnedCellActors,
		LoadedProviderBytes);
	return 0;
}

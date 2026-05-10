// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCreateDualCubeValidationMapCommandlet.h"

#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Components/DirectionalLightComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/WorldSettings.h"
#include "Lightmass/LightmassImportanceVolume.h"
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

static void SetLabel(AActor* Actor, const TCHAR* Label)
{
#if WITH_EDITOR
	if (Actor)
	{
		Actor->SetActorLabel(Label);
	}
#endif
}
}

UMHShadowCreateDualCubeValidationMapCommandlet::UMHShadowCreateDualCubeValidationMapCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowCreateDualCubeValidationMapCommandlet::Main(const FString& Params)
{
	FString MapPath = TEXT("/Game/MHShadow/Test/MHShadowDualCubeValidation");
	ParseStringParam(Params, TEXT("Map="), MapPath);
	if (!FPackageName::IsValidLongPackageName(MapPath))
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid map package path: %s"), *MapPath);
		return 1;
	}

	UWorld* World = GEditor ? GEditor->NewMap(false) : nullptr;
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create a new editor map."));
		return 1;
	}

	UE_LOG(LogTemp, Display, TEXT("Validation map partitioned world: %d"), World->IsPartitionedWorld() ? 1 : 0);

	if (AWorldSettings* WorldSettings = World->GetWorldSettings())
	{
		WorldSettings->bForceNoPrecomputedLighting = false;
		WorldSettings->LightmassSettings.StaticLightingLevelScale = 1.0f;
		WorldSettings->LightmassSettings.NumIndirectLightingBounces = 1;
	}

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load /Engine/BasicShapes/Cube.Cube."));
		return 1;
	}

	const FTransform CubeTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector(1.0, 1.0, 1.0));
	AStaticMeshActor* CubeActor = Cast<AStaticMeshActor>(GEditor->AddActor(World->GetCurrentLevel(), AStaticMeshActor::StaticClass(), CubeTransform));
	if (!CubeActor)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to add validation cube actor."));
		return 1;
	}
	SetLabel(CubeActor, TEXT("MH_ClosedCube_Static"));
	CubeActor->SetMobility(EComponentMobility::Static);
	CubeActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
	CubeActor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Static);
	CubeActor->GetStaticMeshComponent()->SetCastShadow(true);
	CubeActor->GetStaticMeshComponent()->bCastStaticShadow = true;

	const FTransform LightTransform(FRotator(-45.0, -35.0, 0.0), FVector(-300.0, -300.0, 400.0));
	ADirectionalLight* DirectionalLight = Cast<ADirectionalLight>(GEditor->AddActor(World->GetCurrentLevel(), ADirectionalLight::StaticClass(), LightTransform));
	if (!DirectionalLight)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to add validation directional light."));
		return 1;
	}
	SetLabel(DirectionalLight, TEXT("MH_DirectionalLight_Stationary"));
	if (UDirectionalLightComponent* LightComponent = Cast<UDirectionalLightComponent>(DirectionalLight->GetLightComponent()))
	{
		LightComponent->SetMobility(EComponentMobility::Stationary);
		LightComponent->SetIntensity(6.0f);
		LightComponent->SetCastShadows(true);
		LightComponent->CastStaticShadows = true;
		LightComponent->CastDynamicShadows = true;
	}

	ALightmassImportanceVolume* ImportanceVolume = Cast<ALightmassImportanceVolume>(GEditor->AddActor(World->GetCurrentLevel(), ALightmassImportanceVolume::StaticClass(), FTransform::Identity));
	if (!ImportanceVolume)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to add Lightmass Importance Volume."));
		return 1;
	}
	SetLabel(ImportanceVolume, TEXT("MH_LightmassImportanceVolume"));
	UCubeBuilder* VolumeBuilder = NewObject<UCubeBuilder>(ImportanceVolume);
	VolumeBuilder->X = 420.0f;
	VolumeBuilder->Y = 420.0f;
	VolumeBuilder->Z = 420.0f;
	VolumeBuilder->Hollow = false;
	VolumeBuilder->Tessellated = false;
	UActorFactory::CreateBrushForVolumeActor(ImportanceVolume, VolumeBuilder);
	ImportanceVolume->SetActorLocation(FVector::ZeroVector);
	ImportanceVolume->SetActorScale3D(FVector(4.0, 4.0, 4.0));
	ImportanceVolume->ReregisterAllComponents();
	ImportanceVolume->PostEditChange();
	UE_LOG(LogTemp, Display, TEXT("Validation Lightmass Importance Volume bounds: %s"), *ImportanceVolume->GetComponentsBoundingBox(true).ToString());

	const FString Filename = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	World->MarkPackageDirty();
	if (!FEditorFileUtils::SaveMap(World, Filename))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save validation map: %s"), *Filename);
		return 1;
	}

	UE_LOG(LogTemp, Display, TEXT("Saved MH dual cube validation map: %s"), *MapPath);
	return 0;
}

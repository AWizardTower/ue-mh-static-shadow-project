// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCreateLargeCacheStressMapCommandlet.h"

#include "ActorFactories/ActorFactory.h"
#include "Builders/CubeBuilder.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "Lightmass/LightmassImportanceVolume.h"
#include "MHShadowComponent.h"
#include "MHShadowDataAsset.h"
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

static int32 ParseIntParam(const FString& Params, const TCHAR* Key, int32 DefaultValue)
{
	int32 Value = DefaultValue;
	FParse::Value(*Params, Key, Value);
	return Value;
}

static float ParseFloatParam(const FString& Params, const TCHAR* Key, float DefaultValue)
{
	float Value = DefaultValue;
	FParse::Value(*Params, Key, Value);
	return Value;
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

static AStaticMeshActor* SpawnStaticCube(
	UWorld& World,
	UStaticMesh& CubeMesh,
	const FString& Name,
	const FVector& Location,
	const FVector& Scale,
	const FRotator& Rotation = FRotator::ZeroRotator)
{
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = MakeUniqueObjectName(&World, AStaticMeshActor::StaticClass(), FName(*Name));

	AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(Location, Rotation, SpawnParams);
	if (!Actor)
	{
		return nullptr;
	}

	SetLabel(Actor, *Name);
	Actor->SetMobility(EComponentMobility::Static);
	Actor->SetActorScale3D(Scale);

	UStaticMeshComponent* MeshComponent = Actor->GetStaticMeshComponent();
	MeshComponent->SetStaticMesh(&CubeMesh);
	MeshComponent->SetMobility(EComponentMobility::Static);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	MeshComponent->SetCollisionObjectType(ECC_WorldStatic);
	MeshComponent->SetCollisionResponseToAllChannels(ECR_Block);
	MeshComponent->SetCastShadow(true);
	MeshComponent->bCastStaticShadow = true;
	return Actor;
}

static double Deterministic01(int32 X, int32 Y, int32 Salt)
{
	const int32 Value = FMath::Abs((X * 73856093) ^ (Y * 19349663) ^ (Salt * 83492791));
	return double(Value % 1000) / 999.0;
}
}

UMHShadowCreateLargeCacheStressMapCommandlet::UMHShadowCreateLargeCacheStressMapCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowCreateLargeCacheStressMapCommandlet::Main(const FString& Params)
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Error, TEXT("No editor is available for MH large cache stress map creation."));
		return 1;
	}

	FString MapPath = TEXT("/Game/MHShadow/Test/MHShadowLargeCacheStress");
	ParseStringParam(Params, TEXT("Map="), MapPath);
	if (!FPackageName::IsValidLongPackageName(MapPath))
	{
		UE_LOG(LogTemp, Error, TEXT("Invalid map package path: %s"), *MapPath);
		return 1;
	}

	const int32 CellsX = FMath::Clamp(ParseIntParam(Params, TEXT("CellsX="), 8), 1, 32);
	const int32 CellsY = FMath::Clamp(ParseIntParam(Params, TEXT("CellsY="), 8), 1, 32);
	const float CellSize = FMath::Clamp(ParseFloatParam(Params, TEXT("CellSize="), 1000.0f), 300.0f, 3000.0f);
	const FString PlannedAssetPath = TEXT("/Game/MHShadow/Baked/MHShadowData_LightmassDual_LargeCacheStress_4096_Clipmap_Final");

	UWorld* World = GEditor->NewMap(false);
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create a new editor map."));
		return 1;
	}

	if (AWorldSettings* WorldSettings = World->GetWorldSettings())
	{
		WorldSettings->bForceNoPrecomputedLighting = false;
		WorldSettings->LightmassSettings.StaticLightingLevelScale = 1.0f;
		WorldSettings->LightmassSettings.NumIndirectLightingBounces = 1;
		WorldSettings->LightmassSettings.IndirectLightingQuality = 1.0f;
	}

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load /Engine/BasicShapes/Cube.Cube."));
		return 1;
	}

	const double OriginX = -0.5 * double(CellsX - 1) * double(CellSize);
	const double OriginY = -0.5 * double(CellsY - 1) * double(CellSize);
	int32 MeshActorCount = 0;

	for (int32 Y = 0; Y < CellsY; ++Y)
	{
		for (int32 X = 0; X < CellsX; ++X)
		{
			const double CenterX = OriginX + double(X) * double(CellSize);
			const double CenterY = OriginY + double(Y) * double(CellSize);
			const FString Prefix = FString::Printf(TEXT("MH_Cell_%02d_%02d"), X, Y);

			if (SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_Ground"), FVector(CenterX, CenterY, -10.0), FVector(CellSize / 100.0f, CellSize / 100.0f, 0.2f)))
			{
				++MeshActorCount;
			}

			const int32 WallMode = (X + Y) & 3;
			const float WallHeight = 260.0f + 140.0f * static_cast<float>(Deterministic01(X, Y, 11));
			const float WallZ = WallHeight * 0.5f;
			if (WallMode == 0)
			{
				MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_Wall_N"), FVector(CenterX, CenterY + CellSize * 0.36f, WallZ), FVector(CellSize * 0.78f / 100.0f, 0.28f, WallHeight / 100.0f)) ? 1 : 0;
			}
			else if (WallMode == 1)
			{
				MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_Wall_E"), FVector(CenterX + CellSize * 0.36f, CenterY, WallZ), FVector(0.28f, CellSize * 0.78f / 100.0f, WallHeight / 100.0f)) ? 1 : 0;
			}
			else if (WallMode == 2)
			{
				MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_Wall_S"), FVector(CenterX, CenterY - CellSize * 0.36f, WallZ), FVector(CellSize * 0.78f / 100.0f, 0.28f, WallHeight / 100.0f)) ? 1 : 0;
			}
			else
			{
				MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_Wall_W"), FVector(CenterX - CellSize * 0.36f, CenterY, WallZ), FVector(0.28f, CellSize * 0.78f / 100.0f, WallHeight / 100.0f)) ? 1 : 0;
			}

			const float PillarHeight = 220.0f + 250.0f * static_cast<float>(Deterministic01(X, Y, 23));
			const float PillarX = CenterX + (static_cast<float>(Deterministic01(X, Y, 31)) - 0.5f) * CellSize * 0.42f;
			const float PillarY = CenterY + (static_cast<float>(Deterministic01(X, Y, 37)) - 0.5f) * CellSize * 0.42f;
			MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_Pillar"), FVector(PillarX, PillarY, PillarHeight * 0.5f), FVector(0.55f, 0.55f, PillarHeight / 100.0f)) ? 1 : 0;

			if (((X + 2 * Y) % 3) == 0)
			{
				const float RoofYaw = ((X + Y) & 1) ? 90.0f : 0.0f;
				MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_RoofSlab"), FVector(CenterX, CenterY, 320.0f), FVector(CellSize * 0.52f / 100.0f, CellSize * 0.32f / 100.0f, 0.18f), FRotator(0.0f, RoofYaw, 0.0f)) ? 1 : 0;
			}

			if (((2 * X + Y) % 4) == 0)
			{
				const float PlateYaw = ((X * 5 + Y * 7) % 180) - 90.0f;
				const FVector PlateLocation(CenterX - CellSize * 0.18f, CenterY + CellSize * 0.16f, 175.0f);
				MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_ThinPlate"), PlateLocation, FVector(0.10f, CellSize * 0.55f / 100.0f, 2.8f), FRotator(0.0f, PlateYaw, 0.0f)) ? 1 : 0;
			}

			if (Y % 2 == 0 && X < CellsX - 1)
			{
				MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_Bridge"), FVector(CenterX + CellSize * 0.5f, CenterY, 430.0f), FVector(CellSize * 0.82f / 100.0f, 0.18f, 0.22f), FRotator(0.0f, 0.0f, 0.0f)) ? 1 : 0;
			}
		}
	}

	ADirectionalLight* DirectionalLight = World->SpawnActor<ADirectionalLight>(
		FVector(0.0, 0.0, 1400.0),
		FRotator(-50.0, -35.0, 0.0));
	if (!DirectionalLight)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create stress directional light."));
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
	VolumeBuilder->X = CellsX * CellSize + CellSize * 2.0f;
	VolumeBuilder->Y = CellsY * CellSize + CellSize * 2.0f;
	VolumeBuilder->Z = 1400.0f;
	VolumeBuilder->Hollow = false;
	VolumeBuilder->Tessellated = false;
	UActorFactory::CreateBrushForVolumeActor(ImportanceVolume, VolumeBuilder);
	ImportanceVolume->SetActorLocation(FVector::UpVector * 550.0f);
	ImportanceVolume->ReregisterAllComponents();
	ImportanceVolume->PostEditChange();

	AActor* RuntimeActor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!RuntimeActor)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create MH runtime shadow data actor."));
		return 1;
	}
	SetLabel(RuntimeActor, TEXT("MH_RuntimeShadowData"));
	UMHShadowComponent* MHComponent = NewObject<UMHShadowComponent>(RuntimeActor, TEXT("MHShadow"));
	RuntimeActor->AddInstanceComponent(MHComponent);
	MHComponent->RegisterComponent();
	if (UMHShadowDataAsset* ExistingAsset = LoadObject<UMHShadowDataAsset>(nullptr, *(PlannedAssetPath + TEXT(".MHShadowData_LightmassDual_LargeCacheStress_4096_Clipmap_Final"))))
	{
		MHComponent->ShadowData = ExistingAsset;
	}

	APlayerStart* PlayerStart = World->SpawnActor<APlayerStart>(FVector(-CellsX * CellSize * 0.5f, -CellsY * CellSize * 0.5f, 420.0f), FRotator(0.0f, 45.0f, 0.0f));
	SetLabel(PlayerStart, TEXT("MH_PlayerStart"));

	World->UpdateWorldComponents(true, false);
	World->MarkPackageDirty();

	const FString Filename = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	if (!FEditorFileUtils::SaveMap(World, Filename))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save large cache stress map: %s"), *Filename);
		return 1;
	}

	UE_LOG(LogTemp, Display, TEXT("Saved MH large cache stress map: %s"), *MapPath);
	UE_LOG(LogTemp, Display, TEXT("Stress scene cells=%dx%d cellSize=%.1f meshActors=%d boundsApprox=%.1fx%.1f plannedAsset=%s"),
		CellsX,
		CellsY,
		CellSize,
		MeshActorCount,
		CellsX * CellSize,
		CellsY * CellSize,
		*PlannedAssetPath);
	return 0;
}

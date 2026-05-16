// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCreateClipmapBakeMapsCommandlet.h"

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

static bool BuildClipmapBakeWorld(
	const FString& MapPath,
	int32 LevelIndex,
	float Coverage,
	int32 CellsX,
	int32 CellsY,
	float CellSize,
	float VolumeHeight,
	float VolumeZ)
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Error, TEXT("No editor is available for MH clipmap bake map creation."));
		return false;
	}

	UWorld* World = GEditor->NewMap(false);
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create clipmap bake map world: %s"), *MapPath);
		return false;
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
		return false;
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
			const FString Prefix = FString::Printf(TEXT("MH_CL%d_Cell_%02d_%02d"), LevelIndex, X, Y);

			MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_Ground"), FVector(CenterX, CenterY, -10.0), FVector(CellSize / 100.0f, CellSize / 100.0f, 0.2f)) ? 1 : 0;

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
				MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_ThinPlate"), FVector(CenterX - CellSize * 0.18f, CenterY + CellSize * 0.16f, 175.0f), FVector(0.10f, CellSize * 0.55f / 100.0f, 2.8f), FRotator(0.0f, PlateYaw, 0.0f)) ? 1 : 0;
			}

			if (Y % 2 == 0 && X < CellsX - 1)
			{
				MeshActorCount += SpawnStaticCube(*World, *CubeMesh, Prefix + TEXT("_Bridge"), FVector(CenterX + CellSize * 0.5f, CenterY, 430.0f), FVector(CellSize * 0.82f / 100.0f, 0.18f, 0.22f)) ? 1 : 0;
			}
		}
	}

	ADirectionalLight* DirectionalLight = World->SpawnActor<ADirectionalLight>(
		FVector(0.0, 0.0, 1400.0),
		FRotator(-50.0, -35.0, 0.0));
	if (!DirectionalLight)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create clipmap directional light."));
		return false;
	}
	SetLabel(DirectionalLight, *FString::Printf(TEXT("MH_DirectionalLight_CL%d"), LevelIndex));
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
		return false;
	}
	SetLabel(ImportanceVolume, *FString::Printf(TEXT("MH_LightmassImportanceVolume_CL%d"), LevelIndex));
	UCubeBuilder* VolumeBuilder = NewObject<UCubeBuilder>(ImportanceVolume);
	VolumeBuilder->X = Coverage;
	VolumeBuilder->Y = Coverage;
	VolumeBuilder->Z = VolumeHeight;
	VolumeBuilder->Hollow = false;
	VolumeBuilder->Tessellated = false;
	UActorFactory::CreateBrushForVolumeActor(ImportanceVolume, VolumeBuilder);
	ImportanceVolume->SetActorLocation(FVector(0.0, 0.0, VolumeZ));
	ImportanceVolume->ReregisterAllComponents();
	ImportanceVolume->PostEditChange();

	APlayerStart* PlayerStart = World->SpawnActor<APlayerStart>(FVector(-Coverage * 0.42f, -Coverage * 0.42f, 420.0f), FRotator(0.0f, 45.0f, 0.0f));
	SetLabel(PlayerStart, TEXT("MH_PlayerStart"));

	World->UpdateWorldComponents(true, false);
	World->MarkPackageDirty();

	const FString Filename = FPackageName::LongPackageNameToFilename(MapPath, FPackageName::GetMapPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
	if (!FEditorFileUtils::SaveMap(World, Filename))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save clipmap bake map: %s"), *Filename);
		return false;
	}

	UE_LOG(LogTemp, Display, TEXT("Saved MH clipmap bake map: %s level=%d coverage=%.1f texelTarget=%.3fcm meshActors=%d"),
		*MapPath,
		LevelIndex,
		Coverage,
		Coverage / 4096.0f,
		MeshActorCount);
	return true;
}
}

UMHShadowCreateClipmapBakeMapsCommandlet::UMHShadowCreateClipmapBakeMapsCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowCreateClipmapBakeMapsCommandlet::Main(const FString& Params)
{
	FString MapPrefix = TEXT("/Game/MHShadow/Test/MHShadowLargeCacheStress_CL");
	ParseStringParam(Params, TEXT("MapPrefix="), MapPrefix);

	const int32 LevelCount = FMath::Clamp(ParseIntParam(Params, TEXT("LevelCount="), 3), 1, 8);
	const int32 CellsX = FMath::Clamp(ParseIntParam(Params, TEXT("CellsX="), 8), 1, 32);
	const int32 CellsY = FMath::Clamp(ParseIntParam(Params, TEXT("CellsY="), 8), 1, 32);
	const float CellSize = FMath::Clamp(ParseFloatParam(Params, TEXT("CellSize="), 1000.0f), 300.0f, 3000.0f);
	const float Level0Coverage = FMath::Clamp(ParseFloatParam(Params, TEXT("Level0Coverage="), 4096.0f), 512.0f, 65536.0f);
	const float CoverageScale = FMath::Max(ParseFloatParam(Params, TEXT("CoverageScale="), 2.0f), 1.0001f);
	const float VolumeHeight = FMath::Clamp(ParseFloatParam(Params, TEXT("VolumeHeight="), 1800.0f), 300.0f, 20000.0f);
	const float VolumeZ = ParseFloatParam(Params, TEXT("VolumeZ="), 650.0f);

	for (int32 LevelIndex = 0; LevelIndex < LevelCount; ++LevelIndex)
	{
		const FString MapPath = FString::Printf(TEXT("%s%d"), *MapPrefix, LevelIndex);
		if (!FPackageName::IsValidLongPackageName(MapPath))
		{
			UE_LOG(LogTemp, Error, TEXT("Invalid clipmap bake map package path: %s"), *MapPath);
			return 1;
		}

		const float Coverage = Level0Coverage * FMath::Pow(CoverageScale, static_cast<float>(LevelIndex));
		if (!BuildClipmapBakeWorld(MapPath, LevelIndex, Coverage, CellsX, CellsY, CellSize, VolumeHeight, VolumeZ))
		{
			return 1;
		}
	}

	UE_LOG(LogTemp, Display, TEXT("Created %d MH real clipmap bake maps with prefix %s."), LevelCount, *MapPrefix);
	return 0;
}

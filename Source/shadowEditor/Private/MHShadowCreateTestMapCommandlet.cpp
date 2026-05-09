// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCreateTestMapCommandlet.h"

#include "Components/BoxComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "MHShadowBakeVolume.h"
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

template <typename TActor>
static TActor* SpawnNamedActor(UWorld& World, const TCHAR* Name, const FVector& Location, const FRotator& Rotation = FRotator::ZeroRotator)
{
	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = MakeUniqueObjectName(&World, TActor::StaticClass(), FName(Name));

	TActor* Actor = World.SpawnActor<TActor>(Location, Rotation, SpawnParams);
#if WITH_EDITOR
	if (Actor)
	{
		Actor->SetActorLabel(Name);
	}
#endif
	return Actor;
}

static AStaticMeshActor* SpawnStaticCube(
	UWorld& World,
	UStaticMesh& CubeMesh,
	const TCHAR* Name,
	const FVector& Location,
	const FVector& Scale)
{
	AStaticMeshActor* Actor = SpawnNamedActor<AStaticMeshActor>(World, Name, Location);
	if (!Actor)
	{
		return nullptr;
	}

	UStaticMeshComponent* MeshComponent = Actor->GetStaticMeshComponent();
	MeshComponent->SetStaticMesh(&CubeMesh);
	MeshComponent->SetMobility(EComponentMobility::Static);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	MeshComponent->SetCollisionObjectType(ECC_WorldStatic);
	MeshComponent->SetCollisionResponseToAllChannels(ECR_Block);
	Actor->SetActorScale3D(Scale);
	return Actor;
}
}

UMHShadowCreateTestMapCommandlet::UMHShadowCreateTestMapCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowCreateTestMapCommandlet::Main(const FString& Params)
{
	if (!GEditor)
	{
		UE_LOG(LogTemp, Error, TEXT("No editor is available for MH shadow test map creation."));
		return 1;
	}

	FString MapObjectPath = TEXT("/Game/MHShadow/Test/MHShadowBakeTest");
	ParseStringParam(Params, TEXT("Map="), MapObjectPath);
	if (!MapObjectPath.StartsWith(TEXT("/Game/")))
	{
		UE_LOG(LogTemp, Error, TEXT("Map must be a /Game object path, got '%s'."), *MapObjectPath);
		return 1;
	}

	UWorld* World = GEditor->NewMap(false);
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create a new editor map."));
		return 1;
	}

	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!CubeMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to load /Engine/BasicShapes/Cube.Cube."));
		return 1;
	}

	SpawnStaticCube(*World, *CubeMesh, TEXT("MH_Ground"), FVector(0.0, 0.0, -10.0), FVector(18.0, 18.0, 0.2));
	SpawnStaticCube(*World, *CubeMesh, TEXT("MH_BackWall"), FVector(450.0, 0.0, 190.0), FVector(0.3, 10.0, 4.0));
	SpawnStaticCube(*World, *CubeMesh, TEXT("MH_LeftWall"), FVector(-250.0, -450.0, 160.0), FVector(8.0, 0.3, 3.4));
	SpawnStaticCube(*World, *CubeMesh, TEXT("MH_Pillar_A"), FVector(-230.0, 100.0, 140.0), FVector(1.0, 1.0, 3.0));
	SpawnStaticCube(*World, *CubeMesh, TEXT("MH_Pillar_B"), FVector(160.0, -120.0, 95.0), FVector(0.8, 0.8, 2.1));
	SpawnStaticCube(*World, *CubeMesh, TEXT("MH_Roof"), FVector(80.0, 160.0, 330.0), FVector(5.0, 3.5, 0.35));
	SpawnStaticCube(*World, *CubeMesh, TEXT("MH_ThinOccluder"), FVector(-80.0, -220.0, 220.0), FVector(0.35, 3.2, 2.0));

	ADirectionalLight* DirectionalLight = SpawnNamedActor<ADirectionalLight>(
		*World,
		TEXT("MH_DirectionalLight"),
		FVector(0.0, 0.0, 700.0),
		FRotator(-48.0, -35.0, 0.0));
	if (!DirectionalLight)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create MH_DirectionalLight."));
		return 1;
	}
	DirectionalLight->GetLightComponent()->Mobility = EComponentMobility::Static;
	DirectionalLight->GetLightComponent()->Intensity = 6.0f;

	AMHShadowBakeVolume* BakeVolume = SpawnNamedActor<AMHShadowBakeVolume>(
		*World,
		TEXT("MH_BakeVolume"),
		FVector(0.0, 0.0, 180.0));
	if (!BakeVolume)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to create MH_BakeVolume."));
		return 1;
	}

	BakeVolume->DirectionalLight = DirectionalLight;
	BakeVolume->Resolution = 128;
	BakeVolume->TileSize = 64;
	BakeVolume->DepthBias = 5.0f;
	BakeVolume->TracePadding = 250.0f;
	BakeVolume->bStaticGeometryOnly = true;
	BakeVolume->DefaultOutputAssetPath = TEXT("/Game/MHShadow/Baked/MHShadowData_Test");
	BakeVolume->BoundsComponent->SetBoxExtent(FVector(1050.0, 950.0, 520.0));
	BakeVolume->BoundsComponent->UpdateBounds();

	World->UpdateWorldComponents(true, false);
	World->MarkPackageDirty();

	const FString PackageName = FPackageName::ObjectPathToPackageName(MapObjectPath);
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension());
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackageFilename), true);

	if (!FEditorFileUtils::SaveMap(World, PackageFilename))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save MH shadow test map '%s' to '%s'."), *PackageName, *PackageFilename);
		return 1;
	}

	UE_LOG(LogTemp, Display, TEXT("MH shadow test map created: %s"), *PackageName);
	UE_LOG(LogTemp, Display, TEXT("MH shadow test map file: %s"), *PackageFilename);
	UE_LOG(LogTemp, Display, TEXT("Bake volume: MH_BakeVolume Light: MH_DirectionalLight Output: /Game/MHShadow/Baked/MHShadowData_Test Resolution: 128"));
	return 0;
}

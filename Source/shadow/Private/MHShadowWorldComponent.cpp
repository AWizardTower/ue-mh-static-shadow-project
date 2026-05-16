// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowWorldComponent.h"

#include "EngineUtils.h"
#include "MHShadowCellComponent.h"
#include "MHShadowCellDataAsset.h"
#include "MHShadowWorldDataAsset.h"
#include "UObject/UnrealType.h"

#if __has_include("MHStaticShadowRenderer.h")
#include "MHStaticShadowRenderer.h"
#define WITH_MH_STATIC_SHADOW_RENDERER 1
#else
#define WITH_MH_STATIC_SHADOW_RENDERER 0
#endif

namespace
{
static TSet<TWeakObjectPtr<UMHShadowWorldComponent>> GRegisteredWorldComponents;

static bool CellBelongsToWorld(const UMHShadowCellDataAsset& CellData, const UMHShadowWorldDataAsset& WorldData)
{
	const FSoftObjectPath WorldPath(&WorldData);
	return CellData.WorldData.IsNull() || CellData.WorldData.ToSoftObjectPath() == WorldPath;
}

static FMatrix44f MakeWorldToShadowMatrix(const FMHShadowCellFlattenedData& Data)
{
	return FMatrix44f(FMatrix(
		FPlane(Data.WorldToShadowRow0.X, Data.WorldToShadowRow0.Y, Data.WorldToShadowRow0.Z, Data.WorldToShadowRow0.W),
		FPlane(Data.WorldToShadowRow1.X, Data.WorldToShadowRow1.Y, Data.WorldToShadowRow1.Z, Data.WorldToShadowRow1.W),
		FPlane(Data.WorldToShadowRow2.X, Data.WorldToShadowRow2.Y, Data.WorldToShadowRow2.Z, Data.WorldToShadowRow2.W),
		FPlane(Data.WorldToShadowRow3.X, Data.WorldToShadowRow3.Y, Data.WorldToShadowRow3.Z, Data.WorldToShadowRow3.W)));
}

static bool ShouldRegisterWithRenderer(const UActorComponent& Component, bool bRegisterInEditorWorld)
{
	const UWorld* World = Component.GetWorld();
	return World && (World->IsGameWorld() || bRegisterInEditorWorld);
}
}

UMHShadowWorldComponent::UMHShadowWorldComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UMHShadowWorldComponent::OnRegister()
{
	Super::OnRegister();
	RegistrationId = RegistrationId != 0 ? RegistrationId : static_cast<uint64>(GetUniqueID());
	GRegisteredWorldComponents.Add(this);
	if (bAutoRegisterWorld)
	{
		RegisterShadowData();
	}
}

void UMHShadowWorldComponent::OnUnregister()
{
	GRegisteredWorldComponents.Remove(this);
	UnregisterShadowData();
	Super::OnUnregister();
}

void UMHShadowWorldComponent::BeginPlay()
{
	Super::BeginPlay();
	if (bAutoRegisterWorld)
	{
		RegisterShadowData();
	}
}

void UMHShadowWorldComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterShadowData();
	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void UMHShadowWorldComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UMHShadowWorldComponent, WorldData)
		|| PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UMHShadowWorldComponent, bAutoRegisterWorld))
	{
		if (bAutoRegisterWorld)
		{
			RegisterShadowData();
		}
		else
		{
			UnregisterShadowData();
		}
	}
}
#endif

void UMHShadowWorldComponent::RegisterShadowData()
{
#if WITH_MH_STATIC_SHADOW_RENDERER
	if (!ShouldRegisterWithRenderer(*this, bRegisterInEditorWorld))
	{
		UnregisterShadowData();
		return;
	}

	if (!WorldData || !WorldData->IsValidWorldData())
	{
		if (WorldData)
		{
			UE_LOG(LogTemp, Warning, TEXT("MHShadowWorldComponent skipped invalid WorldData component=%s asset=%s"),
				*GetPathName(),
				*WorldData->GetPathName());
		}
		UnregisterShadowData();
		return;
	}

	TArray<const UMHShadowCellDataAsset*> LoadedCells;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UMHShadowCellComponent*> Components;
			It->GetComponents<UMHShadowCellComponent>(Components);
			for (UMHShadowCellComponent* CellComponent : Components)
			{
				if (!CellComponent || !CellComponent->IsCellProviderActive())
				{
					continue;
				}
				const UMHShadowCellDataAsset* CellData = CellComponent->CellData;
				if (CellData && CellBelongsToWorld(*CellData, *WorldData))
				{
					LoadedCells.AddUnique(CellData);
				}
			}
		}
	}

	if (LoadedCells.IsEmpty())
	{
		UnregisterShadowData();
		return;
	}

	FMHShadowCellFlattenedData FlattenedData;
	FString FlattenError;
	if (!WorldData->BuildFlattenedData(LoadedCells, FlattenedData, &FlattenError))
	{
		UE_LOG(LogTemp, Warning, TEXT("MHShadowWorldComponent failed to flatten cells component=%s worldData=%s cells=%d error=%s"),
			*GetPathName(),
			*WorldData->GetPathName(),
			LoadedCells.Num(),
			*FlattenError);
		UnregisterShadowData();
		return;
	}

	RegistrationId = RegistrationId != 0 ? RegistrationId : static_cast<uint64>(GetUniqueID());

	UE::Renderer::MHStaticShadow::FShadowData RenderData;
	RenderData.Id = RegistrationId;
	RenderData.DebugName = FString::Printf(TEXT("%s [CellProvider loadedCells=%d loadedPages=%d missingPages=%d providerBytes=%lld largestCellBytes=%lld]"),
		*WorldData->GetPathName(),
		FlattenedData.LoadedCellCount,
		FlattenedData.LoadedVirtualPageCount,
		FlattenedData.UnavailableVirtualPageCount,
		FlattenedData.ProviderMemoryBytes,
		FlattenedData.LargestLoadedCellBytes);
	RenderData.Resolution = FlattenedData.Resolution;
	RenderData.TileSize = FlattenedData.TileSize;
	RenderData.TileCount = FlattenedData.TileCount;
	RenderData.DepthBias = FlattenedData.DepthBias;
	RenderData.ProjectionMapping = FlattenedData.ProjectionMapping == EMHShadowProjectionMapping::LightmassWorldToShadowMatrix ? 1u : 0u;
	RenderData.LightRect = FVector4f(0.0f, 0.0f, 1.0f, 1.0f);
	RenderData.DepthRange = FVector2f(0.0f, 1.0f);
	RenderData.WorldToShadow = MakeWorldToShadowMatrix(FlattenedData);

	RenderData.ClipmapLevels.Reserve(FlattenedData.ClipmapLevels.Num());
	for (const FMHShadowClipmapLevel& Level : FlattenedData.ClipmapLevels)
	{
		UE::Renderer::MHStaticShadow::FShadowClipmapLevel RenderLevel;
		RenderLevel.ResolutionTileSizeLevel = FIntVector4(Level.Resolution.X, Level.Resolution.Y, Level.TileSize, Level.LevelIndex);
		RenderLevel.TileCountAndOffset = FIntVector4(Level.TileCount.X, Level.TileCount.Y, Level.TileOffset, Level.TileDataCount);
		RenderLevel.PageTableAndNodeOffset = FIntVector4(Level.PageTableOffset, Level.PageTableCount, Level.NodeOffset, Level.NodeCount);
		RenderLevel.TexelWorldSizeAndPadding = FVector4f(
			static_cast<float>(Level.TexelWorldSize.X),
			static_cast<float>(Level.TexelWorldSize.Y),
			0.0f,
			0.0f);
		RenderLevel.WorldToShadowRow0 = FVector4f(Level.WorldToShadowRow0.X, Level.WorldToShadowRow0.Y, Level.WorldToShadowRow0.Z, Level.WorldToShadowRow0.W);
		RenderLevel.WorldToShadowRow1 = FVector4f(Level.WorldToShadowRow1.X, Level.WorldToShadowRow1.Y, Level.WorldToShadowRow1.Z, Level.WorldToShadowRow1.W);
		RenderLevel.WorldToShadowRow2 = FVector4f(Level.WorldToShadowRow2.X, Level.WorldToShadowRow2.Y, Level.WorldToShadowRow2.Z, Level.WorldToShadowRow2.W);
		RenderLevel.WorldToShadowRow3 = FVector4f(Level.WorldToShadowRow3.X, Level.WorldToShadowRow3.Y, Level.WorldToShadowRow3.Z, Level.WorldToShadowRow3.W);
		RenderData.ClipmapLevels.Add(RenderLevel);
	}

	RenderData.ClipmapNodes.Reserve(FlattenedData.ClipmapNodes.Num());
	for (const FMHShadowNode& Node : FlattenedData.ClipmapNodes)
	{
		UE::Renderer::MHStaticShadow::FShadowNode RenderNode;
		RenderNode.ChildIndices = Node.ChildIndices;
		RenderNode.IntervalAndFlags = FVector4f(
			Node.BoundsMinDepth,
			Node.BoundsMaxDepth,
			Node.bHasRepresentativeDepth ? 1.0f : 0.0f,
			0.0f);
		RenderNode.RepresentativeAndBounds = FVector4f(
			Node.RepresentativeDepth,
			Node.BoundsMinDepth,
			Node.BoundsMaxDepth,
			Node.bHasRepresentativeDepth ? 1.0f : 0.0f);
		RenderData.ClipmapNodes.Add(RenderNode);
	}

	RenderData.ClipmapTiles.Reserve(FlattenedData.ClipmapTiles.Num());
	for (const FMHShadowTile& Tile : FlattenedData.ClipmapTiles)
	{
		UE::Renderer::MHStaticShadow::FShadowTile RenderTile;
		RenderTile.TexelRect = Tile.TexelRect;
		RenderTile.NodeAndPage = FIntVector4(Tile.NodeOffset, Tile.NodeCount, Tile.RootNodeIndex, Tile.PageIndex);
		RenderTile.CoordAndFlags = FIntVector4(Tile.TileCoord.X, Tile.TileCoord.Y, Tile.bResidentDefault ? 1 : 0, 0);
		RenderTile.Stats = FVector4f(
			static_cast<float>(Tile.RawTexelCount),
			static_cast<float>(Tile.ValidTexelCount),
			Tile.CompressionRatio,
			static_cast<float>(Tile.CompressedNodeCount));
	RenderData.ClipmapTiles.Add(RenderTile);
	}
	RenderData.ClipmapPageTable = FlattenedData.ClipmapPageTable;
	RenderData.RawDualBytesOverride = FlattenedData.RawDualBytes;
	RenderData.CompressedBytesOverride = FlattenedData.ProviderMemoryBytes;
	RenderData.ProviderMemoryBytes = FlattenedData.ProviderMemoryBytes;
	RenderData.LargestProviderCellBytes = FlattenedData.LargestLoadedCellBytes;
	RenderData.ProviderLoadedCellCount = FlattenedData.LoadedCellCount;
	RenderData.ProviderUnavailableVirtualPages = FlattenedData.UnavailableVirtualPageCount;

	UE::Renderer::MHStaticShadow::RegisterOrUpdateShadowData(RenderData);
	bRegisteredWithRenderer = true;
	UE_LOG(LogTemp, Display, TEXT("MHShadowWorldComponent registered cell provider component=%s worldData=%s loadedCells=%d loadedPages=%d unavailablePages=%d clipmapTiles=%d clipmapNodes=%d providerBytes=%lld largestCellBytes=%lld"),
		*GetPathName(),
		*WorldData->GetPathName(),
		FlattenedData.LoadedCellCount,
		FlattenedData.LoadedVirtualPageCount,
		FlattenedData.UnavailableVirtualPageCount,
		FlattenedData.ClipmapTiles.Num(),
		FlattenedData.ClipmapNodes.Num(),
		FlattenedData.ProviderMemoryBytes,
		FlattenedData.LargestLoadedCellBytes);
#else
	bRegisteredWithRenderer = false;
	UE_LOG(LogTemp, Warning, TEXT("MHShadowWorldComponent renderer bridge is not available for component=%s"), *GetPathName());
#endif
}

void UMHShadowWorldComponent::UnregisterShadowData()
{
#if WITH_MH_STATIC_SHADOW_RENDERER
	if (bRegisteredWithRenderer && RegistrationId != 0)
	{
		UE::Renderer::MHStaticShadow::UnregisterShadowData(RegistrationId);
		bRegisteredWithRenderer = false;
	}
#else
	bRegisteredWithRenderer = false;
#endif
}

void UMHShadowWorldComponent::RebuildFromLoadedCells()
{
	if (bAutoRegisterWorld)
	{
		RegisterShadowData();
	}
}

void UMHShadowWorldComponent::NotifyCellsChanged(UWorld* World)
{
	if (!World)
	{
		return;
	}

	for (TWeakObjectPtr<UMHShadowWorldComponent> WeakComponent : GRegisteredWorldComponents)
	{
		UMHShadowWorldComponent* Component = WeakComponent.Get();
		if (Component && Component->GetWorld() == World && Component->bAutoRegisterWorld)
		{
			Component->RegisterShadowData();
		}
	}
}

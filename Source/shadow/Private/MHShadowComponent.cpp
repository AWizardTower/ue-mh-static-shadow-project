// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowComponent.h"

#include "MHShadowDataAsset.h"
#include "UObject/UnrealType.h"

#if __has_include("MHStaticShadowRenderer.h")
#include "MHStaticShadowRenderer.h"
#define WITH_MH_STATIC_SHADOW_RENDERER 1
#else
#define WITH_MH_STATIC_SHADOW_RENDERER 0
#endif

UMHShadowComponent::UMHShadowComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UMHShadowComponent::OnRegister()
{
	Super::OnRegister();
	RegistrationId = RegistrationId != 0 ? RegistrationId : static_cast<uint64>(GetUniqueID());
	RegisterShadowData();
}

void UMHShadowComponent::OnUnregister()
{
	UnregisterShadowData();
	Super::OnUnregister();
}

void UMHShadowComponent::BeginPlay()
{
	Super::BeginPlay();
	RegisterShadowData();
}

void UMHShadowComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterShadowData();
	Super::EndPlay(EndPlayReason);
}

#if WITH_EDITOR
void UMHShadowComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UMHShadowComponent, ShadowData))
	{
		RegisterShadowData();
	}
}
#endif

void UMHShadowComponent::RegisterShadowData()
{
#if WITH_MH_STATIC_SHADOW_RENDERER
	if (!ShadowData || !ShadowData->IsValidForRendering())
	{
		if (ShadowData)
		{
			UE_LOG(LogTemp, Warning, TEXT("MHShadowComponent skipped invalid ShadowData component=%s asset=%s"),
				*GetPathName(),
				*ShadowData->GetPathName());
		}
		UnregisterShadowData();
		return;
	}

	RegistrationId = RegistrationId != 0 ? RegistrationId : static_cast<uint64>(GetUniqueID());

	UE::Renderer::MHStaticShadow::FShadowData RenderData;
	RenderData.Id = RegistrationId;
	RenderData.DebugName = ShadowData->GetPathName();
	RenderData.Resolution = ShadowData->Resolution;
	RenderData.TileSize = ShadowData->TileSize;
	RenderData.TileCount = ShadowData->TileCount;
	RenderData.DepthBias = ShadowData->DepthBias;
	RenderData.ProjectionMapping = ShadowData->ProjectionMapping == EMHShadowProjectionMapping::LightmassWorldToShadowMatrix ? 1u : 0u;
	RenderData.LightOrigin = FVector3f(ShadowData->LightOrigin);
	RenderData.LightXAxis = FVector3f(ShadowData->LightXAxis.GetSafeNormal());
	RenderData.LightYAxis = FVector3f(ShadowData->LightYAxis.GetSafeNormal());
	RenderData.LightZAxis = FVector3f(ShadowData->LightZAxis.GetSafeNormal());
	RenderData.LightRect = FVector4f(
		static_cast<float>(ShadowData->LightSpaceMin.X),
		static_cast<float>(ShadowData->LightSpaceMin.Y),
		static_cast<float>(ShadowData->LightSpaceMax.X),
		static_cast<float>(ShadowData->LightSpaceMax.Y));
	RenderData.DepthRange = FVector2f(ShadowData->MinLightDepth, ShadowData->MaxLightDepth);
	RenderData.WorldToShadow = FMatrix44f(FMatrix(
		FPlane(ShadowData->WorldToShadowRow0.X, ShadowData->WorldToShadowRow0.Y, ShadowData->WorldToShadowRow0.Z, ShadowData->WorldToShadowRow0.W),
		FPlane(ShadowData->WorldToShadowRow1.X, ShadowData->WorldToShadowRow1.Y, ShadowData->WorldToShadowRow1.Z, ShadowData->WorldToShadowRow1.W),
		FPlane(ShadowData->WorldToShadowRow2.X, ShadowData->WorldToShadowRow2.Y, ShadowData->WorldToShadowRow2.Z, ShadowData->WorldToShadowRow2.W),
		FPlane(ShadowData->WorldToShadowRow3.X, ShadowData->WorldToShadowRow3.Y, ShadowData->WorldToShadowRow3.Z, ShadowData->WorldToShadowRow3.W)));
	RenderData.RawIntervals.Reserve(ShadowData->RawIntervals.Num());
	for (int32 IntervalIndex = 0; IntervalIndex < ShadowData->RawIntervals.Num(); ++IntervalIndex)
	{
		const FMHShadowDepthInterval& Interval = ShadowData->RawIntervals[IntervalIndex];
		const float Flags = ShadowData->RawIntervalFlags.IsValidIndex(IntervalIndex) ? static_cast<float>(ShadowData->RawIntervalFlags[IntervalIndex]) : 0.0f;
		RenderData.RawIntervals.Add(FVector4f(Interval.MinDepth, Interval.MaxDepth, Interval.bValid ? 1.0f : 0.0f, Flags));
	}

	RenderData.Nodes.Reserve(ShadowData->Nodes.Num());

	for (const FMHShadowNode& Node : ShadowData->Nodes)
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

		RenderData.Nodes.Add(RenderNode);
	}

	RenderData.Tiles.Reserve(ShadowData->Tiles.Num());
	for (const FMHShadowTile& Tile : ShadowData->Tiles)
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
		RenderData.Tiles.Add(RenderTile);
	}

	RenderData.PageTable = ShadowData->PageTable;

	RenderData.ClipmapRawIntervals.Reserve(ShadowData->ClipmapRawIntervals.Num());
	for (int32 IntervalIndex = 0; IntervalIndex < ShadowData->ClipmapRawIntervals.Num(); ++IntervalIndex)
	{
		const FMHShadowDepthInterval& Interval = ShadowData->ClipmapRawIntervals[IntervalIndex];
		RenderData.ClipmapRawIntervals.Add(FVector4f(Interval.MinDepth, Interval.MaxDepth, Interval.bValid ? 1.0f : 0.0f, 0.0f));
	}

	RenderData.ClipmapNodes.Reserve(ShadowData->ClipmapNodes.Num());
	for (const FMHShadowNode& Node : ShadowData->ClipmapNodes)
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

	RenderData.ClipmapTiles.Reserve(ShadowData->ClipmapTiles.Num());
	for (const FMHShadowTile& Tile : ShadowData->ClipmapTiles)
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

	RenderData.ClipmapPageTable = ShadowData->ClipmapPageTable;
	RenderData.ClipmapLevels.Reserve(ShadowData->ClipmapLevels.Num());
	for (const FMHShadowClipmapLevel& Level : ShadowData->ClipmapLevels)
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

	UE::Renderer::MHStaticShadow::RegisterOrUpdateShadowData(RenderData);
	bRegisteredWithRenderer = true;
	UE_LOG(LogTemp, Display, TEXT("MHShadowComponent registered ShadowData component=%s asset=%s resolution=%dx%d tiles=%dx%d nodes=%d rawIntervals=%d clipmapLevels=%d clipmapTiles=%d clipmapNodes=%d"),
		*GetPathName(),
		*ShadowData->GetPathName(),
		ShadowData->Resolution.X,
		ShadowData->Resolution.Y,
		ShadowData->TileCount.X,
		ShadowData->TileCount.Y,
		ShadowData->Nodes.Num(),
		ShadowData->RawIntervals.Num(),
		ShadowData->ClipmapLevels.Num(),
		ShadowData->ClipmapTiles.Num(),
		ShadowData->ClipmapNodes.Num());
#else
	bRegisteredWithRenderer = false;
	UE_LOG(LogTemp, Warning, TEXT("MHShadowComponent renderer bridge is not available for component=%s"), *GetPathName());
#endif
}

void UMHShadowComponent::UnregisterShadowData()
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

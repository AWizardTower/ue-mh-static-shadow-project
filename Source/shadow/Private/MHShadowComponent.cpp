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
		UE_LOG(LogTemp, Warning, TEXT("MHShadowComponent skipped invalid ShadowData component=%s asset=%s"),
			*GetPathName(),
			ShadowData ? *ShadowData->GetPathName() : TEXT("None"));
		UnregisterShadowData();
		return;
	}

	RegistrationId = RegistrationId != 0 ? RegistrationId : static_cast<uint64>(GetUniqueID());

	UE::Renderer::MHStaticShadow::FShadowData RenderData;
	RenderData.Id = RegistrationId;
	RenderData.Resolution = ShadowData->Resolution;
	RenderData.TileSize = ShadowData->TileSize;
	RenderData.DepthBias = ShadowData->DepthBias;
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
	RenderData.RawIntervals.Reserve(ShadowData->RawIntervals.Num());
	for (const FMHShadowDepthInterval& Interval : ShadowData->RawIntervals)
	{
		RenderData.RawIntervals.Add(FVector4f(Interval.MinDepth, Interval.MaxDepth, Interval.bValid ? 1.0f : 0.0f, 0.0f));
	}

	RenderData.Nodes.Reserve(ShadowData->Nodes.Num());

	for (const FMHShadowNode& Node : ShadowData->Nodes)
	{
		UE::Renderer::MHStaticShadow::FShadowNode RenderNode;
		RenderNode.ChildIndices = Node.ChildIndices;

		const bool bHasInterval = ShadowData->Intervals.IsValidIndex(Node.IntervalIndex)
			&& ShadowData->Intervals[Node.IntervalIndex].bValid;
		if (bHasInterval)
		{
			const FMHShadowDepthInterval& Interval = ShadowData->Intervals[Node.IntervalIndex];
			RenderNode.IntervalAndFlags = FVector4f(Interval.MinDepth, Interval.MaxDepth, 1.0f, 0.0f);
		}
		else
		{
			RenderNode.IntervalAndFlags = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
		}

		if (Node.ChildIndices.X >= 0 || Node.ChildIndices.Y >= 0 || Node.ChildIndices.Z >= 0 || Node.ChildIndices.W >= 0)
		{
			RenderNode.IntervalAndFlags.W = 1.0f;
		}

		RenderData.Nodes.Add(RenderNode);
	}

	UE::Renderer::MHStaticShadow::RegisterOrUpdateShadowData(RenderData);
	bRegisteredWithRenderer = true;
	UE_LOG(LogTemp, Display, TEXT("MHShadowComponent registered ShadowData component=%s asset=%s resolution=%dx%d nodes=%d rawIntervals=%d"),
		*GetPathName(),
		*ShadowData->GetPathName(),
		ShadowData->Resolution.X,
		ShadowData->Resolution.Y,
		ShadowData->Nodes.Num(),
		ShadowData->RawIntervals.Num());
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

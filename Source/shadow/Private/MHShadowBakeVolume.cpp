// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowBakeVolume.h"

#include "Components/BoxComponent.h"
#include "Components/LightComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/DirectionalLight.h"
#include "Engine/World.h"

AMHShadowBakeVolume::AMHShadowBakeVolume()
{
	PrimaryActorTick.bCanEverTick = true;

	BoundsComponent = CreateDefaultSubobject<UBoxComponent>(TEXT("Bounds"));
	SetRootComponent(BoundsComponent);
	BoundsComponent->SetBoxExtent(FVector(500.0, 500.0, 300.0));
	BoundsComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void AMHShadowBakeVolume::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	DrawDebugBakeRays();
}

#if WITH_EDITOR
bool AMHShadowBakeVolume::ShouldTickIfViewportsOnly() const
{
	return bDrawDebugBakeRays;
}
#endif

FBox AMHShadowBakeVolume::GetBakeBounds() const
{
	if (!BoundsComponent)
	{
		return GetComponentsBoundingBox();
	}

	return FBox::BuildAABB(
		BoundsComponent->GetComponentLocation(),
		BoundsComponent->GetScaledBoxExtent());
}

namespace
{
struct FMHShadowDebugBasis
{
	FVector Origin = FVector::ZeroVector;
	FVector XAxis = FVector::ForwardVector;
	FVector YAxis = FVector::RightVector;
	FVector ZAxis = FVector::UpVector;
	FVector2D LightMin = FVector2D::ZeroVector;
	FVector2D LightMax = FVector2D::ZeroVector;
	float MinDepth = 0.0f;
	float MaxDepth = 0.0f;
};

static FMHShadowDebugBasis BuildDebugBasis(const FBox& Bounds, const ADirectionalLight* DirectionalLight)
{
	FMHShadowDebugBasis Basis;
	Basis.Origin = Bounds.GetCenter();
	Basis.ZAxis = DirectionalLight && DirectionalLight->GetLightComponent()
		? DirectionalLight->GetLightComponent()->GetDirection().GetSafeNormal()
		: FVector::DownVector;
	if (Basis.ZAxis.IsNearlyZero())
	{
		Basis.ZAxis = FVector::DownVector;
	}

	const FVector UpCandidate = FMath::Abs(FVector::DotProduct(Basis.ZAxis, FVector::UpVector)) > 0.95
		? FVector::RightVector
		: FVector::UpVector;
	Basis.XAxis = FVector::CrossProduct(UpCandidate, Basis.ZAxis).GetSafeNormal();
	Basis.YAxis = FVector::CrossProduct(Basis.ZAxis, Basis.XAxis).GetSafeNormal();

	Basis.LightMin = FVector2D(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
	Basis.LightMax = FVector2D(TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest());
	Basis.MinDepth = TNumericLimits<float>::Max();
	Basis.MaxDepth = TNumericLimits<float>::Lowest();

	const FVector Min = Bounds.Min;
	const FVector Max = Bounds.Max;
	for (int32 CornerIndex = 0; CornerIndex < 8; ++CornerIndex)
	{
		const FVector Corner(
			(CornerIndex & 1) ? Max.X : Min.X,
			(CornerIndex & 2) ? Max.Y : Min.Y,
			(CornerIndex & 4) ? Max.Z : Min.Z);
		const FVector Delta = Corner - Basis.Origin;
		const float X = static_cast<float>(FVector::DotProduct(Delta, Basis.XAxis));
		const float Y = static_cast<float>(FVector::DotProduct(Delta, Basis.YAxis));
		const float Z = static_cast<float>(FVector::DotProduct(Delta, Basis.ZAxis));
		Basis.LightMin.X = FMath::Min(Basis.LightMin.X, X);
		Basis.LightMin.Y = FMath::Min(Basis.LightMin.Y, Y);
		Basis.LightMax.X = FMath::Max(Basis.LightMax.X, X);
		Basis.LightMax.Y = FMath::Max(Basis.LightMax.Y, Y);
		Basis.MinDepth = FMath::Min(Basis.MinDepth, Z);
		Basis.MaxDepth = FMath::Max(Basis.MaxDepth, Z);
	}

	return Basis;
}
}

void AMHShadowBakeVolume::DrawDebugBakeRays() const
{
	if (!bDrawDebugBakeRays || !GetWorld())
	{
		return;
	}

#if WITH_EDITOR
	if (bDebugDrawOnlyWhenSelected && !IsSelected())
	{
		return;
	}
#endif

	const FBox BakeBounds = GetBakeBounds();
	if (!BakeBounds.IsValid)
	{
		return;
	}

	const FMHShadowDebugBasis Basis = BuildDebugBasis(BakeBounds, DirectionalLight);
	const int32 GridCount = FMath::Clamp(DebugRayGridCount, 2, 64);
	const FVector2D LightExtent = Basis.LightMax - Basis.LightMin;
	const float Padding = FMath::Max(TracePadding, 0.0f);
	const float AxisLength = FMath::Max(100.0f, BakeBounds.GetExtent().GetMax() * 0.35f);
	const float Thickness = FMath::Max(0.0f, DebugLineThickness);

	DrawDebugDirectionalArrow(GetWorld(), Basis.Origin, Basis.Origin + Basis.XAxis * AxisLength, 35.0f, FColor::Red, false, 0.0f, 0, Thickness + 0.5f);
	DrawDebugDirectionalArrow(GetWorld(), Basis.Origin, Basis.Origin + Basis.YAxis * AxisLength, 35.0f, FColor::Green, false, 0.0f, 0, Thickness + 0.5f);
	DrawDebugDirectionalArrow(GetWorld(), Basis.Origin, Basis.Origin + Basis.ZAxis * AxisLength, 35.0f, FColor::Blue, false, 0.0f, 0, Thickness + 0.5f);
	DrawDebugDirectionalArrow(GetWorld(), Basis.Origin - Basis.ZAxis * AxisLength, Basis.Origin + Basis.ZAxis * AxisLength, 45.0f, FColor::Cyan, false, 0.0f, 0, Thickness + 1.0f);

	DrawDebugString(
		GetWorld(),
		Basis.Origin + FVector(0.0, 0.0, BakeBounds.GetExtent().Z + 80.0),
		TEXT("MH debug rays: cyan=no hit, red=front hit, purple=back hit, orange=front/back interval"),
		nullptr,
		FColor::White,
		0.0f,
		true);

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
	if (!bStaticGeometryOnly)
	{
		ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MHShadowDebugRays), bDebugTraceComplex);
	QueryParams.bReturnFaceIndex = false;

	for (int32 Y = 0; Y < GridCount; ++Y)
	{
		for (int32 X = 0; X < GridCount; ++X)
		{
			const FVector2D UV(
				(static_cast<double>(X) + 0.5) / static_cast<double>(GridCount),
				(static_cast<double>(Y) + 0.5) / static_cast<double>(GridCount));
			const FVector2D LightXY = Basis.LightMin + LightExtent * UV;
			const FVector PlanePoint = Basis.Origin + Basis.XAxis * LightXY.X + Basis.YAxis * LightXY.Y;
			const FVector Start = PlanePoint + Basis.ZAxis * (Basis.MinDepth - Padding);
			const FVector End = PlanePoint + Basis.ZAxis * (Basis.MaxDepth + Padding);

			TArray<FHitResult> Hits;
			GetWorld()->LineTraceMultiByObjectType(Hits, Start, End, ObjectParams, QueryParams);

			float FrontDepth = TNumericLimits<float>::Max();
			float BackDepth = TNumericLimits<float>::Lowest();
			FVector FrontPoint = FVector::ZeroVector;
			FVector BackPoint = FVector::ZeroVector;
			bool bHasHit = false;

			for (const FHitResult& Hit : Hits)
			{
				if (!Hit.bBlockingHit)
				{
					continue;
				}

				const float HitDepth = static_cast<float>(FVector::DotProduct(Hit.ImpactPoint - Basis.Origin, Basis.ZAxis));
				if (HitDepth < FrontDepth)
				{
					FrontDepth = HitDepth;
					FrontPoint = Hit.ImpactPoint;
				}
				if (HitDepth > BackDepth)
				{
					BackDepth = HitDepth;
					BackPoint = Hit.ImpactPoint;
				}
				bHasHit = true;
			}

			DrawDebugLine(GetWorld(), Start, End, bHasHit ? FColor(80, 80, 80) : FColor::Cyan, false, 0.0f, 0, Thickness);

			if (bHasHit)
			{
				DrawDebugLine(GetWorld(), FrontPoint, BackPoint, FColor::Orange, false, 0.0f, 0, Thickness + 1.0f);
				DrawDebugPoint(GetWorld(), FrontPoint, 8.0f, FColor::Red, false, 0.0f);
				DrawDebugPoint(GetWorld(), BackPoint, 8.0f, FColor::Purple, false, 0.0f);
			}
		}
	}
}

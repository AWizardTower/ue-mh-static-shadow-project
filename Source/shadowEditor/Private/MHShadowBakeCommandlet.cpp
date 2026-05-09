// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowBakeCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "MHShadowBakeVolume.h"
#include "MHShadowCompressor.h"
#include "MHShadowDataAsset.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "StaticMeshResources.h"
#include "UObject/SavePackage.h"

namespace
{
struct FMHShadowBakeBasis
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

struct FMHShadowTraceStats
{
	int32 PhysicsTraceTexels = 0;
	int32 ComponentTraceTexels = 0;
	int32 MeshTraceTexels = 0;
	int32 BoundsFallbackTexels = 0;
};

static bool ParseStringParam(const FString& Params, const TCHAR* Key, FString& OutValue)
{
	if (FParse::Value(*Params, Key, OutValue))
	{
		OutValue.TrimStartAndEndInline();
		return !OutValue.IsEmpty();
	}
	return false;
}

static AMHShadowBakeVolume* FindBakeVolume(UWorld* World, const FString& WantedName)
{
	for (TActorIterator<AMHShadowBakeVolume> It(World); It; ++It)
	{
		AMHShadowBakeVolume* Volume = *It;
		if (WantedName.IsEmpty()
			|| Volume->GetName() == WantedName
#if WITH_EDITOR
			|| Volume->GetActorLabel() == WantedName
#endif
			)
		{
			return Volume;
		}
	}
	return nullptr;
}

static ADirectionalLight* FindDirectionalLight(UWorld* World, AMHShadowBakeVolume* Volume, const FString& WantedName)
{
	if (Volume && Volume->DirectionalLight)
	{
		return Volume->DirectionalLight;
	}

	for (TActorIterator<ADirectionalLight> It(World); It; ++It)
	{
		ADirectionalLight* Light = *It;
		if (WantedName.IsEmpty()
			|| Light->GetName() == WantedName
#if WITH_EDITOR
			|| Light->GetActorLabel() == WantedName
#endif
			)
		{
			return Light;
		}
	}
	return nullptr;
}

static FMHShadowBakeBasis BuildBasis(const FBox& Bounds, const ADirectionalLight& DirectionalLight)
{
	FMHShadowBakeBasis Basis;
	Basis.Origin = Bounds.GetCenter();
	Basis.ZAxis = DirectionalLight.GetLightComponent()
		? DirectionalLight.GetLightComponent()->GetDirection().GetSafeNormal()
		: DirectionalLight.GetActorForwardVector().GetSafeNormal();
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

static bool HasBlockingHits(const TArray<FHitResult>& Hits)
{
	for (const FHitResult& Hit : Hits)
	{
		if (Hit.bBlockingHit)
		{
			return true;
		}
	}
	return false;
}

static bool SegmentBoxIntersection(const FVector& Start, const FVector& End, const FBox& Box, float& OutEntryT, float& OutExitT);

static void TraceComponentsByObjectType(
	UWorld& World,
	const FVector& Start,
	const FVector& End,
	const FCollisionQueryParams& QueryParams,
	bool bStaticGeometryOnly,
	TArray<FHitResult>& OutHits)
{
	for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
	{
		TArray<UPrimitiveComponent*> Components;
		ActorIt->GetComponents<UPrimitiveComponent>(Components);

		for (UPrimitiveComponent* Component : Components)
		{
			if (!Component
				|| !Component->IsRegistered()
				|| Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
			{
				continue;
			}

			const ECollisionChannel ObjectType = Component->GetCollisionObjectType();
			if (ObjectType != ECC_WorldStatic && (bStaticGeometryOnly || ObjectType != ECC_WorldDynamic))
			{
				continue;
			}

			FHitResult Hit;
			if (Component->LineTraceComponent(Hit, Start, End, QueryParams) && Hit.bBlockingHit)
			{
				OutHits.Add(Hit);
			}
		}
	}
}

static void TraceStaticMeshTrianglesByObjectType(
	UWorld& World,
	const FVector& Start,
	const FVector& End,
	bool bStaticGeometryOnly,
	TArray<FHitResult>& OutHits)
{
	for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
	{
		TArray<UStaticMeshComponent*> Components;
		ActorIt->GetComponents<UStaticMeshComponent>(Components);

		for (UStaticMeshComponent* Component : Components)
		{
			if (!Component
				|| !Component->IsRegistered()
				|| !Component->GetStaticMesh()
				|| Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
			{
				continue;
			}

			const ECollisionChannel ObjectType = Component->GetCollisionObjectType();
			if (ObjectType != ECC_WorldStatic && (bStaticGeometryOnly || ObjectType != ECC_WorldDynamic))
			{
				continue;
			}

			const FBox ComponentBox = Component->CalcBounds(Component->GetComponentTransform()).GetBox();
			float EntryT = 0.0f;
			float ExitT = 0.0f;
			if (!SegmentBoxIntersection(Start, End, ComponentBox, EntryT, ExitT))
			{
				continue;
			}

			const UStaticMesh* StaticMesh = Component->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = StaticMesh ? StaticMesh->GetRenderData() : nullptr;
			if (!RenderData || RenderData->LODResources.IsEmpty())
			{
				continue;
			}

			const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
			const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
			const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
			if (Indices.Num() < 3 || Positions.GetNumVertices() == 0)
			{
				continue;
			}

			const FTransform& ComponentTransform = Component->GetComponentTransform();
			for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3)
			{
				const uint32 Index0 = Indices[Index];
				const uint32 Index1 = Indices[Index + 1];
				const uint32 Index2 = Indices[Index + 2];
				if (Index0 >= Positions.GetNumVertices()
					|| Index1 >= Positions.GetNumVertices()
					|| Index2 >= Positions.GetNumVertices())
				{
					continue;
				}

				const FVector Vertex0 = ComponentTransform.TransformPosition(FVector(Positions.VertexPosition(Index0)));
				const FVector Vertex1 = ComponentTransform.TransformPosition(FVector(Positions.VertexPosition(Index1)));
				const FVector Vertex2 = ComponentTransform.TransformPosition(FVector(Positions.VertexPosition(Index2)));

				FVector IntersectPoint = FVector::ZeroVector;
				FVector TriangleNormal = FVector::ZeroVector;
				if (FMath::SegmentTriangleIntersection(Start, End, Vertex0, Vertex1, Vertex2, IntersectPoint, TriangleNormal))
				{
					FHitResult Hit;
					Hit.bBlockingHit = true;
					Hit.ImpactPoint = IntersectPoint;
					Hit.ImpactNormal = TriangleNormal;
					Hit.Component = Component;
					OutHits.Add(Hit);
				}
			}
		}
	}
}

static bool SegmentBoxIntersection(const FVector& Start, const FVector& End, const FBox& Box, float& OutEntryT, float& OutExitT)
{
	const FVector Direction = End - Start;
	float EntryT = 0.0f;
	float ExitT = 1.0f;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float StartValue = static_cast<float>(Start[Axis]);
		const float DirectionValue = static_cast<float>(Direction[Axis]);
		const float BoxMin = static_cast<float>(Box.Min[Axis]);
		const float BoxMax = static_cast<float>(Box.Max[Axis]);

		if (FMath::IsNearlyZero(DirectionValue))
		{
			if (StartValue < BoxMin || StartValue > BoxMax)
			{
				return false;
			}
			continue;
		}

		float T0 = (BoxMin - StartValue) / DirectionValue;
		float T1 = (BoxMax - StartValue) / DirectionValue;
		if (T0 > T1)
		{
			Swap(T0, T1);
		}

		EntryT = FMath::Max(EntryT, T0);
		ExitT = FMath::Min(ExitT, T1);
		if (EntryT > ExitT)
		{
			return false;
		}
	}

	OutEntryT = EntryT;
	OutExitT = ExitT;
	return true;
}

static void TraceComponentBoundsByObjectType(
	UWorld& World,
	const FVector& Start,
	const FVector& End,
	bool bStaticGeometryOnly,
	TArray<FHitResult>& OutHits)
{
	const FVector Direction = End - Start;
	for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
	{
		TArray<UPrimitiveComponent*> Components;
		ActorIt->GetComponents<UPrimitiveComponent>(Components);

		for (UPrimitiveComponent* Component : Components)
		{
			if (!Component
				|| !Component->IsRegistered()
				|| Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
			{
				continue;
			}

			const ECollisionChannel ObjectType = Component->GetCollisionObjectType();
			if (ObjectType != ECC_WorldStatic && (bStaticGeometryOnly || ObjectType != ECC_WorldDynamic))
			{
				continue;
			}

			const FBox ComponentBox = Component->CalcBounds(Component->GetComponentTransform()).GetBox();
			float EntryT = 0.0f;
			float ExitT = 0.0f;
			if (!SegmentBoxIntersection(Start, End, ComponentBox, EntryT, ExitT))
			{
				continue;
			}

			FHitResult EntryHit;
			EntryHit.bBlockingHit = true;
			EntryHit.ImpactPoint = Start + Direction * EntryT;
			OutHits.Add(EntryHit);

			FHitResult ExitHit;
			ExitHit.bBlockingHit = true;
			ExitHit.ImpactPoint = Start + Direction * ExitT;
			OutHits.Add(ExitHit);
		}
	}
}

static FMHShadowDepthInterval TraceDualDepthInterval(
	UWorld& World,
	const FMHShadowBakeBasis& Basis,
	const FVector2D& LightXY,
	float TracePadding,
	bool bStaticGeometryOnly,
	bool bAllowBoundsFallback,
	FMHShadowTraceStats& TraceStats)
{
	FMHShadowDepthInterval Interval;
	enum class ETraceSource : uint8
	{
		None,
		Physics,
		Component,
		Mesh,
		Bounds
	};
	ETraceSource TraceSource = ETraceSource::None;

	const FVector PlanePoint = Basis.Origin + Basis.XAxis * LightXY.X + Basis.YAxis * LightXY.Y;
	const FVector Start = PlanePoint + Basis.ZAxis * (Basis.MinDepth - TracePadding);
	const FVector End = PlanePoint + Basis.ZAxis * (Basis.MaxDepth + TracePadding);

	FCollisionObjectQueryParams ObjectParams;
	ObjectParams.AddObjectTypesToQuery(ECC_WorldStatic);
	if (!bStaticGeometryOnly)
	{
		ObjectParams.AddObjectTypesToQuery(ECC_WorldDynamic);
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(MHShadowBake), true);
	QueryParams.bTraceComplex = true;
	QueryParams.bReturnFaceIndex = false;

	TArray<FHitResult> Hits;
	if (!World.LineTraceMultiByObjectType(Hits, Start, End, ObjectParams, QueryParams))
	{
		Hits.Reset();
		QueryParams.bTraceComplex = false;
	}

	if (Hits.IsEmpty())
	{
		World.LineTraceMultiByObjectType(Hits, Start, End, ObjectParams, QueryParams);
	}
	if (HasBlockingHits(Hits))
	{
		TraceSource = ETraceSource::Physics;
	}

	if (!HasBlockingHits(Hits))
	{
		Hits.Reset();
		QueryParams.bTraceComplex = true;
		TraceComponentsByObjectType(World, Start, End, QueryParams, bStaticGeometryOnly, Hits);

		if (!HasBlockingHits(Hits))
		{
			Hits.Reset();
			QueryParams.bTraceComplex = false;
			TraceComponentsByObjectType(World, Start, End, QueryParams, bStaticGeometryOnly, Hits);
		}
		if (HasBlockingHits(Hits))
		{
			TraceSource = ETraceSource::Component;
		}

		if (!HasBlockingHits(Hits))
		{
			Hits.Reset();
			TraceStaticMeshTrianglesByObjectType(World, Start, End, bStaticGeometryOnly, Hits);
			if (HasBlockingHits(Hits))
			{
				TraceSource = ETraceSource::Mesh;
			}
		}

		if (!HasBlockingHits(Hits) && bAllowBoundsFallback)
		{
			Hits.Reset();
			TraceComponentBoundsByObjectType(World, Start, End, bStaticGeometryOnly, Hits);
			if (HasBlockingHits(Hits))
			{
				TraceSource = ETraceSource::Bounds;
			}
		}
	}

	if (!HasBlockingHits(Hits))
	{
		return Interval;
	}

	switch (TraceSource)
	{
	case ETraceSource::Physics:
		++TraceStats.PhysicsTraceTexels;
		break;
	case ETraceSource::Component:
		++TraceStats.ComponentTraceTexels;
		break;
	case ETraceSource::Mesh:
		++TraceStats.MeshTraceTexels;
		break;
	case ETraceSource::Bounds:
		++TraceStats.BoundsFallbackTexels;
		break;
	default:
		break;
	}

	float MinHitDepth = TNumericLimits<float>::Max();
	float MaxHitDepth = TNumericLimits<float>::Lowest();
	for (const FHitResult& Hit : Hits)
	{
		if (!Hit.bBlockingHit)
		{
			continue;
		}

		const float HitDepth = static_cast<float>(FVector::DotProduct(Hit.ImpactPoint - Basis.Origin, Basis.ZAxis));
		MinHitDepth = FMath::Min(MinHitDepth, HitDepth);
		MaxHitDepth = FMath::Max(MaxHitDepth, HitDepth);
	}

	if (MinHitDepth <= MaxHitDepth)
	{
		Interval.MinDepth = MinHitDepth;
		Interval.MaxDepth = MaxHitDepth;
		Interval.bValid = true;
	}

	return Interval;
}

static bool SaveShadowDataAsset(const FString& ObjectPath, UMHShadowDataAsset* Asset)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
	if (!Package)
	{
		return false;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs);
}

static void WriteStatsCsv(const FString& OutputObjectPath, const UMHShadowDataAsset& Asset)
{
	const FString SafeName = FPackageName::GetShortName(OutputObjectPath);
	const FString StatsDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"));
	IFileManager::Get().MakeDirectory(*StatsDir, true);

	const FString Csv =
		TEXT("Resolution,RawTexels,ValidTexels,Nodes,Intervals,RawBytes,CompressedBytes,CompressionRatio,BakeSeconds\n")
		+ FString::Printf(
			TEXT("%dx%d,%d,%d,%d,%d,%lld,%lld,%.6f,%.3f\n"),
			Asset.Resolution.X,
			Asset.Resolution.Y,
			Asset.Stats.RawTexelCount,
			Asset.Stats.ValidTexelCount,
			Asset.Stats.NodeCount,
			Asset.Stats.IntervalCount,
			Asset.Stats.RawBytes,
			Asset.Stats.CompressedBytes,
			Asset.Stats.CompressionRatio,
			Asset.Stats.BakeSeconds);
	FFileHelper::SaveStringToFile(Csv, *FPaths::Combine(StatsDir, SafeName + TEXT("_Stats.csv")));
}

static bool WriteDebugPreviewPng(const FString& OutputObjectPath, const UMHShadowDataAsset& Asset)
{
	if (Asset.Resolution.X <= 0
		|| Asset.Resolution.Y <= 0
		|| Asset.DebugIntervalPreview.Num() != Asset.Resolution.X * Asset.Resolution.Y)
	{
		UE_LOG(LogTemp, Warning, TEXT("Skipping MH shadow debug preview PNG because the preview buffer is invalid."));
		return false;
	}

	TSet<FColor> UniqueColors;
	int32 NonWhitePixels = 0;
	for (const FColor& Color : Asset.DebugIntervalPreview)
	{
		UniqueColors.Add(Color);
		if (Color != FColor::White)
		{
			++NonWhitePixels;
		}
	}

	const FString SafeName = FPackageName::GetShortName(OutputObjectPath);
	const FString DebugDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MHShadow"));
	IFileManager::Get().MakeDirectory(*DebugDir, true);

	TArray64<uint8> PngData;
	FImageUtils::PNGCompressImageArray(
		Asset.Resolution.X,
		Asset.Resolution.Y,
		TArrayView64<const FColor>(Asset.DebugIntervalPreview.GetData(), Asset.DebugIntervalPreview.Num()),
		PngData);

	const FString PngPath = FPaths::Combine(DebugDir, SafeName + TEXT("_DebugPreview.png"));
	const bool bSaved = FFileHelper::SaveArrayToFile(PngData, *PngPath);
	if (bSaved)
	{
		UE_LOG(LogTemp, Display, TEXT("MH shadow debug preview saved: %s NonWhitePixels=%d UniqueColors=%d"),
			*PngPath,
			NonWhitePixels,
			UniqueColors.Num());
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to save MH shadow debug preview PNG: %s"), *PngPath);
	}

	return bSaved;
}
}

UMHShadowBakeCommandlet::UMHShadowBakeCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UMHShadowBakeCommandlet::Main(const FString& Params)
{
	FString MapPath;
	if (ParseStringParam(Params, TEXT("Map="), MapPath))
	{
		UE_LOG(LogTemp, Display, TEXT("Loading map for MH shadow bake: %s"), *MapPath);
		if (!FEditorFileUtils::LoadMap(MapPath, false, true))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to load map '%s'."), *MapPath);
			return 1;
		}
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("No editor world is available for MH shadow bake."));
		return 1;
	}
	World->UpdateWorldComponents(true, false);

	FString VolumeName;
	ParseStringParam(Params, TEXT("Volume="), VolumeName);
	AMHShadowBakeVolume* Volume = FindBakeVolume(World, VolumeName);
	if (!Volume)
	{
		UE_LOG(LogTemp, Error, TEXT("No AMHShadowBakeVolume found. Add one to the map or pass Volume=<ActorName>."));
		return 1;
	}

	FString LightName;
	ParseStringParam(Params, TEXT("Light="), LightName);
	ADirectionalLight* DirectionalLight = FindDirectionalLight(World, Volume, LightName);
	if (!DirectionalLight)
	{
		UE_LOG(LogTemp, Error, TEXT("No directional light found for MH shadow bake."));
		return 1;
	}

	bool bStaticGeometryOnly = Volume->bStaticGeometryOnly;
	int32 StaticOnlyOverride = bStaticGeometryOnly ? 1 : 0;
	if (FParse::Value(*Params, TEXT("StaticOnly="), StaticOnlyOverride))
	{
		bStaticGeometryOnly = StaticOnlyOverride != 0;
	}

	bool bAllowBoundsFallback = false;
	int32 BoundsFallbackOverride = 0;
	if (FParse::Value(*Params, TEXT("BoundsFallback="), BoundsFallbackOverride))
	{
		bAllowBoundsFallback = BoundsFallbackOverride != 0;
	}

	int32 Resolution = Volume->Resolution;
	FParse::Value(*Params, TEXT("Resolution="), Resolution);
	Resolution = FMath::Max(2, Resolution);
	if (!FMath::IsPowerOfTwo(Resolution))
	{
		Resolution = FMath::RoundUpToPowerOfTwo(Resolution);
		UE_LOG(LogTemp, Warning, TEXT("Resolution must be power-of-two. Rounded up to %d."), Resolution);
	}

	FString OutputObjectPath = Volume->DefaultOutputAssetPath;
	ParseStringParam(Params, TEXT("Output="), OutputObjectPath);
	if (!OutputObjectPath.StartsWith(TEXT("/Game/")))
	{
		UE_LOG(LogTemp, Error, TEXT("Output must be a /Game object path, got '%s'."), *OutputObjectPath);
		return 1;
	}

	const double BakeStart = FPlatformTime::Seconds();
	const FBox BakeBounds = Volume->GetBakeBounds();
	const FMHShadowBakeBasis Basis = BuildBasis(BakeBounds, *DirectionalLight);

	int32 StaticMeshActorCount = 0;
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		++StaticMeshActorCount;
	}

	UE_LOG(LogTemp, Display, TEXT("MH shadow bake bounds: Min=(%.1f, %.1f, %.1f) Max=(%.1f, %.1f, %.1f) StaticMeshActors=%d StaticOnly=%d BoundsFallback=%d"),
		BakeBounds.Min.X,
		BakeBounds.Min.Y,
		BakeBounds.Min.Z,
		BakeBounds.Max.X,
		BakeBounds.Max.Y,
		BakeBounds.Max.Z,
		StaticMeshActorCount,
		bStaticGeometryOnly ? 1 : 0,
		bAllowBoundsFallback ? 1 : 0);
	UE_LOG(LogTemp, Display, TEXT("MH shadow light basis: Z=(%.3f, %.3f, %.3f) LightMin=(%.1f, %.1f) LightMax=(%.1f, %.1f) Depth=[%.1f, %.1f]"),
		Basis.ZAxis.X,
		Basis.ZAxis.Y,
		Basis.ZAxis.Z,
		Basis.LightMin.X,
		Basis.LightMin.Y,
		Basis.LightMax.X,
		Basis.LightMax.Y,
		Basis.MinDepth,
		Basis.MaxDepth);

	FMHShadowCompressionInput CompressionInput;
	CompressionInput.Resolution = FIntPoint(Resolution, Resolution);
	CompressionInput.TexelIntervals.SetNum(Resolution * Resolution);
	FMHShadowTraceStats TraceStats;

	TArray<FColor> DebugPreview;
	DebugPreview.SetNum(Resolution * Resolution);

	const FVector2D Extent = Basis.LightMax - Basis.LightMin;
	for (int32 Y = 0; Y < Resolution; ++Y)
	{
		for (int32 X = 0; X < Resolution; ++X)
		{
			const FVector2D UV(
				(static_cast<double>(X) + 0.5) / static_cast<double>(Resolution),
				(static_cast<double>(Y) + 0.5) / static_cast<double>(Resolution));
			const FVector2D LightXY = Basis.LightMin + Extent * UV;
			const FMHShadowDepthInterval Interval = TraceDualDepthInterval(
				*World,
				Basis,
				LightXY,
				Volume->TracePadding,
				bStaticGeometryOnly,
				bAllowBoundsFallback,
				TraceStats);
			const int32 TexelIndex = Y * Resolution + X;
			CompressionInput.TexelIntervals[TexelIndex] = Interval;

			if (Interval.bValid)
			{
				const float Thickness = FMath::Clamp((Interval.MaxDepth - Interval.MinDepth) / FMath::Max(1.0f, Basis.MaxDepth - Basis.MinDepth), 0.0f, 1.0f);
				const uint8 Value = static_cast<uint8>(FMath::RoundToInt(Thickness * 255.0f));
				DebugPreview[TexelIndex] = FColor(Value, 64, 255 - Value, 255);
			}
			else
			{
				DebugPreview[TexelIndex] = FColor::White;
			}
		}
	}

	FMHShadowCompressionOutput CompressionOutput;
	FString CompressionError;
	if (!FMHShadowCompressor::Compress(CompressionInput, CompressionOutput, &CompressionError))
	{
		UE_LOG(LogTemp, Error, TEXT("MH compression failed: %s"), *CompressionError);
		return 1;
	}
	CompressionOutput.Stats.BakeSeconds = FPlatformTime::Seconds() - BakeStart;
	UE_LOG(LogTemp, Display, TEXT("MH shadow trace stats: PhysicsTexels=%d ComponentTexels=%d MeshTexels=%d BoundsFallbackTexels=%d"),
		TraceStats.PhysicsTraceTexels,
		TraceStats.ComponentTraceTexels,
		TraceStats.MeshTraceTexels,
		TraceStats.BoundsFallbackTexels);

	const FString PackageName = FPackageName::ObjectPathToPackageName(OutputObjectPath);
	const FString AssetName = FPackageName::GetShortName(OutputObjectPath);
	UPackage* Package = CreatePackage(*PackageName);
	UMHShadowDataAsset* Asset = NewObject<UMHShadowDataAsset>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);

	Asset->Resolution = FIntPoint(Resolution, Resolution);
	Asset->TileSize = Volume->TileSize;
	Asset->DepthBias = Volume->DepthBias;
	Asset->LightOrigin = Basis.Origin;
	Asset->LightXAxis = Basis.XAxis;
	Asset->LightYAxis = Basis.YAxis;
	Asset->LightZAxis = Basis.ZAxis;
	Asset->LightSpaceMin = Basis.LightMin;
	Asset->LightSpaceMax = Basis.LightMax;
	Asset->MinLightDepth = Basis.MinDepth;
	Asset->MaxLightDepth = Basis.MaxDepth;
	Asset->RawIntervals = MoveTemp(CompressionInput.TexelIntervals);
	Asset->Intervals = MoveTemp(CompressionOutput.Intervals);
	Asset->Nodes = MoveTemp(CompressionOutput.Nodes);
	Asset->Stats = CompressionOutput.Stats;
	Asset->DebugIntervalPreview = MoveTemp(DebugPreview);

	FAssetRegistryModule::AssetCreated(Asset);
	Package->MarkPackageDirty();

	if (!SaveShadowDataAsset(OutputObjectPath, Asset))
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to save MH shadow data asset '%s'."), *OutputObjectPath);
		return 1;
	}

	WriteStatsCsv(OutputObjectPath, *Asset);
	WriteDebugPreviewPng(OutputObjectPath, *Asset);

	UE_LOG(LogTemp, Display, TEXT("MH shadow bake complete: %s"), *OutputObjectPath);
	UE_LOG(LogTemp, Display, TEXT("Resolution=%dx%d ValidTexels=%d Nodes=%d Intervals=%d RawBytes=%lld CompressedBytes=%lld Ratio=%.6f BakeSeconds=%.3f"),
		Asset->Resolution.X,
		Asset->Resolution.Y,
		Asset->Stats.ValidTexelCount,
		Asset->Stats.NodeCount,
		Asset->Stats.IntervalCount,
		Asset->Stats.RawBytes,
		Asset->Stats.CompressedBytes,
		Asset->Stats.CompressionRatio,
		Asset->Stats.BakeSeconds);

	return 0;
}

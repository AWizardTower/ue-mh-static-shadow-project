// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowDataAsset.h"

bool UMHShadowDataAsset::IsValidForRendering() const
{
	return Resolution.X > 0
		&& Resolution.X == Resolution.Y
		&& FMath::IsPowerOfTwo(Resolution.X)
		&& (Nodes.Num() > 0 || RawIntervals.Num() == Resolution.X * Resolution.Y);
}

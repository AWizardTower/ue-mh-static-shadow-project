// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowDataAsset.h"

bool UMHShadowDataAsset::IsValidForRendering() const
{
	const bool bHasRawData = RawIntervals.Num() == Resolution.X * Resolution.Y;
	const bool bHasCompressedTree = Resolution.X == Resolution.Y
		&& FMath::IsPowerOfTwo(Resolution.X)
		&& Nodes.Num() > 0;

	return Resolution.X > 0
		&& Resolution.Y > 0
		&& (bHasCompressedTree || bHasRawData);
}

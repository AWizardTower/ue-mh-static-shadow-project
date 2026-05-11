// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowDataAsset.h"

bool UMHShadowDataAsset::IsValidForRendering() const
{
	const bool bHasRawData = RawIntervals.Num() == Resolution.X * Resolution.Y;
	const bool bHasGlobalCompressedTree = Resolution.X == Resolution.Y
		&& FMath::IsPowerOfTwo(Resolution.X)
		&& Nodes.Num() > 0;
	const bool bHasTiledCompressedTree = TileSize > 0
		&& TileCount.X > 0
		&& TileCount.Y > 0
		&& Tiles.Num() == TileCount.X * TileCount.Y
		&& PageTable.Num() == Tiles.Num()
		&& Nodes.Num() > 0;

	return Resolution.X > 0
		&& Resolution.Y > 0
		&& (bHasGlobalCompressedTree || bHasTiledCompressedTree || bHasRawData);
}

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
	bool bHasClipmapTree = false;
	if (ClipmapLevels.Num() > 0 && ClipmapTiles.Num() == ClipmapPageTable.Num() && ClipmapNodes.Num() > 0)
	{
		bHasClipmapTree = true;
		for (const FMHShadowClipmapLevel& Level : ClipmapLevels)
		{
			const bool bLevelValid = Level.Resolution.X > 0
				&& Level.Resolution.Y > 0
				&& Level.TileSize > 0
				&& Level.TileCount.X > 0
				&& Level.TileCount.Y > 0
				&& Level.TileDataCount == Level.TileCount.X * Level.TileCount.Y
				&& Level.TileOffset >= 0
				&& Level.TileOffset + Level.TileDataCount <= ClipmapTiles.Num()
				&& Level.PageTableOffset >= 0
				&& Level.PageTableOffset + Level.PageTableCount <= ClipmapPageTable.Num()
				&& Level.NodeOffset >= 0
				&& Level.NodeOffset + Level.NodeCount <= ClipmapNodes.Num()
				&& Level.RawIntervalOffset >= 0
				&& Level.RawIntervalOffset + Level.RawIntervalCount <= ClipmapRawIntervals.Num()
				&& Level.PageTableCount == Level.TileDataCount;
			bHasClipmapTree &= bLevelValid;
		}
	}

	return Resolution.X > 0
		&& Resolution.Y > 0
		&& (bHasGlobalCompressedTree || bHasTiledCompressedTree || bHasClipmapTree || bHasRawData);
}

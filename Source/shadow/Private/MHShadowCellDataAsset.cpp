// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowCellDataAsset.h"

bool UMHShadowCellDataAsset::IsValidCellData() const
{
	return CellIndex >= 0
		&& GlobalClipmapTileIndices.Num() == ClipmapTiles.Num()
		&& ClipmapTiles.Num() > 0
		&& ClipmapNodes.Num() > 0;
}

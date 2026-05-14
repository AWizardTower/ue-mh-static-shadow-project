// Copyright Epic Games, Inc. All Rights Reserved.

#include "MHShadowWorldDataAsset.h"

#include "MHShadowCellDataAsset.h"

namespace
{
static int32 GetChildIndex(const FIntVector4& ChildIndices, int32 ChildSlot)
{
	switch (ChildSlot)
	{
	case 0:
		return ChildIndices.X;
	case 1:
		return ChildIndices.Y;
	case 2:
		return ChildIndices.Z;
	default:
		return ChildIndices.W;
	}
}

static void SetChildIndex(FIntVector4& ChildIndices, int32 ChildSlot, int32 ChildIndex)
{
	switch (ChildSlot)
	{
	case 0:
		ChildIndices.X = ChildIndex;
		break;
	case 1:
		ChildIndices.Y = ChildIndex;
		break;
	case 2:
		ChildIndices.Z = ChildIndex;
		break;
	default:
		ChildIndices.W = ChildIndex;
		break;
	}
}

static FMHShadowTile MakeUnavailableTile(const FMHShadowClipmapLevel& Level, int32 LocalTileIndex)
{
	const int32 TileX = LocalTileIndex % FMath::Max(1, Level.TileCount.X);
	const int32 TileY = LocalTileIndex / FMath::Max(1, Level.TileCount.X);

	FMHShadowTile Tile;
	Tile.TileCoord = FIntPoint(TileX, TileY);
	Tile.TexelRect = FIntVector4(
		TileX * Level.TileSize,
		TileY * Level.TileSize,
		(TileX + 1) * Level.TileSize,
		(TileY + 1) * Level.TileSize);
	Tile.NodeOffset = 0;
	Tile.NodeCount = 0;
	Tile.RootNodeIndex = INDEX_NONE;
	Tile.PageIndex = INDEX_NONE;
	Tile.bResidentDefault = false;
	return Tile;
}

static int64 EstimateRawDualBytesFromLevels(const TArray<FMHShadowClipmapLevel>& Levels)
{
	int64 Bytes = 0;
	for (const FMHShadowClipmapLevel& Level : Levels)
	{
		Bytes += static_cast<int64>(FMath::Max(0, Level.Resolution.X))
			* static_cast<int64>(FMath::Max(0, Level.Resolution.Y))
			* static_cast<int64>(sizeof(FVector4f));
	}
	return Bytes;
}
}

bool UMHShadowWorldDataAsset::IsValidWorldData() const
{
	int32 ExpectedPages = 0;
	bool bLevelsValid = ClipmapLevels.Num() > 0;
	for (const FMHShadowClipmapLevel& Level : ClipmapLevels)
	{
		const bool bLevelValid = Level.LevelIndex >= 0
			&& Level.Resolution.X > 0
			&& Level.Resolution.Y > 0
			&& Level.TileSize > 0
			&& Level.TileCount.X > 0
			&& Level.TileCount.Y > 0
			&& Level.TileDataCount == Level.TileCount.X * Level.TileCount.Y;
		bLevelsValid &= bLevelValid;
		ExpectedPages += FMath::Max(0, Level.TileDataCount);
	}

	return Resolution.X > 0
		&& Resolution.Y > 0
		&& TileSize > 0
		&& bLevelsValid
		&& ExpectedPages > 0
		&& (TotalVirtualPages == 0 || TotalVirtualPages == ExpectedPages);
}

bool UMHShadowWorldDataAsset::BuildFlattenedData(const TArray<const UMHShadowCellDataAsset*>& LoadedCells, FMHShadowCellFlattenedData& OutData, FString* OutError) const
{
	OutData = FMHShadowCellFlattenedData();
	if (!IsValidWorldData())
	{
		if (OutError)
		{
			*OutError = FString::Printf(TEXT("Invalid MH shadow world data asset: %s"), *GetPathName());
		}
		return false;
	}

	int32 TotalPages = 0;
	for (const FMHShadowClipmapLevel& Level : ClipmapLevels)
	{
		TotalPages = FMath::Max(TotalPages, Level.TileOffset + Level.TileDataCount);
	}
	if (TotalPages <= 0)
	{
		if (OutError)
		{
			*OutError = TEXT("MH shadow world data has no clipmap pages.");
		}
		return false;
	}

	OutData.BakeSource = BakeSource;
	OutData.ProjectionMapping = ProjectionMapping;
	OutData.Resolution = Resolution;
	OutData.TileSize = TileSize;
	OutData.TileCount = TileCount;
	OutData.DepthBias = DepthBias;
	OutData.WorldToShadowRow0 = WorldToShadowRow0;
	OutData.WorldToShadowRow1 = WorldToShadowRow1;
	OutData.WorldToShadowRow2 = WorldToShadowRow2;
	OutData.WorldToShadowRow3 = WorldToShadowRow3;
	OutData.ClipmapLevels = ClipmapLevels;
	OutData.ClipmapTiles.SetNum(TotalPages);
	OutData.ClipmapPageTable.Init(INDEX_NONE, TotalPages);
	OutData.RawDualBytes = TotalRawDualBytes > 0 ? TotalRawDualBytes : EstimateRawDualBytesFromLevels(ClipmapLevels);

	for (const FMHShadowClipmapLevel& Level : ClipmapLevels)
	{
		for (int32 LocalTileIndex = 0; LocalTileIndex < Level.TileDataCount; ++LocalTileIndex)
		{
			const int32 GlobalTileIndex = Level.TileOffset + LocalTileIndex;
			if (OutData.ClipmapTiles.IsValidIndex(GlobalTileIndex))
			{
				OutData.ClipmapTiles[GlobalTileIndex] = MakeUnavailableTile(Level, LocalTileIndex);
			}
		}
	}

	TSet<int32> WrittenTiles;
	int32 LoadedCellCount = 0;
	int64 LargestCellBytes = 0;

	for (const UMHShadowCellDataAsset* Cell : LoadedCells)
	{
		if (!Cell || !Cell->IsValidCellData())
		{
			continue;
		}

		++LoadedCellCount;
		LargestCellBytes = FMath::Max(LargestCellBytes, Cell->EstimatedCompressedBytes);
		OutData.ProviderMemoryBytes += Cell->EstimatedCompressedBytes;

		const int32 NodeBase = OutData.ClipmapNodes.Num();
		OutData.ClipmapNodes.Reserve(OutData.ClipmapNodes.Num() + Cell->ClipmapNodes.Num());
		for (FMHShadowNode Node : Cell->ClipmapNodes)
		{
			for (int32 ChildSlot = 0; ChildSlot < 4; ++ChildSlot)
			{
				const int32 LocalChildIndex = GetChildIndex(Node.ChildIndices, ChildSlot);
				SetChildIndex(Node.ChildIndices, ChildSlot, LocalChildIndex >= 0 ? LocalChildIndex + NodeBase : INDEX_NONE);
			}
			OutData.ClipmapNodes.Add(Node);
		}

		for (int32 CellTileIndex = 0; CellTileIndex < Cell->ClipmapTiles.Num(); ++CellTileIndex)
		{
			if (!Cell->GlobalClipmapTileIndices.IsValidIndex(CellTileIndex))
			{
				continue;
			}

			const int32 GlobalTileIndex = Cell->GlobalClipmapTileIndices[CellTileIndex];
			if (!OutData.ClipmapTiles.IsValidIndex(GlobalTileIndex))
			{
				if (OutError)
				{
					*OutError = FString::Printf(TEXT("Cell %s references out-of-range global tile %d of %d."),
						*Cell->GetPathName(),
						GlobalTileIndex,
						TotalPages);
				}
				return false;
			}
			if (WrittenTiles.Contains(GlobalTileIndex))
			{
				if (OutError)
				{
					*OutError = FString::Printf(TEXT("Duplicate global tile %d while flattening %s."), GlobalTileIndex, *Cell->GetPathName());
				}
				return false;
			}

			FMHShadowTile Tile = Cell->ClipmapTiles[CellTileIndex];
			Tile.NodeOffset = Tile.NodeOffset >= 0 ? Tile.NodeOffset + NodeBase : INDEX_NONE;
			Tile.RootNodeIndex = Tile.RootNodeIndex >= 0 ? Tile.RootNodeIndex + NodeBase : INDEX_NONE;
			Tile.PageIndex = GlobalTileIndex;
			Tile.bResidentDefault = true;
			OutData.ClipmapTiles[GlobalTileIndex] = Tile;
			OutData.ClipmapPageTable[GlobalTileIndex] = GlobalTileIndex;
			WrittenTiles.Add(GlobalTileIndex);
		}
	}

	OutData.LoadedCellCount = LoadedCellCount;
	OutData.LoadedVirtualPageCount = WrittenTiles.Num();
	OutData.UnavailableVirtualPageCount = TotalPages - WrittenTiles.Num();
	OutData.LargestLoadedCellBytes = LargestCellBytes;

	for (FMHShadowClipmapLevel& Level : OutData.ClipmapLevels)
	{
		Level.NodeOffset = 0;
		Level.NodeCount = OutData.ClipmapNodes.Num();
		Level.PageTableOffset = Level.TileOffset;
		Level.PageTableCount = Level.TileDataCount;
	}

	if (OutData.ClipmapNodes.Num() == 0)
	{
		if (OutError)
		{
			*OutError = TEXT("No loaded MH shadow cell contributed compressed nodes.");
		}
		return false;
	}

	return true;
}

// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class shadowEditor : ModuleRules
{
	public shadowEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"shadow"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"UnrealEd",
			"AssetRegistry",
			"EditorSubsystem",
			"Projects"
		});
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MechTechTest : ModuleRules
{
	public MechTechTest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "RenderCore", "RHI"});
	}
}

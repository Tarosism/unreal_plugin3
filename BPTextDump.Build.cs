using UnrealBuildTool;

public class BPTextDump : ModuleRules
{
    public BPTextDump(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "AssetRegistry",
            "Projects",
            "Json",
            "JsonUtilities",
            "DeveloperSettings"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UnrealEd",        // editor types
            "BlueprintGraph",  // K2 schema helpers
            "Kismet",
            "KismetWidgets",
            "KismetCompiler",
            "ContentBrowser",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "LevelEditor",
            "UMG",
            "UMGEditor"
        });

        if (!Target.bBuildEditor)
        {
            throw new BuildException("BPTextDump is editor-only.");
        }
    }
}
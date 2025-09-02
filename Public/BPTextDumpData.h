#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"

// Basic data structures for BPTextDump analysis results.

struct FPublicApiInfo {
    FString Name;
};

struct FVarMeta {
    FString Name;
    FString Cat;
    FString Sub;
    int32 Reads = 0;
    int32 Writes = 0;
};

struct FObjDefUse {
    TMap<FString, TSet<FString>> Writes;
    TMap<FString, TSet<FString>> Reads;
};

struct FVRW {
    TArray<FString> R;
    TArray<FString> W;
};

struct FAssetInfo {
    FString PkgPath;
    FString AssetType;
    FString GenClassName;
};

// Placeholder analysis result structures
struct FBlueprintFlowData { };
struct FBlueprintMetaData { };
struct FBlueprintDefUseInfo { };
struct FBlueprintFact { };
struct FBlueprintLintResult { };


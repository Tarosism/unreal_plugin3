#pragma once

#include "CoreMinimal.h"
#include "Json.h"
#include "EdGraph/EdGraphPin.h"
#include "BPTextDumpData.h"

class BPTextDumpUtils
{
public:
    static bool WriteJsonToFile(const FJsonObject& Root, const FString& OutPath);
    static bool WriteTextToFile(const FString& Text, const FString& OutPath);
    static void AddFrontMatter(TSharedRef<FJsonObject> J);
    static TSharedPtr<FJsonObject> LoadJsonObject(const FString& Path);

    // Shared analysis helpers
    static FString Sanitize(const FString& In);
    static bool IsInputDataPin(const UEdGraphPin* P);
    static bool IsDataInputPin(const UEdGraphPin* P);
    static FString PinDefaultInline(const UEdGraphPin* P);
    static FString PinDefaultOrText(const UEdGraphPin* P);
    static UEdGraphPin* FindInputPinByName(const UEdGraphNode* Node, const FName& PinName);
    static void AddVarAnchor(TMap<FString, TSet<FString>>& Map, const FString& Var, const FString& Anchor);
    static void AddObjPropAnchor(TMap<FString, FObjDefUse>& Objects, const FString& ObjName, const FString& Prop, bool bWrite, const FString& Anchor);
    static void ExtractEntryPointsFromCatalog(TSharedPtr<FJsonObject> JCatalog, TArray<FString>& OutIds);
    static void ExtractPublicApisFromFlows(const TArray<FString>& FlowJsonPaths, TArray<FPublicApiInfo>& OutApis);
    static void ExtractVarUsage(TSharedPtr<FJsonObject> JMeta, TSharedPtr<FJsonObject> JDefUse, TArray<FVarMeta>& OutVars);
    static void ExtractDependencies(TSharedPtr<FJsonObject> JDefUse, TArray<FString>& OutDeps);
    static FString NormalizeEventIdForLabel(const FString& Id);
    static FString FriendlyFromCategory(const FString& Cat, const FString& Sub);
    static FString Slug(const FString& In);
};

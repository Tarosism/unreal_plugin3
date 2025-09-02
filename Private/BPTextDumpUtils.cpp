#include "BPTextDumpUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Interfaces/IPluginManager.h"
#include "EdGraphSchema_K2.h"

bool BPTextDumpUtils::WriteJsonToFile(const FJsonObject& Root, const FString& OutPath)
{
    FString JsonStr;
    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonStr);
    if (!FJsonSerializer::Serialize(MakeShared<FJsonObject>(Root), Writer))
    {
        return false;
    }
    return FFileHelper::SaveStringToFile(JsonStr, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool BPTextDumpUtils::WriteTextToFile(const FString& Text, const FString& OutPath)
{
    FString Norm = Text;
#if PLATFORM_WINDOWS
    Norm.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
    Norm.ReplaceInline(TEXT("\r"), TEXT("\n"));
    Norm.ReplaceInline(TEXT("\n"), TEXT("\r\n"));
#endif

    FTCHARToUTF8 Conv(*Norm);
    const bool bWantBom = OutPath.EndsWith(TEXT(".md")) || OutPath.EndsWith(TEXT(".txt"));
    TArray<uint8> Bytes;
    if (bWantBom)
    {
        Bytes.Add(0xEF); Bytes.Add(0xBB); Bytes.Add(0xBF);
    }
    Bytes.Append(reinterpret_cast<const uint8*>(Conv.Get()), Conv.Length());

    if (!FFileHelper::SaveArrayToFile(Bytes, *OutPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to write %s"), *OutPath);
        return false;
    }
    return true;
}

void BPTextDumpUtils::AddFrontMatter(TSharedRef<FJsonObject> J)
{
    J->SetStringField(TEXT("ue_version"), FEngineVersion::Current().ToString());
    FString Version = TEXT("0.3-dev");
    if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("BPTextDump")))
    {
        Version = Plugin->GetDescriptor().VersionName;
    }
    J->SetStringField(TEXT("plugin_version"), Version);
}

TSharedPtr<FJsonObject> BPTextDumpUtils::LoadJsonObject(const FString& Path)
{
    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *Path))
    {
        return nullptr;
    }
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    TSharedPtr<FJsonObject> Obj;
    FJsonSerializer::Deserialize(Reader, Obj);
    return Obj;
}

FString BPTextDumpUtils::Sanitize(const FString& In)
{
    FString Out = In;
    Out.ReplaceInline(TEXT("\r"), TEXT(" "));
    Out.ReplaceInline(TEXT("\n"), TEXT(" "));
    Out.ReplaceInline(TEXT("\t"), TEXT(" "));
    return Out;
}

bool BPTextDumpUtils::IsInputDataPin(const UEdGraphPin* P)
{
    return P && P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec;
}

bool BPTextDumpUtils::IsDataInputPin(const UEdGraphPin* P)
{
    return IsInputDataPin(P);
}

FString BPTextDumpUtils::PinDefaultInline(const UEdGraphPin* P)
{
    if (!P) return FString();
    FString V = P->DefaultValue;
    if (V.IsEmpty())
    {
        V = P->DefaultTextValue.ToString();
    }
    return Sanitize(V);
}

FString BPTextDumpUtils::PinDefaultOrText(const UEdGraphPin* P)
{
    return PinDefaultInline(P);
}

UEdGraphPin* BPTextDumpUtils::FindInputPinByName(const UEdGraphNode* Node, const FName& PinName)
{
    if (!Node) return nullptr;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && IsInputDataPin(Pin) && Pin->PinName == PinName)
        {
            return Pin;
        }
    }
    return nullptr;
}

void BPTextDumpUtils::AddVarAnchor(TMap<FString, TSet<FString>>& Map, const FString& Var, const FString& Anchor)
{
    Map.FindOrAdd(Var).Add(Anchor);
}

void BPTextDumpUtils::AddObjPropAnchor(TMap<FString, FObjDefUse>& Objects, const FString& ObjName, const FString& Prop, bool bWrite, const FString& Anchor)
{
    FObjDefUse& DU = Objects.FindOrAdd(ObjName);
    TSet<FString>& Set = bWrite ? DU.Writes.FindOrAdd(Prop) : DU.Reads.FindOrAdd(Prop);
    Set.Add(Anchor);
}

void BPTextDumpUtils::ExtractEntryPointsFromCatalog(TSharedPtr<FJsonObject> JCatalog, TArray<FString>& OutIds)
{
    if (!JCatalog) return;
    const TArray<TSharedPtr<FJsonValue>>* Slices;
    if (JCatalog->TryGetArrayField(TEXT("slices"), Slices))
    {
        for (const TSharedPtr<FJsonValue>& V : *Slices)
        {
            TSharedPtr<FJsonObject> Obj = V->AsObject();
            if (Obj.IsValid())
            {
                FString Id;
                if (Obj->TryGetStringField(TEXT("id"), Id))
                {
                    OutIds.Add(Id);
                }
            }
        }
    }
}

void BPTextDumpUtils::ExtractPublicApisFromFlows(const TArray<FString>& FlowJsonPaths, TArray<FPublicApiInfo>& OutApis)
{
    for (const FString& Path : FlowJsonPaths)
    {
        TSharedPtr<FJsonObject> Obj = LoadJsonObject(Path);
        if (!Obj) continue;
        const TArray<TSharedPtr<FJsonValue>>* Arr;
        if (Obj->TryGetArrayField(TEXT("public_api"), Arr))
        {
            for (const TSharedPtr<FJsonValue>& V : *Arr)
            {
                FPublicApiInfo Info;
                if (V->Type == EJson::String)
                {
                    Info.Name = V->AsString();
                }
                else if (TSharedPtr<FJsonObject> O = V->AsObject())
                {
                    O->TryGetStringField(TEXT("name"), Info.Name);
                }
                if (!Info.Name.IsEmpty())
                {
                    OutApis.Add(Info);
                }
            }
        }
    }
}

void BPTextDumpUtils::ExtractVarUsage(TSharedPtr<FJsonObject> JMeta, TSharedPtr<FJsonObject> JDefUse, TArray<FVarMeta>& OutVars)
{
    if (!JMeta || !JDefUse) return;
    const TSharedPtr<FJsonObject>* VarsMeta;
    const TSharedPtr<FJsonObject>* VarsDef;
    if (!JMeta->TryGetObjectField(TEXT("vars"), VarsMeta) || !JDefUse->TryGetObjectField(TEXT("vars"), VarsDef))
    {
        return;
    }
    for (const auto& Kvp : (*VarsDef)->Values)
    {
        const FString& Name = Kvp.Key;
        FVarMeta Meta;
        Meta.Name = Name;
        const TSharedPtr<FJsonObject>* MV;
        if ((*VarsMeta)->TryGetObjectField(Name, MV))
        {
            (*MV)->TryGetStringField(TEXT("cat"), Meta.Cat);
            (*MV)->TryGetStringField(TEXT("sub"), Meta.Sub);
        }
        const TSharedPtr<FJsonObject>* RW;
        if (Kvp.Value->TryGetObject(RW))
        {
            const TArray<TSharedPtr<FJsonValue>>* RArr;
            if ((*RW)->TryGetArrayField(TEXT("reads"), RArr)) Meta.Reads = RArr->Num();
            const TArray<TSharedPtr<FJsonValue>>* WArr;
            if ((*RW)->TryGetArrayField(TEXT("writes"), WArr)) Meta.Writes = WArr->Num();
        }
        OutVars.Add(Meta);
    }
    OutVars.Sort([](const FVarMeta& A, const FVarMeta& B)
    {
        return (A.Reads + A.Writes) > (B.Reads + B.Writes);
    });
    if (OutVars.Num() > 3)
    {
        OutVars.SetNum(3);
    }
}

void BPTextDumpUtils::ExtractDependencies(TSharedPtr<FJsonObject> JDefUse, TArray<FString>& OutDeps)
{
    if (!JDefUse) return;
    const TSharedPtr<FJsonObject>* Objs;
    if (JDefUse->TryGetObjectField(TEXT("objects"), Objs))
    {
        for (const auto& Kvp : (*Objs)->Values)
        {
            OutDeps.Add(Kvp.Key);
        }
        OutDeps.Sort();
    }
}

FString BPTextDumpUtils::NormalizeEventIdForLabel(const FString& Id)
{
    int32 DotIdx;
    FString Out = Id;
    if (Out.FindChar(TEXT('.'), DotIdx))
    {
        Out = Out.Mid(DotIdx + 1);
    }
    Out.ReplaceInline(TEXT("_"), TEXT(" "));
    return Out;
}

FString BPTextDumpUtils::FriendlyFromCategory(const FString& Cat, const FString& Sub)
{
    if (!Sub.IsEmpty()) return Sub;
    if (Cat == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
    if (Cat == UEdGraphSchema_K2::PC_Int) return TEXT("int");
    if (Cat == UEdGraphSchema_K2::PC_Int64) return TEXT("int64");
    if (Cat == UEdGraphSchema_K2::PC_Real) return TEXT("float");
    if (Cat == UEdGraphSchema_K2::PC_Name) return TEXT("name");
    if (Cat == UEdGraphSchema_K2::PC_String) return TEXT("string");
    return Cat;
}

FString BPTextDumpUtils::Slug(const FString& In)
{
    FString Out;
    bool bDash = false;
    for (TCHAR C : In)
    {
        if (FChar::IsAlnum(C))
        {
            Out.AppendChar(FChar::ToLower(C));
            bDash = false;
        }
        else if (C == TEXT(' ') || C == TEXT('-') || C == TEXT('_'))
        {
            if (!bDash && Out.Len() > 0)
            {
                Out.AppendChar(TEXT('-'));
                bDash = true;
            }
        }
    }
    if (Out.EndsWith(TEXT("-")))
    {
        Out.LeftChopInline(1);
    }
    return Out;
}


#include "BPTextDumpUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "Interfaces/IPluginManager.h"

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


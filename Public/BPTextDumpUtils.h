#pragma once

#include "CoreMinimal.h"
#include "Json.h"

class BPTextDumpUtils
{
public:
    static bool WriteJsonToFile(const FJsonObject& Root, const FString& OutPath);
    static bool WriteTextToFile(const FString& Text, const FString& OutPath);
    static void AddFrontMatter(TSharedRef<FJsonObject> J);
    static TSharedPtr<FJsonObject> LoadJsonObject(const FString& Path);
};

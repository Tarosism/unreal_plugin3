#pragma once
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "BPTextDumpSettings.generated.h"

UCLASS(config = EditorPerProjectUserSettings, defaultconfig, meta = (DisplayName = "BP Text Dump"))
class UBPTextDumpSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, config, Category = "Defaults")
    FString DefaultRootPath = TEXT("/Game");

    UPROPERTY(EditAnywhere, config, Category = "Defaults", meta = (ToolTip = "Empty = Saved/BPTextDump"))
    FString DefaultOutDir;
};

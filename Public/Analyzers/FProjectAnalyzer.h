#pragma once

#include "CoreMinimal.h"
#include "BPTextDumpData.h"

class FProjectAnalyzer {
public:
    explicit FProjectAnalyzer(const TArray<FAssetData>& InAssets);
    void Analyze();
    const TSharedPtr<FJsonObject>& GetResult() const;

private:
    TArray<FAssetData> Assets;
    TSharedPtr<FJsonObject> Result;
};

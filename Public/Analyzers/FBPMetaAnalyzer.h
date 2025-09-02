#pragma once

#include "CoreMinimal.h"
#include "BPTextDumpData.h"

class FBPMetaAnalyzer {
public:
    explicit FBPMetaAnalyzer(const UBlueprint* InBlueprint);
    void Analyze();
    const FBlueprintMetaData& GetResult() const;

private:
    const UBlueprint* Blueprint;
    FBlueprintMetaData Result;
};

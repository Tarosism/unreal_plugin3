#pragma once

#include "CoreMinimal.h"
#include "BPTextDumpData.h"

class FBPFlowAnalyzer {
public:
    explicit FBPFlowAnalyzer(const UBlueprint* InBlueprint);
    void Analyze();
    const FBlueprintFlowData& GetResult() const;

private:
    const UBlueprint* Blueprint;
    FBlueprintFlowData Result;
};

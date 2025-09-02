#pragma once

#include "CoreMinimal.h"
#include "BPTextDumpData.h"

class FBPDefUseAnalyzer {
public:
    FBPDefUseAnalyzer(const UBlueprint* InBlueprint, const FBlueprintFlowData& InFlowData);
    void Analyze();
    const FBlueprintDefUseInfo& GetResult() const;

private:
    const UBlueprint* Blueprint;
    const FBlueprintFlowData& FlowData;
    FBlueprintDefUseInfo Result;
};

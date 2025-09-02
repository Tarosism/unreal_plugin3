#pragma once

#include "CoreMinimal.h"
#include "BPTextDumpData.h"

class FBPFactsAnalyzer {
public:
    FBPFactsAnalyzer(const UBlueprint* InBlueprint, const FBlueprintFlowData& InFlowData);
    void Analyze();
    const TArray<FBlueprintFact>& GetResult() const;

private:
    const UBlueprint* Blueprint;
    const FBlueprintFlowData& FlowData;
    TArray<FBlueprintFact> Result;
};

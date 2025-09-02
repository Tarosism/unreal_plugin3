#pragma once

#include "CoreMinimal.h"
#include "BPTextDumpData.h"

class FBPLintAnalyzer {
public:
    FBPLintAnalyzer(const UBlueprint* InBlueprint,
                    const FBlueprintMetaData& InMeta,
                    const FBlueprintFlowData& InFlow,
                    const FBlueprintDefUseInfo& InDefUse,
                    const TArray<FBlueprintFact>& InFacts);
    void Analyze();
    const TArray<FBlueprintLintResult>& GetResult() const;

private:
    const UBlueprint* Blueprint;
    const FBlueprintMetaData& Meta;
    const FBlueprintFlowData& Flow;
    const FBlueprintDefUseInfo& DefUse;
    const TArray<FBlueprintFact>& Facts;
    TArray<FBlueprintLintResult> Result;
};

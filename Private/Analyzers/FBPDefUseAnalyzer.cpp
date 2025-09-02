#include "Analyzers/FBPDefUseAnalyzer.h"

FBPDefUseAnalyzer::FBPDefUseAnalyzer(const UBlueprint* InBlueprint, const FBlueprintFlowData& InFlowData)
    : Blueprint(InBlueprint)
    , FlowData(InFlowData)
{
}

void FBPDefUseAnalyzer::Analyze()
{
    // Def-use analysis logic will be implemented here
}

const FBlueprintDefUseInfo& FBPDefUseAnalyzer::GetResult() const
{
    return Result;
}


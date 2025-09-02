#include "Analyzers/FBPFlowAnalyzer.h"

FBPFlowAnalyzer::FBPFlowAnalyzer(const UBlueprint* InBlueprint)
    : Blueprint(InBlueprint)
{
}

void FBPFlowAnalyzer::Analyze()
{
    // Flow analysis logic will be implemented here
}

const FBlueprintFlowData& FBPFlowAnalyzer::GetResult() const
{
    return Result;
}


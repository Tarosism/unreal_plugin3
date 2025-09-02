#include "Analyzers/FBPFactsAnalyzer.h"

FBPFactsAnalyzer::FBPFactsAnalyzer(const UBlueprint* InBlueprint, const FBlueprintFlowData& InFlowData)
    : Blueprint(InBlueprint)
    , FlowData(InFlowData)
{
}

void FBPFactsAnalyzer::Analyze()
{
    // Facts analysis logic will be implemented here
}

const TArray<FBlueprintFact>& FBPFactsAnalyzer::GetResult() const
{
    return Result;
}


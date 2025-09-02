#include "Analyzers/FBPLintAnalyzer.h"

FBPLintAnalyzer::FBPLintAnalyzer(const UBlueprint* InBlueprint,
                                 const FBlueprintMetaData& InMeta,
                                 const FBlueprintFlowData& InFlow,
                                 const FBlueprintDefUseInfo& InDefUse,
                                 const TArray<FBlueprintFact>& InFacts)
    : Blueprint(InBlueprint)
    , Meta(InMeta)
    , Flow(InFlow)
    , DefUse(InDefUse)
    , Facts(InFacts)
{
}

void FBPLintAnalyzer::Analyze()
{
    // Lint analysis logic will be implemented here
}

const TArray<FBlueprintLintResult>& FBPLintAnalyzer::GetResult() const
{
    return Result;
}


#include "Analyzers/FBPMetaAnalyzer.h"

FBPMetaAnalyzer::FBPMetaAnalyzer(const UBlueprint* InBlueprint)
    : Blueprint(InBlueprint)
{
}

void FBPMetaAnalyzer::Analyze()
{
    // Metadata analysis logic will be implemented here
}

const FBlueprintMetaData& FBPMetaAnalyzer::GetResult() const
{
    return Result;
}


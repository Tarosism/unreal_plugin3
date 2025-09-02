#include "Analyzers/FProjectAnalyzer.h"
#include "Json.h"

FProjectAnalyzer::FProjectAnalyzer(const TArray<FAssetData>& InAssets)
    : Assets(InAssets)
{
}

void FProjectAnalyzer::Analyze()
{
    // Project-wide analysis logic will be implemented here
    Result = MakeShared<FJsonObject>();
}

const TSharedPtr<FJsonObject>& FProjectAnalyzer::GetResult() const
{
    return Result;
}


#include "BPTextDumpModule.h" // MUST be first include
#include "BTD_Anchors.h"
#include "BTD_Hash.h"
#include "K2Node_Knot.h"          // 리루트(리라우트) 핀 추적
#include "EdGraph/EdGraphPin.h"


#include "Misc/EngineVersion.h"
#include "Misc/App.h"
#include "Interfaces/IPluginManager.h"
#include "ToolMenus.h"
#include "LevelEditor.h"
#include "ContentBrowserModule.h"       
#include "BPTextDumpSettings.h"         
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"      // GetVarName()
#include "K2Node_Event.h"         // EventReference
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_FunctionEntry.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Json.h"
#include "JsonObjectConverter.h"
#include "ContentBrowserModule.h"      // for selection
#include "IContentBrowserSingleton.h"  // for selection
#include "UObject/UObjectGlobals.h" // for UObjectInitialized()
#include "Framework/Commands/UIAction.h" // ← FUIAction/FExecuteAction/FCanExecuteAction
// --- Components / SCS
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/PrimitiveComponent.h"
// --- Widgets / WidgetTree
#include "WidgetBlueprint.h"                 // UMGEditor 모듈
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "BPTextDumpData.h"
#include "BPTextDumpUtils.h"
#include "Analyzers/FBPFlowAnalyzer.h"
#include "Analyzers/FBPMetaAnalyzer.h"
#include "Analyzers/FBPDefUseAnalyzer.h"
#include "Analyzers/FBPFactsAnalyzer.h"
#include "Analyzers/FBPLintAnalyzer.h"
#include "Analyzers/FProjectAnalyzer.h"




//#include "Kismet2/BlueprintEditorUtils.h" // optional (for extra helpers)

IMPLEMENT_MODULE(FBPTextDumpModule, BPTextDump);

static FString DefaultOutDir()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BPTextDump"));
}

static void CollectTopLevelGraphs(UBlueprint* BP, TArray<UEdGraph*>& OutGraphs)
{
    if (!BP) return;
    TArray<UEdGraph*> All;
    BP->GetAllGraphs(All); 
    for (UEdGraph* G : All)
    {
        if (IsValid(G) && G->GetOuter() == BP)
        {
            OutGraphs.Add(G);
        }
    }
}

static FString BuildBPFlowDSL(UBlueprint* BP, UEdGraph* Graph)
{
    FString Out;
    Out += FString::Printf(TEXT("BP %s :: Graph %s\n"), *BP->GetName(), *Graph->GetName());

    TArray<UEdGraphNode*> Nodes = Graph->Nodes;
    Nodes.RemoveAll([](UEdGraphNode* N) { return !IsValid(N); });
    Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
        {
            if (A.NodePosY != B.NodePosY) return A.NodePosY < B.NodePosY;
            return A.NodePosX < B.NodePosX;
        });

    TMap<const UEdGraphNode*, int32> Idx; int32 Next = 1;
    for (UEdGraphNode* N : Nodes) Idx.Add(N, Next++);

    for (UEdGraphNode* N : Nodes)
    {
        const FString Title = BPTextDumpUtils::Sanitize(N->GetNodeTitle(ENodeTitleType::ListView).ToString());
        const FString AKey = BTD::AnchorForNode(N);
        FString Line = FString::Printf(TEXT("@%d[%s] %s | %s"),
            Idx[N], *AKey, *N->GetClass()->GetName(), *Title);


        if (const UK2Node_Variable* VNode = Cast<UK2Node_Variable>(N))
        {
            const FName VarName = VNode->GetVarName();
            if (!VarName.IsNone())
            {
                Line += FString::Printf(TEXT(" | Var: %s"), *VarName.ToString());
            }
        }
        else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(N))
        {
            if (UFunction* Fn = Call->GetTargetFunction())
            {
                Line += FString::Printf(TEXT(" | Func: %s"), *Fn->GetName());

                int Shown = 0;
                for (UEdGraphPin* P : N->Pins)
                {
                    if (!BPTextDumpUtils::IsInputDataPin(P)) continue;
                    if (P->LinkedTo.Num() > 0) continue;

                    const FString V = BPTextDumpUtils::PinDefaultInline(P);
                    if (!V.IsEmpty())
                    {
                        Line += FString::Printf(TEXT(" | %s: %s"), *P->PinName.ToString(), *V);
                        if (++Shown >= 3) break;
                    }
                }
            }
        }
        else if (const UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(N))
        {
            Line += FString::Printf(TEXT(" | Event: %s"), *CE->CustomFunctionName.ToString());
        }
        else if (const UK2Node_Event* Ev = Cast<UK2Node_Event>(N))
        {
            const FName EvName = Ev->EventReference.GetMemberName();
            if (!EvName.IsNone())
            {
                Line += FString::Printf(TEXT(" | Event: %s"), *EvName.ToString());
            }
        }

        Out += Line + TEXT("\n");
    }

    for (UEdGraphNode* N : Nodes)
    {
        for (UEdGraphPin* P : N->Pins)
        {
            if (!P || P->Direction != EGPD_Output) continue;
            const bool bExec = (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
            for (UEdGraphPin* L : P->LinkedTo)
            {
                if (!L || !L->GetOwningNode()) continue;
                const int32 A = Idx[N];
                const int32 B = Idx[L->GetOwningNode()];
                if (bExec)
                {
                    Out += FString::Printf(TEXT("@%d > @%d : %s\n"), A, B, *P->PinName.ToString());
                }
                else
                {
                    Out += FString::Printf(TEXT("@%d.%s -> @%d.%s\n"),
                        A, *P->PinName.ToString(),
                        B, *L->PinName.ToString());
                }
            }
        }
    }

    return Out;
}

static bool LooksNumericStrict(const FString& X)
{
    if (X.IsEmpty()) return false;
    bool bDigit = false, bDot = false;
    for (int32 i = 0; i < X.Len(); ++i)
    {
        TCHAR c = X[i];
        if (i == 0 && (c == TCHAR('-') || c == TCHAR('+'))) continue;
        if (c == TCHAR('.')) { if (bDot) return false; bDot = true; continue; }
        if (c >= '0' && c <= '9') { bDigit = true; continue; }
        return false;
    }
    return bDigit;
}

static void BuildLintForBP(
    UBlueprint* BP,
    const TMap<UEdGraph*, TArray<UEdGraphNode*>>& SortedByGraph,
    const FString& OutRoot)
{
    if (!BP) return;

    // Evidence 수집 컨테이너
    TMap<FString, TArray<FString>> MagicConstToAnchors;   // 상수 문자열 -> 앵커들
    TArray<TArray<FString>>        SingletonEvSets;       // 각 사례별 앵커 목록
    TArray<FString>                SkelAnchors;           // SKEL_* 발견 앵커
    TArray<FString>                UncheckedCastAnchors;  // Dynamic Cast의 CastFailed 미연결
    TArray<FString>                HardPathAnchors;       // 하드 경로 리터럴
    int32                          GetAllActorsCount = 0; // 무거운 호출 카운트

    // defuse/vars 로드 (Read/Write-only 변수 검출)
    const FString PkgPath = BP->GetOutermost()->GetName();
    const FString PkgDir = FPaths::GetPath(PkgPath);
    const FString BPDir = FPaths::Combine(OutRoot, PkgDir);
    const FString DefUsePath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpdefuse.json"), *BP->GetName()));
    TSharedPtr<FJsonObject> JDefUse = BPTextDumpUtils::LoadJsonObject(DefUsePath);
    TMap<FString, FVRW> VarRW;
    if (JDefUse.IsValid() && JDefUse->HasField(TEXT("vars")))
    {
        const TSharedPtr<FJsonObject>& VObj = JDefUse->GetObjectField(TEXT("vars"));
        for (const auto& kv : VObj->Values)
        {
            const FString VName = kv.Key;
            const TSharedPtr<FJsonObject>* One = nullptr;
            if (!kv.Value->TryGetObject(One)) continue;
            FVRW RW;
            const TArray<TSharedPtr<FJsonValue>>* RArr = nullptr;
            const TArray<TSharedPtr<FJsonValue>>* WArr = nullptr;
            if ((*One)->TryGetArrayField(TEXT("reads"), RArr))
                for (const auto& v : *RArr) { FString s; v->TryGetString(s); if (!s.IsEmpty()) RW.R.Add(s); }
            if ((*One)->TryGetArrayField(TEXT("writes"), WArr))
                for (const auto& v : *WArr) { FString s; v->TryGetString(s); if (!s.IsEmpty()) RW.W.Add(s); }
            VarRW.Add(VName, MoveTemp(RW));
        }
    }

    // 그래프 스캔
    for (const auto& KVP : SortedByGraph)
    {
        const TArray<UEdGraphNode*>& Nodes = KVP.Value;
        // 앵커 캐시
        TMap<const UEdGraphNode*, FString> AKey;
        for (UEdGraphNode* N : Nodes) if (IsValid(N)) AKey.Add(N, TEXT("@") + BTD::AnchorForNode(N));

        for (UEdGraphNode* N : Nodes)
        {
            if (!IsValid(N)) continue;
            const FString A = AKey[N];

            // MagicConstant: 비교 노드에서 상수 입력 수집
            if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(N))
            {
                const UFunction* Fn = Call->GetTargetFunction();
                const FString FnName = Fn ? Fn->GetName() : TEXT("");
                if (FnName.StartsWith(TEXT("EqualEqual")) ||
                    FnName.StartsWith(TEXT("Greater")) ||
                    FnName.StartsWith(TEXT("Less")))
                {
                    for (UEdGraphPin* In : N->Pins)
                    {
                        if (!BPTextDumpUtils::IsDataInputPin(In)) continue;
                        if (In->LinkedTo.Num() == 0)
                        {
                            const FString D = BPTextDumpUtils::PinDefaultOrText(In);
                            if (!D.IsEmpty())
                            {
                                const bool bNum = LooksNumericStrict(D);
                                const bool bInteresting =
                                    (!bNum && D.Len() >= 3) ||
                                    (bNum && (FCString::Atod(*D) >= 100.0));
                                if (bInteresting)
                                {
                                    MagicConstToAnchors.FindOrAdd(D).Add(A);
                                }
                            }
                        }
                    }
                }
            }

            // SingletonAssumption & GetAllActorsOfClass 카운트
            if (const UK2Node_CallFunction* CF_GetAll = Cast<UK2Node_CallFunction>(N))
            {
                const UFunction* F = CF_GetAll->GetTargetFunction();
                if (F && F->GetName().Contains(TEXT("GetAllActorsOfClass")))
                {
                    ++GetAllActorsCount;
                    for (UEdGraphPin* Out : CF_GetAll->Pins)
                    {
                        if (!Out || Out->Direction != EGPD_Output) continue;
                        for (UEdGraphPin* L : Out->LinkedTo)
                        {
                            if (!L || !L->GetOwningNode()) continue;
                            UK2Node_CallFunction* CF_ArrayGet = Cast<UK2Node_CallFunction>(L->GetOwningNode());
                            if (!CF_ArrayGet) continue;
                            UFunction* F2 = CF_ArrayGet->GetTargetFunction();
                            if (!F2 || !F2->GetName().Contains(TEXT("Array_Get"))) continue;
                            UEdGraphPin* Idx = BPTextDumpUtils::FindInputPinByName(CF_ArrayGet, TEXT("Index"));
                            const FString D = BPTextDumpUtils::PinDefaultOrText(Idx);
                            if (D == TEXT("0"))
                            {
                                TArray<FString> Ev; Ev.Add(AKey[CF_GetAll]); Ev.Add(AKey[CF_ArrayGet]);
                                for (UEdGraphPin* Out2 : CF_ArrayGet->Pins)
                                {
                                    if (!Out2 || Out2->Direction != EGPD_Output) continue;
                                    for (UEdGraphPin* L2 : Out2->LinkedTo)
                                    {
                                        if (!L2 || !L2->GetOwningNode()) continue;
                                        if (const UK2Node_VariableSet* VS = Cast<UK2Node_VariableSet>(L2->GetOwningNode()))
                                        {
                                            Ev.Add(AKey[VS]);
                                        }
                                    }
                                }
                                TSet<FString> Dd(Ev); Ev = Dd.Array(); Ev.Sort();
                                SingletonEvSets.Add(MoveTemp(Ev));
                            }
                        }
                    }
                }
            }

            // SKELPathDependency + HardPathLiteral
            if (const UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(N))
            {
                const UFunction* Fn = CF->GetTargetFunction();
                const FString OwnerPath = Fn && Fn->GetOwnerClass() ? Fn->GetOwnerClass()->GetPathName() : TEXT("");
                if (OwnerPath.Contains(TEXT("SKEL_"))) SkelAnchors.Add(A);
                for (UEdGraphPin* P : N->Pins)
                {
                    if (!P || P->Direction != EGPD_Input) continue;
                    if (!P->DefaultValue.IsEmpty() && (P->DefaultValue.Contains(TEXT("/Game/")) || P->DefaultValue.Contains(TEXT("/Script/"))))
                        HardPathAnchors.Add(A);
                    if (!P->DefaultTextValue.IsEmpty())
                    {
                        const FString T = P->DefaultTextValue.ToString();
                        if (T.Contains(TEXT("/Game/")) || T.Contains(TEXT("/Script/"))) HardPathAnchors.Add(A);
                    }
                }
            }
            for (UEdGraphPin* P : N->Pins)
            {
                if (!P || P->Direction != EGPD_Input) continue;
                if (P->DefaultObject)
                {
                    const FString ObjPath = P->DefaultObject->GetPathName();
                    if (ObjPath.Contains(TEXT("SKEL_"))) SkelAnchors.Add(A);
                }
                if (!P->DefaultValue.IsEmpty() && P->DefaultValue.Contains(TEXT("SKEL_"))) SkelAnchors.Add(A);
                if (!P->DefaultValue.IsEmpty() && (P->DefaultValue.Contains(TEXT("/Game/")) || P->DefaultValue.Contains(TEXT("/Script/"))))
                    HardPathAnchors.Add(A);
            }

            // UncheckedCast: Dynamic Cast 실패 핀 미연결
            if (N->GetClass()->GetName().Contains(TEXT("K2Node_DynamicCast")))
            {
                for (UEdGraphPin* P : N->Pins)
                {
                    if (!P || P->Direction != EGPD_Output) continue;
                    const FString Nm = P->PinName.ToString();
                    if (Nm.Equals(TEXT("CastFailed"), ESearchCase::IgnoreCase) || Nm.Equals(TEXT("Cast Failed"), ESearchCase::IgnoreCase))
                    {
                        if (P->LinkedTo.Num() == 0) { UncheckedCastAnchors.Add(A); break; }
                    }
                }
            }
        } // for nodes
    } // for graphs

    // 리포트 구성
    int MagicIssues = 0;
    for (auto& kv : MagicConstToAnchors)
    {
        TArray<FString>& Ev = kv.Value;
        TSet<FString> Dd(Ev); Ev = Dd.Array();
        if (Ev.Num() >= 2) ++MagicIssues;
    }
    const int SingletonIssues = SingletonEvSets.Num();
    const int SkelIssues = SkelAnchors.Num() > 0 ? 1 : 0;
    const int UncheckedCastIssues = UncheckedCastAnchors.Num();
    const int HardPathIssues = HardPathAnchors.Num() > 0 ? 1 : 0;
    int WriteOnlyIssues = 0, ReadOnlyIssues = 0;
    for (const auto& kv : VarRW)
    {
        const FVRW& RW = kv.Value;
        if (RW.W.Num() > 0 && RW.R.Num() == 0) ++WriteOnlyIssues;
        if (RW.R.Num() > 0 && RW.W.Num() == 0) ++ReadOnlyIssues;
    }
    const int WarnCount = MagicIssues + SingletonIssues + UncheckedCastIssues + WriteOnlyIssues + (GetAllActorsCount >= 2 ? 1 : 0);
    const int InfoCount = SkelIssues + HardPathIssues + ReadOnlyIssues;

    FString MD;
    MD += TEXT("# Blueprint Lint Report: ");
    MD += BP->GetName();
    MD += TEXT("\n\n## Summary\n");
    MD += FString::Printf(TEXT("- ❗ **Warnings: %d**\n"), WarnCount);
    MD += FString::Printf(TEXT("- ℹ️ **Infos: %d**\n\n---\n\n"), InfoCount);

    if (WarnCount > 0)
    {
        MD += TEXT("### ❗ Warnings\n\n");
        // MagicConstant
        for (auto& kv : MagicConstToAnchors)
        {
            TArray<FString> Ev = kv.Value;
            TSet<FString> Dd(Ev); Ev = Dd.Array(); Ev.Sort();
            if (Ev.Num() < 2) continue;
            MD += TEXT("- **[WARN][MagicConstant]**\n");
            MD += FString::Printf(TEXT("  - **Description**: 리터럴 값 `%s`가 비교 로직에서 반복 사용됩니다. 의미를 명시하고 변경 비용을 줄이기 위해 상수/Enum으로 추출을 권장합니다.\n"), *kv.Key);
            MD += TEXT("  - **Evidence**: ");
            for (int i = 0; i < Ev.Num(); ++i) { if (i > 0) MD += TEXT(", "); MD += TEXT("⟦") + Ev[i] + TEXT("⟧"); }
            MD += TEXT("\n\n");
        }
        // SingletonAssumption
        for (const TArray<FString>& Ev : SingletonEvSets)
        {
            MD += TEXT("- **[WARN][SingletonAssumption]**\n");
            MD += TEXT("  - **Description**: `GetAllActorsOfClass` 결과의 첫 원소(`[0]`)를 가정해 사용하고 있습니다. 액터 개수 변동 시 예외/오동작 위험이 있습니다. 유효성 검사 및 반복 처리(ForEach) 고려를 권장합니다.\n");
            MD += TEXT("  - **Evidence**: ");
            for (int i = 0; i < Ev.Num(); ++i) { if (i > 0) MD += TEXT(", "); MD += TEXT("⟦") + Ev[i] + TEXT("⟧"); }
            MD += TEXT("\n\n");
        }
        // UncheckedCast
        if (UncheckedCastAnchors.Num() > 0)
        {
            TSet<FString> Dd(UncheckedCastAnchors); UncheckedCastAnchors = Dd.Array(); UncheckedCastAnchors.Sort();
            MD += TEXT("- **[WARN][UncheckedCast]**\n");
            MD += TEXT("  - **Description**: Dynamic Cast의 `Cast Failed` 실행 경로가 연결되지 않았습니다. 캐스트 실패 시 안전장치가 없습니다. 분기 처리 또는 `IsValid` 체크를 권장합니다.\n");
            MD += TEXT("  - **Evidence**: ");
            for (int i = 0; i < UncheckedCastAnchors.Num(); ++i) { if (i > 0) MD += TEXT(", "); MD += TEXT("⟦") + UncheckedCastAnchors[i] + TEXT("⟧"); }
            MD += TEXT("\n\n");
        }
        // WriteOnlyVariable
        for (const auto& kv : VarRW)
        {
            const FString& Name = kv.Key; const FVRW& RW = kv.Value;
            if (RW.W.Num() > 0 && RW.R.Num() == 0)
            {
                MD += TEXT("- **[WARN][WriteOnlyVariable]**\n");
                MD += FString::Printf(TEXT("  - **Description**: 변수 `%s`는 값이 기록되지만 어디에서도 읽히지 않습니다. 불필요한 상태거나, 연결 누락일 수 있습니다.\n"), *Name);
                MD += TEXT("  - **Evidence**: ");
                TArray<FString> Ev = RW.W; TSet<FString> Dd2(Ev); Ev = Dd2.Array(); Ev.Sort();
                for (int i = 0; i < Ev.Num(); ++i) { if (i > 0) MD += TEXT(", "); MD += TEXT("⟦") + Ev[i] + TEXT("⟧"); }
                MD += TEXT("\n\n");
            }
        }
        // HeavyCallMultipleGetAll
        if (GetAllActorsCount >= 2)
        {
            MD += TEXT("- **[WARN][HeavyCallMultipleGetAll]**\n");
            MD += TEXT("  - **Description**: `GetAllActorsOfClass` 호출이 여러 번 감지되었습니다. 비용이 큰 호출로, 캐싱/필터링/공유를 고려하세요.\n\n");
        }
    }

    if (InfoCount > 0)
    {
        MD += TEXT("### ℹ️ Infos\n\n");
        // SKEL
        if (SkelAnchors.Num() > 0)
        {
            TSet<FString> Dd(SkelAnchors); SkelAnchors = Dd.Array(); SkelAnchors.Sort();
            MD += TEXT("- **[INFO][SKELPathDependency]**\n");
            MD += TEXT("  - **Description**: `SKEL_*` 스켈레톤 클래스/경로에 대한 의존이 감지되었습니다. 패키징/런타임 환경에서 문제를 유발할 수 있으니, Soft Object Path 또는 적절한 타입으로의 교체를 검토하세요.\n");
            MD += TEXT("  - **Evidence**: ");
            for (int i = 0; i < SkelAnchors.Num(); ++i) { if (i > 0) MD += TEXT(", "); MD += TEXT("⟦") + SkelAnchors[i] + TEXT("⟧"); }
            MD += TEXT("\n\n");
        }
        // HardPath
        if (HardPathAnchors.Num() > 0)
        {
            TSet<FString> Dd(HardPathAnchors); HardPathAnchors = Dd.Array(); HardPathAnchors.Sort();
            MD += TEXT("- **[INFO][HardPathLiteral]**\n");
            MD += TEXT("  - **Description**: 핀 기본값에 하드 경로(`/Game/`, `/Script/`) 문자열이 포함되어 있습니다. 리네임/리디렉션/패키징 시 깨질 수 있으므로 Soft Object Path 또는 데이터에 의존하는 방식 권장.\n");
            MD += TEXT("  - **Evidence**: ");
            for (int i = 0; i < HardPathAnchors.Num(); ++i) { if (i > 0) MD += TEXT(", "); MD += TEXT("⟦") + HardPathAnchors[i] + TEXT("⟧"); }
            MD += TEXT("\n\n");
        }
        // ReadOnlyVariable
        for (const auto& kv : VarRW)
        {
            const FString& Name = kv.Key; const FVRW& RW = kv.Value;
            if (RW.R.Num() > 0 && RW.W.Num() == 0)
            {
                MD += TEXT("- **[INFO][ReadOnlyVariable]**\n");
                MD += FString::Printf(TEXT("  - **Description**: 변수 `%s`는 읽히기만 하고 쓰이지 않습니다. 외부에서 세팅되거나 불변 상태라면 OK지만, 의도치 않은 미설정 가능성도 검토하세요.\n"), *Name);
                MD += TEXT("  - **Evidence**: ");
                TArray<FString> Ev = RW.R; TSet<FString> Dd2(Ev); Ev = Dd2.Array(); Ev.Sort();
                for (int i = 0; i < Ev.Num(); ++i) { if (i > 0) MD += TEXT(", "); MD += TEXT("⟦") + Ev[i] + TEXT("⟧"); }
                MD += TEXT("\n\n");
            }
        }
    }

    // 파일 출력
    IFileManager::Get().MakeDirectory(*BPDir, true);
    const FString OutPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bplint.md"), *BP->GetName()));
    BPTextDumpUtils::WriteTextToFile(MD, OutPath);
}


static int32 DumpBlueprintToDir(UBlueprint* BP, const FString& OutRoot)
{
    if (!BP) return 0;

    FBPFlowAnalyzer FlowAnalyzer(BP);
    FlowAnalyzer.Analyze();
    const FBlueprintFlowData& FlowData = FlowAnalyzer.GetResult();

    FBPMetaAnalyzer MetaAnalyzer(BP);
    MetaAnalyzer.Analyze();
    const FBlueprintMetaData& MetaData = MetaAnalyzer.GetResult();

    FBPDefUseAnalyzer DefUseAnalyzer(BP, FlowData);
    DefUseAnalyzer.Analyze();
    const FBlueprintDefUseInfo& DefUse = DefUseAnalyzer.GetResult();

    FBPFactsAnalyzer FactsAnalyzer(BP, FlowData);
    FactsAnalyzer.Analyze();
    const TArray<FBlueprintFact>& Facts = FactsAnalyzer.GetResult();

    FBPLintAnalyzer LintAnalyzer(BP, MetaData, FlowData, DefUse, Facts);
    LintAnalyzer.Analyze();
    const TArray<FBlueprintLintResult>& Lint = LintAnalyzer.GetResult();

    const FString PkgPath = BP->GetOutermost()->GetName();
    const FString PkgDir = FPaths::GetPath(PkgPath);
    const FString BPDir = FPaths::Combine(OutRoot, PkgDir);
    IFileManager::Get().MakeDirectory(*BPDir, true);

    TSharedRef<FJsonObject> MetaJson = MakeShared<FJsonObject>();
    BPTextDumpUtils::AddFrontMatter(MetaJson);
    BPTextDumpUtils::WriteJsonToFile(*MetaJson, FPaths::Combine(BPDir, FString::Printf(TEXT("%s__BP__Meta.bpmeta.json"), *BP->GetName())));

    return 0;
}



// ---------------- module ----------------

// --- 내부 도우미 ---
// 입력 핀에서 VariableGet까지 링크를 따라가 객체 변수명을 추출한다.
// UK2Node_Knot(리루트) 같은 중간 노드는 재귀적으로 관통한다.
static FString GetObjectVarNameFromInputPin(UEdGraphPin* Pin)
{
    if (!Pin) return TEXT("");

    TSet<UEdGraphPin*> Visited;
    TArray<UEdGraphPin*> Stack;

    for (UEdGraphPin* L : Pin->LinkedTo)
        if (L) Stack.Add(L);

    while (!Stack.IsEmpty())
    {
        // ↓ Pop 대체 (deprecated 회피)
        UEdGraphPin* P = Stack.Last();
        Stack.RemoveAt(Stack.Num() - 1, 1, EAllowShrinking::No);

        if (!P || Visited.Contains(P)) continue;
        Visited.Add(P);

        UEdGraphNode* Node = P->GetOwningNode();
        if (!Node) continue;

        if (const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Node))
        {
            const FName Var = Get->GetVarName();
            if (!Var.IsNone()) return Var.ToString();
        }

        if (const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
        {
            for (UEdGraphPin* InPin : Knot->Pins)
            {
                if (!InPin || InPin->Direction != EGPD_Input) continue; // ← 여기
                for (UEdGraphPin* L2 : InPin->LinkedTo)
                    if (L2) Stack.Add(L2);
            }
            continue;
        }

        // 필요 시 다른 노드 유형도 확장 가능
    }

    return TEXT("");
}




// CallFunction의 대상 객체 변수명 추출: 보통 "self" 또는 "Target" 입력핀을 본다.
static FString GetCallTargetObjectVarName(const UK2Node_CallFunction* Call)
{
    if (!Call) return TEXT("");

    UEdGraphPin* SelfPin = nullptr;
    UEdGraphPin* TargetPin = nullptr;

    for (UEdGraphPin* Pin : Call->Pins)
    {
        if (!Pin || Pin->Direction != EGPD_Input) continue;
        const FString N = Pin->PinName.ToString();
        if (N.Equals(TEXT("self"), ESearchCase::IgnoreCase))
        {
            SelfPin = Pin;
        }
        else if (N.Equals(TEXT("Target"), ESearchCase::IgnoreCase))
        {
            TargetPin = Pin;
        }
    }

    if (SelfPin)
    {
        FString V = GetObjectVarNameFromInputPin(SelfPin);
        if (!V.IsEmpty()) return V;
    }
    if (TargetPin)
    {
        FString V = GetObjectVarNameFromInputPin(TargetPin);
        if (!V.IsEmpty()) return V;
    }

    return TEXT("");
}


// 함수명에서 속성명/읽기·쓰기 유형을 추출한다: SetXxx → write "Xxx", GetYyy → read "Yyy"
static bool ExtractPropertyFromFunctionName(const FString& FnName, FString& OutProp, bool& bIsWrite)
{
    if (FnName.StartsWith(TEXT("Set")))
    {
        OutProp = FnName.Mid(3); // SetVisibility -> Visibility
        bIsWrite = true;  return !OutProp.IsEmpty();
    }
    if (FnName.StartsWith(TEXT("Get")))
    {
        OutProp = FnName.Mid(3); // GetText -> Text
        bIsWrite = false; return !OutProp.IsEmpty();
    }
    // 특수 예외(원하면 확장)
    // e.g., "K2_SetRelativeLocation" → RelativeLocation 등도 필요하면 여기서 규칙 추가
    return false;
}


// --- 메인: Def–Use 구축 & 파일 기록 ---
static void BuildDefUseForBP(
    UBlueprint* BP,
    const TMap<UEdGraph*, TArray<UEdGraphNode*>>& SortedByGraph,
    const FString& MetaFilePath,
    const TArray<FString>& FlowJsonPaths,
    const FString& OutRoot)
{
    if (!BP) return;

    TMap<FString, FObjDefUse> Objects;          // objects[Obj].Writes["Prop"] = {"@A..", ...}
    TMap<FString, TSet<FString>> VarWrites;     // vars[Var].writes
    TMap<FString, TSet<FString>> VarReads;      // vars[Var].reads


    // 그래프 순회
    for (const auto& KVP : SortedByGraph)
    {
        for (UEdGraphNode* N : KVP.Value)
        {
            if (!IsValid(N)) continue;
            const FString NodeAnchor = TEXT("@") + BTD::AnchorForNode(N); // ← 딱 1번만

            if (const UK2Node_VariableSet* SetNode = Cast<UK2Node_VariableSet>(N)) {
                const FName Var = SetNode->GetVarName();
                if (!Var.IsNone()) BPTextDumpUtils::AddVarAnchor(VarWrites, Var.ToString(), NodeAnchor);
            }

            if (const UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(N)) {
                const FName Var = GetNode->GetVarName();
                if (!Var.IsNone()) BPTextDumpUtils::AddVarAnchor(VarReads, Var.ToString(), NodeAnchor);
            }

            if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(N)) {
                const UFunction* Fn = Call->GetTargetFunction();
                if (Fn) {
                    FString Prop; bool bWrite = false;
                    if (ExtractPropertyFromFunctionName(Fn->GetName(), Prop, bWrite)) {
                        const FString ObjName = GetCallTargetObjectVarName(Call);
                        if (!ObjName.IsEmpty()) {
                            BPTextDumpUtils::AddObjPropAnchor(Objects, ObjName, Prop, bWrite, NodeAnchor);
                        }
                    }
                }
                // 입력 핀에 물린 GET → 변수 read (소비자 기준 앵커도 NodeAnchor 사용)
                for (UEdGraphPin* In : Call->Pins) {
                    if (!BPTextDumpUtils::IsDataInputPin(In)) continue;
                    for (UEdGraphPin* L : In->LinkedTo) {
                        if (!L || !L->GetOwningNode()) continue;
                        if (const UK2Node_VariableGet* Get2 = Cast<UK2Node_VariableGet>(L->GetOwningNode())) {
                            const FName V = Get2->GetVarName();
                            if (!V.IsNone()) BPTextDumpUtils::AddVarAnchor(VarReads, V.ToString(), NodeAnchor);
                        }
                    }
                }
            }

        }
    }


    // --- JSON 빌드 ---
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    BPTextDumpUtils::AddFrontMatter(Root); // ue_version, plugin_version
    Root->SetStringField(TEXT("bp"), BP->GetName());

    // hashes
    {
        TSharedRef<FJsonObject> H = MakeShared<FJsonObject>();
        H->SetStringField(TEXT("bpmeta"), BTD::FileSHA256(MetaFilePath));
        H->SetStringField(TEXT("bpflow"), BTD::MultiFileSHA256(FlowJsonPaths));
        Root->SetObjectField(TEXT("hashes"), H);
    }

// objects
    {
        TSharedRef<FJsonObject> JObjs = MakeShared<FJsonObject>();
        // Writes
        for (const auto& KObj : Objects)
        {
            TSharedRef<FJsonObject> JOne = MakeShared<FJsonObject>();

            // writes
            {
                TSharedRef<FJsonObject> JWrites = MakeShared<FJsonObject>();
                for (const auto& KW : KObj.Value.Writes)
                {
                    TArray<TSharedPtr<FJsonValue>> Arr;
                    for (const FString& S : KW.Value) Arr.Add(MakeShared<FJsonValueString>(S));
                    JWrites->SetArrayField(KW.Key, Arr);
                }
                JOne->SetObjectField(TEXT("writes"), JWrites);
            }

            // reads
            {
                TSharedRef<FJsonObject> JReads = MakeShared<FJsonObject>();
                for (const auto& KR : KObj.Value.Reads)
                {
                    TArray<TSharedPtr<FJsonValue>> Arr;
                    for (const FString& S : KR.Value) Arr.Add(MakeShared<FJsonValueString>(S));
                    JReads->SetArrayField(KR.Key, Arr);
                }
                JOne->SetObjectField(TEXT("reads"), JReads);
            }

            JObjs->SetObjectField(KObj.Key, JOne);
        }
        Root->SetObjectField(TEXT("objects"), JObjs);
    }


// vars
    {
        TSharedRef<FJsonObject> JVars = MakeShared<FJsonObject>();

        TSet<FString> AllVarNames;
        for (const auto& K : VarWrites) AllVarNames.Add(K.Key);
        for (const auto& K : VarReads)  AllVarNames.Add(K.Key);

        TArray<FString> Names = AllVarNames.Array();
        Names.Sort();

        for (const FString& VName : Names)
        {
            TSharedRef<FJsonObject> JV = MakeShared<FJsonObject>();

            // writes
            {
                TArray<TSharedPtr<FJsonValue>> Arr;
                if (const TSet<FString>* S = VarWrites.Find(VName))
                {
                    for (const FString& A : *S) Arr.Add(MakeShared<FJsonValueString>(A));
                }
                JV->SetArrayField(TEXT("writes"), Arr);
            }

            // reads
            {
                TArray<TSharedPtr<FJsonValue>> Arr;
                if (const TSet<FString>* S = VarReads.Find(VName))
                {
                    for (const FString& A : *S) Arr.Add(MakeShared<FJsonValueString>(A));
                }
                JV->SetArrayField(TEXT("reads"), Arr);
            }

            JVars->SetObjectField(VName, JV);
        }

        Root->SetObjectField(TEXT("vars"), JVars);
    }


    // --- 파일로 저장 ---
    const FString PkgPath = BP->GetOutermost()->GetName();
    const FString PkgDir = FPaths::GetPath(PkgPath);
    const FString BPDir = FPaths::Combine(OutRoot, PkgDir);
    IFileManager::Get().MakeDirectory(*BPDir, true);

    const FString OutPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpdefuse.json"), *BP->GetName()));
    BPTextDumpUtils::WriteJsonToFile(*Root, OutPath);
}


static FString TypeToFriendly(const FEdGraphPinType& T)
{
    const FName& Cat = T.PinCategory;
    if (Cat == UEdGraphSchema_K2::PC_Boolean) return TEXT("bool");
    if (Cat == UEdGraphSchema_K2::PC_Int)     return TEXT("int");
    if (Cat == UEdGraphSchema_K2::PC_Int64)   return TEXT("int64");
    if (Cat == UEdGraphSchema_K2::PC_Real)
        return (T.PinSubCategory == UEdGraphSchema_K2::PC_Double) ? TEXT("double") : TEXT("float");
    if (Cat == UEdGraphSchema_K2::PC_Name)    return TEXT("name");
    if (Cat == UEdGraphSchema_K2::PC_String)  return TEXT("string");
    if (Cat == UEdGraphSchema_K2::PC_Object)  return T.PinSubCategoryObject.IsValid()
        ? T.PinSubCategoryObject->GetName()
        : TEXT("object");
    const FString Sub = T.PinSubCategory.ToString();
    const FString C = Cat.ToString();
    return !Sub.IsEmpty() ? Sub : (!C.IsEmpty() ? C : TEXT("unknown"));
}


static void BuildAndWriteSummaryForBP(
    UBlueprint* BP,
    const TArray<TSharedPtr<FJsonValue>>& Slices,
    const FString& MetaFilePath,
    const TArray<FString>& FlowJsonPaths,
    const FString& OutRoot)
{
    if (!BP) return;
    const FString UEVer = FEngineVersion::Current().ToString();
    const FString MetaHash = BTD::FileSHA256(MetaFilePath);
    const FString FlowHash = BTD::MultiFileSHA256(FlowJsonPaths);
    const FString ParentPath = (BP->ParentClass) ? BP->ParentClass->GetPathName() : TEXT("");
    const FString Timestamp = FDateTime::Now().ToIso8601();

    // Derive file paths
    const FString PkgPath = BP->GetOutermost()->GetName();
    const FString PkgDir = FPaths::GetPath(PkgPath);
    const FString BPDir = FPaths::Combine(OutRoot, PkgDir);
    const FString CatalogPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpcatalog.json"), *BP->GetName()));
    const FString DefUsePath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpdefuse.json"), *BP->GetName()));

    // Load previously generated analysis
    TSharedPtr<FJsonObject> JMeta = BPTextDumpUtils::LoadJsonObject(MetaFilePath);
    TSharedPtr<FJsonObject> JCatalog = BPTextDumpUtils::LoadJsonObject(CatalogPath);
    TSharedPtr<FJsonObject> JDefUse = BPTextDumpUtils::LoadJsonObject(DefUsePath);

    // Entry points from catalog slices
    TArray<FString> EntryIds; BPTextDumpUtils::ExtractEntryPointsFromCatalog(JCatalog, EntryIds);

    // Public API from per-graph flow jsons
    TArray<FPublicApiInfo> PublicApis;
    BPTextDumpUtils::ExtractPublicApisFromFlows(FlowJsonPaths, PublicApis);

    // Top variables (by reads+writes) with friendly types from meta
    TArray<FVarMeta> TopVars; BPTextDumpUtils::ExtractVarUsage(JMeta, JDefUse, TopVars);

    // Dependencies (object var names) from defuse objects
    TArray<FString> DependsOn; BPTextDumpUtils::ExtractDependencies(JDefUse, DependsOn);

    // Role heuristic
    FString Role;
    if (ParentPath.Contains(TEXT("/Script/UMG.UserWidget")))
        Role = TEXT("타이머/상태/목록 등을 표시하는 **사용자 위젯(User Widget)**.");
    else if (ParentPath.Contains(TEXT("Actor")))
        Role = TEXT("게임플레이 타이머/상태/저장을 다루는 **액터(Actor)**.");
    else
        Role = TEXT("게임플레이 로직을 포함한 **블루프린트 클래스**.");

    // Build markdown
    FString MD;
    MD += TEXT("---\n");
    MD += FString::Printf(TEXT("bp: %s\n"), *BP->GetName());
    MD += FString::Printf(TEXT("parent: %s\n"), *ParentPath);
    MD += FString::Printf(TEXT("ue_version: %s\n"), *UEVer);
    MD += FString::Printf(TEXT("hashes: { bpmeta: %s, bpflow: %s }\n"), *MetaHash, *FlowHash);
    MD += FString::Printf(TEXT("generated_at: %s\n"), *Timestamp);
    MD += TEXT("---\n\n");

    MD += TEXT("## Role\n");
    MD += Role + TEXT("\n\n");

    MD += TEXT("## Entry Points\n");
    if (EntryIds.Num() == 0)
    {
        MD += TEXT("- (none)\n\n");
    }
    else
    {
        for (const FString& Id : EntryIds)
        {
            const FString Label = BPTextDumpUtils::NormalizeEventIdForLabel(Id);
            MD += FString::Printf(TEXT("- **%s**: (see: `%s`)\n"), *Label, *Id);
        }
        MD += TEXT("\n");
    }

    MD += TEXT("## Public API (Callable)\n");
    if (PublicApis.Num() == 0)
    {
        MD += TEXT("- (none)\n\n");
    }
    else
    {
        for (const FPublicApiInfo& A : PublicApis)
        {
            MD += TEXT("- **") + A.Name + TEXT("**\n");
        }
        MD += TEXT("\n");
    }

    MD += TEXT("## State & Vars (Top 3)\n");
    if (TopVars.Num() == 0)
    {
        MD += TEXT("- (none)\n\n");
    }
    else
    {
        for (const FVarMeta& V : TopVars)
        {
            const FString Ty = BPTextDumpUtils::FriendlyFromCategory(V.Cat, V.Sub);
            MD += FString::Printf(TEXT("- `%s` (%s)\n"), *V.Name, *Ty);
        }
        MD += TEXT("\n");
    }

    MD += TEXT("## External I/O\n");
    if (DependsOn.Num() > 0)
    {
        MD += TEXT("- **Depends On**: ");
        for (int i = 0; i < DependsOn.Num(); ++i)
        {
            if (i > 0) MD += TEXT(", ");
            MD += DependsOn[i];
        }
        MD += TEXT("\n");
    }
    else
    {
        MD += TEXT("- (none detected)\n");
    }
    MD += TEXT("\n");

    MD += TEXT("## Hotspots\n");
    MD += TEXT("- (auto) lint 단계에서 추가 예정\n\n");

    MD += TEXT("## Jump Table\n");
    // Prefer eventgraph entries first
    for (const FString& Id : EntryIds) MD += TEXT("- ") + Id + TEXT("\n");
    // Then add function graph flows (their slices exist as \"flow.<slug>\")
    for (const FPublicApiInfo& A : PublicApis)
    {
        const FString Slugged = BPTextDumpUtils::Slug(A.Name);
        MD += TEXT("- flow.") + Slugged + TEXT("\n");
    }

    IFileManager::Get().MakeDirectory(*BPDir, true);
    const FString OutPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpsmry.md"), *BP->GetName()));
    BPTextDumpUtils::WriteTextToFile(MD, OutPath);

}

void FBPTextDumpModule::StartupModule()
{
    IConsoleManager& CM = IConsoleManager::Get();

    DumpAllCmd = CM.RegisterConsoleCommand(
        TEXT("BP.DumpAll"),
        TEXT("Dump Blueprint graphs. Optional: Root=/Game/Subfolder Out=C:/path"),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FBPTextDumpModule::CmdDumpAll),
        ECVF_Cheat
    );

    DumpSelCmd = CM.RegisterConsoleCommand(
        TEXT("BP.DumpSelected"),
        TEXT("Dump selected Blueprints from the Content Browser. Optional: Out=C:/path"),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FBPTextDumpModule::CmdDumpSelected),
        ECVF_Cheat
    );

    DumpOneCmd = CM.RegisterConsoleCommand(
        TEXT("BP.DumpOne"),
        TEXT("Dump a single Blueprint. Args: Path=/Game/Folder/Asset[.Asset] Out=C:/path"),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FBPTextDumpModule::CmdDumpOne),
        ECVF_Cheat
    );
    ProjectRefsCmd = CM.RegisterConsoleCommand(
        TEXT("BP.ProjectRefs"),
        TEXT("Build project-wide reference graph. Optional: Root=/Game Sub=/Game/UI Out=C:/path"),
        FConsoleCommandWithArgsDelegate::CreateRaw(this, &FBPTextDumpModule::CmdProjectRefs),
        ECVF_Cheat
         );

    RegisterMenus();
}


void FBPTextDumpModule::ShutdownModule()
{
    if (DumpAllCmd)
    {
        IConsoleManager::Get().UnregisterConsoleObject(DumpAllCmd);
        DumpAllCmd = nullptr;
    }
    if (DumpSelCmd)
    {
        IConsoleManager::Get().UnregisterConsoleObject(DumpSelCmd);
        DumpSelCmd = nullptr;
    }
    if (DumpOneCmd)
    {
        IConsoleManager::Get().UnregisterConsoleObject(DumpOneCmd);
        DumpOneCmd = nullptr;
    }
    if (ProjectRefsCmd)
        {
        IConsoleManager::Get().UnregisterConsoleObject(ProjectRefsCmd);
        ProjectRefsCmd = nullptr;
        }
    UnregisterMenus();
}

void FBPTextDumpModule::CmdDumpAll(const TArray<FString>& Args)
{
    FString RootPath = TEXT("/Game");
    FString OutRoot = DefaultOutDir();

    for (const FString& A : Args)
    {
        if (A.StartsWith(TEXT("Root="))) RootPath = A.RightChop(5);
        else if (A.StartsWith(TEXT("Out="))) OutRoot = A.RightChop(4);
    }

    IFileManager::Get().MakeDirectory(*OutRoot, /*Tree*/ true);

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter;
    Filter.PackagePaths.Add(*RootPath);
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true; // include derived Blueprints (Widget/Anim/etc.)

    TArray<FAssetData> Assets;
    ARM.Get().GetAssets(Filter, Assets);

    UE_LOG(LogTemp, Display, TEXT("BPTextDump: Found %d Blueprint assets under %s"), Assets.Num(), *RootPath);

    int32 DumpedGraphs = 0;
    int32 DumpedAssets = 0;
    for (const FAssetData& AD : Assets)
    {
        UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
        if (!BP) continue;
        const int32 N = DumpBlueprintToDir(BP, OutRoot);
        if (N > 0) { ++DumpedAssets; DumpedGraphs += N; }
    }

    UE_LOG(LogTemp, Display, TEXT("BPTextDump: Wrote %d graph files from %d assets to %s"), DumpedGraphs, DumpedAssets, *OutRoot);
}

void FBPTextDumpModule::CmdDumpSelected(const TArray<FString>& Args)
{
    FString OutRoot = DefaultOutDir();
    TArray<FString> RootArgs; // 선택이 없을 때 스캔할 루트 경로들

    for (const FString& A : Args)
    {
        if (A.StartsWith(TEXT("Out=")))   OutRoot = A.RightChop(4);
        else if (A.StartsWith(TEXT("Root=")))
        {
            // Root=/Game/Foo or Root=/Game/Foo,/Game/Bar
            const FString Raw = A.RightChop(5);
            Raw.ParseIntoArray(RootArgs, TEXT(","), true);
        }
    }
    IFileManager::Get().MakeDirectory(*OutRoot, true);

    FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

    TArray<FAssetData> SelectedAssets;
    CB.Get().GetSelectedAssets(SelectedAssets);

    TArray<FString> SelectedFolders;
    CB.Get().GetSelectedFolders(SelectedFolders);

    UE_LOG(LogTemp, Display, TEXT("ContentBrowser: %d selected assets, %d selected folders"),
        SelectedAssets.Num(), SelectedFolders.Num());

    // 우선 선택 자산을 처리
    int32 AssetCount = 0, GraphCount = 0;

    auto ProcessAsset = [&](const FAssetData& AD)
        {
            bool bBP = false;
            if (UClass* C = AD.GetClass()) bBP = C->IsChildOf(UBlueprint::StaticClass());
            else                           bBP = AD.AssetClassPath.ToString().EndsWith(TEXT("Blueprint"));
            if (!bBP) return;

            if (UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset()))
            {
                ++AssetCount;
                GraphCount += DumpBlueprintToDir(BP, OutRoot);
            }
        };

    for (const FAssetData& AD : SelectedAssets) ProcessAsset(AD);

    // 선택 폴더 fallback
    if (AssetCount == 0 && SelectedFolders.Num() > 0)
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        for (const FString& P : SelectedFolders) Filter.PackagePaths.Add(*P);

        TArray<FAssetData> Assets; ARM.Get().GetAssets(Filter, Assets);
        UE_LOG(LogTemp, Display, TEXT("Fallback by folders: found %d Blueprint assets"), Assets.Num());
        for (const FAssetData& AD : Assets) ProcessAsset(AD);
    }

    // 선택이 전혀 없거나 폴더에도 없으면 → Root 인자 사용
    if (AssetCount == 0 && RootArgs.Num() > 0)
    {
        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        for (const FString& R : RootArgs) Filter.PackagePaths.Add(*R);

        TArray<FAssetData> Assets; ARM.Get().GetAssets(Filter, Assets);
        UE_LOG(LogTemp, Display, TEXT("Fallback by Root args: found %d Blueprint assets"), Assets.Num());
        for (const FAssetData& AD : Assets) ProcessAsset(AD);
    }

    // 그래도 0이면 → 설정 기본 루트 또는 /Game 스캔
    if (AssetCount == 0)
    {
        FString DefaultRoot = TEXT("/Game");
        if (const UBPTextDumpSettings* S = GetDefault<UBPTextDumpSettings>())
            if (!S->DefaultRootPath.IsEmpty()) DefaultRoot = S->DefaultRootPath;

        FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        FARFilter Filter;
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(*DefaultRoot);

        TArray<FAssetData> Assets; ARM.Get().GetAssets(Filter, Assets);
        UE_LOG(LogTemp, Display, TEXT("Fallback by default root (%s): found %d Blueprint assets"),
            *DefaultRoot, Assets.Num());
        for (const FAssetData& AD : Assets) ProcessAsset(AD);
    }

    UE_LOG(LogTemp, Display, TEXT("Selected %d BPs, wrote %d graph files to %s"),
        AssetCount, GraphCount, *OutRoot);
}




void FBPTextDumpModule::CmdDumpOne(const TArray<FString>& Args)
{
    FString ObjPath; // /Game/Foo/Bar.Bar or /Game/Foo/Bar
    FString OutRoot = DefaultOutDir();
    for (const FString& A : Args)
    {
        if (A.StartsWith(TEXT("Path="))) ObjPath = A.RightChop(5);
        else if (A.StartsWith(TEXT("Out="))) OutRoot = A.RightChop(4);
    }
    if (ObjPath.IsEmpty()) { UE_LOG(LogTemp, Warning, TEXT("BP.DumpOne: Missing Path=/Game/...")); return; }
    if (!ObjPath.Contains(TEXT("."))) { const FString Name = FPaths::GetCleanFilename(ObjPath); ObjPath += TEXT(".") + Name; }

    IFileManager::Get().MakeDirectory(*OutRoot, true);
    UObject* Loaded = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *ObjPath);
    UBlueprint* BP = Cast<UBlueprint>(Loaded);
    if (!BP) { UE_LOG(LogTemp, Error, TEXT("BP.DumpOne: Failed to load %s"), *ObjPath); return; }

    const int32 N = DumpBlueprintToDir(BP, OutRoot);
    UE_LOG(LogTemp, Display, TEXT("BPTextDump: Wrote %d graph files for %s to %s"), N, *ObjPath, *OutRoot);
}

// 편의: UI 액션들
void FBPTextDumpModule::UI_DumpAll()
{
    FString RootPath = TEXT("/Game");
    FString OutRoot = DefaultOutDir();
    if (const UBPTextDumpSettings* S = GetDefault<UBPTextDumpSettings>())
    {
        if (!S->DefaultRootPath.IsEmpty()) RootPath = S->DefaultRootPath;
        if (!S->DefaultOutDir.IsEmpty())   OutRoot = S->DefaultOutDir;
    }

    // CmdDumpAll과 동일 동작
    TArray<FString> Args;
    Args.Add(TEXT("Root=") + RootPath);
    Args.Add(TEXT("Out=") + OutRoot);
    CmdDumpAll(Args);
}

void FBPTextDumpModule::UI_DumpSelected()
{
    FString OutRoot = DefaultOutDir();
    if (const UBPTextDumpSettings* S = GetDefault<UBPTextDumpSettings>())
        if (!S->DefaultOutDir.IsEmpty()) OutRoot = S->DefaultOutDir;

    IFileManager::Get().MakeDirectory(*OutRoot, true);

    // Content Browser 선택 재사용
    FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    TArray<FAssetData> Selected; CB.Get().GetSelectedAssets(Selected);

    int32 AssetCount = 0, GraphCount = 0;
    for (const FAssetData& AD : Selected)
    {
        if (UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset()))
        {
            ++AssetCount;
            GraphCount += DumpBlueprintToDir(BP, OutRoot);
        }
    }
    UE_LOG(LogTemp, Display, TEXT("BPTextDump(UI): Selected %d BPs, wrote %d graph files to %s"), AssetCount, GraphCount, *OutRoot);
}

void FBPTextDumpModule::RegisterMenus()
{
    UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FBPTextDumpModule::RegisterMenus_Impl));
}

void FBPTextDumpModule::RegisterMenus_Impl()
{
    UToolMenus* TM = UToolMenus::Get();

    if (UToolMenu* ToolsMenu = TM->ExtendMenu("LevelEditor.MainMenu.Tools"))
    {
        FToolMenuSection& Sec = ToolsMenu->AddSection("BPTextDump", NSLOCTEXT("BPTextDump", "Section", "BP Text Dump"));
        Sec.AddMenuEntry(
            "BPTextDump_DumpAll",
            NSLOCTEXT("BPTextDump", "DumpAll", "Dump All (Default Root)"),
            NSLOCTEXT("BPTextDump", "DumpAll_TT", "Dump all Blueprints under DefaultRootPath."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateRaw(this, &FBPTextDumpModule::UI_DumpAll))
        );
        Sec.AddMenuEntry(
            "BPTextDump_ProjectRefs",
            NSLOCTEXT("BPTextDump", "ProjectRefs", "Build Project References"),
            NSLOCTEXT("BPTextDump", "ProjectRefs_TT", "Scan all Blueprints and build project_references.json."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateRaw(this, &FBPTextDumpModule::UI_BuildProjectRefs))
             );
        Sec.AddMenuEntry(
            "BPTextDump_DumpSelected",
            NSLOCTEXT("BPTextDump", "DumpSelected", "Dump Selected Blueprints"),
            NSLOCTEXT("BPTextDump", "DumpSelected_TT", "Dump only the Content Browser selection."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateRaw(this, &FBPTextDumpModule::UI_DumpSelected))
        );
    }

    if (UToolMenu* CBMenu = TM->ExtendMenu("ContentBrowser.AssetContextMenu"))
    {
        FToolMenuSection& Sec = CBMenu->AddSection(
            "BPTextDump_Context",
            NSLOCTEXT("BPTextDump", "CtxSection", "BP Text Dump")
        );

        Sec.AddMenuEntry(
            "BPTextDump_DumpSelected_Context",
            NSLOCTEXT("BPTextDump", "DumpSelectedCtx", "Dump Blueprint Graphs"),
            NSLOCTEXT("BPTextDump", "DumpSelectedCtx_TT", "Export graphs of selected Blueprints to JSON/DSL."),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateRaw(this, &FBPTextDumpModule::UI_DumpSelected),
                FCanExecuteAction::CreateLambda([]() {
                    FContentBrowserModule& CB = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
                    TArray<FAssetData> Sel; CB.Get().GetSelectedAssets(Sel);
                    for (const FAssetData& AD : Sel)
                    {
                        if (UClass* AssetClass = AD.GetClass())
                        {
                            if (AssetClass->IsChildOf(UBlueprint::StaticClass()))
                                return true;
                        }
                        else
                        {
                            const FString ClassPathStr = AD.AssetClassPath.ToString();
                            if (ClassPathStr.EndsWith(TEXT("Blueprint")))
                                return true;
                        }
                    }
                    return false;
                    })
            ),
            EUserInterfaceActionType::Button
        );
    }
}

// ============================================================
// Project-Wide Reference Analysis — command handlers
// ============================================================
void FBPTextDumpModule::UI_BuildProjectRefs()
{
    FString RootPath = TEXT("/Game");
    FString OutRoot = DefaultOutDir();
    if (const UBPTextDumpSettings* S = GetDefault<UBPTextDumpSettings>())
    {
        if (!S->DefaultRootPath.IsEmpty()) RootPath = S->DefaultRootPath;
        if (!S->DefaultOutDir.IsEmpty())   OutRoot = S->DefaultOutDir;
    }
    TArray<FString> Args; Args.Add(TEXT("Root=") + RootPath); Args.Add(TEXT("Out=") + OutRoot);
    CmdProjectRefs(Args);
}

void FBPTextDumpModule::CmdProjectRefs(const TArray<FString>& Args)
{
    TArray<FString> Roots; Roots.Add(TEXT("/Game"));
    FString OutRoot = DefaultOutDir();
    for (const FString& A : Args)
    {
        if (A.StartsWith(TEXT("Root="))) { Roots.Reset(); A.Mid(5).ParseIntoArray(Roots, TEXT(","), true); }
        else if (A.StartsWith(TEXT("Out="))) OutRoot = A.Mid(4);
    }
    IFileManager::Get().MakeDirectory(*OutRoot, true);

    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter; Filter.bRecursivePaths = true; Filter.bRecursiveClasses = true;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    for (const FString& R : Roots) Filter.PackagePaths.Add(*R);
    TArray<FAssetData> Assets; ARM.Get().GetAssets(Filter, Assets);

    FProjectAnalyzer Analyzer(Assets);
    Analyzer.Analyze();
    const TSharedPtr<FJsonObject>& Result = Analyzer.GetResult();
    if (Result.IsValid())
    {
        const FString OutPath = FPaths::Combine(OutRoot, TEXT("project_references.json"));
        BPTextDumpUtils::WriteJsonToFile(*Result, OutPath);
    }
}



void FBPTextDumpModule::UnregisterMenus()
{
    if (!UObjectInitialized())
    {
        return;
    }
    UToolMenus::UnregisterOwner(this);
}

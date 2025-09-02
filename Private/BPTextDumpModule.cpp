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



//#include "Kismet2/BlueprintEditorUtils.h" // optional (for extra helpers)

IMPLEMENT_MODULE(FBPTextDumpModule, BPTextDump);

// 기존 파일 안의 정적 헬퍼들을 앞에서 참조할 수 있도록 프로토타입 추가
static bool WriteJsonToFile(const FJsonObject& Root, const FString& OutPath);
static bool WriteTextToFile(const FString& Text, const FString& OutPath);
static void AddFrontMatter(TSharedRef<FJsonObject> J);

// 우리가 쓰는 슬라이스 빌더 프로토타입
static void BuildSlicesForGraph(UEdGraph* Graph,
    const TArray<UEdGraphNode*>& SortedNodes,
    TArray<TSharedPtr<FJsonValue>>& Out);

static void BuildAndWriteSummaryForBP(
    UBlueprint* BP,
    const TArray<TSharedPtr<FJsonValue>>& Slices,
    const FString& MetaFilePath,
    const TArray<FString>& FlowJsonPaths,
    const FString& OutRoot);

static bool IsDataInputPin(const UEdGraphPin* P);
static FString GetCallTargetObjectVarName(const UK2Node_CallFunction * Call);
static FString GetObjectVarNameFromInputPin(UEdGraphPin* Pin);
static UEdGraphPin * FindInputPinByName(UEdGraphNode * N, const TCHAR * NameA, const TCHAR * NameB);

static TSharedPtr<FJsonObject> LoadJsonObject(const FString & Path)
 {
    FString JsonStr;
    if (!FFileHelper::LoadFileToString(JsonStr, *Path)) return nullptr;
    TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(JsonStr);
    TSharedPtr<FJsonObject> Obj;
    FJsonSerializer::Deserialize(R, Obj);
    return Obj;
 }

// ============================================
// Project-Wide Reference Analysis: helpers
// ============================================
static FString ToAssetKeyFromClassPath(const FString& ClassPath)
{
    // "/Game/Path/Asset.Asset_C" -> "/Game/Path/Asset"
    // "/Script/Engine.Actor"     -> 그대로 유지
    if (ClassPath.StartsWith(TEXT("/Game/")))
    {
        int32 DotIdx = INDEX_NONE;
        if (ClassPath.FindChar(TCHAR('.'), DotIdx))
            return ClassPath.Left(DotIdx);
        return ClassPath;
    }
    if (ClassPath.StartsWith(TEXT("/Script/")))
    {
        return ClassPath;
    }
    return TEXT("");
}

static FString ToAssetKeyFromObjectPath(const FString& ObjPath)
{
    // UObject 경로는 보통 "/Game/A/B.Asset_C" 또는 "/Script/Module.Class"
    return ToAssetKeyFromClassPath(ObjPath);
}

static void AddRef(TMap<FString, TSet<FString>>& G, const FString& From, const FString& To)
{
    if (From.IsEmpty() || To.IsEmpty() || From == To) return;
    G.FindOrAdd(From).Add(To);
}

static void ReadNDJSONLines(const FString& Path, TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Fn)
{
    FString Body;
    if (!FFileHelper::LoadFileToString(Body, *Path)) return;
    TArray<FString> Lines;
    Body.ParseIntoArrayLines(Lines);
    for (const FString& L : Lines)
    {
        if (L.TrimStartAndEnd().IsEmpty()) continue;
        TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(L);
        TSharedPtr<FJsonObject> O;
        if (FJsonSerializer::Deserialize(R, O) && O.IsValid())
            Fn(O);
    }
}


// ============================================================
// Phase 6.5 — Source & Origin Analysis helpers
// ============================================================
static FString GetSelfClassPath(UBlueprint* BP)
{
    if (!BP) return TEXT("");
    if (BP->GeneratedClass) return BP->GeneratedClass->GetPathName();
    if (BP->SkeletonGeneratedClass) return BP->SkeletonGeneratedClass->GetPathName();
    // fallback: asset path (덜 정확함)
    return BP->GetPathName();
}

static FString ResolveVarDefinedIn(UBlueprint* BP, const FName VarName)
{
    if (!BP || VarName.IsNone()) return TEXT("");
    // 부모 체인을 따라 올라가며 같은 이름의 FProperty가 존재하면 그 클래스 경로를 반환
    for (UClass* C = BP->ParentClass; C; C = C->GetSuperClass())
    {
        if (FindFProperty<FProperty>(C, VarName))
        {
            return C->GetPathName();
        }
    }
    // 없으면 현재 BP가 원 정의
    return GetSelfClassPath(BP);
}

static FString ResolveFunctionDefinedIn(UBlueprint* BP, const FName FuncName)
{
    if (!BP || FuncName.IsNone()) return TEXT("");
    // 1) 인터페이스에 동일 시그니처 함수가 있으면 인터페이스 경로
    for (const FBPInterfaceDescription& D : BP->ImplementedInterfaces)
    {
        if (D.Interface && D.Interface->FindFunctionByName(FuncName))
        {
            return D.Interface->GetPathName();
        }
    }
    // 2) 부모 체인에서 찾히면 그 클래스 경로
    for (UClass* C = BP->ParentClass; C; C = C->GetSuperClass())
    {
        if (C->FindFunctionByName(FuncName))
        {
            return C->GetPathName();
        }
    }
    // 3) 그 외는 본인
    return GetSelfClassPath(BP);
}


// ---------------- helpers ----------------
static FString JsonLine_Fact(const FString& S, const FString& P, const FString& O, const TArray<FString>& Ev)
{
    // Try to emit numeric JSON for 'o' when it looks like a number
    auto LooksNumeric = [](const FString& X)->bool {
        if (X.IsEmpty()) return false;
        bool bDot = false, bDigit = false;
        for (int32 i = 0; i < X.Len(); ++i) {
            TCHAR c = X[i];
            if (i == 0 && (c == TCHAR('-') || c == TCHAR('+'))) continue;
            if (c == TCHAR('.')) { if (bDot) return false; bDot = true; continue; }
            if (c >= '0' && c <= '9') { bDigit = true; continue; }
            return false;
        }
        return bDigit;
        };
    TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("s"), S);
    Obj->SetStringField(TEXT("p"), P);
    if (LooksNumeric(O))
    {
        Obj->SetNumberField(TEXT("o"), FCString::Atod(*O));
    }
    else
    {
        Obj->SetStringField(TEXT("o"), O);
    }
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& A : Ev) Arr.Add(MakeShared<FJsonValueString>(A));
        Obj->SetArrayField(TEXT("ev"), Arr);
    }
    FString Out;
    auto W = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    FJsonSerializer::Serialize(Obj, W);
    return Out;
}

static FString PinDefaultOrText(const UEdGraphPin* P, const TCHAR* NameA = nullptr, const TCHAR* NameB = nullptr)
{
    if (!P) return TEXT("");
    if (NameA || NameB)
    {
        const FString N = P->PinName.ToString();
        if (NameA && N.Equals(NameA, ESearchCase::IgnoreCase)) { /* ok */ }
        else if (NameB && N.Equals(NameB, ESearchCase::IgnoreCase)) { /* ok */ }
        else return TEXT("");
    }
    if (!P->DefaultTextValue.IsEmpty()) return P->DefaultTextValue.ToString();
    if (!P->DefaultValue.IsEmpty())     return P->DefaultValue;
    return TEXT("");
}

static UEdGraphPin* FindInputPinByName(UEdGraphNode* N, const TCHAR* NameA, const TCHAR* NameB = nullptr)
{
    if (!N) return nullptr;
    for (UEdGraphPin* P : N->Pins)
    {
        if (!P || P->Direction != EGPD_Input) continue;
        const FString Nm = P->PinName.ToString();
        if (Nm.Equals(NameA, ESearchCase::IgnoreCase) || (NameB && Nm.Equals(NameB, ESearchCase::IgnoreCase)))
            return P;
    }
    return nullptr;
}

static void BuildFactsForBP(
    UBlueprint* BP,
    const TMap<UEdGraph*, TArray<UEdGraphNode*>>& SortedByGraph,
    const FString& OutRoot)
{
    if (!BP) return;
    TArray<FString> Lines;

    auto Emit = [&](const FString& S, const FString& P, const FString& O, TArray<FString> Ev)
        {
            {
                TSet<FString> Dedup(Ev);
                Ev = Dedup.Array();
                Ev.Sort();
            }
             Lines.Add(JsonLine_Fact(S, P, O, Ev));
        };

    const FString SelfBP = BP->GetName();

    for (const auto& KVP : SortedByGraph)
    {
        const TArray<UEdGraphNode*>& Nodes = KVP.Value;
        // Precompute node anchors
        TMap<const UEdGraphNode*, FString> AKey;
        for (UEdGraphNode* N : Nodes)
            if (IsValid(N)) AKey.Add(N, TEXT("@") + BTD::AnchorForNode(N));

        for (UEdGraphNode* N : Nodes)
        {
            if (!IsValid(N)) continue;
            const FString A = AKey[N];

            // 1) CallFunction-based heuristics
            if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(N))
            {
                const UFunction* Fn = Call->GetTargetFunction();
                const FString FnName = Fn ? Fn->GetName() : TEXT("");
                const FString OwnerPath = Fn && Fn->GetOwnerClass() ? Fn->GetOwnerClass()->GetPathName() : TEXT("");
                const FString OwnerSimple = Fn && Fn->GetOwnerClass() ? Fn->GetOwnerClass()->GetName() : TEXT("UnknownClass");
                const FString QualifiedFn = OwnerSimple + TEXT(".") + FnName;
                
                                    // Subject: 실제 Target/Self 변수명 우선, 없으면 BP 이름
                    FString Subject = GetCallTargetObjectVarName(Call);
                if (Subject.IsEmpty()) Subject = SelfBP;

                    bool bEmittedSpecific = false;
                
                    if (FnName.Contains(TEXT("SetText")))
                     {
                    UEdGraphPin * TextPin = FindInputPinByName(const_cast<UK2Node_CallFunction*>(Call), TEXT("InText"), TEXT("Text"));
                    FString Val = PinDefaultOrText(TextPin);
                    if (!Val.IsEmpty())
                         {
                        Emit(Subject, TEXT("text_set_to"), Val, { A });
                        }
                     else
                         {
                        Emit(Subject, TEXT("text_set_via"), QualifiedFn, { A });
                        }
                     bEmittedSpecific = true;
                    }

                // 1.a) visibility_set_to (UMG)
                if (FnName.Contains(TEXT("SetVisibility")))
                {
                    UEdGraphPin* VisPin = FindInputPinByName(const_cast<UK2Node_CallFunction*>(Call), TEXT("InVisibility"), TEXT("In Visibility"));
                    FString VisVal;
                    if (VisPin)
                    {
                        if (!VisPin->LinkedTo.Num())
                        {
                            VisVal = PinDefaultOrText(VisPin);
                        }
                        else
                        {
                            // fallback: use linked node title
                            if (UEdGraphPin* L = VisPin->LinkedTo[0])
                                if (UEdGraphNode* LN = L->GetOwningNode())
                                    VisVal = LN->GetNodeTitle(ENodeTitleType::ListView).ToString();
                        }
                    }
                    if (VisVal.IsEmpty()) VisVal = TEXT("Unknown");
                    Emit(Subject, TEXT("visibility_set_to"), VisVal, { A });
                    bEmittedSpecific = true;
                }

                if (FnName.Contains(TEXT("SetIsEnabled")))
                {
                    UEdGraphPin* PinE = FindInputPinByName(const_cast<UK2Node_CallFunction*>(Call), TEXT("bInIsEnabled"), TEXT("In Is Enabled"));
                    FString V = PinDefaultOrText(PinE);
                    if (V.IsEmpty() && PinE && PinE->LinkedTo.Num() == 0) V = TEXT("false");
                    if (!V.IsEmpty()) Emit(Subject, TEXT("enabled_set_to"), V, { A });
                    bEmittedSpecific = true;
                }

                // 1.d) adds_child_widget (PanelWidget::AddChild 계열)
                if (FnName.StartsWith(TEXT("AddChild")))
                {
                    // child 후보 핀 이름들
                    UEdGraphPin * ChildPin = FindInputPinByName(const_cast<UK2Node_CallFunction*>(Call), TEXT("Content"));
                    if (!ChildPin) ChildPin = FindInputPinByName(const_cast<UK2Node_CallFunction*>(Call), TEXT("Child"));
                    if (!ChildPin) ChildPin = FindInputPinByName(const_cast<UK2Node_CallFunction*>(Call), TEXT("InContent"));
                    FString ChildName;
                    if (ChildPin)
                    {
                        // 변수 추적해서 이름을 얻을 수 있으면 사용
                        FString V = GetObjectVarNameFromInputPin(ChildPin);
                        if (!V.IsEmpty()) ChildName = V;
                    }
                    if (ChildName.IsEmpty()) ChildName = TEXT("Widget");
                    Emit(Subject, TEXT("adds_child_widget"), ChildName, { A });
                    bEmittedSpecific = true;
                }

                // 1.e) clears_child_widgets
                if (FnName.Contains(TEXT("ClearChildren")))
                {
                    Emit(Subject, TEXT("clears_child_widgets"), TEXT("all"), { A });
                    bEmittedSpecific = true;
                }


                // 1.b) comparison: is_compared_to
                auto IsCmp = [](const FString& Name)->bool {
                    return Name.StartsWith(TEXT("EqualEqual")) || Name.StartsWith(TEXT("Greater")) || Name.StartsWith(TEXT("Less"));
                    };
                if (IsCmp(FnName))
                {
                    // Find a variable on one input and a constant on the other
                    FString VarName, ConstStr;
                    for (UEdGraphPin* In : N->Pins)
                    {
                        if (!IsDataInputPin(In)) continue;
                        if (In->LinkedTo.Num() == 0)
                        {
                            const FString D = PinDefaultOrText(In);
                            if (!D.IsEmpty()) ConstStr = D;
                        }
                        else
                        {
                            for (UEdGraphPin* L : In->LinkedTo)
                            {
                                if (!L || !L->GetOwningNode()) continue;
                                if (const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(L->GetOwningNode()))
                                {
                                    const FName V = Get->GetVarName();
                                    if (!V.IsNone()) VarName = V.ToString();
                                }
                            }
                        }
                    }
                    if (!VarName.IsEmpty() && !ConstStr.IsEmpty())
                    {
                        Emit(VarName, TEXT("is_compared_to"), ConstStr, { A });
                    }
                    bEmittedSpecific = true;
                }

                // 1.c) depends_on: external owner functions
                if (!bEmittedSpecific && !OwnerPath.IsEmpty())
                {
                    // exclude self-context call (best-effort)
                    const bool bSelf = Call->FunctionReference.IsSelfContext();
                    if (!bSelf)
                    {
                        Emit(Subject, TEXT("depends_on"), QualifiedFn, { A });
                    }
                }
            }

            // 2) VariableSet: is_acquired_via
            if (const UK2Node_VariableSet* Set = Cast<UK2Node_VariableSet>(N))
            {
                const FName TargetVar = Set->GetVarName();
                FString VarName = TargetVar.IsNone() ? TEXT("") : TargetVar.ToString();
                if (!VarName.IsEmpty())
                {
                    // Look at input data pins to detect common acquisition patterns
                    for (UEdGraphPin* P : N->Pins)
                    {
                        if (!IsDataInputPin(P)) continue;
                        for (UEdGraphPin* L : P->LinkedTo)
                        {
                            if (!L || !L->GetOwningNode()) continue;
                            if (const UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(L->GetOwningNode()))
                            {
                                const UFunction* F = CF->GetTargetFunction();
                                const FString FnName = F ? F->GetName() : TEXT("");
                                if (FnName.Contains(TEXT("GetAllActorsOfClass")))
                                {
                                    FString How = TEXT("GetAllActorsOfClass");
                                    // Check for downstream Array Get index 0 pattern
                                    UEdGraphPin* OutP = nullptr;
                                    for (UEdGraphPin* pp : CF->Pins)
                                        if (pp && pp->Direction == EGPD_Output) { OutP = pp; break; }
                                    if (OutP)
                                    {
                                        for (UEdGraphPin* L2 : OutP->LinkedTo)
                                        {
                                            if (!L2 || !L2->GetOwningNode()) continue;
                                            if (L2->GetOwningNode()->GetClass()->GetName().Contains(TEXT("K2Node_CallFunction")))
                                            {
                                                UK2Node_CallFunction* CF2 = Cast<UK2Node_CallFunction>(L2->GetOwningNode());
                                                if (UFunction* F2 = CF2 ? CF2->GetTargetFunction() : nullptr)
                                                {
                                                    if (F2->GetName().Contains(TEXT("Array_Get")))
                                                    {
                                                        UEdGraphPin* Idx = FindInputPinByName(CF2, TEXT("Index"));
                                                        const FString D = PinDefaultOrText(Idx);
                                                        if (D == TEXT("0")) How += TEXT("[0]");
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    Emit(VarName, TEXT("is_acquired_via"), How, { A });
                                }
                            }
                        }
                    }
                }
            }

            // 3) Branch controls_visibility_of (best-effort)
            if (N->GetClass()->GetName().Contains(TEXT("K2Node_IfThenElse")))
            {
                // condition source
                FString Subject = TEXT("ComparisonResult");
                if (UEdGraphPin* Cond = FindInputPinByName(N, TEXT("Condition")))
                {
                    for (UEdGraphPin* L : Cond->LinkedTo)
                    {
                        if (!L || !L->GetOwningNode()) continue;
                        // Try to extract variable driving the condition
                        if (const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(L->GetOwningNode()))
                        {
                            const FName V = Get->GetVarName();
                            if (!V.IsNone()) Subject = V.ToString();
                        }
                        if (const UK2Node_CallFunction* CFcmp = Cast<UK2Node_CallFunction>(L->GetOwningNode()))
                        {
                            if (const UFunction* Fcmp = CFcmp->GetTargetFunction())
                            {
                                const FString Nm = Fcmp->GetName();
                                if (Nm.StartsWith(TEXT("EqualEqual")) || Nm.StartsWith(TEXT("Greater")) || Nm.StartsWith(TEXT("Less")))
                                {
                                    FString VarName, ConstStr;
                                    for (UEdGraphPin* In : CFcmp->Pins)
                                    {
                                        if (!IsDataInputPin(In)) continue;
                                        if (In->LinkedTo.Num() == 0)
                                        {
                                            const FString D = PinDefaultOrText(In);
                                            if (!D.IsEmpty()) ConstStr = D;
                                        }
                                        else
                                        {
                                            for (UEdGraphPin* Lk : In->LinkedTo)
                                            {
                                                if (!Lk || !Lk->GetOwningNode()) continue;
                                                if (const UK2Node_VariableGet* G2 = Cast<UK2Node_VariableGet>(Lk->GetOwningNode()))
                                                {
                                                    const FName Vn = G2->GetVarName();
                                                    if (!Vn.IsNone()) VarName = Vn.ToString();
                                                }
                                            }
                                        }
                                    }
                                    if (!VarName.IsEmpty() && !ConstStr.IsEmpty())
                                    {
                                        // 증거 앵커는 비교 노드의 앵커를 추가로 포함
                                        TArray<FString> Ev = { A };
                                        if (const FString* AComp = AKey.Find(CFcmp)) Ev.Add(*AComp);
                                        Emit(VarName, TEXT("is_compared_to"), ConstStr, Ev);
                                    }
                                }
                            }
                        }

                    }
                }
                // follow Then/Else exec to find SetVisibility
                auto FindVisTargetOnExec = [&](const TCHAR* ExecName)->FString {
                    if (UEdGraphPin* Exec = FindInputPinByName(N, ExecName)) { /* not used */ }
                    // outputs pins named Then/Else are outputs; scan outputs
                    for (UEdGraphPin* P : N->Pins)
                    {
                        if (!P || P->Direction != EGPD_Output) continue;
                        const FString Nm = P->PinName.ToString();
                        if (!Nm.Equals(ExecName, ESearchCase::IgnoreCase)) continue;
                        for (UEdGraphPin* L : P->LinkedTo)
                        {
                            if (!L || !L->GetOwningNode()) continue;
                            if (const UK2Node_CallFunction* CF = Cast<UK2Node_CallFunction>(L->GetOwningNode()))
                            {
                                if (const UFunction* F = CF->GetTargetFunction())
                                {
                                    if (F->GetName().Contains(TEXT("SetVisibility")))
                                    {
                                        FString Target = GetCallTargetObjectVarName(CF);
                                        if (Target.IsEmpty()) Target = TEXT("Widget");
                                        return Target;
                                    }
                                }
                            }
                        }
                    }
                    return FString();
                    };
                const FString TgtThen = FindVisTargetOnExec(TEXT("Then"));
                const FString TgtElse = FindVisTargetOnExec(TEXT("Else"));
                if (!TgtThen.IsEmpty() || !TgtElse.IsEmpty())
                {
                    const FString Target = !TgtThen.IsEmpty() ? TgtThen : TgtElse;
                    Emit(Subject, TEXT("controls_visibility_of"), Target, { A });
                }
            }
        } // nodes
    } // graphs

    // Write NDJSON (one JSON per line)
    const FString PkgPath = BP->GetOutermost()->GetName();
    const FString PkgDir = FPaths::GetPath(PkgPath);
    const FString BPDir = FPaths::Combine(OutRoot, PkgDir);
    IFileManager::Get().MakeDirectory(*BPDir, true);
    const FString OutPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpfacts.ndjson"), *BP->GetName()));
    FString Joined;
    for (int32 i = 0; i < Lines.Num(); ++i) { Joined += Lines[i]; if (i + 1 < Lines.Num()) Joined += TEXT("\n"); }
    WriteTextToFile(Joined, OutPath);
}

static FString FriendlyFromCategory(const FString& Cat, const FString& Sub)
{
    const FString C = Cat.ToLower();
    const FString S = Sub;
    if (C.Contains(TEXT("boolean")) || C == TEXT("bool")) return TEXT("bool");
    if (C.Contains(TEXT("int64"))) return TEXT("int64");
    if (C.Contains(TEXT("int"))) return TEXT("int");
    if (C.Contains(TEXT("double"))) return TEXT("double");
    if (C.Contains(TEXT("real")) || C.Contains(TEXT("float"))) return TEXT("float");
    if (C.Contains(TEXT("name"))) return TEXT("name");
    if (C.Contains(TEXT("string"))) return TEXT("string");
    if (C.Contains(TEXT("object")))
        return S.IsEmpty() ? TEXT("object") : S; // e.g. TextBlock, VerticalBox, BP class name
    return S.IsEmpty() ? (Cat.IsEmpty() ? TEXT("unknown") : Cat) : S;
}

static FString NormalizeEventIdForLabel(const FString& Id)
{
    const FString L = Id.ToLower();
    if (L.Contains(TEXT("construct"))) return TEXT("Event Construct");
    if (L.Contains(TEXT("beginplay"))) return TEXT("Event BeginPlay");
    if (L.Contains(TEXT("tick"))) return TEXT("Event Tick");
    return TEXT("Event/Custom");
}

struct FPublicApiInfo { FString Name; };

static void ExtractPublicApisFromFlows(const TArray<FString>& FlowJsonPaths, TArray<FPublicApiInfo>& Out)
{
    Out.Reset();
    for (const FString& P : FlowJsonPaths)
    {
        TSharedPtr<FJsonObject> J = LoadJsonObject(P);
        if (!J.IsValid()) continue;
        FString GraphName; J->TryGetStringField(TEXT("graph_name"), GraphName);
        if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase)) continue;
        const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
        if (!J->TryGetArrayField(TEXT("nodes"), Nodes)) continue;
        bool bHasEntry = false;
        for (const TSharedPtr<FJsonValue>& V : *Nodes)
        {
            const TSharedPtr<FJsonObject>* N; if (!V->TryGetObject(N)) continue;
            FString Klass; (*N)->TryGetStringField(TEXT("class"), Klass);
            if (Klass.Contains(TEXT("K2Node_FunctionEntry")))
            {
                bHasEntry = true; break;
            }
        }
        if (bHasEntry)
        {
            FPublicApiInfo I; I.Name = GraphName;
            Out.Add(MoveTemp(I));
        }
    }
    // sort by name for stable output
    Out.Sort([](const FPublicApiInfo& A, const FPublicApiInfo& B) { return A.Name < B.Name; });
}

static void ExtractEntryPointsFromCatalog(const TSharedPtr<FJsonObject>& Catalog,
    TArray<FString>& OutIds /*flow.eventgraph.* only*/)
{
    OutIds.Reset();
    if (!Catalog.IsValid()) return;
    const TArray<TSharedPtr<FJsonValue>>* Slices = nullptr;
    if (!Catalog->TryGetArrayField(TEXT("slices"), Slices)) return;
    for (const TSharedPtr<FJsonValue>& V : *Slices)
    {
        const TSharedPtr<FJsonObject>* O; if (!V->TryGetObject(O)) continue;
        FString Id; (*O)->TryGetStringField(TEXT("id"), Id);
        if (Id.StartsWith(TEXT("flow.eventgraph.")))
        {
            OutIds.Add(Id);
        }
    }
}

struct FVarMeta { FString Name; FString Cat; FString Sub; int Reads = 0; int Writes = 0; };

static void ExtractVarUsage(const TSharedPtr<FJsonObject>& Meta, const TSharedPtr<FJsonObject>& DefUse, TArray<FVarMeta>& OutTop)
{
    // Build map from name -> (cat, sub)
    TMap<FString, FVarMeta> Map;
    if (Meta.IsValid())
    {
        const TArray<TSharedPtr<FJsonValue>>* Vars = nullptr;
        if (Meta->TryGetArrayField(TEXT("variables"), Vars))
        {
            for (const TSharedPtr<FJsonValue>& V : *Vars)
            {
                const TSharedPtr<FJsonObject>* O; if (!V->TryGetObject(O)) continue;
                FVarMeta M;
                (*O)->TryGetStringField(TEXT("name"), M.Name);
                (*O)->TryGetStringField(TEXT("type_category"), M.Cat);
                (*O)->TryGetStringField(TEXT("type_subcat"), M.Sub);
                if (!M.Name.IsEmpty())
                {
                    Map.Add(M.Name, MoveTemp(M));
                }
            }
        }
    }
    // Fill reads/writes from bpdefuse.json
    if (DefUse.IsValid())
    {
        const TSharedPtr<FJsonObject>* VarsObj = nullptr;
        if (const TSharedPtr<FJsonObject>* VarsPtr = &DefUse->GetObjectField(TEXT("vars")))
        {
            VarsObj = VarsPtr;
        }
        if (VarsObj && VarsObj->IsValid())
        {
            for (const auto& Kvp : (*VarsObj)->Values)
            {
                const FString Name = Kvp.Key;
                const TSharedPtr<FJsonObject>* VObj; if (!Kvp.Value->TryGetObject(VObj)) continue;
                int R = 0, W = 0;
                const TArray<TSharedPtr<FJsonValue>>* Reads = nullptr;
                const TArray<TSharedPtr<FJsonValue>>* Writes = nullptr;
                if ((*VObj)->TryGetArrayField(TEXT("reads"), Reads)) R = Reads->Num();
                if ((*VObj)->TryGetArrayField(TEXT("writes"), Writes)) W = Writes->Num();
                FVarMeta* M = Map.Find(Name);
                if (!M)
                {
                    FVarMeta Tmp; Tmp.Name = Name; Tmp.Reads = R; Tmp.Writes = W;
                    Map.Add(Name, MoveTemp(Tmp));
                }
                else
                {
                    M->Reads = R; M->Writes = W;
                }
            }
        }
    }
    TArray<FVarMeta> Arr; Map.GenerateValueArray(Arr);
    Arr.Sort([](const FVarMeta& A, const FVarMeta& B) {
        const int SA = A.Reads + A.Writes;
        const int SB = B.Reads + B.Writes;
        if (SA != SB) return SA > SB;
        return A.Name < B.Name;
        });
    if (Arr.Num() > 3) Arr.SetNum(3);
    OutTop = MoveTemp(Arr);
}

static void ExtractDependencies(const TSharedPtr<FJsonObject>& DefUse, TArray<FString>& OutObjVars /*top few object vars*/)
{
    OutObjVars.Reset();
    if (!DefUse.IsValid()) return;
    const TSharedPtr<FJsonObject>* Objs = nullptr;
    if (!DefUse->TryGetObjectField(TEXT("objects"), Objs) || !Objs || !Objs->IsValid()) return;
    struct TScore { FString Name; int Score = 0; };
    TArray<TScore> Sc;
    for (const auto& Kvp : (*Objs)->Values)
    {
        const FString ObjVar = Kvp.Key;
        const TSharedPtr<FJsonObject>* One; if (!Kvp.Value->TryGetObject(One)) continue;
        int R = 0, W = 0;
        const TSharedPtr<FJsonObject>* RObj = nullptr; const TSharedPtr<FJsonObject>* WObj = nullptr;
        if ((*One)->TryGetObjectField(TEXT("reads"), RObj) && RObj && RObj->IsValid()) R = (*RObj)->Values.Num();
        if ((*One)->TryGetObjectField(TEXT("writes"), WObj) && WObj && WObj->IsValid()) W = (*WObj)->Values.Num();
        Sc.Add({ ObjVar, R + W });
    }
    Sc.Sort([](const TScore& A, const TScore& B) { if (A.Score != B.Score) return A.Score > B.Score; return A.Name < B.Name; });
    const int MaxN = FMath::Min(3, Sc.Num());
    for (int i = 0; i < MaxN; i++) OutObjVars.Add(Sc[i].Name);
}


// === Def-Use 공용 컨테이너/헬퍼 (파일 스코프) ===
struct FObjDefUse {
    TMap<FString, TSet<FString>> Writes;
    TMap<FString, TSet<FString>> Reads;
};

static void AddVarAnchor(TMap<FString, TSet<FString>>& Map, const FString& Key, const FString& Anchor)
{
    Map.FindOrAdd(Key).Add(Anchor);
}

static void AddObjPropAnchor(TMap<FString, FObjDefUse>& Objects,
    const FString& Obj, const FString& Prop,
    bool bWrite, const FString& Anchor)
{
    FObjDefUse& R = Objects.FindOrAdd(Obj);
    (bWrite ? R.Writes.FindOrAdd(Prop) : R.Reads.FindOrAdd(Prop)).Add(Anchor);
}


static void BuildDefUseForBP(
    UBlueprint* BP,
    const TMap<UEdGraph*, TArray<UEdGraphNode*>>& SortedByGraph,
    const FString& MetaFilePath,
    const TArray<FString>& FlowJsonPaths,
    const FString& OutRoot);

// 모든 그래프의 슬라이스를 메모리에 수집 (카탈로그/요약 동시 사용)
static void CollectSlicesForBP(
    UBlueprint* /*BP*/,
    const TMap<UEdGraph*, TArray<UEdGraphNode*>>& SortedByGraph,
    TArray<TSharedPtr<FJsonValue>>& OutSlices)
{
    OutSlices.Reset();
    for (auto& KVP : SortedByGraph)
        BuildSlicesForGraph(KVP.Key, KVP.Value, OutSlices);
}


// bpcatalog.json 파일을 기록 (OutSlices는 CollectSlicesForBP 결과물)
static void WriteCatalogForBP_FromSlices(
    UBlueprint* BP,
    const TArray<TSharedPtr<FJsonValue>>& Slices,
    const FString& OutRoot)
{
    if (!BP) return;
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    AddFrontMatter(J);
    J->SetStringField(TEXT("bp"), BP->GetName());

    // graphs
    TArray<TSharedPtr<FJsonValue>> GArr;
    TArray<UEdGraph*> All; BP->GetAllGraphs(All);
    for (UEdGraph* G : All) if (IsValid(G) && G->GetOuter() == BP)
        GArr.Add(MakeShared<FJsonValueString>(G->GetName()));
    J->SetArrayField(TEXT("graphs"), GArr);

    // slices
    J->SetArrayField(TEXT("slices"), Slices);

    const FString PkgPath = BP->GetOutermost()->GetName();
    const FString PkgDir = FPaths::GetPath(PkgPath);
    const FString BPDir = FPaths::Combine(OutRoot, PkgDir);
    IFileManager::Get().MakeDirectory(*BPDir, true);

    WriteJsonToFile(*J, FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpcatalog.json"), *BP->GetName())));
}



static FString Slug(const FString& In) {
    FString Out; Out.Reserve(In.Len());
    for (TCHAR C : In) { const TCHAR L = FChar::ToLower(C); if (FChar::IsAlnum(L)) Out.AppendChar(L); }
    return Out;
}

// Event 이름 정규화 (UE 표준 이벤트 일부를 친화적으로)
static FString NormalizeEventName(const FString& Raw) {
    const FString S = Slug(Raw);
    if (S == TEXT("receivebeginplay")) return TEXT("beginplay");
    if (S == TEXT("receivetick") || S == TEXT("tick")) return TEXT("tick");
    return S;
}


static FString PluginVersion()
{
    if (const TSharedPtr<IPlugin> P = IPluginManager::Get().FindPlugin(TEXT("BPTextDump")))
        return P->GetDescriptor().VersionName;
    return TEXT("0.3-dev");
}

static void AddFrontMatter(TSharedRef<FJsonObject> J)
{
    J->SetStringField(TEXT("ue_version"), FEngineVersion::Current().ToString());
    J->SetStringField(TEXT("plugin_version"), PluginVersion());
}




static bool IsInputDataPin(const UEdGraphPin* P)
{
    return P && P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec;
}

static FString Sanitize(const FString& In)
{
    FString S = In;
    S.ReplaceCharInline('\r', ' ');
    S.ReplaceCharInline('\n', ' ');
    return S.TrimStartAndEnd();
}

static FString PinDefaultInline(const UEdGraphPin* P)
{
    if (!P) return TEXT("");
    if (!P->DefaultTextValue.IsEmpty())
    {
        const FString S = Sanitize(P->DefaultTextValue.ToString());
        return FString::Printf(TEXT("\"%s\""), *S);
    }
    if (!P->DefaultValue.IsEmpty())
    {
        const FString S = Sanitize(P->DefaultValue);
        return FString::Printf(TEXT("\"%s\""), *S);
    }
    if (P->DefaultObject)
    {
        return FString::Printf(TEXT("\"%s\""), *P->DefaultObject->GetPathName());
    }
    return TEXT("");
}

// ---------- WidgetTree → JSON (nested object) ----------

static FString Vec2ToStr(const FVector2D& V) {
    return FString::Printf(TEXT("(%.0f, %.0f)"), V.X, V.Y);
}

static FString AnchorsToPresetOrText(const FAnchors& A)
{
    auto eq = [](float x, float y) { return FMath::IsNearlyEqual(x, y, KINDA_SMALL_NUMBER); };
    const bool point = eq(A.Minimum.X, A.Maximum.X) && eq(A.Minimum.Y, A.Maximum.Y);
    if (point) {
        const float x = A.Minimum.X, y = A.Minimum.Y;
        if (eq(x, 0.f) && eq(y, 0.f))   return TEXT("TopLeft");
        if (eq(x, 0.5f) && eq(y, 0.f))   return TEXT("TopCenter");
        if (eq(x, 1.f) && eq(y, 0.f))   return TEXT("TopRight");
        if (eq(x, 0.f) && eq(y, 0.5f))  return TEXT("CenterLeft");
        if (eq(x, 0.5f) && eq(y, 0.5f))  return TEXT("Center");
        if (eq(x, 1.f) && eq(y, 0.5f))  return TEXT("CenterRight");
        if (eq(x, 0.f) && eq(y, 1.f))   return TEXT("BottomLeft");
        if (eq(x, 0.5f) && eq(y, 1.f))   return TEXT("BottomCenter");
        if (eq(x, 1.f) && eq(y, 1.f))   return TEXT("BottomRight");
    }
    if (eq(A.Minimum.X, 0.f) && eq(A.Minimum.Y, 0.f) && eq(A.Maximum.X, 1.f) && eq(A.Maximum.Y, 1.f))
        return TEXT("Fill");
    return FString::Printf(TEXT("Custom(min=(%.2f, %.2f), max=(%.2f, %.2f))"),
        A.Minimum.X, A.Minimum.Y, A.Maximum.X, A.Maximum.Y);
}

static TSharedRef<FJsonObject> MakeSlotJson(UWidget* W)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    if (!W || !W->Slot) return J;

    UPanelSlot* Slot = W->Slot;
    J->SetStringField(TEXT("class"), Slot->GetClass()->GetName());

    // CanvasPanelSlot: anchors / pos / size / align / autosize
    if (UCanvasPanelSlot* CS = Cast<UCanvasPanelSlot>(Slot))
    {
        J->SetStringField(TEXT("anchors"), AnchorsToPresetOrText(CS->GetAnchors()));

        const FVector2D Pos = CS->GetPosition();
        J->SetStringField(TEXT("position"), Vec2ToStr(Pos));

        const bool bAuto = CS->GetAutoSize();
        J->SetBoolField(TEXT("size_to_content"), bAuto);

        const FVector2D Size = CS->GetSize();
        if (!bAuto) {
            J->SetStringField(TEXT("size"), Vec2ToStr(Size));
        }

        const FVector2D Align = CS->GetAlignment();
        if (!Align.IsZero()) {
            J->SetStringField(TEXT("alignment"), Vec2ToStr(Align));
        }
    }

    // TODO: 필요해지면 다른 슬롯 유형(HBox/VBox/Overlay/Grid 등)도 여기서 확장

    return J;
}

static TSharedRef<FJsonObject> WidgetToJson(UWidget* W, bool bIsRoot)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    if (!W) return J;

    // 예시와 동일하게 루트는 name="RootWidget"으로 표기
    J->SetStringField(TEXT("name"), bIsRoot ? TEXT("RootWidget") : W->GetName());
    J->SetStringField(TEXT("class"), W->GetClass()->GetName());

    if (!bIsRoot && W->Slot) {
        J->SetObjectField(TEXT("slot"), MakeSlotJson(W));
    }

    TArray<TSharedPtr<FJsonValue>> JChildren;
    if (UPanelWidget* Panel = Cast<UPanelWidget>(W)) {
        for (int32 i = 0; i < Panel->GetChildrenCount(); ++i) {
            if (UWidget* C = Panel->GetChildAt(i)) {
                JChildren.Add(MakeShared<FJsonValueObject>(WidgetToJson(C, /*bIsRoot*/false)));
            }
        }
    }
    J->SetArrayField(TEXT("children"), JChildren);
    return J;
}

static TSharedPtr<FJsonObject> BuildWidgetTreeJson(UBlueprint* BP)
{
    // 1) 위젯 블루프린트인지 확인
    UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(BP);
    if (!WBP) return nullptr;

    // 2) 디자인(에셋) 트리 사용
    UWidgetTree* WT = WBP->WidgetTree; // UBaseWidgetBlueprint가 보유
    if (!WT || !WT->RootWidget)
    {
        UE_LOG(LogTemp, Warning, TEXT("BuildWidgetTreeJson: FAILED for %s. WidgetTree or RootWidget is null (design-time)."), *BP->GetName());
        return nullptr;
    }

    // 3) 기존의 WidgetToJson 재사용
    UWidget* Root = WT->RootWidget;
    return WidgetToJson(Root, /*bIsRoot*/true);
}



// 들여쓰기: depth=0(루트)은 그대로, 그 아래부터 " L-- " 포맷
static FString IndentLine(const FString& Label, int32 Depth)
{
    if (Depth <= 0) return Label;
    const int32 Spaces = 1 + (Depth - 1) * 5; // 예시와 동일한 간격
    return FString::ChrN(Spaces, ' ') + TEXT("L-- ") + Label;
}

// 충돌 플래그 표기용
static bool HasCollisionEnabled(const UActorComponent* C)
{
    if (const UPrimitiveComponent* P = Cast<UPrimitiveComponent>(C))
    {
        return (P->GetCollisionEnabled() != ECollisionEnabled::NoCollision) || P->GetGenerateOverlapEvents();
    }
    return false;
}

// ---- 컴포넌트 트리(SCS) ----
static void AppendSCSNodeLines(const USCS_Node* Node, int32 Depth, TArray<FString>& Out)
{
    if (!Node || !Node->ComponentTemplate) return;

    const FString VarName = Node->GetVariableName().IsNone() ? TEXT("(Unnamed)") : Node->GetVariableName().ToString();
    const FString ClassName = Node->ComponentTemplate->GetClass()->GetName();

    FString Label = FString::Printf(TEXT("%s (%s)"), *VarName, *ClassName);
    if (HasCollisionEnabled(Node->ComponentTemplate)) Label += TEXT(" *Collision*");
    if (Node->ComponentTemplate->IsEditorOnly())      Label += TEXT(" *EditorOnly*");

    Out.Add(IndentLine(Label, Depth));

    const TArray<USCS_Node*>& Children = const_cast<USCS_Node*>(Node)->GetChildNodes();
    for (const USCS_Node* Child : Children)
    {
        AppendSCSNodeLines(Child, Depth + 1, Out);
    }
}

static void BuildComponentTreeLines(UBlueprint* BP, TArray<FString>& Out)
{
    Out.Reset();
    if (!BP || !BP->SimpleConstructionScript) return;

    for (const USCS_Node* Root : BP->SimpleConstructionScript->GetRootNodes())
    {
        // 루트는 들여쓰기 없이 출력
        if (Root && Root->ComponentTemplate)
        {
            const FString VarName = Root->GetVariableName().IsNone() ? TEXT("(Unnamed)") : Root->GetVariableName().ToString();
            const FString ClassName = Root->ComponentTemplate->GetClass()->GetName();

            FString RootLabel = FString::Printf(TEXT("%s (%s)"), *VarName, *ClassName);
            if (HasCollisionEnabled(Root->ComponentTemplate)) RootLabel += TEXT(" *Collision*");
            if (Root->ComponentTemplate->IsEditorOnly())      RootLabel += TEXT(" *EditorOnly*");

            Out.Add(RootLabel);
        }
         const TArray<USCS_Node*>&Children = const_cast<USCS_Node*>(Root)->GetChildNodes();
        for (const USCS_Node* Child : Children)
             {
            AppendSCSNodeLines(Child, /*Depth=*/1, Out);
             }
    }
}

// ---- 위젯 트리(WidgetTree) ----
static void AppendWidgetNodeLines(const UWidget* W, int32 Depth, TArray<FString>& Out)
{
    if (!W) return;

    const FString Label = FString::Printf(TEXT("%s (%s)"), *W->GetName(), *W->GetClass()->GetName());
    Out.Add(IndentLine(Label, Depth));

    if (const UPanelWidget* Panel = Cast<UPanelWidget>(W))
    {
        for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
        {
            if (UWidget* Child = Panel->GetChildAt(i))
            {
                AppendWidgetNodeLines(Child, Depth + 1, Out);
            }
        }
    }
}

static void BuildWidgetTreeLines(UBlueprint* BP, TArray<FString>& Out)
{
    Out.Reset();
    if (!BP || !BP->GeneratedClass) return;
    if (!BP->GeneratedClass->IsChildOf(UUserWidget::StaticClass())) return;

    if (const UUserWidget* CDO = Cast<UUserWidget>(BP->GeneratedClass->GetDefaultObject(false)))
    {
        if (const UWidgetTree* WT = CDO->WidgetTree)
        {
            if (const UWidget* Root = WT->RootWidget)
            {
                Out.Add(TEXT("RootWidget"));                 // 예시 포맷 유지
                AppendWidgetNodeLines(Root, /*Depth=*/1, Out);
            }
        }
    }
}


static FString ExportPropertyOnCDO(UBlueprint* BP, const FName VarName)
{
    if (!BP || !BP->GeneratedClass) return TEXT("");
    UObject* CDO = BP->GeneratedClass->GetDefaultObject(false);
    if (!CDO) return TEXT("");
    if (FProperty* P = FindFProperty<FProperty>(BP->GeneratedClass, VarName))
    {
        FString Out; P->ExportText_InContainer(0, Out, CDO, CDO, CDO, PPF_None);
        return Out;
    }
    return TEXT("");
}

static TSharedRef<FJsonObject> MakeBPContextJson(UBlueprint* BP)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    if (!BP) return J;

    J->SetStringField(TEXT("bp_asset_class"), BP->GetClass()->GetName());

    if (UClass* Parent = BP->ParentClass)
    {
        J->SetStringField(TEXT("bp_parent_class"), Parent->GetName());
        J->SetStringField(TEXT("bp_parent_class_path"), Parent->GetPathName());
    }

    // BlueprintType 안정 처리
    const TCHAR* TypeNames[] = { TEXT("Normal"), TEXT("Const"), TEXT("MacroLibrary"), TEXT("Interface"), TEXT("FunctionLibrary") };
    const int32 TypeIdx = (int32)BP->BlueprintType;
    const TCHAR* TypeName = (TypeIdx >= 0 && TypeIdx < (int32)UE_ARRAY_COUNT(TypeNames)) ? TypeNames[TypeIdx] : TEXT("Unknown");
    J->SetStringField(TEXT("blueprint_type"), FString(TypeName));

    // Interfaces
    {
        TArray<TSharedPtr<FJsonValue>> Ifaces;
        for (const FBPInterfaceDescription& D : BP->ImplementedInterfaces)
        {
            if (D.Interface)
            {
                Ifaces.Add(MakeShared<FJsonValueString>(D.Interface->GetPathName()));
            }
        }
        J->SetArrayField(TEXT("implemented_interfaces"), Ifaces);
    }

    // Variables
    {
        TArray<TSharedPtr<FJsonValue>> JVars;
        for (const FBPVariableDescription& V : BP->NewVariables)
        {
            TSharedRef<FJsonObject> JV = MakeShared<FJsonObject>();
            JV->SetStringField(TEXT("name"), V.VarName.ToString());
            JV->SetStringField(TEXT("type_category"), V.VarType.PinCategory.ToString());
            JV->SetStringField(TEXT("type_subcat"), V.VarType.PinSubCategory.ToString());
            JV->SetStringField(TEXT("default_editor"), V.DefaultValue);
            JV->SetStringField(TEXT("default_cdo"), ExportPropertyOnCDO(BP, V.VarName));

            const uint64 F = (uint64)V.PropertyFlags;
            JV->SetStringField(TEXT("flags_hex"), FString::Printf(TEXT("0x%llx"), F));
            JV->SetBoolField(TEXT("editable"), (F & CPF_Edit) != 0);
            JV->SetBoolField(TEXT("blueprint_read_only"), (F & CPF_BlueprintReadOnly) != 0);
            JV->SetBoolField(TEXT("instance_editable"), ((F & CPF_DisableEditOnInstance) == 0) && ((F & CPF_Edit) != 0));
            JV->SetBoolField(TEXT("expose_on_spawn"), (F & CPF_ExposeOnSpawn) != 0);

            JV->SetStringField(TEXT("category"), V.Category.ToString());

            // UE5.6: FBPVariableDescription에 직접 ToolTip 멤버 없음 → 안전하게 비움(원하면 차기버전에서 Editor API로 조회)
            JV->SetStringField(TEXT("tooltip"), TEXT(""));

            // MetaData 전체 덤프는 UE5.6에선 공개 API 경로가 들쑥날쑥 → 건너뜀
            JV->SetStringField(TEXT("defined_in"), ResolveVarDefinedIn(BP, V.VarName));
            JVars.Add(MakeShared<FJsonValueObject>(JV));
        }
        J->SetArrayField(TEXT("variables"), JVars);
    }

    // --- functions (source & origin) ---
    {
        TArray<TSharedPtr<FJsonValue>> JFuncs;
        TSet<FString> Dedup; // key = type|name

        TArray<UEdGraph*> AllGraphs;
        BP->GetAllGraphs(AllGraphs);
        for (UEdGraph* G : AllGraphs)
        {
            if (!IsValid(G) || G->GetOuter() != BP) continue; // top-level만

            bool bHasFunctionEntry = false;
            for (UEdGraphNode* N : G->Nodes)
            {
                if (IsValid(N) && N->IsA<UK2Node_FunctionEntry>()) { bHasFunctionEntry = true; break; }
            }
            if (bHasFunctionEntry)
            {
                const FString Name = G->GetName();
                const FString Key = TEXT("function|") + Name;
                if (!Dedup.Contains(Key))
                {
                    TSharedRef<FJsonObject> JF = MakeShared<FJsonObject>();
                    JF->SetStringField(TEXT("name"), Name);
                    JF->SetStringField(TEXT("type"), TEXT("function"));
                    JF->SetStringField(TEXT("defined_in"), ResolveFunctionDefinedIn(BP, FName(*Name)));
                    JFuncs.Add(MakeShared<FJsonValueObject>(JF));
                    Dedup.Add(Key);
                }
            }

            // 이벤트/커스텀 이벤트도 함수 목록으로 기록
            for (UEdGraphNode* N : G->Nodes)
            {
                if (!IsValid(N)) continue;
                if (const UK2Node_Event* Ev = Cast<UK2Node_Event>(N))
                {
                    const FName EvName = Ev->EventReference.GetMemberName();
                    if (!EvName.IsNone())
                    {
                        const FString Name = EvName.ToString();
                        const FString Key = TEXT("event|") + Name;
                        if (!Dedup.Contains(Key))
                        {
                            TSharedRef<FJsonObject> JF = MakeShared<FJsonObject>();
                            JF->SetStringField(TEXT("name"), Name);
                            JF->SetStringField(TEXT("type"), TEXT("event"));
                            JF->SetStringField(TEXT("defined_in"), ResolveFunctionDefinedIn(BP, EvName));
                            JFuncs.Add(MakeShared<FJsonValueObject>(JF));
                            Dedup.Add(Key);
                        }
                    }
                }
                else if (const UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(N))
                {
                    const FString Name = CE->CustomFunctionName.ToString();
                    if (!Name.IsEmpty())
                    {
                        const FString Key = TEXT("custom_event|") + Name;
                        if (!Dedup.Contains(Key))
                        {
                            TSharedRef<FJsonObject> JF = MakeShared<FJsonObject>();
                            JF->SetStringField(TEXT("name"), Name);
                            JF->SetStringField(TEXT("type"), TEXT("custom_event"));
                            JF->SetStringField(TEXT("defined_in"), GetSelfClassPath(BP));
                            JFuncs.Add(MakeShared<FJsonValueObject>(JF));
                            Dedup.Add(Key);
                        }
                    }
                }
            }
        }
        if (JFuncs.Num() > 0)
        {
            J->SetArrayField(TEXT("functions"), JFuncs);
        }
    }


    // --- Component / Widget hierarchy (as text lines) ---
    {
                // 그대로 유지 (컴포넌트 트리 라인)
            TArray<FString> CompLines;
        BuildComponentTreeLines(BP, CompLines);
        if (CompLines.Num() > 0)
             {
            TArray<TSharedPtr<FJsonValue>> JArr;
            for (const FString& L : CompLines) JArr.Add(MakeShared<FJsonValueString>(L));
            J->SetArrayField(TEXT("component_tree"), JArr);
            }
           // 필드명만 바꿔서 충돌 방지: widget_tree_lines
            TArray<FString> WidgetLines;
        BuildWidgetTreeLines(BP, WidgetLines);
        if (WidgetLines.Num() > 0)
             {
            TArray<TSharedPtr<FJsonValue>> JArr;
            for (const FString& L : WidgetLines) JArr.Add(MakeShared<FJsonValueString>(L));
            J->SetArrayField(TEXT("widget_tree_lines"), JArr);
            }
         }

     if (TSharedPtr<FJsonObject> WTree = BuildWidgetTreeJson(BP))
         {
        J->SetObjectField(TEXT("widget_tree"), WTree.ToSharedRef());
        }
     return J;
}

static UEdGraph* FindFunctionGraphByName(UBlueprint* BP, const FName FuncName)
{
    if (!BP || FuncName.IsNone()) return nullptr;
    TArray<UEdGraph*> All; BP->GetAllGraphs(All);
    const FString Want = FuncName.ToString();
    for (UEdGraph* G : All)
    {
        if (IsValid(G) && G->GetOuter() == BP && G->GetName() == Want)
            return G;
    }
    return nullptr;
}

static UEdGraph* FindMacroGraphByName(UBlueprint* BP, const FName MacroName)
{
    if (!BP || MacroName.IsNone()) return nullptr;
    TArray<UEdGraph*> All; BP->GetAllGraphs(All);
    const FString Want = MacroName.ToString();
    for (UEdGraph* G : All)
    {
        if (IsValid(G) && G->GetOuter() == BP && G->GetName() == Want)
            return G;
    }
    return nullptr;
}

static TSharedRef<FJsonObject> MakeNodeJson(UEdGraphNode* Node)
{
    TSharedRef<FJsonObject> JNode = MakeShared<FJsonObject>();
    JNode->SetStringField(TEXT("guid"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
    JNode->SetStringField(TEXT("class"), Node->GetClass()->GetName());
    JNode->SetStringField(TEXT("title"), Sanitize(Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString()));
    JNode->SetStringField(TEXT("comment"), Sanitize(Node->NodeComment));
    JNode->SetNumberField(TEXT("pos_x"), Node->NodePosX);
    JNode->SetNumberField(TEXT("pos_y"), Node->NodePosY);

    const FString AKey = BTD::AnchorForNode(Node);
    JNode->SetStringField(TEXT("akey"), AKey);

    // ---------- 노드별 메타 ----------
    if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
    {
        TSharedRef<FJsonObject> JCall = MakeShared<FJsonObject>();
        if (UFunction* Fn = Call->GetTargetFunction())
        {
            const FString OwnerPath = Fn->GetOwnerClass() ? Fn->GetOwnerClass()->GetPathName() : TEXT("");
            const FString FnName = Fn->GetName();

            // 기존 call 오브젝트 유지
            JCall->SetStringField(TEXT("owner_class_path"), OwnerPath);
            JCall->SetStringField(TEXT("function_name"), FnName);
            JCall->SetBoolField(TEXT("is_const"), Fn->HasAnyFunctionFlags(FUNC_Const));
            JCall->SetBoolField(TEXT("is_static"), Fn->HasAnyFunctionFlags(FUNC_Static));

            JNode->SetStringField(TEXT("function_name"), FnName);
            JNode->SetStringField(TEXT("function_owner_path"), OwnerPath);
        }
        JCall->SetBoolField(TEXT("is_pure"), Call->IsNodePure());
        JNode->SetObjectField(TEXT("call"), JCall);
    }
    else if (const UK2Node_MacroInstance* MI = Cast<UK2Node_MacroInstance>(Node))
    {
        TSharedRef<FJsonObject> JMac = MakeShared<FJsonObject>();
        if (MI->GetMacroGraph())
        {
            JMac->SetStringField(TEXT("macro_graph_name"), MI->GetMacroGraph()->GetName());
            if (UBlueprint* SrcBP = MI->GetSourceBlueprint())
                JMac->SetStringField(TEXT("macro_source_bp_path"), SrcBP->GetPathName());
        }
        JNode->SetObjectField(TEXT("macro"), JMac);
    }
    else if (const UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(Node))
    {
        const FString EvName = CE->CustomFunctionName.ToString();
        TSharedRef<FJsonObject> JEv = MakeShared<FJsonObject>();
        JEv->SetStringField(TEXT("event_name"), EvName);
        JNode->SetObjectField(TEXT("custom_event"), JEv);

        JNode->SetStringField(TEXT("event_name"), EvName);
    }
    else if (const UK2Node_Event* Ev = Cast<UK2Node_Event>(Node))
    {
        const FName EvFName = Ev->EventReference.GetMemberName();
        const FString EvName = EvFName.IsNone() ? TEXT("") : EvFName.ToString();
        if (!EvName.IsEmpty())
        {
            JNode->SetStringField(TEXT("event_name"), EvName);
        }
    }

    // 변수 노드 (Get/Set 공통)
    if (const UK2Node_Variable* VNode = Cast<UK2Node_Variable>(Node))
    {
        const FName VarName = VNode->GetVarName();
        if (!VarName.IsNone())
        {
            JNode->SetStringField(TEXT("variable_name"), VarName.ToString());
        }
    }

    // ---------- 핀들 ----------
    TArray<TSharedPtr<FJsonValue>> JPins;
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (!Pin) continue;
        TSharedRef<FJsonObject> JPin = MakeShared<FJsonObject>();
        const bool bExec = (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);
        const bool bInput = (Pin->Direction == EGPD_Input);

        JPin->SetStringField(TEXT("name"), Pin->PinName.ToString());
        JPin->SetStringField(TEXT("dir"), bInput ? TEXT("in") : TEXT("out"));
        JPin->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
        JPin->SetStringField(TEXT("subcat"), Pin->PinType.PinSubCategory.ToString());
        JPin->SetBoolField(TEXT("is_exec"), bExec);
        JPin->SetBoolField(TEXT("is_by_ref"), Pin->PinType.bIsReference);

        const bool bLinked = Pin->LinkedTo.Num() > 0;
        JPin->SetBoolField(TEXT("is_linked"), bLinked);

        if (bInput && !bLinked)
        {
            if (!Pin->DefaultValue.IsEmpty())
                 JPin->SetStringField(TEXT("default_value"), Pin->DefaultValue);
            if (!Pin->DefaultTextValue.IsEmpty())
                 JPin->SetStringField(TEXT("default_text"), Pin->DefaultTextValue.ToString());
            if (Pin->DefaultObject)
                 JPin->SetStringField(TEXT("default_object_path"), Pin->DefaultObject->GetPathName());
        }

        // links
        TArray<TSharedPtr<FJsonValue>> JLinks;
        for (UEdGraphPin* L : Pin->LinkedTo)
        {
            if (!L || !L->GetOwningNode()) continue;
            TSharedRef<FJsonObject> JLink = MakeShared<FJsonObject>();
            JLink->SetStringField(TEXT("to_guid"), L->GetOwningNode()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensInBraces));
            JLink->SetStringField(TEXT("to_pin"), L->PinName.ToString());
            JLinks.Add(MakeShared<FJsonValueObject>(JLink));
        }
        JPin->SetArrayField(TEXT("links"), JLinks);
        JPins.Add(MakeShared<FJsonValueObject>(JPin));
    }
    JNode->SetArrayField(TEXT("pins"), JPins);

    return JNode;
}


static void AttachCalleeGraphHints(UBlueprint* BP, const TSharedRef<FJsonObject>& JNode, UEdGraphNode* Node)
{
    if (!BP || !Node) return;

    if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
    {
        if (Call->FunctionReference.IsSelfContext())
        {
            if (UEdGraph* FG = FindFunctionGraphByName(BP, Call->FunctionReference.GetMemberName()))
            {
                TSharedPtr<FJsonObject> JCall = JNode->GetObjectField(TEXT("call"));
                if (JCall.IsValid())
                {
                    JCall->SetStringField(TEXT("callee_graph_name"), FG->GetName());
                }
            }
        }
    }
    else if (const UK2Node_MacroInstance* MI = Cast<UK2Node_MacroInstance>(Node))
    {
        if (UEdGraph* MG = MI->GetMacroGraph())
        {
            TSharedPtr<FJsonObject> JMac = JNode->GetObjectField(TEXT("macro"));
            if (JMac.IsValid())
            {
                JMac->SetStringField(TEXT("callee_graph_name"), MG->GetName());
            }
        }
    }
}

static TSharedRef<FJsonObject> MakeGraphJson(UBlueprint* BP, UEdGraph* Graph)
{
    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    J->SetStringField(TEXT("bp_package"), BP->GetOutermost()->GetName());
    J->SetStringField(TEXT("bp_name"), BP->GetName());
    J->SetStringField(TEXT("graph_name"), Graph->GetName());

    TArray<TSharedPtr<FJsonValue>> JNodes;
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!IsValid(Node)) continue;
        TSharedRef<FJsonObject> JNode = MakeNodeJson(Node);
        AttachCalleeGraphHints(BP, JNode, Node);
        JNodes.Add(MakeShared<FJsonValueObject>(JNode));
    }
    J->SetArrayField(TEXT("nodes"), JNodes);

    return J;
}

static void BuildSlicesForGraph(UEdGraph* Graph,
    const TArray<UEdGraphNode*>& SortedNodes,
    TArray<TSharedPtr<FJsonValue>>& Out)
{
    if (!Graph || SortedNodes.Num() == 0) return;

    const FString GraphName = Graph->GetName();
    const FString GSlug = Slug(GraphName);
    const FString AFirst = BTD::AnchorForNode(SortedNodes[0]);
    const FString ALast = BTD::AnchorForNode(SortedNodes.Last());

    const bool bIsEventGraph = (GSlug == TEXT("eventgraph"));
    if (bIsEventGraph)
    {
        // full
        {
            TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("id"), TEXT("flow.eventgraph.full"));
            J->SetStringField(TEXT("type"), TEXT("flow"));
            J->SetStringField(TEXT("range"), FString::Printf(TEXT("@%s-@%s"), *AFirst, *ALast));
            J->SetStringField(TEXT("source"), GraphName);
            Out.Add(MakeShared<FJsonValueObject>(J));
        }
        // events
        struct Ev { int32 Idx; FString Id; };
        TArray<Ev> Entries;
        for (int32 i = 0; i < SortedNodes.Num(); ++i)
        {
            if (const UK2Node_CustomEvent* CE = Cast<UK2Node_CustomEvent>(SortedNodes[i]))
                Entries.Add({ i, FString::Printf(TEXT("flow.eventgraph.%s"), *NormalizeEventName(CE->CustomFunctionName.ToString())) });
            else if (const UK2Node_Event* Ev = Cast<UK2Node_Event>(SortedNodes[i]))
            {
                const FName FN = Ev->EventReference.GetMemberName();
                Entries.Add({ i, FString::Printf(TEXT("flow.eventgraph.%s"), *NormalizeEventName(FN.IsNone() ? TEXT("event") : FN.ToString())) });
            }
        }
        for (int32 k = 0; k < Entries.Num(); ++k)
        {
            const int32 Start = Entries[k].Idx;
            const int32 End = (k + 1 < Entries.Num()) ? (Entries[k + 1].Idx - 1) : (SortedNodes.Num() - 1);
            const FString A0 = BTD::AnchorForNode(SortedNodes[Start]);
            const FString A1 = BTD::AnchorForNode(SortedNodes[End]);
            TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
            J->SetStringField(TEXT("id"), Entries[k].Id);
            J->SetStringField(TEXT("type"), TEXT("flow"));
            J->SetStringField(TEXT("range"), FString::Printf(TEXT("@%s-@%s"), *A0, *A1));
            J->SetStringField(TEXT("source"), GraphName);
            Out.Add(MakeShared<FJsonValueObject>(J));
        }
        return;
    }

    // function/macro/etc → graph-wide slice
    {
        TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("id"), FString::Printf(TEXT("flow.%s"), *GSlug));
        J->SetStringField(TEXT("type"), TEXT("flow"));
        J->SetStringField(TEXT("range"), FString::Printf(TEXT("@%s-@%s"), *AFirst, *ALast));
        J->SetStringField(TEXT("source"), GraphName);
        Out.Add(MakeShared<FJsonValueObject>(J));
    }
}

static bool WriteJsonToFile(const FJsonObject& Root, const FString& OutPath)
{
    FString JsonStr;
    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonStr);
    if (!FJsonSerializer::Serialize(MakeShared<FJsonObject>(Root), Writer)) return false;
    return FFileHelper::SaveStringToFile(JsonStr, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

static void WriteCatalogForBP(UBlueprint* BP,
    const TMap<UEdGraph*, TArray<UEdGraphNode*>>& SortedByGraph,
    const FString& OutRoot)
{
    if (!BP) return;

    TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
    AddFrontMatter(J);
    J->SetStringField(TEXT("bp"), BP->GetName());

    // graphs
    TArray<TSharedPtr<FJsonValue>> GArr;
    for (auto& KVP : SortedByGraph)
        GArr.Add(MakeShared<FJsonValueString>(KVP.Key->GetName()));
    J->SetArrayField(TEXT("graphs"), GArr);

    // slices
    TArray<TSharedPtr<FJsonValue>> SArr;
    for (auto& KVP : SortedByGraph)
        BuildSlicesForGraph(KVP.Key, KVP.Value, SArr);
    J->SetArrayField(TEXT("slices"), SArr);

    const FString PkgPath = BP->GetOutermost()->GetName();
    const FString PkgDir = FPaths::GetPath(PkgPath);
    const FString BPDir = FPaths::Combine(OutRoot, PkgDir);
    IFileManager::Get().MakeDirectory(*BPDir, true);

    WriteJsonToFile(*J, FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpcatalog.json"), *BP->GetName())));
}


static bool WriteTextToFile(const FString& Text, const FString& OutPath)
{
    // CRLF 정규화 (윈도우 뷰어 호환)
    FString Norm = Text;
#if PLATFORM_WINDOWS
    Norm.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
    Norm.ReplaceInline(TEXT("\r"), TEXT("\n"));
    Norm.ReplaceInline(TEXT("\n"), TEXT("\r\n"));
#endif

    // UTF-8로 변환
    FTCHARToUTF8 Conv(*Norm);

    // md/txt는 BOM 포함 저장 → 레거시 뷰어 호환
    const bool bWantBom = OutPath.EndsWith(TEXT(".md")) || OutPath.EndsWith(TEXT(".txt"));

    TArray<uint8> Bytes;
    if (bWantBom) { Bytes.Add(0xEF); Bytes.Add(0xBB); Bytes.Add(0xBF); } // BOM
    Bytes.Append(reinterpret_cast<const uint8*>(Conv.Get()), Conv.Length());

    if (!FFileHelper::SaveArrayToFile(Bytes, *OutPath))
    {
        UE_LOG(LogTemp, Warning, TEXT("Failed to write %s"), *OutPath);
        return false;
    }
    return true;
}



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
        const FString Title = Sanitize(N->GetNodeTitle(ENodeTitleType::ListView).ToString());
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
                    if (!IsInputDataPin(P)) continue;
                    if (P->LinkedTo.Num() > 0) continue;

                    const FString V = PinDefaultInline(P);
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
    TSharedPtr<FJsonObject> JDefUse = LoadJsonObject(DefUsePath);
    struct FVRW { TArray<FString> R; TArray<FString> W; };
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
                        if (!IsDataInputPin(In)) continue;
                        if (In->LinkedTo.Num() == 0)
                        {
                            const FString D = PinDefaultOrText(In);
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
                            UEdGraphPin* Idx = FindInputPinByName(CF_ArrayGet, TEXT("Index"));
                            const FString D = PinDefaultOrText(Idx);
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
    WriteTextToFile(MD, OutPath);
}


static int32 DumpBlueprintToDir(UBlueprint* BP, const FString& OutRoot)
{
    if (!BP) return 0;
    TArray<UEdGraph*> Graphs; CollectTopLevelGraphs(BP, Graphs);

    const FString PkgPath = BP->GetOutermost()->GetName();
    const FString PkgDir = FPaths::GetPath(PkgPath);
    const FString BPDir = FPaths::Combine(OutRoot, PkgDir);
    IFileManager::Get().MakeDirectory(*BPDir, true);

    // ① BP 메타 1회 기록
    WriteJsonToFile(*MakeBPContextJson(BP), FPaths::Combine(BPDir, FString::Printf(TEXT("%s__BP__Meta.bpmeta.json"), *BP->GetName())));

    TMap<UEdGraph*, TArray<UEdGraphNode*>> SortedByGraph; // for catalog

    int32 Dumped = 0;
    for (UEdGraph* G : Graphs)
    {
        if (!IsValid(G)) continue;

        // Stable sort (same as DSL)
        TArray<UEdGraphNode*> Nodes = G->Nodes;
        Nodes.RemoveAll([](UEdGraphNode* N) { return !IsValid(N); });
        Nodes.Sort([](const UEdGraphNode& A, const UEdGraphNode& B)
            {
                if (A.NodePosY != B.NodePosY) return A.NodePosY < B.NodePosY;
                return A.NodePosX < B.NodePosX;
            });
        SortedByGraph.Add(G, Nodes);

        const FString Base = FPaths::Combine(BPDir, FString::Printf(TEXT("%s__%s"), *BP->GetName(), *G->GetName()));
        TSharedRef<FJsonObject> J = MakeGraphJson(BP, G);
        AddFrontMatter(J);
        WriteJsonToFile(*J, Base + TEXT(".bpflow.json"));
        WriteTextToFile(BuildBPFlowDSL(BP, G), Base + TEXT(".bpflow.txt"));
        ++Dumped;
    }

    // 그래프별 정렬은 기존대로 끝났다고 가정
    TArray<TSharedPtr<FJsonValue>> Slices;
    CollectSlicesForBP(BP, SortedByGraph, Slices);

    // 카탈로그 파일 생성 (메모리 슬라이스 활용)
    WriteCatalogForBP_FromSlices(BP, Slices, OutRoot);

    // 해시 계산 대상 파일 목록 준비
    TArray<FString> FlowJsonPaths;
    for (UEdGraph* G : Graphs)
    {
        if (!IsValid(G)) continue;
        const FString Base = FPaths::Combine(BPDir, FString::Printf(TEXT("%s__%s"), *BP->GetName(), *G->GetName()));
        FlowJsonPaths.Add(Base + TEXT(".bpflow.json"));
    }
    const FString MetaPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s__BP__Meta.bpmeta.json"), *BP->GetName()));

    // bpsmry.md 생성
    BuildAndWriteSummaryForBP(BP, Slices, MetaPath, FlowJsonPaths, OutRoot);
    // bpdefuse.json 생성
    BuildDefUseForBP(BP, SortedByGraph, MetaPath, FlowJsonPaths, OutRoot);

    BuildFactsForBP(BP, SortedByGraph, OutRoot);
    BuildLintForBP(BP, SortedByGraph, OutRoot);
    return Dumped;
}


// ---------------- module ----------------

// --- 내부 도우미 ---
static bool IsDataInputPin(const UEdGraphPin* P)
{
    return P && P->Direction == EGPD_Input && P->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec;
}


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
                if (!Var.IsNone()) AddVarAnchor(VarWrites, Var.ToString(), NodeAnchor);
            }

            if (const UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(N)) {
                const FName Var = GetNode->GetVarName();
                if (!Var.IsNone()) AddVarAnchor(VarReads, Var.ToString(), NodeAnchor);
            }

            if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(N)) {
                const UFunction* Fn = Call->GetTargetFunction();
                if (Fn) {
                    FString Prop; bool bWrite = false;
                    if (ExtractPropertyFromFunctionName(Fn->GetName(), Prop, bWrite)) {
                        const FString ObjName = GetCallTargetObjectVarName(Call);
                        if (!ObjName.IsEmpty()) {
                            AddObjPropAnchor(Objects, ObjName, Prop, bWrite, NodeAnchor);
                        }
                    }
                }
                // 입력 핀에 물린 GET → 변수 read (소비자 기준 앵커도 NodeAnchor 사용)
                for (UEdGraphPin* In : Call->Pins) {
                    if (!IsDataInputPin(In)) continue;
                    for (UEdGraphPin* L : In->LinkedTo) {
                        if (!L || !L->GetOwningNode()) continue;
                        if (const UK2Node_VariableGet* Get2 = Cast<UK2Node_VariableGet>(L->GetOwningNode())) {
                            const FName V = Get2->GetVarName();
                            if (!V.IsNone()) AddVarAnchor(VarReads, V.ToString(), NodeAnchor);
                        }
                    }
                }
            }

        }
    }


    // --- JSON 빌드 ---
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    AddFrontMatter(Root); // ue_version, plugin_version
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
    WriteJsonToFile(*Root, OutPath);
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
    TSharedPtr<FJsonObject> JMeta = LoadJsonObject(MetaFilePath);
    TSharedPtr<FJsonObject> JCatalog = LoadJsonObject(CatalogPath);
    TSharedPtr<FJsonObject> JDefUse = LoadJsonObject(DefUsePath);

    // Entry points from catalog slices
    TArray<FString> EntryIds; ExtractEntryPointsFromCatalog(JCatalog, EntryIds);

    // Public API from per-graph flow jsons
    TArray<FPublicApiInfo> PublicApis;
    ExtractPublicApisFromFlows(FlowJsonPaths, PublicApis);

    // Top variables (by reads+writes) with friendly types from meta
    TArray<FVarMeta> TopVars; ExtractVarUsage(JMeta, JDefUse, TopVars);

    // Dependencies (object var names) from defuse objects
    TArray<FString> DependsOn; ExtractDependencies(JDefUse, DependsOn);

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
            const FString Label = NormalizeEventIdForLabel(Id);
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
            const FString Ty = FriendlyFromCategory(V.Cat, V.Sub);
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
        const FString Slugged = Slug(A.Name);
        MD += TEXT("- flow.") + Slugged + TEXT("\n");
    }

    IFileManager::Get().MakeDirectory(*BPDir, true);
    const FString OutPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpsmry.md"), *BP->GetName()));
    WriteTextToFile(MD, OutPath);

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
    // Args: Root=/Game[,/Plugin/Content] Out=C:/path
    TArray<FString> Roots; Roots.Add(TEXT("/Game"));
    FString OutRoot = DefaultOutDir();
    for (const FString& A : Args)
    {
        if (A.StartsWith(TEXT("Root="))) { Roots.Reset(); A.Mid(5).ParseIntoArray(Roots, TEXT(","), true); }
        else if (A.StartsWith(TEXT("Out="))) OutRoot = A.Mid(4);
    }
    IFileManager::Get().MakeDirectory(*OutRoot, true);

    // 1) 인벤토리: 모든 BP 로드, 이름 인덱스 생성, 팩 생성 보장
    FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    FARFilter Filter; Filter.bRecursivePaths = true; Filter.bRecursiveClasses = true;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    for (const FString& R : Roots) Filter.PackagePaths.Add(*R);
    TArray<FAssetData> Assets; ARM.Get().GetAssets(Filter, Assets);

    struct FAssetInfo {
        FString PkgPath;        // "/Game/Path/Asset"
        FString AssetType;      // "Blueprint" | "WidgetBlueprint"
        FString GenClassName;   // "Asset_C"
    };
    TArray<FAssetInfo> All;
    TMap<FString, FString> NameToAssetKey; // "BP_Foo" -> "/Game/.../BP_Foo", "BP_Foo_C" -> same

    for (const FAssetData& AD : Assets)
    {
        UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
        if (!BP) continue;
        // ensure packs
        DumpBlueprintToDir(BP, OutRoot);

        FAssetInfo Info;
        Info.PkgPath = BP->GetOutermost()->GetName(); // "/Game/.../Asset"
        Info.AssetType = BP->IsA<UWidgetBlueprint>() ? TEXT("WidgetBlueprint") : TEXT("Blueprint");
        Info.GenClassName = BP->GeneratedClass ? BP->GeneratedClass->GetName() : FString();
        All.Add(MoveTemp(Info));

        NameToAssetKey.Add(BP->GetName(), ToAssetKeyFromClassPath(BP->GetOutermost()->GetName())); // package key
        if (!Info.GenClassName.IsEmpty())
            NameToAssetKey.Add(Info.GenClassName, ToAssetKeyFromClassPath(BP->GetOutermost()->GetName()));
    }

    // 2) 레퍼런스 그래프 구축
    TMap<FString, TSet<FString>> RefTo; // From -> {To,...}
    for (const FAssetInfo& It : All)
    {
        const FString From = It.PkgPath;
        const FString PkgDir = FPaths::GetPath(From);
        const FString BPDir = FPaths::Combine(OutRoot, PkgDir);
        const FString BaseName = FPaths::GetCleanFilename(From); // Asset

        // 2.a) bpmeta.json
        const FString MetaPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s__BP__Meta.bpmeta.json"), *BaseName));
        if (TSharedPtr<FJsonObject> JM = LoadJsonObject(MetaPath))
        {
            FString ParentPath;
            if (JM->TryGetStringField(TEXT("bp_parent_class_path"), ParentPath))
                AddRef(RefTo, From, ToAssetKeyFromClassPath(ParentPath));
            const TArray<TSharedPtr<FJsonValue>>* Ifaces = nullptr;
            if (JM->TryGetArrayField(TEXT("implemented_interfaces"), Ifaces))
            {
                for (const auto& V : *Ifaces)
                {
                    FString P; if (V->TryGetString(P))
                        AddRef(RefTo, From, ToAssetKeyFromClassPath(P));
                }
            }
        }

        // 2.b) bpcatalog.json -> 그래프 목록
        const FString CatalogPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpcatalog.json"), *BaseName));
        TArray<FString> GraphNames;
        if (TSharedPtr<FJsonObject> JC = LoadJsonObject(CatalogPath))
        {
            const TArray<TSharedPtr<FJsonValue>>* GArr = nullptr;
            if (JC->TryGetArrayField(TEXT("graphs"), GArr))
            {
                for (const auto& V : *GArr) { FString N; if (V->TryGetString(N)) GraphNames.Add(N); }
            }
        }

        // 2.c) 각 graph의 bpflow.json: 핀 기본 오브젝트 경로 스캔
        for (const FString& GName : GraphNames)
        {
            const FString FlowPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s__%s.bpflow.json"), *BaseName, *GName));
            if (TSharedPtr<FJsonObject> JF = LoadJsonObject(FlowPath))
            {
                const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
                if (JF->TryGetArrayField(TEXT("nodes"), Nodes))
                {
                    for (const auto& NV : *Nodes)
                    {
                        const TSharedPtr<FJsonObject>* NObj = nullptr; if (!NV->TryGetObject(NObj)) continue;
                        const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
                        if ((*NObj)->TryGetArrayField(TEXT("pins"), Pins))
                        {
                            for (const auto& PV : *Pins)
                            {
                                const TSharedPtr<FJsonObject>* PObj = nullptr; if (!PV->TryGetObject(PObj)) continue;
                                FString DOP;
                                if ((*PObj)->TryGetStringField(TEXT("default_object_path"), DOP))
                                    AddRef(RefTo, From, ToAssetKeyFromObjectPath(DOP));
                                FString DO2;
                                if ((*PObj)->TryGetStringField(TEXT("default_object"), DO2))
                                    AddRef(RefTo, From, ToAssetKeyFromObjectPath(DO2));
                            }
                        }
                    }
                }
            }
        }

        // 2.d) bpfacts.ndjson: depends_on 등에서 클래스명/경로 해석
        const FString FactsPath = FPaths::Combine(BPDir, FString::Printf(TEXT("%s.bpfacts.ndjson"), *BaseName));
        ReadNDJSONLines(FactsPath, [&](const TSharedPtr<FJsonObject>& O)
            {
                FString S, P, Obj;
                O->TryGetStringField(TEXT("s"), S);
                O->TryGetStringField(TEXT("p"), P);
                O->TryGetStringField(TEXT("o"), Obj);
                if (Obj.IsEmpty()) return;
                // 1) 경로 그대로
                if (Obj.StartsWith(TEXT("/Game/")) || Obj.StartsWith(TEXT("/Script/")))
                {
                    AddRef(RefTo, From, ToAssetKeyFromClassPath(Obj));
                    return;
                }
                // 2) "Class.Func" 형태면 Class 추출 → 이름 인덱스로 매핑
                int32 Dot = INDEX_NONE;
                if (Obj.FindChar(TCHAR('.'), Dot))
                {
                    const FString OwnerName = Obj.Left(Dot);
                    if (const FString* Key = NameToAssetKey.Find(OwnerName))
                    {
                        AddRef(RefTo, From, *Key);
                    }
                }
                else
                {
                    // 3) 순수 이름(위젯/BP 변수명 등)도 일치하면 매핑
                    if (const FString* Key = NameToAssetKey.Find(Obj))
                        AddRef(RefTo, From, *Key);
                }
            });
    }

    // 3) 역방향 맵 구성
    TMap<FString, TSet<FString>> RefBy;
    for (const auto& kv : RefTo)
        for (const FString& To : kv.Value)
            RefBy.FindOrAdd(To).Add(kv.Key);

    // 4) 결과 JSON 구성
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("project_name"), FApp::GetProjectName());
    Root->SetStringField(TEXT("analysis_timestamp"), FDateTime::Now().ToIso8601());
    TSharedRef<FJsonObject> JAssets = MakeShared<FJsonObject>();
    for (const FAssetInfo& It : All)
    {
        TSharedRef<FJsonObject> JOne = MakeShared<FJsonObject>();
        JOne->SetStringField(TEXT("asset_type"), It.AssetType);
        // references_to
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            const TSet<FString>* S = RefTo.Find(It.PkgPath);
            if (S) { TArray<FString> V = S->Array(); V.RemoveAll([](const FString& X) { return X.IsEmpty(); }); V.Sort(); for (const FString& T : V) Arr.Add(MakeShared<FJsonValueString>(T)); }
            JOne->SetArrayField(TEXT("references_to"), Arr);
        }
        // referenced_by
        {
            TArray<TSharedPtr<FJsonValue>> Arr;
            const TSet<FString>* S = RefBy.Find(It.PkgPath);
            if (S) { TArray<FString> V = S->Array(); V.RemoveAll([](const FString& X) { return X.IsEmpty(); }); V.Sort(); for (const FString& T : V) Arr.Add(MakeShared<FJsonValueString>(T)); }
            JOne->SetArrayField(TEXT("referenced_by"), Arr);
        }
        JAssets->SetObjectField(*It.PkgPath, JOne);
    }
    Root->SetObjectField(TEXT("assets"), JAssets);

    const FString OutPath = FPaths::Combine(OutRoot, TEXT("project_references.json"));
    WriteJsonToFile(*Root, OutPath);
}


void FBPTextDumpModule::UnregisterMenus()
{
    if (!UObjectInitialized())
    {
        return;
    }
    UToolMenus::UnregisterOwner(this);
}

#pragma once

#include "Modules/ModuleManager.h"

struct IConsoleCommand; // engine defines this as struct
class UBlueprint;       // forward declare for function sigs

class FBPTextDumpModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    void CmdDumpAll(const TArray<FString>& Args);
    void CmdDumpSelected(const TArray<FString>& Args);
    void CmdDumpOne(const TArray<FString>& Args);
    void CmdProjectRefs(const TArray<FString>& Args);
    void UI_BuildProjectRefs();
    void UI_DumpAll();
    void UI_DumpSelected();
    void RegisterMenus();
    void RegisterMenus_Impl();
    void UnregisterMenus();

    IConsoleCommand* DumpAllCmd = nullptr;
    IConsoleCommand* DumpSelCmd = nullptr;
    IConsoleCommand* DumpOneCmd = nullptr;
    IConsoleCommand* ProjectRefsCmd = nullptr;
};
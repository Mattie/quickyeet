#pragma once

#include <windows.h>
#include <shobjidl.h>

#include <atomic>

class QuickYeetExplorerCommand final : public IExplorerCommand {
public:
    QuickYeetExplorerCommand();

    IFACEMETHODIMP QueryInterface(REFIID interface_id, void** object) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    IFACEMETHODIMP GetTitle(IShellItemArray* items, PWSTR* title) override;
    IFACEMETHODIMP GetIcon(IShellItemArray* items, PWSTR* icon) override;
    IFACEMETHODIMP GetToolTip(IShellItemArray* items, PWSTR* tooltip) override;
    IFACEMETHODIMP GetCanonicalName(GUID* canonical_name) override;
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL slow_ok, EXPCMDSTATE* state) override;
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx* bind_context) override;
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override;
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override;

private:
    ~QuickYeetExplorerCommand();

    std::atomic<ULONG> references_{1};
};

#pragma once

#include <windows.h>
#include <shlobj.h>

#include "quickyeet/HistoryStore.h"

#include <atomic>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <vector>

class QuickYeetContextMenu final : public IShellExtInit, public IContextMenu3 {
public:
    QuickYeetContextMenu();

    IFACEMETHODIMP QueryInterface(REFIID interface_id, void** object) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE folder, IDataObject* data_object,
                             HKEY program_id) override;

    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first_command,
                                   UINT last_command, UINT flags) override;
    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* command_info) override;
    IFACEMETHODIMP GetCommandString(UINT_PTR command, UINT type, UINT* reserved,
                                   char* name, UINT name_length) override;
    IFACEMETHODIMP HandleMenuMsg(UINT message, WPARAM w_param, LPARAM l_param) override;
    IFACEMETHODIMP HandleMenuMsg2(UINT message, WPARAM w_param, LPARAM l_param,
                                 LRESULT* result) override;

private:
    ~QuickYeetContextMenu();

    enum class ActionKind {
        move,
        recycle_bin,
        browse,
        new_folder,
        toggle_pin,
        set_alias,
        remove,
        open_config_folder
    };
    struct Action {
        ActionKind kind;
        std::filesystem::path destination;
        bool value = false;
    };
    struct DirectoryMenu {
        std::filesystem::path path;
        bool saved_destination = false;
        bool pinned = false;
        bool populated = false;
    };

    UINT AddAction(HMENU menu, const std::wstring& title, Action action, bool enabled = true);
    void AddDestination(HMENU menu, const quickyeet::DestinationRecord& destination);
    void AddDirectorySubmenu(HMENU menu, const std::filesystem::path& path,
                             const std::wstring& title, bool saved_destination, bool pinned);
    void AddHeader(HMENU menu, const wchar_t* title);
    void AddUnavailable(HMENU menu, const wchar_t* title);
    void PopulateRootMenu(HMENU menu);
    void PopulateDirectoryMenu(HMENU menu, DirectoryMenu node);
    void QueueMoveSelection(HWND owner, const std::filesystem::path& destination);
    void QueueRecycleSelection(HWND owner);
    bool LoadHistoryOrReport(HWND owner);
    void SaveHistoryOrReport(HWND owner);
    HRESULT RunAction(HWND owner, const Action& action);

    std::atomic<ULONG> references_{1};
    UINT first_command_ = 0;
    UINT command_capacity_ = 0;
    std::vector<std::filesystem::path> selected_files_;
    std::vector<Action> actions_;
    std::unordered_map<HMENU, DirectoryMenu> directory_menus_;
    HMENU root_menu_ = nullptr;
    bool root_populated_ = false;
    quickyeet::HistoryStore history_;
};

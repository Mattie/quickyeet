#include "ContextMenu.h"

#include "InputDialog.h"
#include "ShellModule.h"
#include "quickyeet/MoveEngine.h"
#include "quickyeet/Text.h"

#include <shellapi.h>
#include <process.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <new>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
namespace {

constexpr UINT kReservedCommandCount = 2048;
constexpr size_t kMaximumSubdirectories = 32;

std::wstring DestinationTitle(const quickyeet::DestinationRecord& destination) {
    if (!destination.alias.empty()) {
        return destination.alias;
    }
    const std::wstring filename = destination.path.filename().wstring();
    return filename.empty() ? destination.path.wstring() : filename;
}

void InsertSubmenu(HMENU menu, const UINT position, const std::wstring& title, HMENU submenu) {
    MENUITEMINFOW item{sizeof(item)};
    item.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_SUBMENU;
    item.fType = MFT_STRING;
    item.dwTypeData = const_cast<wchar_t*>(title.c_str());
    item.hSubMenu = submenu;
    InsertMenuItemW(menu, position, TRUE, &item);
}

bool IsValidFolderName(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ') {
        return false;
    }
    constexpr wchar_t invalid[] = L"<>:\"/\\|?*";
    return std::none_of(name.begin(), name.end(), [](const wchar_t character) {
        return character < 32 || wcschr(invalid, character) != nullptr;
    });
}

std::vector<fs::path> ChildDirectories(const fs::path& parent, bool* truncated, std::error_code* error) {
    std::vector<fs::path> directories;
    *truncated = false;
    error->clear();
    fs::directory_iterator iterator(parent, fs::directory_options::skip_permission_denied, *error);
    const fs::directory_iterator end;
    while (!*error && iterator != end) {
        std::error_code type_error;
        if (iterator->is_directory(type_error) && !type_error) {
            if (directories.size() == kMaximumSubdirectories) {
                *truncated = true;
                break;
            }
            directories.push_back(iterator->path());
        }
        iterator.increment(*error);
    }
    std::sort(directories.begin(), directories.end(), [](const fs::path& left, const fs::path& right) {
        return _wcsicmp(left.filename().c_str(), right.filename().c_str()) < 0;
    });
    return directories;
}

struct MoveRequest {
    std::vector<fs::path> sources;
    fs::path destination;
    HWND owner = nullptr;
    bool recycle = false;
};

struct ServerLockRelease {
    ~ServerLockRelease() { --g_server_locks; }
};

unsigned __stdcall MoveWorker(void* context) noexcept {
    std::unique_ptr<MoveRequest> request(static_cast<MoveRequest*>(context));
    ServerLockRelease release_lock;
    try {
        if (request->recycle) {
            const unsigned long error = quickyeet::RecycleFiles(request->sources);
            if (error != ERROR_SUCCESS) {
                const std::wstring message =
                    L"QuickYeet could not send the selected files to Recycle Bin:\n\n" +
                    quickyeet::WindowsErrorMessage(error);
                MessageBoxW(request->owner, message.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
            }
            return 0;
        }
        const auto results =
            quickyeet::MoveFilesWithCollisionSuffix(request->sources, request->destination);
        std::wostringstream failures;
        size_t succeeded = 0;
        for (const auto& result : results) {
            if (result.succeeded()) {
                ++succeeded;
            } else {
                failures << L"\n" << result.source.filename().wstring() << L": "
                         << quickyeet::WindowsErrorMessage(result.error_code);
            }
        }

        if (succeeded > 0) {
            quickyeet::HistoryStore history;
            std::wstring history_error;
            if (history.Load(&history_error)) {
                history.RecordUse(request->destination);
                if (!history.Save(&history_error)) {
                    MessageBoxW(request->owner, history_error.c_str(), L"QuickYeet",
                                MB_OK | MB_ICONERROR);
                }
            } else {
                MessageBoxW(request->owner, history_error.c_str(), L"QuickYeet",
                            MB_OK | MB_ICONERROR);
            }
        }
        if (!failures.str().empty()) {
            const std::wstring message =
                L"QuickYeet could not move some selected files:" + failures.str();
            MessageBoxW(request->owner, message.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
        }
    } catch (const std::exception&) {
        MessageBoxW(request->owner, L"QuickYeet could not finish the file operation.", L"QuickYeet",
                    MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(request->owner, L"QuickYeet could not finish the file operation.", L"QuickYeet",
                    MB_OK | MB_ICONERROR);
    }
    return 0;
}

}  // namespace

QuickYeetContextMenu::QuickYeetContextMenu() { ++g_object_count; }

QuickYeetContextMenu::~QuickYeetContextMenu() { --g_object_count; }

HRESULT QuickYeetContextMenu::QueryInterface(REFIID interface_id, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (interface_id == IID_IUnknown || interface_id == IID_IShellExtInit) {
        *object = static_cast<IShellExtInit*>(this);
    } else if (interface_id == IID_IContextMenu || interface_id == IID_IContextMenu2 ||
               interface_id == IID_IContextMenu3) {
        *object = static_cast<IContextMenu3*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG QuickYeetContextMenu::AddRef() { return ++references_; }

ULONG QuickYeetContextMenu::Release() {
    const ULONG remaining = --references_;
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

HRESULT QuickYeetContextMenu::Initialize(PCIDLIST_ABSOLUTE, IDataObject* data_object, HKEY) try {
    selected_files_.clear();
    if (data_object == nullptr) {
        return E_INVALIDARG;
    }

    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    const HRESULT result = data_object->GetData(&format, &medium);
    if (FAILED(result)) {
        return result;
    }

    const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (drop != nullptr) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT index = 0; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0);
            std::wstring path(static_cast<size_t>(length) + 1, L'\0');
            DragQueryFileW(drop, index, path.data(), length + 1);
            path.resize(length);
            selected_files_.emplace_back(std::move(path));
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return selected_files_.empty() ? S_FALSE : S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
} catch (...) {
    return E_FAIL;
}

HRESULT QuickYeetContextMenu::QueryContextMenu(HMENU menu, const UINT index, const UINT first_command,
                                               const UINT last_command, const UINT flags) try {
    if ((flags & CMF_DEFAULTONLY) != 0 || selected_files_.empty() || first_command > last_command) {
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
    }

    first_command_ = first_command;
    command_capacity_ = (std::min)(kReservedCommandCount, last_command - first_command + 1);
    actions_.clear();
    directory_menus_.clear();
    root_menu_ = CreatePopupMenu();
    if (root_menu_ == nullptr) {
        return E_OUTOFMEMORY;
    }
    root_populated_ = false;
    AddUnavailable(root_menu_, L"Loading...");
    InsertSubmenu(menu, index, L"QuickYeet", root_menu_);
    return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, command_capacity_);
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
} catch (...) {
    return E_FAIL;
}

UINT QuickYeetContextMenu::AddAction(HMENU menu, const std::wstring& title, Action action,
                                     const bool enabled) {
    if (actions_.size() >= command_capacity_) {
        return UINT_MAX;
    }
    const UINT offset = static_cast<UINT>(actions_.size());
    actions_.push_back(std::move(action));
    AppendMenuW(menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED),
                static_cast<UINT_PTR>(first_command_) + offset,
                title.c_str());
    return offset;
}

void QuickYeetContextMenu::AddHeader(HMENU menu, const wchar_t* title) {
    AppendMenuW(menu, MF_STRING | MF_DISABLED, static_cast<UINT_PTR>(-1), title);
}

void QuickYeetContextMenu::AddUnavailable(HMENU menu, const wchar_t* title) {
    AppendMenuW(menu, MF_STRING | MF_GRAYED, static_cast<UINT_PTR>(-1), title);
}

void QuickYeetContextMenu::PopulateRootMenu(HMENU menu) {
    while (GetMenuItemCount(menu) > 0) {
        DeleteMenu(menu, 0, MF_BYPOSITION);
    }

    std::wstring load_error;
    const bool loaded = history_.Load(&load_error);
    AddHeader(menu, L"RECENT");
    const auto recent = loaded ? history_.Recent(6) : std::vector<quickyeet::DestinationRecord>{};
    if (!loaded) {
        AddUnavailable(menu, L"(Configuration unavailable)");
    } else if (recent.empty()) {
        AddUnavailable(menu, L"(No recent destinations)");
    } else {
        for (const auto& destination : recent) {
            AddDestination(menu, destination);
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AddHeader(menu, L"PINNED");
    const auto pinned = loaded ? history_.Pinned() : std::vector<quickyeet::DestinationRecord>{};
    if (!loaded) {
        AddUnavailable(menu, L"(Configuration unavailable)");
    } else if (pinned.empty()) {
        AddUnavailable(menu, L"(No pinned destinations)");
    } else {
        for (const auto& destination : pinned) {
            AddDestination(menu, destination);
        }
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (history_.RecycleBinEnabled()) {
        AddAction(menu, L"Recycle Bin", {ActionKind::recycle_bin, {}});
    }
    AddAction(menu, L"Browse...", {ActionKind::browse, {}});
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AddAction(menu, L"Open Config Folder", {ActionKind::open_config_folder, {}});
}

void QuickYeetContextMenu::AddDestination(HMENU menu,
                                          const quickyeet::DestinationRecord& destination) {
    AddDirectorySubmenu(menu, destination.path, DestinationTitle(destination), true, destination.pinned);
}

void QuickYeetContextMenu::AddDirectorySubmenu(HMENU menu, const fs::path& path,
                                               const std::wstring& title,
                                               const bool saved_destination, const bool pinned) {
    HMENU submenu = CreatePopupMenu();
    AddUnavailable(submenu, L"Loading...");
    directory_menus_.emplace(submenu, DirectoryMenu{path, saved_destination, pinned, false});
    InsertSubmenu(menu, GetMenuItemCount(menu), title, submenu);
}

void QuickYeetContextMenu::PopulateDirectoryMenu(HMENU menu, DirectoryMenu node) {
    while (GetMenuItemCount(menu) > 0) {
        DeleteMenu(menu, 0, MF_BYPOSITION);
    }

    const DWORD attributes = GetFileAttributesW(node.path.c_str());
    const bool available = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    AddAction(menu, L"Move here", {ActionKind::move, node.path}, available);

    bool truncated = false;
    std::error_code enumeration_error;
    if (available) {
        const auto children = ChildDirectories(node.path, &truncated, &enumeration_error);
        if (!children.empty()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            for (const auto& child : children) {
                AddDirectorySubmenu(menu, child, child.filename().wstring(), false, false);
            }
            if (truncated) {
                AddUnavailable(menu, L"(More folders available through Browse...)");
            }
        }
    } else {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AddUnavailable(menu, L"(Folder unavailable)");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AddAction(menu, L"New Folder...", {ActionKind::new_folder, node.path}, available);
    if (node.saved_destination) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AddAction(menu, node.pinned ? L"Unpin destination" : L"Pin destination",
                  {ActionKind::toggle_pin, node.path, !node.pinned});
        AddAction(menu, L"Set alias...", {ActionKind::set_alias, node.path});
        AddAction(menu, L"Remove from list", {ActionKind::remove, node.path});
    }
}

HRESULT QuickYeetContextMenu::InvokeCommand(CMINVOKECOMMANDINFO* command_info) try {
    if (command_info == nullptr || HIWORD(command_info->lpVerb) != 0) {
        return E_INVALIDARG;
    }
    const UINT offset = LOWORD(command_info->lpVerb);
    if (offset >= actions_.size()) {
        return E_INVALIDARG;
    }
    return RunAction(command_info->hwnd, actions_[offset]);
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
} catch (...) {
    return E_FAIL;
}

HRESULT QuickYeetContextMenu::RunAction(HWND owner, const Action& action) {
    switch (action.kind) {
        case ActionKind::move:
            QueueMoveSelection(owner, action.destination);
            return S_OK;
        case ActionKind::recycle_bin:
            QueueRecycleSelection(owner);
            return S_OK;
        case ActionKind::browse: {
            const auto destination = BrowseForDestination(owner);
            if (destination) {
                QueueMoveSelection(owner, *destination);
            }
            return S_OK;
        }
        case ActionKind::new_folder: {
            const auto name = ShowTextPrompt(owner, L"New Folder", L"Folder name:");
            if (!name) {
                return S_OK;
            }
            if (!IsValidFolderName(*name)) {
                MessageBoxW(owner, L"Enter a Windows folder name without reserved characters or a trailing dot or space.",
                            L"QuickYeet", MB_OK | MB_ICONWARNING);
                return S_OK;
            }
            const fs::path new_folder = action.destination / *name;
            if (!CreateDirectoryW(new_folder.c_str(), nullptr)) {
                MessageBoxW(owner,
                            (L"QuickYeet could not create the folder:\n\n" +
                             quickyeet::WindowsErrorMessage(GetLastError()))
                                .c_str(),
                            L"QuickYeet", MB_OK | MB_ICONERROR);
                return S_OK;
            }
            QueueMoveSelection(owner, new_folder);
            return S_OK;
        }
        case ActionKind::toggle_pin:
            if (!LoadHistoryOrReport(owner)) {
                return S_OK;
            }
            history_.SetPinned(action.destination, action.value);
            SaveHistoryOrReport(owner);
            return S_OK;
        case ActionKind::set_alias: {
            if (!LoadHistoryOrReport(owner)) {
                return S_OK;
            }
            const auto record = history_.Find(action.destination);
            const std::wstring current = record ? record->alias : L"";
            const auto alias = ShowTextPrompt(owner, L"Destination Alias",
                                              L"Alias (leave blank to use the folder name):", current);
            if (alias) {
                history_.SetAlias(action.destination, *alias);
                SaveHistoryOrReport(owner);
            }
            return S_OK;
        }
        case ActionKind::remove:
            if (!LoadHistoryOrReport(owner)) {
                return S_OK;
            }
            history_.Remove(action.destination);
            SaveHistoryOrReport(owner);
            return S_OK;
        case ActionKind::open_config_folder: {
            std::wstring error;
            if (!quickyeet::OpenConfigFolder(&error)) {
                MessageBoxW(owner, error.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
            }
            return S_OK;
        }
    }
    return E_UNEXPECTED;
}

void QuickYeetContextMenu::QueueMoveSelection(HWND owner, const fs::path& destination) {
    auto request = std::make_unique<MoveRequest>();
    request->sources = selected_files_;
    request->destination = destination;
    request->owner = owner;

    ++g_server_locks;
    const uintptr_t thread = _beginthreadex(nullptr, 0, MoveWorker, request.get(), 0, nullptr);
    if (thread == 0) {
        --g_server_locks;
        MessageBoxW(owner, L"QuickYeet could not start the file move.", L"QuickYeet",
                    MB_OK | MB_ICONERROR);
        return;
    }
    request.release();
    CloseHandle(reinterpret_cast<HANDLE>(thread));
}

void QuickYeetContextMenu::QueueRecycleSelection(HWND owner) {
    auto request = std::make_unique<MoveRequest>();
    request->sources = selected_files_;
    request->owner = owner;
    request->recycle = true;

    ++g_server_locks;
    const uintptr_t thread = _beginthreadex(nullptr, 0, MoveWorker, request.get(), 0, nullptr);
    if (thread == 0) {
        --g_server_locks;
        MessageBoxW(owner, L"QuickYeet could not start the Recycle Bin operation.", L"QuickYeet",
                    MB_OK | MB_ICONERROR);
        return;
    }
    request.release();
    CloseHandle(reinterpret_cast<HANDLE>(thread));
}

bool QuickYeetContextMenu::LoadHistoryOrReport(HWND owner) {
    std::wstring error;
    if (history_.Load(&error)) {
        return true;
    }
    MessageBoxW(owner, error.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
    return false;
}

void QuickYeetContextMenu::SaveHistoryOrReport(HWND owner) {
    std::wstring error;
    if (!history_.Save(&error)) {
        MessageBoxW(owner, error.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
    }
}

HRESULT QuickYeetContextMenu::GetCommandString(const UINT_PTR command, const UINT type, UINT*,
                                               char* name, const UINT name_length) try {
    if (command >= actions_.size() || name == nullptr || name_length == 0) {
        return E_INVALIDARG;
    }
    const Action& action = actions_[command];
    std::wstring help = L"Use QuickYeet";
    if (action.kind == ActionKind::move) {
        help = L"Move the selected file to " + action.destination.wstring();
    } else if (action.kind == ActionKind::recycle_bin) {
        help = L"Send the selected file to Recycle Bin";
    } else if (action.kind == ActionKind::browse) {
        help = L"Choose another destination and move the selected file";
    } else if (action.kind == ActionKind::open_config_folder) {
        help = L"Open QuickYeet's local configuration folder";
    }

    if (type == GCS_HELPTEXTW) {
        wcsncpy_s(reinterpret_cast<wchar_t*>(name), name_length, help.c_str(), _TRUNCATE);
        return S_OK;
    }
    if (type == GCS_HELPTEXTA) {
        const std::string utf8 = quickyeet::ToUtf8(help);
        strncpy_s(name, name_length, utf8.c_str(), _TRUNCATE);
        return S_OK;
    }
    return E_NOTIMPL;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
} catch (...) {
    return E_FAIL;
}

HRESULT QuickYeetContextMenu::HandleMenuMsg(const UINT message, const WPARAM w_param,
                                            const LPARAM l_param) {
    return HandleMenuMsg2(message, w_param, l_param, nullptr);
}

HRESULT QuickYeetContextMenu::HandleMenuMsg2(const UINT message, const WPARAM w_param,
                                             const LPARAM, LRESULT* result) try {
    if (result != nullptr) {
        *result = 0;
    }
    if (message == WM_INITMENUPOPUP) {
        const HMENU menu = reinterpret_cast<HMENU>(w_param);
        if (menu == root_menu_ && !root_populated_) {
            PopulateRootMenu(menu);
            root_populated_ = true;
            return S_OK;
        }
        const auto node = directory_menus_.find(menu);
        if (node != directory_menus_.end() && !node->second.populated) {
            DirectoryMenu snapshot = node->second;
            PopulateDirectoryMenu(menu, std::move(snapshot));
            const auto populated_node = directory_menus_.find(menu);
            if (populated_node != directory_menus_.end()) {
                populated_node->second.populated = true;
            }
        }
    }
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
} catch (...) {
    return E_FAIL;
}

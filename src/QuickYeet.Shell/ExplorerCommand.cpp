#include "ExplorerCommand.h"

#include "InputDialog.h"
#include "ShellModule.h"
#include "quickyeet/HistoryStore.h"
#include "quickyeet/MoveEngine.h"
#include "quickyeet/Text.h"

#include <process.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <filesystem>
#include <memory>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace {

constexpr size_t kRecentDestinationCount = 6;

enum class ModernActionKind {
    move,
    recycle_bin,
    browse,
    manage_destinations,
    heading,
    unavailable,
    separator
};

std::wstring DestinationTitle(const quickyeet::DestinationRecord& destination) {
    if (!destination.alias.empty()) {
        return destination.alias;
    }
    const std::wstring filename = destination.path.filename().wstring();
    return filename.empty() ? destination.path.wstring() : filename;
}

std::wstring QuoteCommandLineArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

fs::path PopupExecutablePath() {
    std::wstring module_path(32'768, L'\0');
    const DWORD length = GetModuleFileNameW(g_module_instance, module_path.data(),
                                            static_cast<DWORD>(module_path.size()));
    if (length == 0 || length == module_path.size()) {
        return {};
    }
    module_path.resize(length);
    return fs::path(module_path).parent_path() / L"QuickYeetPopup.exe";
}

std::vector<fs::path> SelectedFilePaths(IShellItemArray* items) {
    std::vector<fs::path> paths;
    if (items == nullptr) {
        return paths;
    }

    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) {
        return paths;
    }
    paths.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(index, &item))) {
            continue;
        }
        SFGAOF attributes = 0;
        const HRESULT attributes_result = item->GetAttributes(SFGAO_FOLDER, &attributes);
        PWSTR path = nullptr;
        if (SUCCEEDED(attributes_result) && (attributes & SFGAO_FOLDER) == 0 &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.emplace_back(path);
            CoTaskMemFree(path);
        }
        item->Release();
    }
    return paths;
}

struct MoveRequest {
    std::vector<fs::path> sources;
    fs::path destination;
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
                MessageBoxW(nullptr, message.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
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
                    MessageBoxW(nullptr, history_error.c_str(), L"QuickYeet",
                                MB_OK | MB_ICONERROR);
                }
            } else {
                MessageBoxW(nullptr, history_error.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
            }
        }
        if (!failures.str().empty()) {
            const std::wstring message =
                L"QuickYeet could not move some selected files:" + failures.str();
            MessageBoxW(nullptr, message.c_str(), L"QuickYeet", MB_OK | MB_ICONERROR);
        }
    } catch (const std::exception&) {
        MessageBoxW(nullptr, L"QuickYeet could not finish the file operation.", L"QuickYeet",
                    MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"QuickYeet could not finish the file operation.", L"QuickYeet",
                    MB_OK | MB_ICONERROR);
    }
    return 0;
}

HRESULT QueueFileOperation(const std::vector<fs::path>& sources, const fs::path& destination,
                           const bool recycle) {
    auto request = std::make_unique<MoveRequest>();
    request->sources = sources;
    request->destination = destination;
    request->recycle = recycle;

    ++g_server_locks;
    const uintptr_t thread = _beginthreadex(nullptr, 0, MoveWorker, request.get(), 0, nullptr);
    if (thread == 0) {
        --g_server_locks;
        return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    }
    request.release();
    CloseHandle(reinterpret_cast<HANDLE>(thread));
    return S_OK;
}

HRESULT LaunchPopup(const std::vector<fs::path>& paths) {
    const fs::path executable = PopupExecutablePath();
    if (executable.empty() || GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    }

    std::wstring command_line = QuoteCommandLineArgument(executable.wstring());
    for (const auto& path : paths) {
        command_line.push_back(L' ');
        command_line += QuoteCommandLineArgument(path.wstring());
    }

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        executable.parent_path().c_str(), &startup, &process)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return S_OK;
}

class QuickYeetSubcommand final : public IExplorerCommand {
public:
    QuickYeetSubcommand(std::wstring title, const ModernActionKind action,
                        fs::path destination = {})
        : title_(std::move(title)), action_(action), destination_(std::move(destination)) {
        ++g_object_count;
    }

    IFACEMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_id != IID_IUnknown && interface_id != IID_IExplorerCommand) {
            return E_NOINTERFACE;
        }
        *object = static_cast<IExplorerCommand*>(this);
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++references_; }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    IFACEMETHODIMP GetTitle(IShellItemArray*, PWSTR* title) override {
        if (title == nullptr) {
            return E_POINTER;
        }
        if (action_ == ModernActionKind::separator) {
            *title = nullptr;
            return S_FALSE;
        }
        return SHStrDupW(title_.c_str(), title);
    }

    IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* icon) override {
        if (icon == nullptr) {
            return E_POINTER;
        }
        *icon = nullptr;
        return E_NOTIMPL;
    }

    IFACEMETHODIMP GetToolTip(IShellItemArray*, PWSTR* tooltip) override {
        if (tooltip == nullptr) {
            return E_POINTER;
        }
        switch (action_) {
            case ModernActionKind::move:
                return SHStrDupW((L"Move the selected files to " + destination_.wstring()).c_str(),
                                 tooltip);
            case ModernActionKind::recycle_bin:
                return SHStrDupW(L"Send the selected files to Recycle Bin", tooltip);
            case ModernActionKind::browse:
                return SHStrDupW(L"Choose another destination", tooltip);
            case ModernActionKind::manage_destinations:
                return SHStrDupW(L"Browse and manage QuickYeet destinations", tooltip);
            default:
                *tooltip = nullptr;
                return E_NOTIMPL;
        }
    }

    IFACEMETHODIMP GetCanonicalName(GUID* canonical_name) override {
        if (canonical_name == nullptr) {
            return E_POINTER;
        }
        *canonical_name = GUID_NULL;
        return S_OK;
    }

    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
        if (state == nullptr) {
            return E_POINTER;
        }
        if (action_ == ModernActionKind::heading || action_ == ModernActionKind::unavailable) {
            *state = ECS_DISABLED;
            return S_OK;
        }
        if (action_ == ModernActionKind::separator) {
            *state = ECS_ENABLED;
            return S_OK;
        }
        DWORD count = 0;
        *state = items != nullptr && SUCCEEDED(items->GetCount(&count)) && count > 0
                     ? ECS_ENABLED
                     : ECS_HIDDEN;
        return S_OK;
    }

    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override try {
        if (action_ == ModernActionKind::heading || action_ == ModernActionKind::unavailable ||
            action_ == ModernActionKind::separator) {
            return E_NOTIMPL;
        }

        const auto paths = SelectedFilePaths(items);
        if (paths.empty()) {
            return E_INVALIDARG;
        }
        switch (action_) {
            case ModernActionKind::move:
                return QueueFileOperation(paths, destination_, false);
            case ModernActionKind::recycle_bin:
                return QueueFileOperation(paths, {}, true);
            case ModernActionKind::browse: {
                const auto destination = BrowseForDestination(nullptr);
                return destination ? QueueFileOperation(paths, *destination, false) : S_OK;
            }
            case ModernActionKind::manage_destinations:
                return LaunchPopup(paths);
            default:
                return E_NOTIMPL;
        }
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }

    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
        if (flags == nullptr) {
            return E_POINTER;
        }
        *flags = action_ == ModernActionKind::separator ? ECF_ISSEPARATOR : ECF_DEFAULT;
        return S_OK;
    }

    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
        if (commands == nullptr) {
            return E_POINTER;
        }
        *commands = nullptr;
        return E_NOTIMPL;
    }

private:
    ~QuickYeetSubcommand() { --g_object_count; }

    std::atomic<ULONG> references_{1};
    std::wstring title_;
    ModernActionKind action_;
    fs::path destination_;
};

using ExplorerCommandPointer = Microsoft::WRL::ComPtr<IExplorerCommand>;
using ExplorerCommandList = std::vector<ExplorerCommandPointer>;

class QuickYeetCommandEnumerator final : public IEnumExplorerCommand {
public:
    explicit QuickYeetCommandEnumerator(ExplorerCommandList commands, const size_t index = 0)
        : commands_(std::move(commands)), index_(index) {
        ++g_object_count;
    }

    IFACEMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_id != IID_IUnknown && interface_id != IID_IEnumExplorerCommand) {
            return E_NOINTERFACE;
        }
        *object = static_cast<IEnumExplorerCommand*>(this);
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++references_; }

    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    IFACEMETHODIMP Next(ULONG requested, IExplorerCommand** commands, ULONG* fetched) override {
        if (commands == nullptr || (requested != 1 && fetched == nullptr)) {
            return E_POINTER;
        }
        if (fetched != nullptr) {
            *fetched = 0;
        }

        ULONG produced = 0;
        while (produced < requested && index_ < commands_.size()) {
            commands_[index_].CopyTo(&commands[produced]);
            ++produced;
            ++index_;
        }
        if (fetched != nullptr) {
            *fetched = produced;
        }
        return produced == requested ? S_OK : S_FALSE;
    }

    IFACEMETHODIMP Skip(const ULONG requested) override {
        const size_t remaining = commands_.size() - index_;
        if (static_cast<size_t>(requested) > remaining) {
            index_ = commands_.size();
            return S_FALSE;
        }
        index_ += static_cast<size_t>(requested);
        return S_OK;
    }

    IFACEMETHODIMP Reset() override {
        index_ = 0;
        return S_OK;
    }

    IFACEMETHODIMP Clone(IEnumExplorerCommand** enumerator) override try {
        if (enumerator == nullptr) {
            return E_POINTER;
        }
        *enumerator = nullptr;
        auto* copy = new (std::nothrow) QuickYeetCommandEnumerator(commands_, index_);
        if (copy == nullptr) {
            return E_OUTOFMEMORY;
        }
        *enumerator = copy;
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }

private:
    ~QuickYeetCommandEnumerator() { --g_object_count; }

    std::atomic<ULONG> references_{1};
    ExplorerCommandList commands_;
    size_t index_ = 0;
};

void AddSubcommand(ExplorerCommandList* commands, std::wstring title,
                   const ModernActionKind action, fs::path destination = {}) {
    ExplorerCommandPointer command;
    command.Attach(new (std::nothrow)
                       QuickYeetSubcommand(std::move(title), action, std::move(destination)));
    if (command == nullptr) {
        throw std::bad_alloc();
    }
    commands->push_back(std::move(command));
}

void AddDestinationGroup(ExplorerCommandList* commands, const wchar_t* heading,
                         const wchar_t* empty_message,
                         const std::vector<quickyeet::DestinationRecord>& destinations) {
    AddSubcommand(commands, heading, ModernActionKind::heading);
    if (destinations.empty()) {
        AddSubcommand(commands, empty_message, ModernActionKind::unavailable);
        return;
    }
    for (const auto& destination : destinations) {
        AddSubcommand(commands, DestinationTitle(destination), ModernActionKind::move,
                      destination.path);
    }
}

ExplorerCommandList BuildSubcommands() {
    ExplorerCommandList commands;
    quickyeet::HistoryStore history;
    std::wstring load_error;
    const bool loaded = history.Load(&load_error);

    if (loaded) {
        AddDestinationGroup(&commands, L"RECENT", L"(No recent destinations)",
                            history.Recent(kRecentDestinationCount));
        AddSubcommand(&commands, L"", ModernActionKind::separator);
        AddDestinationGroup(&commands, L"PINNED", L"(No pinned destinations)", history.Pinned());
    } else {
        AddSubcommand(&commands, L"Configuration unavailable", ModernActionKind::unavailable);
    }

    AddSubcommand(&commands, L"", ModernActionKind::separator);
    if (loaded && history.RecycleBinEnabled()) {
        AddSubcommand(&commands, L"Recycle Bin", ModernActionKind::recycle_bin);
    }
    AddSubcommand(&commands, L"Browse...", ModernActionKind::browse);
    AddSubcommand(&commands, L"Manage Destinations...", ModernActionKind::manage_destinations);
    return commands;
}

}  // namespace

QuickYeetExplorerCommand::QuickYeetExplorerCommand() { ++g_object_count; }

QuickYeetExplorerCommand::~QuickYeetExplorerCommand() { --g_object_count; }

HRESULT QuickYeetExplorerCommand::QueryInterface(REFIID interface_id, void** object) {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (interface_id != IID_IUnknown && interface_id != IID_IExplorerCommand) {
        return E_NOINTERFACE;
    }
    *object = static_cast<IExplorerCommand*>(this);
    AddRef();
    return S_OK;
}

ULONG QuickYeetExplorerCommand::AddRef() { return ++references_; }

ULONG QuickYeetExplorerCommand::Release() {
    const ULONG remaining = --references_;
    if (remaining == 0) {
        delete this;
    }
    return remaining;
}

HRESULT QuickYeetExplorerCommand::GetTitle(IShellItemArray*, PWSTR* title) {
    return title == nullptr ? E_POINTER : SHStrDupW(L"QuickYeet", title);
}

HRESULT QuickYeetExplorerCommand::GetIcon(IShellItemArray*, PWSTR* icon) {
    if (icon == nullptr) {
        return E_POINTER;
    }
    *icon = nullptr;
    return E_NOTIMPL;
}

HRESULT QuickYeetExplorerCommand::GetToolTip(IShellItemArray*, PWSTR* tooltip) {
    return tooltip == nullptr ? E_POINTER
                              : SHStrDupW(L"Move the selected files with QuickYeet", tooltip);
}

HRESULT QuickYeetExplorerCommand::GetCanonicalName(GUID* canonical_name) {
    if (canonical_name == nullptr) {
        return E_POINTER;
    }
    *canonical_name = CLSID_QuickYeetModern;
    return S_OK;
}

HRESULT QuickYeetExplorerCommand::GetState(IShellItemArray* items, BOOL,
                                           EXPCMDSTATE* state) {
    if (state == nullptr) {
        return E_POINTER;
    }
    DWORD count = 0;
    *state = items != nullptr && SUCCEEDED(items->GetCount(&count)) && count > 0
                 ? ECS_ENABLED
                 : ECS_HIDDEN;
    return S_OK;
}

HRESULT QuickYeetExplorerCommand::Invoke(IShellItemArray*, IBindCtx*) { return E_NOTIMPL; }

HRESULT QuickYeetExplorerCommand::GetFlags(EXPCMDFLAGS* flags) {
    if (flags == nullptr) {
        return E_POINTER;
    }
    *flags = ECF_HASSUBCOMMANDS;
    return S_OK;
}

HRESULT QuickYeetExplorerCommand::EnumSubCommands(IEnumExplorerCommand** commands) try {
    if (commands == nullptr) {
        return E_POINTER;
    }
    *commands = nullptr;

    auto* enumerator = new (std::nothrow) QuickYeetCommandEnumerator(BuildSubcommands());
    if (enumerator == nullptr) {
        return E_OUTOFMEMORY;
    }
    *commands = enumerator;
    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
} catch (...) {
    return E_FAIL;
}

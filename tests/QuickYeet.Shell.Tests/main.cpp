#include "ShellModule.h"

#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

using DllCanUnloadNowFunction = HRESULT(STDAPICALLTYPE*)();
using DllGetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kTestConfigPathEnvironmentVariable[] = L"QUICKYEET_TEST_CONFIG_PATH";
constexpr wchar_t kIsolatedDestinationTitle[] = L"QuickYeet isolated contract destination";

std::optional<std::wstring> ReadEnvironmentVariable(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }

    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    return value;
}

class ScopedTestConfiguration {
public:
    ~ScopedTestConfiguration() {
        if (environment_set_) {
            SetEnvironmentVariableW(kTestConfigPathEnvironmentVariable,
                                    previous_value_ ? previous_value_->c_str() : nullptr);
        }
        std::error_code ignored;
        fs::remove_all(directory_, ignored);
    }

    bool Initialize() {
        wchar_t temporary_root[MAX_PATH]{};
        const DWORD root_length = GetTempPathW(ARRAYSIZE(temporary_root), temporary_root);
        if (root_length == 0 || root_length >= ARRAYSIZE(temporary_root)) {
            return false;
        }

        wchar_t unique_path[MAX_PATH]{};
        if (GetTempFileNameW(temporary_root, L"QYT", 0, unique_path) == 0) {
            return false;
        }
        directory_ = unique_path;
        if (!DeleteFileW(directory_.c_str())) {
            return false;
        }

        std::error_code directory_error;
        if (!fs::create_directory(directory_, directory_error) || directory_error) {
            return false;
        }

        const fs::path config_path = directory_ / L"config.yaml";
        std::ofstream output(config_path, std::ios::binary);
        output << "version: 1\n"
                  "show_recycle_bin: false\n"
                  "destinations:\n"
                  "  - path: 'C:\\QuickYeet-shell-contract-destination'\n"
                  "    display_name: 'QuickYeet isolated contract destination'\n"
                  "    pinned: true\n"
                  "    use_count: 1\n"
                  "    last_used: 1\n";
        output.close();
        if (!output) {
            return false;
        }

        previous_value_ = ReadEnvironmentVariable(kTestConfigPathEnvironmentVariable);
        if (!SetEnvironmentVariableW(kTestConfigPathEnvironmentVariable,
                                     config_path.c_str())) {
            return false;
        }
        environment_set_ = true;
        return true;
    }

private:
    fs::path directory_;
    std::optional<std::wstring> previous_value_;
    bool environment_set_ = false;
};

class CountOnlySelection final : public IShellItemArray {
public:
    IFACEMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_id != IID_IUnknown && interface_id != IID_IShellItemArray) {
            return E_NOINTERFACE;
        }
        *object = static_cast<IShellItemArray*>(this);
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    IFACEMETHODIMP_(ULONG) Release() override { return --references_; }

    IFACEMETHODIMP BindToHandler(IBindCtx*, REFGUID, REFIID, void**) override {
        details_requested_ = true;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetPropertyStore(GETPROPERTYSTOREFLAGS, REFIID, void**) override {
        details_requested_ = true;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetPropertyDescriptionList(REFPROPERTYKEY, REFIID, void**) override {
        details_requested_ = true;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetAttributes(SIATTRIBFLAGS, SFGAOF, SFGAOF*) override {
        details_requested_ = true;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP GetCount(DWORD* count) override {
        if (count == nullptr) {
            return E_POINTER;
        }
        *count = 1;
        return S_OK;
    }
    IFACEMETHODIMP GetItemAt(DWORD, IShellItem**) override {
        details_requested_ = true;
        return E_NOTIMPL;
    }
    IFACEMETHODIMP EnumItems(IEnumShellItems**) override {
        details_requested_ = true;
        return E_NOTIMPL;
    }

    [[nodiscard]] bool DetailsRequested() const noexcept { return details_requested_; }

private:
    ULONG references_ = 1;
    bool details_requested_ = false;
};

class FileDropDataObject final : public IDataObject {
public:
    explicit FileDropDataObject(std::wstring path) : path_(std::move(path)) {}

    IFACEMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_id != IID_IUnknown && interface_id != IID_IDataObject) {
            return E_NOINTERFACE;
        }
        *object = static_cast<IDataObject*>(this);
        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    IFACEMETHODIMP_(ULONG) Release() override { return --references_; }

    IFACEMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (format == nullptr || medium == nullptr) {
            return E_POINTER;
        }
        if (format->cfFormat != CF_HDROP || (format->tymed & TYMED_HGLOBAL) == 0) {
            return DV_E_FORMATETC;
        }

        const SIZE_T path_bytes = (path_.size() + 2) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + path_bytes);
        if (memory == nullptr) {
            return E_OUTOFMEMORY;
        }
        auto* drop = static_cast<DROPFILES*>(GlobalLock(memory));
        if (drop == nullptr) {
            GlobalFree(memory);
            return E_OUTOFMEMORY;
        }
        drop->pFiles = sizeof(DROPFILES);
        drop->fWide = TRUE;
        auto* destination = reinterpret_cast<wchar_t*>(
            reinterpret_cast<unsigned char*>(drop) + sizeof(DROPFILES));
        CopyMemory(destination, path_.c_str(), (path_.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(memory);

        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = memory;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    IFACEMETHODIMP QueryGetData(FORMATETC* format) override {
        return format != nullptr && format->cfFormat == CF_HDROP &&
                       (format->tymed & TYMED_HGLOBAL) != 0
                   ? S_OK
                   : DV_E_FORMATETC;
    }
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* output) override {
        if (output != nullptr) {
            output->ptd = nullptr;
        }
        return E_NOTIMPL;
    }
    IFACEMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumFormatEtc(DWORD, IEnumFORMATETC**) override { return E_NOTIMPL; }
    IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    ULONG references_ = 1;
    std::wstring path_;
};

bool MenuContains(HMENU menu, const wchar_t* expected) {
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        wchar_t label[128]{};
        GetMenuStringW(menu, static_cast<UINT>(index), label, ARRAYSIZE(label), MF_BYPOSITION);
        if (wcscmp(label, expected) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

int wmain(const int argument_count, wchar_t** arguments) {
    if (argument_count != 2) {
        std::cerr << "FAIL  pass the QuickYeet shell DLL path\n";
        return 1;
    }

    ScopedTestConfiguration isolated_configuration;
    if (!isolated_configuration.Initialize()) {
        std::cerr << "FAIL  the isolated shell test configuration could not be created\n";
        return 1;
    }

    const HMODULE module = LoadLibraryW(arguments[1]);
    if (module == nullptr) {
        std::cerr << "FAIL  the shell DLL could not be loaded\n";
        return 1;
    }

    const auto can_unload = reinterpret_cast<DllCanUnloadNowFunction>(
        GetProcAddress(module, "DllCanUnloadNow"));
    const auto get_class_object = reinterpret_cast<DllGetClassObjectFunction>(
        GetProcAddress(module, "DllGetClassObject"));
    if (can_unload == nullptr || get_class_object == nullptr) {
        std::cerr << "FAIL  required COM exports are missing\n";
        FreeLibrary(module);
        return 1;
    }

    IClassFactory* factory = nullptr;
    HRESULT result = get_class_object(CLSID_QuickYeet, IID_PPV_ARGS(&factory));
    if (FAILED(result) || factory == nullptr) {
        std::cerr << "FAIL  the COM class factory could not be created\n";
        FreeLibrary(module);
        return 1;
    }

    IShellExtInit* extension = nullptr;
    result = factory->CreateInstance(nullptr, IID_PPV_ARGS(&extension));
    factory->Release();
    if (FAILED(result) || extension == nullptr) {
        std::cerr << "FAIL  the Explorer extension could not be created\n";
        FreeLibrary(module);
        return 1;
    }

    IContextMenu3* context_menu = nullptr;
    result = extension->QueryInterface(IID_PPV_ARGS(&context_menu));
    if (FAILED(result) || context_menu == nullptr) {
        std::cerr << "FAIL  the extension does not expose IContextMenu3\n";
        extension->Release();
        FreeLibrary(module);
        return 1;
    }

    FileDropDataObject selected_file(L"C:\\QuickYeet-shell-contract-test.txt");
    result = extension->Initialize(nullptr, &selected_file, nullptr);
    extension->Release();
    HMENU root_menu = CreatePopupMenu();
    const HRESULT menu_result = SUCCEEDED(result)
                                    ? context_menu->QueryContextMenu(root_menu, 0, 1, 2048, CMF_NORMAL)
                                    : result;
    HMENU quickyeet_menu = GetSubMenu(root_menu, 0);
    const bool menu_is_lazy = SUCCEEDED(menu_result) && quickyeet_menu != nullptr &&
                              GetMenuItemCount(quickyeet_menu) == 1 &&
                              MenuContains(quickyeet_menu, L"Loading...");
    LRESULT menu_message_result = 0;
    const HRESULT populate_result =
        menu_is_lazy
            ? context_menu->HandleMenuMsg2(WM_INITMENUPOPUP,
                                           reinterpret_cast<WPARAM>(quickyeet_menu), 0,
                                           &menu_message_result)
            : E_FAIL;
    const bool classic_contract_ok = SUCCEEDED(populate_result) &&
                                     !MenuContains(quickyeet_menu, L"Recycle Bin") &&
                                     MenuContains(quickyeet_menu,
                                                  kIsolatedDestinationTitle) &&
                                     MenuContains(quickyeet_menu, L"Browse...") &&
                                     MenuContains(quickyeet_menu, L"Open Config Folder");
    DestroyMenu(root_menu);
    context_menu->Release();
    if (!classic_contract_ok) {
        std::cerr << "FAIL  the classic menu contract is incomplete\n";
        FreeLibrary(module);
        return 1;
    }

    factory = nullptr;
    result = get_class_object(CLSID_QuickYeetModern, IID_PPV_ARGS(&factory));
    if (FAILED(result) || factory == nullptr) {
        std::cerr << "FAIL  the modern COM class factory could not be created\n";
        FreeLibrary(module);
        return 1;
    }

    IExplorerCommand* explorer_command = nullptr;
    result = factory->CreateInstance(nullptr, IID_PPV_ARGS(&explorer_command));
    factory->Release();
    if (FAILED(result) || explorer_command == nullptr) {
        std::cerr << "FAIL  the Windows 11 Explorer command could not be created\n";
        FreeLibrary(module);
        return 1;
    }

    PWSTR title = nullptr;
    EXPCMDFLAGS flags = ECF_DEFAULT;
    EXPCMDSTATE selection_state = ECS_HIDDEN;
    CountOnlySelection selection;
    result = explorer_command->GetTitle(nullptr, &title);
    const HRESULT flags_result = explorer_command->GetFlags(&flags);
    const HRESULT state_result = explorer_command->GetState(&selection, FALSE, &selection_state);
    IEnumExplorerCommand* subcommands = nullptr;
    const HRESULT enumeration_result = explorer_command->EnumSubCommands(&subcommands);

    bool saw_recent = false;
    bool saw_pinned = false;
    bool saw_separator = false;
    bool saw_browse = false;
    bool saw_manage_destinations = false;
    bool saw_recycle_bin = false;
    bool saw_isolated_destination = false;
    bool child_contract_ok = SUCCEEDED(enumeration_result) && subcommands != nullptr;
    if (child_contract_ok) {
        IEnumExplorerCommand* clone = nullptr;
        child_contract_ok = SUCCEEDED(subcommands->Clone(&clone)) && clone != nullptr;
        if (clone != nullptr) {
            clone->Release();
        }

        IExplorerCommand* child = nullptr;
        ULONG fetched = 0;
        HRESULT next_result = S_OK;
        while (child_contract_ok &&
               (next_result = subcommands->Next(1, &child, &fetched)) == S_OK) {
            PWSTR child_title = nullptr;
            EXPCMDFLAGS child_flags = ECF_DEFAULT;
            EXPCMDSTATE child_state = ECS_HIDDEN;
            const HRESULT child_title_result = child->GetTitle(nullptr, &child_title);
            const HRESULT child_flags_result = child->GetFlags(&child_flags);
            const HRESULT child_state_result = child->GetState(&selection, FALSE, &child_state);
            const bool child_is_separator = (child_flags & ECF_ISSEPARATOR) != 0;
            const bool child_title_ok = child_is_separator
                                            ? child_title_result == S_FALSE && child_title == nullptr
                                            : SUCCEEDED(child_title_result) && child_title != nullptr;
            child_contract_ok = SUCCEEDED(child_flags_result) && child_title_ok &&
                                (child_flags & ECF_HASSUBCOMMANDS) == 0 &&
                                SUCCEEDED(child_state_result);
            if (child_contract_ok) {
                saw_separator = saw_separator || child_is_separator;
                if (!child_is_separator) {
                    saw_recent = saw_recent || wcscmp(child_title, L"RECENT") == 0;
                    saw_pinned = saw_pinned || wcscmp(child_title, L"PINNED") == 0;
                    saw_browse = saw_browse || wcscmp(child_title, L"Browse...") == 0;
                    saw_manage_destinations =
                        saw_manage_destinations ||
                        wcscmp(child_title, L"Manage Destinations...") == 0;
                    saw_recycle_bin = saw_recycle_bin || wcscmp(child_title, L"Recycle Bin") == 0;
                    saw_isolated_destination =
                        saw_isolated_destination ||
                        wcscmp(child_title, kIsolatedDestinationTitle) == 0;
                }
            }
            CoTaskMemFree(child_title);
            child->Release();
            child = nullptr;
            fetched = 0;
        }
        child_contract_ok = child_contract_ok && next_result == S_FALSE && fetched == 0;
        subcommands->Release();
    }

    const bool modern_contract_ok = SUCCEEDED(result) && title != nullptr &&
                                    wcscmp(title, L"QuickYeet") == 0 &&
                                    SUCCEEDED(flags_result) && flags == ECF_HASSUBCOMMANDS &&
                                    SUCCEEDED(state_result) && selection_state == ECS_ENABLED &&
                                    child_contract_ok && saw_recent && saw_pinned && saw_separator &&
                                    saw_browse && saw_manage_destinations &&
                                    saw_isolated_destination && !saw_recycle_bin &&
                                    !selection.DetailsRequested();
    CoTaskMemFree(title);
    explorer_command->Release();
    if (!modern_contract_ok) {
        std::cerr << "FAIL  the Windows 11 command contract is incomplete\n";
        FreeLibrary(module);
        return 1;
    }

    if (can_unload() != S_OK) {
        std::cerr << "FAIL  the shell DLL retained COM objects after release\n";
        FreeLibrary(module);
        return 1;
    }

    FreeLibrary(module);
    std::cout << "PASS  classic and Windows 11 Explorer COM interfaces\n";
    return 0;
}

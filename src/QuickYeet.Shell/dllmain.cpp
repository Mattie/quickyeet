#include "ContextMenu.h"
#include "ExplorerCommand.h"
#include "ShellModule.h"

#include <shlobj.h>

#include <new>
#include <string>

HINSTANCE g_module_instance = nullptr;
std::atomic<long> g_object_count{0};
std::atomic<long> g_server_locks{0};

namespace {

class QuickYeetClassFactory final : public IClassFactory {
public:
    explicit QuickYeetClassFactory(const CLSID& class_id) : class_id_(class_id) { ++g_object_count; }
    ~QuickYeetClassFactory() { --g_object_count; }

    IFACEMETHODIMP QueryInterface(REFIID interface_id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interface_id != IID_IUnknown && interface_id != IID_IClassFactory) {
            return E_NOINTERFACE;
        }
        *object = static_cast<IClassFactory*>(this);
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

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID interface_id, void** object) override {
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        if (class_id_ == CLSID_QuickYeetModern) {
            auto* command = new (std::nothrow) QuickYeetExplorerCommand();
            if (command == nullptr) {
                return E_OUTOFMEMORY;
            }
            const HRESULT result = command->QueryInterface(interface_id, object);
            command->Release();
            return result;
        }

        auto* handler = new (std::nothrow) QuickYeetContextMenu();
        if (handler == nullptr) {
            return E_OUTOFMEMORY;
        }
        const HRESULT result = handler->QueryInterface(interface_id, object);
        handler->Release();
        return result;
    }

    IFACEMETHODIMP LockServer(const BOOL lock) override {
        if (lock) {
            ++g_server_locks;
        } else {
            --g_server_locks;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    CLSID class_id_;
};

HRESULT SetDefaultValue(HKEY root, const wchar_t* key_path, const wchar_t* value) {
    HKEY key = nullptr;
    const LSTATUS created = RegCreateKeyExW(root, key_path, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                            KEY_SET_VALUE, nullptr, &key, nullptr);
    if (created != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(created);
    }
    const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    const LSTATUS written = RegSetValueExW(key, nullptr, 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(written);
}

HRESULT SetNamedValue(HKEY root, const wchar_t* key_path, const wchar_t* name,
                      const wchar_t* value) {
    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(root, key_path, 0, KEY_SET_VALUE, &key);
    if (opened != ERROR_SUCCESS) {
        return HRESULT_FROM_WIN32(opened);
    }
    const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    const LSTATUS written = RegSetValueExW(key, name, 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(written);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module_instance = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

__control_entrypoint(DllExport)
STDAPI DllCanUnloadNow(void) {
    return g_object_count.load() == 0 && g_server_locks.load() == 0 ? S_OK : S_FALSE;
}

_Check_return_
STDAPI DllGetClassObject(_In_ REFCLSID class_id, _In_ REFIID interface_id,
                         _Outptr_ LPVOID* object) {
    if (class_id != CLSID_QuickYeet && class_id != CLSID_QuickYeetModern) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    auto* factory = new (std::nothrow) QuickYeetClassFactory(class_id);
    if (factory == nullptr) {
        return E_OUTOFMEMORY;
    }
    const HRESULT result = factory->QueryInterface(interface_id, object);
    factory->Release();
    return result;
}

STDAPI DllRegisterServer() {
    wchar_t module_path[MAX_PATH]{};
    if (GetModuleFileNameW(g_module_instance, module_path, ARRAYSIZE(module_path)) == 0) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    constexpr wchar_t class_key[] =
        L"Software\\Classes\\CLSID\\{3DF187D7-6A2E-4E81-87C4-6B0D28A49678}\\InprocServer32";
    constexpr wchar_t handler_key[] =
        L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\QuickYeet";
    constexpr wchar_t modern_class_key[] =
        L"Software\\Classes\\CLSID\\{8D5E4991-C581-45A6-88C7-4611C27821F1}\\InprocServer32";
    HRESULT result = SetDefaultValue(HKEY_CURRENT_USER, class_key, module_path);
    if (SUCCEEDED(result)) {
        result = SetNamedValue(HKEY_CURRENT_USER, class_key, L"ThreadingModel", L"Apartment");
    }
    if (SUCCEEDED(result)) {
        result = SetDefaultValue(HKEY_CURRENT_USER, handler_key,
                                 L"{3DF187D7-6A2E-4E81-87C4-6B0D28A49678}");
    }
    if (SUCCEEDED(result)) {
        result = SetDefaultValue(HKEY_CURRENT_USER, modern_class_key, module_path);
    }
    if (SUCCEEDED(result)) {
        result = SetNamedValue(HKEY_CURRENT_USER, modern_class_key, L"ThreadingModel", L"Apartment");
    }
    if (SUCCEEDED(result)) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    return result;
}

STDAPI DllUnregisterServer() {
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Classes\\*\\shellex\\ContextMenuHandlers\\QuickYeet");
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Classes\\CLSID\\{3DF187D7-6A2E-4E81-87C4-6B0D28A49678}");
    RegDeleteTreeW(HKEY_CURRENT_USER,
                   L"Software\\Classes\\CLSID\\{8D5E4991-C581-45A6-88C7-4611C27821F1}");
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

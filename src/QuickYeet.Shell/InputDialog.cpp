#include "InputDialog.h"

#include "resource.h"

#include <shobjidl.h>

#include <filesystem>

namespace {

struct PromptState {
    const std::wstring* title;
    const std::wstring* prompt;
    const std::wstring* initial_value;
    std::optional<std::wstring> result;
};

HINSTANCE ResourceModule() {
    static const int module_anchor = 0;
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_anchor), &module);
    return module;
}

INT_PTR CALLBACK PromptProcedure(HWND dialog, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<PromptState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<PromptState*>(l_param);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetWindowTextW(dialog, state->title->c_str());
        SetDlgItemTextW(dialog, IDC_PROMPT_TEXT, state->prompt->c_str());
        SetDlgItemTextW(dialog, IDC_VALUE, state->initial_value->c_str());
        SendDlgItemMessageW(dialog, IDC_VALUE, EM_SETSEL, 0, -1);
        return TRUE;
    }
    if (state == nullptr) {
        return FALSE;
    }

    if (message == WM_COMMAND && LOWORD(w_param) == IDOK) {
        const int length = GetWindowTextLengthW(GetDlgItem(dialog, IDC_VALUE));
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        GetDlgItemTextW(dialog, IDC_VALUE, value.data(), length + 1);
        value.resize(static_cast<size_t>(length));
        state->result = std::move(value);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if ((message == WM_COMMAND && LOWORD(w_param) == IDCANCEL) || message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

}  // namespace

std::optional<std::wstring> ShowTextPrompt(HWND owner, const std::wstring& title,
                                           const std::wstring& prompt,
                                           const std::wstring& initial_value) {
    PromptState state{&title, &prompt, &initial_value, std::nullopt};
    DialogBoxParamW(ResourceModule(), MAKEINTRESOURCEW(IDD_TEXT_PROMPT), owner, PromptProcedure,
                    reinterpret_cast<LPARAM>(&state));
    return state.result;
}

std::optional<std::wstring> BrowseForDestination(HWND owner, const std::wstring& initial_folder) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        return std::nullopt;
    }

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    dialog->SetTitle(L"Choose where to move the file");

    IShellItem* initial_item = nullptr;
    if (!initial_folder.empty() &&
        SUCCEEDED(SHCreateItemFromParsingName(initial_folder.c_str(), nullptr,
                                              IID_PPV_ARGS(&initial_item)))) {
        dialog->SetFolder(initial_item);
        initial_item->Release();
    }

    std::optional<std::wstring> selected;
    result = dialog->Show(owner);
    if (SUCCEEDED(result)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                selected = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return selected;
}

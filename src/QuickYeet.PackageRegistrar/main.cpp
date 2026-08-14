#include "quickyeet/ConfigMigration.h"

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr wchar_t kPackageFamilyName[] = L"QuickYeet.Identity_en5h6rmf829ty";

std::wstring QuoteArgument(const std::wstring& argument) {
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

std::wstring PowerShellLiteral(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size());
    for (const wchar_t character : value) {
        escaped.push_back(character);
        if (character == L'\'') {
            escaped.push_back(L'\'');
        }
    }
    return L"'" + escaped + L"'";
}

std::wstring FindPowerShell() {
    std::wstring windows_directory(MAX_PATH, L'\0');
    const UINT length = GetWindowsDirectoryW(windows_directory.data(),
                                             static_cast<UINT>(windows_directory.size()));
    if (length == 0 || length >= windows_directory.size()) {
        return {};
    }
    windows_directory.resize(length);
    return windows_directory + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
}

int RunPowerShell(const std::wstring& script) {
    const std::wstring executable = FindPowerShell();
    if (executable.empty()) {
        return ERROR_FILE_NOT_FOUND;
    }

    std::wstring command_line = QuoteArgument(executable) +
                                L" -NoLogo -NoProfile -NonInteractive -WindowStyle Hidden "
                                L"-ExecutionPolicy Bypass -Command \"& { " +
                                script + L" }\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return static_cast<int>(GetLastError());
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = ERROR_GEN_FAILURE;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

}  // namespace

int wmain(const int argument_count, wchar_t** arguments) {
    if (argument_count < 2) {
        std::wcerr << L"Usage: QuickYeetPackageRegistrar register <package> <external-location> | "
                      L"unregister | migrate-config <local-app-data>\n";
        return ERROR_INVALID_PARAMETER;
    }

    const std::wstring operation = arguments[1];
    if (operation == L"register" && argument_count == 4) {
        const std::wstring script =
            L"Add-AppxPackage -Path " + PowerShellLiteral(arguments[2]) +
            L" -ExternalLocation " + PowerShellLiteral(arguments[3]) +
            L" -ForceUpdateFromAnyVersion -ErrorAction Stop";
        return RunPowerShell(script);
    }
    if (operation == L"unregister" && argument_count == 2) {
        return RunPowerShell(
            L"Get-AppxPackage -Name 'QuickYeet.Identity' | Remove-AppxPackage -ErrorAction Stop");
    }
    if (operation == L"migrate-config" && argument_count == 3) {
        const fs::path local_app_data = arguments[2];
        const fs::path virtualized_config =
            local_app_data / L"Packages" / kPackageFamilyName / L"LocalCache" / L"Local" /
            L"QuickYeet" / L"config.yaml";
        const fs::path real_config = local_app_data / L"QuickYeet" / L"config.yaml";
        std::wstring migration_error;
        if (!quickyeet::MigrateConfigFile(virtualized_config, real_config, &migration_error)) {
            std::wcerr << migration_error << L'\n';
            return ERROR_GEN_FAILURE;
        }
        return ERROR_SUCCESS;
    }

    std::wcerr << L"Invalid QuickYeet package registration arguments.\n";
    return ERROR_INVALID_PARAMETER;
}

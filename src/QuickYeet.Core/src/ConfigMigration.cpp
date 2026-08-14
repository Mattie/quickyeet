#include "quickyeet/ConfigMigration.h"

#include "quickyeet/Text.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace quickyeet {
namespace {

void SetError(std::wstring* error, std::wstring message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool PathExists(const fs::path& path, bool* exists, std::wstring* error) {
    std::error_code inspect_error;
    *exists = fs::exists(path, inspect_error);
    if (!inspect_error) {
        return true;
    }
    SetError(error, L"QuickYeet could not inspect its configuration during upgrade: " +
                        WindowsErrorMessage(inspect_error.value()));
    return false;
}

bool FilesEqual(const fs::path& left, const fs::path& right, bool* equal,
                std::wstring* error) {
    std::error_code left_size_error;
    const auto left_size = fs::file_size(left, left_size_error);
    std::error_code right_size_error;
    const auto right_size = fs::file_size(right, right_size_error);
    if (left_size_error || right_size_error) {
        const auto code = left_size_error ? left_size_error : right_size_error;
        SetError(error, L"QuickYeet could not compare configuration files during upgrade: " +
                            WindowsErrorMessage(code.value()));
        return false;
    }
    if (left_size != right_size) {
        *equal = false;
        return true;
    }

    std::ifstream left_input(left, std::ios::binary);
    std::ifstream right_input(right, std::ios::binary);
    if (!left_input || !right_input) {
        SetError(error, L"QuickYeet could not read configuration files during upgrade.");
        return false;
    }

    std::array<char, 4096> left_buffer{};
    std::array<char, 4096> right_buffer{};
    while (left_input && right_input) {
        left_input.read(left_buffer.data(), left_buffer.size());
        right_input.read(right_buffer.data(), right_buffer.size());
        const auto left_count = left_input.gcount();
        const auto right_count = right_input.gcount();
        if (left_count != right_count ||
            !std::equal(left_buffer.begin(), left_buffer.begin() + left_count,
                        right_buffer.begin())) {
            *equal = false;
            return true;
        }
    }
    if ((!left_input.eof() && left_input.fail()) ||
        (!right_input.eof() && right_input.fail())) {
        SetError(error, L"QuickYeet could not finish comparing configuration files during upgrade.");
        return false;
    }
    *equal = true;
    return true;
}

fs::path TemporaryPathFor(const fs::path& destination) {
    return destination.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(GetCurrentThreadId()) + L"." +
           std::to_wstring(GetTickCount64()) + L".migration.tmp";
}

bool CopyAtomically(const fs::path& source, const fs::path& destination,
                    const bool replace_existing, std::wstring* error) {
    std::error_code directory_error;
    fs::create_directories(destination.parent_path(), directory_error);
    if (directory_error) {
        SetError(error, L"QuickYeet could not create its shared configuration folder during upgrade: " +
                            WindowsErrorMessage(directory_error.value()));
        return false;
    }

    const fs::path temporary = TemporaryPathFor(destination);
    if (!CopyFileW(source.c_str(), temporary.c_str(), FALSE)) {
        SetError(error, L"QuickYeet could not stage its configuration during upgrade: " +
                            WindowsErrorMessage(GetLastError()));
        return false;
    }

    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replace_existing) {
        flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), flags)) {
        const DWORD move_error = GetLastError();
        DeleteFileW(temporary.c_str());
        SetError(error, L"QuickYeet could not finish preserving its configuration during upgrade: " +
                            WindowsErrorMessage(move_error));
        return false;
    }
    return true;
}

bool PreserveBackup(const fs::path& source, const fs::path& base_backup,
                    std::wstring* error) {
    constexpr unsigned int maximum_backup_attempts = 1'000;
    for (unsigned int index = 0; index < maximum_backup_attempts; ++index) {
        fs::path candidate = base_backup;
        if (index > 0) {
            candidate = base_backup.parent_path() /
                        (base_backup.stem().wstring() + L"-" + std::to_wstring(index + 1) +
                         base_backup.extension().wstring());
        }

        bool candidate_exists = false;
        if (!PathExists(candidate, &candidate_exists, error)) {
            return false;
        }
        if (!candidate_exists) {
            return CopyAtomically(source, candidate, false, error);
        }

        bool equal = false;
        if (!FilesEqual(source, candidate, &equal, error)) {
            return false;
        }
        if (equal) {
            return true;
        }
    }
    SetError(error, L"QuickYeet could not choose a configuration backup name during upgrade.");
    return false;
}

}  // namespace

bool MigrateConfigFile(const fs::path& virtualized_config, const fs::path& real_config,
                       std::wstring* error) {
    bool virtualized_exists = false;
    if (!PathExists(virtualized_config, &virtualized_exists, error)) {
        return false;
    }
    if (!virtualized_exists) {
        return true;
    }

    bool real_exists = false;
    if (!PathExists(real_config, &real_exists, error)) {
        return false;
    }
    if (!real_exists) {
        return CopyAtomically(virtualized_config, real_config, false, error);
    }

    bool equal = false;
    if (!FilesEqual(virtualized_config, real_config, &equal, error)) {
        return false;
    }
    if (equal) {
        return true;
    }

    std::error_code virtualized_time_error;
    const auto virtualized_time = fs::last_write_time(virtualized_config, virtualized_time_error);
    std::error_code real_time_error;
    const auto real_time = fs::last_write_time(real_config, real_time_error);
    if (virtualized_time_error || real_time_error) {
        const auto code = virtualized_time_error ? virtualized_time_error : real_time_error;
        SetError(error, L"QuickYeet could not compare configuration timestamps during upgrade: " +
                            WindowsErrorMessage(code.value()));
        return false;
    }

    if (virtualized_time > real_time) {
        const fs::path backup =
            real_config.parent_path() / L"config.pre-migration-real-backup.yaml";
        return PreserveBackup(real_config, backup, error) &&
               CopyAtomically(virtualized_config, real_config, true, error);
    }

    const fs::path backup =
        real_config.parent_path() / L"config.pre-migration-virtualized-backup.yaml";
    return PreserveBackup(virtualized_config, backup, error);
}

}  // namespace quickyeet

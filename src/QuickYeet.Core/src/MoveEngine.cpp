#include "quickyeet/MoveEngine.h"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <system_error>

namespace quickyeet {
namespace fs = std::filesystem;
namespace {

class ScopedComApartment {
public:
    ScopedComApartment() : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }
    [[nodiscard]] HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

}  // namespace

fs::path NextAvailablePath(const fs::path& destination_directory, const fs::path& source_name) {
    const fs::path filename = source_name.filename();
    fs::path candidate = destination_directory / filename;

    std::error_code error;
    if (!fs::exists(candidate, error) && !error) {
        return candidate;
    }

    const std::wstring stem = filename.stem().wstring();
    const std::wstring extension = filename.extension().wstring();
    for (unsigned int suffix = 2; suffix < 1'000'000; ++suffix) {
        candidate = destination_directory / fs::path(stem + L" (" + std::to_wstring(suffix) + L")" + extension);
        error.clear();
        if (!fs::exists(candidate, error) && !error) {
            return candidate;
        }
    }

    throw std::runtime_error("QuickYeet could not find a free destination name.");
}

std::vector<MoveResult> MoveFilesWithCollisionSuffix(const std::vector<fs::path>& sources,
                                                     const fs::path& destination_directory) {
    std::vector<MoveResult> results;
    results.reserve(sources.size());

    const DWORD attributes = GetFileAttributesW(destination_directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_DIRECTORY;
        for (const auto& source : sources) {
            results.push_back({source, {}, error});
        }
        return results;
    }

    for (const auto& source : sources) {
        MoveResult result{source, {}, 0};
        const DWORD source_attributes = GetFileAttributesW(source.c_str());
        if (source_attributes == INVALID_FILE_ATTRIBUTES || (source_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            result.error_code = source_attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_FILE_NOT_FOUND;
            results.push_back(std::move(result));
            continue;
        }

        try {
            for (;;) {
                result.destination = NextAvailablePath(destination_directory, source.filename());
                if (MoveFileExW(source.c_str(), result.destination.c_str(),
                                MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
                    break;
                }

                result.error_code = GetLastError();
                if (result.error_code != ERROR_ALREADY_EXISTS && result.error_code != ERROR_FILE_EXISTS) {
                    break;
                }
            }
        } catch (const std::exception&) {
            result.error_code = ERROR_FILE_EXISTS;
        }
        results.push_back(std::move(result));
    }

    return results;
}

unsigned long RecycleFiles(const std::vector<fs::path>& sources) {
    if (sources.empty()) {
        return ERROR_INVALID_PARAMETER;
    }
    for (const auto& source : sources) {
        const DWORD attributes = GetFileAttributesW(source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return GetLastError();
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return ERROR_FILE_NOT_FOUND;
        }
    }

    ScopedComApartment apartment;
    if (!apartment.usable()) {
        return static_cast<unsigned long>(apartment.result());
    }

    Microsoft::WRL::ComPtr<IFileOperation> operation;
    HRESULT result = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&operation));
    if (FAILED(result)) {
        return static_cast<unsigned long>(result);
    }
    const DWORD flags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING |
                        FOFX_RECYCLEONDELETE | FOFX_ADDUNDORECORD | FOFX_EARLYFAILURE;
    result = operation->SetOperationFlags(flags);
    if (FAILED(result)) {
        return static_cast<unsigned long>(result);
    }

    for (const auto& source : sources) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        result = SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&item));
        if (FAILED(result)) {
            return static_cast<unsigned long>(result);
        }
        result = operation->DeleteItem(item.Get(), nullptr);
        if (FAILED(result)) {
            return static_cast<unsigned long>(result);
        }
    }

    result = operation->PerformOperations();
    if (FAILED(result)) {
        return static_cast<unsigned long>(result);
    }
    BOOL aborted = FALSE;
    result = operation->GetAnyOperationsAborted(&aborted);
    if (FAILED(result)) {
        return static_cast<unsigned long>(result);
    }
    return aborted != FALSE ? ERROR_CANCELLED : ERROR_SUCCESS;
}

}  // namespace quickyeet

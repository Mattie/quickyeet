#include "quickyeet/Text.h"

#include <windows.h>

#include <stdexcept>
#include <vector>

namespace quickyeet {

std::string ToUtf8(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        throw std::runtime_error("Windows could not encode text as UTF-8.");
    }

    std::string output(static_cast<size_t>(required), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), output.data(), required,
                                            nullptr, nullptr);
    if (written != required) {
        throw std::runtime_error("Windows returned an incomplete UTF-8 conversion.");
    }
    return output;
}

std::wstring FromUtf8(const std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        throw std::runtime_error("The configuration contains invalid UTF-8.");
    }

    std::wstring output(static_cast<size_t>(required), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                            static_cast<int>(value.size()), output.data(), required);
    if (written != required) {
        throw std::runtime_error("Windows returned an incomplete UTF-8 conversion.");
    }
    return output;
}

std::wstring WindowsErrorMessage(const unsigned long error_code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error_code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return L"Windows error " + std::to_wstring(error_code);
    }

    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

}  // namespace quickyeet

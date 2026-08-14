#pragma once

#include <string>
#include <string_view>

namespace quickyeet {

std::string ToUtf8(std::wstring_view value);
std::wstring FromUtf8(std::string_view value);
std::wstring WindowsErrorMessage(unsigned long error_code);

}  // namespace quickyeet

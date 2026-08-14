#pragma once

#include <windows.h>

#include <optional>
#include <string>

std::optional<std::wstring> ShowTextPrompt(HWND owner, const std::wstring& title,
                                           const std::wstring& prompt,
                                           const std::wstring& initial_value = {});

std::optional<std::wstring> BrowseForDestination(HWND owner,
                                                 const std::wstring& initial_folder = {});

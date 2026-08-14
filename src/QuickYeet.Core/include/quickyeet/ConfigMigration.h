#pragma once

#include <filesystem>
#include <string>

namespace quickyeet {

// Preserves a package-virtualized configuration at the shared real-AppData path.
// When both files differ, the newer file becomes active and the other is backed up.
bool MigrateConfigFile(const std::filesystem::path& virtualized_config,
                       const std::filesystem::path& real_config,
                       std::wstring* error = nullptr);

}  // namespace quickyeet

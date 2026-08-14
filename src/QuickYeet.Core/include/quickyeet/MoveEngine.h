#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace quickyeet {

struct MoveResult {
    std::filesystem::path source;
    std::filesystem::path destination;
    unsigned long error_code = 0;

    [[nodiscard]] bool succeeded() const noexcept { return error_code == 0; }
};

// Finds a free name in destination_directory, starting with the original name and then " (2)".
std::filesystem::path NextAvailablePath(const std::filesystem::path& destination_directory,
                                        const std::filesystem::path& source_name);

// Moves regular files without replacing anything already in the destination.
std::vector<MoveResult> MoveFilesWithCollisionSuffix(
    const std::vector<std::filesystem::path>& sources,
    const std::filesystem::path& destination_directory);

// Sends regular files to the Windows Recycle Bin. Returns zero when every file was recycled.
unsigned long RecycleFiles(const std::vector<std::filesystem::path>& sources);

}  // namespace quickyeet

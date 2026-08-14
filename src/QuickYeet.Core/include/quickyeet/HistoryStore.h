#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace quickyeet {

struct DestinationRecord {
    std::filesystem::path path;
    std::wstring alias;
    std::uint64_t use_count = 0;
    std::int64_t last_used = 0;
    bool pinned = false;
};

class HistoryStore {
public:
    explicit HistoryStore(std::filesystem::path storage_path = DefaultStoragePath());

    static std::filesystem::path DefaultConfigFolder();
    // Contract tests can set QUICKYEET_TEST_CONFIG_PATH to isolate the default store.
    static std::filesystem::path DefaultStoragePath();
    static std::int64_t CurrentUnixTime();

    bool Load(std::wstring* error = nullptr);
    bool Save(std::wstring* error = nullptr) const;

    void RecordUse(const std::filesystem::path& path, std::int64_t used_at = CurrentUnixTime());
    void SetPinned(const std::filesystem::path& path, bool pinned);
    void SetAlias(const std::filesystem::path& path, std::wstring alias);
    bool Remove(const std::filesystem::path& path);

    [[nodiscard]] std::optional<DestinationRecord> Find(const std::filesystem::path& path) const;
    [[nodiscard]] std::vector<DestinationRecord> Pinned() const;
    [[nodiscard]] std::vector<DestinationRecord> Recent(
        std::size_t limit, std::int64_t now = CurrentUnixTime()) const;
    [[nodiscard]] bool RecycleBinEnabled() const noexcept { return recycle_bin_enabled_; }
    [[nodiscard]] const std::vector<DestinationRecord>& Records() const noexcept { return records_; }
    [[nodiscard]] const std::filesystem::path& StoragePath() const noexcept { return storage_path_; }

private:
    DestinationRecord& Upsert(const std::filesystem::path& path);

    std::filesystem::path storage_path_;
    std::vector<DestinationRecord> records_;
    bool recycle_bin_enabled_ = true;
};

std::filesystem::path NormalizeDestinationPath(const std::filesystem::path& path);
bool OpenConfigFolder(std::wstring* error = nullptr);

}  // namespace quickyeet

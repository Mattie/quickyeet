#include "quickyeet/HistoryStore.h"

#include "quickyeet/Text.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>

namespace quickyeet {
namespace fs = std::filesystem;
namespace {

constexpr char kLegacyHeader[] = "quickyeet-v1";
constexpr wchar_t kConfigFileName[] = L"config.yaml";
constexpr wchar_t kLegacyFileName[] = L"destinations.tsv";
constexpr wchar_t kTestConfigPathEnvironmentVariable[] = L"QUICKYEET_TEST_CONFIG_PATH";

std::optional<fs::path> TestConfigPathOverride() {
    const DWORD required =
        GetEnvironmentVariableW(kTestConfigPathEnvironmentVariable, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }

    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(kTestConfigPathEnvironmentVariable,
                                                   value.data(), required);
    if (written == 0 || written >= required) {
        return std::nullopt;
    }
    value.resize(written);
    return fs::path(std::move(value));
}

bool SamePath(const fs::path& left, const fs::path& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

std::wstring CleanAlias(std::wstring alias) {
    for (wchar_t& character : alias) {
        if (character < 32) {
            character = L' ';
        }
    }
    return alias;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    for (;;) {
        const size_t tab = line.find('\t', start);
        fields.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
        if (tab == std::string::npos) {
            return fields;
        }
        start = tab + 1;
    }
}

void SetError(std::wstring* error, const std::wstring& value) {
    if (error != nullptr) {
        *error = value;
    }
}

std::string_view Trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

std::string_view StripYamlComment(std::string_view value) {
    bool in_single_quotes = false;
    bool in_double_quotes = false;
    bool escaped = false;
    for (size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (in_double_quotes && escaped) {
            escaped = false;
            continue;
        }
        if (in_double_quotes && character == '\\') {
            escaped = true;
            continue;
        }
        if (!in_double_quotes && character == '\'') {
            if (in_single_quotes && index + 1 < value.size() && value[index + 1] == '\'') {
                ++index;
            } else {
                in_single_quotes = !in_single_quotes;
            }
            continue;
        }
        if (!in_single_quotes && character == '"') {
            in_double_quotes = !in_double_quotes;
            continue;
        }
        if (!in_single_quotes && !in_double_quotes && character == '#' &&
            (index == 0 || std::isspace(static_cast<unsigned char>(value[index - 1])) != 0)) {
            return Trim(value.substr(0, index));
        }
    }
    return Trim(value);
}

int HexValue(const char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

std::string QuoteYaml(const std::wstring& value) {
    const std::string utf8 = ToUtf8(value);
    std::string quoted;
    quoted.reserve(utf8.size() + 2);
    quoted.push_back('"');
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char character : utf8) {
        if (character == '"' || character == '\\') {
            quoted.push_back('\\');
            quoted.push_back(static_cast<char>(character));
        } else if (character < 32) {
            quoted += "\\x";
            quoted.push_back(hex[character >> 4]);
            quoted.push_back(hex[character & 0x0F]);
        } else {
            quoted.push_back(static_cast<char>(character));
        }
    }
    quoted.push_back('"');
    return quoted;
}

bool ParseYamlString(std::string_view value, std::wstring* parsed) {
    value = Trim(value);
    if (value == "null" || value == "~") {
        parsed->clear();
        return true;
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        std::string unescaped;
        unescaped.reserve(value.size() - 2);
        for (size_t index = 1; index + 1 < value.size(); ++index) {
            const char character = value[index];
            if (character == '"') {
                return false;
            }
            if (character != '\\') {
                unescaped.push_back(character);
                continue;
            }
            if (++index + 1 >= value.size()) {
                return false;
            }
            const char escaped = value[index];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    unescaped.push_back(escaped);
                    break;
                case 'b':
                    unescaped.push_back('\b');
                    break;
                case 'f':
                    unescaped.push_back('\f');
                    break;
                case 'n':
                    unescaped.push_back('\n');
                    break;
                case 'r':
                    unescaped.push_back('\r');
                    break;
                case 't':
                    unescaped.push_back('\t');
                    break;
                case 'x': {
                    if (index + 3 >= value.size()) {
                        return false;
                    }
                    const int high = HexValue(value[index + 1]);
                    const int low = HexValue(value[index + 2]);
                    if (high < 0 || low < 0) {
                        return false;
                    }
                    unescaped.push_back(static_cast<char>((high << 4) | low));
                    index += 2;
                    break;
                }
                default:
                    return false;
            }
        }
        *parsed = FromUtf8(unescaped);
        return true;
    }
    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
        std::string unescaped;
        for (size_t index = 1; index + 1 < value.size(); ++index) {
            if (value[index] == '\'') {
                if (index + 2 >= value.size() || value[index + 1] != '\'') {
                    return false;
                }
                unescaped.push_back('\'');
                ++index;
                continue;
            }
            unescaped.push_back(value[index]);
        }
        *parsed = FromUtf8(unescaped);
        return true;
    }
    if (!value.empty() &&
        (value.front() == '"' || value.front() == '\'' || value.back() == '"' ||
         value.back() == '\'')) {
        return false;
    }
    *parsed = FromUtf8(std::string(value));
    return true;
}

template <typename Integer>
bool ParseInteger(const std::string_view value, Integer* parsed) {
    const std::string_view trimmed = Trim(value);
    const auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), *parsed);
    return result.ec == std::errc{} && result.ptr == trimmed.data() + trimmed.size();
}

struct PendingRecord {
    DestinationRecord record;
    bool has_path = false;
    bool has_display_name = false;
    bool has_pinned = false;
    bool has_use_count = false;
    bool has_last_used = false;
};

bool SetYamlProperty(const std::string_view property, PendingRecord* pending,
                     std::wstring* error) {
    const size_t separator = property.find(':');
    if (separator == std::string_view::npos) {
        SetError(error, L"QuickYeet's YAML configuration contains an invalid destination field.");
        return false;
    }
    const std::string_view key = Trim(property.substr(0, separator));
    const std::string_view value = Trim(property.substr(separator + 1));
    if (key == "path" && !pending->has_path) {
        std::wstring path;
        if (!ParseYamlString(value, &path)) {
            SetError(error, L"QuickYeet's YAML configuration contains an invalid path.");
            return false;
        }
        pending->record.path = NormalizeDestinationPath(fs::path(path));
        pending->has_path = true;
        return true;
    }
    if (key == "display_name" && !pending->has_display_name) {
        if (!ParseYamlString(value, &pending->record.alias)) {
            SetError(error, L"QuickYeet's YAML configuration contains an invalid display name.");
            return false;
        }
        pending->record.alias = CleanAlias(std::move(pending->record.alias));
        pending->has_display_name = true;
        return true;
    }
    if (key == "pinned" && !pending->has_pinned) {
        if (value != "true" && value != "false") {
            SetError(error, L"QuickYeet's YAML configuration contains an invalid pin state.");
            return false;
        }
        pending->record.pinned = value == "true";
        pending->has_pinned = true;
        return true;
    }
    if (key == "use_count" && !pending->has_use_count) {
        if (!ParseInteger(value, &pending->record.use_count)) {
            SetError(error, L"QuickYeet's YAML configuration contains an invalid use count.");
            return false;
        }
        pending->has_use_count = true;
        return true;
    }
    if (key == "last_used" && !pending->has_last_used) {
        if (!ParseInteger(value, &pending->record.last_used)) {
            SetError(error, L"QuickYeet's YAML configuration contains an invalid last-used time.");
            return false;
        }
        pending->has_last_used = true;
        return true;
    }
    SetError(error, L"QuickYeet's YAML configuration contains an unknown or repeated destination field.");
    return false;
}

bool FinishYamlRecord(std::optional<PendingRecord>* pending,
                      std::vector<DestinationRecord>* records, std::wstring* error) {
    if (!pending->has_value()) {
        return true;
    }
    if (!(*pending)->has_path || (*pending)->record.path.empty()) {
        SetError(error, L"Every destination in QuickYeet's YAML configuration needs a path.");
        return false;
    }
    records->push_back(std::move((*pending)->record));
    pending->reset();
    return true;
}

bool ParseYamlConfiguration(std::istream& input, std::vector<DestinationRecord>* records,
                            bool* recycle_bin_enabled, std::wstring* error) {
    bool saw_version = false;
    bool saw_recycle_bin_setting = false;
    bool saw_destinations = false;
    bool destinations_are_empty = false;
    std::optional<PendingRecord> pending;
    std::string line;
    bool first_line = true;
    while (std::getline(input, line)) {
        if (first_line && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        first_line = false;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string_view trimmed_line = Trim(line);
        if (trimmed_line.empty() || trimmed_line.front() == '#') {
            continue;
        }

        const size_t indentation = line.find_first_not_of(' ');
        if (indentation == std::string::npos) {
            continue;
        }
        const std::string_view content =
            StripYamlComment(std::string_view(line).substr(indentation));
        if (content.empty()) {
            continue;
        }
        if (indentation == 0) {
            if (!FinishYamlRecord(&pending, records, error)) {
                return false;
            }
            const size_t separator = content.find(':');
            const std::string_view top_level_key = Trim(content.substr(0, separator));
            if (content == "version: 1" && !saw_version) {
                saw_version = true;
            } else if (separator != std::string_view::npos &&
                       top_level_key == "show_recycle_bin" && !saw_recycle_bin_setting) {
                const std::string_view value = Trim(content.substr(separator + 1));
                if (value != "true" && value != "false") {
                    SetError(error, L"QuickYeet's YAML configuration contains an invalid Recycle Bin setting.");
                    return false;
                }
                *recycle_bin_enabled = value == "true";
                saw_recycle_bin_setting = true;
            } else if ((content == "destinations:" || content == "destinations: []") &&
                       !saw_destinations) {
                saw_destinations = true;
                destinations_are_empty = content == "destinations: []";
            } else {
                SetError(error, L"QuickYeet's YAML configuration has an unsupported top-level field or version.");
                return false;
            }
            continue;
        }

        if (!saw_destinations || destinations_are_empty) {
            SetError(error, L"QuickYeet's YAML configuration contains a destination in the wrong place.");
            return false;
        }
        if (content == "-" || (content.size() > 1 && content[0] == '-' && content[1] == ' ')) {
            if (!FinishYamlRecord(&pending, records, error)) {
                return false;
            }
            pending.emplace();
            const std::string_view first_property = Trim(content.substr(1));
            if (!first_property.empty() && !SetYamlProperty(first_property, &*pending, error)) {
                return false;
            }
        } else {
            if (!pending.has_value() || !SetYamlProperty(content, &*pending, error)) {
                if (!pending.has_value()) {
                    SetError(error, L"QuickYeet's YAML configuration contains a destination field without a list item.");
                }
                return false;
            }
        }
    }
    if (!FinishYamlRecord(&pending, records, error)) {
        return false;
    }
    if (!saw_version || !saw_destinations) {
        SetError(error, L"QuickYeet's YAML configuration is missing its version or destination list.");
        return false;
    }
    return true;
}

bool LoadLegacyConfiguration(const fs::path& path, std::vector<DestinationRecord>* records,
                             std::wstring* error) {
    std::ifstream input(path, std::ios::binary);
    std::string line;
    if (!input || !std::getline(input, line) || line != kLegacyHeader) {
        SetError(error, L"QuickYeet's legacy destination configuration is damaged.");
        return false;
    }
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const auto fields = SplitTabs(line);
        if (fields.size() != 5) {
            SetError(error, L"QuickYeet's legacy destination configuration is damaged.");
            return false;
        }
        DestinationRecord record;
        record.pinned = fields[0] == "1";
        record.use_count = std::stoull(fields[1]);
        record.last_used = std::stoll(fields[2]);
        record.alias = CleanAlias(FromUtf8(fields[3]));
        record.path = NormalizeDestinationPath(fs::path(FromUtf8(fields[4])));
        if (record.path.empty()) {
            SetError(error, L"QuickYeet's legacy destination configuration contains an empty path.");
            return false;
        }
        records->push_back(std::move(record));
    }
    return true;
}

double Rank(const DestinationRecord& record, const std::int64_t now) {
    constexpr double seconds_per_day = 86'400.0;
    const double age_days = std::max(0.0, static_cast<double>(now - record.last_used) / seconds_per_day);
    const double recency = 2.0 / (1.0 + age_days / 7.0);
    const double frequency = 0.6 * std::log2(static_cast<double>(record.use_count) + 1.0);
    return recency + frequency;
}

}  // namespace

fs::path NormalizeDestinationPath(const fs::path& path) {
    if (path.empty()) {
        return {};
    }

    std::error_code error;
    fs::path normalized = fs::absolute(path, error);
    if (error) {
        normalized = path;
    }
    normalized = normalized.lexically_normal();
    if (normalized.has_relative_path() && normalized.filename().empty()) {
        normalized = normalized.parent_path();
    }
    return normalized;
}

HistoryStore::HistoryStore(fs::path storage_path) : storage_path_(std::move(storage_path)) {}

fs::path HistoryStore::DefaultConfigFolder() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local_app_data))) {
        return fs::path(L".") / L"QuickYeet";
    }
    fs::path result = fs::path(local_app_data) / L"QuickYeet";
    CoTaskMemFree(local_app_data);
    return result;
}

fs::path HistoryStore::DefaultStoragePath() {
    if (const auto test_path = TestConfigPathOverride()) {
        return *test_path;
    }
    return DefaultConfigFolder() / kConfigFileName;
}

std::int64_t HistoryStore::CurrentUnixTime() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool HistoryStore::Load(std::wstring* error) {
    records_.clear();
    recycle_bin_enabled_ = true;
    std::ifstream input(storage_path_, std::ios::binary);
    if (!input) {
        std::error_code exists_error;
        if (!fs::exists(storage_path_, exists_error)) {
            if (exists_error) {
                SetError(error, L"QuickYeet could not inspect its YAML configuration.");
                return false;
            }
            const fs::path legacy_path = storage_path_.parent_path() / kLegacyFileName;
            std::error_code legacy_exists_error;
            const bool legacy_exists = storage_path_.filename() == kConfigFileName &&
                                       fs::exists(legacy_path, legacy_exists_error);
            if (legacy_exists_error) {
                SetError(error, L"QuickYeet could not inspect its legacy configuration.");
                return false;
            }
            if (legacy_exists) {
                try {
                    if (!LoadLegacyConfiguration(legacy_path, &records_, error) || !Save(error)) {
                        records_.clear();
                        return false;
                    }
                } catch (const std::exception&) {
                    records_.clear();
                    SetError(error, L"QuickYeet could not migrate its legacy destination configuration.");
                    return false;
                }
                const fs::path backup_path = legacy_path.wstring() + L".backup";
                MoveFileExW(legacy_path.c_str(), backup_path.c_str(), MOVEFILE_WRITE_THROUGH);
            }
            return true;
        }
        SetError(error, L"QuickYeet could not open its configuration.");
        return false;
    }

    try {
        if (!ParseYamlConfiguration(input, &records_, &recycle_bin_enabled_, error)) {
            records_.clear();
            recycle_bin_enabled_ = true;
            return false;
        }
    } catch (const std::exception&) {
        records_.clear();
        recycle_bin_enabled_ = true;
        SetError(error, L"QuickYeet's YAML configuration is damaged.");
        return false;
    }
    return true;
}

bool HistoryStore::Save(std::wstring* error) const {
    std::error_code directory_error;
    fs::create_directories(storage_path_.parent_path(), directory_error);
    if (directory_error) {
        SetError(error, L"QuickYeet could not create its local configuration folder.");
        return false;
    }

    const fs::path temporary =
        storage_path_.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetCurrentThreadId()) + L".tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            SetError(error, L"QuickYeet could not write its configuration.");
            return false;
        }
        output << "version: 1\n";
        output << "show_recycle_bin: " << (recycle_bin_enabled_ ? "true" : "false") << '\n';
        if (records_.empty()) {
            output << "destinations: []\n";
        } else {
            output << "destinations:\n";
            for (const auto& record : records_) {
                output << "  - path: " << QuoteYaml(record.path.wstring()) << '\n'
                       << "    display_name: " << QuoteYaml(CleanAlias(record.alias)) << '\n'
                       << "    pinned: " << (record.pinned ? "true" : "false") << '\n'
                       << "    use_count: " << record.use_count << '\n'
                       << "    last_used: " << record.last_used << '\n';
            }
        }
        output.close();
        if (!output) {
            SetError(error, L"QuickYeet could not finish writing its configuration.");
            return false;
        }
    } catch (const std::exception&) {
        SetError(error, L"QuickYeet could not encode its configuration.");
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), storage_path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD move_error = GetLastError();
        DeleteFileW(temporary.c_str());
        SetError(error, L"QuickYeet could not save its configuration: " +
                            WindowsErrorMessage(move_error));
        return false;
    }
    return true;
}

DestinationRecord& HistoryStore::Upsert(const fs::path& path) {
    const fs::path normalized = NormalizeDestinationPath(path);
    const auto existing = std::find_if(records_.begin(), records_.end(), [&](const DestinationRecord& record) {
        return SamePath(record.path, normalized);
    });
    if (existing != records_.end()) {
        return *existing;
    }
    records_.push_back({normalized});
    return records_.back();
}

void HistoryStore::RecordUse(const fs::path& path, const std::int64_t used_at) {
    DestinationRecord& record = Upsert(path);
    ++record.use_count;
    record.last_used = used_at;
}

void HistoryStore::SetPinned(const fs::path& path, const bool pinned) {
    Upsert(path).pinned = pinned;
}

void HistoryStore::SetAlias(const fs::path& path, std::wstring alias) {
    Upsert(path).alias = CleanAlias(std::move(alias));
}

bool HistoryStore::Remove(const fs::path& path) {
    const fs::path normalized = NormalizeDestinationPath(path);
    const auto old_size = records_.size();
    records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const DestinationRecord& record) {
                       return SamePath(record.path, normalized);
                   }),
                   records_.end());
    return records_.size() != old_size;
}

std::optional<DestinationRecord> HistoryStore::Find(const fs::path& path) const {
    const fs::path normalized = NormalizeDestinationPath(path);
    const auto existing = std::find_if(records_.begin(), records_.end(), [&](const DestinationRecord& record) {
        return SamePath(record.path, normalized);
    });
    if (existing == records_.end()) {
        return std::nullopt;
    }
    return *existing;
}

std::vector<DestinationRecord> HistoryStore::Pinned() const {
    std::vector<DestinationRecord> pinned;
    std::copy_if(records_.begin(), records_.end(), std::back_inserter(pinned),
                 [](const DestinationRecord& record) { return record.pinned; });
    std::sort(pinned.begin(), pinned.end(), [](const DestinationRecord& left, const DestinationRecord& right) {
        return left.alias.empty() ? left.path.wstring() < right.path.wstring() : left.alias < right.alias;
    });
    return pinned;
}

std::vector<DestinationRecord> HistoryStore::Recent(const std::size_t limit, const std::int64_t now) const {
    std::vector<DestinationRecord> recent;
    std::copy_if(records_.begin(), records_.end(), std::back_inserter(recent),
                 [](const DestinationRecord& record) { return !record.pinned && record.use_count > 0; });
    std::stable_sort(recent.begin(), recent.end(), [&](const DestinationRecord& left, const DestinationRecord& right) {
        const double left_rank = Rank(left, now);
        const double right_rank = Rank(right, now);
        return left_rank == right_rank ? left.last_used > right.last_used : left_rank > right_rank;
    });
    if (recent.size() > limit) {
        recent.resize(limit);
    }
    return recent;
}

bool OpenConfigFolder(std::wstring* error) {
    const fs::path folder = HistoryStore::DefaultConfigFolder();
    std::error_code directory_error;
    fs::create_directories(folder, directory_error);
    if (directory_error) {
        SetError(error, L"QuickYeet could not create its local configuration folder.");
        return false;
    }

    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_FLAG_NO_UI;
    execute.lpVerb = L"open";
    execute.lpFile = folder.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        DWORD launch_error = GetLastError();
        if (launch_error == ERROR_SUCCESS) {
            launch_error = ERROR_OPEN_FAILED;
        }
        SetError(error, L"QuickYeet could not open its configuration folder: " +
                            WindowsErrorMessage(launch_error));
        return false;
    }
    return true;
}

}  // namespace quickyeet

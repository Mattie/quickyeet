#include "quickyeet/ConfigMigration.h"
#include "quickyeet/HistoryStore.h"
#include "quickyeet/MoveEngine.h"
#include "quickyeet/Text.h"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void Require(const bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        wchar_t root[MAX_PATH]{};
        Require(GetTempPathW(ARRAYSIZE(root), root) != 0, "GetTempPathW failed");
        path_ = fs::path(root) /
                (L"QuickYeetTests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64()));
        Require(fs::create_directories(path_), "Could not create the test directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void WriteFile(const fs::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    output << contents;
    Require(static_cast<bool>(output), "Could not write a test file");
}

std::string ReadFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void SetWriteTime(const fs::path& path, const fs::file_time_type time) {
    std::error_code error;
    fs::last_write_time(path, time, error);
    Require(!error, "Could not set a test file timestamp");
}

void MigratesVirtualizedConfigWhenRealConfigIsMissing() {
    TemporaryDirectory temporary;
    const fs::path virtualized = temporary.path() / L"virtual" / L"config.yaml";
    const fs::path real = temporary.path() / L"real" / L"config.yaml";
    fs::create_directories(virtualized.parent_path());
    WriteFile(virtualized, "virtualized");

    std::wstring error;
    Require(quickyeet::MigrateConfigFile(virtualized, real, &error),
            "A virtualized-only configuration should migrate");
    Require(ReadFile(real) == "virtualized", "Migration should preserve virtualized bytes");
    Require(ReadFile(virtualized) == "virtualized", "Migration should leave its source intact");
}

void LeavesExistingRealConfigWhenVirtualizedConfigIsMissing() {
    TemporaryDirectory temporary;
    const fs::path virtualized = temporary.path() / L"virtual" / L"config.yaml";
    const fs::path real = temporary.path() / L"real" / L"config.yaml";
    fs::create_directories(real.parent_path());
    WriteFile(real, "real");

    std::wstring error;
    Require(quickyeet::MigrateConfigFile(virtualized, real, &error),
            "A real-only configuration should need no migration");
    Require(ReadFile(real) == "real", "The existing real configuration should remain active");
}

void AvoidsBackupsForIdenticalMigrationFiles() {
    TemporaryDirectory temporary;
    const fs::path virtualized = temporary.path() / L"virtual" / L"config.yaml";
    const fs::path real = temporary.path() / L"real" / L"config.yaml";
    fs::create_directories(virtualized.parent_path());
    fs::create_directories(real.parent_path());
    WriteFile(virtualized, "same");
    WriteFile(real, "same");

    std::wstring error;
    Require(quickyeet::MigrateConfigFile(virtualized, real, &error),
            "Identical configurations should need no migration");
    Require(!fs::exists(real.parent_path() / L"config.pre-migration-real-backup.yaml") &&
                !fs::exists(real.parent_path() /
                            L"config.pre-migration-virtualized-backup.yaml"),
            "Identical configurations should not produce backups");
}

void ChoosesNewestMigrationConfigAndPreservesTheOther() {
    TemporaryDirectory temporary;
    const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(2);
    const auto new_time = fs::file_time_type::clock::now() - std::chrono::hours(1);

    const fs::path virtual_new = temporary.path() / L"virtual-new" / L"config.yaml";
    const fs::path real_old = temporary.path() / L"real-old" / L"config.yaml";
    fs::create_directories(virtual_new.parent_path());
    fs::create_directories(real_old.parent_path());
    WriteFile(virtual_new, "virtual-new");
    WriteFile(real_old, "real-old");
    SetWriteTime(virtual_new, new_time);
    SetWriteTime(real_old, old_time);

    std::wstring error;
    Require(quickyeet::MigrateConfigFile(virtual_new, real_old, &error),
            "The newer virtualized configuration should become active");
    Require(ReadFile(real_old) == "virtual-new", "The newer virtualized bytes should become active");
    Require(ReadFile(real_old.parent_path() / L"config.pre-migration-real-backup.yaml") ==
                "real-old",
            "The displaced real configuration should be backed up");

    const fs::path virtual_old = temporary.path() / L"virtual-old" / L"config.yaml";
    const fs::path real_new = temporary.path() / L"real-new" / L"config.yaml";
    fs::create_directories(virtual_old.parent_path());
    fs::create_directories(real_new.parent_path());
    WriteFile(virtual_old, "virtual-old");
    WriteFile(real_new, "real-new");
    SetWriteTime(virtual_old, old_time);
    SetWriteTime(real_new, new_time);

    Require(quickyeet::MigrateConfigFile(virtual_old, real_new, &error),
            "The newer real configuration should stay active");
    Require(ReadFile(real_new) == "real-new", "The newer real bytes should remain active");
    Require(ReadFile(real_new.parent_path() /
                     L"config.pre-migration-virtualized-backup.yaml") == "virtual-old",
            "The displaced virtualized configuration should be backed up");
}

void PrefersRealConfigWhenMigrationTimestampsTie() {
    TemporaryDirectory temporary;
    const fs::path virtualized = temporary.path() / L"virtual" / L"config.yaml";
    const fs::path real = temporary.path() / L"real" / L"config.yaml";
    fs::create_directories(virtualized.parent_path());
    fs::create_directories(real.parent_path());
    WriteFile(virtualized, "virtual");
    WriteFile(real, "real");
    const auto tied_time = fs::file_time_type::clock::now() - std::chrono::hours(1);
    SetWriteTime(virtualized, tied_time);
    SetWriteTime(real, tied_time);

    std::wstring error;
    Require(quickyeet::MigrateConfigFile(virtualized, real, &error),
            "Equal timestamps should migrate deterministically");
    Require(ReadFile(real) == "real", "The real configuration should win a timestamp tie");
    Require(ReadFile(real.parent_path() /
                     L"config.pre-migration-virtualized-backup.yaml") == "virtual",
            "The tied virtualized configuration should be backed up");
}

void NumbersConflictingMigrationBackups() {
    TemporaryDirectory temporary;
    const fs::path virtualized = temporary.path() / L"virtual" / L"config.yaml";
    const fs::path real = temporary.path() / L"real" / L"config.yaml";
    fs::create_directories(virtualized.parent_path());
    fs::create_directories(real.parent_path());
    WriteFile(virtualized, "virtual-old");
    WriteFile(real, "real-new");
    WriteFile(real.parent_path() / L"config.pre-migration-virtualized-backup.yaml",
              "previous-backup");
    SetWriteTime(virtualized, fs::file_time_type::clock::now() - std::chrono::hours(2));
    SetWriteTime(real, fs::file_time_type::clock::now() - std::chrono::hours(1));

    std::wstring error;
    Require(quickyeet::MigrateConfigFile(virtualized, real, &error),
            "A conflicting backup name should receive a suffix");
    Require(ReadFile(real.parent_path() /
                     L"config.pre-migration-virtualized-backup-2.yaml") == "virtual-old",
            "The numbered backup should preserve the newly displaced configuration");
}

void LeavesMigrationSourceIntactWhenDestinationFails() {
    TemporaryDirectory temporary;
    const fs::path virtualized = temporary.path() / L"virtual" / L"config.yaml";
    const fs::path blocked_parent = temporary.path() / L"blocked";
    const fs::path real = blocked_parent / L"config.yaml";
    fs::create_directories(virtualized.parent_path());
    WriteFile(virtualized, "virtualized");
    WriteFile(blocked_parent, "not-a-directory");

    std::wstring error;
    Require(!quickyeet::MigrateConfigFile(virtualized, real, &error),
            "An unwritable destination should fail migration");
    Require(!error.empty(), "A failed migration should report why it failed");
    Require(ReadFile(virtualized) == "virtualized",
            "A failed migration should leave the virtualized source intact");
}

void MovesWithoutOverwriting() {
    TemporaryDirectory temporary;
    const fs::path inbox = temporary.path() / L"inbox";
    const fs::path archive = temporary.path() / L"archive";
    fs::create_directories(inbox);
    fs::create_directories(archive);
    WriteFile(inbox / L"notes.txt", "new");
    WriteFile(archive / L"notes.txt", "existing");

    const auto results = quickyeet::MoveFilesWithCollisionSuffix({inbox / L"notes.txt"}, archive);

    Require(results.size() == 1 && results[0].succeeded(), "The move should succeed");
    Require(results[0].destination.filename() == L"notes (2).txt", "The first suffix should be (2)");
    Require(ReadFile(archive / L"notes.txt") == "existing", "The original file must be preserved");
    Require(ReadFile(archive / L"notes (2).txt") == "new", "The moved contents must be preserved");
    Require(!fs::exists(inbox / L"notes.txt"), "The source should no longer exist");
}

void MovesMultipleFilesTogether() {
    TemporaryDirectory temporary;
    const fs::path inbox = temporary.path() / L"inbox";
    const fs::path archive = temporary.path() / L"archive";
    fs::create_directories(inbox);
    fs::create_directories(archive);
    WriteFile(inbox / L"first.txt", "first");
    WriteFile(inbox / L"second.txt", "second");

    const auto results = quickyeet::MoveFilesWithCollisionSuffix(
        {inbox / L"first.txt", inbox / L"second.txt"}, archive);

    Require(results.size() == 2 && results[0].succeeded() && results[1].succeeded(),
            "Every selected file should move successfully");
    Require(ReadFile(archive / L"first.txt") == "first" &&
                ReadFile(archive / L"second.txt") == "second",
            "A multi-file move should preserve every file's contents");
    Require(!fs::exists(inbox / L"first.txt") && !fs::exists(inbox / L"second.txt"),
            "Every moved source should leave the original folder");
}

void AdvancesPastExistingSuffixes() {
    TemporaryDirectory temporary;
    const fs::path inbox = temporary.path() / L"inbox";
    const fs::path archive = temporary.path() / L"archive";
    fs::create_directories(inbox);
    fs::create_directories(archive);
    WriteFile(inbox / L"notes.txt", "new");
    WriteFile(archive / L"notes.txt", "one");
    WriteFile(archive / L"notes (2).txt", "two");

    const auto results = quickyeet::MoveFilesWithCollisionSuffix({inbox / L"notes.txt"}, archive);

    Require(results[0].succeeded(), "The move should succeed");
    Require(results[0].destination.filename() == L"notes (3).txt", "Suffixes should advance predictably");
}

void RejectsFoldersAsSources() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / L"source";
    const fs::path destination = temporary.path() / L"destination";
    fs::create_directories(source);
    fs::create_directories(destination);

    const auto results = quickyeet::MoveFilesWithCollisionSuffix({source}, destination);

    Require(results.size() == 1 && !results[0].succeeded(), "Folder moves are outside the file command");
    Require(fs::exists(source), "A rejected source folder must stay in place");
}

void RejectsInvalidRecycleSources() {
    TemporaryDirectory temporary;
    const fs::path folder = temporary.path() / L"folder";
    fs::create_directories(folder);

    Require(quickyeet::RecycleFiles({}) == ERROR_INVALID_PARAMETER,
            "Recycling requires at least one selected file");
    Require(quickyeet::RecycleFiles({folder}) != ERROR_SUCCESS,
            "QuickYeet should not send folders to Recycle Bin");
    Require(fs::exists(folder), "A rejected recycle source folder must stay in place");
}

void PersistsLocalDestinationConfig() {
    TemporaryDirectory temporary;
    const fs::path history_path = temporary.path() / L"state" / L"config.yaml";
    const fs::path client_work = temporary.path() / L"Client Work";
    fs::create_directories(client_work);

    quickyeet::HistoryStore saved(history_path);
    saved.RecordUse(client_work, 1'000);
    saved.RecordUse(client_work, 1'100);
    saved.SetAlias(client_work, L"Client \"M\u00F8ve\"");
    saved.SetPinned(client_work, true);
    std::wstring save_error;
    Require(saved.Save(&save_error), "History should save");

    quickyeet::HistoryStore loaded(history_path);
    std::wstring load_error;
    Require(loaded.Load(&load_error), "History should load");
    const auto record = loaded.Find(client_work);
    Require(record.has_value(), "The destination should survive a reload");
    Require(record->path == quickyeet::NormalizeDestinationPath(client_work),
            "The destination path should survive a reload");
    Require(record->use_count == 2 && record->last_used == 1'100, "Usage should survive a reload");
    Require(record->alias == L"Client \"M\u00F8ve\"" && record->pinned,
            "Display name and pin should survive a reload");
    const std::string yaml = ReadFile(history_path);
    Require(yaml.find("version: 1") != std::string::npos &&
                yaml.find("destinations:") != std::string::npos &&
                yaml.find("display_name: \"Client \\\"M") != std::string::npos &&
                yaml.find("pinned: true") != std::string::npos,
            "The saved destination configuration should be readable YAML");
}

void LoadsManuallyOrderedYamlConfig() {
    TemporaryDirectory temporary;
    const fs::path config_path = temporary.path() / L"config.yaml";
    const fs::path destination = temporary.path() / L"Manual Destination";
    fs::create_directories(destination);
    WriteFile(config_path,
              "\xEF\xBB\xBFversion: 1 # schema version\n"
              "destinations: # local destinations\n"
              "  - pinned: true\n"
              "    display_name: 'Manual # destination' # visible label\n"
              "    last_used: 42\n"
              "    path: '" + quickyeet::ToUtf8(destination.wstring()) + "'\n"
              "    use_count: 3\n");

    quickyeet::HistoryStore history(config_path);
    std::wstring error;
    Require(history.Load(&error), "A manually ordered YAML destination should load");
    const auto record = history.Find(destination);
    Require(record.has_value(), "The manually configured destination should be available");
    Require(record->alias == L"Manual # destination" && record->pinned &&
                record->use_count == 3 && record->last_used == 42,
            "Every manually configured destination field should load");
}

void PersistsEmptyYamlConfig() {
    TemporaryDirectory temporary;
    const fs::path config_path = temporary.path() / L"config.yaml";
    quickyeet::HistoryStore saved(config_path);
    std::wstring error;
    Require(saved.Save(&error), "An empty YAML configuration should save");
    Require(ReadFile(config_path).find("destinations: []") != std::string::npos,
            "An empty destination list should be represented as an empty YAML sequence");

    quickyeet::HistoryStore loaded(config_path);
    Require(loaded.Load(&error), "An empty YAML configuration should load");
    Require(loaded.Records().empty(), "An empty YAML configuration should stay empty");
}

void DefaultsRecycleBinOptionOn() {
    TemporaryDirectory temporary;
    const fs::path config_path = temporary.path() / L"config.yaml";
    WriteFile(config_path, "version: 1\ndestinations: []\n");

    quickyeet::HistoryStore history(config_path);
    std::wstring error;
    Require(history.Load(&error), "An existing YAML configuration should load");
    Require(history.RecycleBinEnabled(),
            "Recycle Bin should default on when an existing configuration omits the setting");
    Require(history.Save(&error), "The default Recycle Bin setting should save");
    Require(ReadFile(config_path).find("show_recycle_bin: true") != std::string::npos,
            "Saving should make the default Recycle Bin setting visible in YAML");
}

void HonorsDisabledRecycleBinOption() {
    TemporaryDirectory temporary;
    const fs::path config_path = temporary.path() / L"config.yaml";
    WriteFile(config_path,
              "version: 1\n"
              "show_recycle_bin: false\n"
              "destinations: []\n");

    quickyeet::HistoryStore history(config_path);
    std::wstring error;
    Require(history.Load(&error), "A disabled Recycle Bin setting should load");
    Require(!history.RecycleBinEnabled(), "The YAML setting should hide Recycle Bin");
    Require(history.Save(&error), "The disabled Recycle Bin setting should save");

    quickyeet::HistoryStore reloaded(config_path);
    Require(reloaded.Load(&error) && !reloaded.RecycleBinEnabled(),
            "The disabled Recycle Bin setting should survive a save and reload");
}

void RejectsInvalidRecycleBinOption() {
    TemporaryDirectory temporary;
    const fs::path config_path = temporary.path() / L"config.yaml";
    WriteFile(config_path,
              "version: 1\n"
              "show_recycle_bin: sometimes\n"
              "destinations: []\n");

    quickyeet::HistoryStore history(config_path);
    std::wstring error;
    Require(!history.Load(&error), "An invalid Recycle Bin setting should fail to load");
    Require(!error.empty() && history.RecycleBinEnabled(),
            "A rejected Recycle Bin setting should report an error and restore the safe default");
}

void MigratesLegacyDestinationConfig() {
    TemporaryDirectory temporary;
    const fs::path state = temporary.path() / L"state";
    const fs::path config_path = state / L"config.yaml";
    const fs::path legacy_path = state / L"destinations.tsv";
    const fs::path destination = temporary.path() / L"Legacy Destination";
    fs::create_directories(state);
    fs::create_directories(destination);
    const std::string legacy_contents =
        "quickyeet-v1\n1\t4\t1234\tLegacy destination\t" +
        quickyeet::ToUtf8(destination.wstring()) + "\n";
    WriteFile(legacy_path, legacy_contents);

    quickyeet::HistoryStore history(config_path);
    std::wstring error;
    Require(history.Load(&error), "The legacy destination configuration should migrate");
    const auto record = history.Find(destination);
    Require(record.has_value() && record->alias == L"Legacy destination" && record->pinned &&
                record->use_count == 4 && record->last_used == 1234,
            "Migration should preserve every destination field");
    Require(fs::exists(config_path), "Migration should create the YAML configuration");
    const fs::path backup_path = legacy_path.wstring() + L".backup";
    Require(fs::exists(backup_path),
            "Migration should preserve a backup of the legacy configuration");
    Require(ReadFile(backup_path) == legacy_contents,
            "The legacy backup should preserve the original bytes");
    Require(ReadFile(config_path).find("version: 1") != std::string::npos,
            "The migrated configuration should use YAML");
}

void PreservesLegacyConfigWhenMigrationFails() {
    TemporaryDirectory temporary;
    const fs::path state = temporary.path() / L"state";
    const fs::path config_path = state / L"config.yaml";
    const fs::path legacy_path = state / L"destinations.tsv";
    fs::create_directories(state);
    const std::string damaged = "quickyeet-v1\n1\tnot-a-number\t1234\tBroken\tC:\\Broken\n";
    WriteFile(legacy_path, damaged);

    quickyeet::HistoryStore history(config_path);
    std::wstring error;
    Require(!history.Load(&error), "A damaged legacy configuration should fail migration");
    Require(fs::exists(legacy_path) && ReadFile(legacy_path) == damaged,
            "A failed migration should leave the legacy configuration untouched");
    Require(!fs::exists(config_path) && !fs::exists(legacy_path.wstring() + L".backup"),
            "A failed migration should not create partial YAML or backup files");
}

void RejectsDamagedYamlConfig() {
    TemporaryDirectory temporary;
    const fs::path config_path = temporary.path() / L"config.yaml";
    WriteFile(config_path,
              "version: 1\n"
              "destinations:\n"
              "  - pinned: sometimes\n");

    quickyeet::HistoryStore history(config_path);
    std::wstring error;
    Require(!history.Load(&error), "An invalid YAML pin state should be rejected");
    Require(!error.empty() && history.Records().empty(),
            "A rejected YAML configuration should report an error and expose no partial records");
}

void KeepsConfigFileInConfigFolder() {
    const fs::path folder = quickyeet::HistoryStore::DefaultConfigFolder();
    const fs::path file = quickyeet::HistoryStore::DefaultStoragePath();
    Require(!folder.empty(), "The local configuration folder should be available");
    Require(file.parent_path() == folder,
            "The destination configuration should live in the configuration folder");
    Require(file.filename() == L"config.yaml", "The YAML configuration filename should be stable");
}

void RanksRecentAndFrequentDestinations() {
    TemporaryDirectory temporary;
    quickyeet::HistoryStore history(temporary.path() / L"config.yaml");
    const fs::path old_frequent = temporary.path() / L"old-frequent";
    const fs::path recent = temporary.path() / L"recent";
    const fs::path pinned = temporary.path() / L"pinned";
    constexpr std::int64_t now = 10'000'000;
    constexpr std::int64_t ninety_days = 90 * 86'400;
    for (int index = 0; index < 8; ++index) {
        history.RecordUse(old_frequent, now - ninety_days);
    }
    history.RecordUse(recent, now);
    history.RecordUse(pinned, now);
    history.SetPinned(pinned, true);

    const auto ranked = history.Recent(6, now);
    Require(ranked.size() == 2, "Pinned destinations should have their own section");
    Require(ranked[0].path == recent, "A fresh destination should rise above stale frequent history");
    Require(history.Pinned().size() == 1 && history.Pinned()[0].path == pinned,
            "Pinned destinations should be returned separately");
}

void RemovesDestinations() {
    TemporaryDirectory temporary;
    const fs::path destination = temporary.path() / L"destination";
    quickyeet::HistoryStore history(temporary.path() / L"config.yaml");
    history.RecordUse(destination, 1'000);
    Require(history.Remove(destination), "An existing destination should be removed");
    Require(!history.Find(destination), "A removed destination should not be found");
    Require(!history.Remove(destination), "Removing the same destination twice should be harmless");
}

}  // namespace

int wmain() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"moves without overwriting", MovesWithoutOverwriting},
        {"moves multiple files together", MovesMultipleFilesTogether},
        {"advances past existing suffixes", AdvancesPastExistingSuffixes},
        {"rejects folders as sources", RejectsFoldersAsSources},
        {"rejects invalid Recycle Bin sources", RejectsInvalidRecycleSources},
        {"migrates virtualized-only config", MigratesVirtualizedConfigWhenRealConfigIsMissing},
        {"keeps real-only config", LeavesExistingRealConfigWhenVirtualizedConfigIsMissing},
        {"avoids identical migration backups", AvoidsBackupsForIdenticalMigrationFiles},
        {"chooses newest migration config", ChoosesNewestMigrationConfigAndPreservesTheOther},
        {"prefers real config on migration tie", PrefersRealConfigWhenMigrationTimestampsTie},
        {"numbers conflicting migration backups", NumbersConflictingMigrationBackups},
        {"preserves migration source on failure", LeavesMigrationSourceIntactWhenDestinationFails},
        {"persists local destination config", PersistsLocalDestinationConfig},
        {"loads manually ordered YAML config", LoadsManuallyOrderedYamlConfig},
        {"persists empty YAML config", PersistsEmptyYamlConfig},
        {"defaults Recycle Bin option on", DefaultsRecycleBinOptionOn},
        {"honors disabled Recycle Bin option", HonorsDisabledRecycleBinOption},
        {"rejects invalid Recycle Bin option", RejectsInvalidRecycleBinOption},
        {"migrates legacy destination config", MigratesLegacyDestinationConfig},
        {"preserves legacy config when migration fails", PreservesLegacyConfigWhenMigrationFails},
        {"rejects damaged YAML config", RejectsDamagedYamlConfig},
        {"keeps config file in config folder", KeepsConfigFileInConfigFolder},
        {"ranks recent and frequent destinations", RanksRecentAndFrequentDestinations},
        {"removes destinations", RemovesDestinations},
    };

    size_t passed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
            std::cout << "PASS  " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL  " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << passed << '/' << tests.size() << " tests passed\n";
    return passed == tests.size() ? 0 : 1;
}

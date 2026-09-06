#ifndef ACCOUNT_MANAGEMENT_STORAGE_H
#define ACCOUNT_MANAGEMENT_STORAGE_H

#include "account_management_types.h"

#include <functional>
#include <string>

namespace account {

std::string account_bucket_for_name(const std::string& name);
std::string legacy_player_file_path(const std::string& root_directory, const std::string& character_name);
std::string legacy_object_file_path(const std::string& root_directory, const std::string& character_name);
std::string legacy_exploits_file_path(const std::string& root_directory, const std::string& character_name);
std::string account_file_path(const std::string& root_directory, const std::string& account_name);
std::string account_character_directory(const std::string& root_directory, const std::string& account_name, const std::string& character_name);
std::string account_character_snapshot_path(const std::string& root_directory, const std::string& account_name, const std::string& character_name);
std::string account_character_player_path(const std::string& root_directory, const std::string& account_name, const std::string& character_name);
std::string account_character_object_path(const std::string& root_directory, const std::string& account_name, const std::string& character_name);
std::string account_character_exploits_path(const std::string& root_directory, const std::string& account_name, const std::string& character_name);

std::string serialize_account_to_json(const AccountData& account);
bool deserialize_account_from_json(const std::string& json, AccountData* account, std::string* error_message = nullptr);

bool write_account_file(const std::string& root_directory, const AccountData& account, std::string* error_message = nullptr);
bool read_account_file(const std::string& root_directory, const std::string& account_name, AccountData* account, std::string* error_message = nullptr);
// Uncached on-disk read (the real scan). read_account_file delegates here when the cache is disabled,
// and it is the cache's backing resolver on a miss. Call directly to bypass the cache.
bool read_account_file_uncached(const std::string& root_directory, const std::string& account_name, AccountData* account, std::string* error_message = nullptr);
bool read_account_file_by_email(const std::string& root_directory, const std::string& email, AccountData* account, std::string* error_message = nullptr);
bool read_account_file_by_identifier(const std::string& root_directory, const std::string& identifier, AccountData* account, std::string* error_message = nullptr);

// One account record found on disk, in either supported layout.
struct AccountRecordOnDisk {
    // The bucket entry name: "<email>" for the directory layout, "<name>.json" for the legacy flat one.
    std::string directory_entry_name;
    // Full path of the JSON actually read.
    std::string record_path;
    // Whether the record parsed. When false, `account` is meaningless and failure_reason says why.
    bool parsed = false;
    // True when this record came from the directory layout (<email>/account.json), false for the
    // legacy flat layout (<name>.json). Taken from stat(), not guessed from the entry name.
    bool directory_layout = false;
    AccountData account;
    std::string failure_reason;
};

// Walks accounts/ and visits every record in either layout, parsed or not. Visits one record at a
// time rather than returning them all: at boot this runs over every account on the box, and holding
// every parsed AccountData at once would be a real memory spike on a machine that already swaps.
// Returns false only when the accounts directory itself cannot be read; an individual bad record is
// reported to the visitor with parsed == false, never as a failure of the walk.
bool for_each_account_record_on_disk(const std::string& root_directory,
    const std::function<void(const AccountRecordOnDisk&)>& visitor,
    std::string* error_message = nullptr);

// The key a record is (or should be) indexed/quarantined under, derived purely from the record
// itself. Directory-layout records (parsed or not) are keyed by their entry name, which IS the
// email even when the file fails to parse. A legacy flat record that parsed is keyed by the email
// it actually contains. A legacy flat record that did NOT parse has revealed no email at all -- its
// entry name is "<name>.json", not an address -- so it is keyed by its own record path instead: an
// unparseable flat record must not reserve an email it never disclosed. `directory_layout` comes
// straight from stat() in the enumerator, not guessed from the entry name.
//
// Shared by the boot walker (db.cpp) and the `account index verify` visitor (act_wiz.cpp) so both
// agree on what an unparsed/quarantined record is keyed by -- two independent derivations of this
// rule is exactly how a prior bug (verify reporting false drift for quarantined records) got in.
std::string account_index_quarantine_key(const AccountRecordOnDisk& record);

std::string serialize_character_migration_to_json(const CharacterMigrationData& migration);
bool deserialize_character_migration_from_json(const std::string& json, CharacterMigrationData* migration, std::string* error_message = nullptr);

// Read an entire text file into *contents (POSIX-backed). Exposed for stage-timing the
// LOAD pipeline's file-read step.
bool read_text_file(const std::string& path, std::string* contents, std::string* error_message);

// Atomic write: temp(path+".tmp") -> fwrite -> rename. Exposed for stage-timing the SAVE
// pipeline's disk-write step against a throwaway path.
bool write_text_file_atomically(const std::string& path, const std::string& text,
    std::string* error_message);

} // namespace account

#endif

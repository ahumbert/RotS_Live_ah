#ifndef ACCOUNT_INDEX_H
#define ACCOUNT_INDEX_H

#include "account_management_types.h"

#include <cstddef>
#include <string>
#include <vector>

// In-memory index over the account records on disk. Holds KEYS ONLY: it answers "which file holds
// the account for this email / account name / character name", never what is inside that file.
// Reads of the record itself still go to disk, which is what keeps this bounded in memory and keeps
// the file the single source of truth.
namespace account_index {

// One indexed record. A quarantined entry still occupies its email so that a record we could not
// parse cannot be silently overwritten by a fresh account created at the same address.
struct Entry {
    std::string normalized_email;
    std::string record_path;
    std::string normalized_account_name;
    bool quarantined = false;
    std::string quarantine_reason;
};

// Re-derives every key this record owns from the record itself, dropping any key it owned before.
// Correct across link, unlink, rename and account-name change without diffing old against new.
void upsert(const account::AccountData& account, const std::string& record_path);

// Records an account file we could not use, keyed by its directory name. Its email stays occupied.
void quarantine(const std::string& normalized_email, const std::string& record_path,
    const std::string& reason);

// Lookups. Each returns false and sets *error_message (when non-null) if the key is unknown or the
// record behind it is quarantined.
bool find_path_by_email(const std::string& email, std::string* record_path,
    std::string* error_message);
bool find_path_by_account_name(const std::string& account_name, std::string* record_path,
    std::string* error_message);
bool find_owner_email_by_character(const std::string& character_name, std::string* owner_email,
    std::string* error_message);

// Resolves an account name to the email that keys its record. Returns false when the name is
// unknown or its record is quarantined. Exists because a caller that needs the storage key must not
// have to parse it back out of the record path — that yields the bucket name for a legacy flat
// record, which is silently wrong.
bool find_email_by_account_name(const std::string& account_name, std::string* email,
    std::string* error_message);

bool is_quarantined(const std::string& email);
std::vector<Entry> quarantined_entries();

// Number of indexed records (not keys).
std::size_t size();

void clear();

// Whether the account resolvers consult this index. Default OFF so the test binary and any
// non-server caller keep the exact directory-scanning behaviour; the live server turns it on at
// boot once the index has been built.
void set_enabled(bool enabled);
bool is_enabled();

} // namespace account_index

#endif

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

// Boot refuses to continue past this many unusable account records. The threshold is a bug
// detector, not a corruption tolerance: the write path cannot produce a torn file, so the realistic
// causes of an unreadable record are ours (a serialization change, a normalize_email change, a
// rollback to a binary that rejects a newer field) and they hit many records at once. One is a
// genuine one-off and must not take the game down; six means we shipped something.
static constexpr std::size_t MAX_QUARANTINED_RECORDS_AT_BOOT = 5;

// One indexed record. A quarantined entry still occupies its email so that a record we could not
// parse cannot be silently overwritten by a fresh account created at the same address.
struct Entry {
    std::string normalized_email;
    std::string record_path;
    std::string normalized_account_name;
    bool quarantined = false;
    std::string quarantine_reason;
    // True when this entry came from the legacy flat layout (accounts/<bucket>/<name>.json) rather
    // than the directory layout. Used only to enforce upsert's directory-over-flat precedence.
    bool legacy_flat_layout = false;
};

// Re-derives every key this record owns from the record itself, dropping any key it owned before.
// Correct across link, unlink, rename and account-name change without diffing old against new.
//
// `legacy_flat_layout` marks a record read from accounts/<bucket>/<name>.json. When a record already
// indexed under this email came from the directory layout, a legacy flat record for the same email
// is IGNORED: the directory layout is authoritative, matching the precedence
// find_account_file_path_by_account_name applies (account_management.cpp:834ff). Without this,
// readdir order decides which of the two owns the lookup. A directory record always overwrites a
// flat one, regardless of arrival order.
void upsert(const account::AccountData& account, const std::string& record_path,
    bool legacy_flat_layout = false);

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
std::size_t quarantined_count();

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

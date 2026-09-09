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
// causes of an unreadable record are ours (a serialization change, a normalize_email change) and
// they hit many records at once. One is a genuine one-off and must not take the game down; six
// means we shipped something.
static constexpr std::size_t MAX_QUARANTINED_RECORDS_AT_BOOT = 5;

// One indexed record. A quarantined entry still occupies its email so that a record we could not
// parse cannot be silently overwritten by a fresh account created at the same address.
struct Entry {
    std::string normalized_email;
    std::string record_path;
    std::string normalized_account_name;
    bool quarantined = false;
    std::string quarantine_reason;
    // Set when a record that indexed cleanly at boot later failed to read. Report-only: unlike
    // quarantine this withdraws NOTHING, so a player whose record hiccups keeps resolving and
    // keeps saving. See note_unreadable_at_runtime.
    bool unreadable_at_runtime = false;
    std::string unreadable_at_runtime_reason;
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
// NOTE: there is deliberately NO erase API, only upsert / quarantine / clear. Nothing in the system
// deletes an account record or changes an account's email today -- character deletion rewrites the
// account record, it does not remove one -- so no entry here can go stale. Adding either capability
// REQUIRES adding an erase path first: without one the index keeps serving a path to a file that no
// longer exists, and find_account_by_email_internal then answers "Failed to open account file ..."
// where the caller at interpre.cpp:3030 compares against "No account exists for that email address."
// to offer account creation. The create-account branch would silently stop working.
void upsert(const account::AccountData& account, const std::string& record_path,
    bool legacy_flat_layout = false);

// Marks a record the server failed to read AFTER boot, so it shows up in `account index` instead of
// failing silently. Returns true only the FIRST time a given record is marked, which is how the
// caller knows to log: the write chokepoint runs on every successful login, so logging
// unconditionally would repeat the same line forever for one bad file.
//
// Deliberately NOT quarantine(). Quarantine withdraws the record's account-name and character
// claims, and there is no way back short of a reboot -- so a transient EIO or a moment of EACCES
// would drop a live player's character key, and save_char would then write NOTHING for them,
// silently, until the next boot. That is the very loss this subsystem exists to prevent. This marks
// and reports; it changes no lookup's answer.
//
// Only marks a record the index already holds. A key with no entry is not invented here: an entry
// carries a record path that find_path_by_email would then hand out.
bool note_unreadable_at_runtime(const std::string& record_key, const std::string& reason);

// Every record marked by note_unreadable_at_runtime, for the wizard listing.
std::vector<Entry> unreadable_at_runtime_entries();
std::size_t unreadable_at_runtime_count();

// Records an account file we could not use. NOTHING ON DISK IS TOUCHED: despite the name this moves,
// copies, renames and writes no file. It drops the record's keys from the lookup maps, clears any
// contention it was part of, and stores the reason -- so the index refuses to resolve it while its
// address stays reserved against a fresh account being created over a real player's record. The
// state lives only as long as the process; the next boot walk decides again from the files as they
// are. Keyed by whatever key the caller passes -- normally
// account::account_index_quarantine_key()'s result. That key is NOT re-normalized here: for the two
// email-shaped cases it already IS normalize_email()'d, and for the two path-shaped cases (an
// unparsed or emailless legacy flat record, keyed by its own record_path) it is a case-sensitive
// filesystem path that lowercasing would silently corrupt -- account_index_quarantine_key's own doc
// comment enumerates exactly which shape each case is. Its email stays occupied.
void quarantine(const std::string& normalized_email, const std::string& record_path,
    const std::string& reason);

// Lookups. Each returns false and sets *error_message (when non-null) if the key is unknown or the
// record behind it is quarantined. The account-name lookups additionally refuse an ambiguous name
// (see is_account_name_ambiguous).
bool find_path_by_email(const std::string& email, std::string* record_path,
    std::string* error_message);
bool find_path_by_account_name(const std::string& account_name, std::string* record_path,
    std::string* error_message);
// `owner_account_name`, when non-null, is filled with the owning record's normalized account name --
// the same string a read of that record would yield, because deserialize_account_from_json
// normalizes account_name with the very normalize_account_name() applied at upsert
// (account_management_storage.cpp:151). It exists so the owner resolver does not have to re-read and
// re-parse the record file just to name the account it already resolved; see
// find_linked_character_owner_account_uncached.
bool find_owner_email_by_character(const std::string& character_name, std::string* owner_email,
    std::string* error_message, std::string* owner_account_name = nullptr);

// Resolves an account name to the email that keys its record. Returns false when the name is
// unknown or its record is quarantined. Exists because a caller that needs the storage key must not
// have to parse it back out of the record path — that yields the bucket name for a legacy flat
// record, which is silently wrong.
bool find_email_by_account_name(const std::string& account_name, std::string* email,
    std::string* error_message);

// True when more than one email claims this account name. A map cannot represent that, and
// resolving to one of them is destructive rather than merely lossy: write_account_file deletes the
// path find_account_file_path_by_account_name returns when it differs from its target, so the loser
// would lose its file. The directory scan refused to answer for such a name and so do the two
// account-name lookups above, with that scan's exact text.
//
// Contention is tracked as the SET of claimants, not as a sticky flag, so it lowers again the moment
// the duplicate is repaired on disk and the repaired record is written: an ambiguity that only a
// reboot could clear is one that keeps refusing lookups long after the operator fixed the data.
bool is_account_name_ambiguous(const std::string& account_name);

// True when more than one account record lists this character. The same shape as the account-name
// case and the more dangerous of the two: save_char (db.cpp) picks the directory it writes a
// character file into from the owner this resolves to, and CREATES one there when the winner has no
// such file, so silently resolving to whichever record was upserted last migrates a player's saves
// into an account that does not own them. find_owner_email_by_character refuses for such a name with
// find_character_owner_account's exact text.
//
// This is the one that has to lower again: a refusal here makes save_char write NOTHING for that
// character, silently and to the log only. Unlinking the character from one of the two records and
// writing that record withdraws its claim, which drops the key back to a single claimant and lets
// the survivor resolve immediately -- no reboot.
bool is_character_ambiguous(const std::string& character_name);

// True when more than one account record claims this email with a DIFFERENT account name -- two
// legacy flat records, in practice, since a directory record's path is derived from its email so two
// of them cannot coexist. Same-name duplicates across the two layouts are not ambiguous: that is the
// ordinary flat-plus-directory pair the scan (and upsert's precedence rule) deduplicates.
// find_path_by_email refuses for such an email with find_account_by_email_internal's exact text.
//
// Claimants here are RECORD PATHS carrying the account name each one declares, since the contested
// thing is the address and the records disputing it are distinguished by where they live. Contested
// means the claims disagree about the account name -- the scan's own duplicate test -- so an address
// heals as soon as a rewrite makes the names agree, or when the surviving record is quarantined.
bool is_email_ambiguous(const std::string& email);

// One contested key, for display. `kind` is "character", "account name" or "email"; `claimants` are
// the emails disputing it (record paths, for the email kind), sorted. Exists because a contested
// character key silently stops that character's saves: a state that dangerous has to be visible from
// in-game, not only inferable from the log.
struct ContestedKey {
    std::string kind;
    std::string key;
    std::vector<std::string> claimants;
};

// Every contested key, sorted by kind then key so the wizard listing is stable between calls.
std::vector<ContestedKey> contested_keys();

bool is_quarantined(const std::string& email);

// Exact-key quarantine lookup: takes the key a record is filed under (account_index_quarantine_key's
// result) and does NOT normalize it, exactly as quarantine() does not. is_quarantined() above is the
// email-shaped door onto the same map and normalizes its argument, which silently corrupts the two
// path-shaped quarantine keys (a case-sensitive filesystem path with an uppercase bucket letter).
// Callers that already hold a record's index key must use this one.
bool is_quarantined_record_key(const std::string& record_key);

std::vector<Entry> quarantined_entries();
std::size_t quarantined_count();

// Number of indexed records (not keys).
std::size_t size();

void clear();

// The root directory every indexed record_path was composed against ("." for boot_db and every live
// call site). The resolvers ignore their own root_directory argument once they answer from here, so
// each fast path falls through to its directory scan when the caller's root does not match this one
// -- a root mismatch is a programming mistake, not an unknown-key lookup, so falling back to the
// (correct, slower) scan is the safe response. clear() resets this to ".".
void set_root_directory(const std::string& root_directory);
const std::string& root_directory();
bool matches_root(const std::string& root_directory);

// Whether the account resolvers consult this index. Default OFF so the test binary and any
// non-server caller keep the exact directory-scanning behaviour; the live server turns it on at
// boot once the index has been built.
void set_enabled(bool enabled);
bool is_enabled();

} // namespace account_index

#endif

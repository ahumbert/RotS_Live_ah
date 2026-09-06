#include "account_index.h"

#include "account_management_identity.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace account_index {

namespace {

    // email -> entry. The email is the storage key on disk, so it is the identity here too.
    std::unordered_map<std::string, Entry> g_entries;
    // normalized account name -> email.
    std::unordered_map<std::string, std::string> g_account_names;
    // normalized character name -> email.
    std::unordered_map<std::string, std::string> g_characters;
    // email -> the character keys that email currently owns, so upsert can drop what it no longer owns.
    std::unordered_map<std::string, std::vector<std::string>> g_owned_characters;
    // Account names claimed by more than one email. A map cannot represent that, and silently
    // resolving to whichever record was upserted last is destructive, not merely lossy:
    // write_account_file std::remove()s the path find_account_file_path_by_account_name returns when
    // it differs from the target (account_management_storage.cpp:240-245), so the record that lost
    // the race would have its file deleted. The directory scan refused to answer at all in this
    // situation, and so does the index -- see find_path_by_account_name.
    std::unordered_set<std::string> g_ambiguous_account_names;
    // Character keys claimed by more than one account record. Same shape as the account-name set
    // above and the same reason for existing, except that the consequence is worse: save_char
    // (db.cpp:3269-3296) chooses the directory it writes a character file into from the owner this
    // index resolves, and the branch at db.cpp:3286 CREATES that file when the resolved account has
    // none -- so answering with whichever record was upserted last would migrate a player's saves
    // into an account that does not own the character, and would flip on every write to either
    // record. find_character_owner_account refused to answer here, and so does
    // find_owner_email_by_character.
    //
    // Sticky until clear(): once two records claim a character, nothing short of a reboot un-flags
    // it. That is deliberate and matches g_ambiguous_account_names. Only the boot walk can produce
    // this state today -- admin_link_character/admin_link_and_migrate_character
    // (account_management_identity.cpp:983,1019) refuse to link a character another account already
    // owns -- so a live write cannot raise the flag, and the state it describes is on disk and has
    // to be repaired there.
    std::unordered_set<std::string> g_ambiguous_characters;
    // Emails claimed by more than one record under different account names. Mirrors
    // find_account_by_email_internal's own duplicate handling (account_management.cpp:1060-1075):
    // two records at one address are a duplicate only when their account NAMES differ; when they
    // agree it is the ordinary legacy-flat-plus-directory pair, which that scan deduplicates by
    // preferring the directory record and upsert's precedence rule resolves the same way.
    std::unordered_set<std::string> g_ambiguous_emails;

    bool g_enabled = false;
    // The root directory every record_path in here was composed against. The resolvers ignore their
    // own root_directory argument on the fast path, so answering a caller working against a
    // different tree would hand back paths from this one. "." is what boot_db and every live call
    // site use.
    std::string g_root_directory = ".";

    void set_error(std::string* error_message, const std::string& text)
    {
        if (error_message != nullptr)
            *error_message = text;
    }

    // Drops every key owned by this email except the entry itself.
    void erase_owned_keys(const std::string& email)
    {
        for (auto name_entry = g_account_names.begin(); name_entry != g_account_names.end();) {
            if (name_entry->second == email)
                name_entry = g_account_names.erase(name_entry);
            else
                ++name_entry;
        }

        const auto owned = g_owned_characters.find(email);
        if (owned != g_owned_characters.end()) {
            for (const std::string& character_key : owned->second) {
                const auto character_entry = g_characters.find(character_key);
                if (character_entry != g_characters.end() && character_entry->second == email)
                    g_characters.erase(character_entry);
            }
            g_owned_characters.erase(owned);
        }
    }

} // namespace

void upsert(const account::AccountData& account, const std::string& record_path,
    bool legacy_flat_layout)
{
    const std::string email = account::normalize_email(account.normalized_email);
    if (email.empty())
        return;

    const std::string normalized_account_name = account::normalize_account_name(account.account_name);

    const auto existing = g_entries.find(email);
    // Two DIFFERENT records at one address, disagreeing about the account name: the case
    // find_account_by_email_internal refuses to answer for. Tested before the precedence return
    // below, because the flat-record-ignored case is one of the two ways this arises. Three
    // conditions, each load-bearing:
    //   - a differing record_path is what makes this two records rather than one being rewritten.
    //     A live write cannot fail it: final_path is composed purely from the email, so every
    //     rewrite (an account rename included) lands on the path already indexed.
    //   - a differing account name is the scan's own duplicate test; equal names are the ordinary
    //     flat-plus-directory pair, deduplicated by precedence, not a duplicate.
    //   - a quarantined incumbent is excluded: it holds no account name to compare and its email is
    //     deliberately reserved, so "could not be read" must stay the answer for that address
    //     rather than being reworded into a duplicate report.
    if (existing != g_entries.end() && !existing->second.quarantined
        && existing->second.record_path != record_path
        && existing->second.normalized_account_name != normalized_account_name)
        g_ambiguous_emails.insert(email);

    // Directory-over-flat precedence: a legacy flat record must not displace a directory record (or
    // a quarantined directory record -- quarantine() leaves legacy_flat_layout at its default of
    // false) already indexed under the same email. Without this, whichever one readdir happens to
    // visit last would silently win.
    if (legacy_flat_layout && existing != g_entries.end() && !existing->second.legacy_flat_layout)
        return;

    erase_owned_keys(email);

    Entry entry;
    entry.normalized_email = email;
    entry.record_path = record_path;
    entry.normalized_account_name = normalized_account_name;
    entry.legacy_flat_layout = legacy_flat_layout;
    g_entries[email] = entry;

    if (!entry.normalized_account_name.empty()) {
        // erase_owned_keys above already dropped any name key this email owned, so a key that is
        // still here belongs to a DIFFERENT email: two records claim one account name. Record that
        // and let the lookups refuse, exactly as the scan did. Two layouts of the SAME email are not
        // a duplicate -- that case never reaches here, it is either the early return above or an
        // email whose own key was just erased.
        const auto existing_name = g_account_names.find(entry.normalized_account_name);
        if (existing_name != g_account_names.end() && existing_name->second != email)
            g_ambiguous_account_names.insert(entry.normalized_account_name);
        g_account_names[entry.normalized_account_name] = email;
    }

    std::vector<std::string> owned;
    owned.reserve(account.characters.size());
    for (const std::string& character_name : account.characters) {
        const std::string character_key = account::normalize_account_name(character_name);
        if (character_key.empty())
            continue;
        // erase_owned_keys above already dropped every character key this email owned, so a key
        // still present belongs to a DIFFERENT email: two records claim one character. Record it and
        // let find_owner_email_by_character refuse, exactly as the scan did -- overwriting instead
        // would send this character's saves to whichever record was written last.
        const auto existing_character = g_characters.find(character_key);
        if (existing_character != g_characters.end() && existing_character->second != email)
            g_ambiguous_characters.insert(character_key);
        g_characters[character_key] = email;
        owned.push_back(character_key);
    }
    g_owned_characters[email] = std::move(owned);
}

void quarantine(const std::string& normalized_email, const std::string& record_path,
    const std::string& reason)
{
    // Deliberately NOT re-run through normalize_email: the caller's key is already final. For the
    // two email-shaped quarantine cases it is already normalize_email()'d (normalizing an
    // already-normalized email is a harmless no-op, so this is not a behavior change for them), but
    // for the two path-shaped cases (an unparsed or emailless legacy flat record, keyed by its own
    // record_path) the key is a case-sensitive filesystem path -- lowercasing it here previously
    // silently produced a key ("accounts/k-o/...") that never matched the record's real path
    // ("accounts/K-O/...", bucket letters are always uppercase), which made `account index verify`
    // report false drift for that quarantine shape even after both callers agreed on the same key
    // rule.
    if (normalized_email.empty())
        return;

    erase_owned_keys(normalized_email);

    Entry entry;
    entry.normalized_email = normalized_email;
    entry.record_path = record_path;
    entry.quarantined = true;
    entry.quarantine_reason = reason;
    g_entries[normalized_email] = entry;
}

bool find_path_by_email(const std::string& email, std::string* record_path,
    std::string* error_message)
{
    const std::string normalized_email = account::normalize_email(email);
    if (g_ambiguous_emails.count(normalized_email) != 0) {
        // Verbatim find_account_by_email_internal's duplicate text (account_management.cpp:1073).
        set_error(error_message, "Multiple account records exist for that email address.");
        return false;
    }

    const auto entry = g_entries.find(normalized_email);
    if (entry == g_entries.end()) {
        set_error(error_message, "No account exists for that email address.");
        return false;
    }
    if (entry->second.quarantined) {
        set_error(error_message, "That account record could not be read.");
        return false;
    }
    if (record_path != nullptr)
        *record_path = entry->second.record_path;
    set_error(error_message, "");
    return true;
}

bool find_path_by_account_name(const std::string& account_name, std::string* record_path,
    std::string* error_message)
{
    const std::string normalized_account_name = account::normalize_account_name(account_name);
    if (g_ambiguous_account_names.count(normalized_account_name) != 0) {
        // Verbatim find_account_file_path_by_account_name's duplicate text (account_management.cpp).
        // Returning a path here would let write_account_file delete the other record's file.
        set_error(error_message, "Multiple account records exist for account '" + normalized_account_name + "'.");
        return false;
    }

    const auto name_entry = g_account_names.find(normalized_account_name);
    if (name_entry == g_account_names.end()) {
        // Verbatim the text find_account_file_path_by_account_name sets when its directory scan
        // finds no match (account_management.cpp). This function replaces that scan, so callers --
        // and the tests that assert on the message -- must not be able to tell which one answered.
        set_error(error_message, "Failed to open account file for account '" + normalized_account_name + "': " + std::strerror(ENOENT));
        return false;
    }
    // Note that this inherits find_path_by_email's refusal when the resolved email is claimed by two
    // records under different names -- with that function's message, not this one's. The name scan
    // would have answered (it matches on the name, which is unique in that configuration), so this
    // is a deliberate divergence in the refusing direction: only one of the two records is indexed
    // under the email, so a path returned here could well be the other record's, and both callers
    // that matter treat a refusal as "no existing file" (write_account_file then retires nothing,
    // create_account then proceeds exactly as it does after the scan's own duplicate-email refusal).
    return find_path_by_email(name_entry->second, record_path, error_message);
}

bool find_owner_email_by_character(const std::string& character_name, std::string* owner_email,
    std::string* error_message)
{
    const std::string character_key = account::normalize_account_name(character_name);
    if (g_ambiguous_characters.count(character_key) != 0) {
        // Verbatim find_character_owner_account's duplicate text (account_management.cpp:979).
        // Setting a message is what makes this the ERROR form for the caller: the owner resolver
        // (account_management_identity.cpp:937-949) reads an EMPTY message as "resolved, this
        // character is linked to no account", which save_char would act on by treating the character
        // as unlinked -- the exact silent mis-save this refusal exists to prevent.
        set_error(error_message, "Multiple account records claim that linked character.");
        return false;
    }

    const auto character_entry = g_characters.find(character_key);
    if (character_entry == g_characters.end()) {
        set_error(error_message, "");
        return false;
    }

    const auto entry = g_entries.find(character_entry->second);
    if (entry == g_entries.end() || entry->second.quarantined) {
        set_error(error_message, "That account record could not be read.");
        return false;
    }

    if (owner_email != nullptr)
        *owner_email = character_entry->second;
    set_error(error_message, "");
    return true;
}

bool find_email_by_account_name(const std::string& account_name, std::string* email,
    std::string* error_message)
{
    const std::string normalized_account_name = account::normalize_account_name(account_name);
    if (g_ambiguous_account_names.count(normalized_account_name) != 0) {
        set_error(error_message, "Multiple account records exist for account '" + normalized_account_name + "'.");
        return false;
    }

    const auto name_entry = g_account_names.find(normalized_account_name);
    if (name_entry == g_account_names.end()) {
        // Deliberately NOT the scan text used by find_path_by_account_name above: the scan this one
        // replaces (resolve_account_storage_key) reports nothing at all, it just returns "". Its
        // only caller passes a null error_message, so this string is never shown to a player.
        set_error(error_message, "No account exists with that name.");
        return false;
    }

    const auto entry = g_entries.find(name_entry->second);
    if (entry == g_entries.end() || entry->second.quarantined) {
        set_error(error_message, "That account record could not be read.");
        return false;
    }

    if (email != nullptr)
        *email = name_entry->second;
    set_error(error_message, "");
    return true;
}

bool is_quarantined(const std::string& email)
{
    return is_quarantined_record_key(account::normalize_email(email));
}

bool is_quarantined_record_key(const std::string& record_key)
{
    const auto entry = g_entries.find(record_key);
    return entry != g_entries.end() && entry->second.quarantined;
}

std::vector<Entry> quarantined_entries()
{
    std::vector<Entry> quarantined;
    for (const auto& entry : g_entries) {
        if (entry.second.quarantined)
            quarantined.push_back(entry.second);
    }
    return quarantined;
}

std::size_t quarantined_count()
{
    std::size_t count = 0;
    for (const auto& entry : g_entries) {
        if (entry.second.quarantined)
            ++count;
    }
    return count;
}

std::size_t size()
{
    return g_entries.size();
}

bool is_account_name_ambiguous(const std::string& account_name)
{
    return g_ambiguous_account_names.count(account::normalize_account_name(account_name)) != 0;
}

bool is_character_ambiguous(const std::string& character_name)
{
    return g_ambiguous_characters.count(account::normalize_account_name(character_name)) != 0;
}

bool is_email_ambiguous(const std::string& email)
{
    return g_ambiguous_emails.count(account::normalize_email(email)) != 0;
}

void clear()
{
    g_entries.clear();
    g_account_names.clear();
    g_characters.clear();
    g_owned_characters.clear();
    g_ambiguous_account_names.clear();
    g_ambiguous_characters.clear();
    g_ambiguous_emails.clear();
    g_root_directory = ".";
}

void set_root_directory(const std::string& root_directory)
{
    g_root_directory = root_directory;
}

const std::string& root_directory()
{
    return g_root_directory;
}

bool matches_root(const std::string& root_directory)
{
    return root_directory == g_root_directory;
}

void set_enabled(bool enabled)
{
    g_enabled = enabled;
}

bool is_enabled()
{
    return g_enabled;
}

std::vector<std::string> rebuild_report(const std::vector<Entry>& records_on_disk)
{
    std::vector<std::string> disagreements;
    std::unordered_set<std::string> seen;

    for (const Entry& record : records_on_disk) {
        seen.insert(record.normalized_email);
        const auto indexed = g_entries.find(record.normalized_email);
        if (indexed == g_entries.end()) {
            disagreements.push_back("missing from index: " + record.normalized_email);
            continue;
        }
        if (indexed->second.quarantined) {
            // The index correctly has this record marked unreadable, and the record is right here
            // on disk under the same key -- that IS the index agreeing with disk about a file it
            // cannot parse, not drift. `account index` on its own already lists every quarantined
            // record; verify's job is to find disagreement, and there isn't any here.
            continue;
        }
        if (indexed->second.record_path != record.record_path) {
            disagreements.push_back("path differs for " + record.normalized_email + ": index has "
                + indexed->second.record_path + ", disk has " + record.record_path);
        }
        if (indexed->second.normalized_account_name != record.normalized_account_name) {
            disagreements.push_back("account name differs for " + record.normalized_email + ": index has "
                + indexed->second.normalized_account_name + ", disk has " + record.normalized_account_name);
        }
    }

    for (const auto& indexed : g_entries) {
        if (seen.find(indexed.first) == seen.end())
            disagreements.push_back("in index but not on disk: " + indexed.first);
    }

    return disagreements;
}

} // namespace account_index

#include "account_index.h"

#include "account_management_identity.h"

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

    bool g_enabled = false;

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

void upsert(const account::AccountData& account, const std::string& record_path)
{
    const std::string email = account::normalize_email(account.normalized_email);
    if (email.empty())
        return;

    erase_owned_keys(email);

    Entry entry;
    entry.normalized_email = email;
    entry.record_path = record_path;
    entry.normalized_account_name = account::normalize_account_name(account.account_name);
    g_entries[email] = entry;

    if (!entry.normalized_account_name.empty())
        g_account_names[entry.normalized_account_name] = email;

    std::vector<std::string> owned;
    owned.reserve(account.characters.size());
    for (const std::string& character_name : account.characters) {
        const std::string character_key = account::normalize_account_name(character_name);
        if (character_key.empty())
            continue;
        g_characters[character_key] = email;
        owned.push_back(character_key);
    }
    g_owned_characters[email] = std::move(owned);
}

void quarantine(const std::string& normalized_email, const std::string& record_path,
    const std::string& reason)
{
    const std::string email = account::normalize_email(normalized_email);
    if (email.empty())
        return;

    erase_owned_keys(email);

    Entry entry;
    entry.normalized_email = email;
    entry.record_path = record_path;
    entry.quarantined = true;
    entry.quarantine_reason = reason;
    g_entries[email] = entry;
}

bool find_path_by_email(const std::string& email, std::string* record_path,
    std::string* error_message)
{
    const auto entry = g_entries.find(account::normalize_email(email));
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
    const auto name_entry = g_account_names.find(account::normalize_account_name(account_name));
    if (name_entry == g_account_names.end()) {
        set_error(error_message, "No account exists with that name.");
        return false;
    }
    return find_path_by_email(name_entry->second, record_path, error_message);
}

bool find_owner_email_by_character(const std::string& character_name, std::string* owner_email,
    std::string* error_message)
{
    const auto character_entry = g_characters.find(account::normalize_account_name(character_name));
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
    const auto name_entry = g_account_names.find(account::normalize_account_name(account_name));
    if (name_entry == g_account_names.end()) {
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
    const auto entry = g_entries.find(account::normalize_email(email));
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

void clear()
{
    g_entries.clear();
    g_account_names.clear();
    g_characters.clear();
    g_owned_characters.clear();
}

void set_enabled(bool enabled)
{
    g_enabled = enabled;
}

bool is_enabled()
{
    return g_enabled;
}

} // namespace account_index

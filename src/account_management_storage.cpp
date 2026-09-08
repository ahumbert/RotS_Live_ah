std::string account_bucket_for_name(const std::string& name)
{
    const std::string normalized_name = normalize_account_name(name);
    if (normalized_name.empty())
        return "ZZZ";

    switch (normalized_name[0]) {
    case 'a':
    case 'b':
    case 'c':
    case 'd':
    case 'e':
        return "A-E";
    case 'f':
    case 'g':
    case 'h':
    case 'i':
    case 'j':
        return "F-J";
    case 'k':
    case 'l':
    case 'm':
    case 'n':
    case 'o':
        return "K-O";
    case 'p':
    case 'q':
    case 'r':
    case 's':
    case 't':
        return "P-T";
    case 'u':
    case 'v':
    case 'w':
    case 'x':
    case 'y':
    case 'z':
        return "U-Z";
    default:
        return "ZZZ";
    }
}

std::string account_file_path(const std::string& root_directory, const std::string& account_name)
{
    return account_file_path_from_email(root_directory, account_name);
}

std::string legacy_player_file_path(const std::string& root_directory, const std::string& character_name)
{
    const std::string normalized_name = normalize_account_name(character_name);
    return root_directory + "/players/" + account_bucket_for_name(normalized_name) + "/" + normalized_name;
}

std::string legacy_object_file_path(const std::string& root_directory, const std::string& character_name)
{
    const std::string normalized_name = normalize_account_name(character_name);
    return root_directory + "/plrobjs/" + account_bucket_for_name(normalized_name) + "/" + normalized_name + ".obj";
}

std::string legacy_exploits_file_path(const std::string& root_directory, const std::string& character_name)
{
    const std::string normalized_name = normalize_account_name(character_name);
    return root_directory + "/exploits/" + account_bucket_for_name(normalized_name) + "/" + normalized_name + ".exploits";
}

std::string serialize_account_to_json(const AccountData& account)
{
    std::ostringstream output;
    output << "{\n";
    output << "  \"version\": " << account.version << ",\n";
    output << "  \"account_name\": \"" << json_utils::escape_json_string(account.account_name) << "\",\n";
    output << "  \"normalized_email\": \"" << json_utils::escape_json_string(account.normalized_email) << "\",\n";
    output << "  \"password_hash\": \"" << json_utils::escape_json_string(account.password_hash) << "\",\n";
    output << "  \"password_salt\": \"" << json_utils::escape_json_string(account.password_salt) << "\",\n";
    output << "  \"characters\": [";
    for (size_t index = 0; index < account.characters.size(); ++index) {
        if (index > 0)
            output << ", ";
        output << "\"" << json_utils::escape_json_string(account.characters[index]) << "\"";
    }
    output << "],\n";
    output << "  \"character_links\": [";
    for (size_t index = 0; index < account.character_links.size(); ++index) {
        const AccountData::CharacterLinkReference& link = account.character_links[index];
        if (index > 0)
            output << ", ";
        output << "{";
        output << "\"character_name\": \"" << json_utils::escape_json_string(link.character_name) << "\", ";
        output << "\"character_path\": \"" << json_utils::escape_json_string(json_path_or_empty(link.character_path)) << "\", ";
        output << "\"object_path\": \"" << json_utils::escape_json_string(json_path_or_empty(link.object_path)) << "\", ";
        output << "\"exploits_path\": \"" << json_utils::escape_json_string(json_path_or_empty(link.exploits_path)) << "\"";
        output << "}";
    }
    output << "],\n";
    output << "  \"email_verified\": " << (account.email_verified ? "true" : "false") << ",\n";
    output << "  \"email_verified_by\": \"" << json_utils::escape_json_string(account.email_verified_by) << "\",\n";
    output << "  \"email_verified_at\": " << account.email_verified_at << ",\n";
    output << "  \"verification_code_hash\": \"" << json_utils::escape_json_string(account.verification_code_hash) << "\",\n";
    output << "  \"verification_code_sent_at\": " << account.verification_code_sent_at << ",\n";
    output << "  \"verification_code_expires_at\": " << account.verification_code_expires_at << ",\n";
    output << "  \"verification_attempt_count\": " << account.verification_attempt_count << ",\n";
    output << "  \"verification_last_attempt_at\": " << account.verification_last_attempt_at << ",\n";
    output << "  \"blocked\": " << (account.blocked ? "true" : "false") << ",\n";
    output << "  \"block_reason\": \"" << json_utils::escape_json_string(account.block_reason) << "\",\n";
    output << "  \"blocked_by\": \"" << json_utils::escape_json_string(account.blocked_by) << "\",\n";
    output << "  \"blocked_at\": " << account.blocked_at << ",\n";
    output << "  \"created_at\": " << account.created_at << ",\n";
    output << "  \"updated_at\": " << account.updated_at << ",\n";
    output << "  \"password_reset_at\": " << account.password_reset_at << ",\n";
    output << "  \"password_reset_by\": \"" << json_utils::escape_json_string(account.password_reset_by) << "\",\n";
    output << "  \"failed_login_count\": " << account.failed_login_count << ",\n";
    output << "  \"failed_login_last_at\": " << account.failed_login_last_at << ",\n";
    output << "  \"failed_login_last_host\": \"" << json_utils::escape_json_string(account.failed_login_last_host) << "\",\n";
    output << "  \"password_reset_code_hash\": \"" << json_utils::escape_json_string(account.password_reset_code_hash) << "\",\n";
    output << "  \"password_reset_code_sent_at\": " << account.password_reset_code_sent_at << ",\n";
    output << "  \"password_reset_code_expires_at\": " << account.password_reset_code_expires_at << ",\n";
    // roster_sort is written ahead of password_reset_attempt_count, which stays the true last
    // (comma-less) field: a strict JSON reader rejects a trailing comma before "}", so any field
    // that a test (or a future one) strips out by deleting its own line must not be the very last
    // entry in the object.
    output << "  \"roster_sort\": \"" << json_utils::escape_json_string(account.roster_sort) << "\",\n";
    output << "  \"password_reset_attempt_count\": " << account.password_reset_attempt_count << "\n";
    output << "}\n";
    return output.str();
}

bool deserialize_account_from_json(const std::string& json, AccountData* account, std::string* error_message)
{
    if (account == nullptr) {
        set_error(error_message, "Account output parameter must not be null.");
        return false;
    }

    AccountData parsed_account;
    json_utils::JsonReader reader(json);
    if (!reader.parse_root_object([&parsed_account](const std::string& key, json_utils::JsonReader* nested_reader, std::string* nested_error_message) {
            return parse_account_property(key, nested_reader, &parsed_account, nested_error_message);
        },
            error_message))
        return false;

    if (parsed_account.version != ACCOUNT_SCHEMA_VERSION) {
        set_error(error_message, "Unsupported account schema version.");
        return false;
    }

    if (!is_valid_account_name(parsed_account.account_name, error_message))
        return false;

    parsed_account.account_name = normalize_account_name(parsed_account.account_name);
    parsed_account.normalized_email = normalize_email(parsed_account.normalized_email);
    for (std::string& character_name : parsed_account.characters) {
        if (!validate_identifier_for_path(character_name, "Character name", error_message))
            return false;
        character_name = normalize_account_name(character_name);
    }
    for (AccountData::CharacterLinkReference& link : parsed_account.character_links) {
        if (!validate_identifier_for_path(link.character_name, "Character name", error_message))
            return false;
        link.character_name = normalize_account_name(link.character_name);
    }
    sync_character_links_from_characters(&parsed_account);

    *account = std::move(parsed_account);
    set_error(error_message, "");
    return true;
}

bool write_account_file(const std::string& root_directory, const AccountData& account, std::string* error_message)
{
    if (!validate_identifier_for_path(account.account_name, "Account name", error_message))
        return false;
    if (!is_valid_email(account.normalized_email, error_message))
        return false;

    const std::string accounts_directory = root_directory + "/accounts";
    const std::string normalized_email = normalize_email(account.normalized_email);
    const std::string bucket_directory = accounts_directory + "/" + account_bucket_for_name(normalized_email);
    const std::string account_directory = account_directory_path_from_email(root_directory, normalized_email);
    const std::string final_path = account_file_path_from_email(root_directory, normalized_email);
    const std::string temp_path = final_path + ".tmp";
    const std::string legacy_flat_path = legacy_account_file_path_from_account_name(root_directory, account.account_name);

    std::string existing_account_path;
    const bool found_existing_account_path = find_account_file_path_by_account_name(root_directory, account.account_name, &existing_account_path, nullptr);

    if (!create_directory_if_missing(accounts_directory, error_message))
        return false;
    if (!create_directory_if_missing(bucket_directory, error_message))
        return false;
    if (!create_directory_if_missing(account_directory, error_message))
        return false;

    AccountData normalized_account = account;
    normalized_account.account_name = normalize_account_name(account.account_name);
    normalized_account.normalized_email = normalized_email;
    sync_character_links_from_characters(&normalized_account);
    for (AccountData::CharacterLinkReference& link : normalized_account.character_links) {
        link.character_name = normalize_account_name(link.character_name);
        link.character_path = character_json_file_name(link.character_name);
        const std::string expected_object_path = objects_json_file_name(link.character_name);
        if (!safe_relative_object_path_or_empty(link.object_path, expected_object_path).empty())
            link.object_path = expected_object_path;
    }

    // Both refusals below happen BEFORE the temp file is opened, deliberately. They used to sit
    // after open_secure_output_file and returned without fclose() and without removing
    // "<final>.tmp", so every one of them leaked a descriptor and left a stray temp file behind.
    // This function runs on every successful login (clear_account_login_failures), so a single
    // account whose record will not parse leaked one descriptor per login attempt and walked a
    // process that runs for weeks towards EMFILE -- at which point the server can neither accept
    // connections nor write player files. Nothing between here and the open touches the
    // filesystem, so moving the checks up changes no on-disk ordering on the success path.
    if (path_exists(final_path)) {
        AccountData existing_account_at_target;
        std::string existing_read_error;
        if (!read_account_file_from_path(final_path, &existing_account_at_target, &existing_read_error)) {
            // The post-boot half of the quarantine story. Boot walks every record and reports what
            // it cannot read; nothing walks the tree again afterwards, so a record that goes bad
            // later was reported NOWHERE -- this function runs on every successful login
            // (clear_account_login_failures), whose caller passes a null error_message and ignores
            // the return, so the failure was completely silent. We already have the file open and
            // already know it will not parse, so marking it here costs no walk and no extra I/O.
            //
            // Marked, not quarantined: quarantine withdraws the record's account-name and character
            // claims with no way back short of a reboot, so a transient EIO would cost a live player
            // every save until the next boot. This changes no lookup's answer -- see
            // note_unreadable_at_runtime. A later successful write re-upserts the entry and clears
            // the mark, so a repaired record heals without a reboot.
            if (account_index_is_authoritative_for(root_directory)
                && account_index::note_unreadable_at_runtime(normalized_email,
                    existing_read_error.empty() ? "Account record could not be read." : existing_read_error)) {
                char log_buffer[MAX_STRING_LENGTH];
                std::snprintf(log_buffer, sizeof(log_buffer),
                    "Account record '%s' could not be read after boot: %s (the account is still indexed; `account index` lists it)",
                    final_path.c_str(),
                    existing_read_error.empty() ? "Account record could not be read." : existing_read_error.c_str());
                log(log_buffer);
                mudlog(log_buffer, BRF, LEVEL_IMMORT, TRUE);
            }
            set_error(error_message, "Existing account file could not be read safely.");
            return false;
        }

        if (normalize_account_name(existing_account_at_target.account_name) != normalized_account.account_name) {
            set_error(error_message, "Account storage path is already occupied by a different account.");
            return false;
        }
    }

    FILE* file = open_secure_output_file(temp_path, error_message);
    if (file == nullptr)
        return false;

    const std::string json = serialize_account_to_json(normalized_account);

    const size_t written_length = std::fwrite(json.data(), sizeof(char), json.size(), file);
    const int close_result = std::fclose(file);
    if (written_length != json.size() || close_result != 0) {
        std::remove(temp_path.c_str());
        set_error(error_message, "Failed to write temporary account file '" + temp_path + "'.");
        return false;
    }

    if (std::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        std::remove(temp_path.c_str());
        set_error(error_message, "Failed to move temporary account file into place: " + std::string(std::strerror(errno)));
        return false;
    }

    if (found_existing_account_path && existing_account_path != final_path) {
        if (std::remove(existing_account_path.c_str()) != 0 && errno != ENOENT) {
            set_error(error_message, "Failed to retire stale account file '" + existing_account_path + "': " + std::strerror(errno));
            return false;
        }
    }

    if (legacy_flat_path != final_path && std::remove(legacy_flat_path.c_str()) != 0 && errno != ENOENT) {
        set_error(error_message, "Failed to retire legacy account file '" + legacy_flat_path + "': " + std::strerror(errno));
        return false;
    }

    // Single account.json write chokepoint: flush the cache so subsequent reads see the new state.
    if (account_cache::is_enabled())
        account_cache::invalidate_all();

    // Same chokepoint, same condition: after the rename, only on a fully successful write. The
    // index re-derives every key from the record just written, so link, unlink, rename and an
    // account-name change are all handled without diffing against what was there before.
    //
    // Root-guarded, the other half of the resolvers' matches_root() guard: final_path was composed
    // against THIS caller's root, and the index holds paths for one tree only. Indexing a foreign
    // root's path would leave lookups against the real (".") tree serving a path into that other
    // tree. Deliberately NOT gated on is_enabled(): the index is maintained on every write whether
    // or not the resolvers currently consult it, which is what lets `account index on` arm
    // immediately instead of having to rebuild (act_wiz.cpp's toggle relies on this).
    if (account_index::matches_root(root_directory))
        account_index::upsert(normalized_account, final_path);

    set_error(error_message, "");
    return true;
}

bool read_account_file_uncached(const std::string& root_directory, const std::string& account_name, AccountData* account, std::string* error_message)
{
    if (account == nullptr) {
        set_error(error_message, "Account output parameter must not be null.");
        return false;
    }

    if (!validate_identifier_for_path(account_name, "Account name", error_message))
        return false;

    std::string account_path;
    if (!find_account_file_path_by_account_name(root_directory, account_name, &account_path, error_message))
        return false;

    return read_account_file_from_path(account_path, account, error_message);
}

bool read_account_file(const std::string& root_directory, const std::string& account_name, AccountData* account, std::string* error_message)
{
    // When the cache is enabled (live server) route through it; otherwise (tests, non-server callers)
    // behave exactly as the uncached read. read_account_file_cached's backing resolver is the uncached
    // read above, so there is no recursion.
    if (account_cache::is_enabled())
        return account_cache::read_account_file_cached(root_directory, account_name, account, error_message);
    return read_account_file_uncached(root_directory, account_name, account, error_message);
}

bool read_account_file_by_email(const std::string& root_directory, const std::string& email, AccountData* account, std::string* error_message)
{
    if (!is_valid_email(email, error_message))
        return false;

    return find_account_by_email_internal(root_directory, email, account, error_message);
}

bool read_account_file_by_identifier(const std::string& root_directory, const std::string& identifier, AccountData* account, std::string* error_message)
{
    if (identifier.find('@') != std::string::npos)
        return read_account_file_by_email(root_directory, identifier, account, error_message);

    return read_account_file(root_directory, identifier, account, error_message);
}

bool for_each_account_record_on_disk(const std::string& root_directory,
    const std::function<void(const AccountRecordOnDisk&)>& visitor,
    std::string* error_message)
{
    const std::string accounts_directory = root_directory + "/accounts";
    DIR* accounts_dir = opendir(accounts_directory.c_str());
    if (accounts_dir == nullptr) {
        set_error(error_message, "Failed to open accounts directory '" + accounts_directory + "': " + std::strerror(errno));
        return false;
    }

    while (dirent* bucket_entry = readdir(accounts_dir)) {
        // Skip "." / ".." and any other hidden entry (e.g. a stray .DS_Store or editor swap file) --
        // classify_bucket_entry applies the same rule to the entries inside a bucket, so both
        // walkers agree that the whole dotfile class is litter.
        if (bucket_entry->d_name[0] == '.')
            continue;

        const std::string bucket_path = accounts_directory + "/" + bucket_entry->d_name;
        struct stat bucket_info { };
        if (stat(bucket_path.c_str(), &bucket_info) != 0 || !S_ISDIR(bucket_info.st_mode))
            continue;

        DIR* bucket_dir = opendir(bucket_path.c_str());
        if (bucket_dir == nullptr) {
            // A bucket that will not open is EVERY account in it leaving the index at once, and
            // account_storage_contains_unreadable_records hard-fails on the same condition. Skipping
            // it silently here left a state where the guard refused every registration on the server
            // while nothing was quarantined, nothing was logged and `account index verify` reported
            // agreement. Visit it as one unreadable record instead: that is what makes it visible in
            // `account index`, counted at boot, and armed as the guard's escape hatch. Keyed by the
            // bucket's own path -- see account_index_key_for_unreadable_bucket.
            AccountRecordOnDisk record;
            record.directory_entry_name = bucket_entry->d_name;
            record.record_path = bucket_path;
            record.directory_layout = false;
            record.parsed = false;
            record.failure_reason = "Failed to open account bucket directory '" + bucket_path + "': " + std::strerror(errno);
            visitor(record);
            continue;
        }

        while (dirent* account_entry = readdir(bucket_dir)) {
            // One shared rule for "what is even a record", used by this walker and by
            // account_storage_contains_unreadable_records. Litter is not visited at all, so it never
            // counts against MAX_QUARANTINED_RECORDS_AT_BOOT; a candidate that cannot be stat'ed is
            // VISITED with parsed == false, which is what quarantines it.
            const BucketEntryClassification classification = classify_bucket_entry(bucket_path, account_entry->d_name);
            if (classification.kind == BucketEntryKind::NotARecord)
                continue;

            AccountRecordOnDisk record;
            record.directory_entry_name = account_entry->d_name;
            record.record_path = classification.record_path;
            record.directory_layout = classification.directory_layout;

            if (classification.kind == BucketEntryKind::Unreadable) {
                record.parsed = false;
                record.failure_reason = classification.failure_reason;
                visitor(record);
                continue;
            }

            AccountData parsed_account;
            std::string read_error;
            if (read_account_file_from_path(classification.record_path, &parsed_account, &read_error)) {
                record.parsed = true;
                record.account = std::move(parsed_account);
            } else {
                record.parsed = false;
                // Never an empty reason. The reader can return false without setting a message, and
                // an empty reason becomes a log line and a wizard "QUARANTINED <path>: " line that
                // trail off into nothing -- the one piece of evidence about a record we refused,
                // with the evidence missing.
                record.failure_reason = read_error.empty() ? "Account record could not be read." : read_error;
            }

            visitor(record);
        }

        closedir(bucket_dir);
    }

    closedir(accounts_dir);
    set_error(error_message, "");
    return true;
}

std::string account_index_quarantine_key(const AccountRecordOnDisk& record)
{
    if (record.directory_layout)
        return record.directory_entry_name;
    if (record.parsed) {
        // A parsed legacy flat record is keyed by its own email UNLESS that email is empty (the
        // "no usable email address" case, db.cpp), in which case it has revealed nothing to key it
        // by and falls through to record_path below, exactly like the unparsed case.
        const std::string email = normalize_email(record.account.normalized_email);
        if (!email.empty())
            return email;
    }
    return record.record_path;
}

std::string account_character_directory(const std::string& root_directory, const std::string& account_name, const std::string&)
{
    const std::string account_storage_key = resolve_account_storage_key(root_directory, account_name);
    if (account_storage_key.empty())
        return root_directory + "/accounts/" + std::string(kInvalidAccountDirectoryName);
    return root_directory + "/accounts/" + account_bucket_for_name(account_storage_key) + "/" + account_storage_key;
}

bool is_invalid_account_storage_directory(const std::string& directory_path)
{
    const size_t separator = directory_path.find_last_of('/');
    const std::string leaf = (separator == std::string::npos) ? directory_path : directory_path.substr(separator + 1);
    return leaf == kInvalidAccountDirectoryName;
}

std::string account_character_snapshot_path(const std::string& root_directory, const std::string& account_name, const std::string& character_name)
{
    return account_character_directory(root_directory, account_name, character_name) + "/" + character_asset_slug(character_name) + ".migration.json";
}

std::string account_character_player_path(const std::string& root_directory, const std::string& account_name, const std::string& character_name)
{
    return account_character_directory(root_directory, account_name, character_name) + "/" + character_json_file_name(character_name);
}

std::string account_character_object_path(const std::string& root_directory, const std::string& account_name, const std::string& character_name)
{
    return account_character_directory(root_directory, account_name, character_name) + "/" + objects_json_file_name(character_name);
}

std::string account_character_exploits_path(const std::string& root_directory, const std::string& account_name, const std::string& character_name)
{
    return account_character_directory(root_directory, account_name, character_name) + "/" + exploits_json_file_name(character_name);
}

std::string serialize_character_migration_to_json(const CharacterMigrationData& migration)
{
    auto write_snapshot = [](std::ostringstream& output, const char* name, const LegacyAssetSnapshot& snapshot) {
        output << "  \"" << name << "\": {\n";
        output << "    \"source_path\": \"" << json_utils::escape_json_string(snapshot.source_path) << "\",\n";
        output << "    \"encoding\": \"" << json_utils::escape_json_string(snapshot.encoding) << "\",\n";
        output << "    \"content\": \"" << json_utils::escape_json_string(snapshot.content) << "\",\n";
        output << "    \"present\": " << (snapshot.present ? "true" : "false") << "\n";
        output << "  }";
    };

    std::ostringstream output;
    output << "{\n";
    output << "  \"version\": " << migration.version << ",\n";
    output << "  \"account_name\": \"" << json_utils::escape_json_string(migration.account_name) << "\",\n";
    output << "  \"character_name\": \"" << json_utils::escape_json_string(migration.character_name) << "\",\n";
    output << "  \"migrated_at\": " << migration.migrated_at << ",\n";
    // The on-disk migration artifact is transitional only. Do not persist raw
    // legacy player bytes that still carry legacy password/host state.
    write_snapshot(output, "object_file", migration.object_file);
    output << ",\n";
    write_snapshot(output, "exploits_file", migration.exploits_file);
    output << "\n}\n";
    return output.str();
}

bool deserialize_character_migration_from_json(const std::string& json, CharacterMigrationData* migration, std::string* error_message)
{
    if (migration == nullptr) {
        set_error(error_message, "Migration output parameter must not be null.");
        return false;
    }

    CharacterMigrationData parsed_migration;
    json_utils::JsonReader reader(json);
    if (!reader.parse_root_object([&parsed_migration](const std::string& key, json_utils::JsonReader* nested_reader, std::string* nested_error_message) {
            return parse_migration_property(key, nested_reader, &parsed_migration, nested_error_message);
        },
            error_message))
        return false;

    if (parsed_migration.version != ACCOUNT_SCHEMA_VERSION) {
        set_error(error_message, "Unsupported character migration schema version.");
        return false;
    }

    if (!is_valid_account_name(parsed_migration.account_name, error_message))
        return false;

    parsed_migration.account_name = normalize_account_name(parsed_migration.account_name);
    parsed_migration.character_name = normalize_account_name(parsed_migration.character_name);

    std::string decoded_content;
    if (parsed_migration.player_file.present && parsed_migration.player_file.encoding == "hex" && !hex_decode(parsed_migration.player_file.content, &decoded_content, error_message))
        return false;
    if (parsed_migration.object_file.present && parsed_migration.object_file.encoding == "hex" && !hex_decode(parsed_migration.object_file.content, &decoded_content, error_message))
        return false;
    if (parsed_migration.exploits_file.present && parsed_migration.exploits_file.encoding == "hex" && !hex_decode(parsed_migration.exploits_file.content, &decoded_content, error_message))
        return false;

    *migration = std::move(parsed_migration);
    set_error(error_message, "");
    return true;
}

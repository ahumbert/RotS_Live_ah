#include "account_index.h"
#include "account_management.h"
#include "account_management_types.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

account::AccountData make_account(const std::string& email, const std::string& name,
    const std::vector<std::string>& characters)
{
    account::AccountData account;
    account.normalized_email = email;
    account.account_name = name;
    account.characters = characters;
    return account;
}

// The resolvers under test read real files, so these need a real accounts/ tree. Kept local rather
// than shared with account_management_tests.cpp, which has its own copy in its own anonymous
// namespace.
class IndexTemporaryDirectory {
public:
    IndexTemporaryDirectory()
    {
        char directory_template[] = "/tmp/rots-account-index-XXXXXX";
        char* created_path = mkdtemp(directory_template);
        EXPECT_NE(created_path, nullptr);
        if (created_path)
            m_path = created_path;
    }

    ~IndexTemporaryDirectory()
    {
        if (!m_path.empty())
            remove_tree(m_path);
    }

    const std::string& path() const { return m_path; }

private:
    static void remove_tree(const std::string& path)
    {
        DIR* directory = opendir(path.c_str());
        if (directory == nullptr) {
            std::remove(path.c_str());
            return;
        }

        while (dirent* entry = readdir(directory)) {
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
                continue;

            const std::string child_path = path + "/" + entry->d_name;
            struct stat file_info { };
            if (stat(child_path.c_str(), &file_info) != 0)
                continue;

            if (S_ISDIR(file_info.st_mode))
                remove_tree(child_path);
            else
                std::remove(child_path.c_str());
        }

        closedir(directory);
        rmdir(path.c_str());
    }

    std::string m_path;
};

// Restores the flag no matter how the test exits, so one failing EXPECT cannot leave the index on
// for every test that runs after it.
class ScopedIndexEnabled {
public:
    explicit ScopedIndexEnabled(bool enabled) { account_index::set_enabled(enabled); }
    ~ScopedIndexEnabled() { account_index::set_enabled(false); }
};

class AccountIndexTest : public ::testing::Test {
protected:
    void SetUp() override { account_index::clear(); }
    void TearDown() override { account_index::clear(); }
};

TEST_F(AccountIndexTest, UpsertMakesRecordFindableByAllThreeKeys)
{
    const account::AccountData account = make_account("player@example.com", "player", { "Frodo", "Sam" });
    account_index::upsert(account, "accounts/P-T/player@example.com/account.json");

    std::string path;
    ASSERT_TRUE(account_index::find_path_by_email("player@example.com", &path, nullptr));
    EXPECT_EQ(path, "accounts/P-T/player@example.com/account.json");

    path.clear();
    ASSERT_TRUE(account_index::find_path_by_account_name("player", &path, nullptr));
    EXPECT_EQ(path, "accounts/P-T/player@example.com/account.json");

    std::string owner_email;
    ASSERT_TRUE(account_index::find_owner_email_by_character("Frodo", &owner_email, nullptr));
    EXPECT_EQ(owner_email, "player@example.com");
    ASSERT_TRUE(account_index::find_owner_email_by_character("Sam", &owner_email, nullptr));
    EXPECT_EQ(owner_email, "player@example.com");
}

TEST_F(AccountIndexTest, LookupsAreCaseInsensitiveViaNormalization)
{
    const account::AccountData account = make_account("player@example.com", "player", { "Frodo" });
    account_index::upsert(account, "accounts/P-T/player@example.com/account.json");

    std::string path;
    EXPECT_TRUE(account_index::find_path_by_email("PLAYER@Example.COM", &path, nullptr));
    EXPECT_TRUE(account_index::find_path_by_account_name("PlAyEr", &path, nullptr));

    std::string owner_email;
    EXPECT_TRUE(account_index::find_owner_email_by_character("FRODO", &owner_email, nullptr));
}

TEST_F(AccountIndexTest, UnknownKeysReturnFalse)
{
    std::string path;
    EXPECT_FALSE(account_index::find_path_by_email("nobody@example.com", &path, nullptr));
    EXPECT_FALSE(account_index::find_path_by_account_name("nobody", &path, nullptr));

    std::string owner_email;
    EXPECT_FALSE(account_index::find_owner_email_by_character("Nobody", &owner_email, nullptr));
}

TEST_F(AccountIndexTest, UpsertDropsKeysTheRecordNoLongerOwns)
{
    account_index::upsert(make_account("player@example.com", "player", { "Frodo", "Sam" }),
        "accounts/P-T/player@example.com/account.json");
    account_index::upsert(make_account("player@example.com", "player", { "Frodo" }),
        "accounts/P-T/player@example.com/account.json");

    std::string owner_email;
    EXPECT_TRUE(account_index::find_owner_email_by_character("Frodo", &owner_email, nullptr));
    EXPECT_FALSE(account_index::find_owner_email_by_character("Sam", &owner_email, nullptr))
        << "an unlinked character must stop resolving, or a save writes to the wrong account";
}

TEST_F(AccountIndexTest, UpsertFollowsAnAccountNameChange)
{
    account_index::upsert(make_account("player@example.com", "oldname", { "Frodo" }),
        "accounts/P-T/player@example.com/account.json");
    account_index::upsert(make_account("player@example.com", "newname", { "Frodo" }),
        "accounts/P-T/player@example.com/account.json");

    std::string path;
    EXPECT_TRUE(account_index::find_path_by_account_name("newname", &path, nullptr));
    EXPECT_FALSE(account_index::find_path_by_account_name("oldname", &path, nullptr));
}

TEST_F(AccountIndexTest, EnabledFlagDefaultsOffAndToggles)
{
    EXPECT_FALSE(account_index::is_enabled());
    account_index::set_enabled(true);
    EXPECT_TRUE(account_index::is_enabled());
    account_index::set_enabled(false);
    EXPECT_FALSE(account_index::is_enabled());
}

TEST_F(AccountIndexTest, SizeCountsRecordsNotKeys)
{
    account_index::upsert(make_account("a@example.com", "aaa", { "One", "Two", "Three" }),
        "accounts/A-E/a@example.com/account.json");
    account_index::upsert(make_account("b@example.com", "bbb", {}),
        "accounts/A-E/b@example.com/account.json");
    EXPECT_EQ(account_index::size(), 2u);
}

TEST_F(AccountIndexTest, FindEmailByAccountNameReturnsTheEmailForAnIndexedAccount)
{
    account_index::upsert(make_account("player@example.com", "player", { "Frodo" }),
        "accounts/P-T/player@example.com/account.json");

    std::string email;
    ASSERT_TRUE(account_index::find_email_by_account_name("player", &email, nullptr));
    EXPECT_EQ(email, "player@example.com");
}

TEST_F(AccountIndexTest, FindEmailByAccountNameReturnsFalseForAnUnknownName)
{
    std::string email;
    EXPECT_FALSE(account_index::find_email_by_account_name("nobody", &email, nullptr));
}

TEST_F(AccountIndexTest, QuarantinedEmailStaysOccupiedSoItCannotBeOverwritten)
{
    account_index::quarantine("broken@example.com", "accounts/A-E/broken@example.com/account.json",
        "unparseable JSON");

    EXPECT_TRUE(account_index::is_quarantined("broken@example.com"));

    std::string path;
    std::string error_message;
    EXPECT_FALSE(account_index::find_path_by_email("broken@example.com", &path, &error_message));
    EXPECT_EQ(error_message, "That account record could not be read.")
        << "must NOT read as 'no account exists', or creation would write over a real record";
}

TEST_F(AccountIndexTest, QuarantineDropsTheKeysTheRecordHeldBefore)
{
    account_index::upsert(make_account("player@example.com", "player", { "Frodo" }),
        "accounts/P-T/player@example.com/account.json");
    account_index::quarantine("player@example.com", "accounts/P-T/player@example.com/account.json",
        "mismatched email");

    std::string owner_email;
    EXPECT_FALSE(account_index::find_owner_email_by_character("Frodo", &owner_email, nullptr));

    std::string path;
    EXPECT_FALSE(account_index::find_path_by_account_name("player", &path, nullptr));
}

TEST_F(AccountIndexTest, QuarantinedCountTracksOnlyBadRecords)
{
    account_index::upsert(make_account("good@example.com", "good", {}),
        "accounts/F-J/good@example.com/account.json");
    account_index::quarantine("bad1@example.com", "accounts/A-E/bad1@example.com/account.json", "x");
    account_index::quarantine("bad2@example.com", "accounts/A-E/bad2@example.com/account.json", "y");

    EXPECT_EQ(account_index::quarantined_count(), 2u);
    EXPECT_EQ(account_index::size(), 3u);
}

TEST_F(AccountIndexTest, QuarantineThresholdIsFive)
{
    EXPECT_EQ(account_index::MAX_QUARANTINED_RECORDS_AT_BOOT, 5u);
}

TEST_F(AccountIndexTest, QuarantinedEntriesReportPathAndReason)
{
    account_index::quarantine("bad@example.com", "accounts/A-E/bad@example.com/account.json",
        "unparseable JSON");

    const std::vector<account_index::Entry> entries = account_index::quarantined_entries();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].normalized_email, "bad@example.com");
    EXPECT_EQ(entries[0].record_path, "accounts/A-E/bad@example.com/account.json");
    EXPECT_EQ(entries[0].quarantine_reason, "unparseable JSON");
}

TEST_F(AccountIndexTest, BootToleratesFailuresUpToTheThreshold)
{
    for (std::size_t i = 0; i < account_index::MAX_QUARANTINED_RECORDS_AT_BOOT; ++i) {
        const std::string email = "bad" + std::to_string(i) + "@example.com";
        account_index::quarantine(email, "accounts/A-E/" + email + "/account.json", "unparseable");
    }

    EXPECT_EQ(account_index::quarantined_count(), account_index::MAX_QUARANTINED_RECORDS_AT_BOOT);
    EXPECT_FALSE(account_index::quarantined_count() > account_index::MAX_QUARANTINED_RECORDS_AT_BOOT)
        << "exactly at the threshold must still boot";
}

TEST_F(AccountIndexTest, BootRefusesPastTheThreshold)
{
    for (std::size_t i = 0; i <= account_index::MAX_QUARANTINED_RECORDS_AT_BOOT; ++i) {
        const std::string email = "bad" + std::to_string(i) + "@example.com";
        account_index::quarantine(email, "accounts/A-E/" + email + "/account.json", "unparseable");
    }

    EXPECT_TRUE(account_index::quarantined_count() > account_index::MAX_QUARANTINED_RECORDS_AT_BOOT);
}

TEST_F(AccountIndexTest, FlatRecordDoesNotDisplaceADirectoryRecordForTheSameEmail)
{
    const account::AccountData directory_account = make_account("player@example.com", "player", { "Frodo" });
    account_index::upsert(directory_account, "accounts/P-T/player@example.com/account.json", /*legacy_flat_layout=*/false);

    const account::AccountData flat_account = make_account("player@example.com", "player", { "Frodo" });
    account_index::upsert(flat_account, "accounts/P-T/player.json", /*legacy_flat_layout=*/true);

    std::string path;
    ASSERT_TRUE(account_index::find_path_by_email("player@example.com", &path, nullptr));
    EXPECT_EQ(path, "accounts/P-T/player@example.com/account.json")
        << "the directory record must remain authoritative; the flat record must not win";
}

TEST_F(AccountIndexTest, DirectoryRecordDisplacesAFlatRecordRegardlessOfArrivalOrder)
{
    const account::AccountData flat_account = make_account("player@example.com", "player", { "Frodo" });
    account_index::upsert(flat_account, "accounts/P-T/player.json", /*legacy_flat_layout=*/true);

    const account::AccountData directory_account = make_account("player@example.com", "player", { "Frodo" });
    account_index::upsert(directory_account, "accounts/P-T/player@example.com/account.json", /*legacy_flat_layout=*/false);

    std::string path;
    ASSERT_TRUE(account_index::find_path_by_email("player@example.com", &path, nullptr));
    EXPECT_EQ(path, "accounts/P-T/player@example.com/account.json")
        << "a directory record arriving after a flat one must still overwrite it";
}

TEST_F(AccountIndexTest, DisabledIndexIsNotConsulted)
{
    account_index::set_enabled(false);
    account_index::upsert(make_account("player@example.com", "player", { "Frodo" }),
        "accounts/P-T/player@example.com/account.json");

    // The index still answers its own API when queried directly; is_enabled() only governs whether
    // the account resolvers consult it. This test pins that distinction so the flag is not
    // mistakenly wired into the lookups themselves.
    std::string path;
    EXPECT_TRUE(account_index::find_path_by_email("player@example.com", &path, nullptr));
    EXPECT_FALSE(account_index::is_enabled());
}

TEST_F(AccountIndexTest, UnknownEmailErrorMatchesTheScanText)
{
    std::string path;
    std::string error_message;
    EXPECT_FALSE(account_index::find_path_by_email("nobody@example.com", &path, &error_message));
    EXPECT_EQ(error_message, "No account exists for that email address.")
        << "must match find_account_by_email_internal's text verbatim; tests assert on it";
}

TEST_F(AccountIndexTest, UnknownAccountNameErrorMatchesTheScanText)
{
    std::string path;
    std::string error_message;
    EXPECT_FALSE(account_index::find_path_by_account_name("nobody", &path, &error_message));
    EXPECT_EQ(error_message,
        std::string("Failed to open account file for account 'nobody': ") + std::strerror(ENOENT))
        << "must match find_account_file_path_by_account_name's not-found text verbatim";
}

TEST_F(AccountIndexTest, UnknownCharacterIsReportedWithoutAnErrorMessage)
{
    // find_linked_character_owner_account_uncached's index fast path tells "not linked" (a success
    // for that resolver, true with an empty owner) apart from a real failure by whether a message
    // was set. An unknown character MUST leave it empty or every save of an unlinked character
    // starts reporting an error.
    std::string owner_email;
    std::string error_message = "sentinel";
    EXPECT_FALSE(account_index::find_owner_email_by_character("Nobody", &owner_email, &error_message));
    EXPECT_EQ(error_message, "");
}

// Step 9 of the task brief: with a real accounts/ tree on disk, every resolver must give the same
// answer with the index on as it does with the index off (the directory scan). This is the test
// that would catch a fast path that resolves to the wrong record, not merely a slow one.
TEST_F(AccountIndexTest, ResolversAgreeWithTheScanWhenTheIndexIsOn)
{
    IndexTemporaryDirectory root_directory;
    ASSERT_FALSE(root_directory.path().empty());
    const std::string root = root_directory.path();

    std::string error_message;
    account::AccountData first_account;
    ASSERT_TRUE(account::create_account_for_email(root, "agree@example.com", "ValidPass1", 1700000000,
        &first_account, &error_message))
        << error_message;
    ASSERT_TRUE(account::add_character_to_account(&first_account, "Frodo", &error_message))
        << error_message;
    ASSERT_TRUE(account::write_account_file(root, first_account, &error_message)) << error_message;

    account::AccountData second_account;
    ASSERT_TRUE(account::create_account_for_email(root, "other@example.com", "ValidPass1", 1700000000,
        &second_account, &error_message))
        << error_message;
    ASSERT_TRUE(account::add_character_to_account(&second_account, "Samwise", &error_message))
        << error_message;
    ASSERT_TRUE(account::write_account_file(root, second_account, &error_message)) << error_message;

    struct Observation {
        bool by_email_ok = false;
        std::string by_email_name;
        bool by_email_error = false;
        bool by_name_ok = false;
        std::string by_name_email;
        bool owner_ok = false;
        std::string owner_name;
        std::string owner_error;
        bool missing_owner_ok = false;
        std::string missing_owner_name;
        std::string missing_owner_error;
        bool unknown_email_ok = false;
        std::string unknown_email_error;
        bool unknown_name_ok = false;
        std::string unknown_name_error;
        std::string character_directory;
    };

    const std::string account_name = first_account.account_name;

    const auto observe = [&root, &account_name]() {
        Observation observation;
        std::string message;

        account::AccountData read_by_email;
        observation.by_email_ok = account::read_account_file_by_email(root, "agree@example.com",
            &read_by_email, &message);
        observation.by_email_name = read_by_email.account_name;
        observation.by_email_error = !message.empty();

        account::AccountData read_by_name;
        observation.by_name_ok = account::read_account_file(root, account_name, &read_by_name,
            &message);
        observation.by_name_email = read_by_name.normalized_email;

        observation.owner_ok = account::find_linked_character_owner_account_uncached(root, "Frodo",
            &observation.owner_name, &observation.owner_error);

        observation.missing_owner_ok = account::find_linked_character_owner_account_uncached(root,
            "Meriadoc", &observation.missing_owner_name, &observation.missing_owner_error);

        account::AccountData unknown;
        observation.unknown_email_ok = account::read_account_file_by_email(root, "nobody@example.com",
            &unknown, &observation.unknown_email_error);
        observation.unknown_name_ok = account::read_account_file(root, "nobodyatall", &unknown,
            &observation.unknown_name_error);

        observation.character_directory = account::account_character_directory(root,
            account_name, "Frodo");
        return observation;
    };

    Observation scanned;
    {
        ScopedIndexEnabled disabled(false);
        scanned = observe();
    }

    Observation indexed;
    {
        // Without this the root guard added for the fast paths would make every call fall through
        // to the scan and the comparison below would be scan-against-scan, i.e. vacuous.
        account_index::set_root_directory(root);
        ScopedIndexEnabled enabled(true);
        indexed = observe();
        account_index::set_root_directory(".");
    }

    EXPECT_EQ(scanned.by_email_ok, indexed.by_email_ok);
    EXPECT_EQ(scanned.by_email_name, indexed.by_email_name);
    EXPECT_EQ(scanned.by_name_ok, indexed.by_name_ok);
    EXPECT_EQ(scanned.by_name_email, indexed.by_name_email);

    EXPECT_EQ(scanned.owner_ok, indexed.owner_ok);
    EXPECT_EQ(scanned.owner_name, indexed.owner_name);
    EXPECT_EQ(scanned.owner_error, indexed.owner_error);

    // The load-bearing one: an unlinked character resolves successfully with an empty owner and an
    // empty error. If the index path returned false here, every save of an unlinked character would
    // start looking like a failure.
    EXPECT_TRUE(scanned.missing_owner_ok);
    EXPECT_EQ(scanned.missing_owner_ok, indexed.missing_owner_ok);
    EXPECT_EQ(scanned.missing_owner_name, indexed.missing_owner_name);
    EXPECT_EQ(scanned.missing_owner_error, indexed.missing_owner_error);
    EXPECT_EQ(indexed.missing_owner_name, "");
    EXPECT_EQ(indexed.missing_owner_error, "");

    EXPECT_EQ(scanned.unknown_email_ok, indexed.unknown_email_ok);
    EXPECT_EQ(scanned.unknown_email_error, indexed.unknown_email_error);
    EXPECT_EQ(scanned.unknown_name_ok, indexed.unknown_name_ok);
    EXPECT_EQ(scanned.unknown_name_error, indexed.unknown_name_error);

    EXPECT_EQ(scanned.character_directory, indexed.character_directory);
    EXPECT_NE(indexed.character_directory, "");
}

// --- Duplicate account names ------------------------------------------------------------------
// Two records claiming one account name are not merely ambiguous, they are dangerous:
// write_account_file std::remove()s the path find_account_file_path_by_account_name returns when it
// differs from the record being written, so resolving to one of the pair deletes the other's file.
// The directory scan refused to answer; so must the index.

TEST_F(AccountIndexTest, TwoEmailsClaimingOneAccountNameMakeTheNameLookupFail)
{
    account_index::upsert(make_account("first@example.com", "shared", { "Frodo" }),
        "accounts/A-E/first@example.com/account.json");
    account_index::upsert(make_account("second@example.com", "shared", { "Sam" }),
        "accounts/P-T/second@example.com/account.json");

    EXPECT_TRUE(account_index::is_account_name_ambiguous("shared"));

    std::string path;
    std::string error_message;
    EXPECT_FALSE(account_index::find_path_by_account_name("shared", &path, &error_message));
    EXPECT_EQ(error_message, "Multiple account records exist for account 'shared'.")
        << "must match find_account_file_path_by_account_name's duplicate text verbatim";

    std::string email;
    error_message.clear();
    EXPECT_FALSE(account_index::find_email_by_account_name("shared", &email, &error_message));
    EXPECT_EQ(error_message, "Multiple account records exist for account 'shared'.");
}

TEST_F(AccountIndexTest, AnAmbiguousNameLeaksNoPathToEitherRecord)
{
    account_index::upsert(make_account("first@example.com", "shared", { "Frodo" }),
        "accounts/A-E/first@example.com/account.json");
    account_index::upsert(make_account("second@example.com", "shared", { "Sam" }),
        "accounts/P-T/second@example.com/account.json");

    std::string path = "untouched";
    EXPECT_FALSE(account_index::find_path_by_account_name("shared", &path, nullptr));
    EXPECT_EQ(path, "untouched") << "a path here would let write_account_file delete the other record";

    std::string email = "untouched";
    EXPECT_FALSE(account_index::find_email_by_account_name("shared", &email, nullptr));
    EXPECT_EQ(email, "untouched");

    // The per-email keys are untouched: only the shared NAME is unresolvable.
    std::string by_email;
    EXPECT_TRUE(account_index::find_path_by_email("first@example.com", &by_email, nullptr));
    EXPECT_EQ(by_email, "accounts/A-E/first@example.com/account.json");
    EXPECT_TRUE(account_index::find_path_by_email("second@example.com", &by_email, nullptr));
    EXPECT_EQ(by_email, "accounts/P-T/second@example.com/account.json");
}

TEST_F(AccountIndexTest, OneEmailInBothLayoutsIsNotAmbiguous)
{
    // A flat record and a directory record for the SAME email are one account stored two ways --
    // exactly the case the Task 3 precedence rule exists for -- not two records claiming one name.
    account_index::upsert(make_account("player@example.com", "player", { "Frodo" }),
        "accounts/P-T/player.json", /*legacy_flat_layout=*/true);
    account_index::upsert(make_account("player@example.com", "player", { "Frodo" }),
        "accounts/P-T/player@example.com/account.json", /*legacy_flat_layout=*/false);

    EXPECT_FALSE(account_index::is_account_name_ambiguous("player"));

    std::string path;
    ASSERT_TRUE(account_index::find_path_by_account_name("player", &path, nullptr));
    EXPECT_EQ(path, "accounts/P-T/player@example.com/account.json");
}

TEST_F(AccountIndexTest, ReUpsertingTheSameRecordDoesNotMakeItsOwnNameAmbiguous)
{
    // write_account_file upserts on every account write, so the common case is the same email
    // re-binding its own name over and over. That must never look like a duplicate.
    for (int pass = 0; pass < 3; ++pass) {
        account_index::upsert(make_account("player@example.com", "player", { "Frodo" }),
            "accounts/P-T/player@example.com/account.json");
    }

    EXPECT_FALSE(account_index::is_account_name_ambiguous("player"));
    std::string path;
    EXPECT_TRUE(account_index::find_path_by_account_name("player", &path, nullptr));
}

TEST_F(AccountIndexTest, AmbiguityIsCaseInsensitiveLikeEveryOtherKey)
{
    account_index::upsert(make_account("first@example.com", "Shared", { "Frodo" }),
        "accounts/A-E/first@example.com/account.json");
    account_index::upsert(make_account("second@example.com", "SHARED", { "Sam" }),
        "accounts/P-T/second@example.com/account.json");

    std::string path;
    std::string error_message;
    EXPECT_FALSE(account_index::find_path_by_account_name("sHaReD", &path, &error_message));
    EXPECT_EQ(error_message, "Multiple account records exist for account 'shared'.");
}

// --- Root guard ---------------------------------------------------------------------------------

TEST_F(AccountIndexTest, RootDirectoryDefaultsToDotAndClearResetsIt)
{
    EXPECT_EQ(account_index::root_directory(), ".");
    EXPECT_TRUE(account_index::matches_root("."));
    EXPECT_FALSE(account_index::matches_root("/tmp/elsewhere"));

    account_index::set_root_directory("/tmp/elsewhere");
    EXPECT_TRUE(account_index::matches_root("/tmp/elsewhere"));
    EXPECT_FALSE(account_index::matches_root("."));

    account_index::clear();
    EXPECT_EQ(account_index::root_directory(), ".");
}

TEST_F(AccountIndexTest, ResolversFallBackToTheScanForAMismatchedRoot)
{
    // The fast paths ignore their own root_directory argument, so answering from an index built for
    // a different tree would hand back paths from that tree. They must fall through to the scan --
    // which still gets the right answer, just slowly.
    IndexTemporaryDirectory root_directory;
    ASSERT_FALSE(root_directory.path().empty());
    const std::string root = root_directory.path();

    std::string error_message;
    account::AccountData created;
    ASSERT_TRUE(account::create_account_for_email(root, "rooted@example.com", "ValidPass1",
        1700000000, &created, &error_message))
        << error_message;
    ASSERT_TRUE(account::add_character_to_account(&created, "Frodo", &error_message)) << error_message;
    ASSERT_TRUE(account::write_account_file(root, created, &error_message)) << error_message;

    // Index enabled, but still claiming the default "." root while the caller uses the temp tree.
    ScopedIndexEnabled enabled(true);
    ASSERT_EQ(account_index::root_directory(), ".");

    account::AccountData read_back;
    EXPECT_TRUE(account::read_account_file_by_email(root, "rooted@example.com", &read_back,
        &error_message))
        << error_message;
    EXPECT_EQ(read_back.account_name, created.account_name);

    std::string owner;
    EXPECT_TRUE(account::find_linked_character_owner_account_uncached(root, "Frodo", &owner,
        &error_message))
        << error_message;
    EXPECT_EQ(owner, created.account_name);
}

TEST_F(AccountIndexTest, QuarantinedAddressIsNotFreeForCreation)
{
    // create_account_for_email decides an address is free when its lookup fails, and a quarantined
    // record's lookup fails the same way an unused address's does. Without a guard, this would let a
    // brand-new account overwrite a real player's unparseable-but-real record.
    IndexTemporaryDirectory root_directory;
    ASSERT_FALSE(root_directory.path().empty());
    const std::string root = root_directory.path();

    account::AccountData created;
    std::string error_message;
    {
        // Root must match the caller's root_directory, exactly like the resolver fast paths --
        // otherwise the guard would be silently skipped and this test would pass for the wrong
        // reason.
        account_index::set_root_directory(root);
        ScopedIndexEnabled enabled(true);
        account_index::quarantine("occupied@example.com",
            "accounts/K-O/occupied@example.com/account.json", "unparseable JSON");

        EXPECT_TRUE(account_index::is_quarantined("occupied@example.com"));

        ASSERT_FALSE(account::create_account_for_email(root, "occupied@example.com", "ValidPass1",
            1000, &created, &error_message))
            << "creating here would overwrite a real player's record";
        account_index::set_root_directory(".");
    }

    EXPECT_FALSE(error_message.empty());
    EXPECT_EQ(error_message.find("already exists"), std::string::npos)
        << "must not disclose that a record exists at this address: " << error_message;
}

TEST_F(AccountIndexTest, RebuildReportNamesEveryDisagreement)
{
    account_index::upsert(make_account("real@example.com", "real", { "Frodo" }),
        "accounts/P-T/real@example.com/account.json");

    // A record the live index knows about that a fresh scan would not produce.
    account_index::upsert(make_account("ghost@example.com", "ghost", {}),
        "accounts/F-J/ghost@example.com/account.json");

    std::vector<account_index::Entry> on_disk;
    account_index::Entry real_entry;
    real_entry.normalized_email = "real@example.com";
    real_entry.record_path = "accounts/P-T/real@example.com/account.json";
    real_entry.normalized_account_name = "real";
    on_disk.push_back(real_entry);

    const std::vector<std::string> disagreements = account_index::rebuild_report(on_disk);
    ASSERT_EQ(disagreements.size(), 1u);
    EXPECT_NE(disagreements[0].find("ghost@example.com"), std::string::npos);
}

TEST_F(AccountIndexTest, RebuildReportIsEmptyWhenTheIndexAgrees)
{
    account_index::upsert(make_account("real@example.com", "real", { "Frodo" }),
        "accounts/P-T/real@example.com/account.json");

    std::vector<account_index::Entry> on_disk;
    account_index::Entry real_entry;
    real_entry.normalized_email = "real@example.com";
    real_entry.record_path = "accounts/P-T/real@example.com/account.json";
    real_entry.normalized_account_name = "real";
    on_disk.push_back(real_entry);

    EXPECT_TRUE(account_index::rebuild_report(on_disk).empty());
}

TEST_F(AccountIndexTest, RebuildReportTreatsAQuarantinedCorruptJsonRecordStillOnDiskAsAgreement)
{
    // Shape 1: directory layout, unparsed (corrupt account.json). Quarantined under its directory
    // entry name, which IS the email even though the file never parsed.
    account_index::quarantine("corrupt@example.com",
        "accounts/A-E/corrupt@example.com/account.json", "unparseable JSON");

    std::vector<account_index::Entry> on_disk;
    account_index::Entry corrupt_entry;
    corrupt_entry.normalized_email = "corrupt@example.com";
    corrupt_entry.record_path = "accounts/A-E/corrupt@example.com/account.json";
    // normalized_account_name deliberately left empty: an unparsed record on disk discloses no
    // account name, exactly like the live enumerator's view of it.
    on_disk.push_back(corrupt_entry);

    EXPECT_TRUE(account_index::rebuild_report(on_disk).empty());
}

TEST_F(AccountIndexTest, RebuildReportTreatsAQuarantinedMismatchedEmailRecordStillOnDiskAsAgreement)
{
    // Shape 2: directory layout, PARSED, but the email inside the file disagrees with the
    // directory name -- quarantined under the directory name, not the (wrong) email in the file.
    // The file parsed fine, so it discloses a real, non-empty account name -- but quarantine()
    // never records one. Without the "quarantined is agreement" guard in rebuild_report, this
    // on-disk entry's non-empty account name would collide with the index's empty one and produce
    // a false "account name differs" disagreement. That makes this test actually exercise the
    // guard rather than pass by an accidental empty-equals-empty coincidence.
    account_index::quarantine("actual@example.com",
        "accounts/A-E/actual@example.com/account.json", "mismatched normalized email");

    std::vector<account_index::Entry> on_disk;
    account_index::Entry mismatched_entry;
    mismatched_entry.normalized_email = "actual@example.com";
    mismatched_entry.record_path = "accounts/A-E/actual@example.com/account.json";
    mismatched_entry.normalized_account_name = "wrongemailguy";
    on_disk.push_back(mismatched_entry);

    EXPECT_TRUE(account_index::rebuild_report(on_disk).empty());
}

TEST_F(AccountIndexTest, RebuildReportTreatsAQuarantinedFlatNoEmailRecordStillOnDiskAsAgreement)
{
    // Shape 3: legacy flat layout, PARSED, but the email inside is empty -- quarantined under its
    // own record path since it disclosed no email to key it by. Again give it a real, non-empty
    // account name so the test would fail without the guard (same reasoning as shape 2).
    account_index::quarantine("accounts/K-O/legacyname.json",
        "accounts/K-O/legacyname.json", "no usable email address");

    std::vector<account_index::Entry> on_disk;
    account_index::Entry flat_entry;
    flat_entry.normalized_email = "accounts/K-O/legacyname.json";
    flat_entry.record_path = "accounts/K-O/legacyname.json";
    flat_entry.normalized_account_name = "legacyname";
    on_disk.push_back(flat_entry);

    EXPECT_TRUE(account_index::rebuild_report(on_disk).empty());
}

TEST_F(AccountIndexTest, RebuildReportStillFlagsAQuarantinedRecordThatVanishedFromDisk)
{
    account_index::quarantine("corrupt@example.com",
        "accounts/A-E/corrupt@example.com/account.json", "unparseable JSON");

    // Nothing on disk corresponds to it any more (e.g. someone deleted the bad file by hand without
    // rebooting) -- the index still thinks it owns this key, and that IS drift.
    const std::vector<account_index::Entry> on_disk;

    const std::vector<std::string> disagreements = account_index::rebuild_report(on_disk);
    ASSERT_EQ(disagreements.size(), 1u);
    EXPECT_NE(disagreements[0].find("corrupt@example.com"), std::string::npos);
}

} // namespace

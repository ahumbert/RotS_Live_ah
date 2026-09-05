#include "account_index.h"
#include "account_management_types.h"

#include <gtest/gtest.h>

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

} // namespace

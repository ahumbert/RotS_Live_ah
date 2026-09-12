#include "../account_errors.h"

#include <gtest/gtest.h>

#include <string>

namespace {

class AccountErrorsTest : public ::testing::Test {
protected:
    void SetUp() override { account_errors::clear(); }
    void TearDown() override { account_errors::clear(); }
};

TEST_F(AccountErrorsTest, RecordsAFailureWithBothNamesAndTheReason)
{
    account_errors::record(account_errors::Source::Migration, "alpha-admin", "aragorn",
        "legacy player data could not be converted");

    const std::vector<account_errors::Entry> entries = account_errors::recent(10);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].source, account_errors::Source::Migration);
    EXPECT_EQ(entries[0].account, "alpha-admin");
    EXPECT_EQ(entries[0].character, "aragorn");
    EXPECT_EQ(entries[0].reason, "legacy player data could not be converted");
    EXPECT_GT(entries[0].when, 0);
}

TEST_F(AccountErrorsTest, ReportsTheNewestFailureFirst)
{
    account_errors::record(account_errors::Source::Boot, "acct", "first", "one");
    account_errors::record(account_errors::Source::Save, "acct", "second", "two");

    const std::vector<account_errors::Entry> entries = account_errors::recent(10);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].character, "second") << "the newest failure is the one an immortal is asking about";
    EXPECT_EQ(entries[1].character, "first");
}

TEST_F(AccountErrorsTest, ReturnsNoMoreThanTheRequestedNumber)
{
    for (int index = 0; index < 5; ++index)
        account_errors::record(account_errors::Source::Boot, "acct", "char" + std::to_string(index), "reason");

    const std::vector<account_errors::Entry> entries = account_errors::recent(2);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].character, "char4");
    EXPECT_EQ(entries[1].character, "char3");
}

TEST_F(AccountErrorsTest, KeepsOnlyTheMostRecentFailuresSoALongUptimeCannotGrowIt)
{
    for (std::size_t index = 0; index < account_errors::MAX_RECORDED_ERRORS + 25; ++index)
        account_errors::record(account_errors::Source::Boot, "acct", "char" + std::to_string(index), "reason");

    EXPECT_EQ(account_errors::size(), account_errors::MAX_RECORDED_ERRORS);

    const std::vector<account_errors::Entry> entries = account_errors::recent(account_errors::MAX_RECORDED_ERRORS);
    ASSERT_FALSE(entries.empty());
    EXPECT_EQ(entries[0].character, "char" + std::to_string(account_errors::MAX_RECORDED_ERRORS + 24));
    EXPECT_EQ(entries.back().character, "char25") << "the oldest entries are the ones dropped";
}

TEST_F(AccountErrorsTest, BoundsAStoredFieldSoOneEnormousReasonCannotBloatTheRing)
{
    const std::string enormous_reason(account_errors::MAX_RECORDED_FIELD_LENGTH * 4, 'x');
    account_errors::record(account_errors::Source::Save, "acct", "aragorn", enormous_reason);

    const std::vector<account_errors::Entry> entries = account_errors::recent(1);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_LE(entries[0].reason.size(), account_errors::MAX_RECORDED_FIELD_LENGTH);
}

TEST_F(AccountErrorsTest, AnnouncesTheFailureAsAGreppableLine)
{
    testing::internal::CaptureStderr();
    account_errors::record(account_errors::Source::Migration, "alpha-admin", "aragorn", "description exceeds 511 bytes");
    const std::string logged = testing::internal::GetCapturedStderr();

    // The fixed field order is the point: `acct=<x> char=<y>` has to be searchable as one string.
    EXPECT_NE(logged.find("ACCTERR migration acct=alpha-admin char=aragorn: description exceeds 511 bytes"),
        std::string::npos)
        << logged;
}

TEST_F(AccountErrorsTest, RendersAMissingNameRatherThanChangingTheShapeOfTheLine)
{
    testing::internal::CaptureStderr();
    account_errors::record(account_errors::Source::Boot, "", "aragorn", "unreadable");
    const std::string logged = testing::internal::GetCapturedStderr();

    EXPECT_NE(logged.find("ACCTERR boot acct=? char=aragorn: unreadable"), std::string::npos) << logged;
}

} // namespace

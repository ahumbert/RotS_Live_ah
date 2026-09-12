#include "account_errors.h"

#include "utils.h"

#include <deque>

namespace account_errors {

namespace {

    // A deque rather than a vector: entries are appended at one end and dropped from the other, and
    // the ring is small enough that nothing here needs to be cleverer than that.
    std::deque<Entry> g_entries;

    std::string bounded(const std::string& value)
    {
        if (value.size() <= MAX_RECORDED_FIELD_LENGTH)
            return value;
        return value.substr(0, MAX_RECORDED_FIELD_LENGTH);
    }

    // "?" rather than nothing: a field that disappears changes the shape of the line, and the shape
    // is what makes `acct=<x> char=<y>` searchable.
    const char* displayable(const std::string& value)
    {
        return value.empty() ? "?" : value.c_str();
    }

} // namespace

const char* source_name(Source source)
{
    switch (source) {
    case Source::Boot:
        return "boot";
    case Source::Migration:
        return "migration";
    case Source::Save:
        return "save";
    }
    return "unknown";
}

void record(Source source, const std::string& account, const std::string& character,
    const std::string& reason, const std::string& actor)
{
    Entry entry;
    entry.when = std::time(nullptr);
    entry.source = source;
    entry.account = bounded(account);
    entry.character = bounded(character);
    entry.actor = bounded(actor);
    entry.reason = bounded(reason);

    g_entries.push_back(entry);
    while (g_entries.size() > MAX_RECORDED_ERRORS)
        g_entries.pop_front();

    // Logged from here rather than at each call site, so the stored entry and the line in the log
    // can never disagree about what happened.
    const std::string by = entry.actor.empty() ? std::string() : " by=" + entry.actor;
    vmudlog(BRF, "ACCTERR %s acct=%s char=%s%s: %s", source_name(source),
        displayable(entry.account), displayable(entry.character), by.c_str(),
        displayable(entry.reason));
}

std::vector<Entry> recent(std::size_t limit)
{
    std::vector<Entry> entries;
    if (limit == 0)
        return entries;

    entries.reserve(limit < g_entries.size() ? limit : g_entries.size());
    for (auto entry = g_entries.rbegin(); entry != g_entries.rend() && entries.size() < limit; ++entry)
        entries.push_back(*entry);
    return entries;
}

std::size_t size()
{
    return g_entries.size();
}

void clear()
{
    g_entries.clear();
}

} // namespace account_errors

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "revision.hpp"

#include "error.hpp"
#include "logging.hpp"
#include "process.hpp"

#include <chrono>
#include <format>
#include <initializer_list>
#include <string>

namespace mfb
{

quill::Logger *const logBuild = log::logger("build");

namespace
{

/**
 * Create the command that runs git with `arguments` in the repository of `dir`.
 */
Command git_command(const fs::path &dir, std::initializer_list<std::string> arguments, bool quiet)
{
    Command command{.argv = {"git", "-C", dir.string()}, .quiet = quiet};
    command.argv.insert(command.argv.end(), arguments);
    return command;
}

/**
 * Run git in the repository of `dir` and return what it prints.
 */
std::string git(const fs::path &dir, std::initializer_list<std::string> arguments)
{
    return run_capture(git_command(dir, arguments, false));
}

/**
 * Run git in the repository of `dir` and return what it prints, or nothing
 * if it fails.
 */
std::optional<std::string> try_git(const fs::path &dir, std::initializer_list<std::string> arguments)
{
    try {
        return run_capture(git_command(dir, arguments, true));
    } catch (const BuildError &) {
        return std::nullopt;
    }
}

/**
 * Parse a commit count that git printed.
 */
int64_t parse_count(const std::string &text)
{
    auto count = parse_int(text);
    if (!count)
        fail("git printed '{}' instead of a commit count", text);
    return *count;
}

} // namespace

std::optional<RevisionChoice> parse_revision_choice(std::string_view text)
{
    using enum RevisionChoice::Kind;
    if (text == "auto")
        return RevisionChoice{.kind = Auto};
    if (text == "git")
        return RevisionChoice{.kind = Git};
    if (text == "time")
        return RevisionChoice{.kind = Time};
    auto number = parse_int(text);
    if (!number || *number < 0)
        return std::nullopt;
    return RevisionChoice{.kind = Number, .number = *number};
}

int64_t build_revision_from_time(int64_t epoch)
{
    using namespace std::chrono;
    sys_seconds when{seconds{epoch}};
    auto day = floor<days>(when);
    year_month_day date{day};
    auto day_of_year = (day - sys_days{date.year() / January / 1}).count() + 1;
    // time of day in 10-second slots, always four digits, so the number and
    // its decimal text sort the same way
    auto slot = (when - day).count() / 10;
    return ((static_cast<int>(date.year()) - 2024) * 1000 + day_of_year) * 10000 + slot;
}

bool is_in_git_tree(const fs::path &dir)
{
    for (auto path = fs::weakly_canonical(dir);; path = path.parent_path()) {
        // a .git file points to the repository of a worktree or submodule
        std::error_code error;
        if (fs::exists(path / ".git", error))
            return true;
        if (path == path.parent_path())
            return false;
    }
}

int64_t git_revision(const fs::path &dir)
{
    require_tool("git");
    // NOTE: This is the first git command, so it fails with git's own message
    // if git refuses to work with the repository.
    if (git(dir, {"rev-parse", "--is-shallow-repository"}) == "true")
        fail(
            "The git repository of {} is a shallow clone, which lacks commits to count for the revision. Fetch its "
            "whole history (git fetch --unshallow), or set the revision with --revision",
            dir.string());
    if (!try_git(dir, {"rev-parse", "--verify", "--quiet", "HEAD"}))
        fail("The git repository of {} has no commits to count for the revision yet", dir.string());

    auto tag = try_git(dir, {"describe", "--tags", "--abbrev=0", "--match=v*", "HEAD"});
    if (!tag) {
        auto count = parse_count(git(dir, {"rev-list", "--count", "HEAD"}));
        LOG_INFO(logBuild, "Revision {}: all commits, there is no v* tag", count);
        return count;
    }

    // the tagged commit counts too
    auto count = parse_count(git(dir, {"rev-list", "--count", std::format("refs/tags/{}..HEAD", *tag)})) + 1;
    LOG_INFO(logBuild, "Revision {}: the commits from tag {} on", count, *tag);
    return count;
}

int64_t resolve_revision(const RevisionChoice &choice, const fs::path &recipe_dir, int64_t epoch)
{
    switch (choice.kind) {
    case RevisionChoice::Kind::Number:
        LOG_INFO(logBuild, "Revision {}: set on the command line", choice.number);
        return choice.number;
    case RevisionChoice::Kind::Git:
        if (!is_in_git_tree(recipe_dir))
            fail("--revision=git counts commits, but {} is not in a git repository", recipe_dir.string());
        return git_revision(recipe_dir);
    case RevisionChoice::Kind::Auto:
        if (is_in_git_tree(recipe_dir))
            return git_revision(recipe_dir);
        break;
    case RevisionChoice::Kind::Time:
        break;
    }

    auto revision = build_revision_from_time(epoch);
    LOG_INFO(logBuild, "Revision {}: from SOURCE_DATE_EPOCH", revision);
    return revision;
}

} // namespace mfb

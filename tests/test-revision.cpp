/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "process.hpp"
#include "revision.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace mfb;
using Catch::Matchers::ContainsSubstring;
using mfb::test::TempDir;

namespace
{

/**
 * Run git in `repo`, as a committer that never signs anything.
 */
std::string git(const fs::path &repo, std::vector<std::string> arguments)
{
    arguments.insert(
        arguments.begin(),
        {"git",
         "-C",
         repo.string(),
         "-c",
         "user.name=Test",
         "-c",
         "user.email=test@example.org",
         "-c",
         "commit.gpgsign=false",
         "-c",
         "tag.gpgsign=false"});
    return run_capture({.argv = arguments, .quiet = true});
}

void commit(const fs::path &repo)
{
    git(repo, {"commit", "--quiet", "--allow-empty", "-m", "change"});
}

} // namespace

TEST_CASE("revision choices", "[revision]")
{
    using enum RevisionChoice::Kind;
    CHECK(parse_revision_choice("auto")->kind == Auto);
    CHECK(parse_revision_choice("git")->kind == Git);
    CHECK(parse_revision_choice("time")->kind == Time);
    auto number = parse_revision_choice("42");
    REQUIRE(number);
    CHECK(number->kind == Number);
    CHECK(number->number == 42);
    CHECK(parse_revision_choice("0")->number == 0);

    CHECK(!parse_revision_choice(""));
    CHECK(!parse_revision_choice("-1"));
    CHECK(!parse_revision_choice("4x"));
    CHECK(!parse_revision_choice("Git"));
}

TEST_CASE("build revisions", "[revision]")
{
    // 2026-01-01T00:00:01Z: two years since 2024, day 1, slot 0
    CHECK(build_revision_from_time(1767225601) == 20010000);
    // 2026-10-15T02:38:17Z: day 288, second 9497 = slot 949
    CHECK(build_revision_from_time(1792031897) == 22880949);
}

TEST_CASE("git revisions", "[revision]")
{
    if (!find_tool("git"))
        SKIP("git is not installed");
    TempDir dir("revision-git");
    auto repo = dir / "repo";
    fs::create_directories(repo / "recipes");
    fs::create_directories(dir / "plain");
    constexpr int64_t epoch = 1792031897;

    CHECK(!is_in_git_tree(dir / "plain"));
    CHECK(resolve_revision({.kind = RevisionChoice::Kind::Auto}, dir / "plain", epoch) == 22880949);
    CHECK_THROWS_WITH(
        resolve_revision({.kind = RevisionChoice::Kind::Git}, dir / "plain", epoch),
        ContainsSubstring("is not in a git repository"));

    git(repo, {"init", "--quiet", "--initial-branch=main"});
    CHECK(is_in_git_tree(repo / "recipes"));
    CHECK_THROWS_WITH(git_revision(repo / "recipes"), ContainsSubstring("has no commits"));

    // without a v* tag, every commit counts
    commit(repo);
    commit(repo);
    git(repo, {"tag", "release-1"});
    CHECK(git_revision(repo / "recipes") == 2);
    CHECK(resolve_revision({.kind = RevisionChoice::Kind::Auto}, repo / "recipes", epoch) == 2);

    // the tagged commit counts too
    git(repo, {"tag", "v1.0"});
    CHECK(git_revision(repo) == 1);
    commit(repo);
    commit(repo);
    CHECK(git_revision(repo) == 3);

    // the nearest tag wins, annotated or not
    git(repo, {"tag", "--annotate", "-m", "Release 1.1", "v1.1"});
    commit(repo);
    CHECK(git_revision(repo) == 2);

    // the time and given numbers ignore the repository
    CHECK(resolve_revision({.kind = RevisionChoice::Kind::Time}, repo, epoch) == 22880949);
    CHECK(resolve_revision({.kind = RevisionChoice::Kind::Number, .number = 7}, repo, epoch) == 7);

    // a shallow clone lacks the commits to count
    run_capture(
        {.argv = {"git", "clone", "--quiet", "--depth=1", "file://" + repo.string(), (dir / "shallow").string()},
         .quiet = true});
    CHECK_THROWS_WITH(
        resolve_revision({.kind = RevisionChoice::Kind::Auto}, dir / "shallow", epoch),
        ContainsSubstring("shallow clone"));
}

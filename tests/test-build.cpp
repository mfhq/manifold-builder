/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "build.hpp"
#include "core-build.hpp"
#include "sandbox.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace mfb;
using mfb::test::TempDir;

namespace
{

/**
 * Create an empty file, with the directories above it.
 */
void add_empty_file(const fs::path &file)
{
    fs::create_directories(file.parent_path());
    write_file(file, "");
}

/**
 * Make a file or directory look as if nobody used it for 40 days.
 */
void make_old(const fs::path &path)
{
    timespec old = {.tv_sec = time(nullptr) - 40 * 24 * 3600, .tv_nsec = 0};
    const timespec times[2] = {old, old};
    REQUIRE(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
}

} // namespace

TEST_CASE("Build: pruning the cache", "[build]")
{
    TempDir dir("cache");
    const auto &cache = dir.path();

    // not used for 40 days
    for (const auto *file : {"downloads/old", "packages/old.deb", "packages/lock", "packages/partial/old.deb"}) {
        add_empty_file(cache / file);
        make_old(cache / file);
    }
    for (const auto *mirror : {"git/old.git", "git/busy.git"}) {
        fs::create_directories(cache / mirror / "objects");
        make_old(cache / mirror);
    }

    // used lately
    add_empty_file(cache / "downloads/new");
    add_empty_file(cache / "packages/new.deb");
    fs::create_directories(cache / "git/new.git/objects");

    // another build is using this mirror
    int lock = open((cache / "git/busy.git.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    REQUIRE(lock >= 0);
    REQUIRE(flock(lock, LOCK_EX) == 0);

    CHECK(prune_cache(cache, std::chrono::days(30)) == 4);
    close(lock);

    // unused entries are gone
    CHECK(!fs::exists(cache / "downloads/old"));
    CHECK(!fs::exists(cache / "packages/old.deb"));
    CHECK(!fs::exists(cache / "packages/partial/old.deb"));
    CHECK(!fs::exists(cache / "git/old.git"));

    // what was used lately or is held by another build stays
    CHECK(fs::exists(cache / "downloads/new"));
    CHECK(fs::exists(cache / "packages/new.deb"));
    CHECK(fs::exists(cache / "git/new.git"));
    CHECK(fs::exists(cache / "git/busy.git"));

    // apt's lock is no package
    CHECK(fs::exists(cache / "packages/lock"));

    // a cache without some of its directories is fine
    CHECK(prune_cache(dir / "missing", std::chrono::days(30)) == 0);
}

TEST_CASE("Core: Manifold configuration", "[build][core]")
{
    Script script;
    postprocess_core(script, "/tree", "x86_64-linux-gnu");
    const auto &text = script.text();

    // libraries of the app
    CHECK(text.find("/tree/etc/ld.so.conf.d/manifold-app.conf") != std::string::npos);
    CHECK(text.find("/app/lib/x86_64-linux-gnu") != std::string::npos);

    // fonts of the host
    CHECK(text.find("/tree/etc/fonts/conf.d/50-manifold-host-fonts.conf") != std::string::npos);
    CHECK(
        text.find(R"(<remap-dir as-path="/usr/share/fonts">/run/host/usr/share/fonts</remap-dir>)")
        != std::string::npos);
}

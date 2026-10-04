/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "identity.hpp"
#include "process.hpp"
#include "sources/source.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <zip.h>

using namespace mfb;
using mfb::test::git;
using mfb::test::TempDir;

namespace
{

/**
 * Fetch a valid source into `destination`, with the scratch directory below `dir`.
 */
SourceRecord fetch(const Source &source, const fs::path &destination, const TempDir &dir)
{
    REQUIRE(check_source(source).empty());
    return source_fetcher_for(source.type)->fetch(source, destination, {.dir = dir.path(), .scratch = dir / "scratch"});
}

/**
 * Create a small project with a README and src/main.c.
 */
void make_project(const fs::path &dir)
{
    fs::create_directories(dir / "src");
    write_file(dir / "README", "hello\n");
    write_file(dir / "src/main.c", "int main(void) { return 0; }\n");
}

} // namespace

TEST_CASE("Sources: dir", "[sources]")
{
    TempDir dir("source-dir");
    make_project(dir / "project");

    auto record = fetch({.type = "dir", .path = "project"}, dir / "out", dir);

    // the tree is copied and pinned by its hash
    CHECK(read_file(dir / "out/src/main.c") == "int main(void) { return 0; }\n");
    CHECK(record.type == "dir");
    CHECK(record.path == "project");
    CHECK(record.pin() == tree_hash(dir / "project"));
    // a missing directory fails
    CHECK_THROWS_AS(fetch({.type = "dir", .path = "missing"}, dir / "out", dir), BuildError);
}

TEST_CASE("Sources: file", "[sources]")
{
    TempDir dir("source-file");
    write_file(dir / "launcher.sh", "#!/bin/sh\n");
    auto sha256 = checksum_file_hex(dir / "launcher.sh", ChecksumKind::Sha256);

    SECTION("a local file is copied and recorded with its checksum")
    {
        auto record = fetch({.type = "file", .path = "launcher.sh"}, dir / "out", dir);

        CHECK(fs::exists(dir / "out/launcher.sh"));
        CHECK(record.sha256 == sha256);

        // dest-filename renames it
        fetch({.type = "file", .path = "launcher.sh", .sha256 = sha256, .dest_filename = "run"}, dir / "out", dir);
        CHECK(fs::exists(dir / "out/run"));

        // a wrong checksum fails
        Source wrong{.type = "file", .path = "launcher.sh", .sha256 = checksum_data_hex("other", ChecksumKind::Sha256)};
        CHECK_THROWS_AS(fetch(wrong, dir / "out", dir), BuildError);
    }
}

TEST_CASE("Sources: archive", "[sources]")
{
    TempDir dir("source-archive");

    SECTION("the top directory of a tarball is stripped")
    {
        make_project(dir / "project-1.0");
        run({.argv = {"tar", "-czf", "project-1.0.tar.gz", "project-1.0"}, .cwd = dir.path()});
        auto sha256 = checksum_file_hex(dir / "project-1.0.tar.gz", ChecksumKind::Sha256);

        auto record = fetch({.type = "archive", .path = "project-1.0.tar.gz", .sha256 = sha256}, dir / "out", dir);

        CHECK(fs::exists(dir / "out/src/main.c"));
        CHECK(record.sha256 == sha256);

        // strip-components: 0 keeps it
        fetch({.type = "archive", .path = "project-1.0.tar.gz", .strip_components = 0}, dir / "kept", dir);
        CHECK(fs::exists(dir / "kept/project-1.0/README"));
    }

    SECTION("zip files are unpacked and stripped the same way")
    {
        if (!find_tool("unzip"))
            SKIP("unzip is not installed");

        // libzip reads the content on close
        const std::string readme = "hello\n";
        zip_t *archive = zip_open((dir / "project.zip").c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
        REQUIRE(archive != nullptr);
        auto *content = zip_source_buffer(archive, readme.data(), readme.size(), 0);
        zip_file_add(archive, "project-1.0/src/README", content, ZIP_FL_ENC_UTF_8);
        REQUIRE(zip_close(archive) == 0);

        fetch({.type = "archive", .path = "project.zip"}, dir / "out", dir);
        CHECK(read_file(dir / "out/src/README") == "hello\n");
        CHECK(!fs::exists(dir / "out/project-1.0"));
    }
}

TEST_CASE("Sources: patch", "[sources]")
{
    TempDir dir("source-patch");
    make_project(dir / "project");
    write_file(dir / "fix.patch", R"(--- a/README
+++ b/README
@@ -1 +1 @@
-hello
+hello, patched
)");

    fetch({.type = "dir", .path = "project"}, dir / "out", dir);
    auto record = fetch({.type = "patch", .path = "fix.patch"}, dir / "out", dir);

    CHECK(read_file(dir / "out/README") == "hello, patched\n");
    CHECK(record.sha256 == checksum_file_hex(dir / "fix.patch", ChecksumKind::Sha256));

    // applying it again fails instead of reversing the patch
    CHECK_THROWS_AS(fetch({.type = "patch", .path = "fix.patch"}, dir / "out", dir), BuildError);
    CHECK(read_file(dir / "out/README") == "hello, patched\n");
}

TEST_CASE("Sources: git", "[sources]")
{
    if (!find_tool("git"))
        SKIP("git is not installed");

    TempDir dir("source-git");
    const auto repo = dir / "repo";
    const auto url = repo.string();

    make_project(repo);
    git(repo, {"init", "--quiet", "--initial-branch=main"});
    git(repo, {"add", "."});
    git(repo, {"commit", "--quiet", "-m", "initial"});
    git(repo, {"tag", "v1.0"});
    auto commit = git(repo, {"rev-parse", "HEAD"});

    // a tag is pinned to its commit
    auto record = fetch({.type = "git", .url = url, .tag = "v1.0"}, dir / "out", dir);
    CHECK(record.pin() == commit);
    CHECK(fs::exists(dir / "out/src/main.c"));

    // after an earlier source, the checkout is copied over its files
    write_file(dir / "extra", "x");
    fetch({.type = "file", .path = "extra"}, dir / "both", dir);
    fetch({.type = "git", .url = url}, dir / "both", dir);
    CHECK(fs::exists(dir / "both/extra"));
    CHECK(fs::exists(dir / "both/README"));

    // a tag must point at the commit given with it
    Source mismatch{
        .type = "git", .url = url, .tag = "v1.0", .commit = checksum_data_hex("x", ChecksumKind::Sha256).substr(0, 40)};
    CHECK_THROWS_AS(fetch(mismatch, dir / "mismatch", dir), BuildError);
}

TEST_CASE("Sources: parts", "[sources]")
{
    TempDir dir("fetch-sources");
    make_project(dir / "project");
    write_file(dir / "data.txt", "data\n");

    Part hello{
        .name = "hello",
        .buildsystem = "meson",
        .sources = {{.type = "dir", .path = "project"}, {.type = "file", .dest = "extra/data", .path = "data.txt"}},
        .dir = dir.path(),
    };
    Part no_sources{.name = "nothing", .buildsystem = "simple", .build_commands = {"true"}};

    auto records = fetch_sources({&hello, &no_sources}, dir / "sources", dir / "scratch");

    REQUIRE(records.size() == 2);
    CHECK(records[0].part == "hello");
    CHECK(fs::exists(dir / "sources/hello/src/main.c"));
    CHECK(fs::exists(dir / "sources/hello/extra/data/data.txt"));

    // a part without sources still gets a directory to build in
    CHECK(fs::is_directory(dir / "sources/nothing"));
}

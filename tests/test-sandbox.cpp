/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "process.hpp"
#include "sandbox.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sys/xattr.h>
#include <unistd.h>

using namespace mfb;
using mfb::test::TempDir;

namespace
{

/**
 * Run a script like Sandbox::run does, minus the namespace, and describe
 * its failure.
 */
std::string failure_of(const Script &script)
{
    TempDir dir("sandbox");
    auto report = dir / "report";
    auto text = std::format("exec {}>{}{}", REPORT_FD, shell_quote(report.string()), script.text());
    int status = run_status({.argv = {"bash", "-c", text}, .quiet = true});
    REQUIRE(status != 0);
    return describe_failure(script, read_file(report), status);
}

/**
 * Add recipe commands to the script, traced like the builder runs them.
 */
void run_traced(Script &script, std::string_view commands)
{
    script.run({"bash", "-Eeuo", "pipefail", "-c", traced_script(commands)});
}

} // namespace

TEST_CASE("Script: traces show the recipe's commands only", "[sandbox]")
{
    const auto output = run_capture({.argv = {"bash", "-c", "exec 2>&1\n" + traced_script("echo hello\n")}});
    CHECK(output == "+ echo hello\nhello");
}

TEST_CASE("Script: failure reports", "[sandbox]")
{
    Script script;

    SECTION("a failing recipe command is reported with its step")
    {
        script.run({"true"});
        script.begin_step("action 'greet'");
        run_traced(script, "echo hello\nls /nonexistent\necho never\n");
        script.end_step();
        CHECK(failure_of(script) == "action 'greet': Command failed with status 2: ls /nonexistent");
    }

    SECTION("the recipe's own line is reported, not the command inside it")
    {
        script.begin_step("part 'hello'");
        run_traced(script, "greeting=$(false)\n");
        script.end_step();
        CHECK(failure_of(script) == "part 'hello': Command failed with status 1: greeting=$(false)");
    }

    SECTION("a failure that did not stop the script is not reported")
    {
        script.begin_step("part 'hello'");
        run_traced(script, "echo \"$(false; echo hello)\"\nls /nonexistent\n");
        script.end_step();
        CHECK(failure_of(script) == "part 'hello': Command failed with status 2: ls /nonexistent");
    }

    SECTION("a failing command of our own names only the step")
    {
        script.begin_step("bootstrapping the core");
        script.run({"true"});
        script.end_step();
        script.begin_step("installing packages");
        script.run({"bash", "-c", "exit 100"});
        script.end_step();
        CHECK(failure_of(script) == "installing packages failed with status 100");
    }

    SECTION("a failure outside of any step points at --verbose")
    {
        script.begin_step("part 'hello'");
        script.run({"true"});
        script.end_step();
        script.run({"false"});
        CHECK(failure_of(script) == "Build script failed with status 1 (run with --verbose to see the script)");
    }
}

TEST_CASE("Sandbox: strip_overlay_markers", "[sandbox]")
{
    if (!find_tool("getfattr") || !find_tool("setfattr"))
        SKIP("getfattr or setfattr is not installed");

    auto mark = [](const fs::path &path, const char *name) {
        return setxattr(path.c_str(), name, "y", 1, 0) == 0;
    };
    auto marked = [](const fs::path &path, const char *name) {
        return getxattr(path.c_str(), name, nullptr, 0) >= 0;
    };

    TempDir dir("markers");
    fs::create_directories(dir / "usr/lib");
    fs::create_directories(dir / "with space");
    // an upper layer is full of links into the layers below it
    fs::create_symlink("libfoo.so.1", dir / "usr/lib/libfoo.so");

    if (!mark(dir / "usr/lib", "user.overlay.opaque"))
        SKIP("the temporary directory has no user extended attributes");
    REQUIRE(mark(dir / "with space", "user.overlay.opaque"));
    REQUIRE(mark(dir / "usr", "user.other"));

    Script script;
    strip_overlay_markers(script, dir.path());

    SECTION("the markers are removed from the marked directories")
    {
        CHECK(run_status({.argv = {"bash", "-c", script.text()}}) == 0);

        CHECK(!marked(dir / "usr/lib", "user.overlay.opaque"));
        CHECK(!marked(dir / "with space", "user.overlay.opaque"));
        // other attributes are none of our business
        CHECK(marked(dir / "usr", "user.other"));
    }

    SECTION("a marker that stays fails the script")
    {
        if (geteuid() == 0)
            SKIP("root can change attributes of any directory");

        fs::create_directories(dir / "locked");
        REQUIRE(mark(dir / "locked", "user.overlay.opaque"));
        fs::permissions(dir / "locked", fs::perms::owner_write, fs::perm_options::remove);
        CHECK(run_status({.argv = {"bash", "-c", script.text()}, .quiet = true}) != 0);
        fs::permissions(dir / "locked", fs::perms::owner_write, fs::perm_options::add);
    }
}

TEST_CASE("Sandbox: move_paths", "[sandbox]")
{
    TempDir dir("move");
    auto tree = dir / "tree";
    fs::create_directories(tree / "usr/include/foo");
    write_file(tree / "usr/include/foo/foo.h", "");
    fs::create_directories(tree / "usr/share/doc/foo");
    write_file(tree / "usr/share/doc/foo/copyright", "");
    // the link's target is in a layer below
    fs::create_directories(tree / "usr/lib");
    fs::create_symlink("libfoo.so.1", tree / "usr/lib/libfoo.so");

    Script script;

    SECTION("listed paths and globs move to the SDK tree")
    {
        const auto cwd = shell_quote(dir.path().string());
        script.line("cd " + cwd);
        move_paths(script, tree, "usr", {"include", "lib/*.so", "share/doc", "nothing/*"}, dir / "sdk");

        // the shell options and the directory are the script's own again
        script.line("! shopt -q globstar && ! shopt -q nullglob && [ \"$PWD\" = " + cwd + " ]");

        CHECK(run_status({.argv = {"bash", "-c", script.text()}}) == 0);

        // everything listed is in the SDK now
        CHECK(fs::exists(dir / "sdk/usr/include/foo/foo.h"));
        CHECK(fs::exists(dir / "sdk/usr/share/doc/foo/copyright"));
        CHECK(fs::is_symlink(dir / "sdk/usr/lib/libfoo.so"));

        // and gone from the tree
        CHECK(!fs::exists(tree / "usr/include"));
        CHECK(!fs::exists(tree / "usr/share/doc"));
        CHECK(!fs::exists(fs::symlink_status(tree / "usr/lib/libfoo.so")));
    }

    SECTION("a path that could not be copied stays where it is")
    {
        // no directory can be made below a file
        write_file(dir / "file", "");
        move_paths(script, tree, "usr", {"include"}, dir / "file/sdk");

        CHECK(run_status({.argv = {"bash", "-c", script.text()}, .quiet = true}) != 0);
        CHECK(fs::exists(tree / "usr/include"));
    }
}

TEST_CASE("Sandbox: merge_into_usr", "[sandbox]")
{
    TempDir dir("merge-usr");
    fs::create_directories(dir / "lib/udev/rules.d");
    write_file(dir / "lib/udev/rules.d/60-foo.rules", "");
    fs::create_directories(dir / "usr/lib/udev/rules.d");
    write_file(dir / "usr/lib/udev/rules.d/50-bar.rules", "");
    fs::create_directories(dir / "sbin");
    write_file(dir / "sbin/foo", "");
    fs::create_directories(dir / "etc");
    fs::create_symlink("usr/bin", dir / "bin");

    Script script;
    merge_into_usr(script, dir.path());

    CHECK(run_status({.argv = {"bash", "-c", script.text()}}) == 0);

    // directories were merged into their /usr counterparts
    CHECK(fs::exists(dir / "usr/lib/udev/rules.d/60-foo.rules"));
    CHECK(fs::exists(dir / "usr/lib/udev/rules.d/50-bar.rules"));
    CHECK(fs::exists(dir / "usr/sbin/foo"));
    CHECK(!fs::exists(dir / "lib"));
    CHECK(!fs::exists(dir / "sbin"));

    // links and everything else stay
    CHECK(fs::is_symlink(dir / "bin"));
    CHECK(fs::is_directory(dir / "etc"));
}

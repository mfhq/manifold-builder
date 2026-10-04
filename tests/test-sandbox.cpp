/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "process.hpp"
#include "sandbox.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>

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

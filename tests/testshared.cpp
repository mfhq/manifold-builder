/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "testshared.hpp"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "logging.hpp"
#include "process.hpp"

namespace mfb::test
{

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

/**
 * Sets up the global state that main() would set up for the builder.
 */
class TestSetupListener : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo &) override
    {
        // log verbosely, so a failing test shows the debug messages leading up to it
        log::init(true);
    }
};

} // namespace mfb::test

CATCH_REGISTER_LISTENER(mfb::test::TestSetupListener)

/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "utils.hpp"

namespace mfb
{

/**
 * How the builder picks the revision of the images it makes.
 */
struct RevisionChoice {
    enum class Kind {
        Auto,   ///< git if the recipe is in a git repository, time otherwise
        Git,    ///< the commits of HEAD from the last v* tag on
        Time,   ///< the build time
        Number, ///< a revision given on the command line
    };

    Kind kind = Kind::Auto;
    int64_t number = 0; ///< The revision for Kind::Number
};

/**
 * Parse the value of --revision: a number, "auto", "git" or "time".
 */
std::optional<RevisionChoice> parse_revision_choice(std::string_view text);

/**
 * Compute a revision the builder gives a build at `epoch`.
 *
 * The revision is one number made of the years since 2024, the day of the
 * year in three digits and the time of day in 10-second slots in four digits,
 * all in UTC (22880949 for 2026-10-15T02:38:17Z). This way a later build gets
 * a higher revision without any repository involved. A repository may
 * replace it with a count of its own.
 */
int64_t build_revision_from_time(int64_t epoch);

/**
 * Check whether `dir` is in a git work tree.
 *
 * We look for .git ourselves, so a repository git refuses to work with
 * (e.g. one owned by a different user) still counts.
 */
bool is_in_git_tree(const fs::path &dir);

/**
 * Count the commits of HEAD in the git repository of `dir`, from the last
 * tag starting with "v" on.
 *
 * The tagged commit counts too, so the revision is never 0. Without such a
 * tag, we count all commits of HEAD. A shallow clone lacks commits to count,
 * so we refuse it.
 */
int64_t git_revision(const fs::path &dir);

/**
 * Pick the revision of a build of the recipe in `recipe_dir` at `epoch`.
 */
int64_t resolve_revision(const RevisionChoice &choice, const fs::path &recipe_dir, int64_t epoch);

} // namespace mfb

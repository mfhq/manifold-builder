/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "error.hpp"
#include "identity.hpp"
#include "manifest.hpp"
#include "recipe.hpp"
#include "utils.hpp"

#include <format>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace mfb::test
{

/**
 * Get the tests/ directory of the source tree.
 */
inline fs::path tests_dir()
{
    return MFB_TESTS_DIR;
}

/**
 * Temporary directory that is removed with its contents on destruction.
 */
class TempDir
{
public:
    explicit TempDir(std::string_view name)
        : m_path(fs::temp_directory_path() / std::format("mfb-test-{}-{}", name, getpid()))
    {
        fs::remove_all(m_path);
        fs::create_directories(m_path);
    }

    ~TempDir()
    {
        std::error_code ignored;
        fs::remove_all(m_path, ignored);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    const fs::path &path() const
    {
        return m_path;
    }

    fs::path operator/(const fs::path &relative) const
    {
        return m_path / relative;
    }

private:
    fs::path m_path;
};

/**
 * Create a manifest of version 1, revision 1 for x86_64, with hashes made from the name.
 */
inline Manifest fake_manifest(const std::string &component, const std::string &kind)
{
    return Manifest{
        .component = component,
        .kind = kind,
        .version = "1",
        .revision = 1,
        .architecture = "x86_64",
        .identity = checksum_data_hex(component, ChecksumKind::Sha256),
        .content_hash = checksum_data_hex(component + " 1", ChecksumKind::Sha256),
    };
}

/**
 * Run git in `repo`, as a committer that never signs anything.
 */
std::string git(const fs::path &repo, std::vector<std::string> arguments);

/**
 * Check whether parsing the recipe text fails.
 */
inline bool recipe_rejected(std::string_view yaml, const fs::path &dir = fs::current_path())
{
    try {
        parse_recipe(yaml, "inline", dir);
    } catch (const BuildError &) {
        return true;
    }
    return false;
}

/**
 * Start of an app recipe, which still needs parts to be valid.
 */
constexpr std::string_view APP_HEADER = R"(component: com.example.app
version: 1
base: net.example.core
base-version: 1
)";

/**
 * A complete minimal app recipe that tests append keys to.
 */
constexpr std::string_view APP_RECIPE = R"(component: com.example.app
version: 1
base: net.example.core
base-version: 1
parts:
- name: hello
  buildsystem: meson
)";

} // namespace mfb::test

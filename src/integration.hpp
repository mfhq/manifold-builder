/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#pragma once

#include "manifest.hpp"
#include "recipe.hpp"
#include "utils.hpp"

#include <optional>
#include <string>

namespace mfb
{

/**
 * Rename files below /app as the recipe says, in the tree of the image. A unit
 * or a desktop entry of the app that refers to a renamed unit or icon by its
 * name gets the new name. Fails the build if a pattern matches no file, if a
 * new name is taken, or if units that belong together by their names would be
 * separated.
 */
void rename_files(const fs::path &app, const Recipe &recipe);

/**
 * Find what an application installs on the host in the tree of its image: what
 * the recipe lists, the desktop entry named after the application, and the
 * icons of all desktop entries.
 *
 * @return What to record in the manifest, or nothing if the application
 *     installs nothing on the host.
 */
std::optional<IntegrationInfo> collect_integration(const fs::path &tree, const Recipe &recipe);

/**
 * Check a finished manifest and the files in the tree it describes with the
 * rules Manifold installs them by, so a build can not produce a bundle that
 * does not install.
 */
void check_integration(const fs::path &tree, const std::string &manifest_json);

} // namespace mfb

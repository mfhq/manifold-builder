/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "integration.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace mfb;
using Catch::Matchers::ContainsSubstring;
using mfb::test::recipe_rejected;
using mfb::test::TempDir;

namespace
{

constexpr std::string_view APP_RECIPE =
    "component: com.example.app\nversion: 1\nbase: c.d\nbase-version: 1\ncommand: app\n"
    "parts: [{name: p, buildsystem: meson}]\n";

const std::string ENTRY = "[Desktop Entry]\nType=Application\nName=App\nExec=app\nIcon=com.example.app\n";

/**
 * Add a file to the /app of a tree.
 */
void add_file(const TempDir &tree, const std::string &path, const std::string &content = ENTRY)
{
    auto file = tree / "app" / path;
    fs::create_directories(file.parent_path());
    write_file(file, content);
}

Recipe app_recipe(std::string_view integration = "")
{
    return parse_recipe(std::string(APP_RECIPE) + std::string(integration), "inline");
}

/**
 * Get the manifest of an application that installs what `integration` holds.
 */
std::string finished_manifest(const TempDir &work, const IntegrationInfo &integration)
{
    Manifest manifest{
        .component = "com.example.app",
        .kind = "app",
        .version = "1",
        .revision = 1,
        .architecture = "x86_64",
        .identity = checksum_data_hex("identity", ChecksumKind::Sha256),
        .content_hash = checksum_data_hex("content", ChecksumKind::Sha256),
        .base =
            BaseRef{
                "c.d",
                "1",
                1,
                false,
                checksum_data_hex("base identity", ChecksumKind::Sha256),
                checksum_data_hex("base content", ChecksumKind::Sha256),
            },
        .app = AppInfo{"app", "roaming", false},
        .integration = integration,
        .image = test::fake_image(work.path(), "app.raw"),
    };
    return to_json(manifest);
}

} // namespace

TEST_CASE("a recipe lists what an application installs on the host", "[integration]")
{
    auto recipe = app_recipe(
        "integration:\n"
        "  desktop-entry: [com.example.app.open.desktop]\n"
        "  udev-rule: [60-com.example.app.rules]\n"
        "  alias:\n"
        "  - app\n"
        "  - {name: app-report, command: bin/report}\n");
    REQUIRE(recipe.integration);
    CHECK(recipe.integration->desktop_entry == std::vector<std::string>{"com.example.app.open.desktop"});
    REQUIRE(recipe.integration->alias.size() == 2);
    CHECK(std::get<std::string>(recipe.integration->alias[0]) == "app");
    CHECK(std::get<AliasSettings>(recipe.integration->alias[1]).command == "bin/report");

    CHECK(app_recipe().hash != recipe.hash);
}

TEST_CASE("names that Manifold would refuse fail the recipe", "[integration]")
{
    const std::string app(APP_RECIPE);
    CHECK(recipe_rejected(app + "integration: {desktop-entry: [other.desktop]}\n"));
    CHECK(recipe_rejected(app + "integration: {desktop-entry: [com.example.app.txt]}\n"));
    CHECK(recipe_rejected(app + "integration: {desktop-entry: [dir/com.example.app.desktop]}\n"));
    CHECK(recipe_rejected(app + "integration: {udev-rule: [com.example.app.rules]}\n"));
    CHECK(recipe_rejected(app + "integration: {systemd-unit-system: [com.example.app.exe]}\n"));
    CHECK(recipe_rejected(app + "integration: {alias: [a/b]}\n"));
    CHECK(recipe_rejected(app + "integration: {alias: [{name: x, command: ../bin/x}]}\n"));
    CHECK(recipe_rejected(app + "integration: {alias: [{name: x}]}\n"));
    CHECK(recipe_rejected(app + "integration: {unknown-kind: [x]}\n"));

    // only an application can install anything on the host
    CHECK(recipe_rejected(std::string(test::PLATFORM_HEADER) + "integration: {alias: [x]}\n"));

    // the command ends up in generated files
    CHECK(recipe_rejected(
        "component: com.example.app\nversion: 1\nbase: c.d\nbase-version: 1\ncommand: 'a b'\n"
        "parts: [{name: p, buildsystem: meson}]\n"));
}

TEST_CASE("the main desktop entry and its icons need no mention", "[integration]")
{
    TempDir tree("integration-auto");
    add_file(tree, "share/applications/com.example.app.desktop");
    add_file(tree, "share/applications/com.example.app.other.desktop");
    add_file(tree, "share/icons/hicolor/48x48/apps/com.example.app.png", "png");
    add_file(tree, "share/icons/hicolor/64x64@2/apps/com.example.app.png", "png");
    add_file(tree, "share/icons/hicolor/scalable/apps/com.example.app.svg", "<svg/>");
    add_file(tree, "share/icons/hicolor/32x32/mimetypes/com.example.app.png", "png");
    add_file(tree, "share/icons/hicolor/48x48/actions/com.example.app.png", "png");
    add_file(tree, "share/icons/hicolor/48x48/apps/com.example.app.xpm", "xpm");
    add_file(tree, "share/icons/hicolor/48x48/apps/com.example.app.unused.png", "png");
    add_file(tree, "bin/app", "#!/bin/sh\n");

    auto info = collect_integration(tree.path(), app_recipe());
    REQUIRE(info);
    REQUIRE(info->desktop_entry.size() == 1);
    CHECK(info->desktop_entry[0].path == "/app/share/applications/com.example.app.desktop");
    CHECK(info->desktop_entry[0].sha256 == checksum_data_hex(ENTRY, ChecksumKind::Sha256));
    CHECK(info->desktop_entry[0].blake3 == checksum_data_hex(ENTRY, ChecksumKind::Blake3));

    // sorted, so that the manifest does not depend on the order of a directory
    REQUIRE(info->icons.size() == 4);
    CHECK(info->icons[0].path == "/app/share/icons/hicolor/32x32/mimetypes/com.example.app.png");
    CHECK(info->icons[1].path == "/app/share/icons/hicolor/48x48/apps/com.example.app.png");
    CHECK(info->icons[2].path == "/app/share/icons/hicolor/64x64@2/apps/com.example.app.png");
    CHECK(info->icons[3].path == "/app/share/icons/hicolor/scalable/apps/com.example.app.svg");

    // a recipe may name that desktop entry too
    info = collect_integration(tree.path(), app_recipe("integration: {desktop-entry: [com.example.app.desktop]}\n"));
    REQUIRE(info);
    CHECK(info->desktop_entry.size() == 1);
}

TEST_CASE("an application without integration gets no section", "[integration]")
{
    TempDir tree("integration-none");
    add_file(tree, "bin/app", "#!/bin/sh\n");
    CHECK_FALSE(collect_integration(tree.path(), app_recipe()));
}

TEST_CASE("icons are only collected if they can be installed", "[integration]")
{
    TempDir tree("integration-icons");
    add_file(tree, "share/applications/com.example.app.desktop", ENTRY + "[Desktop Action a]\nIcon=theirs\n");
    add_file(tree, "share/icons/hicolor/48x48/apps/theirs.png", "png");
    add_file(tree, "share/icons/hicolor/48x48/apps/com.example.app.png", "png");
    fs::create_directories(tree / "app/share/icons/hicolor/64x64/apps");
    fs::create_symlink(
        "../../48x48/apps/com.example.app.png", tree / "app/share/icons/hicolor/64x64/apps/com.example.app.png");

    auto info = collect_integration(tree.path(), app_recipe());
    REQUIRE(info);
    REQUIRE(info->icons.size() == 1);
    CHECK(info->icons[0].path == "/app/share/icons/hicolor/48x48/apps/com.example.app.png");
}

TEST_CASE("what a recipe lists has to be there", "[integration]")
{
    TempDir tree("integration-listed");
    add_file(tree, "lib/systemd/system/com.example.app.service", "[Service]\nExecStart=app\n");
    add_file(tree, "lib/udev/rules.d/60-com.example.app.rules", "");
    add_file(tree, "bin/report", "#!/bin/sh\n");
    fs::create_symlink("com.example.app.service", tree / "app/lib/systemd/system/com.example.app.link.service");

    auto info = collect_integration(
        tree.path(),
        app_recipe(
            "integration:\n"
            "  systemd-unit-system: [com.example.app.service]\n"
            "  alias: [{name: zz, command: bin/report}, report]\n"));
    REQUIRE(info);
    REQUIRE(info->systemd_unit_system.size() == 1);
    CHECK(info->systemd_unit_system[0].path == "/app/lib/systemd/system/com.example.app.service");
    REQUIRE(info->alias.size() == 2);
    CHECK(info->alias[0].name == "report");
    CHECK(info->alias[0].command == "/app/bin/report");
    CHECK(info->alias[1].name == "zz");
    CHECK(info->alias[1].command == "/app/bin/report");

    CHECK_THROWS_AS(
        collect_integration(tree.path(), app_recipe("integration: {mime-package: [com.example.app.xml]}\n")),
        BuildError);
    CHECK_THROWS_AS(
        collect_integration(tree.path(), app_recipe("integration: {udev-rule: [60-com.example.app.rules]}\n")),
        BuildError);
    CHECK_THROWS_AS(
        collect_integration(
            tree.path(), app_recipe("integration: {systemd-unit-system: [com.example.app.link.service]}\n")),
        BuildError);
    CHECK_THROWS_AS(collect_integration(tree.path(), app_recipe("integration: {alias: [missing]}\n")), BuildError);
}

TEST_CASE("the manifest is checked with the rules of Manifold", "[integration]")
{
    TempDir tree("integration-check");
    TempDir work("integration-check-work");
    add_file(tree, "bin/app", "#!/bin/sh\n");
    add_file(tree, "share/applications/com.example.app.desktop");
    add_file(tree, "lib/systemd/system/com.example.app.service", "[Service]\nExecStart=/bin/sh\n");

    auto recipe = app_recipe("integration: {alias: [app]}\n");
    auto info = collect_integration(tree.path(), recipe);
    REQUIRE(info);
    check_integration(tree.path(), finished_manifest(work, *info));

    // a file that changed after it was recorded
    add_file(tree, "share/applications/com.example.app.desktop", ENTRY + "Comment=New\n");
    CHECK_THROWS_WITH(
        check_integration(tree.path(), finished_manifest(work, *info)), ContainsSubstring("changed after"));

    // the main desktop entry has to run the command of the application
    add_file(
        tree, "share/applications/com.example.app.desktop", "[Desktop Entry]\nType=Application\nName=A\nExec=other\n");
    info = collect_integration(tree.path(), recipe);
    REQUIRE(info);
    CHECK_THROWS_AS(check_integration(tree.path(), finished_manifest(work, *info)), BuildError);

    // a unit that runs a program of the host
    add_file(tree, "share/applications/com.example.app.desktop");
    info = collect_integration(
        tree.path(), app_recipe("integration: {systemd-unit-system: [com.example.app.service]}\n"));
    REQUIRE(info);
    CHECK_THROWS_AS(check_integration(tree.path(), finished_manifest(work, *info)), BuildError);

    // names that no recipe can have, but a manifest could
    info = collect_integration(tree.path(), recipe);
    REQUIRE(info);
    info->alias[0].name = "sudo";
    CHECK_THROWS_AS(check_integration(tree.path(), finished_manifest(work, *info)), BuildError);
    info = collect_integration(tree.path(), recipe);
    REQUIRE(info);
    add_file(tree, "share/applications/org.other.desktop");
    info->desktop_entry[0].path = "/app/share/applications/org.other.desktop";
    CHECK_THROWS_WITH(
        check_integration(tree.path(), finished_manifest(work, *info)), ContainsSubstring("is not named like"));
}

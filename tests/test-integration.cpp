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
using mfb::test::APP_RECIPE;
using mfb::test::fake_image;
using mfb::test::fake_manifest;
using mfb::test::PLATFORM_HEADER;
using mfb::test::recipe_rejected;
using mfb::test::TempDir;

namespace
{

const std::string ENTRY_HEAD = "[Desktop Entry]\nType=Application\nName=App\n"; ///< start of every desktop entry
const std::string ENTRY = ENTRY_HEAD + "Exec=app\nIcon=com.example.app\n";      ///< main entry of the test app
const std::string PROGRAM = "#!/bin/sh\n";                                      ///< a program that does nothing

/**
 * Add a file to the /app of a tree.
 */
void add_file(const TempDir &tree, const std::string &path, const std::string &content = ENTRY)
{
    auto file = tree / "app" / path;
    fs::create_directories(file.parent_path());
    write_file(file, content);
}

/**
 * Parse the recipe of the test application, with more keys appended.
 */
Recipe app_recipe(std::string_view keys = "")
{
    return parse_recipe(std::string(APP_RECIPE) + std::string(keys), "inline");
}

/**
 * Get the manifest of an application that installs what `integration` holds.
 */
std::string finished_manifest(const TempDir &work, const IntegrationInfo &integration)
{
    auto manifest = fake_manifest("com.example.app", "app");
    manifest.base = fake_manifest("net.example.core", "kit").as_base();
    manifest.app = AppInfo{"app", "roaming", false};
    manifest.integration = integration;
    manifest.image = fake_image(work.path(), "app.raw");
    return to_json(manifest);
}

} // namespace

TEST_CASE("Host integration: recipe keys", "[integration]")
{
    const std::string app(APP_RECIPE);
    const std::string platform(PLATFORM_HEADER);

    // file names are checked per kind
    CHECK(recipe_rejected(app + "integration: {desktop-entry: [other.desktop]}\n"));
    CHECK(recipe_rejected(app + "integration: {desktop-entry: [com.example.app.txt]}\n"));
    CHECK(recipe_rejected(app + "integration: {systemd-unit-system: [com.example.app.exe]}\n"));
    CHECK(recipe_rejected(app + "integration: {desktop-entry: [dir/com.example.app.desktop]}\n"));
    // a udev rule starts with its priority
    CHECK(recipe_rejected(app + "integration: {udev-rule: [com.example.app.rules]}\n"));
    CHECK(recipe_rejected(app + "integration: {unknown-kind: [x]}\n"));

    // an alias is a program name and runs a path below /app
    CHECK(recipe_rejected(app + "integration: {alias: [a/b]}\n"));
    CHECK(recipe_rejected(app + "integration: {alias: [{name: report, command: ../bin/report}]}\n"));
    CHECK(recipe_rejected(app + "integration: {alias: [{name: report}]}\n"));

    // rename takes a path below /app and a plain new name
    CHECK(recipe_rejected(app + "rename: {../bin/sh: x}\n"));
    CHECK(recipe_rejected(app + "rename: {/etc/passwd: x}\n"));
    CHECK(recipe_rejected(app + "rename: {bin/app: sub/app}\n"));
    CHECK(recipe_rejected(app + "rename: {bin/app: ..}\n"));

    // only an application can install anything on the host
    CHECK(recipe_rejected(platform + "integration: {alias: [x]}\n"));
    CHECK(recipe_rejected(platform + "rename: {bin/app: x}\n"));
}

TEST_CASE("Host integration: collecting files", "[integration]")
{
    TempDir tree("integration-collect");
    add_file(tree, "bin/app", PROGRAM);

    // nothing to integrate gives no section
    CHECK(!collect_integration(tree.path(), app_recipe()));

    SECTION("the main desktop entry and its icons are collected without being listed")
    {
        // collected: the main entry and its icons in apps/ and mimetypes/
        add_file(tree, "share/applications/com.example.app.desktop");
        add_file(tree, "share/icons/hicolor/48x48/apps/com.example.app.png", "png");
        add_file(tree, "share/icons/hicolor/64x64@2/apps/com.example.app.png", "png");
        add_file(tree, "share/icons/hicolor/scalable/apps/com.example.app.svg", "<svg/>");
        add_file(tree, "share/icons/hicolor/32x32/mimetypes/com.example.app.png", "png");

        // left alone: an unlisted entry, other categories, other formats, other names
        add_file(tree, "share/applications/com.example.app.other.desktop");
        add_file(tree, "share/icons/hicolor/48x48/actions/com.example.app.png", "png");
        add_file(tree, "share/icons/hicolor/48x48/apps/com.example.app.xpm", "xpm");
        add_file(tree, "share/icons/hicolor/48x48/apps/com.example.app.unused.png", "png");

        auto info = collect_integration(tree.path(), app_recipe());
        REQUIRE(info);
        REQUIRE(info->desktop_entry.size() == 1);
        CHECK(info->desktop_entry[0].path == "/app/share/applications/com.example.app.desktop");
        CHECK(info->desktop_entry[0].sha256 == checksum_data_hex(ENTRY, ChecksumKind::Sha256));
        CHECK(info->desktop_entry[0].blake3 == checksum_data_hex(ENTRY, ChecksumKind::Blake3));

        // sorted, so that the manifest does not depend on the order of a directory
        REQUIRE(info->icon.size() == 4);
        CHECK(info->icon[0].path == "/app/share/icons/hicolor/32x32/mimetypes/com.example.app.png");
        CHECK(info->icon[1].path == "/app/share/icons/hicolor/48x48/apps/com.example.app.png");
        CHECK(info->icon[2].path == "/app/share/icons/hicolor/64x64@2/apps/com.example.app.png");
        CHECK(info->icon[3].path == "/app/share/icons/hicolor/scalable/apps/com.example.app.svg");

        // a recipe may name that desktop entry too
        info = collect_integration(
            tree.path(), app_recipe("integration: {desktop-entry: [com.example.app.desktop]}\n"));
        REQUIRE(info);
        CHECK(info->desktop_entry.size() == 1);
    }

    SECTION("icons are only collected if they can be installed")
    {
        // an action with an icon of another name
        add_file(tree, "share/applications/com.example.app.desktop", ENTRY + "[Desktop Action a]\nIcon=theirs\n");
        add_file(tree, "share/icons/hicolor/48x48/apps/theirs.png", "png");

        // the icon of the application, and a link to it
        add_file(tree, "share/icons/hicolor/48x48/apps/com.example.app.png", "png");
        fs::create_directories(tree / "app/share/icons/hicolor/64x64/apps");
        fs::create_symlink(
            "../../48x48/apps/com.example.app.png", tree / "app/share/icons/hicolor/64x64/apps/com.example.app.png");

        auto info = collect_integration(tree.path(), app_recipe());
        REQUIRE(info);
        REQUIRE(info->icon.size() == 1);
        CHECK(info->icon[0].path == "/app/share/icons/hicolor/48x48/apps/com.example.app.png");
    }

    SECTION("a desktop entry has to run a program of the image")
    {
        const std::string file = "share/applications/com.example.app.other.desktop";
        auto recipe = app_recipe("integration: {desktop-entry: [com.example.app.other.desktop]}\n");
        add_file(tree, "libexec/helper", PROGRAM);

        // collect an entry that runs the given command line
        auto collect_with = [&](const std::string &exec) {
            add_file(tree, file, ENTRY_HEAD + "Exec=" + exec + "\n");
            return collect_integration(tree.path(), recipe);
        };

        CHECK_NOTHROW(collect_with("app %U"));
        CHECK_NOTHROW(collect_with("\"app\" %U"));
        CHECK_NOTHROW(collect_with("/app/libexec/helper"));

        // a program of the base
        CHECK_THROWS_AS(collect_with("vulkaninfo"), BuildError);
        // a path outside of /app
        CHECK_THROWS_AS(collect_with("/usr/bin/app"), BuildError);
        // a program in the wrong place
        CHECK_THROWS_AS(collect_with("/app/bin/helper"), BuildError);
        // no program at all
        CHECK_THROWS_AS(collect_with(""), BuildError);

        // the actions of an entry run programs too
        add_file(tree, file, ENTRY_HEAD + "Exec=app\n\n[Desktop Action new]\nName=New\nExec=missing --new\n");
        CHECK_THROWS_WITH(collect_integration(tree.path(), recipe), ContainsSubstring("runs 'missing'"));
    }

    SECTION("listed files are recorded, and have to exist")
    {
        const std::string listed = R"(integration:
  systemd-unit-system: [com.example.app.service]
  alias: [{name: zz, command: bin/report}, report]
)";
        add_file(tree, "lib/systemd/system/com.example.app.service", "[Service]\nExecStart=app\n");
        add_file(tree, "lib/udev/rules.d/60-com.example.app.rules", "");
        add_file(tree, "bin/report", PROGRAM);
        fs::create_symlink("com.example.app.service", tree / "app/lib/systemd/system/com.example.app.link.service");

        auto collect_listed = [&](std::string_view integration) {
            return collect_integration(tree.path(), app_recipe(integration));
        };

        auto info = collect_listed(listed);
        REQUIRE(info);
        REQUIRE(info->systemd_unit_system.size() == 1);
        CHECK(info->systemd_unit_system[0].path == "/app/lib/systemd/system/com.example.app.service");

        // sorted by name
        REQUIRE(info->alias.size() == 2);
        CHECK(info->alias[0].name == "report");
        CHECK(info->alias[0].command == "/app/bin/report");
        CHECK(info->alias[1].name == "zz");
        CHECK(info->alias[1].command == "/app/bin/report");

        // a missing file
        CHECK_THROWS_AS(collect_listed("integration: {mime-package: [com.example.app.xml]}\n"), BuildError);
        // an empty file
        CHECK_THROWS_AS(collect_listed("integration: {udev-rule: [60-com.example.app.rules]}\n"), BuildError);
        // a link
        CHECK_THROWS_AS(
            collect_listed("integration: {systemd-unit-system: [com.example.app.link.service]}\n"), BuildError);
        // an alias without a program
        CHECK_THROWS_AS(collect_listed("integration: {alias: [missing]}\n"), BuildError);
    }
}

TEST_CASE("Host integration: renaming files", "[integration]")
{
    const std::string service = R"([Unit]
Requires=helper.service other.service
Wants=worker@one.service
[Service]
ExecStart=app helper.service
)";
    const std::string renamed_service = R"([Unit]
Requires=com.example.app.helper.service other.service
Wants=com.example.app.worker@one.service
[Service]
ExecStart=app helper.service
)";
    const std::string rename_keys = R"(rename:
  lib/udev/rules.d/60-steam-input.rules: 60-com.example.app.input.rules
  share/icons/hicolor/*/apps/steam.png: com.example.app.steam.png
  lib/systemd/system/helper.socket: com.example.app.helper.socket
  lib/systemd/system/helper.service: com.example.app.helper.service
  lib/systemd/system/worker@.service: com.example.app.worker@.service
integration:
  udev-rule: [60-com.example.app.input.rules]
  systemd-unit-system: [com.example.app.service, com.example.app.helper.socket]
)";
    const std::string units = "lib/systemd/system/";

    TempDir tree("integration-rename");
    add_file(tree, "bin/app", PROGRAM);
    add_file(tree, "lib/udev/rules.d/60-steam-input.rules", "KERNEL==\"uinput\", TAG+=\"uaccess\"\n");
    add_file(tree, "share/icons/hicolor/48x48/apps/steam.png", "png");
    add_file(tree, "share/icons/hicolor/64x64/apps/steam.png", "png");
    add_file(tree, "share/applications/com.example.app.desktop", ENTRY_HEAD + "Exec=app\nIcon=steam\n");
    add_file(tree, units + "helper.socket", "[Socket]\nListenStream=/run/helper\n");
    add_file(tree, units + "helper.service", "[Service]\nExecStart=app --helper\n");
    add_file(tree, units + "worker@.service", "[Service]\nExecStart=app --worker %i\n");
    add_file(tree, units + "com.example.app.service", service);

    // a link that leaves the tree
    write_file(tree / "outside.service", "Requires=helper.service\n");
    fs::create_symlink(tree / "outside.service", tree / "app" / units / "link.service");

    auto recipe = app_recipe(rename_keys);
    rename_files(tree / "app", recipe);

    // a link is not followed out of the tree
    CHECK(read_file(tree / "outside.service") == "Requires=helper.service\n");

    // files have their new names
    CHECK(fs::exists(tree / "app/lib/udev/rules.d/60-com.example.app.input.rules"));
    CHECK(fs::exists(tree / "app/share/icons/hicolor/48x48/apps/com.example.app.steam.png"));
    CHECK(fs::exists(tree / "app/share/icons/hicolor/64x64/apps/com.example.app.steam.png"));
    CHECK(!fs::exists(tree / "app/share/icons/hicolor/48x48/apps/steam.png"));

    // the units refer to the new names, but the commands run in the image and stay as they are
    CHECK(read_file(tree / "app" / units / "com.example.app.service") == renamed_service);
    CHECK_THAT(
        read_file(tree / "app/share/applications/com.example.app.desktop"),
        ContainsSubstring("\nIcon=com.example.app.steam\n"));

    // collected under the new names
    auto info = collect_integration(tree.path(), recipe);
    REQUIRE(info);
    CHECK(info->udev_rule.size() == 1);
    CHECK(info->icon.size() == 2);
    CHECK(info->systemd_unit_system.size() == 2);
}

TEST_CASE("Host integration: renames that can not work", "[integration]")
{
    const std::string socket_and_service = R"(rename:
  lib/systemd/system/helper.socket: com.example.app.a.socket
  lib/systemd/system/helper.service: com.example.app.b.service
)";
    const std::string icon_sizes = R"(rename:
  share/icons/hicolor/48x48/apps/steam.png: one.png
  share/icons/hicolor/64x64/apps/steam.png: two.png
)";

    TempDir tree("integration-rename-fails");
    add_file(tree, "lib/systemd/system/helper.socket", "[Socket]\n");
    add_file(tree, "lib/systemd/system/helper.service", "[Service]\n");
    add_file(tree, "share/icons/hicolor/48x48/apps/steam.png", "png");
    add_file(tree, "share/icons/hicolor/64x64/apps/steam.png", "png");
    add_file(tree, "share/icons/hicolor/64x64/apps/taken.png", "png");

    // check that renaming in a copy of the tree fails with the given message
    auto fails = [&](std::string_view rename, std::string_view message) {
        TempDir copy("integration-rename-copy");
        fs::copy(tree / "app", copy / "app", fs::copy_options::recursive);
        CHECK_THROWS_WITH(rename_files(copy / "app", app_recipe(rename)), ContainsSubstring(std::string(message)));
    };

    // no such file
    fails("rename: {share/missing.png: other.png}\n", "matches no file");
    // name is taken
    fails("rename: {share/icons/hicolor/64x64/apps/steam.png: taken.png}\n", "exists");
    // a socket starts the service of the same name
    fails("rename: {lib/systemd/system/helper.socket: com.example.app.helper.socket}\n", "renamed together");
    fails(socket_and_service, "the same new name");
    // an icon has one name in all of its sizes
    fails(icon_sizes, "more than one new name");
}

TEST_CASE("Host integration: manifest check", "[integration]")
{
    TempDir tree("integration-check");
    TempDir work("integration-check-work");
    add_file(tree, "bin/app", PROGRAM);
    add_file(tree, "share/applications/com.example.app.desktop");
    add_file(tree, "lib/systemd/system/com.example.app.service", "[Service]\nExecStart=/bin/sh\n");

    auto recipe = app_recipe("integration: {alias: [app]}\n");
    auto info = collect_integration(tree.path(), recipe);
    REQUIRE(info);

    // what was collected is fine with the rules of Manifold
    REQUIRE_NOTHROW(check_integration(tree.path(), finished_manifest(work, *info)));

    SECTION("a file may not change after it was recorded")
    {
        add_file(tree, "share/applications/com.example.app.desktop", ENTRY + "Comment=New\n");
        CHECK_THROWS_WITH(
            check_integration(tree.path(), finished_manifest(work, *info)), ContainsSubstring("changed after"));
    }

    SECTION("the main desktop entry has to run the command of the application")
    {
        add_file(tree, "bin/other", PROGRAM);
        add_file(tree, "share/applications/com.example.app.desktop", ENTRY_HEAD + "Exec=other\n");

        info = collect_integration(tree.path(), recipe);
        REQUIRE(info);
        CHECK_THROWS_AS(check_integration(tree.path(), finished_manifest(work, *info)), BuildError);
    }

    SECTION("a unit may not run a program of the host")
    {
        info = collect_integration(
            tree.path(), app_recipe("integration: {systemd-unit-system: [com.example.app.service]}\n"));
        REQUIRE(info);
        CHECK_THROWS_AS(check_integration(tree.path(), finished_manifest(work, *info)), BuildError);
    }

    SECTION("a manifest can not name an alias sudo")
    {
        info->alias[0].name = "sudo";
        CHECK_THROWS_AS(check_integration(tree.path(), finished_manifest(work, *info)), BuildError);
    }

    SECTION("a manifest can not name a desktop entry after another component")
    {
        add_file(tree, "share/applications/org.other.desktop");
        info->desktop_entry[0].path = "/app/share/applications/org.other.desktop";
        CHECK_THROWS_WITH(
            check_integration(tree.path(), finished_manifest(work, *info)),
            ContainsSubstring("has to start with com.example.app"));
    }
}

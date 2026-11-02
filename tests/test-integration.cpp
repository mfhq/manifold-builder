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
    REQUIRE(info->icon.size() == 4);
    CHECK(info->icon[0].path == "/app/share/icons/hicolor/32x32/mimetypes/com.example.app.png");
    CHECK(info->icon[1].path == "/app/share/icons/hicolor/48x48/apps/com.example.app.png");
    CHECK(info->icon[2].path == "/app/share/icons/hicolor/64x64@2/apps/com.example.app.png");
    CHECK(info->icon[3].path == "/app/share/icons/hicolor/scalable/apps/com.example.app.svg");

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
    add_file(tree, "bin/app", "#!/bin/sh\n");
    add_file(tree, "share/applications/com.example.app.desktop", ENTRY + "[Desktop Action a]\nIcon=theirs\n");
    add_file(tree, "share/icons/hicolor/48x48/apps/theirs.png", "png");
    add_file(tree, "share/icons/hicolor/48x48/apps/com.example.app.png", "png");
    fs::create_directories(tree / "app/share/icons/hicolor/64x64/apps");
    fs::create_symlink(
        "../../48x48/apps/com.example.app.png", tree / "app/share/icons/hicolor/64x64/apps/com.example.app.png");

    auto info = collect_integration(tree.path(), app_recipe());
    REQUIRE(info);
    REQUIRE(info->icon.size() == 1);
    CHECK(info->icon[0].path == "/app/share/icons/hicolor/48x48/apps/com.example.app.png");
}

TEST_CASE("a desktop entry has to run a program of the image", "[integration]")
{
    TempDir tree("integration-programs");
    add_file(tree, "bin/app", "#!/bin/sh\n");
    add_file(tree, "libexec/helper", "#!/bin/sh\n");
    const std::string entry = "[Desktop Entry]\nType=Application\nName=App\n";
    auto recipe = app_recipe("integration: {desktop-entry: [com.example.app.other.desktop]}\n");

    for (const auto *exec : {"Exec=app %U\n", "Exec=\"app\" %U\n", "Exec=/app/libexec/helper\n"}) {
        add_file(tree, "share/applications/com.example.app.other.desktop", entry + exec);
        CHECK_NOTHROW(collect_integration(tree.path(), recipe));
    }

    // a program of the base, a path outside of /app, a program in the wrong place, and none at all
    for (const auto *exec : {"Exec=vulkaninfo\n", "Exec=/usr/bin/app\n", "Exec=/app/bin/helper\n", "Exec=\n"}) {
        add_file(tree, "share/applications/com.example.app.other.desktop", entry + exec);
        CHECK_THROWS_AS(collect_integration(tree.path(), recipe), BuildError);
    }

    // the actions of an entry run programs too
    add_file(
        tree,
        "share/applications/com.example.app.other.desktop",
        entry + "Exec=app\n\n[Desktop Action new]\nName=New\nExec=missing --new\n");
    CHECK_THROWS_WITH(collect_integration(tree.path(), recipe), ContainsSubstring("runs 'missing'"));
}

TEST_CASE("a recipe renames files, and what refers to them", "[integration]")
{
    TempDir tree("integration-rename");
    const std::string units = "lib/systemd/system/";
    add_file(tree, "bin/app", "#!/bin/sh\n");
    add_file(tree, "lib/udev/rules.d/60-steam-input.rules", "KERNEL==\"uinput\", TAG+=\"uaccess\"\n");
    add_file(tree, "share/icons/hicolor/48x48/apps/steam.png", "png");
    add_file(tree, "share/icons/hicolor/64x64/apps/steam.png", "png");
    add_file(
        tree,
        "share/applications/com.example.app.desktop",
        "[Desktop Entry]\nType=Application\nName=A\nExec=app\nIcon=steam\n");
    add_file(tree, units + "helper.socket", "[Socket]\nListenStream=/run/helper\n");
    add_file(tree, units + "helper.service", "[Service]\nExecStart=app --helper\n");
    add_file(tree, units + "worker@.service", "[Service]\nExecStart=app --worker %i\n");
    add_file(
        tree,
        units + "com.example.app.service",
        "[Unit]\nRequires=helper.service other.service\nWants=worker@one.service\n"
        "[Service]\nExecStart=app helper.service\n");
    auto recipe = app_recipe(
        "rename:\n"
        "  lib/udev/rules.d/60-steam-input.rules: 60-com.example.app.input.rules\n"
        "  share/icons/hicolor/*/apps/steam.png: com.example.app.steam.png\n"
        "  lib/systemd/system/helper.socket: com.example.app.helper.socket\n"
        "  lib/systemd/system/helper.service: com.example.app.helper.service\n"
        "  lib/systemd/system/worker@.service: com.example.app.worker@.service\n"
        "integration:\n"
        "  udev-rule: [60-com.example.app.input.rules]\n"
        "  systemd-unit-system: [com.example.app.service, com.example.app.helper.socket]\n");

    // a link of the tree may point anywhere on the host, where the builder runs
    write_file(tree / "outside.service", "Requires=helper.service\n");
    fs::create_symlink(tree / "outside.service", tree / "app" / units / "link.service");

    rename_files(tree / "app", recipe);
    CHECK(read_file(tree / "outside.service") == "Requires=helper.service\n");
    CHECK(fs::exists(tree / "app/lib/udev/rules.d/60-com.example.app.input.rules"));
    CHECK(fs::exists(tree / "app/share/icons/hicolor/48x48/apps/com.example.app.steam.png"));
    CHECK(fs::exists(tree / "app/share/icons/hicolor/64x64/apps/com.example.app.steam.png"));
    CHECK_FALSE(fs::exists(tree / "app/share/icons/hicolor/48x48/apps/steam.png"));

    // the units refer to the new names, but the commands run in the image and stay as they are
    CHECK(
        read_file(tree / "app" / units / "com.example.app.service")
        == "[Unit]\nRequires=com.example.app.helper.service other.service\nWants=com.example.app.worker@one.service\n"
           "[Service]\nExecStart=app helper.service\n");
    CHECK_THAT(
        read_file(tree / "app/share/applications/com.example.app.desktop"),
        ContainsSubstring("\nIcon=com.example.app.steam\n"));

    auto info = collect_integration(tree.path(), recipe);
    REQUIRE(info);
    CHECK(info->udev_rule.size() == 1);
    CHECK(info->icon.size() == 2);
    CHECK(info->systemd_unit_system.size() == 2);
}

TEST_CASE("a rename that can not work fails the build", "[integration]")
{
    TempDir tree("integration-rename-fails");
    add_file(tree, "lib/systemd/system/helper.socket", "[Socket]\n");
    add_file(tree, "lib/systemd/system/helper.service", "[Service]\n");
    add_file(tree, "share/icons/hicolor/48x48/apps/steam.png", "png");
    add_file(tree, "share/icons/hicolor/64x64/apps/steam.png", "png");
    add_file(tree, "share/icons/hicolor/64x64/apps/taken.png", "png");

    auto fails = [&](std::string_view rename, std::string_view message) {
        TempDir copy("integration-rename-copy");
        fs::copy(tree / "app", copy / "app", fs::copy_options::recursive);
        CHECK_THROWS_WITH(
            rename_files(copy / "app", app_recipe(std::string(rename))), ContainsSubstring(std::string(message)));
    };
    fails("rename: {share/missing.png: other.png}\n", "matches no file");
    fails("rename: {share/icons/hicolor/64x64/apps/steam.png: taken.png}\n", "exists");
    // a socket starts the service of the same name, so they have to keep one name
    fails("rename: {lib/systemd/system/helper.socket: com.example.app.helper.socket}\n", "renamed together");
    fails(
        "rename:\n  lib/systemd/system/helper.socket: com.example.app.a.socket\n"
        "  lib/systemd/system/helper.service: com.example.app.b.service\n",
        "the same new name");
    // an icon has one name in all of its sizes
    fails(
        "rename:\n  share/icons/hicolor/48x48/apps/steam.png: one.png\n"
        "  share/icons/hicolor/64x64/apps/steam.png: two.png\n",
        "more than one new name");

    const std::string app(APP_RECIPE);
    CHECK(recipe_rejected(app + "rename: {../bin/sh: x}\n"));
    CHECK(recipe_rejected(app + "rename: {/etc/passwd: x}\n"));
    CHECK(recipe_rejected(app + "rename: {bin/app: sub/app}\n"));
    CHECK(recipe_rejected(app + "rename: {bin/app: ..}\n"));
    CHECK(recipe_rejected(std::string(test::PLATFORM_HEADER) + "rename: {bin/app: x}\n"));
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
    add_file(tree, "bin/other", "#!/bin/sh\n");
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

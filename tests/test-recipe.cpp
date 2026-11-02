/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "recipe.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace mfb;
using mfb::test::APP_HEADER;
using mfb::test::APP_RECIPE;
using mfb::test::CORE_BOOTSTRAP;
using mfb::test::CORE_HEADER;
using mfb::test::PLATFORM_HEADER;
using mfb::test::recipe_rejected;
using mfb::test::TempDir;
using mfb::test::tests_dir;

namespace
{

/**
 * Start of a platform recipe without the keys MetaInfo is generated from, which still needs a version.
 */
constexpr std::string_view BARE_PLATFORM = R"(type: platform
component: net.example.platform
base: net.example.core
)";

/**
 * Get the names of the parts, in order.
 */
std::vector<std::string> names(const std::vector<const Part *> &parts)
{
    std::vector<std::string> result;
    for (const auto *part : parts)
        result.push_back(part->name);
    return result;
}

} // namespace

TEST_CASE("Recipe: sample recipes", "[recipe]")
{
    const auto samples = tests_dir() / "samples";

    // the core has a bootstrap, packages and a part
    auto core = load_recipe(samples / "net.debian.core13.yaml");
    CHECK(core.is_core());
    CHECK(core.component == "net.debian.core13");
    CHECK(!core.sdk_only.empty());
    CHECK(core.packages.at("apt").size() > 10);
    REQUIRE(core.bootstrap);
    CHECK(core.bootstrap->suite == "trixie");
    REQUIRE(core.parts.size() == 1);
    CHECK(std::get<Part>(core.parts[0]).post_install.size() == 3);

    // the test app has a command, but no SDK
    auto test_app = load_recipe(samples / "com.example.mftestapp.yaml");
    CHECK(test_app.command == "mftestapp");
    CHECK(!test_app.generate_sdk);

    // KDE Frameworks are a platform
    CHECK(load_recipe(samples / "net.debian.core13.kf6.yaml").is_platform());
}

TEST_CASE("Recipe: hashes", "[recipe]")
{
    auto path = tests_dir() / "samples" / "com.example.mftestapp.yaml";

    auto recipe = load_recipe(path);
    auto reformatted = parse_recipe("# a comment\n" + read_file(path) + "\n\n", "inline", path.parent_path());

    // comments and blank lines do not count
    CHECK(reformatted.hash == recipe.hash);
    // another recipe hashes differently
    CHECK(parse_recipe(APP_RECIPE, "inline").hash != recipe.hash);
}

TEST_CASE("Recipe: validation", "[recipe]")
{
    const std::string app(APP_RECIPE);
    const std::string core(CORE_HEADER);
    const std::string bootstrap(CORE_BOOTSTRAP);
    const std::string commandless = R"(component: com.example.app
version: 1
base: net.example.core
base-version: 1
parts:
- name: hello
  buildsystem: meson
)";

    // unknown keys and invalid values
    CHECK(recipe_rejected(app + "unknown-key: 1\n"));
    CHECK(recipe_rejected(app + "kit-strategy: loose\n"));
    // versions have no epoch
    CHECK(recipe_rejected(std::string(BARE_PLATFORM) + "version: '1:2'\n"));

    // an app needs a command, which Manifold runs by its name
    CHECK(recipe_rejected(commandless));
    CHECK(recipe_rejected(commandless + "command: bin/app\n"));
    // the command ends up in generated files
    CHECK(recipe_rejected(commandless + "command: 'a b'\n"));

    // a core needs a bootstrap with suite and snapshot
    CHECK(recipe_rejected(core));
    CHECK(recipe_rejected(core + "bootstrap: {suite: trixie}\n"));

    // locales are plain names (UTF-8 is implied), listed once
    CHECK(recipe_rejected(core + bootstrap + "locales: [de_DE.UTF-8]\n"));
    CHECK(recipe_rejected(core + bootstrap + "locales: [de_DE, de_DE]\n"));
}

TEST_CASE("Recipe: keys per recipe type", "[recipe]")
{
    const std::string bootstrap(CORE_BOOTSTRAP);
    const std::string core = std::string(CORE_HEADER) + bootstrap;
    const std::string platform(PLATFORM_HEADER);
    const std::string app(APP_RECIPE);
    const std::string actions = "actions: [{name: tidy, run: 'true'}]\n";

    // valid as it is, but not with app keys
    auto takes_no_app_keys = [](const std::string &recipe) {
        INFO(recipe);
        CHECK(!recipe_rejected(recipe));
        CHECK(recipe_rejected(recipe + "command: app\n"));
        CHECK(recipe_rejected(recipe + "kit-strategy: strict\n"));
        CHECK(recipe_rejected(recipe + "generate-sdk: true\n"));
        CHECK(recipe_rejected(recipe + "run-on-sdk: true\n"));
        // the default does no harm
        CHECK(!recipe_rejected(recipe + "kit-strategy: roaming\n"));
    };
    takes_no_app_keys(core);
    takes_no_app_keys(platform);

    // a core has no base or base-version
    CHECK(recipe_rejected(core + "base: net.example.other\n"));
    CHECK(recipe_rejected(core + "base-version: 1\n"));

    // a platform has no base-version, but its parts go into its /usr like those of a core
    CHECK(recipe_rejected(platform + "base-version: 1\n"));
    CHECK(!recipe_rejected(platform + "parts: [{name: hello, buildsystem: meson}]\n"));

    // only cores have a bootstrap and actions
    CHECK(!recipe_rejected(core + actions));
    CHECK(recipe_rejected(platform + bootstrap));
    CHECK(recipe_rejected(platform + actions));
    CHECK(recipe_rejected(app + bootstrap));
    CHECK(recipe_rejected(app + actions));
}

TEST_CASE("Recipe: MetaInfo keys", "[recipe]")
{
    const std::string app(APP_RECIPE);
    TempDir dir("metainfo");
    write_file(dir / "com.example.app.metainfo.xml", "<component/>\n");

    auto plain = parse_recipe(app, "inline", dir.path());
    auto named = parse_recipe(app + "metainfo-file: com.example.app.metainfo.xml\n", "inline", dir.path());

    // metainfo-file is optional and counts for the hash
    CHECK(!plain.metainfo_file);
    CHECK(named.metainfo_file == "com.example.app.metainfo.xml");
    CHECK(named.hash != plain.hash);

    // it names an existing file next to the recipe
    CHECK(recipe_rejected(app + "metainfo-file: ''\n"));
    CHECK(recipe_rejected(app + "metainfo-file: missing.metainfo.xml\n", dir.path()));

    // an app ships its own MetaInfo
    for (std::string key : {"name", "summary", "description", "project-license", "homepage"})
        CHECK(recipe_rejected(app + key + ": x\n"));
    // cores and platforms only need the keys to generate MetaInfo
    CHECK(!recipe_rejected(std::string(BARE_PLATFORM) + "version: 1\n"));
}

TEST_CASE("Recipe: part validation", "[recipe][parts]")
{
    const std::string app(APP_HEADER);

    // the build's HOME and DESTDIR are next to the sources
    CHECK(recipe_rejected(app + "parts: [{name: home, buildsystem: simple, build-commands: ['true']}]\n"));
    CHECK(recipe_rejected(app + "parts: [{name: _install, buildsystem: simple, build-commands: ['true']}]\n"));
    // the name becomes a directory
    CHECK(recipe_rejected(app + "parts: [{name: a/b, buildsystem: meson}]\n"));
    // used twice
    CHECK(recipe_rejected(app + "parts: [{name: hello, buildsystem: meson}, {name: hello, buildsystem: meson}]\n"));

    // unknown build system
    CHECK(recipe_rejected(app + "parts: [{name: hello, buildsystem: qmake}]\n"));
    // nothing to build with
    CHECK(recipe_rejected(app + "parts: [{name: hello, buildsystem: simple}]\n"));
    // the subdir stays inside the sources
    CHECK(recipe_rejected(app + "parts: [{name: hello, buildsystem: meson, subdir: ../x}]\n"));
    // post-install must be a list of commands, like in Flatpak
    CHECK(recipe_rejected(app + "parts: [{name: hello, buildsystem: meson, post-install: 'true'}]\n"));

    // wrap the source into an app with one part
    auto source_rejected = [&](std::string source) {
        return recipe_rejected(app + "parts: [{name: hello, buildsystem: meson, sources: [" + source + "]}]\n");
    };

    // unknown type
    CHECK(source_rejected("{type: svn, url: u}"));
    // a required key is missing
    CHECK(source_rejected("{type: git}"));
    CHECK(source_rejected("{type: dir}"));
    // keys the type does not take
    CHECK(source_rejected("{type: git, url: u, sha256: x}"));
    CHECK(source_rejected("{type: patch, url: u}"));
    // downloads need a checksum
    CHECK(source_rejected("{type: archive, url: 'https://x/a.tar.xz'}"));
    CHECK(source_rejected("{type: file, url: u}"));
    // either a url or a path
    CHECK(source_rejected("{type: archive, url: u, path: p, sha256: x}"));
    // paths stay inside the sources
    CHECK(source_rejected("{type: file, path: p, dest-filename: a/b}"));
    CHECK(source_rejected("{type: dir, path: d, dest: ../outside}"));

    // valid sources
    CHECK(!source_rejected("{type: archive, url: 'https://x/a.tar.xz', sha256: x, strip-components: 2}"));
    CHECK(!source_rejected("{type: archive, path: a.zip}"));
    CHECK(!source_rejected("{type: file, path: p, dest-filename: q}"));
    CHECK(!source_rejected("{type: dir, path: d, dest: sub/dir}"));
    CHECK(!source_rejected("{type: patch, path: fix.patch, strip-components: 0}"));
}

TEST_CASE("Recipe: included and nested parts", "[recipe][parts]")
{
    const std::string dep_yaml = "name: dep\nbuildsystem: cmake\n";

    TempDir dir("includes");
    fs::create_directories(dir / "parts");
    write_file(dir / "parts/dep.yaml", dep_yaml);
    write_file(dir / "parts/lib.yaml", R"(name: lib
buildsystem: meson
sources: [{type: dir, path: lib-src}]
parts: [dep.yaml]
)");
    write_file(dir / "app.yaml", std::string(APP_HEADER) + R"(parts:
- parts/lib.yaml
- name: app
  buildsystem: meson
  parts:
  - {name: inner, buildsystem: simple, build-commands: [make], only-arches: [aarch64]}
  - {name: other, buildsystem: simple, build-commands: [make], skip-arches: [aarch64]}
)");

    auto recipe = load_recipe(dir / "app.yaml");

    // sources of an included part are relative to its own file
    REQUIRE(recipe.parts.size() == 2);
    const auto &lib = std::get<Part>(recipe.parts[0]);
    CHECK(lib.dir == fs::canonical(dir / "parts"));
    CHECK(std::get<Part>(lib.parts[0]).dir == fs::canonical(dir / "parts"));
    CHECK(std::get<Part>(recipe.parts[1]).dir == fs::absolute(dir.path()));

    // nested parts are built first, and arch filters drop parts
    CHECK(names(parts_to_build(recipe, "x86_64")) == std::vector<std::string>{"dep", "lib", "other", "app"});
    CHECK(names(parts_to_build(recipe, "aarch64")) == std::vector<std::string>{"dep", "lib", "inner", "app"});

    SECTION("the hash covers the included files")
    {
        write_file(dir / "parts/dep.yaml", dep_yaml + "config-opts: [-DX=1]\n");

        CHECK(load_recipe(dir / "app.yaml").hash != recipe.hash);
    }

    SECTION("part names must be unique across files")
    {
        write_file(dir / "parts/dep.yaml", "name: lib\nbuildsystem: cmake\n");

        CHECK_THROWS_AS(load_recipe(dir / "app.yaml"), BuildError);
    }

    SECTION("include cycles are refused")
    {
        write_file(dir / "parts/dep.yaml", dep_yaml + "parts: [lib.yaml]\n");

        CHECK_THROWS_AS(load_recipe(dir / "app.yaml"), BuildError);
    }
}

TEST_CASE("Recipe: build-options", "[recipe][parts]")
{
    const std::string yaml = std::string(APP_HEADER) + R"(build-options:
  cflags: -O2
  env: {A: global, B: global}
  prepend-path: /global/bin
  config-opts: [-Dglobal=true]
  arch:
    x86_64: {cflags: -march=x86-64-v2, env: {B: global-x86}}
parts:
- name: hello
  buildsystem: meson
  build-options:
    cflags: -g
    ldflags: -Wl,--as-needed
    env: {A: part}
    config-opts: [-Dpart=true]
    arch: {aarch64: {cflags: -mbranch-protection=standard}}
)";

    auto recipe = parse_recipe(yaml, "inline");
    const auto &part = std::get<Part>(recipe.parts[0]);
    auto x86 = merge_build_settings(recipe, part, "x86_64");
    auto arm = merge_build_settings(recipe, part, "aarch64");

    // flags append: global, global arch, part
    CHECK(x86.cflags == "-O2 -march=x86-64-v2 -g");
    CHECK(x86.ldflags == "-Wl,--as-needed");
    CHECK(x86.config_opts == std::vector<std::string>{"-Dglobal=true", "-Dpart=true"});

    // the most specific value of a variable wins
    CHECK(x86.env.at("A") == "part");
    CHECK(x86.env.at("B") == "global-x86");
    CHECK(x86.prepend_path == "/global/bin");

    // the part's arch options only apply on aarch64
    CHECK(arm.cflags == "-O2 -g -mbranch-protection=standard");
    CHECK(arm.env.at("B") == "global");
}

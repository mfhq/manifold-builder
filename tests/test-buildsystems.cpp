/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "buildsystems/buildsystem.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace mfb;

namespace
{

const InstallTarget APP = {"/app", "lib"};                   ///< Install target of an app
const InstallTarget CORE = {"/usr", "lib/x86_64-linux-gnu"}; ///< Install target of a core part

/**
 * Check whether a text contains a string.
 */
bool contains(const std::string &text, std::string_view what)
{
    return text.find(what) != std::string::npos;
}

/**
 * Generate the build script of a part, by default without any source files.
 */
std::string script_for(
    const Part &part,
    const InstallTarget &target,
    BuildSettings settings = {},
    const fs::path &source_dir = "/nonexistent")
{
    return part_build_script(part, target, settings, source_dir);
}

/**
 * Write an empty shell script that can be run.
 */
void write_executable(const fs::path &path)
{
    write_file(path, "#!/bin/sh\n");
    fs::permissions(path, fs::perms::owner_exec, fs::perm_options::add);
}

} // namespace

TEST_CASE("BuildSystem: meson", "[buildsystems]")
{
    const Part part{.name = "hello", .buildsystem = "meson", .config_opts = {"-Dfoo=bar"}, .make_args = {"-v"}};
    const auto script = script_for(part, APP);

    CHECK(contains(script, "meson setup _mfold-build --prefix=/app --libdir=lib --buildtype=release -Dfoo=bar"));
    CHECK(contains(script, "ninja -C _mfold-build -j\"$MANIFOLD_BUILDER_N_JOBS\" -v\n"));
    CHECK(contains(script, "ninja -C _mfold-build install\n"));

    // a core part keeps its libraries in the multiarch directory
    CHECK(contains(script_for(part, CORE), "--prefix=/usr --libdir=lib/x86_64-linux-gnu"));
}

TEST_CASE("BuildSystem: cmake", "[buildsystems]")
{
    Part part{.name = "hello", .buildsystem = "cmake", .config_opts = {"-DFOO=ON"}, .make_install_args = {"-v"}};

    // ninja is the default generator
    auto script = script_for(part, APP);
    CHECK(
        contains(script, "cmake -S . -B _mfold-build -G Ninja -DCMAKE_INSTALL_PREFIX=/app -DCMAKE_INSTALL_LIBDIR=lib"));
    CHECK(contains(script, "-DCMAKE_BUILD_TYPE=Release -DFOO=ON\n"));
    CHECK(contains(script, "ninja -C _mfold-build -j\"$MANIFOLD_BUILDER_N_JOBS\"\n"));
    CHECK(contains(script, "ninja -C _mfold-build install -v\n"));

    // cmake-make uses Unix Makefiles
    part.buildsystem = "cmake-make";
    script = script_for(part, APP);
    CHECK(contains(script, "-G 'Unix Makefiles'"));
    CHECK(contains(script, "make -C _mfold-build -j\"$MANIFOLD_BUILDER_N_JOBS\"\n"));
    CHECK(contains(script, "make -C _mfold-build install -v\n"));
}

TEST_CASE("BuildSystem: autotools", "[buildsystems]")
{
    const Part part{.name = "hello", .buildsystem = "autotools", .config_opts = {"--disable-foo"}};

    // autoreconf runs if the project has no configure script
    const auto script = script_for(part, APP);
    CHECK(script.starts_with("autoreconf -fi\n./configure --prefix=/app --libdir=/app/lib --disable-foo\n"));
    CHECK(contains(script, "make -j\"$MANIFOLD_BUILDER_N_JOBS\"\n"));
    CHECK(contains(script, "make install\n"));
    // with a /usr prefix, configuration goes to /etc
    CHECK(contains(
        script_for(part, CORE),
        "./configure --prefix=/usr --libdir=/usr/lib/x86_64-linux-gnu --sysconfdir=/etc --localstatedir=/var"));

    // of the project's scripts that generate configure, autogen.sh goes first
    test::TempDir sources("autotools");
    write_executable(sources / "bootstrap");
    write_executable(sources / "autogen.sh");
    CHECK(script_for(part, APP, {}, sources.path()).starts_with("NOCONFIGURE=1 ./autogen.sh\n./configure "));
    // nothing has to be generated
    write_executable(sources / "configure");
    CHECK(script_for(part, APP, {}, sources.path()).starts_with("./configure "));
}

TEST_CASE("BuildSystem: simple", "[buildsystems]")
{
    Part part{.name = "foo", .buildsystem = "simple", .build_commands = {"make", "make install PREFIX=$MANIFOLD_DEST"}};
    CHECK(script_for(part, APP) == "make\nmake install PREFIX=$MANIFOLD_DEST\n");
}

TEST_CASE("BuildSystem: part script assembly", "[buildsystems]")
{
    const Part part{
        .name = "hello",
        .buildsystem = "meson",
        .subdir = "sub dir",
        .config_opts = {"-Dpart=true"},
        .build_commands = {"echo between"},
        .post_install = {"echo after"},
    };

    const auto script = script_for(part, APP, {.config_opts = {"-Dglobal=true"}});

    // the caller runs the script in the subdir
    CHECK(!contains(script, "sub dir"));

    // build-options come first, so that the part's own config-opts win
    CHECK(contains(script, "-Dglobal=true -Dpart=true"));

    // build-commands run between building and installing, as in flatpak-builder
    CHECK(script.find("ninja -C _mfold-build -j") < script.find("echo between"));
    CHECK(script.find("echo between") < script.find("ninja -C _mfold-build install"));

    // the caller runs post-install
    CHECK(!contains(script, "echo after"));
}

TEST_CASE("BuildSystem: unknown names", "[buildsystems]")
{
    CHECK(!build_system_known("qmake"));
    CHECK_THROWS_AS(build_system_for("qmake"), BuildError);
}

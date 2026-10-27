/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "cores/core-system.hpp"
#include "cores/debian/debian.hpp"
#include "cores/debian/dpkg.hpp"
#include "sandbox.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <manifold.h>

using namespace mfb;
using mfb::test::APP_RECIPE;
using mfb::test::CORE_BOOTSTRAP;
using mfb::test::CORE_HEADER;

namespace
{

/**
 * Create a Debian 13 system for the given dpkg architecture, without scripts.
 */
DebianSystem trixie(std::string architecture = "amd64")
{
    return DebianSystem("/nonexistent", Bootstrap{.suite = "trixie", .architecture = std::move(architecture)}, 0);
}

} // namespace

TEST_CASE("Debian: architecture names", "[debian]")
{
    // dpkg and Manifold names
    CHECK(manifold_architecture("amd64") == "x86_64");
    CHECK(manifold_architecture("arm64") == "aarch64");
    CHECK(dpkg_architecture("x86_64") == "amd64");

    // 32-bit ARM is named by ABI, since armhf and armel share the CPU part of the tuple
    CHECK(manifold_architecture("armhf") == "armhf");
    CHECK(dpkg_architecture("armel") == "armel");

    // what a DebianSystem reports
    CHECK(trixie().multiarch() == "x86_64-linux-gnu");
    CHECK(trixie("armhf").multiarch() == "arm-linux-gnueabihf");

    // without an architecture, we build for the host
    CHECK(trixie("").architecture() == mf_get_host_architecture());
}

TEST_CASE("Debian: bootstrap section", "[debian]")
{
    CHECK(DebianSystem::check_bootstrap({.suite = "trixie", .snapshot = "2026-01-04T00:00:00Z"}).empty());

    // suite and snapshot are required
    CHECK(!DebianSystem::check_bootstrap({.suite = "", .snapshot = "x"}).empty());
    CHECK(!DebianSystem::check_bootstrap({.suite = "trixie"}).empty());

    // the architecture is a dpkg name
    CHECK(!DebianSystem::check_bootstrap({.suite = "trixie", .architecture = "x86_64", .snapshot = "x"}).empty());

    // only Debian for now
    CHECK(!check_bootstrap({.distribution = "fedora", .suite = "43", .snapshot = "x"}).empty());
}

TEST_CASE("Debian: generated commands", "[debian]")
{
    auto debian = trixie();

    // the slim configuration keeps the catalogs of the locales
    Script slim;
    debian.slimify(slim, "/tree", {"de_DE", "pt_BR"});
    CHECK(slim.text().find("path-include /usr/share/locale/de/*") != std::string::npos);
    CHECK(slim.text().find("path-include /usr/share/locale/pt_BR/*") != std::string::npos);
    CHECK(slim.text().find("! -name de_DE") != std::string::npos);
    CHECK(slim.text().find("path-exclude /usr/share/man/*") != std::string::npos);

    // commands in a tree run without questions
    Script command;
    debian.run_in_tree(command, "/tree", "apt-get install -y foo");
    CHECK(command.text().find("DEBIAN_FRONTEND=noninteractive") != std::string::npos);
    CHECK(command.text().find("umask 022") != std::string::npos);

    // packages install from the shared cache without removing anything
    Script install;
    debian.install(install, "/tree", {"foo"});
    CHECK(install.text().find("--no-remove") != std::string::npos);
    CHECK(install.text().find("Dir::Cache::Archives=/" + debian.package_cache_dir()) != std::string::npos);
    // the shared package cache belongs to us, not to apt's sandbox user
    CHECK(install.text().find("APT::Sandbox::User=root") != std::string::npos);
    // debuerreotype empties apt's own archives directory after every dpkg run
    CHECK(debian.package_cache_dir() != "var/cache/apt/archives");
}

TEST_CASE("Debian: dpkg status files", "[debian]")
{
    auto packages = parse_dpkg_status(R"(Package: zlib1g
Status: install ok installed
Version: 1:1.3.1-1

Package: removed
Status: deinstall ok config-files
Version: 1.0

Package: apt
Status: install ok installed
Version: 3.0.3
)");

    // packages that are not installed are left out, the rest is sorted
    REQUIRE(packages.size() == 2);
    CHECK(packages.begin()->first == "apt");
    CHECK(packages["zlib1g"] == "1:1.3.1-1");
}

TEST_CASE("Debian: dpkg-shlibdeps output", "[debian]")
{
    const std::string depends =
        "shlibs:Depends=libc6 (>= 2.34), libfoo1 (>= 1.2) | libfoo1t64, libbar2:any, libc6 (>= 2.38)\n";

    // each package once, the first of its alternatives, without versions
    CHECK(parse_shlibs_depends(depends) == std::vector<std::string>{"libbar2", "libc6", "libfoo1"});

    // an app without any ELF file
    CHECK(parse_shlibs_depends("").empty());
}

TEST_CASE("Debian: packages of a recipe", "[debian]")
{
    auto debian = trixie();
    auto app = parse_recipe(
        std::string(APP_RECIPE) + "packages: {apt: [libfoo1, fontconfig]}\npackages-sdk: {apt: [libfoo-dev, meson]}\n",
        "inline");

    // the runtime gets packages, the SDK gets packages-sdk as well
    CHECK(debian.packages_request_runtime(app) == std::vector<std::string>{"fontconfig", "libfoo1"});
    CHECK(debian.packages_request_sdk(app) == std::vector<std::string>{"fontconfig", "libfoo-dev", "libfoo1", "meson"});

    // locales bring the packages that generate them
    auto core = parse_recipe(
        std::string(CORE_HEADER) + std::string(CORE_BOOTSTRAP) + "locales: [de_DE]\npackages: {apt: [fontconfig]}\n",
        "inline");
    CHECK(debian.packages_request_runtime(core) == std::vector<std::string>{"fontconfig", "libc-l10n", "locales"});

    // development packages, which the builder only warns about in a runtime
    CHECK(debian.is_development_package("libfoo-dev"));
    CHECK(debian.is_development_package("libc6-dbg"));
    CHECK(debian.is_development_package("libfoo1-dbgsym"));
    CHECK(!debian.is_development_package("fontconfig"));

    // packages of another package system are refused
    auto fedora = parse_recipe(std::string(APP_RECIPE) + "packages: {dnf: [foo-devel]}\n", "inline");
    CHECK_NOTHROW(debian.check_packages(app));
    CHECK_THROWS_AS(debian.check_packages(fedora), BuildError);
}

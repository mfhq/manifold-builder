/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "base-store.hpp"
#include "checksum.hpp"
#include "manifest.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace mfb;
using mfb::test::TempDir;

namespace
{

/**
 * References to a layer and to its SDK.
 */
struct LayerRefs {
    BaseRef runtime; ///< reference to a layer
    BaseRef sdk;     ///< reference to its SDK
};

/**
 * Write the manifests and fake images of a layer and its SDK to `dir`, with `build` telling rebuilds apart.
 */
LayerRefs write_layer(
    const fs::path &dir,
    const std::string &component,
    const std::string &kind,
    const std::string &version,
    int64_t revision,
    const std::string &build = "",
    const std::optional<LayerRefs> &base = std::nullopt)
{
    LayerRefs refs;
    for (bool sdk : {false, true}) {
        auto manifest = test::fake_manifest(component + (sdk ? ".sdk" : ""), kind);
        manifest.version = version;
        manifest.revision = revision;
        manifest.sdk = sdk;
        manifest.content_hash = checksum_data_hex(manifest.component + version + build, ChecksumKind::Sha256);

        // an SDK is layered on its own runtime
        if (sdk)
            manifest.base = refs.runtime;
        else if (base)
            manifest.base = base->runtime;
        // the SDK of a platform names the SDK of its core
        if (sdk && base)
            manifest.base_sdk = base->sdk;

        auto stem = std::format("{}_{}-{}_x86-64", manifest.component, version, revision);
        manifest.image = test::fake_image(dir, stem + ".raw");
        write_manifest(manifest, dir / (stem + ".manifold.json"));

        (sdk ? refs.sdk : refs.runtime) = manifest.as_base();
    }
    return refs;
}

/**
 * Install a layer from `dir` into a store laid out like Manifold's, on top of `base` if one is given.
 */
fs::path install_layer(
    const fs::path &dir,
    const fs::path &store,
    const std::string &component,
    const std::string &full_version,
    const std::string &base = "")
{
    auto stem = std::format("{}_{}_x86-64", component, full_version);
    auto manifest = manifest_from_json(read_file(dir / (stem + ".manifold.json")), "");
    auto image = store / "images/by-verity" / (manifest.image->verity_root_hash + ".raw");
    auto version_dir = store / "runtimes" / component / full_version;

    fs::create_directories(image.parent_path());
    fs::create_directories(version_dir);
    fs::copy_file(dir / (stem + ".raw"), image);
    fs::create_symlink(fs::path("../../../images/by-verity") / image.filename(), version_dir / "runtime.raw");
    if (!base.empty())
        fs::create_symlink("../../../runtimes/" + base, version_dir / "base");
    write_manifest(manifest, version_dir / "manifest.json");
    fs::create_symlink(full_version, store / "runtimes" / component / "current");

    return image;
}

} // namespace

TEST_CASE("Store: finding the base of a build", "[store]")
{
    TempDir tmp("core-lookup");
    const auto &dir = tmp.path();

    // a directory of extracted bundles, with several versions and revisions of a core
    write_layer(dir, "net.example.core", "core", "12.1a", 1);
    auto core_refs = write_layer(dir, "net.example.core", "core", "12.2", 1);
    write_layer(dir, "net.example.core", "core", "12.10", 1);
    write_layer(dir, "net.example.core", "core", "12.10", 2);
    // a platform built on core 12.2
    write_layer(dir, "net.example.platform", "platform", "20260117.0900", 0, "", core_refs);
    // a newer build of the core version the platform was built on
    write_layer(dir, "net.example.core", "core", "12.2", 2, "rebuilt");

    // the newest version satisfying the minimum wins, with its newest revision: 12.10 is newer than 12.2
    auto core = find_stack(dir, "net.example.core", "12.2").core;
    CHECK(core.runtime.version == "12.10");
    CHECK(core.runtime_image.filename() == "net.example.core_12.10-2_x86-64.raw");
    CHECK(core.sdk_image.filename() == "net.example.core.sdk_12.10-2_x86-64.raw");
    // no version is new enough
    CHECK_THROWS_AS(find_stack(dir, "net.example.core", "14"), BuildError);

    // a platform uses the exact core it was built on
    auto on_platform = find_stack(dir, "net.example.platform", "");
    REQUIRE(on_platform.platform);
    CHECK(on_platform.base().runtime.component == "net.example.platform");
    // not the rebuilt 12.2-2, and not the newer 12.10
    CHECK(on_platform.core.runtime.version == "12.2");
    CHECK(on_platform.core.runtime.revision == 1);
    // and the SDK of that build, not of the rebuild
    CHECK(on_platform.core.sdk.revision == 1);

    // an installed store is read like Manifold does
    auto store = dir / "store";
    auto core_image = install_layer(dir, store, "net.example.core", "12.2-1");
    auto sdk_image = install_layer(dir, store, "net.example.core.sdk", "12.2-1");
    auto installed = find_stack(store, "net.example.core", "12.1").core;
    CHECK(installed.runtime.version == "12.2");
    CHECK(installed.runtime_image == core_image);
    // the SDK of the same version, found through its own directory
    CHECK(installed.sdk_image == sdk_image);
    // the installed version is too old
    CHECK_THROWS_AS(find_stack(store, "net.example.core", "12.3"), BuildError);

    // an installed platform uses the core its base link points to
    install_layer(dir, store, "net.example.platform", "20260117.0900-0", "net.example.core/12.2-1");
    install_layer(dir, store, "net.example.platform.sdk", "20260117.0900-0", "net.example.platform/20260117.0900-0");
    auto installed_platform = find_stack(store, "net.example.platform", "");
    REQUIRE(installed_platform.platform);
    CHECK(installed_platform.base().runtime.component == "net.example.platform");
    CHECK(installed_platform.core.runtime_image == core_image);
    CHECK(installed_platform.core.sdk_image == sdk_image);
}

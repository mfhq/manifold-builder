/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "base-store.hpp"
#include "identity.hpp"
#include "manifest.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <fstream>

using namespace mfb;
using mfb::test::TempDir;

namespace
{

/**
 * Write a fake image file to `dir` and describe it with made-up hashes that still look valid, so Manifold accepts it.
 */
ImageFile image_of(const fs::path &dir, const std::string &file)
{
    std::ofstream(dir / file) << "not really an image";
    return ImageFile{
        .file = file,
        .size = fs::file_size(dir / file),
        .blake3 = checksum_file_hex(dir / file, ChecksumKind::Blake3),
        .sha256 = checksum_file_hex(dir / file, ChecksumKind::Sha256),
        .verity_root_hash = checksum_data_hex("verity " + file, ChecksumKind::Sha256),
        .data_offset = 1048576,
        .hash_offset = 2097152,
    };
}

/**
 * Write the manifest and fake image of a core or its SDK to `dir`, with `build` telling rebuilds apart.
 */
void write_core(
    const fs::path &dir,
    const std::string &version,
    bool sdk,
    int64_t revision = 1,
    const std::string &build = "")
{
    auto manifest = test::fake_manifest(sdk ? "net.example.core.sdk" : "net.example.core", "core");
    manifest.version = version;
    manifest.revision = revision;
    manifest.architecture = "amd64";
    manifest.sdk = sdk;
    manifest.identity = checksum_data_hex(sdk ? "sdk-identity" : "core-identity", ChecksumKind::Sha256);
    manifest.content_hash = checksum_data_hex(version + build + (sdk ? "-sdk" : ""), ChecksumKind::Sha256);

    // an SDK is layered on its own runtime
    if (sdk)
        manifest.base = BaseRef{
            .component = "net.example.core",
            .version = version,
            .identity = checksum_data_hex("core-identity", ChecksumKind::Sha256),
            .content_hash = checksum_data_hex(version + build, ChecksumKind::Sha256),
        };

    auto stem = std::format("{}_{}-{}_amd64", manifest.component, version, revision);
    manifest.image = image_of(dir, stem + ".raw");
    write_manifest(manifest, dir / (stem + ".manifold.json"));
}

/**
 * Write the manifest and fake image of a platform built on core 12.2, or of its SDK, to `dir`.
 */
void write_platform(const fs::path &dir, bool sdk)
{
    auto manifest = test::fake_manifest(sdk ? "net.example.platform.sdk" : "net.example.platform", "platform");
    manifest.version = "20260117.0900";
    manifest.revision = 0;
    manifest.sdk = sdk;
    manifest.identity = checksum_data_hex("platform-identity", ChecksumKind::Sha256);
    manifest.content_hash = checksum_data_hex(sdk ? "p-sdk" : "p", ChecksumKind::Sha256);

    // an SDK is layered on its own runtime
    if (sdk)
        manifest.base = BaseRef{
            .component = "net.example.platform",
            .version = manifest.version,
            .identity = checksum_data_hex("platform-identity", ChecksumKind::Sha256),
            .content_hash = checksum_data_hex("p", ChecksumKind::Sha256),
        };
    else
        manifest.base = BaseRef{
            .component = "net.example.core",
            .version = "12.2",
            .identity = checksum_data_hex("core-identity", ChecksumKind::Sha256),
            .content_hash = checksum_data_hex("12.2", ChecksumKind::Sha256),
        };

    auto stem = std::format("{}_{}-0_x86-64", manifest.component, manifest.version);
    manifest.image = image_of(dir, stem + ".raw");
    write_manifest(manifest, dir / (stem + ".manifold.json"));
}

/**
 * Install a layer from `dir` into a store laid out like Manifold's, on top of `base` if one is given.
 */
fs::path install_layer(
    const fs::path &dir,
    const fs::path &store,
    const std::string &component,
    const std::string &full_version,
    const std::string &architecture,
    const std::string &base = "")
{
    auto stem = std::format("{}_{}_{}", component, full_version, architecture);
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
    for (auto version : {"12.1a", "12.2", "12.10"}) {
        write_core(dir, version, false);
        write_core(dir, version, true);
    }
    write_core(dir, "12.10", false, 2);
    write_core(dir, "12.10", true, 2);

    // the newest version satisfying the minimum wins, with its newest revision: 12.10 is newer than 12.2
    auto core = find_stack(dir, "net.example.core", "12.2").core;
    CHECK(core.runtime.version == "12.10");
    CHECK(core.runtime_image.filename() == "net.example.core_12.10-2_amd64.raw");
    CHECK(core.sdk_image.filename() == "net.example.core.sdk_12.10-2_amd64.raw");
    // no version is new enough
    CHECK_THROWS_AS(find_stack(dir, "net.example.core", "14"), BuildError);

    // a platform uses the exact core it was built on
    write_platform(dir, false);
    write_platform(dir, true);
    // a newer build of the core version the platform was built on
    write_core(dir, "12.2", false, 2, "rebuilt");
    write_core(dir, "12.2", true, 2, "rebuilt");
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
    auto core_image = install_layer(dir, store, "net.example.core", "12.2-1", "amd64");
    auto sdk_image = install_layer(dir, store, "net.example.core.sdk", "12.2-1", "amd64");
    auto installed = find_stack(store, "net.example.core", "12.1").core;
    CHECK(installed.runtime.version == "12.2");
    CHECK(installed.runtime_image == core_image);
    // the SDK of the same version, found through its own directory
    CHECK(installed.sdk_image == sdk_image);
    // the installed version is too old
    CHECK_THROWS_AS(find_stack(store, "net.example.core", "12.3"), BuildError);

    // an installed platform uses the core its base link points to
    install_layer(dir, store, "net.example.platform", "20260117.0900-0", "x86-64", "net.example.core/12.2-1");
    install_layer(
        dir, store, "net.example.platform.sdk", "20260117.0900-0", "x86-64", "net.example.platform/20260117.0900-0");
    auto installed_platform = find_stack(store, "net.example.platform", "");
    REQUIRE(installed_platform.platform);
    CHECK(installed_platform.base().runtime.component == "net.example.platform");
    CHECK(installed_platform.core.runtime_image == core_image);
    CHECK(installed_platform.core.sdk_image == sdk_image);
}

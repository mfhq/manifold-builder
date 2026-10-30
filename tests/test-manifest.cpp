/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "appstream.hpp"
#include "checksum.hpp"
#include "identity.hpp"
#include "image.hpp"
#include "manifest.hpp"
#include "testshared.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace mfb;
using Catch::Matchers::ContainsSubstring;
using mfb::test::TempDir;

namespace
{

// b3sum of the empty input
constexpr std::string_view EMPTY_BLAKE3 = "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262";

/**
 * Check that MetaInfo validates, and show what the validator found if not.
 */
void check_valid(const std::string &xml)
{
    auto problems = validate_metainfo(xml);
    for (const auto &problem : problems)
        UNSCOPED_INFO("validator: " << problem.text);
    CHECK(problems.empty());
}

/**
 * Write the tree of a core with its MetaInfo and icon, and the MetaInfo of one of its packages.
 */
void write_core_tree(const fs::path &tree)
{
    fs::create_directories(tree / "usr/share/metainfo");
    write_file(
        tree / "usr/share/metainfo/net.example.core.metainfo.xml",
        R"(<?xml version="1.0" encoding="UTF-8"?>
<component type="runtime">
  <id>net.example.core</id>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>MIT</project_license>
  <name>Example Core</name>
  <name xml:lang="de">Beispielkern</name>
  <summary>Base layer for examples</summary>
  <url type="homepage">https://example.org/core</url>
  <icon type="stock">net.example.core</icon>
</component>
)");
    // a package of the core with MetaInfo of its own
    write_file(
        tree / "usr/share/metainfo/org.example.tool.metainfo.xml",
        R"(<?xml version="1.0" encoding="UTF-8"?>
<component type="console-application">
  <id>org.example.tool</id>
  <metadata_license>CC0-1.0</metadata_license>
  <name>Example Tool</name>
  <summary>A tool in the core</summary>
</component>
)");

    auto icon = tree / "usr/share/icons/hicolor/scalable/apps/net.example.core.svg";
    fs::create_directories(icon.parent_path());
    write_file(
        icon,
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="#3584e4"/></svg>)");
}

} // namespace

TEST_CASE("Checksum: files and data", "[hashes]")
{
    TempDir dir("hashes");

    // the empty input has its known digests
    write_file(dir / "empty", "");
    CHECK(
        checksum_file_hex(dir / "empty", ChecksumKind::Sha256)
        == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(checksum_file_hex(dir / "empty", ChecksumKind::Blake3) == EMPTY_BLAKE3);
    CHECK(checksum_data_hex("", ChecksumKind::Blake3) == EMPTY_BLAKE3);

    // a large file is hashed in chunks, with both digests at once
    const std::string data(3 << 20, 'x');
    write_file(dir / "large", data);
    auto both = checksum_file(dir / "large");
    CHECK(both.blake3 == checksum_file_hex(dir / "large", ChecksumKind::Blake3));
    CHECK(both.sha256 == checksum_file_hex(dir / "large", ChecksumKind::Sha256));
    CHECK(both.sha256 == checksum_data_hex(data, ChecksumKind::Sha256));

    // a directory is refused
    CHECK_THROWS_AS(checksum_file(dir.path()), BuildError);
}

TEST_CASE("Image: UUID and systemd architecture names", "[image]")
{
    // the first 128 bits of the hash, with version and variant set
    CHECK(image_uuid(EMPTY_BLAKE3) == "af1349b9-f5f9-81a6-a040-4dea36dcc949");

    CHECK(repart_architecture("x86_64") == "x86-64");
    CHECK(repart_architecture("aarch64") == "arm64");

    // systemd does not tell the ABIs of 32-bit ARM apart
    CHECK(repart_architecture("armhf") == "arm");
}

TEST_CASE("Identity: hashes of a package set", "[hashes]")
{
    const std::string core = "net.debian.core13";
    InstalledPackages packages = {{"apt", "3.0.3"}, {"zlib1g", "1:1.3.1-1"}};
    InstalledPackages upgraded = {{"apt", "3.0.4"}, {"zlib1g", "1:1.3.1-1"}};
    InstalledPackages added = {{"apt", "3.0.3"}, {"bash", "5.2"}, {"zlib1g", "1:1.3.1-1"}};

    auto identity = identity_hash(core, "x86_64", packages);

    // versions do not change the identity, the package set and architecture do
    CHECK(identity == identity_hash(core, "x86_64", upgraded));
    CHECK(identity != identity_hash(core, "x86_64", added));
    CHECK(identity != identity_hash(core, "arm64", packages));

    // the hashed text, to notice format changes
    CHECK(
        identity
        == checksum_data_hex(
            "manifold-identity/1\nbase: net.debian.core13\narchitecture: x86_64\napt\nzlib1g\n", ChecksumKind::Blake3));

    auto content = content_hash(core, "x86_64", core, "13.1", packages);

    // the content hash covers versions and extra lines
    CHECK(content != content_hash(core, "x86_64", core, "13.1", upgraded));
    CHECK(content != content_hash(core, "x86_64", core, "13.1", packages, {"recipe: x"}));
}

TEST_CASE("Identity: directory tree hashes", "[hashes]")
{
    TempDir dir("tree-hash");
    fs::create_directories(dir / "a/sub");
    write_file(dir / "a/sub/file", "content");
    write_file(dir / "a/script", "#!/bin/sh\n");
    fs::create_symlink("sub/file", dir / "a/link");

    auto hash = tree_hash(dir / "a");

    // a copy hashes the same
    fs::copy(dir / "a", dir / "b", fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    CHECK(tree_hash(dir / "b") == hash);

    // the executable bit counts
    fs::permissions(dir / "b/script", fs::perms::owner_exec, fs::perm_options::add);
    auto executable = tree_hash(dir / "b");
    CHECK(executable != hash);

    // so does the content
    write_file(dir / "b/sub/file", "changed");
    CHECK(tree_hash(dir / "b") != executable);
}

TEST_CASE("Manifest: invalid hashes are not written", "[manifest]")
{
    TempDir dir("bad-hash");
    Manifest manifest;
    manifest.identity = checksum_data_hex("x", ChecksumKind::Blake3);
    manifest.content_hash = "AF1349B9";

    CHECK_THROWS_AS(write_manifest(manifest, dir / "manifest.json"), BuildError);
    CHECK(!fs::exists(dir / "manifest.json"));
}

TEST_CASE("Manifest: file names of images and bundles", "[manifest]")
{
    Manifest manifest{.component = "a", .version = "1", .revision = 2, .architecture = "aarch64"};

    CHECK(
        image_file_name("net.debian.core13.sdk", "13.1", 22880949, "x86_64", ".raw")
        == "net.debian.core13.sdk_13.1-22880949_x86-64.raw");
    CHECK(image_file_name(manifest, ".manifold.json") == "a_1-2_aarch64.manifold.json");

    // a bundle is named without the revision
    CHECK(bundle_file_name("com.example.App", "1.0", "x86_64") == "com.example.App_1.0_x86-64.manifold");
}

TEST_CASE("Manifest: JSON round trip", "[manifest]")
{
    Manifest manifest;
    manifest.component = "net.debian.core13";
    manifest.kind = "core";
    manifest.version = "13.1";
    manifest.architecture = "x86_64";
    manifest.packages["apt"] = {{"apt", "3.0.3"}};
    manifest.build.tools["mkfs.erofs"] = "1.9.4";
    manifest.sources = std::vector<SourceRecord>{
        {.part = "a", .type = "git", .url = "https://example.org/a.git", .commit = "0123"},
        {.part = "b", .type = "dir", .path = "b-src", .tree_hash = "abcd"},
    };

    auto json = to_json(manifest);
    CHECK_THAT(json, ContainsSubstring("\"content-hash\""));
    CHECK_THAT(json, !ContainsSubstring("\"image\""));
    CHECK_THAT(json, ContainsSubstring("\"apt\": {"));

    // a source only records the keys it has
    CHECK_THAT(json, !ContainsSubstring("\"sha256\""));
    CHECK_THAT(json, ContainsSubstring("\"tree-hash\": \"abcd\""));

    auto back = manifest_from_json(json, "roundtrip");
    CHECK(back.component == manifest.component);
    CHECK(back.packages == manifest.packages);
    REQUIRE(back.sources);
    CHECK(back.sources->at(0).pin() == "0123");
    CHECK(back.sources->at(1).pin() == "abcd");
    CHECK(!back.sources->at(1).url);
}

TEST_CASE("MetaInfo: generated documents", "[metainfo]")
{
    MetaInfo sdk{
        .id = "net.example.core.sdk",
        .kind = "addon",
        .extends = "net.example.core",
        .name = "Example Core (SDK)",
        .summary = "Development files for Example Core",
        .description = "First paragraph\nstill first.\n\nSecond <paragraph>.\n",
        .project_license = "MIT",
        .metadata_license = "CC0-1.0",
        .homepage = "https://example.org/core",
        .version = "1.0",
        .release_date = "2026-01-04",
    };

    // the SDK is an addon that extends its runtime
    auto xml = generate_metainfo(sdk);
    CHECK_THAT(xml, ContainsSubstring("type=\"addon\""));
    CHECK_THAT(xml, ContainsSubstring("<extends>net.example.core</extends>"));
    // paragraphs are joined and escaped
    CHECK_THAT(xml, ContainsSubstring("<p>First paragraph still first.</p>"));
    CHECK_THAT(xml, ContainsSubstring("Second &lt;paragraph&gt;"));
    // only the MetaInfo in the bundle names the image to install
    CHECK_THAT(xml, !ContainsSubstring("<bundle"));
    check_valid(xml);

    // a runtime extends nothing
    MetaInfo runtime = sdk;
    runtime.id = "net.example.core";
    runtime.kind = "runtime";
    runtime.extends.clear();
    auto runtime_xml = generate_metainfo(runtime);
    CHECK_THAT(runtime_xml, ContainsSubstring("type=\"runtime\""));
    check_valid(runtime_xml);
}

TEST_CASE("MetaInfo: errors and warnings of the validator", "[metainfo]")
{
    // an application needs a description, and should have a content rating
    auto issues = validate_metainfo(R"(<?xml version="1.0" encoding="UTF-8"?>
<component type="desktop-application">
  <id>com.example.Broken</id>
  <name>Broken</name>
  <summary>Has no description</summary>
  <metadata_license>CC0-1.0</metadata_license>
  <project_license>MIT</project_license>
</component>
)");

    CHECK(std::ranges::contains(issues, true, &MetaInfoIssue::error));
    CHECK(std::ranges::contains(issues, false, &MetaInfoIssue::error));
}

TEST_CASE("MetaInfo: catalog of a bundle", "[metainfo]")
{
    TempDir dir("metainfo");
    auto tree = dir / "tree";
    write_core_tree(tree);

    // the MetaInfo of the image is read
    auto info = parse_metainfo(read_file(tree / "usr/share/metainfo/net.example.core.metainfo.xml"));
    CHECK(info.id == "net.example.core");
    CHECK(info.kind == "runtime");
    CHECK(info.name == "Example Core");
    CHECK(info.summary == "Base layer for examples");
    CHECK(info.project_license == "MIT");
    CHECK(info.metadata_license == "CC0-1.0");
    CHECK(info.homepage == "https://example.org/core");

    // the catalog keeps translations and icon of the image only
    AppStreamCatalog catalog(dir / "catalog");
    catalog.add_tree(tree, "/usr", "net.example.core", manifold_bundle_id("x86_64", "net.example.core", "1.0"));
    auto catalog_file = catalog.write();
    CHECK(catalog_file == dir / "catalog/manifold-local.xml");
    auto xml = read_file(catalog_file);
    CHECK_THAT(xml, ContainsSubstring("<components "));
    CHECK_THAT(xml, ContainsSubstring("origin=\"manifold-local\""));
    CHECK_THAT(xml, ContainsSubstring("Beispielkern"));
    // it names the image to install
    CHECK_THAT(xml, ContainsSubstring("<bundle type=\"manifold\">x86_64/net.example.core/1.0</bundle>"));
    // the icon is stored next to the catalog
    CHECK_THAT(xml, ContainsSubstring("<icon type=\"cached\""));
    CHECK(fs::is_regular_file(dir / "catalog/icons/64x64/net.example.core.jxl"));
    // MetaInfo of packages in the image is left out
    CHECK_THAT(xml, !ContainsSubstring("org.example.tool"));

    // the tree has to describe the image it belongs to
    CHECK_THROWS_AS(
        catalog.add_tree(tree, "/usr", "net.example.other", manifold_bundle_id("x86_64", "net.example.other", "1.0")),
        BuildError);
}

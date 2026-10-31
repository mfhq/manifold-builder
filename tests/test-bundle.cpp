/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "bundle.hpp"
#include "manifest.hpp"
#include "process.hpp"
#include "testshared.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <zip.h>

using namespace mfb;
using Catch::Matchers::ContainsSubstring;
using mfb::test::TempDir;

namespace
{

/**
 * Replace an entry of a ZIP archive with other data.
 */
void replace_entry(const fs::path &archive_path, const std::string &name, std::string_view data)
{
    zip_t *archive = zip_open(archive_path.c_str(), 0, nullptr);
    zip_source_t *changed = zip_source_buffer(archive, data.data(), data.size(), 0);
    zip_file_add(archive, name.c_str(), changed, ZIP_FL_OVERWRITE);
    zip_close(archive);
}

/**
 * Write a bundle with one fake image, its manifest and a catalog that Manifold accepts.
 */
fs::path make_test_bundle(const fs::path &dir, MfSigner *signer = nullptr)
{
    const std::string catalog = R"(<?xml version="1.0" encoding="UTF-8"?>
<components version="1.0">
  <component type="generic">
    <id>net.example.core</id>
    <name>Example Core</name>
  </component>
</components>
)";

    auto manifest = test::fake_manifest("net.example.core", "core");
    manifest.image = test::fake_image(dir, "core.raw");

    fs::create_directories(dir / "icons");
    fs::create_directories(dir / "media");

    auto bundle = dir / "core.manifold";
    write_bundle(bundle, {{dir / "core.raw", to_json(manifest), catalog}}, dir / "icons", dir / "media", signer);
    return bundle;
}

} // namespace

TEST_CASE("Bundle: verification notices a changed image", "[bundle]")
{
    TempDir dir("bundle");
    auto bundle = make_test_bundle(dir.path());

    CHECK_NOTHROW(verify_bundle(bundle));
    // same size, other content
    replace_entry(bundle, "core.raw", "not really an imagf");
    CHECK_THROWS_AS(verify_bundle(bundle), BuildError);
}

TEST_CASE("Bundle: a signing key is only used when asked for, and then has to exist", "[bundle]")
{
    // the test run points XDG_CONFIG_HOME at an empty directory
    CHECK(select_signer(nullptr) == nullptr);
    CHECK_THROWS_AS(select_signer("no-such-key"), BuildError);
}

TEST_CASE("Bundle: a bundle that cannot be signed is written unsigned", "[bundle]")
{
    if (!find_tool("ssh-keygen"))
        SKIP("ssh-keygen is not installed");

    TempDir dir("sign-fails");
    write_file(dir / "key", "not a key\n");
    g_autoptr(MfSigner) signer = select_signer((dir / "key").c_str());

    CHECK_THROWS_WITH(make_test_bundle(dir.path(), signer), ContainsSubstring("was written unsigned"));

    g_autoptr(MfBundle) bundle = mf_bundle_new_from_path((dir / "core.manifold").c_str(), nullptr);
    REQUIRE(bundle != nullptr);
    CHECK(!mf_bundle_is_signed(bundle));
}

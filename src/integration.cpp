/*
 * Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "integration.hpp"

#include "checksum.hpp"
#include "error.hpp"
#include "logging.hpp"

#include <algorithm>
#include <manifold.h>
#include <ranges>
#include <set>

namespace mfb
{

quill::Logger *const logIntegration = log::logger("integration");

namespace
{

/**
 * Describe a file for the manifest. Manifold reads these files from the image
 * as they are, so using a symlink is wrong here.
 */
IntegrationFile describe_file(const fs::path &app, const fs::path &relative)
{
    auto path = app / relative;
    auto shown = (fs::path("/app") / relative).string();

    if (!fs::is_regular_file(fs::symlink_status(path)))
        fail("{} has to be a regular file", shown);
    if (fs::file_size(path) == 0)
        fail("{} is empty", shown);

    auto checksums = checksum_file(path);
    return {shown, checksums.sha256, checksums.blake3};
}

std::vector<IntegrationFile> describe_files(
    const fs::path &app,
    MfIntegrationKind kind,
    const std::vector<std::string> &names)
{
    std::vector<IntegrationFile> files;
    for (const auto &name : names)
        files.push_back(describe_file(app, fs::path(mf_integration_kind_get_dir(kind)) / name));
    return files;
}

fs::path tree_path(const fs::path &app, const IntegrationFile &file)
{
    return app.parent_path() / fs::path(file.path).relative_path();
}

/**
 * Get the values of a key in a file of `Key=value` lines, from all of its groups.
 */
std::vector<std::string> key_values(const std::string &content, std::string_view key)
{
    auto prefix = std::format("{}=", key);
    std::vector<std::string> values;

    for (auto range : content | std::views::split('\n')) {
        std::string_view line(range);
        if (line.starts_with(prefix))
            values.emplace_back(trim(line.substr(prefix.size())));
    }

    return values;
}

/**
 * Warn about a program that a desktop entry runs by its name, if it is not in
 * /app/bin. It may be one of the base, but it is more likely to be a mistake.
 */
void warn_about_missing_programs(const fs::path &app, const IntegrationFile &entry, const std::string &content)
{
    for (const auto &exec : key_values(content, "Exec")) {
        auto program = exec.substr(0, exec.find(' '));

        if (!program.empty() && program.find('/') == std::string::npos
            && !fs::exists(fs::symlink_status(app / "bin" / program)))
            LOG_WARNING(logIntegration, "{} runs '{}', which is not in /app/bin", entry.path, program);
    }
}

/**
 * Warn about the keys of a unit that name a file of the image. Manifold runs
 * the commands of a unit in the image, but systemd reads all other keys on the
 * host, where these files do not exist.
 */
void warn_about_image_paths(const fs::path &app, const IntegrationFile &unit)
{
    auto content = read_file(tree_path(app, unit));

    for (auto range : content | std::views::split('\n')) {
        std::string_view line(range);
        auto equals = line.find('=');

        if (equals != std::string_view::npos && !line.starts_with("Exec") && line.substr(equals).contains("/app/"))
            LOG_WARNING(logIntegration, "{}: systemd looks for this on the host: {}", unit.path, line);
    }
}

/**
 * Find the files of some icons of the hicolor theme. A desktop entry can name
 * any icon, so an icon that Manifold can not install only gets a warning: the
 * entry still works, it just shows no icon on the host.
 */
std::vector<IntegrationFile> collect_icons(
    const fs::path &app,
    const std::string &component,
    const std::set<std::string> &names)
{
    const fs::path theme = mf_integration_kind_get_dir(MF_INTEGRATION_KIND_ICON);
    std::vector<IntegrationFile> icons;

    if (!fs::is_directory(app / theme))
        return icons;

    for (const auto &size : fs::directory_iterator(app / theme)) {
        for (const auto *category : {"apps", "mimetypes"}) {
            if (!fs::is_directory(size.path() / category))
                continue;
            for (const auto &file : fs::directory_iterator(size.path() / category)) {
                const auto &path = file.path();
                auto relative = path.lexically_relative(app);

                if (!names.contains(path.stem().string()))
                    continue;
                if (!mf_integration_name_is_valid(
                        MF_INTEGRATION_KIND_ICON, path.filename().c_str(), component.c_str())) {
                    LOG_WARNING(
                        logIntegration,
                        "Not installing /app/{}: {} can not use this name",
                        relative.string(),
                        component);
                    continue;
                }
                if (file.is_symlink()) {
                    LOG_WARNING(logIntegration, "Not installing /app/{}: it is a link", relative.string());
                    continue;
                }
                icons.push_back(describe_file(app, relative));
            }
        }
    }

    return icons;
}

std::vector<IntegrationAlias> collect_aliases(const fs::path &app, const std::vector<AliasEntry> &entries)
{
    std::vector<IntegrationAlias> aliases;

    for (const auto &entry : entries) {
        const auto *settings = std::get_if<AliasSettings>(&entry);
        auto name = settings != nullptr ? settings->name : std::get<std::string>(entry);
        auto command = settings != nullptr ? settings->command : "bin/" + name;

        if (!fs::exists(fs::symlink_status(app / command)))
            fail("The alias '{}' runs /app/{}, which does not exist", name, command);
        aliases.push_back({name, "/app/" + command});
    }

    return aliases;
}

GBytes *read_bytes(const fs::path &path)
{
    auto content = read_file(path);
    return g_bytes_new(content.data(), content.size());
}

} // namespace

std::optional<IntegrationInfo> collect_integration(const fs::path &tree, const Recipe &recipe)
{
    const auto app = tree / "app";
    auto settings = recipe.integration.value_or(IntegrationSettings{});
    auto main_entry = recipe.component + ".desktop";
    fs::path entries_dir = mf_integration_kind_get_dir(MF_INTEGRATION_KIND_DESKTOP_ENTRY);
    IntegrationInfo info;
    std::set<std::string> icons;

    if (!std::ranges::contains(settings.desktop_entry, main_entry)
        && fs::exists(fs::symlink_status(app / entries_dir / main_entry)))
        settings.desktop_entry.push_back(main_entry);

    info.desktop_entry = describe_files(app, MF_INTEGRATION_KIND_DESKTOP_ENTRY, settings.desktop_entry);
    info.mime_package = describe_files(app, MF_INTEGRATION_KIND_MIME_PACKAGE, settings.mime_package);
    info.systemd_unit_system = describe_files(
        app, MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM, settings.systemd_unit_system);
    info.systemd_unit_user = describe_files(app, MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER, settings.systemd_unit_user);
    info.udev_rule = describe_files(app, MF_INTEGRATION_KIND_UDEV_RULE, settings.udev_rule);
    info.alias = collect_aliases(app, settings.alias);

    for (const auto &entry : info.desktop_entry) {
        auto content = read_file(tree_path(app, entry));

        warn_about_missing_programs(app, entry, content);
        for (const auto &icon : key_values(content, "Icon"))
            icons.insert(icon);
    }
    info.icons = collect_icons(app, recipe.component, icons);

    for (const auto &unit : info.systemd_unit_system)
        warn_about_image_paths(app, unit);
    for (const auto &unit : info.systemd_unit_user)
        warn_about_image_paths(app, unit);

    // the order of the manifest must not depend on the recipe or on the directory order
    for (auto *files :
         {&info.desktop_entry,
          &info.icons,
          &info.mime_package,
          &info.systemd_unit_system,
          &info.systemd_unit_user,
          &info.udev_rule})
        std::ranges::sort(*files, {}, &IntegrationFile::path);
    std::ranges::sort(info.alias, {}, &IntegrationAlias::name);

    if (info.desktop_entry.empty() && info.icons.empty() && info.mime_package.empty()
        && info.systemd_unit_system.empty() && info.systemd_unit_user.empty() && info.udev_rule.empty()
        && info.alias.empty())
        return std::nullopt;
    return info;
}

void check_integration(const fs::path &tree, const std::string &manifest_json)
{
    g_autoptr(GBytes) data = g_bytes_new(manifest_json.data(), manifest_json.size());
    g_autoptr(GError) error = nullptr;
    g_autoptr(MfManifest) manifest = mf_manifest_new_from_bytes(data, &error);

    if (manifest == nullptr)
        fail("The manifest is not valid: {}", error->message);
    if (!mf_manifest_check_integration(manifest, &error))
        fail("{}", error->message);

    auto *integration = mf_manifest_get_integration(manifest);
    if (integration == nullptr)
        return;

    auto *files = mf_integration_get_files(integration);
    for (guint i = 0; i < files->len; i++) {
        auto *file = static_cast<MfIntegrationFile *>(g_ptr_array_index(files, i));
        const char *path = mf_integration_file_get_path(file);
        g_autoptr(GBytes) content = read_bytes(tree / fs::path(path).relative_path());
        g_autoptr(GBytes) rendered = nullptr;

        if (!mf_integration_file_check_content(file, content))
            fail("{} changed after it was recorded in the manifest", path);
        rendered = mf_integration_file_render(file, manifest, content, &error);
        if (rendered == nullptr)
            fail("{}", error->message);
    }

    auto *aliases = mf_integration_get_aliases(integration);
    for (guint i = 0; i < aliases->len; i++) {
        auto *alias = static_cast<MfIntegrationAlias *>(g_ptr_array_index(aliases, i));
        g_autoptr(GBytes) rendered = mf_integration_alias_render(alias, manifest, &error);

        if (rendered == nullptr)
            fail("{}", error->message);
    }
}

} // namespace mfb

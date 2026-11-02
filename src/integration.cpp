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
#include <fnmatch.h>
#include <manifold.h>
#include <map>
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
 * Check that every program a desktop entry runs is in the image. Manifold runs
 * a program that has no path from /app/bin, and refuses one outside of /app.
 */
void check_programs(const fs::path &app, const IntegrationFile &entry, const std::string &content)
{
    for (const auto &exec : key_values(content, "Exec")) {
        std::string program;
        fs::path path;

        if (exec.starts_with('"'))
            program = exec.substr(1, exec.find('"', 1) - 1);
        else
            program = exec.substr(0, exec.find(' '));
        if (program.find('/') == std::string::npos)
            path = app / "bin" / program;
        else if (program.starts_with("/app/"))
            path = app.parent_path() / fs::path(program).relative_path();

        if (program.empty() || path.empty() || !fs::exists(fs::symlink_status(path)))
            fail("{} runs '{}', which is not in /app/bin or below /app", entry.path, program);
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

/**
 * Get the files and links below a directory, sorted by their path relative to
 * it. A link to a directory is not followed, as it may lead out of the tree.
 */
std::vector<fs::path> list_files(const fs::path &dir)
{
    auto files = list_tree(dir);

    std::erase_if(files, [&](const fs::path &file) {
        return fs::is_directory(fs::symlink_status(dir / file));
    });
    std::ranges::sort(files);

    return files;
}

/**
 * Change each line of a text, and keep the text as it is otherwise.
 */
std::string map_lines(const std::string &content, const std::function<std::string(std::string)> &map)
{
    std::string result;

    for (auto range : content | std::views::split('\n')) {
        result += map(std::string(std::string_view(range)));
        result += '\n';
    }
    // a text has one line more than it has newlines
    result.pop_back();

    return result;
}

/**
 * Get the name of a unit or an icon without its suffix, like `foo@` of
 * `foo@.service`, and the suffix.
 */
std::pair<std::string, std::string> split_suffix(const std::string &name)
{
    auto dot = name.rfind('.');
    if (dot == std::string::npos)
        return {name, ""};
    return {name.substr(0, dot), name.substr(dot)};
}

/**
 * Replace a renamed unit in a word of a unit file. An instance of a renamed
 * template unit gets the new name of the template.
 */
std::optional<std::string> rename_unit_word(std::string_view word, const std::map<std::string, std::string> &units)
{
    if (auto found = units.find(std::string(word)); found != units.end())
        return found->second;

    for (const auto &[from, to] : units) {
        auto at = from.find("@.");
        if (at == std::string::npos)
            continue;
        auto prefix = std::string_view(from).substr(0, at + 1);
        auto suffix = std::string_view(from).substr(at + 1);
        if (word.size() > prefix.size() + suffix.size() && word.starts_with(prefix) && word.ends_with(suffix)) {
            auto instance = word.substr(prefix.size(), word.size() - prefix.size() - suffix.size());
            auto to_at = to.find("@.");
            return std::format("{}{}{}", to.substr(0, to_at + 1), instance, to.substr(to_at + 1));
        }
    }

    return std::nullopt;
}

/**
 * Give the renamed units their new names in the keys of a unit. We leave the
 * commands alone, as they are run in the image, where the units have no name.
 */
std::string rename_units_in(const std::string &content, const std::map<std::string, std::string> &units)
{
    return map_lines(content, [&](std::string line) {
        auto equals = line.find('=');
        std::vector<std::string> words;
        bool changed = false;

        if (equals == std::string::npos || line.starts_with('#') || line.starts_with(';') || line.starts_with("Exec"))
            return line;
        for (auto range : std::string_view(line).substr(equals + 1) | std::views::split(' ')) {
            auto renamed = rename_unit_word(std::string_view(range), units);
            changed = changed || renamed.has_value();
            words.push_back(renamed.value_or(std::string(std::string_view(range))));
        }
        return changed ? line.substr(0, equals + 1) + join(words, " ") : line;
    });
}

/**
 * Give the renamed icons their new names in a desktop entry.
 */
std::string rename_icons_in(const std::string &content, const std::map<std::string, std::string> &icons)
{
    return map_lines(content, [&](std::string line) {
        auto equals = line.find('=');

        if (equals == std::string::npos || !(line.starts_with("Icon=") || line.starts_with("Icon[")))
            return line;
        auto found = icons.find(line.substr(equals + 1));
        return found != icons.end() ? line.substr(0, equals + 1) + found->second : line;
    });
}

/**
 * Rewrite a file of the app in place. Only a regular file is rewritten: the
 * builder runs on the host, where a link of the tree may point anywhere.
 */
void rewrite_file(const fs::path &path, const std::function<std::string(const std::string &)> &rewrite)
{
    if (!fs::is_regular_file(fs::symlink_status(path)))
        return;
    auto content = read_file(path);
    auto rewritten = rewrite(content);
    if (rewritten != content)
        write_file(path, rewritten);
}

/**
 * Check that the units which systemd pairs by their names stay pairs: a socket,
 * a timer or a path unit starts the service of the same name.
 */
void check_unit_pairs(const fs::path &dir, const std::map<std::string, std::string> &units)
{
    static const std::pair<const char *, const char *> pairs[] = {
        {".socket", ".service"},
        {".timer", ".service"},
        {".path", ".service"},
    };

    for (const auto &[from, to] : units) {
        auto [stem, suffix] = split_suffix(from);
        for (const auto &[first, second] : pairs) {
            std::string partner;
            if (suffix == first)
                partner = stem + second;
            else if (suffix == second)
                partner = stem + first;
            else
                continue;

            auto renamed = units.find(partner);
            if (renamed == units.end()) {
                if (fs::exists(fs::symlink_status(dir / partner)))
                    fail(
                        "rename: {} and {} belong together by their names, so they have to be renamed together",
                        from,
                        partner);
                continue;
            }
            if (split_suffix(renamed->second).first != split_suffix(to).first)
                fail("rename: {} and {} belong together by their names, so they need the same new name", from, partner);
        }
    }
}

GBytes *read_bytes(const fs::path &path)
{
    auto content = read_file(path);
    return g_bytes_new(content.data(), content.size());
}

} // namespace

void rename_files(const fs::path &app, const Recipe &recipe)
{
    std::map<fs::path, fs::path> renamed;

    if (!recipe.rename)
        return;

    auto files = list_files(app);
    for (const auto &[pattern, name] : *recipe.rename) {
        bool matched = false;

        for (const auto &file : files) {
            if (fnmatch(pattern.c_str(), file.c_str(), FNM_PATHNAME) != 0)
                continue;
            matched = true;

            auto to = file.parent_path() / name;
            if (renamed.contains(file))
                fail("rename: /app/{} is matched by more than one pattern", file.string());
            if (fs::exists(fs::symlink_status(app / to)))
                fail("rename: /app/{} can not get the name {}, as /app/{} exists", file.string(), name, to.string());
            LOG_INFO(logIntegration, "Renaming /app/{} to {}", file.string(), name);
            fs::rename(app / file, app / to);
            renamed[file] = to;
        }
        if (!matched)
            fail("rename: '{}' matches no file below /app", pattern);
    }

    // the units of each manager refer to each other by their names
    for (auto kind : {MF_INTEGRATION_KIND_SYSTEMD_UNIT_SYSTEM, MF_INTEGRATION_KIND_SYSTEMD_UNIT_USER}) {
        fs::path dir = mf_integration_kind_get_dir(kind);
        std::map<std::string, std::string> units;

        for (const auto &[from, to] : renamed) {
            if (from.parent_path() == dir)
                units[from.filename().string()] = to.filename().string();
        }
        if (units.empty())
            continue;
        check_unit_pairs(app / dir, units);
        for (const auto &file : list_files(app / dir))
            rewrite_file(app / dir / file, [&](const std::string &content) {
                return rename_units_in(content, units);
            });
    }

    // desktop entries refer to icons by their names, which are the same in every size
    fs::path theme = mf_integration_kind_get_dir(MF_INTEGRATION_KIND_ICON);
    std::map<std::string, std::string> icons;
    for (const auto &[from, to] : renamed) {
        auto in_theme = from.lexically_relative(theme);
        if (in_theme.empty() || *in_theme.begin() == "..")
            continue;
        auto old_name = from.stem().string();
        auto new_name = to.stem().string();
        if (auto known = icons.find(old_name); known != icons.end() && known->second != new_name)
            fail("rename: the icon {} gets more than one new name: {} and {}", old_name, known->second, new_name);
        icons[old_name] = new_name;
    }
    fs::path entries = mf_integration_kind_get_dir(MF_INTEGRATION_KIND_DESKTOP_ENTRY);
    if (icons.empty() || !fs::is_directory(app / entries))
        return;
    for (const auto &file : list_files(app / entries))
        rewrite_file(app / entries / file, [&](const std::string &content) {
            return rename_icons_in(content, icons);
        });
}

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

        check_programs(app, entry, content);
        for (const auto &icon : key_values(content, "Icon"))
            icons.insert(icon);
    }
    info.icon = collect_icons(app, recipe.component, icons);

    for (const auto &unit : info.systemd_unit_system)
        warn_about_image_paths(app, unit);
    for (const auto &unit : info.systemd_unit_user)
        warn_about_image_paths(app, unit);

    // the order of the manifest must not depend on the recipe or on the directory order
    for (auto *files :
         {&info.desktop_entry,
          &info.icon,
          &info.mime_package,
          &info.systemd_unit_system,
          &info.systemd_unit_user,
          &info.udev_rule})
        std::ranges::sort(*files, {}, &IntegrationFile::path);
    std::ranges::sort(info.alias, {}, &IntegrationAlias::name);

    if (info.desktop_entry.empty() && info.icon.empty() && info.mime_package.empty() && info.systemd_unit_system.empty()
        && info.systemd_unit_user.empty() && info.udev_rule.empty() && info.alias.empty())
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

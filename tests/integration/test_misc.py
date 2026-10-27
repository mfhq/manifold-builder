# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Tests for the command line that build nothing.

BROKEN_RECIPE = '''\
component: com.example.Broken
version: 1
command: broken
base: net.debian.core13
base-version: "13"
parts: [{name: broken, buildsystem: quake}]
'''


def test_version(builder):
    result = builder.run('--version')
    assert result.returncode == 0
    assert result.stdout.startswith('manifold-builder ')


def test_usage(builder):
    """Exit with status 2 if the command line makes no sense."""
    assert builder.run().returncode == 2  # no command
    assert builder.run('build').returncode == 2  # no recipe


def test_invalid_recipe(builder, tmp_path):
    """Print what is wrong with a recipe that can not be loaded."""
    recipe = tmp_path / 'broken.yaml'
    recipe.write_text(BROKEN_RECIPE)
    result = builder.run('build', str(recipe))
    assert result.returncode == 1
    assert "'buildsystem' must be" in result.stdout


def test_check_refuses_garbage(builder, tmp_path):
    bundle = tmp_path / 'garbage.manifold'
    bundle.write_bytes(b'not a zip file')
    assert builder.run('check', str(bundle)).returncode == 1

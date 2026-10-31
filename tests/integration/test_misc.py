# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Tests for the command line that build nothing.

from conftest import SAMPLES_DIR

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
    # contradicting options
    assert builder.run('build', '--sign=key', '--no-sign', 'recipe.yaml').returncode == 2
    assert builder.run('sign', 'key', 'bundle.manifold').returncode == 2  # not a command


def test_missing_signing_key(builder, tmp_path):
    recipe = SAMPLES_DIR / 'net.debian.core13.yaml'
    output = builder.build_fails(recipe, tmp_path / 'core.manifold', '--sign=no-such-key')
    assert 'no-such-key' in output


def test_invalid_recipe(builder, tmp_path):
    """Print what is wrong with a recipe that can not be loaded."""
    recipe = tmp_path / 'broken.yaml'
    recipe.write_text(BROKEN_RECIPE)
    output = builder.build_fails(recipe, tmp_path / 'broken.manifold')
    assert "'buildsystem' must be" in output


def test_check_refuses_garbage(builder, tmp_path):
    bundle = tmp_path / 'garbage.manifold'
    bundle.write_bytes(b'not a zip file')
    assert builder.run('check', str(bundle)).returncode == 1

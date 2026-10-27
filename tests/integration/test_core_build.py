# Copyright (C) 2025-2026 Matthias Klumpp <matthias@tenstral.net>
#
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Build a small core from the Debian snapshot archive, then a platform and an
# application on top of it, found in a plain directory of extracted bundles.

import shutil
import zipfile
from pathlib import Path

import pytest
from conftest import DATA_DIR

CORE_RECIPE = '''\
type: core
component: com.example.testcore
version: "1.0"

bootstrap:
  suite: trixie
  snapshot: "2026-10-18T00:00:00Z"

locales: [de_DE]

packages:
  apt: [libzstd1]
packages-sdk:
  # dpkg-dev finds the packages an app links against
  apt: [libzstd-dev, pkgconf, dpkg-dev]

actions:
  - name: marker
    run: echo testcore > /etc/testcore

parts:
  - name: data
    buildsystem: simple
    sources:
      - type: file
        path: parts/com.example.testcore.metainfo.xml
    build-commands:
      - install -D -m 0644 /dev/null $DESTDIR$MANIFOLD_DEST/share/testcore/data.txt
      - install -D -m 0644 /dev/null $DESTDIR$MANIFOLD_DEST/share/testcore/junk/junk.txt
      # an old-style path, which goes below /usr
      - install -D -m 0644 /dev/null $DESTDIR/lib/udev/rules.d/60-testcore.rules
      # the core's own MetaInfo, instead of one generated from the recipe
      - install -D -m 0644 com.example.testcore.metainfo.xml $DESTDIR/usr/local/share/metainfo/com.example.testcore.metainfo.xml
    cleanup:
      - /share/testcore/junk
    post-install:
      - ln -s data.txt /usr/share/testcore/link.txt
  # sees what the part before it installed, before cleanup, and can not write
  # to the tree
  - name: reader
    buildsystem: simple
    build-commands:
      - test -e /usr/share/testcore/junk/junk.txt
      - test -L /lib && test -e /lib/udev/rules.d/60-testcore.rules
      - "! touch /usr/share/testcore/stray 2>/dev/null"
      - install -D -m 0644 /usr/share/testcore/data.txt $DESTDIR$MANIFOLD_DEST/share/testcore/copy.txt

sdk-only:
  - include
  - lib/*/pkgconfig
'''

PLATFORM_RECIPE = '''\
type: platform
component: com.example.testcore.jpeg
version: "1.0"
name: JPEG Platform
summary: libjpeg on the test core
base: com.example.testcore
packages:
  apt: [libjpeg62-turbo]
packages-sdk:
  apt: [libjpeg62-turbo-dev]
parts:
  - name: jpeg-data
    buildsystem: simple
    build-commands:
      # the platform's own development packages are there
      - test -e /usr/include/jpeglib.h
      - install -D -m 0644 /dev/null $DESTDIR$MANIFOLD_DEST/share/jpeg-platform/data.txt
      - install -D -m 0644 /dev/null $DESTDIR$MANIFOLD_DEST/include/jpeg-platform.h
sdk-only:
  - include
'''

PLATFORM_APP_RECIPE = '''\
component: com.example.Hello
version: 1.0.0
command: hello
base: com.example.testcore.jpeg
base-version: "1.0"
parts:
  - name: metainfo
    buildsystem: simple
    sources:
      - type: file
        path: parts/com.example.Hello.metainfo.xml
    build-commands:
      - install -D -m 0644 com.example.Hello.metainfo.xml $MANIFOLD_DEST/share/metainfo/com.example.Hello.appdata.xml
  - name: hello
    buildsystem: simple
    build-commands:
      - install -D /usr/bin/true $MANIFOLD_DEST/bin/hello
'''

METAINFO_APP_RECIPE = '''\
component: com.example.Hello
version: 1.0.0
command: hello
base: com.example.testcore
base-version: "1.0"
metainfo-file: hello.metainfo.xml
generate-sdk: true
sdk-only:
  - share/hello/sdk-only.txt
parts:
  - parts/metainfo.yaml
  - name: hello
    buildsystem: simple
    build-commands:
      - install -D /usr/bin/true $MANIFOLD_DEST/bin/hello
      - install -D -m 0644 /dev/null $MANIFOLD_DEST/share/hello/sdk-only.txt
'''


def unpack(bundle: Path, runtimes: Path):
    """Extract a bundle into a directory the builder searches for runtimes."""
    runtimes.mkdir(exist_ok=True)
    with zipfile.ZipFile(bundle) as archive:
        archive.extractall(runtimes)


@pytest.fixture(scope='module')
def core(builder, tmp_path_factory):
    """Build the test core once for all tests."""
    work = tmp_path_factory.mktemp('core')
    shutil.copytree(DATA_DIR / 'parts', work / 'parts')
    recipe = work / 'com.example.testcore.yaml'
    recipe.write_text(CORE_RECIPE)
    return builder.build(recipe, work / 'testcore.manifold')


@pytest.fixture(scope='module')
def platform(builder, core, tmp_path_factory):
    """Build the JPEG platform on the test core once for all tests."""
    dir = tmp_path_factory.mktemp('platform')
    unpack(core.path, dir / 'runtimes')
    recipe = dir / 'com.example.testcore.jpeg.yaml'
    recipe.write_text(PLATFORM_RECIPE)
    return builder.build(recipe, dir / 'jpeg.manifold', '--runtimes-dir', str(dir / 'runtimes'))


def test_core_manifests(core):
    """Check what the manifests say about the core and its SDK."""
    runtime = core.manifest('core')
    sdk = core.manifest('core', sdk=True)

    assert runtime['component'] == 'com.example.testcore'
    assert runtime['build']['distribution'] == 'debian'
    assert runtime['build']['suite'] == 'trixie'
    assert sdk['base']['content-hash'] == runtime['content-hash']

    # the library goes to the runtime, its development package to the SDK
    apt = runtime['packages']['apt']
    assert 'libzstd1' in apt and 'libzstd-dev' not in apt
    assert 'libzstd-dev' in sdk['packages']['apt']
    assert 'pkgconf' in sdk['packages']['apt']
    # locales pull in the packages that generate them
    assert 'locales' in apt and 'libc-l10n' in apt


def test_core_tree(core):
    """Check the files of the core image against its recipe."""
    runtime = core.manifest('core')
    tree = core.extract(runtime)
    assert (tree / 'manifold.json').is_file()

    # MetaInfo shipped by the core is moved to /usr/share unchanged
    metainfo = (tree / 'usr/share/metainfo/com.example.testcore.metainfo.xml').read_text()
    assert metainfo == (DATA_DIR / 'parts/com.example.testcore.metainfo.xml').read_text()
    assert not (tree / 'usr/local/share/metainfo/com.example.testcore.metainfo.xml').exists()

    # only the catalog names the image
    assert '<name>Test Core</name>' in core.component(runtime)
    assert not [name for name in core.names if name.endswith('.metainfo.xml')]

    # the action ran
    assert (tree / 'etc/testcore').read_text() == 'testcore\n'
    # files of the Manifold configuration
    assert (tree / 'etc/ld.so.conf.d/manifold-app.conf').is_file()
    assert (tree / 'etc/fonts/conf.d/50-manifold-host-fonts.conf').is_file()

    # parts: installed, linked, copied, cleaned up
    assert (tree / 'usr/share/testcore/data.txt').is_file()
    assert (tree / 'usr/share/testcore/link.txt').is_symlink()
    assert (tree / 'usr/share/testcore/copy.txt').is_file()
    assert not (tree / 'usr/share/testcore/junk').exists()

    # old-style paths go below /usr
    assert (tree / 'lib').is_symlink()
    assert (tree / 'usr/lib/udev/rules.d/60-testcore.rules').is_file()

    # development files and documentation go to the SDK
    assert not (tree / 'usr/include').exists()
    assert not (tree / 'usr/share/doc').exists()

    # passwd installs chage setgid shadow, which images do not keep
    assert (tree / 'usr/bin/chage').is_file()
    assert not [
        path for path in tree.rglob('*') if not path.is_symlink() and path.stat().st_mode & 0o6000
    ]


def test_core_sdk(core):
    """Check the files and the generated MetaInfo of the core SDK."""
    sdk = core.manifest('core', sdk=True)
    sdk_tree = core.extract(sdk)
    assert (sdk_tree / 'manifold.json').is_file()
    assert (sdk_tree / 'usr/include/zstd.h').is_file()

    # the SDK's MetaInfo is generated from the core's
    sdk_metainfo = (
        sdk_tree / 'usr/share/metainfo/com.example.testcore.sdk.metainfo.xml'
    ).read_text()
    assert '<name>Test Core (SDK)</name>' in sdk_metainfo
    assert '<extends>com.example.testcore</extends>' in sdk_metainfo
    assert 'manifold.id' not in sdk_metainfo
    assert '<name>Test Core (SDK)</name>' in core.component(sdk)


def test_core_is_reproducible(builder, core, tmp_path):
    shutil.copytree(DATA_DIR / 'parts', tmp_path / 'parts')
    recipe = tmp_path / 'com.example.testcore.yaml'
    recipe.write_text(CORE_RECIPE)
    again = builder.build(recipe, tmp_path / 'testcore.manifold')
    for sdk in (False, True):
        first = core.manifest('core', sdk)['image']
        second = again.manifest('core', sdk)['image']
        assert first['verity-root-hash'] == second['verity-root-hash']
        assert first['sha256'] == second['sha256']


def test_platform(platform, core):
    """Check the platform built on the core, and its SDK."""
    runtime = platform.manifest('platform')
    sdk = platform.manifest('platform', sdk=True)
    tree = platform.extract(runtime)
    sdk_tree = platform.extract(sdk)

    assert runtime['base']['component'] == 'com.example.testcore'
    assert 'libjpeg62-turbo' in runtime['packages']['apt']
    assert 'libjpeg62-turbo-dev' in sdk['packages']['apt']

    # the platform ships no MetaInfo, so it is generated from the recipe
    metainfo = (tree / 'usr/share/metainfo/com.example.testcore.jpeg.metainfo.xml').read_text()
    assert '<name>JPEG Platform</name>' in metainfo
    assert 'manifold.id' not in metainfo

    # the part goes to the platform, its header to the platform's SDK
    assert (tree / 'usr/share/jpeg-platform/data.txt').is_file()
    assert not (tree / 'usr/include').exists()
    assert (sdk_tree / 'usr/include/jpeg-platform.h').is_file()

    # the SDK's package database also has what the core SDK below it installed
    assert {'libzstd-dev', 'pkgconf'} <= sdk['packages']['apt'].keys()

    # the SDK installs the library too, but only the platform stores it
    assert list(tree.glob('usr/lib/*/libjpeg.so.62*'))
    assert not list(sdk_tree.glob('usr/lib/*/libjpeg.so.62*'))


def test_app_on_platform(builder, core, platform, tmp_path):
    """Build an app on the platform, which gets a kit of its own."""
    runtimes = tmp_path / 'runtimes'
    unpack(core.path, runtimes)
    unpack(platform.path, runtimes)
    shutil.copytree(DATA_DIR / 'parts', tmp_path / 'parts')
    recipe = tmp_path / 'com.example.Hello.yaml'
    recipe.write_text(PLATFORM_APP_RECIPE)

    bundle = builder.build(recipe, tmp_path / 'hello.manifold', '--runtimes-dir', str(runtimes))

    kit = bundle.manifest('kit')
    app = bundle.manifest('app')
    assert kit['component'].startswith('com.example.testcore.jpeg.')
    assert kit['base']['component'] == 'com.example.testcore.jpeg'

    tree = bundle.extract(app)
    assert (tree / 'app/bin/hello').is_file()
    # the app's appdata file is renamed, and its content kept
    metainfo = (tree / 'app/share/metainfo/com.example.Hello.metainfo.xml').read_text()
    assert metainfo == (DATA_DIR / 'parts/com.example.Hello.metainfo.xml').read_text()
    assert not (tree / 'app/share/metainfo/com.example.Hello.appdata.xml').exists()
    assert '<name>Hello</name>' in bundle.component(app)


def test_app_metainfo_file(builder, core, tmp_path):
    """Build an app whose recipe names a MetaInfo file, together with its SDK."""
    runtimes = tmp_path / 'runtimes'
    unpack(core.path, runtimes)
    shutil.copytree(DATA_DIR / 'parts', tmp_path / 'parts')
    # the recipe's file replaces the one the app installs
    shipped = (DATA_DIR / 'parts/com.example.Hello.metainfo.xml').read_text()
    (tmp_path / 'hello.metainfo.xml').write_text(
        shipped.replace('Says hello', 'Says hello from the recipe')
    )
    recipe = tmp_path / 'com.example.Hello.yaml'
    recipe.write_text(METAINFO_APP_RECIPE)

    bundle = builder.build(recipe, tmp_path / 'hello.manifold', '--runtimes-dir', str(runtimes))

    app = bundle.manifest('app')
    metainfo = (
        bundle.extract(app) / 'app/share/metainfo/com.example.Hello.metainfo.xml'
    ).read_text()
    assert metainfo == (tmp_path / 'hello.metainfo.xml').read_text()
    assert 'Says hello from the recipe' in bundle.component(app)

    # the app SDK sits on the kit SDK and keeps the sdk-only files
    app_sdk = bundle.manifest('app', sdk=True)
    kit_sdk = bundle.manifest('kit', sdk=True)
    assert app_sdk['component'] == 'com.example.Hello.sdk'
    assert app_sdk['base']['component'] == kit_sdk['component']
    # the kit SDK installs nothing, its package list is the one of the core SDK
    assert 'libzstd-dev' in kit_sdk['packages']['apt']
    # the app SDK has the same package list
    assert app_sdk['packages'] == kit_sdk['packages']

    sdk_tree = bundle.extract(app_sdk)
    assert (sdk_tree / 'app/share/hello/sdk-only.txt').is_file()
    # the app SDK's MetaInfo is generated from the app's
    sdk_metainfo = (sdk_tree / 'app/share/metainfo/com.example.Hello.sdk.metainfo.xml').read_text()
    assert '<name>Hello (SDK)</name>' in sdk_metainfo
    assert '<extends>com.example.Hello</extends>' in sdk_metainfo

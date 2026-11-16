#!/usr/bin/env python3
"""Provide the pinned MSVC toolchain, Windows SDK and CommonLibSSE-NG bundle in this workspace.

When ../SkiingInSkyrim (or SPCF_SIBLING) already has them, deps/ and toolchain/ are symlinked to it
(read only; nothing there is changed). Otherwise everything is downloaded here, as SkiingInSkyrim
does; running MSVC's installer payloads implies accepting the Visual Studio Build Tools license.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.request
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wine_env  # noqa: E402

ROOT = wine_env.ROOT
DEPS = ROOT / 'deps'
TOOLCHAIN = ROOT / 'toolchain'
SIBLING = Path(os.environ.get('SPCF_SIBLING', ROOT.parent / 'SkiingInSkyrim'))

COMMONLIB_TAG = 'v15.7.0'
COMMONLIB_ASSET = f'commonlibsse-ng-prebuilt-{COMMONLIB_TAG}-all-msvc-cmake.zip'
COMMONLIB_SHA256 = '651cd22690fcd69b29624a0c2cf287a4ff7b5e3d9740ef967161ea0c32417c6d'
COMMONLIB_URL = f'https://github.com/alandtse/CommonLibSSE-NG/releases/download/{COMMONLIB_TAG}/{COMMONLIB_ASSET}'

# The bundle's CommonLibSSE.lib needs fmt 12.2's out-of-line fmt::detail::allocate.
SOURCES = {
    'fmt': ('https://github.com/fmtlib/fmt.git', '12.2.0'),
    'spdlog': ('https://github.com/gabime/spdlog.git', 'v1.17.0'),
    'DirectXTK': ('https://github.com/microsoft/DirectXTK.git', 'may2026'),
    'DirectXMath': ('https://github.com/microsoft/DirectXMath.git', 'jun2026'),
}

MSVC_WINE = ('https://github.com/mstorsjo/msvc-wine.git', '514f8ea34842cd6d831804d0e9658d3a32870ae1')
MSVC_COMPONENT = 'Microsoft.VisualStudio.Component.VC.14.51.x86.x64'
MSVC_VERSION = '14.51.36231'
CL_VERSION = '19.51.36260'
SDK_VERSION = '10.0.26100'


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def link(target, source):
    if target.is_symlink() or target.exists():
        return False
    if not source.exists():
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    target.symlink_to(source.resolve(), target_is_directory=True)
    print(f'Linked {target.relative_to(ROOT)} -> {source.resolve()}')
    return True


def link_sibling():
    link(DEPS / 'commonlib-prebuilt', SIBLING / 'deps/commonlib-prebuilt')
    for name in SOURCES:
        link(DEPS / 'src' / name, SIBLING / 'deps/src' / name)
    link(TOOLCHAIN / 'msvc', SIBLING / 'toolchain/msvc')


def fetch_commonlib():
    target = DEPS / 'commonlib-prebuilt'
    if (target / 'lib/CommonLibSSE.lib').exists():
        return
    archive = DEPS / COMMONLIB_ASSET
    if not archive.exists() or sha256(archive) != COMMONLIB_SHA256:
        print('Downloading', COMMONLIB_URL)
        urllib.request.urlretrieve(COMMONLIB_URL, archive)
    if sha256(archive) != COMMONLIB_SHA256:
        raise SystemExit(f'{archive.name}: SHA-256 mismatch')
    staging = DEPS / 'commonlib-prebuilt.tmp'
    shutil.rmtree(staging, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(staging)
    inner = next(p.parent.parent for p in staging.rglob('lib/CommonLibSSE.lib'))
    shutil.rmtree(target, ignore_errors=True)
    inner.rename(target)
    shutil.rmtree(staging, ignore_errors=True)


def clone(name, url, ref):
    target = DEPS / 'src' / name
    if target.exists():
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    if len(ref) == 40:
        subprocess.run(['git', 'init', '-q', str(target)], check=True)
        subprocess.run(['git', '-C', str(target), 'fetch', '-q', '--depth', '1', url, ref], check=True)
        subprocess.run(['git', '-C', str(target), 'checkout', '-q', 'FETCH_HEAD'], check=True)
    else:
        subprocess.run(['git', '-c', 'advice.detachedHead=false', 'clone', '-q', '--depth', '1',
                        '--branch', ref, url, str(target)], check=True)
    return target


def fetch_msvc():
    dest = TOOLCHAIN / 'msvc'
    cl = dest / 'VC/Tools/MSVC' / MSVC_VERSION / 'bin/Hostx64/x64/cl.exe'
    if cl.exists() and any((dest / 'Windows Kits/10/Include').glob(SDK_VERSION + '*')):
        return
    vsdownload = clone('msvc-wine', *MSVC_WINE) / 'vsdownload.py'
    cache = ROOT / 'build/vs-cache'
    cache.mkdir(parents=True, exist_ok=True)
    wine_env.ensure_prefix()
    environment = dict(os.environ)
    environment['PATH'] = str(ROOT / 'tools') + os.pathsep + environment.get('PATH', '')
    subprocess.run([sys.executable, str(vsdownload), '--accept-license',
                    '--dest', str(dest), '--cache', str(cache),
                    '--host-arch', 'x64', '--architecture', 'x64',
                    '--with-sdk', 'yes', '--sdk-version', SDK_VERSION,
                    MSVC_COMPONENT], env=environment, check=True)


def check_compiler():
    cl = TOOLCHAIN / 'msvc/VC/Tools/MSVC' / MSVC_VERSION / 'bin/Hostx64/x64/cl.exe'
    wine_env.ensure_prefix()
    banner = wine_env.run(cl, [], capture_output=True, text=True, timeout=300).stderr
    if f'Version {CL_VERSION}' not in banner:
        raise SystemExit(f'cl.exe is not {CL_VERSION}:\n{banner}')
    print(next(line for line in banner.splitlines() if CL_VERSION in line))


def main():
    DEPS.mkdir(exist_ok=True)
    link_sibling()
    fetch_commonlib()
    for name, (url, ref) in SOURCES.items():
        clone(name, url, ref)
    fetch_msvc()
    check_compiler()
    print('Toolchain ready')


if __name__ == '__main__':
    main()

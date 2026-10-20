#!/usr/bin/env python3
"""Downloads the build tools of the Roll Away PC port into tools/:

  llvm-mingw   clang + mingw-w64 (C/C++ compiler for Windows, i686 target)
  ninja        build tool

Each archive is checked against its published SHA256 before it is unpacked.

    python tools/setup.py
"""
import hashlib
import os
import shutil
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DOWNLOADS = os.path.join(HERE, '_downloads')

TOOLS = [
    {
        'name': 'llvm-mingw',
        'url': 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/'
               'llvm-mingw-20261006-ucrt-x86_64.zip',
        'sha256': '317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a',
        'dest': 'llvm-mingw',
        'strip_top_dir': True,
    },
    {
        'name': 'ninja',
        'url': 'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip',
        'sha256': '07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65',
        'dest': 'ninja',
        'strip_top_dir': False,
    },
]


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def fetch(tool):
    dest = os.path.join(HERE, tool['dest'])
    if os.path.isdir(dest):
        print(f'{tool["name"]}: already in {dest}')
        return
    os.makedirs(DOWNLOADS, exist_ok=True)
    archive = os.path.join(DOWNLOADS, tool['url'].rsplit('/', 1)[1])
    if not os.path.exists(archive) or sha256(archive) != tool['sha256']:
        print(f'{tool["name"]}: downloading {tool["url"]}')
        urllib.request.urlretrieve(tool['url'], archive)
    digest = sha256(archive)
    if digest != tool['sha256']:
        sys.exit(f'{tool["name"]}: SHA256 mismatch ({digest}), not unpacking')
    print(f'{tool["name"]}: unpacking')
    tmp = dest + '.tmp'
    shutil.rmtree(tmp, ignore_errors=True)
    with zipfile.ZipFile(archive) as z:
        z.extractall(tmp)
    if tool['strip_top_dir']:
        (top,) = os.listdir(tmp)
        os.rename(os.path.join(tmp, top), dest)
        os.rmdir(tmp)
    else:
        os.rename(tmp, dest)


def main():
    for tool in TOOLS:
        fetch(tool)
    print('done')


if __name__ == '__main__':
    main()

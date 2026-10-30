#!/usr/bin/env python3
"""Repackage the pinned complete r6 payload with the verified r7 Xlorie binary."""
import argparse
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile

BASE_SHA = '9533f812ea464987cf66bf2aa89deb7e2862e289ae5a14a9ca3d6a48b7255016'
XLORIE_SHA = '741e9b87cbf01ea48e2db89648bd2fd1441b6df2ddadf70ebdf05793f4544ef6'
TARGET = 'data/data/com.termux/files/usr/bin/Xlorie'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def archive(deb, section):
    result = run('dpkg-deb', section, str(deb), capture_output=True)
    return tarfile.open(fileobj=io.BytesIO(result.stdout), mode='r:')


def contents(deb):
    result = {}
    with archive(deb, '--fsys-tarfile') as data:
        for member in data:
            name = member.name.removeprefix('./').rstrip('/') or '.'
            record = dict(type=member.type.decode(), mode=member.mode, uid=member.uid,
                          gid=member.gid, size=member.size, link=member.linkname, mtime=member.mtime)
            if member.isfile():
                record['sha256'] = sha(data.extractfile(member).read())
            result[name] = record
    return result


def control(deb):
    with archive(deb, '--ctrl-tarfile') as data:
        return {m.name.removeprefix('./'): data.extractfile(m).read()
                for m in data if m.isfile()}


def main():
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument('--base-deb', required=True, type=Path)
    parser.add_argument('--xlorie', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--version', required=True, choices=['21.1.16-7'])
    parser.add_argument('--source-date-epoch', required=True, type=int)
    args = parser.parse_args()
    if args.source_date_epoch < 0:
        parser.error('source-date-epoch must be nonnegative')
    if sha(args.base_deb.read_bytes()) != BASE_SHA:
        parser.error('base deb SHA256 does not match the complete approved r6 package')
    binary = args.xlorie.read_bytes()
    if sha(binary) != XLORIE_SHA:
        parser.error('Xlorie SHA256 does not match the validated v8 binary')
    old_data = contents(args.base_deb)
    old_control = control(args.base_deb)
    if set(old_control) != {'control'}:
        parser.error('unexpected r6 maintainer/control files')
    text = old_control['control'].decode()
    if not re.search(r'^Package: xorg-server$', text, re.M) or not re.search(r'^Architecture: aarch64$', text, re.M):
        parser.error('unexpected base package/architecture')
    if not re.search(r'^Version: 21\.1\.16-6$', text, re.M):
        parser.error('unexpected base version')
    old_size = int(re.search(r'^Installed-Size: (\d+)$', text, re.M).group(1))
    installed_size = old_size + math.ceil(len(binary) / 1024) - math.ceil(old_data[TARGET]['size'] / 1024)
    text = re.sub(r'^Version: .*$', f'Version: {args.version}', text, flags=re.M)
    text = re.sub(r'^Installed-Size: .*$', f'Installed-Size: {installed_size}', text, flags=re.M)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = args.output_dir / f'xorg-server_{args.version}_aarch64.deb'
    report = args.output_dir / 'xorg-server-payload-diff.json'
    if output.exists() or report.exists():
        parser.error('output already exists; refusing to overwrite')
    env = dict(os.environ, SOURCE_DATE_EPOCH=str(args.source_date_epoch),
               DPKG_DEB_THREADS_MAX='1', TZ='UTC', LC_ALL='C')
    with tempfile.TemporaryDirectory(prefix='xorg-r7-repack-') as work:
        root = Path(work) / 'root'
        run('dpkg-deb', '--raw-extract', str(args.base_deb.resolve()), str(root))
        target = root / TARGET
        with target.open('wb') as stream:
            stream.write(binary)
        os.utime(target, (args.source_date_epoch, args.source_date_epoch))
        (root / 'DEBIAN/control').write_text(text)
        os.utime(root / 'DEBIAN/control', (args.source_date_epoch, args.source_date_epoch))
        os.utime(root / 'DEBIAN', (args.source_date_epoch, args.source_date_epoch))
        # Preserve all original data mtimes, including directories touched by extraction.
        for name, entry in sorted(old_data.items(), key=lambda pair: pair[0].count('/'), reverse=True):
            if name != TARGET:
                os.utime(root / name, (entry['mtime'], entry['mtime']), follow_symlinks=False)
        run('dpkg-deb', '--root-owner-group', '--uniform-compression', '-Zxz', '-z9',
            '--build', str(root), str(output.resolve()), env=env)
    new_data = contents(output)
    if set(old_data) != set(new_data):
        raise RuntimeError('package data file list changed')
    changes = {name: {'before': old_data[name], 'after': new_data[name]}
               for name in old_data if old_data[name] != new_data[name]}
    if set(changes) != {TARGET}:
        raise RuntimeError(f'unexpected payload changes: {list(changes)}')
    for key in ('type', 'mode', 'uid', 'gid', 'link'):
        if old_data[TARGET][key] != new_data[TARGET][key]:
            raise RuntimeError(f'Xlorie {key} changed')
    if new_data[TARGET]['sha256'] != XLORIE_SHA:
        raise RuntimeError('packaged binary hash changed')
    new_control = control(output)
    if new_control != {'control': text.encode()}:
        raise RuntimeError('unexpected output control files')
    strip_mutable = lambda value: re.sub(r'^(Version|Installed-Size): .*\n', '', value, flags=re.M)
    if strip_mutable(text) != strip_mutable(old_control['control'].decode()):
        raise RuntimeError('unexpected control-field changes')
    result = dict(package='xorg-server', version=args.version, architecture='aarch64',
                  source_date_epoch=args.source_date_epoch, base_sha256=BASE_SHA,
                  xlorie_sha256=XLORIE_SHA, deb_sha256=sha(output.read_bytes()),
                  payload_entries=len(new_data), unchanged_payload_entries=len(new_data)-1,
                  payload_changes=changes, control_changes={
                      'Version': {'before': '21.1.16-6', 'after': args.version},
                      'Installed-Size': {'before': old_size, 'after': installed_size}},
                  unchanged_dependencies=True, build_method='repackage validated binary; no native compilation')
    report.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Prepare Rockpod v12 iPod6G PictureFlow index/config/empty slide on a Mac.
Uses the already committed 0x54434810 Rockbox database; never scans audio tags.
Default is staging only. --install backs up and installs native HFS+ files,
but refuses installation if relevant overlay entries could shadow them.
"""
import argparse
import hashlib
import json
import shutil
import struct
import tempfile
from pathlib import Path

MAGIC = 0x54434810
NAMES = {'pictureflow.cfg', 'pictureflow_album.idx', 'emptyslide.pfraw'}

def require(ok, message):
    if not ok:
        raise ValueError(message)

def table(path):
    data = path.read_bytes()
    require(len(data) >= 12, f'{path.name}: truncated header')
    magic, size, count = struct.unpack_from('<III', data)
    require(magic == MAGIC and size == len(data) - 12, f'{path.name}: unexpected header/size')
    records = {}
    pos = 12
    while pos < len(data):
        require(pos + 8 <= len(data), 'Truncated tag record')
        length, _ = struct.unpack_from('<ii', data, pos)
        require(length > 0 and pos + 8 + length <= len(data), 'Invalid tag length')
        raw = data[pos + 8:pos + 8 + length]
        require(b'\0' in raw, 'Unterminated tag')
        raw.split(b'\0', 1)[0].decode('utf-8')
        records[pos] = raw.split(b'\0', 1)[0]
        pos += 8 + length
    require(len(records) == count, f'{path.name}: tag count mismatch')
    return records, data

def pool(records, used):
    data = bytearray()
    offsets = {}
    for seek in sorted(used):
        offsets[seek] = len(data)
        data.extend(records[seek] + b'\0')
    return bytes(data), offsets

def overlay_conflicts(path):
    if not path.exists():
        return []
    with path.open('rb') as f:
        h = f.read(40)
        require(len(h) == 40 and h[:8] == b'RPOVL11\0', 'Unknown overlay header')
        version, block, count, total, bitmap = struct.unpack_from('<5I', h, 8)
        require((version, block, count) == (1, 4096, 2048), 'Unknown overlay layout')
        require(total * block <= path.stat().st_size and bitmap == (total + 7)//8, 'Invalid overlay size')
        f.seek((1 + (bitmap + block - 1)//block) * block)
        found = []
        for slot in range(count):
            e = f.read(320)
            require(len(e) == 320, 'Truncated overlay table')
            ident, parent, flags = struct.unpack_from('<III', e)
            if not flags & 1:
                continue
            length = struct.unpack_from('<H', e, 38)[0]
            require(length <= 260, 'Invalid overlay name')
            name = e[40:40 + length].decode('utf-8')
            if name in NAMES:
                found.append(dict(slot=slot, id=ident, parent=parent, flags=flags, name=name))
        return found

def configuration(path, albums):
    text = path.read_text(encoding='utf-8') if path.exists() else ''
    values = {'file version': '1', 'cache version': '5', 'update albumart': '0',
              'last album': '0', 'art cache pos': str(albums), 'art cache inspected': str(albums)}
    lines = [line for line in text.splitlines() if line.partition(':')[0].strip() not in values]
    return ('\n'.join(lines + [f'{k}: {v}' for k, v in values.items()]) + '\n').encode()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--ipod', type=Path, default=Path('/Volumes/IPOD'))
    ap.add_argument('--output-dir', type=Path, default=Path.home()/'Downloads',
                    help='Parent directory for staging files and installation backups')
    ap.add_argument('--install', action='store_true', help='Install after validation and local backup; refuse overlay conflicts')
    args = ap.parse_args()
    root = args.ipod.resolve()
    rb = root / '.rockbox'
    require((rb / 'rocks/demos/pictureflow.rock').is_file(), 'Expected v12 plugin at .rockbox/rocks/demos/pictureflow.rock not found')
    master_path = rb / 'database_idx.tcd'
    master = master_path.read_bytes()
    require(len(master) >= 24, 'Truncated master database')
    magic, size, tracks, serial, commit, dirty = struct.unpack_from('<6I', master)
    require(magic == MAGIC and dirty == 0 and tracks > 0, 'Database is dirty, empty, or incompatible')
    require(not (rb/'database_tmp.tcd').exists(), 'Pending database commit; stop')
    require(len(master) == 24 + tracks * 96, 'Unexpected master record layout (expected 23 tags + flags)')
    albums, album_file = table(rb/'database_1.tcd')
    artists, artist_file = table(rb/'database_7.tcd')
    pairs = {}
    for i in range(tracks):
        row = struct.unpack_from('<24i', master, 24 + i * 96)
        if row[23] & 1:
            continue
        album, artist, year = row[1], row[7], row[9]
        require(album in albums and artist in artists, 'Master points outside album/artist tables')
        pairs[(album, artist)] = max(pairs.get((album, artist), 0), year)
    require(0 < len(pairs) <= 65535, 'Album count outside iPod index limits')
    anames, aoffset = pool(albums, {a for a, _ in pairs})
    rnames, roffset = pool(artists, {r for _, r in pairs})
    require(len(roffset) <= 65535, 'Artist count outside iPod index limits')
    # ARM32 pf_index_t: pointers are placeholders replaced by the iPod loader.
    header = struct.pack('<4sHH10I', b'PFID', len(roffset), len(pairs),
                         0, 0, len(rnames), 0xffffffff, 0, 0, len(anames), 0xffffffff, 0, 0)
    rows = b''.join(struct.pack('<5i', aoffset[a], roffset[r], pairs[(a,r)], r, a)
                    for a,r in sorted(pairs))
    index = header + rnames + anames + rows
    require(len(header) == 48 and len(rows) == 20 * len(pairs), 'Index layout mismatch')
    # A neutral fallback image; solid RGB565 is independent of transposition.
    empty = struct.pack('<ii', 128, 128) + struct.pack('<H', 0x4208) * (128 * 128)
    cache = rb/'rocks/demos/pictureflow'
    cfg = rb/'rocks/demos/pictureflow.cfg'
    outputs = {cache/'pictureflow_album.idx': index, cache/'emptyslide.pfraw': empty,
               cfg: configuration(cfg, len(pairs))}
    conflicts = overlay_conflicts(root/'.rockpod-rw')
    output_dir = args.output_dir.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix='rockpod-coverflow-', dir=output_dir))
    for target, data in outputs.items():
        (staging/target.name).write_bytes(data)
    report = dict(tracks=tracks, albums=len(pairs), artists=len(roffset), overlay_conflicts=conflicts,
                  database_sha256={p.name: hashlib.sha256(data).hexdigest() for p,data in
                  [(master_path,master),(rb/'database_1.tcd',album_file),(rb/'database_7.tcd',artist_file)]})
    (staging/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f'Tracks: {tracks}; album/artist pairs: {len(pairs)}; artists: {len(roffset)}')
    print('Prepared files:', staging)
    if conflicts:
        print('Overlay entries may hide these files:')
        for e in conflicts:
            print(e)
    if not args.install:
        print('Staging only; nothing on the iPod was changed.')
        return
    require(not conflicts, 'Installation stopped: resolve overlay conflicts first; no iPod files changed')
    for p,data in [(master_path,master),(rb/'database_1.tcd',album_file),(rb/'database_7.tcd',artist_file)]:
        require(p.read_bytes() == data, 'Database changed during preparation; retry')
    backup = staging/'backup'
    backup.mkdir()
    for target in outputs:
        if target.exists():
            shutil.copy2(target, backup/target.name)
    for target,data in outputs.items():
        target.parent.mkdir(parents=True,exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.pf-mac-', delete=False) as f:
            temporary = Path(f.name)
            f.write(data)
            f.flush()
            import os
            os.fsync(f.fileno())
        temporary.replace(target)
        require(target.read_bytes() == data, f'Readback failed: {target}')
    print('Installed and read back successfully. Backups:', backup)
    print('Eject cleanly before booting. Rebuild this index after rebuilding the music database.')

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, struct.error, UnicodeError) as exc:
        raise SystemExit('STOP: ' + str(exc))

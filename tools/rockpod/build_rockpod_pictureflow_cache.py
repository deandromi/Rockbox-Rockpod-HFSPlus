#!/usr/bin/env python3
"""
Build Rockpod PictureFlow .pfraw files on macOS without Rockpod's dithering.

Defaults:
  Music: /Volumes/IPOD/Music
  Cache: /Volumes/IPOD/.rockbox/rocks/demos/pictureflow

Requirements:
  - ffmpeg
  - ffprobe

This script:
  1. Finds album folders containing cover.jpg
  2. Tries audio files until it finds usable album + album_artist tags
  3. Calculates Rockpod's modified-FNV cache filename
  4. Converts cover.jpg to 128x128 RGB
  5. Packs it as non-dithered RGB565 in PictureFlow's transposed layout
  6. Builds all new .pfraw files in a temporary folder first
  7. Only after a successful build, replaces album .pfraw files in the cache

It does NOT delete or modify pictureflow_album.idx.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

DEFAULT_MUSIC = Path("/Volumes/IPOD/Music")
DEFAULT_CACHE = Path("/Volumes/IPOD/.rockbox/rocks/demos/pictureflow")

AUDIO_EXTENSIONS = {
    ".m4a", ".mp4", ".aac", ".flac", ".mp3", ".ogg", ".opus", ".wav", ".aiff", ".aif"
}

TARGET_W = 128
TARGET_H = 128


def fail(msg: str, code: int = 1) -> None:
    print(f"\nERROR: {msg}", file=sys.stderr)
    raise SystemExit(code)


def check_program(name: str) -> None:
    if shutil.which(name) is None:
        fail(f"{name} was not found in PATH. Install ffmpeg first (brew install ffmpeg).")


def run_json(cmd: list[str]) -> dict:
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"Command failed: {' '.join(cmd)}")
    return json.loads(proc.stdout)


def get_tags(track: Path) -> tuple[str, str]:
    """Return (album, album_artist), closely matching what PictureFlow hashes."""
    data = run_json([
        "ffprobe",
        "-v", "error",
        "-show_entries", "format_tags=album,album_artist,artist",
        "-of", "json",
        str(track),
    ])

    raw_tags = data.get("format", {}).get("tags", {})
    tags = {str(k).lower(): str(v) for k, v in raw_tags.items()}

    album = tags.get("album", "").strip()
    album_artist = tags.get("album_artist", "").strip()

    # Rockbox commonly falls back to track artist when album artist is unavailable.
    if not album_artist:
        album_artist = tags.get("artist", "").strip()

    if not album:
        raise ValueError("missing album tag")
    if not album_artist:
        raise ValueError("missing album_artist/artist tag")

    return album, album_artist


def mfnv(text: str) -> int:
    """Rockpod's modified FNV hash, with C-style uint32 overflow."""
    p = 16777619
    h = 0x811C9DC5

    for b in text.encode("utf-8"):
        h = ((h ^ b) * p) & 0xFFFFFFFF

    h = (h + ((h << 13) & 0xFFFFFFFF)) & 0xFFFFFFFF
    h = (h ^ (h >> 7)) & 0xFFFFFFFF
    h = (h + ((h << 3) & 0xFFFFFFFF)) & 0xFFFFFFFF
    h = (h ^ (h >> 17)) & 0xFFFFFFFF
    h = (h + ((h << 5) & 0xFFFFFFFF)) & 0xFFFFFFFF

    return h


def cache_filename(album: str, album_artist: str) -> str:
    # Rockpod uses snprintf("%x%x.pfraw", hash_album, hash_artist)
    return f"{mfnv(album):x}{mfnv(album_artist):x}.pfraw"


def audio_tracks(album_dir: Path) -> list[Path]:
    return sorted(
        (
            p for p in album_dir.iterdir()
            if (
                p.is_file()
                and p.suffix.lower() in AUDIO_EXTENSIONS
                and not p.name.startswith("._")
                and not p.name.startswith(".")
            )
        ),
        key=lambda p: p.name.lower(),
    )


def find_valid_tags(album_dir: Path) -> tuple[Path, str, str]:
    """
    Try every real audio file in the album folder until one yields
    a usable album + album_artist/artist combination.

    This avoids failing an entire album just because track 01 has
    incomplete or oddly written metadata.
    """
    tracks = audio_tracks(album_dir)

    if not tracks:
        raise ValueError("no audio file found")

    tag_errors: list[str] = []

    for track in tracks:
        try:
            album, artist = get_tags(track)
            return track, album, artist
        except Exception as exc:
            tag_errors.append(f"{track.name}: {exc}")

    # Keep the final error concise, but include how many tracks were tried.
    raise ValueError(
        f"no usable album tags found in any of {len(tracks)} track(s)"
    )


def decode_cover_rgb24(cover: Path) -> bytes:
    """
    Ask ffmpeg for an exact 128x128 RGB24 frame.
    Square cover.jpg files preserve their proportions at this size.
    """
    proc = subprocess.run([
        "ffmpeg",
        "-hide_banner",
        "-loglevel", "error",
        "-i", str(cover),
        "-vf", f"scale={TARGET_W}:{TARGET_H}:flags=lanczos",
        "-frames:v", "1",
        "-f", "rawvideo",
        "-pix_fmt", "rgb24",
        "pipe:1",
    ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.decode("utf-8", "replace").strip())

    expected = TARGET_W * TARGET_H * 3
    if len(proc.stdout) != expected:
        raise RuntimeError(
            f"ffmpeg returned {len(proc.stdout)} RGB bytes; expected {expected}"
        )

    return proc.stdout


def scale8_to_bits(v: int, bits: int) -> int:
    """
    Match Rockbox's non-dithered 8-bit -> RGB565 rounding closely.
    Round the 8-bit channel to the target RGB565 channel width.
    """
    val = v * ((1 << bits) - 1)
    return ((val >> 8) + val + 128) >> 8


def build_pfraw(cover: Path, output: Path) -> None:
    rgb = decode_cover_rgb24(cover)

    # Convert row-major RGB24 to RGB565 first.
    pixels: list[int] = [0] * (TARGET_W * TARGET_H)

    pos = 0
    for y in range(TARGET_H):
        row = y * TARGET_W
        for x in range(TARGET_W):
            r = rgb[pos]
            g = rgb[pos + 1]
            b = rgb[pos + 2]
            pos += 3

            r5 = scale8_to_bits(r, 5)
            g6 = scale8_to_bits(g, 6)
            b5 = scale8_to_bits(b, 5)

            pixels[row + x] = (r5 << 11) | (g6 << 5) | b5

    with output.open("wb") as f:
        # pfraw_header: int32 width, int32 height.
        # iPod 6G is little-endian.
        f.write(struct.pack("<ii", TARGET_W, TARGET_H))

        # PictureFlow's custom formatter stores the bitmap transposed:
        # destination = row + column * height.
        for x in range(TARGET_W):
            for y in range(TARGET_H):
                f.write(struct.pack("<H", pixels[y * TARGET_W + x]))


def album_dirs(root: Path) -> list[Path]:
    result: list[Path] = []
    for cover in root.rglob("cover.jpg"):
        if (
            cover.is_file()
            and not cover.name.startswith("._")
            and not any(part.startswith("._") for part in cover.parts)
        ):
            result.append(cover.parent)
    return sorted(set(result), key=lambda p: str(p).lower())


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Build non-dithered Rockpod PictureFlow .pfraw cache files."
    )
    parser.add_argument(
        "--music",
        type=Path,
        default=DEFAULT_MUSIC,
        help=f"Music root (default: {DEFAULT_MUSIC})",
    )
    parser.add_argument(
        "--cache",
        type=Path,
        default=DEFAULT_CACHE,
        help=f"PictureFlow cache directory (default: {DEFAULT_CACHE})",
    )
    parser.add_argument(
        "--keep-old",
        action="store_true",
        help="Do not remove existing album .pfraw files before installing generated ones.",
    )
    args = parser.parse_args()

    check_program("ffmpeg")
    check_program("ffprobe")

    music = args.music.expanduser()
    cache = args.cache.expanduser()

    if not music.is_dir():
        fail(f"Music directory does not exist: {music}")

    cache.mkdir(parents=True, exist_ok=True)

    albums = album_dirs(music)
    if not albums:
        fail(f"No cover.jpg files were found under {music}")

    print(f"Music root : {music}")
    print(f"PF cache   : {cache}")
    print(f"Albums     : {len(albums)}")
    print()
    print("Building new cache files first. Existing PictureFlow files are not touched yet.")
    print()

    generated: list[tuple[Path, str, str, Path]] = []
    failures: list[tuple[Path, str]] = []
    duplicate_hashes: dict[str, tuple[Path, str, str]] = {}
    duplicate_count = 0

    with tempfile.TemporaryDirectory(prefix="rockpod-pfraw-") as tmp_name:
        tmp = Path(tmp_name)

        for i, directory in enumerate(albums, 1):
            cover = directory / "cover.jpg"

            try:
                track, album, artist = find_valid_tags(directory)
                filename = cache_filename(album, artist)

                if filename in duplicate_hashes:
                    first_dir, first_album, first_artist = duplicate_hashes[filename]

                    if album == first_album and artist == first_artist:
                        duplicate_count += 1
                        print(
                            f"[{i}/{len(albums)}] DUP   "
                            f"{artist} - {album}  ->  {filename} "
                            f"(already built from {first_dir})"
                        )
                        continue

                    raise ValueError(
                        "true PictureFlow hash collision: "
                        f"{artist} - {album} has the same hash as "
                        f"{first_artist} - {first_album} ({first_dir})"
                    )

                duplicate_hashes[filename] = (directory, album, artist)
                dest = tmp / filename
                build_pfraw(cover, dest)
                generated.append((directory, album, artist, dest))

                print(
                    f"[{i}/{len(albums)}] OK    "
                    f"{artist} - {album}  ->  {filename}"
                )

            except Exception as exc:
                failures.append((directory, str(exc)))
                print(f"[{i}/{len(albums)}] FAIL  {directory}: {exc}")

        print()
        print("=" * 72)
        print(f"Generated successfully : {len(generated)}")
        print(f"Duplicate album keys   : {duplicate_count}")
        print(f"Failed/skipped         : {len(failures)}")
        print("=" * 72)

        if failures:
            print("\nProblem albums:")
            for directory, reason in failures:
                print(f"  - {directory}: {reason}")

            print(
                "\nNothing has been installed yet because at least one album failed."
            )
            print(
                "Fix the listed albums and run the script again. "
                "This avoids leaving PictureFlow with a half-built cache."
            )
            raise SystemExit(2)

        print("\nAll covers built successfully.")

        # Only now touch the live PictureFlow cache.
        if not args.keep_old:
            removed = 0
            ignored_sidecars = 0

            # Snapshot the list first. macOS may create/remove AppleDouble
            # sidecars (._*.pfraw) while the folder is being modified.
            old_files = list(cache.glob("*.pfraw"))

            for old in old_files:
                # PictureFlow's own fallback graphic is not an album cache entry.
                if old.name == "emptyslide.pfraw":
                    continue

                # Never treat macOS AppleDouble metadata files as PictureFlow art.
                if old.name.startswith("._"):
                    ignored_sidecars += 1
                    continue

                try:
                    old.unlink()
                    removed += 1
                except FileNotFoundError:
                    # Harmless race: Finder/macOS may already have removed it.
                    pass

            print(f"Removed old album .pfraw files: {removed}")
            if ignored_sidecars:
                print(f"Ignored macOS ._ sidecars       : {ignored_sidecars}")

        for _, _, _, built in generated:
            shutil.copy2(built, cache / built.name)

        print(f"Installed new .pfraw files     : {len(generated)}")

    idx = cache / "pictureflow_album.idx"
    print()
    if idx.exists():
        print(f"Left index untouched           : {idx}")
    else:
        print("WARNING: pictureflow_album.idx was not found.")
        print("A matching PictureFlow album index is required before using this cache.")

    print()
    print("DONE.")
    print("Run `sync`, eject the iPod normally, then open Cover Flow.")
    print("PictureFlow 'Rebuild Cache' replaces these images with artwork generated on the iPod.")


if __name__ == "__main__":
    main()

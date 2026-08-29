#!/usr/bin/env python3
"""Add the vanilla structure + loot `data/` half to a resource pack, making it a
combined pack ReBedrock reads for structures/chest-loot while it stays a normal
(vanilla-inert `data/`) resource pack.

This operates on the USER's own pack, assembled from their own extracted
Minecraft: it copies `data/minecraft/structure/**` and `data/minecraft/loot_table/**`
from a reference `data/` directory into the pack's `data/` half, at verbatim
vanilla paths. Nothing here is committed to the ReBedrock repo or shipped — the
pack is the user's runtime content, the same self-supply model as textures.

Usage:
  add_structures_to_pack.py <pack.zip|pack_dir> <reference_data_dir> [--dirs structure loot_table]

<reference_data_dir> is a `.../data/minecraft` directory (an extracted jar's data
half). The pack is updated in place; a .zip is rewritten with the new entries.
"""
from __future__ import annotations

import argparse
import shutil
import sys
import tempfile
import zipfile
from pathlib import Path


def collect(reference_data: Path, subdirs: list[str]) -> list[tuple[str, Path]]:
    """(archive_name, source_path) for every file under the requested subdirs."""
    out: list[tuple[str, Path]] = []
    for sub in subdirs:
        root = reference_data / sub
        if not root.is_dir():
            print(f"  (skip: {root} not found)")
            continue
        for path in sorted(root.rglob("*")):
            if path.is_file():
                rel = path.relative_to(reference_data)
                out.append((f"data/minecraft/{rel.as_posix()}", path))
    return out


def add_to_zip(pack_zip: Path, entries: list[tuple[str, Path]]) -> None:
    # Rewrite the zip: keep every non-data/minecraft/<subdir> entry, then add ours
    # (so re-running replaces rather than duplicates).
    replace_prefixes = tuple(sorted({name.rsplit("/", 1)[0].split("/")[2] for name, _ in entries}))
    keep_prefixes = tuple(f"data/minecraft/{p}/" for p in replace_prefixes)
    tmp_fd, tmp_name = tempfile.mkstemp(suffix=".zip", dir=str(pack_zip.parent))
    tmp = Path(tmp_name)
    import os

    os.close(tmp_fd)
    with zipfile.ZipFile(pack_zip) as src, zipfile.ZipFile(
        tmp, "w", zipfile.ZIP_DEFLATED
    ) as dst:
        for item in src.infolist():
            if item.filename.startswith(keep_prefixes):
                continue  # our subdir: replaced below
            dst.writestr(item, src.read(item.filename))
        for name, path in entries:
            dst.write(path, name)
    shutil.move(str(tmp), str(pack_zip))


def add_to_dir(pack_dir: Path, entries: list[tuple[str, Path]]) -> None:
    for name, path in entries:
        dest = pack_dir / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, dest)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pack", type=Path, help="pack .zip or directory")
    parser.add_argument("reference_data", type=Path, help=".../data/minecraft directory")
    parser.add_argument("--dirs", nargs="+", default=["structure", "loot_table"])
    args = parser.parse_args()

    if not args.reference_data.is_dir():
        sys.exit(f"reference data dir not found: {args.reference_data}")

    entries = collect(args.reference_data, args.dirs)
    if not entries:
        sys.exit("nothing to add (no matching files under reference data)")
    print(f"Adding {len(entries)} files ({', '.join(args.dirs)}) to {args.pack}")

    if args.pack.is_dir():
        add_to_dir(args.pack, entries)
    elif zipfile.is_zipfile(args.pack):
        add_to_zip(args.pack, entries)
    else:
        sys.exit(f"pack is neither a directory nor a zip: {args.pack}")
    print("Done.")


if __name__ == "__main__":
    main()

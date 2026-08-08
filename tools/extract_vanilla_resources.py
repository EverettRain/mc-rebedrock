#!/usr/bin/env python3
"""Extract and classify vanilla Minecraft resources for MC Rebedrock."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import zipfile
from collections import Counter
from pathlib import Path, PurePosixPath


VERSIONS = {
    "1.16.1": "mc-1.16.1-java",
}

CATEGORY_MAP = {
    "textures": "textures",
    "models": "models",
    "blockstates": "blockstates",
    "shaders": "shaders",
    "font": "fonts",
    "lang": "localization",
    "particles": "particles",
    "atlases": "atlases",
    "texts": "texts",
    "sounds": "audio",
}

AUDIO_SUFFIXES = {".ogg", ".wav", ".mp3"}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "versions",
        nargs="*",
        choices=tuple(VERSIONS),
        default=["1.16.1"],
        help="Minecraft version to extract (only 1.16.1 is supported)",
    )
    parser.add_argument(
        "--gradle-user-home",
        type=Path,
        default=Path(os.environ.get("GRADLE_USER_HOME", Path.home() / ".gradle")),
        help="Gradle user home containing Fabric Loom's asset cache",
    )
    return parser.parse_args()


def find_minecraft_jar(workspace: Path, project_name: str) -> Path:
    cache = workspace / project_name / ".gradle" / "loom-cache" / "minecraftMaven"
    candidates = sorted(
        path
        for path in cache.glob("**/*.jar")
        if not path.name.endswith("-sources.jar")
        and ".backup" not in path.name
    )
    if len(candidates) != 1:
        raise RuntimeError(
            f"Expected exactly one merged Minecraft JAR under {cache}, found {len(candidates)}"
        )
    return candidates[0]


def find_asset_index(gradle_user_home: Path, version: str) -> Path:
    index_dir = gradle_user_home / "caches" / "fabric-loom" / "assets" / "indexes"
    candidates = sorted(index_dir.glob(f"{version}-*.json"))
    if not candidates:
        raise RuntimeError(
            f"No asset index for Minecraft {version} under {index_dir}. "
            f"Run that project's './gradlew downloadAssets' first."
        )
    return candidates[-1]


def jar_destination(entry_name: str, output: Path) -> tuple[str, Path] | None:
    path = PurePosixPath(entry_name)
    if entry_name.endswith("/"):
        return None

    if len(path.parts) >= 3 and path.parts[0] == "assets":
        namespace = path.parts[1]
        relative = path.parts[2:]
        first = relative[0]

        if first == "sounds.json":
            return "audio", output / "audio" / namespace / "sounds.json"

        category = CATEGORY_MAP.get(first, "misc")
        if category == "misc":
            destination = output / category / "assets" / namespace / Path(*relative)
        else:
            destination = output / category / namespace / Path(*relative[1:])
        return category, destination

    if len(path.parts) >= 3 and path.parts[0] == "data":
        return "data", output / "data" / Path(*path.parts[1:])

    if path.name in {"pack.mcmeta", "pack.png"}:
        return "metadata", output / "metadata" / "jar" / path.name

    return None


def external_destination(logical_name: str, output: Path) -> tuple[str, Path]:
    path = PurePosixPath(logical_name)
    namespace = path.parts[0] if len(path.parts) > 1 else "minecraft"
    relative = path.parts[1:] if len(path.parts) > 1 else path.parts
    first = relative[0] if relative else "misc"

    if path.suffix.lower() in AUDIO_SUFFIXES or first in {"sounds", "sounds.json"}:
        return "audio", output / "audio" / namespace / Path(*relative)

    category = CATEGORY_MAP.get(first, "external")
    if category == "misc":
        category = "external"
    if category == "external":
        destination = output / category / namespace / Path(*relative)
    else:
        destination = output / category / namespace / Path(*relative[1:])
    return category, destination


def link_or_copy(source: Path, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if destination.stat().st_size == source.stat().st_size:
            return
        destination.unlink()
    try:
        os.link(source, destination)
    except OSError:
        shutil.copy2(source, destination)


def extract_version(
    workspace: Path,
    output_root: Path,
    gradle_user_home: Path,
    version: str,
    project_name: str,
) -> Counter[str]:
    minecraft_jar = find_minecraft_jar(workspace, project_name)
    asset_index = find_asset_index(gradle_user_home, version)
    asset_objects = asset_index.parent.parent / "objects"
    output = output_root / version
    counts: Counter[str] = Counter()

    with zipfile.ZipFile(minecraft_jar) as archive:
        for entry in archive.infolist():
            classified = jar_destination(entry.filename, output)
            if classified is None:
                continue
            category, destination = classified
            destination.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(entry) as source, destination.open("wb") as target:
                shutil.copyfileobj(source, target)
            counts[category] += 1

    index_data = json.loads(asset_index.read_text(encoding="utf-8"))
    missing: list[str] = []
    for logical_name, metadata in index_data.get("objects", {}).items():
        digest = metadata["hash"]
        source = asset_objects / digest[:2] / digest
        if not source.is_file() or source.stat().st_size != metadata["size"]:
            missing.append(logical_name)
            continue
        category, destination = external_destination(logical_name, output)
        link_or_copy(source, destination)
        counts[category] += 1

    if missing:
        preview = "\n".join(f"  - {name}" for name in missing[:20])
        raise RuntimeError(
            f"Minecraft {version} is missing {len(missing)} asset objects:\n{preview}\n"
            f"Run './gradlew downloadAssets' in {project_name} and retry."
        )

    metadata_dir = output / "metadata"
    metadata_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(asset_index, metadata_dir / "asset-index.json")
    manifest_path = metadata_dir / "extraction-manifest.json"
    actual_counts: Counter[str] = Counter()
    for category_dir in output.iterdir():
        if not category_dir.is_dir():
            continue
        actual_counts[category_dir.name] = sum(
            1
            for path in category_dir.rglob("*")
            if path.is_file() and path != manifest_path
        )
    actual_counts["metadata"] += 1

    manifest = {
        "minecraft_version": version,
        "minecraft_jar": str(minecraft_jar.relative_to(workspace)),
        "asset_index": asset_index.name,
        "counts": dict(sorted(actual_counts.items())),
        "total_files": sum(actual_counts.values()),
    }
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    return actual_counts


def main() -> None:
    args = parse_args()
    rebedrock = Path(__file__).resolve().parents[1]
    workspace = rebedrock.parent
    output_root = rebedrock / "resources" / "vanilla"

    for version in args.versions:
        counts = extract_version(
            workspace,
            output_root,
            args.gradle_user_home.expanduser().resolve(),
            version,
            VERSIONS[version],
        )
        summary = ", ".join(f"{name}={count}" for name, count in sorted(counts.items()))
        print(f"Minecraft {version}: {sum(counts.values())} files ({summary})")


if __name__ == "__main__":
    main()

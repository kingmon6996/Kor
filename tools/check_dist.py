# Copyright 2026 Pravanjan Roy (kingmon6996)
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import annotations

import argparse
import re
import sys
import tarfile
import zipfile
from pathlib import Path, PurePosixPath


_PRIVATE_SUFFIXES = {".kr", ".pem", ".key", ".p12", ".pfx"}
_DEVELOPMENT_DIRS = {
    ".git",
    ".venv",
    "benchmarks",
    "build",
    "dist",
    "examples",
    "tests",
    "venv",
}


def fail(message: str) -> None:
    raise ValueError(message)


def check_path_names(names: list[str], artifact: Path, *, wheel: bool = False) -> None:
    for name in names:
        normalized = name.replace("\\", "/")
        path = PurePosixPath(normalized)
        if wheel and any(part in _DEVELOPMENT_DIRS for part in path.parts):
            fail(f"{artifact.name} contains development-only path: {name}")
        if any(part == ".env" or part.startswith(".env.") for part in path.parts):
            fail(f"{artifact.name} contains an environment file: {name}")
        if path.suffix.lower() in _PRIVATE_SUFFIXES:
            fail(f"{artifact.name} contains a local database or sensitive file: {name}")
        if "__pycache__" in path.parts or path.suffix in {".pyc", ".pyo"}:
            fail(f"{artifact.name} contains a Python cache file: {name}")


def read_sdist(artifact: Path) -> tuple[list[str], str]:
    if not tarfile.is_tarfile(artifact):
        fail(f"Not a readable source archive: {artifact}")
    with tarfile.open(artifact, "r:*") as archive:
        members = [member for member in archive.getmembers() if member.isfile()]
        names = [member.name for member in members]
        check_path_names(names, artifact)
        required_suffixes = {
            "/CMakeLists.txt",
            "/LICENSE",
            "/README.md",
            "/pyproject.toml",
            "/include/kor/value.hpp",
            "/src/bindings/module.cpp",
            "/src/version.cpp",
            "/python/kor/__init__.py",
            "/python/kor/_version.py",
        }
        for suffix in required_suffixes:
            if not any(name.endswith(suffix) for name in names):
                fail(f"{artifact.name} is missing required source file {suffix}.")

        version_file = next(
            (member for member in members if member.name.endswith("/python/kor/_version.py")),
            None,
        )
        if version_file is None:
            fail(f"{artifact.name} does not contain the package version source.")
        source = archive.extractfile(version_file)
        if source is None:
            fail(f"Unable to read package version from {artifact.name}.")
        match = re.search(rb'^__version__ = "([^"]+)"\r?$', source.read(), re.MULTILINE)
        if match is None:
            fail(f"{artifact.name} contains an unrecognized package version declaration.")
        return names, match.group(1).decode("ascii")


def check_wheel(artifact: Path, expected_version: str) -> None:
    if not zipfile.is_zipfile(artifact):
        fail(f"Not a readable wheel archive: {artifact}")
    with zipfile.ZipFile(artifact) as wheel:
        names = wheel.namelist()
        check_path_names(names, artifact, wheel=True)
        required = {
            "kor/__init__.py",
            "kor/_version.py",
            "kor/py.typed",
        }
        missing = required.difference(names)
        if missing:
            fail(f"{artifact.name} is missing required package files: {sorted(missing)}")

        extension_suffixes = (".pyd", ".so", ".dylib")
        if not any(
            name.startswith("kor/_kor") and name.endswith(extension_suffixes)
            for name in names
        ):
            fail(f"{artifact.name} does not contain the Kor native extension.")

        metadata_name = next(
            (name for name in names if name.endswith(".dist-info/METADATA")),
            None,
        )
        wheel_name = next(
            (name for name in names if name.endswith(".dist-info/WHEEL")),
            None,
        )
        if metadata_name is None or wheel_name is None:
            fail(f"{artifact.name} is missing wheel distribution metadata.")
        metadata = wheel.read(metadata_name).decode("utf-8")
        wheel_metadata = wheel.read(wheel_name).decode("utf-8")
        if f"Version: {expected_version}" not in metadata:
            fail(f"{artifact.name} metadata version does not match {expected_version}.")
        if "Name: kor-db" not in metadata:
            fail(f"{artifact.name} metadata does not identify the kor-db distribution.")
        if "Root-Is-Purelib: false" not in wheel_metadata:
            fail(f"{artifact.name} is incorrectly tagged as a pure-Python wheel.")
        tags = re.findall(r"^Tag: (.+)$", wheel_metadata, re.MULTILINE)
        if not tags or any(
            len(parts := tag.split("-")) != 3
            or parts[0] != parts[1]
            or not re.fullmatch(r"cp3\d+", parts[0])
            or parts[2] == "any"
            for tag in tags
        ):
            fail(f"{artifact.name} has an invalid or unsupported wheel ABI tag: {tags}")
        if not any(
            name.endswith(".dist-info/licenses/LICENSE")
            or name.endswith(".dist-info/LICENSE")
            for name in names
        ):
            fail(f"{artifact.name} does not include the project license.")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check built Kor wheels and source archives for required files and exclusions."
    )
    parser.add_argument("--sdist", type=Path, required=True)
    parser.add_argument("--wheel", type=Path, action="append", required=True)
    args = parser.parse_args()
    try:
        _, version = read_sdist(args.sdist)
        for wheel in args.wheel:
            check_wheel(wheel, version)
    except (OSError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"Distribution check failed: {error}", file=sys.stderr)
        return 1
    print(f"Validated sdist and {len(args.wheel)} wheel(s) for kor-db {version}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

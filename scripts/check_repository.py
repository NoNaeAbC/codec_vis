#!/usr/bin/env python3
"""Check commit candidates for machine-specific paths and generated files."""

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


# Public libheif seed; provenance and license are stored beside the fixture.
BINARY_FIXTURES = {
    "gui/fixtures/vvc32.vvic":
        "4eacc2bb40fc837834dedd923c1fd45cdc98ea4e3ba91ea2edb0c310063a0349",
}


def main():
    root = Path(__file__).resolve().parents[1]
    candidates = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=root,
    ).decode().split("\0")
    private_path = re.compile(
        r"/(?:home|Users)/[^/\s]+|~[/\\]|[A-Za-z]:\\Users\\[^\\\s]+"
    )
    failures = []
    count = 0
    for name in sorted(set(filter(None, candidates))):
        path = root / name
        if not path.exists() and not path.is_symlink():
            continue  # Deleted tracked files are not part of the next commit.
        count += 1
        if path.is_symlink():
            if not path.resolve().is_relative_to(root):
                failures.append(f"{name}: symlink leaves the repository")
            continue
        if (path.name == "compile_commands.json"
                or "__pycache__" in path.parts
                or path.suffix in {".pyc", ".pyo", ".o", ".spv"}):
            failures.append(f"{name}: generated file")
        data = path.read_bytes()
        if b"\0" in data or name in BINARY_FIXTURES:
            if hashlib.sha256(data).hexdigest() != BINARY_FIXTURES.get(name):
                failures.append(f"{name}: unreviewed binary file")
            continue
        try:
            content = data.decode("utf-8")
        except UnicodeDecodeError:
            failures.append(f"{name}: unreviewed binary file")
            continue
        for line_number, line in enumerate(content.splitlines(), 1):
            if private_path.search(line) or str(root) in line:
                failures.append(f"{name}:{line_number}: private machine path")
            if re.match(r"^(?:<{7}|>{7})(?: |$)", line):
                failures.append(f"{name}:{line_number}: merge conflict marker")

    snapshot = root / "third_party/native_video_decode/native"
    for entry in json.loads((snapshot / "SOURCE.json").read_text()):
        path = snapshot / entry["snapshot"]
        if (not path.resolve().is_relative_to(snapshot)
                or hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]):
            failures.append(f"{entry['snapshot']}: native source manifest mismatch")
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"Repository checks passed ({count} commit candidates).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

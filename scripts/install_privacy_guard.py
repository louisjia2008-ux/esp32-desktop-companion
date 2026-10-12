#!/usr/bin/env python3
"""Install this clone's privacy hooks and pinned Gitleaks without admin access.

The default download and --gitleaks-archive use fixed official release SHA256
digests. --gitleaks-bin is an explicit trust override for an already audited
local executable; its version is checked, but its origin cannot be established
from the executable alone. Nothing is installed into a system directory.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile
import urllib.error
import urllib.parse
import urllib.request


VERSION = "8.30.1"
RELEASE_URL = f"https://github.com/gitleaks/gitleaks/releases/download/v{VERSION}"
# Official gitleaks_8.30.1_checksums.txt, pinned rather than trusting a freshly
# downloaded checksum file alongside a potentially changed release archive.
ARCHIVE_SHA256 = {
    "darwin_arm64": "b40ab0ae55c505963e365f271a8d3846efbc170aa17f2607f13df610a9aeb6a5",
    "darwin_x64": "dfe101a4db2255fc85120ac7f3d25e4342c3c20cf749f2c20a18081af1952709",
    "linux_arm64": "e4a487ee7ccd7d3a7f7ec08657610aa3606637dab924210b3aee62570fb4b080",
    "linux_x64": "551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb",
}
MAX_DOWNLOAD_BYTES = 50 * 1024 * 1024


class InstallError(Exception):
    """An installation failure that is safe to display."""


def git(*args: str, cwd: Path | None = None, missing_ok: bool = False) -> str:
    result = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True, check=False
    )
    if result.returncode and not (missing_ok and result.returncode == 1):
        raise InstallError("Git command failed; run this installer inside this repository.")
    return result.stdout.strip()


def repository_paths() -> tuple[Path, Path]:
    root = Path(git("rev-parse", "--show-toplevel")).resolve()
    if Path(__file__).resolve().parent.parent != root:
        raise InstallError("Run the installer from the repository containing this script.")
    common = Path(git("rev-parse", "--git-common-dir", cwd=root))
    if not common.is_absolute():
        common = root / common
    return root, common.resolve()


def platform_key() -> str:
    system = platform.system().lower()
    machine = platform.machine().lower()
    arch = {"arm64": "arm64", "aarch64": "arm64", "x86_64": "x64", "amd64": "x64"}.get(machine)
    key = f"{system}_{arch}"
    if key not in ARCHIVE_SHA256:
        raise InstallError(
            "Automatic installation supports macOS/Linux on arm64 or x64. "
            "Use --gitleaks-bin with an independently audited Gitleaks 8.30.1 executable."
        )
    return key


def check_hook_conflicts(root: Path, common: Path) -> None:
    configured = git("config", "--get", "core.hooksPath", cwd=root, missing_ok=True)
    if configured:
        active = Path(os.path.expanduser(configured))
        if not active.is_absolute():
            active = root / active
        if active.resolve() != (root / ".githooks").resolve():
            raise InstallError(
                "An existing core.hooksPath points to other hooks. No hook configuration was changed. "
                "Integrate .githooks/pre-commit and .githooks/pre-push into your existing hooks, "
                "or review that configuration before retrying."
            )
    else:
        # A default hooks directory can contain active custom hooks even when
        # core.hooksPath is unset. Do not silently disable them by changing it.
        default_hooks = common / "hooks"
        if default_hooks.is_dir():
            for entry in default_hooks.iterdir():
                if entry.is_file() and not entry.name.endswith(".sample") and os.access(entry, os.X_OK):
                    raise InstallError(
                        "Existing executable Git hooks would be displaced. No hook configuration was changed. "
                        "Integrate the privacy hooks with your existing hooks before retrying."
                    )
    for relative in (".githooks/pre-commit", ".githooks/pre-push", "scripts/privacy_guard.py"):
        if not (root / relative).is_file():
            raise InstallError("Privacy guard files are incomplete; update this checkout before installing.")


class HTTPSRedirectHandler(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise InstallError("Refused an insecure release download redirect.")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def download_archive(filename: str) -> bytes:
    opener = urllib.request.build_opener(HTTPSRedirectHandler())
    request = urllib.request.Request(
        f"{RELEASE_URL}/{filename}", headers={"User-Agent": "esp32-privacy-guard-installer"}
    )
    try:
        with opener.open(request, timeout=30) as response:
            data = response.read(MAX_DOWNLOAD_BYTES + 1)
    except (urllib.error.URLError, OSError, TimeoutError):
        # A URL error may contain a signed redirect URL. Never echo it.
        raise InstallError(
            "Could not download the pinned official release. Check your network, "
            "or use --gitleaks-archive with the official archive for offline installation."
        ) from None
    if len(data) > MAX_DOWNLOAD_BYTES:
        raise InstallError("Release archive exceeded the download size limit.")
    return data


def read_limited(path: Path) -> bytes:
    if not path.is_file() or path.stat().st_size > MAX_DOWNLOAD_BYTES:
        raise InstallError("The supplied file is missing or exceeds the size limit.")
    return path.read_bytes()


def verify_and_extract(archive: bytes, key: str) -> bytes:
    if hashlib.sha256(archive).hexdigest() != ARCHIVE_SHA256[key]:
        raise InstallError("Gitleaks archive SHA256 does not match the pinned official release; installation stopped.")
    try:
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as tar:
            members = [item for item in tar.getmembers() if item.name == "gitleaks"]
            if len(members) != 1 or not members[0].isfile() or members[0].size > MAX_DOWNLOAD_BYTES:
                raise InstallError("The verified archive has an unexpected executable layout.")
            member = tar.extractfile(members[0])
            if member is None:
                raise InstallError("The verified archive does not contain the expected executable.")
            return member.read()
    except (tarfile.TarError, OSError):
        raise InstallError("The verified release archive could not be read.") from None


def atomic_write(path: Path, data: bytes, executable: bool = False) -> None:
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".privacy-install-", delete=False) as handle:
            temporary = Path(handle.name)
            handle.write(data)
        temporary.chmod(0o700 if executable else 0o600)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def check_version(binary: Path) -> None:
    try:
        result = subprocess.run([str(binary), "version"], capture_output=True, text=True, timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired):
        raise InstallError("The Gitleaks executable could not run on this machine.") from None
    if result.returncode or result.stdout.strip().removeprefix("v") != VERSION:
        raise InstallError("Privacy guard requires exactly Gitleaks 8.30.1; executable version check failed.")


def install_tool(common: Path, args: argparse.Namespace) -> Path:
    tools = common / "privacy-tools"
    if tools.is_symlink():
        raise InstallError("Refused a symlink at the local privacy-tools directory.")
    tools.mkdir(mode=0o700, exist_ok=True)
    binary = tools / "gitleaks"
    if args.gitleaks_bin:
        source = Path(args.gitleaks_bin).expanduser().resolve()
        check_version(source)
        atomic_write(binary, read_limited(source), executable=True)
        print("Using an explicitly trusted local Gitleaks executable (version checked; archive provenance not checked).")
    else:
        key = platform_key()
        filename = f"gitleaks_{VERSION}_{key}.tar.gz"
        cache = tools / filename
        if args.gitleaks_archive:
            archive = read_limited(Path(args.gitleaks_archive).expanduser())
        elif cache.is_file():
            archive = read_limited(cache)
        elif args.offline:
            raise InstallError("No verified release archive is cached. Supply --gitleaks-archive or retry with network access.")
        else:
            archive = download_archive(filename)
        executable = verify_and_extract(archive, key)
        atomic_write(cache, archive)
        atomic_write(binary, executable, executable=True)
    check_version(binary)
    return binary


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ci", action="store_true", help="Install the scanner only; do not configure Git hooks.")
    parser.add_argument("--offline", action="store_true", help="Never download; require a supplied or cached tool.")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--gitleaks-archive", help="Official platform archive, checked against its pinned SHA256.")
    source.add_argument("--gitleaks-bin", help="Explicitly trust an independently audited local Gitleaks 8.30.1 executable.")
    args = parser.parse_args()
    try:
        root, common = repository_paths()
        if not args.ci:
            check_hook_conflicts(root, common)
        binary = install_tool(common, args)
        if not args.ci:
            for hook in ("pre-commit", "pre-push"):
                path = root / ".githooks" / hook
                path.chmod(path.stat().st_mode | 0o111)
            git("config", "--local", "core.hooksPath", ".githooks", cwd=root)
            print("Privacy pre-commit and pre-push hooks installed for this clone.")
        print(f"Gitleaks {VERSION} ready: {binary}")
        return 0
    except (InstallError, OSError) as error:
        message = str(error) if isinstance(error, InstallError) else "Local installation could not be completed; check file permissions."
        print(f"Privacy guard installation failed: {message}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

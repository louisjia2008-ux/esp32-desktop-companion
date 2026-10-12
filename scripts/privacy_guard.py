#!/usr/bin/env python3
"""Fail-closed privacy checks for the Git index and complete pushed ancestry.

Only path, line, rule and object identity are printed. Secret values, snippets
and external scanner output are deliberately never included in diagnostics.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tempfile

GITLEAKS_VERSION = "8.30.1"
MAX_TEXT_BYTES = 8 * 1024 * 1024
POLICY_PATH = ".privacy-policy.json"
ZERO = re.compile(r"^0+$")
OID = re.compile(r"^[0-9a-f]{40}(?:[0-9a-f]{24})?$")
NAME = (r"(?:wifi[_-]?(?:pass(?:word)?|pwd|ssid)|wlan[_-]?(?:password|ssid)|"
        r"ssid|password|passphrase|passwd|pwd|(?:new|saved)[_-]?password|"
        r"pairing|pair(?:ing)?[_-]?(?:key|code|secret)|api[_-]?key|access[_-]?token|"
        r"auth[_-]?token|secret(?:[_-]?key)?|private[_-]?key|"
        r"k[A-Za-z0-9_]*(?:Password|Passphrase|Ssid|PairingCode|PairingKey|ApiKey|Token))_?")
ASSIGNMENT = re.compile(
    r"(?i)(?<![\w])" + NAME + r"\b[\"']?\s*\]?\s*(?:\[[^\]\n]*\]\s*)?"
    r"(?:=(?!=)|:)\s*(?:[bur])?(?P<q>[\"'])(?P<value>[^\r\n]*?)(?P=q)")
BARE_ASSIGNMENT = re.compile(r"(?im)^\s*(?:export\s+)?" + NAME +
                             r"\s*[:=]\s*(?P<value>[^\s#;,]+)\s*(?:[#;].*)?$")
BUILD_FLAG = re.compile(r"(?i)-D" + NAME + r"\s*=\s*\\?[\"']?(?P<value>[^\s\"'\\]+)")
WIFI_BEGIN = re.compile(r'(?i)WiFi\.begin\(\s*"([^"\n]+)"\s*,\s*"([^"\n]+)"')
NVS_WRITE = re.compile(r'(?i)putString\(\s*"(?:' + NAME + r'|pass)"\s*,\s*"([^"\n]+)"')
ENCODED_ASSIGNMENT = re.compile(
    r"(?i)(?<![\w])" + NAME + r"\b[\"']?\s*\]?\s*(?:=(?!=)|:)\s*"
    r"(?:bytes\.fromhex|bytes\.from_hex|base64\.b64decode|unhexlify)"
    r"\(\s*[\"'](?P<value>[^\"'\r\n]+)[\"']")
PRIVATE_PATH = re.compile(r"/(?:Users|home|Volumes)/[^\s\"'`<>),;]+")
PRIVATE_IP = re.compile(r"(?<![\d.])(?:192\.168\.\d{1,3}\.\d{1,3}|"
                        r"10\.\d{1,3}\.\d{1,3}\.\d{1,3}|"
                        r"172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})(?![\d.])")
MAC = re.compile(r"(?i)(?<![\da-f])(?:[\da-f]{2}:){5}[\da-f]{2}(?![\da-f])")
EMAIL = re.compile(r"[\w.+-]+@[\w-]+(?:\.[\w-]+)*\.[A-Za-z]{2,24}\b")
PHONE = re.compile(r"(?<![\w])1[3-9]\d{9}(?![\w])")
TOKEN_URL = re.compile(r"(?i)https?://[^\s\"'<>]+(?:[?&](?:token|api_key|access_token|password|secret)=)[^\s\"'&<>]+")
URL_AUTH = re.compile(r"https?://[^\s/:\"'<>]+:[^\s/@\"'<>]+@[^\s\"'<>]+")
SERIAL = re.compile(r"/dev/(?:cu|tty)\.usb(?:modem|serial)[\w-]+")
KEY_SHAPE = re.compile(r"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|"
                       r"sk-(?:proj-)?[A-Za-z0-9_-]{24,}|AKIA[0-9A-Z]{16}|"
                       r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----)")
PRIVATE_DIRS = {".runtime", "output", "evidence", "backups", "recovery", ".venv",
                ".pio", ".pio-core", ".ralph", ".codex", ".agents", ".aws",
                ".ssh", "__pycache__", "node_modules", ".build"}
PRIVATE_SUFFIXES = {".key", ".pem", ".p12", ".pfx", ".bin", ".elf", ".map",
                    ".log", ".jsonl", ".dwp", ".dwc", ".ah1", ".mp4", ".mov",
                    ".mkv", ".webm", ".wav", ".mp3", ".sqlite", ".db"}
SOURCE_SUFFIXES = {".py", ".cpp", ".hpp", ".h", ".c", ".swift", ".js", ".ts", ".sh"}


class GuardError(Exception):
    """A safe, fixed diagnostic, never an exception from secret-bearing input."""


@dataclass(frozen=True)
class Finding:
    path: str
    rule: str
    line: int = 0
    object_id: str = ""


def git(root: Path, *args: str) -> bytes:
    result = subprocess.run(["git", *args], cwd=root, capture_output=True)
    if result.returncode:
        raise GuardError("Git data unavailable; check repository completeness and retry.")
    return result.stdout


def repository() -> Path:
    return Path(git(Path.cwd(), "rev-parse", "--show-toplevel").decode().strip())


def load_policy(root: Path, staged: bool = False) -> dict:
    try:
        data = git(root, "show", ":" + POLICY_PATH) if staged else (root / POLICY_PATH).read_bytes()
        policy = json.loads(data)
        if policy["version"] != 1:
            raise ValueError()
        for path, digest in policy["binary_assets"].items():
            if not isinstance(path, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise ValueError()
        for entry in policy["fixture_exceptions"]:
            if (not all(isinstance(entry[k], str) for k in ("path", "rule", "sha256", "reason"))
                    or not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) or not entry["reason"]):
                raise ValueError()
        return policy
    except (OSError, ValueError, KeyError, TypeError):
        raise GuardError("Privacy policy is missing or malformed; publication blocked.") from None


def exempt(policy: dict, path: str, rule: str, value: str) -> bool:
    digest = hashlib.sha256(value.encode()).hexdigest()
    return any(e["path"] == path and e["rule"] == rule and e["sha256"] == digest
               for e in policy["fixture_exceptions"])


def placeholder(value: str) -> bool:
    return (not value or value in {"--", "null", "None", "true", "false"}
            or bool(re.fullmatch(r"(?:\$\{[^}]+\}|\$[A-Z_][A-Z0-9_]*|<[^>]+>|YOUR_[A-Z_]+)", value)))


def content_candidates(text: str, path: str = ""):
    """Internal values stay in memory, solely for exact synthetic exemptions."""
    patterns = [ASSIGNMENT, BUILD_FLAG, ENCODED_ASSIGNMENT]
    if PurePosixPath(path).suffix.lower() not in SOURCE_SUFFIXES - {".sh"}:
        patterns.append(BARE_ASSIGNMENT)
    for pattern in patterns:
        for match in pattern.finditer(text):
            value = match.group("value")
            if not placeholder(value):
                yield "credential-literal", value, match.start()
    for pattern in (WIFI_BEGIN, NVS_WRITE):
        for match in pattern.finditer(text):
            for value in match.groups():
                if not placeholder(value):
                    yield "credential-literal", value, match.start()
    rules = [(PRIVATE_PATH, "personal-path"), (PRIVATE_IP, "private-network"),
             (MAC, "device-mac"), (EMAIL, "personal-email"), (PHONE, "personal-phone"),
             (TOKEN_URL, "credential-url"), (URL_AUTH, "credential-url"),
             (SERIAL, "device-serial"), (KEY_SHAPE, "key-shape")]
    for pattern, rule in rules:
        for match in pattern.finditer(text):
            value = match.group()
            if rule == "private-network" and value == "192.168.4.1":
                continue  # The device's fixed public Setup AP address.
            if rule == "device-mac" and value.lower() == "00:00:00:00:00:00":
                continue
            if rule == "personal-email" and re.fullmatch(r"[\w.+-]+@users\.noreply\.github\.com", value):
                continue
            if rule == "device-serial" and value.endswith("XXXX"):
                continue
            yield rule, value, match.start()
    runtime = re.compile(
        r'(?m)^(?:\[[^\]\n]*\]\s*)?(?:\d{4}-\d\d-\d\d[^\n]*?\s)?'
        r'(?:WIFI_NETWORK ip=\d|TOUCH_METRICS samples=\d|HEARTBEAT uptime_ms=\d|'
        r'AUTH_OK claude_companion=|HUB_ACTION_COMPLETED action=|'
        r'WIFI_(?:CONNECTED|CONNECT_FAILED|DISCONNECTED)\b)[^\n]*$')
    for match in runtime.finditer(text):
        yield "runtime-record", match.group(), match.start()
    # Copied Bridge output and renamed key files do not necessarily have a field name.
    pairing_output = re.compile(r'(?im)^Bridge pairing code[^\n]*\n\s*([0-9a-f]{32,64})\s*$')
    for match in pairing_output.finditer(text):
        yield "pairing-material", match.group(1), match.start()
    if PurePosixPath(path).suffix.lower() not in SOURCE_SUFFIXES:
        for match in re.finditer(r'(?im)^\s*([0-9a-f]{32}|[0-9a-f]{64})\s*$', text):
            yield "opaque-secret-material", match.group(1), match.start()
    # Parse complete JSON documents rather than depending on key order or formatting.
    try:
        document = json.loads(text)
    except (ValueError, RecursionError):
        document = None
    pending = [document]
    if document is None and PurePosixPath(path).suffix.lower() not in SOURCE_SUFFIXES:
        for line in text.splitlines():
            if line.lstrip().startswith(("{", "[")):
                try:
                    pending.append(json.loads(line))
                except (ValueError, RecursionError):
                    pass
    while pending:
        value = pending.pop()
        if isinstance(value, dict):
            keys = set(value)
            if ("session_id" in keys and keys & {"timestamp", "messages", "content"}
                    or keys >= {"device_id", "uptime_ms", "wifi_connected"}):
                yield "runtime-record", json.dumps(value, sort_keys=True), 0
            pending.extend(value.values())
        elif isinstance(value, list):
            pending.extend(value)
        elif isinstance(value, str) and re.fullmatch(r"(?i)[0-9a-f]{32}", value):
            yield "opaque-secret-material", value, 0


def scan_blob(path: str, data: bytes, mode: str, policy: dict, object_id: str = "") -> list[Finding]:
    p = PurePosixPath(path)
    if p.is_absolute() or ".." in p.parts or mode not in {"100644", "100755"}:
        return [Finding(path, "unsupported-git-entry", object_id=object_id)]
    lower = path.lower()
    if (any(part.lower() in PRIVATE_DIRS for part in p.parts)
            or p.suffix.lower() in PRIVATE_SUFFIXES
            or (p.name.startswith(".env") and p.name != ".env.example")
            or re.search(r"(?:secrets|credentials).*\.json$|config\.local\.|\.local\.json$", lower)
            or (p.suffix.lower() not in SOURCE_SUFFIXES | {".md"}
                and re.search(r"(?:^|[/_.-])pair(?:ing)?[_.-]?(?:key|code|secret)?(?:[_.-]|$)", lower))
            or lower.startswith("mac_bridge/native/build/")
            or p.name in {".gitleaks.toml", ".gitleaksignore"}):
        return [Finding(path, "private-file", object_id=object_id)]
    if path in policy["binary_assets"]:
        if hashlib.sha256(data).hexdigest() != policy["binary_assets"][path]:
            return [Finding(path, "changed-binary-asset", object_id=object_id)]
        return []
    if len(data) > MAX_TEXT_BYTES:
        return [Finding(path, "oversized-unreviewed-file", object_id=object_id)]
    try:
        text = data.decode("utf-8")
        if "\0" in text:
            raise UnicodeError()
    except UnicodeError:
        return [Finding(path, "unreviewed-binary", object_id=object_id)]
    return [Finding(path, rule, text.count("\n", 0, offset) + 1, object_id)
            for rule, value, offset in content_candidates(text, path)
            if not exempt(policy, path, rule, value)]


def index_entries(root: Path):
    for row in git(root, "ls-files", "--stage", "-z").split(b"\0"):
        if not row:
            continue
        meta, raw_path = row.split(b"\t", 1)
        mode, oid, stage = meta.decode().split()
        if stage != "0":
            raise GuardError("Unmerged index; resolve it before publication.")
        yield raw_path.decode(), mode, oid


def commit_tips(root: Path, args: argparse.Namespace) -> tuple[list[str], list[str]]:
    if args.pre_push:
        candidates = []
        for row in sys.stdin:
            fields = row.split()
            if len(fields) != 4 or not OID.fullmatch(fields[1]) or not OID.fullmatch(fields[3]):
                raise GuardError("Malformed pre-push input; publication blocked.")
            if not ZERO.fullmatch(fields[1]):
                candidates.append(fields[1])
    else:
        candidates = git(root, "for-each-ref", "--format=%(objectname)").decode().splitlines()
        candidates.append(git(root, "rev-parse", "HEAD").decode().strip())
    tips, tags = [], set()
    for candidate in candidates:
        if not OID.fullmatch(candidate):
            raise GuardError("Invalid Git object identity; publication blocked.")
        while git(root, "cat-file", "-t", candidate).strip() == b"tag":
            tags.add(candidate)
            header = git(root, "cat-file", "tag", candidate).split(b"\n", 1)[0]
            if not header.startswith(b"object "):
                raise GuardError("Malformed annotated tag; publication blocked.")
            candidate = header[7:].decode()
            if not OID.fullmatch(candidate):
                raise GuardError("Invalid tag target; publication blocked.")
        # Tags must point to a commit; arbitrary blob/tree tags are rejected.
        if git(root, "cat-file", "-t", candidate).strip() != b"commit":
            raise GuardError("Only commit-backed refs can be checked; publication blocked.")
        tips.append(candidate)
    return sorted(set(tips)), sorted(tags)


def resolve_gitleaks(root: Path) -> Path:
    common = Path(git(root, "rev-parse", "--git-common-dir").decode().strip())
    if not common.is_absolute():
        common = root / common
    configured = os.environ.get("GITLEAKS_BIN")
    candidate = configured or (str(common / "privacy-tools/gitleaks")
                               if (common / "privacy-tools/gitleaks").is_file()
                               else shutil.which("gitleaks"))
    if not candidate:
        raise GuardError("Gitleaks missing. Run python3 scripts/install_privacy_guard.py; publication blocked.")
    binary = Path(candidate).resolve()
    try:
        result = subprocess.run([str(binary), "version"], capture_output=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        raise GuardError("Gitleaks cannot run; publication blocked.") from None
    if result.returncode or result.stdout.decode(errors="replace").strip() != GITLEAKS_VERSION:
        raise GuardError("Pinned Gitleaks 8.30.1 required; reinstall the privacy guard.")
    return binary


def gitleaks(root: Path, binary: Path, objects: list[tuple[str, str, bytes]]) -> list[Finding]:
    with tempfile.TemporaryDirectory(prefix="esp32-privacy-") as temporary:
        temp = Path(temporary)
        tree = temp / "objects"
        tree.mkdir()
        identities = {}
        seen = set()
        for path, object_id, data in objects:
            digest = hashlib.sha256(data).digest()
            if digest in seen or len(data) > MAX_TEXT_BYTES or b"\0" in data:
                continue
            try:
                data.decode("utf-8")
            except UnicodeError:
                continue  # Project rules already reject unreviewed binary objects.
            seen.add(digest)
            # Flat numeric names prevent .gitattributes and path allowlists from hiding data.
            name = str(len(identities)) + ".txt"
            target = tree / name
            target.write_bytes(data)
            target.chmod(0o600)
            identities[name] = (path, object_id)
        command = ["dir", str(tree), "--max-decode-depth=10", "--max-archive-depth=2"]
        report = temp / "report.json"
        env = {k: v for k, v in os.environ.items() if k not in {"GITLEAKS_CONFIG", "GITLEAKS_CONFIG_TOML"}}
        # Config/ignore overrides from the scanned repository are not accepted.
        if any((root / name).exists() for name in (".gitleaks.toml", ".gitleaksignore")):
            raise GuardError("Unreviewed Gitleaks override file; publication blocked.")
        try:
            result = subprocess.run([str(binary), *command, "--redact=100", "--ignore-gitleaks-allow",
                                     "--no-banner", "--log-level=error", "--report-format=json",
                                     "--report-path=" + str(report)], cwd=temp, env=env,
                                    capture_output=True, timeout=180)
            records = json.loads(report.read_bytes())
            if result.returncode not in {0, 1} or not isinstance(records, list):
                raise ValueError()
            if result.returncode == 1 and not records:
                raise ValueError()
        except (OSError, ValueError, subprocess.TimeoutExpired):
            raise GuardError("Gitleaks scan failed; no external output displayed; publication blocked.") from None
        findings = []
        for record in records:
            source = identities.get(Path(str(record.get("File", ""))).name)
            if source is None:
                raise GuardError("Unmapped scanner result; publication blocked.")
            path, object_id = source
            findings.append(Finding(path, "gitleaks:" + str(record.get("RuleID", "secret")),
                                    int(record.get("StartLine", 0)), object_id))
        return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--staged", action="store_true")
    mode.add_argument("--history", action="store_true")
    mode.add_argument("--pre-push", action="store_true")
    args = parser.parse_args()
    try:
        root = repository()
        policy = load_policy(root, args.staged)
        findings = []
        staged = list(index_entries(root)) if args.staged else []
        tips, tags = (None, []) if args.staged else commit_tips(root, args)
        if tips == []:  # A push containing only ref deletions exposes no objects.
            print("Privacy guard PASS: ref deletions only.")
            return 0
        if tips is not None and git(root, "rev-parse", "--is-shallow-repository").strip() != b"false":
            raise GuardError("Shallow history cannot be checked completely; fetch full history before pushing.")
        binary = resolve_gitleaks(root)
        checked, objects = set(), []
        entries = [("index", staged)] if tips is None else []
        if tips is not None:
            commits = git(root, "rev-list", *tips, "--").decode().splitlines()
            for commit in commits:
                data = git(root, "cat-file", "commit", commit)
                path = "@git/commit/" + commit + ".txt"
                findings.extend(scan_blob(path, data, "100644", policy, commit))
                objects.append((path, commit, data))
                tree = []
                for row in git(root, "ls-tree", "-rz", commit).split(b"\0"):
                    if row:
                        meta, path = row.split(b"\t", 1)
                        file_mode, kind, oid = meta.decode().split()
                        tree.append((path.decode(), file_mode, oid))
                entries.append((commit, tree))
            for tag in tags:
                data = git(root, "cat-file", "tag", tag)
                path = "@git/tag/" + tag + ".txt"
                findings.extend(scan_blob(path, data, "100644", policy, tag))
                objects.append((path, tag, data))
        for commit, tree in entries:
            for path, file_mode, oid in tree:
                identity = (path, file_mode, oid)
                if identity in checked:
                    continue
                checked.add(identity)
                data = git(root, "cat-file", "blob", oid) if file_mode in {"100644", "100755"} else b""
                findings.extend(scan_blob(path, data, file_mode, policy, commit))
                if file_mode in {"100644", "100755"}:
                    objects.append((path, commit, data))
        findings.extend(gitleaks(root, binary, objects))
        if findings:
            print("Privacy guard BLOCKED: remove private content; do not bypass the hook.", file=sys.stderr)
            for finding in sorted(set(findings), key=lambda v: (v.path, v.line, v.rule, v.object_id)):
                print(json.dumps(finding.__dict__, ensure_ascii=True), file=sys.stderr)
            return 1
        print(f"Privacy guard PASS: {len(checked)} Git file versions; Gitleaks and project rules clean.")
        return 0
    except GuardError as error:
        print("Privacy guard BLOCKED: " + str(error), file=sys.stderr)
        return 2
    except Exception:
        print("Privacy guard BLOCKED: unexpected scan error; no input or exception details displayed.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

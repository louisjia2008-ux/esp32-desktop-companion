#!/usr/bin/env python3
"""Preview or atomically merge the ESP32 observer into user Codex hooks.

Existing hook handlers and config.toml (including notify) are preserved.
"""
from __future__ import annotations

import argparse
import copy
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shlex
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "mac_bridge"))
from codex_hook import hooks_settings


def merge_settings(existing: dict, hook_path: Path) -> dict:
    if not isinstance(existing, dict) or not isinstance(existing.get("hooks", {}), dict):
        raise ValueError("Existing hooks must be a JSON object")
    result = copy.deepcopy(existing)
    hooks = result.setdefault("hooks", {})
    fragment = hooks_settings(hook_path)
    for event, additions in fragment["hooks"].items():
        groups = hooks.get(event, [])
        if not isinstance(groups, list):
            raise ValueError("Existing hook event must contain matcher groups")
        retained = []
        for group in groups:
            if not isinstance(group, dict) or not isinstance(group.get("hooks"), list):
                raise ValueError("Existing matcher group is invalid")
            handlers = []
            for handler in group["hooks"]:
                command = handler.get("command", "") if isinstance(handler, dict) else ""
                try: args = shlex.split(command)
                except (ValueError, TypeError): args = []
                owned = isinstance(handler, dict) and handler.get("type") == "command" and len(args) == 2 and args[1] == str(hook_path)
                if not owned: handlers.append(handler)
            if handlers:
                retained.append(dict(group, hooks=handlers))
        hooks[event] = retained + additions
    return result


def install(target: Path, hook_path: Path) -> dict:
    if target.is_symlink():
        raise ValueError("Refusing a symlinked hook configuration")
    raw = target.read_bytes() if target.exists() else None
    existing = json.loads(raw) if raw is not None else {}
    merged = merge_settings(existing, hook_path)
    if merged == existing:
        return {"changed": False, "target": str(target)}
    target.parent.mkdir(parents=True, exist_ok=True)
    backup = None
    if raw is not None:
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        backup = target.with_name(target.name + ".backup-" + stamp)
        fd = os.open(backup, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as f: f.write(raw)
    fd, temporary = tempfile.mkstemp(prefix=".esp32-hooks-", dir=str(target.parent))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            json.dump(merged, f, indent=2, ensure_ascii=False)
            f.write("\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(temporary, target)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)
    return {"changed": True, "target": str(target), "backup": str(backup) if backup else None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--target", type=Path, default=Path.home() / ".codex/hooks.json")
    args = parser.parse_args()
    hook = ROOT / "mac_bridge/codex_hook.py"
    if args.apply:
        print(json.dumps(install(args.target, hook), ensure_ascii=False))
    else:
        print(json.dumps(hooks_settings(hook), indent=2, ensure_ascii=False))


if __name__ == "__main__": main()

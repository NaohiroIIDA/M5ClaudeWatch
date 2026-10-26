#!/usr/bin/env python3
"""Merge M5ClaudeWatch hooks into a Claude Code settings.json (default: ~/.claude/settings.json).

    python3 install-hooks.py            # user settings (all projects)
    python3 install-hooks.py --uninstall
    python3 install-hooks.py --settings .claude/settings.local.json
"""

import argparse
import json
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parent
HOOK = ROOT / "host" / "m5claudewatch_hook.py"
TEMPLATE = ROOT / "hooks" / "settings.example.json"


HOOK_NAMES = ("m5claudewatch_hook.py", "ccwatch_hook.py")  # include the pre-rename name


def is_ours(group):
    return any(name in h.get("command", "") for h in group.get("hooks", []) for name in HOOK_NAMES)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--settings", default=str(Path.home() / ".claude" / "settings.json"))
    ap.add_argument("--uninstall", action="store_true")
    args = ap.parse_args()

    path = Path(args.settings).expanduser()
    settings = json.loads(path.read_text()) if path.exists() else {}
    if path.exists():
        shutil.copy(path, path.with_suffix(path.suffix + ".bak"))

    hooks = settings.setdefault("hooks", {})
    for event in list(hooks):
        hooks[event] = [g for g in hooks[event] if not is_ours(g)]
        if not hooks[event]:
            del hooks[event]

    if not args.uninstall:
        template = json.loads(TEMPLATE.read_text().replace("__HOOK__", str(HOOK)))
        for event, groups in template["hooks"].items():
            hooks.setdefault(event, []).extend(groups)
    if not hooks:
        del settings["hooks"]

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n")
    print(("removed from " if args.uninstall else "installed into ") + str(path))


if __name__ == "__main__":
    main()

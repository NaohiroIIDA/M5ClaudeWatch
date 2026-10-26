#!/usr/bin/env python3
"""Claude Code hook that forwards events to the CC Watch bridge.

Register it for UserPromptSubmit, Stop, Notification, PermissionRequest and
SessionEnd (see hooks/settings.example.json). Stdlib only, Python 3.9+.
If the bridge is not running, every event is a silent no-op so Claude Code
behaves exactly as without the watch.
"""

import json
import os
import socket
import sys

SOCKET_PATH = os.environ.get("CCWATCH_SOCKET", os.path.expanduser("~/.claude/ccwatch.sock"))
PERM_WAIT = float(os.environ.get("CCWATCH_PERM_TIMEOUT", "110")) + 5
DETAIL_MAX = 400
MSG_MAX = 300


def clip(text, limit):
    text = str(text).strip()
    return text if len(text) <= limit else text[: limit - 1] + "…"


def describe_tool(name, tool_input):
    """One readable line (or a few) describing what the tool wants to do."""
    if not isinstance(tool_input, dict):
        return clip(tool_input, DETAIL_MAX)
    if name == "Bash":
        return clip(tool_input.get("command", ""), DETAIL_MAX)
    if name in ("Edit", "MultiEdit", "Write", "Read", "NotebookEdit"):
        path = tool_input.get("file_path") or tool_input.get("notebook_path", "")
        return clip(os.path.relpath(path) if path.startswith(os.getcwd()) else path, DETAIL_MAX)
    if name in ("WebFetch",):
        return clip(tool_input.get("url", ""), DETAIL_MAX)
    if name in ("WebSearch",):
        return clip(tool_input.get("query", ""), DETAIL_MAX)
    if name == "ExitPlanMode":
        return clip(tool_input.get("plan", "プランを承認しますか?"), DETAIL_MAX)
    if name == "AskUserQuestion":
        qs = tool_input.get("questions") or []
        return clip("\n".join(q.get("question", "") for q in qs if isinstance(q, dict)), DETAIL_MAX)
    for key in ("command", "file_path", "path", "url", "pattern", "description", "prompt"):
        if key in tool_input:
            return clip(tool_input[key], DETAIL_MAX)
    return clip(json.dumps(tool_input, ensure_ascii=False), DETAIL_MAX)


def build_request(data):
    event = data.get("hook_event_name", "")
    base = {
        "sid": data.get("session_id", ""),
        "proj": os.path.basename(data.get("cwd") or os.getcwd()),
    }
    if event == "UserPromptSubmit":
        return dict(base, ev="busy")
    if event == "Stop":
        return dict(base, ev="done", msg=clip(data.get("last_assistant_message") or "", MSG_MAX))
    if event == "Notification":
        kind = data.get("notification_type", "")
        if kind == "permission_prompt":
            return None  # handled by PermissionRequest
        if kind in ("idle_prompt", "elicitation_dialog", "agent_needs_input"):
            return dict(base, ev="notify", msg=clip(data.get("message", "入力を待っています"), MSG_MAX))
        return None
    if event == "PermissionRequest":
        tool = data.get("tool_name", "")
        return dict(base, ev="perm", tool=tool, detail=describe_tool(tool, data.get("tool_input")))
    if event == "SessionEnd":
        return dict(base, ev="end")
    return None


def talk(request, wait):
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.settimeout(1.0)
    try:
        sock.connect(SOCKET_PATH)
        sock.sendall((json.dumps(request, ensure_ascii=False) + "\n").encode())
        if not wait:
            return None
        sock.settimeout(wait)
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        return json.loads(buf or b"{}")
    finally:
        sock.close()


def main():
    try:
        data = json.load(sys.stdin)
    except ValueError:
        return
    request = build_request(data)
    if request is None:
        return
    try:
        reply = talk(request, PERM_WAIT if request["ev"] == "perm" else 0)
    except (OSError, ValueError):
        return  # bridge not running: no-op
    if request["ev"] != "perm" or not reply:
        return

    decision = reply.get("decision")
    if decision == "allow":
        out = {"behavior": "allow"}
    elif decision == "deny":
        out = {"behavior": "deny", "message": "ユーザーがCC Watchで拒否しました。指示を待ってください。",
               "interrupt": True}
    else:
        return  # no decision: Claude Code shows its normal prompt
    print(json.dumps({"hookSpecificOutput": {"hookEventName": "PermissionRequest", "decision": out}},
                     ensure_ascii=False))


if __name__ == "__main__":
    main()

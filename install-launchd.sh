#!/bin/sh
# Run the M5ClaudeWatch bridge at login via launchd.
#   ./install-launchd.sh                      # BLE
#   ./install-launchd.sh --serial /dev/cu.usbmodem1101
#   ./install-launchd.sh --uninstall
set -eu

LABEL=com.m5claudewatch.bridge
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/m5claudewatch-bridge.log"
ROOT=$(cd "$(dirname "$0")" && pwd)

launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
# Remove the agent installed before the rename to M5ClaudeWatch.
LEGACY=com.ccwatch.bridge
launchctl bootout "gui/$(id -u)/$LEGACY" 2>/dev/null || true
rm -f "$HOME/Library/LaunchAgents/$LEGACY.plist"
if [ "${1:-}" = "--uninstall" ]; then
    rm -f "$PLIST"
    echo "removed $LABEL"
    exit 0
fi

UV=$(command -v uv) || { echo "uv not found in PATH" >&2; exit 1; }
EXTRA=""
for arg in "$@"; do EXTRA="$EXTRA
        <string>$arg</string>"; done

mkdir -p "$HOME/Library/LaunchAgents"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$UV</string>
        <string>run</string>
        <string>$ROOT/host/m5claudewatch_bridge.py</string>$EXTRA
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>ThrottleInterval</key>
    <integer>10</integer>
    <key>StandardOutPath</key>
    <string>$LOG</string>
    <key>StandardErrorPath</key>
    <string>$LOG</string>
</dict>
</plist>
EOF

launchctl bootstrap "gui/$(id -u)" "$PLIST"
echo "installed $LABEL (log: $LOG)"

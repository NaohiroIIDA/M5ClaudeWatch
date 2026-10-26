# M5ClaudeWatch

M5Stack StopWatch を Claude Code の手元コンパニオンにするツール。
[GOROman/vibewatch](https://github.com/GOROman/vibewatch) に触発されて作ったもので、Claude Code の hooks に直結しています。

- **作業中**: 青いリングが回転し、経過時間をストップウォッチ表示
- **完了**: 振動と効果音で通知し、所要時間と Claude の最後のメッセージを表示
- **入力待ち**: Claude が入力を待っているときに通知
- **承認リクエスト**: ツール名と内容（コマンドやファイル名）を表示し、**OK / NG** をボタンまたはタッチで返す
  - 左ボタン（BtnA）または画面の NG → 拒否（Claude は中断して指示待ちになる）
  - 右ボタン（BtnB）または画面の OK → 許可
  - どちらかを長押し → 時計では答えず、PC のいつものダイアログで確認
  - 未回答のあいだは 30 秒ごとに再通知

```
Claude Code ──hooks──▶ m5claudewatch_hook.py ──Unix socket──▶ m5claudewatch_bridge.py ──BLE / USB──▶ StopWatch
                      (標準ライブラリのみ)               (常駐、bleak)                      (firmware/)
```

## 構成

| パス | 内容 |
|---|---|
| `firmware/` | StopWatch 用ファームウェア（PlatformIO / M5Unified / NimBLE） |
| `host/m5claudewatch_bridge.py` | Mac で常駐するブリッジ。時計と BLE（またはシリアル）でつながる |
| `host/m5claudewatch_hook.py` | Claude Code の hook から呼ばれるスクリプト |
| `hooks/settings.example.json` | hooks の設定テンプレート |
| `install-hooks.py` | hooks を `~/.claude/settings.json` にマージする |
| `install-launchd.sh` | ブリッジをログイン時に自動起動する |

## セットアップ

### 1. ファームウェアを書き込む

StopWatch を USB-C で接続します。

```sh
cd firmware
pio run -e m5stack-stopwatch --target upload
```

### 2. ブリッジを起動する

```sh
uv run host/m5claudewatch_bridge.py          # BLE。"M5ClaudeWatch" を自動で見つけて接続
# または USB 接続のまま使う場合
uv run host/m5claudewatch_bridge.py --serial /dev/cu.usbmodemXXXX
```

初回だけ、ターミナルアプリへの Bluetooth アクセス許可を macOS が求めます。
接続した時計のアドレスは `~/.config/m5claudewatch/config.json` に保存され、以降はその時計にだけ接続します。

### 3. hooks を登録する

```sh
python3 install-hooks.py               # ~/.claude/settings.json に追加（バックアップ: settings.json.bak）
python3 install-hooks.py --uninstall   # 削除
```

登録後に起動した Claude Code セッションから有効になります。

### 4. ブリッジをログイン時に自動起動する（任意）

```sh
./install-launchd.sh                   # BLE（--serial PORT も渡せる）
./install-launchd.sh --uninstall       # 解除
tail -f ~/Library/Logs/m5claudewatch-bridge.log
```

launchd から起動した場合、ターミナルとは別に Bluetooth の許可が必要になることがあります。ログに `watch connected` が出ないときは「システム設定 > プライバシーとセキュリティ > Bluetooth」を確認してください。
**ブリッジが止まっている、または時計が未接続のときは hook は何もしない**ため、Claude Code はいつもどおり動きます。

## 設定

| 環境変数 | 既定値 | 意味 |
|---|---|---|
| `M5CLAUDEWATCH_PERM_TIMEOUT` | `110` | 時計の回答を待つ秒数。過ぎると PC のダイアログに切り替える（hook の timeout 120 秒より短くすること） |
| `M5CLAUDEWATCH_SOCKET` | `~/.claude/m5claudewatch.sock` | ブリッジのソケットのパス |

## 通信プロトコル

改行区切りの JSON を Nordic UART 互換の GATT サービス（`6E400001-…`）または USB シリアル（115200bps）で送ります。
詳細は [firmware/src/main.cpp](firmware/src/main.cpp) の先頭にあります。

## 注意

- 承認待ちのあいだ、PC 側のダイアログが同時に出るかどうかは Claude Code のバージョンによって異なる可能性があります。時計で答えられないときは長押しで PC 側に回してください。
- BLE は現状ペアリング（暗号化）なしです。ブリッジは保存したアドレスの時計にしか接続しませんが、より厳密にしたい場合は USB シリアルで使ってください。

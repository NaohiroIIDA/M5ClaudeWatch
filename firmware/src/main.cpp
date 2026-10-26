// M5ClaudeWatch: Claude Code companion for M5Stack StopWatch.
//
// The host bridge (host/m5claudewatch_bridge.py) talks to this firmware with
// newline-delimited JSON over BLE (Nordic UART compatible service) or USB
// serial. Host -> device messages:
//   {"t":"busy","proj":"app","el":12,"n":1}     Claude is working (el = elapsed s)
//   {"t":"done","proj":"app","msg":"...","el":80,"n":0}
//   {"t":"perm","id":"3","proj":"app","tool":"Bash","detail":"rm -rf x"}
//   {"t":"cancel","id":"3"}                       request resolved elsewhere
//   {"t":"notify","proj":"app","msg":"..."}      Claude waits for input
//   {"t":"idle"} / {"t":"reset"} / {"t":"ping"}
// Device -> host messages:
//   {"t":"hello","fw":"0.1.0"}
//   {"t":"ans","id":"3","ok":true}  or  {"t":"ans","id":"3","skip":true}
//   {"t":"pong"}

#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <NimBLEDevice.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

constexpr char kDeviceName[] = "M5ClaudeWatch";
constexpr char kFirmwareVersion[] = "0.1.0";
constexpr char kServiceUuid[] = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char kRxUuid[] = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
constexpr char kTxUuid[] = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

constexpr int kSize = 466;
constexpr int kCx = kSize / 2;
constexpr int kCy = kSize / 2;
constexpr std::size_t kNotifyChunk = 20;  // safe for the default ATT MTU
constexpr std::size_t kMaxLine = 2048;
constexpr std::size_t kMaxPerms = 8;

constexpr std::uint8_t kBrightActive = 120;
constexpr std::uint8_t kBrightDim = 12;
constexpr std::uint32_t kDimAfterMs = 45000;
constexpr std::uint32_t kRemindEveryMs = 30000;
constexpr std::uint32_t kFeedbackMs = 650;
constexpr std::uint32_t kBatteryPeriodMs = 30000;
constexpr bool kSoundEnabled = false;  // haptics only

// Touch / layout rectangles for the approval screen.
struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const {
        return px >= x - 8 && px < x + w + 8 && py >= y - 8 && py < y + h + 8;
    }
};
constexpr Rect kNgRect{70, 322, 158, 72};
constexpr Rect kOkRect{238, 322, 158, 72};

enum class Mode { Idle, Working, Done, Notice };

struct PermRequest {
    std::string id;
    std::string project;
    std::string tool;
    std::string detail;
};

struct Pulse {
    std::uint8_t strength;
    std::uint16_t onMs;
    std::uint16_t offMs;
};

struct Note {
    float frequency;  // 0 = rest
    std::uint16_t durationMs;
};

M5Canvas canvas(&M5.Display);
QueueHandle_t g_rxQueue = nullptr;
NimBLECharacteristic* g_tx = nullptr;
volatile bool g_bleConnected = false;
std::string g_bleLine;     // only touched from the NimBLE task
std::string g_serialLine;  // only touched from loop()

Mode g_mode = Mode::Idle;
std::string g_project;
std::string g_message;
std::uint32_t g_workStartedAt = 0;
std::uint32_t g_doneDurationMs = 0;
int g_othersWorking = 0;

std::deque<PermRequest> g_perms;
std::uint32_t g_lastRemindAt = 0;

bool g_feedbackOk = false;
std::uint32_t g_feedbackUntil = 0;

bool g_dirty = true;
bool g_dimmed = false;
std::uint32_t g_lastActivityAt = 0;
std::uint32_t g_lastFrameAt = 0;
int g_batteryLevel = -1;
bool g_charging = false;
std::uint32_t g_lastBatteryAt = 0;

std::vector<Pulse> g_vibeSeq;
std::size_t g_vibeIndex = 0;
bool g_vibeOn = false;
std::uint32_t g_vibeNextAt = 0;

std::vector<Note> g_melody;
std::size_t g_melodyIndex = 0;
std::uint32_t g_melodyNextAt = 0;

std::uint16_t rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return lgfx::color565(r, g, b);
}

const std::uint16_t kColBg = rgb(0, 0, 0);
const std::uint16_t kColText = rgb(235, 235, 235);
const std::uint16_t kColSub = rgb(140, 140, 150);
const std::uint16_t kColClaude = rgb(217, 119, 87);  // Claude orange
const std::uint16_t kColWork = rgb(80, 170, 255);
const std::uint16_t kColDone = rgb(70, 210, 120);
const std::uint16_t kColAsk = rgb(255, 170, 40);
const std::uint16_t kColNg = rgb(255, 120, 40);
const std::uint16_t kColOk = rgb(40, 130, 255);
const std::uint16_t kColPanel = rgb(28, 28, 34);

// ---------------------------------------------------------------------------
// Haptics and sound (non-blocking schedulers driven from loop())

void vibrate(std::initializer_list<Pulse> pattern) {
    g_vibeSeq.assign(pattern.begin(), pattern.end());
    g_vibeIndex = 0;
    g_vibeOn = false;
    g_vibeNextAt = millis();
}

void play(std::initializer_list<Note> notes) {
    if (!kSoundEnabled) return;
    g_melody.assign(notes.begin(), notes.end());
    g_melodyIndex = 0;
    g_melodyNextAt = millis();
}

void updateHaptics(std::uint32_t now) {
    if (!g_vibeSeq.empty() && static_cast<std::int32_t>(now - g_vibeNextAt) >= 0) {
        if (g_vibeOn) {
            M5.Power.setVibration(0);
            g_vibeOn = false;
            g_vibeNextAt = now + g_vibeSeq[g_vibeIndex].offMs;
            ++g_vibeIndex;
        } else if (g_vibeIndex < g_vibeSeq.size()) {
            M5.Power.setVibration(g_vibeSeq[g_vibeIndex].strength);
            g_vibeOn = true;
            g_vibeNextAt = now + g_vibeSeq[g_vibeIndex].onMs;
        } else {
            g_vibeSeq.clear();
        }
    }
    if (!g_melody.empty() && static_cast<std::int32_t>(now - g_melodyNextAt) >= 0) {
        if (g_melodyIndex < g_melody.size()) {
            const Note& note = g_melody[g_melodyIndex++];
            if (note.frequency > 0) {
                M5.Speaker.tone(note.frequency, note.durationMs);
            }
            g_melodyNextAt = now + note.durationMs;
        } else {
            g_melody.clear();
        }
    }
}

void feedbackPermArrived() {
    vibrate({{220, 120, 90}, {220, 120, 0}});
    play({{1320, 70}, {0, 40}, {1320, 70}});
}

void feedbackDone() {
    vibrate({{200, 250, 120}, {200, 90, 0}});
    play({{784, 90}, {1046, 90}, {1318, 160}});
}

void feedbackNotice() {
    vibrate({{160, 160, 0}});
    play({{988, 120}});
}

// ---------------------------------------------------------------------------
// Host link

void sendLine(const std::string& line) {
    const std::string payload = line + "\n";
    if (g_bleConnected && g_tx != nullptr) {
        for (std::size_t off = 0; off < payload.size(); off += kNotifyChunk) {
            const std::size_t n = std::min(kNotifyChunk, payload.size() - off);
            // Pass the chunk to notify() directly: setValue()+notify() lets a
            // later chunk overwrite an earlier one before it is sent.
            g_tx->notify(reinterpret_cast<const std::uint8_t*>(payload.data() + off), n);
        }
    }
    if (Serial) {
        Serial.print(payload.c_str());
    }
}

void sendJson(const JsonDocument& doc) {
    std::string line;
    serializeJson(doc, line);
    sendLine(line);
}

void sendHello() {
    JsonDocument doc;
    doc["t"] = "hello";
    doc["fw"] = kFirmwareVersion;
    sendJson(doc);
}

void enqueueLine(const std::string& line) {
    if (line.empty()) {
        return;
    }
    char* copy = strdup(line.c_str());
    if (copy != nullptr && xQueueSend(g_rxQueue, &copy, 0) != pdTRUE) {
        free(copy);
    }
}

class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, NimBLEConnInfo&) override {
        g_bleConnected = true;
        enqueueLine("{\"t\":\"_link\"}");
    }
    void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
        g_bleConnected = false;
        g_bleLine.clear();
        enqueueLine("{\"t\":\"_link\"}");
        NimBLEDevice::startAdvertising();
    }
};

class RxCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
        const std::string chunk = characteristic->getValue();
        for (char c : chunk) {
            if (c == '\n') {
                enqueueLine(g_bleLine);
                g_bleLine.clear();
            } else if (g_bleLine.size() < kMaxLine) {
                g_bleLine.push_back(c);
            }
        }
    }
};

ServerCallbacks g_serverCallbacks;
RxCallbacks g_rxCallbacks;

void initBle() {
    NimBLEDevice::init(kDeviceName);
    NimBLEDevice::setMTU(247);
    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(&g_serverCallbacks);
    NimBLEService* service = server->createService(kServiceUuid);
    g_tx = service->createCharacteristic(kTxUuid, NIMBLE_PROPERTY::NOTIFY);
    NimBLECharacteristic* rx = service->createCharacteristic(
        kRxUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    rx->setCallbacks(&g_rxCallbacks);
    service->start();

    // Flags + 128-bit UUID + the 13-char name exceed the 31-byte advertising
    // payload, so the name goes in the scan response.
    NimBLEAdvertisementData advData;
    advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    advData.addServiceUUID(NimBLEUUID(kServiceUuid));
    NimBLEAdvertisementData scanData;
    scanData.setName(kDeviceName);
    NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
    advertising->setAdvertisementData(advData);
    advertising->setScanResponseData(scanData);
    advertising->start();
}

// ---------------------------------------------------------------------------
// State changes

void wake() {
    g_lastActivityAt = millis();
    if (g_dimmed) {
        g_dimmed = false;
        M5.Display.setBrightness(kBrightActive);
    }
    g_dirty = true;
}

void answerPerm(bool ok, bool skip) {
    if (g_perms.empty()) {
        return;
    }
    JsonDocument doc;
    doc["t"] = "ans";
    doc["id"] = g_perms.front().id;
    if (skip) {
        doc["skip"] = true;
    } else {
        doc["ok"] = ok;
    }
    sendJson(doc);
    g_perms.pop_front();
    g_lastRemindAt = millis();

    if (skip) {
        vibrate({{90, 40, 0}});
        play({{660, 60}});
    } else {
        g_feedbackOk = ok;
        g_feedbackUntil = millis() + kFeedbackMs;
        if (ok) {
            vibrate({{150, 45, 0}});
            play({{880, 50}, {1320, 80}});
        } else {
            vibrate({{255, 110, 0}});
            play({{660, 60}, {440, 110}});
        }
    }
    wake();
}

void handleMessage(const char* line) {
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) {
        return;
    }
    const char* type = doc["t"] | "";
    const std::uint32_t now = millis();
    if (doc["n"].is<int>()) {
        g_othersWorking = std::max(0, doc["n"].as<int>());
    }

    if (std::strcmp(type, "_link") == 0) {
        if (g_bleConnected) {
            sendHello();
        }
        g_dirty = true;
        return;
    }
    if (std::strcmp(type, "ping") == 0) {
        sendLine("{\"t\":\"pong\"}");
        return;
    }
    if (std::strcmp(type, "busy") == 0) {
        const std::string project = doc["proj"] | "";
        if (g_mode != Mode::Working || project != g_project || doc["el"].is<int>()) {
            g_workStartedAt = now - static_cast<std::uint32_t>(doc["el"] | 0) * 1000U;
        }
        g_mode = Mode::Working;
        g_project = project;
        wake();
        return;
    }
    if (std::strcmp(type, "done") == 0) {
        g_doneDurationMs = doc["el"].is<int>()
                               ? static_cast<std::uint32_t>(doc["el"].as<int>()) * 1000U
                               : (g_mode == Mode::Working ? now - g_workStartedAt : 0);
        g_mode = Mode::Done;
        g_project = doc["proj"] | "";
        g_message = doc["msg"] | "";
        if (!(doc["quiet"] | false)) {
            feedbackDone();
        }
        wake();
        return;
    }
    if (std::strcmp(type, "notify") == 0) {
        g_mode = Mode::Notice;
        g_project = doc["proj"] | "";
        g_message = doc["msg"] | "";
        if (!(doc["quiet"] | false)) {
            feedbackNotice();
        }
        wake();
        return;
    }
    if (std::strcmp(type, "perm") == 0) {
        PermRequest req;
        req.id = doc["id"] | "";
        req.project = doc["proj"] | "";
        req.tool = doc["tool"] | "";
        req.detail = doc["detail"] | "";
        const bool exists = std::any_of(g_perms.begin(), g_perms.end(),
                                        [&](const PermRequest& p) { return p.id == req.id; });
        if (!exists && !req.id.empty() && g_perms.size() < kMaxPerms) {
            g_perms.push_back(req);
            g_lastRemindAt = now;
            if (!(doc["quiet"] | false)) {
                feedbackPermArrived();
            }
        }
        wake();
        return;
    }
    if (std::strcmp(type, "cancel") == 0) {
        const std::string id = doc["id"] | "";
        g_perms.erase(std::remove_if(g_perms.begin(), g_perms.end(),
                                     [&](const PermRequest& p) { return p.id == id; }),
                      g_perms.end());
        g_dirty = true;
        return;
    }
    if (std::strcmp(type, "idle") == 0) {
        g_mode = Mode::Idle;
        g_dirty = true;
        return;
    }
    if (std::strcmp(type, "reset") == 0) {
        g_perms.clear();
        g_mode = Mode::Idle;
        g_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Rendering

std::size_t utf8Length(unsigned char lead) {
    if (lead < 0x80) return 1;
    if ((lead >> 5) == 0x6) return 2;
    if ((lead >> 4) == 0xE) return 3;
    if ((lead >> 3) == 0x1E) return 4;
    return 1;
}

// Greedy wrap by rendered width using the canvas' current font.
std::vector<std::string> wrapText(const std::string& text, int maxWidth, std::size_t maxLines) {
    std::vector<std::string> lines;
    std::string line;
    bool truncated = false;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t len = std::min(utf8Length(text[i]), text.size() - i);
        const std::string ch = text.substr(i, len);
        i += len;
        if (ch == "\r") continue;
        if (ch == "\n" || canvas.textWidth((line + ch).c_str()) > maxWidth) {
            lines.push_back(line);
            line = (ch == "\n" || ch == " ") ? "" : ch;
            if (lines.size() == maxLines) {
                truncated = true;
                break;
            }
            continue;
        }
        line += ch;
    }
    if (!truncated && !line.empty()) {
        lines.push_back(line);
    }
    if (truncated && !lines.empty()) {
        std::string& last = lines.back();
        while (!last.empty() && canvas.textWidth((last + "…").c_str()) > maxWidth) {
            std::size_t cut = last.size() - 1;
            while (cut > 0 && (static_cast<unsigned char>(last[cut]) & 0xC0) == 0x80) --cut;
            last.erase(cut);
        }
        last += "…";
    }
    return lines;
}

void drawWrapped(const std::string& text, int y, int maxWidth, std::size_t maxLines,
                 int lineHeight, std::uint16_t color) {
    canvas.setTextColor(color);
    canvas.setTextDatum(top_center);
    const auto lines = wrapText(text, maxWidth, maxLines);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        canvas.drawString(lines[i].c_str(), kCx, y + static_cast<int>(i) * lineHeight);
    }
}

std::string formatDuration(std::uint32_t ms) {
    const std::uint32_t total = ms / 1000;
    char buf[16];
    if (total >= 3600) {
        std::snprintf(buf, sizeof(buf), "%u:%02u:%02u", total / 3600, (total / 60) % 60, total % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%02u:%02u", total / 60, total % 60);
    }
    return buf;
}

void drawLabel(const char* text, int y, std::uint16_t color, const lgfx::IFont* font) {
    canvas.setFont(font);
    canvas.setTextColor(color);
    canvas.setTextDatum(middle_center);
    canvas.drawString(text, kCx, y);
}

void drawStatusBar() {
    canvas.setFont(&fonts::efontJA_14);
    canvas.setTextDatum(middle_center);
    char buf[48];
    const char* link = g_bleConnected ? "BLE" : (Serial ? "USB" : "未接続");
    if (g_batteryLevel >= 0) {
        std::snprintf(buf, sizeof(buf), "%s  %d%%%s", link, g_batteryLevel, g_charging ? "+" : "");
    } else {
        std::snprintf(buf, sizeof(buf), "%s", link);
    }
    canvas.setTextColor(g_bleConnected || Serial ? kColSub : kColNg);
    canvas.drawString(buf, kCx, 440);
}

void drawOthersWorking(int y) {
    const int others = g_mode == Mode::Working ? g_othersWorking - 1 : g_othersWorking;
    if (others <= 0) return;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "他 %d セッション実行中", others);
    drawLabel(buf, y, kColWork, &fonts::efontJA_16);
}

void drawIdle() {
    canvas.drawCircle(kCx, kCy, 228, rgb(40, 40, 46));
    drawLabel("Claude Code", kCy - 20, kColClaude, &fonts::FreeSansBold18pt7b);
    drawLabel(g_bleConnected || Serial ? "待機中" : "ブリッジ未接続", kCy + 30, kColSub,
              &fonts::lgfxJapanGothicP_20);
    drawOthersWorking(kCy + 80);
}

void drawWorking(std::uint32_t now) {
    const float t = static_cast<float>(now % 1600) / 1600.0f;
    const int start = static_cast<int>(t * 360.0f);
    canvas.fillArc(kCx, kCy, 222, 230, 0, 360, rgb(20, 40, 70));
    canvas.fillArc(kCx, kCy, 220, 232, start, start + 70, kColWork);

    drawLabel("WORKING", 96, kColWork, &fonts::FreeSansBold18pt7b);
    drawLabel(g_project.c_str(), 140, kColSub, &fonts::lgfxJapanGothicP_20);

    const std::string elapsed = formatDuration(now - g_workStartedAt);
    canvas.setFont(&fonts::Font7);
    canvas.setTextSize(elapsed.size() > 5 ? 1.3f : 1.8f);
    canvas.setTextColor(kColText);
    canvas.setTextDatum(middle_center);
    canvas.drawString(elapsed.c_str(), kCx, kCy + 20);
    canvas.setTextSize(1.0f);

    drawOthersWorking(330);
}

void drawDone() {
    canvas.fillArc(kCx, kCy, 220, 232, 0, 360, kColDone);
    drawLabel("DONE", 88, kColDone, &fonts::FreeSansBold24pt7b);
    drawLabel(g_project.c_str(), 134, kColSub, &fonts::lgfxJapanGothicP_20);
    if (g_doneDurationMs > 0) {
        const std::string label = "所要 " + formatDuration(g_doneDurationMs);
        drawLabel(label.c_str(), 170, kColText, &fonts::lgfxJapanGothicP_24);
    }
    canvas.setFont(&fonts::lgfxJapanGothicP_20);
    drawWrapped(g_message, 205, 330, 5, 27, kColText);
    drawOthersWorking(358);
    drawLabel("ボタンで閉じる", 395, kColSub, &fonts::efontJA_14);
}

void drawNotice(std::uint32_t now) {
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) / 300.0f);
    canvas.fillArc(kCx, kCy, 220, 232, 0, 360,
                   rgb(static_cast<std::uint8_t>(120 + 120 * pulse),
                       static_cast<std::uint8_t>(100 + 100 * pulse), 30));
    drawLabel("入力待ち", 96, kColAsk, &fonts::lgfxJapanGothicP_32);
    drawLabel(g_project.c_str(), 140, kColSub, &fonts::lgfxJapanGothicP_20);
    canvas.setFont(&fonts::lgfxJapanGothicP_20);
    drawWrapped(g_message, 185, 340, 6, 27, kColText);
    drawLabel("ボタンで閉じる", 395, kColSub, &fonts::efontJA_14);
}

void drawButton(const Rect& r, const char* label, std::uint16_t color) {
    canvas.fillRoundRect(r.x, r.y, r.w, r.h, 18, color);
    canvas.setFont(&fonts::FreeSansBold24pt7b);
    canvas.setTextColor(rgb(255, 255, 255));
    canvas.setTextDatum(middle_center);
    canvas.drawString(label, r.x + r.w / 2, r.y + r.h / 2 + 2);
}

void drawPerm(std::uint32_t now) {
    const PermRequest& req = g_perms.front();
    const float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(now) / 220.0f);
    canvas.fillArc(kCx, kCy, 220, 232, 0, 360,
                   rgb(static_cast<std::uint8_t>(130 + 125 * pulse),
                       static_cast<std::uint8_t>(80 + 90 * pulse), 20));

    char head[40];
    if (g_perms.size() > 1) {
        std::snprintf(head, sizeof(head), "承認リクエスト 1/%u", static_cast<unsigned>(g_perms.size()));
    } else {
        std::snprintf(head, sizeof(head), "承認リクエスト");
    }
    drawLabel(head, 62, kColAsk, &fonts::lgfxJapanGothicP_24);
    drawLabel(req.project.c_str(), 96, kColSub, &fonts::efontJA_16);
    drawLabel(req.tool.c_str(), 132, kColText, &fonts::lgfxJapanGothicP_32);

    canvas.fillRoundRect(58, 160, 350, 150, 14, kColPanel);
    canvas.setFont(&fonts::lgfxJapanGothic_20);
    drawWrapped(req.detail, 170, 326, 5, 27, rgb(210, 220, 200));

    drawButton(kNgRect, "NG", kColNg);
    drawButton(kOkRect, "OK", kColOk);
    drawLabel("長押し: PCで確認", 414, kColSub, &fonts::efontJA_14);
}

void drawFeedback() {
    const std::uint16_t color = g_feedbackOk ? kColOk : kColNg;
    canvas.fillCircle(kCx, kCy, 150, color);
    drawLabel(g_feedbackOk ? "OK" : "NG", kCy + 4, rgb(255, 255, 255), &fonts::FreeSansBold24pt7b);
    drawLabel(g_feedbackOk ? "承認しました" : "拒否しました", kCy + 190, color,
              &fonts::lgfxJapanGothicP_20);
}

bool isAnimating(std::uint32_t now) {
    return !g_perms.empty() || g_mode == Mode::Working || g_mode == Mode::Notice ||
           static_cast<std::int32_t>(g_feedbackUntil - now) > 0;
}

void render(std::uint32_t now) {
    canvas.fillScreen(kColBg);
    if (static_cast<std::int32_t>(g_feedbackUntil - now) > 0) {
        drawFeedback();
    } else if (!g_perms.empty()) {
        drawPerm(now);
    } else {
        switch (g_mode) {
            case Mode::Idle: drawIdle(); break;
            case Mode::Working: drawWorking(now); break;
            case Mode::Done: drawDone(); break;
            case Mode::Notice: drawNotice(now); break;
        }
    }
    drawStatusBar();
    canvas.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Input

void handleInput() {
    const bool anyPress = M5.BtnA.wasPressed() || M5.BtnB.wasPressed();
    const auto touch = M5.Touch.getDetail();
    if (g_dimmed && (anyPress || touch.wasPressed())) {
        wake();  // first press only wakes the screen
        return;
    }
    if (anyPress || touch.wasPressed()) {
        g_lastActivityAt = millis();
    }

    if (!g_perms.empty()) {
        // NimBLE writes happen on another task; ignore input while the
        // feedback overlay is up so a double press cannot answer two requests.
        if (static_cast<std::int32_t>(g_feedbackUntil - millis()) > 0) return;
        if (M5.BtnA.wasHold() || M5.BtnB.wasHold() || touch.wasHold()) {
            answerPerm(false, true);
        } else if (M5.BtnA.wasClicked()) {
            answerPerm(false, false);
        } else if (M5.BtnB.wasClicked()) {
            answerPerm(true, false);
        } else if (touch.wasClicked()) {
            if (kNgRect.contains(touch.x, touch.y)) {
                answerPerm(false, false);
            } else if (kOkRect.contains(touch.x, touch.y)) {
                answerPerm(true, false);
            }
        }
        return;
    }

    if ((g_mode == Mode::Done || g_mode == Mode::Notice) &&
        (M5.BtnA.wasClicked() || M5.BtnB.wasClicked() || touch.wasClicked())) {
        g_mode = g_othersWorking > 0 ? Mode::Working : Mode::Idle;
        g_dirty = true;
    }
}

void pollSerial() {
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == '\n') {
            if (!g_serialLine.empty()) {
                handleMessage(g_serialLine.c_str());
            }
            g_serialLine.clear();
        } else if (c >= 0 && g_serialLine.size() < kMaxLine) {
            g_serialLine.push_back(static_cast<char>(c));
        }
    }
}

void updateBattery(std::uint32_t now) {
    if (g_lastBatteryAt != 0 && now - g_lastBatteryAt < kBatteryPeriodMs) return;
    g_lastBatteryAt = now;
    const int level = M5.Power.getBatteryLevel();
    const bool charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    if (level != g_batteryLevel || charging != g_charging) {
        g_batteryLevel = level;
        g_charging = charging;
        g_dirty = true;
    }
}

void showSplash() {
    canvas.fillScreen(kColBg);
    drawLabel("M5ClaudeWatch", kCy - 10, kColClaude, &fonts::FreeSansBold24pt7b);
    drawLabel(kFirmwareVersion, kCy + 40, kColSub, &fonts::FreeSans12pt7b);
    canvas.pushSprite(0, 0);
    play({{523, 80}, {784, 80}, {1046, 120}});
}

}  // namespace

void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);

    auto config = M5.config();
    config.clear_display = true;
    config.internal_spk = true;
    M5.begin(config);
    M5.Display.setBrightness(kBrightActive);
    M5.Display.setRotation(0);
    M5.Speaker.setVolume(30);
    M5.BtnA.setHoldThresh(800);
    M5.BtnB.setHoldThresh(800);

    canvas.setColorDepth(16);
    canvas.setPsram(true);
    canvas.createSprite(kSize, kSize);

    g_rxQueue = xQueueCreate(16, sizeof(char*));
    showSplash();
    initBle();
    g_lastActivityAt = millis();
    sendHello();
}

void loop() {
    M5.update();
    const std::uint32_t now = millis();

    char* message = nullptr;
    while (xQueueReceive(g_rxQueue, &message, 0) == pdTRUE) {
        handleMessage(message);
        free(message);
    }
    pollSerial();
    handleInput();
    updateHaptics(now);
    updateBattery(now);

    // Re-alert while a request is waiting so it is not missed.
    if (!g_perms.empty() && now - g_lastRemindAt >= kRemindEveryMs) {
        g_lastRemindAt = now;
        feedbackPermArrived();
        wake();
    }

    if (!g_dimmed && g_perms.empty() && g_mode != Mode::Notice &&
        now - g_lastActivityAt >= kDimAfterMs) {
        g_dimmed = true;
        M5.Display.setBrightness(kBrightDim);
    }

    const std::uint32_t frameInterval = isAnimating(now) ? 33 : 1000;
    if (g_dirty || now - g_lastFrameAt >= frameInterval) {
        g_dirty = false;
        g_lastFrameAt = now;
        render(now);
    }
    delay(1);
}

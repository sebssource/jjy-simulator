#include "web_server.h"

#include <WebServer.h>

#include "carrier.h"
#include "scheduler.h"
#include "shared_state.h"

extern WebServer webServer;

static bool wantsJson()
{
    if (!webServer.hasArg("format")) {
        return false;
    }
    String fmt = webServer.arg("format");
    fmt.toLowerCase();
    return fmt == "json";
}

static String buildScheduleFooter()
{
    return String(BUILD_INFO);
}

static String internalModeLabel()
{
    String label = "WIFI_";
    label += wifiModeLabel(modeState.wifiMode);
    label.toUpperCase();
    label += "/BCAST_";
    label += broadcastModeLabel(modeState.broadcastMode);
    label.toUpperCase();
    label += "/SLEEP_";
    label += sleepModeLabel(modeState.sleepMode);
    label.toUpperCase();
    return label;
}

static String formatSecondsAsHhMmSs(uint32_t totalSeconds)
{
    const uint32_t hours = totalSeconds / 3600;
    const uint32_t minutes = (totalSeconds % 3600) / 60;
    const uint32_t seconds = totalSeconds % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", hours, minutes, seconds);
    return String(buf);
}

static String currentModeSummary()
{
    String summary = "Wi-Fi ";
    summary += wifiModeLabel(modeState.wifiMode);
    summary += ", Broadcast ";
    summary += broadcastModeLabel(modeState.broadcastMode);
    return summary;
}

static String currentModeBadge()
{
    const char* cls;
    if (modeState.broadcastMode == BroadcastMode::ON) {
        cls = "perm";
    } else if (modeState.wifiMode == WifiMode::ON) {
        cls = "override";
    } else if (modeState.sleepMode == SleepMode::OFF) {
        cls = "sleep";
    } else {
        cls = "auto";
    }
    return String(cls) + "\">" + currentModeSummary();
}

static String currentModeName()
{
    if (modeState.broadcastMode == BroadcastMode::ON) {
        return String("Always Transmit");
    }
    if (modeState.wifiMode == WifiMode::ON) {
        return String("Always-On Wi-Fi");
    }
    if (modeState.sleepMode == SleepMode::OFF) {
        return String("No Deep Sleep");
    }
    return String("Automatic Daily Broadcasts");
}

static String htmlHead(const String& title)
{
    return R"UIPART(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
  <title>)UIPART" + title + R"UIPART(</title>
  <style>
    * { box-sizing: border-box; }
    html, body { margin: 0; padding: 0; }
    body { font-family: -apple-system, "Noto Sans", "Segoe UI", Roboto, Helvetica, Arial, sans-serif; background: #f4f6f9; color: #1f2937; line-height: 1.5; }
    .wrap { max-width: 900px; margin: 0 auto; padding: 24px; }
    .card { background: #fff; border-radius: 18px; box-shadow: 0 1px 3px rgba(0,0,0,0.06); padding: 24px; margin-bottom: 20px; }
    header.card { display: flex; align-items: center; justify-content: space-between; gap: 16px; margin-bottom: 20px; }
    h1 { margin: 0; font-size: 1.5rem; font-weight: 800; letter-spacing: -0.02em; }
    h2 { margin: 0 0 20px; font-size: 1.125rem; font-weight: 700; }
    .badge { display: inline-block; padding: 6px 14px; border-radius: 999px; font-size: 0.75rem; font-weight: 700; letter-spacing: 0.05em; text-transform: uppercase; }
    .badge.auto { background: #d1fae5; color: #065f46; }
    .badge.override { background: #fef3c7; color: #92400e; }
    .badge.perm { background: #fee2e2; color: #991b1b; }
    .badge.off { background: #f3f4f6; color: #374151; }
    .badge.sleep { background: #e0e7ff; color: #3730a3; }
    .alert { padding: 16px 18px; border-radius: 14px; margin-bottom: 20px; font-size: 0.875rem; }
    .alert.amber { background: #fffbeb; border-left: 5px solid #f59e0b; color: #92400e; }
    .alert.red { background: #fef2f2; border-left: 5px solid #ef4444; color: #7f1d1d; }
    .alert strong { font-weight: 700; }
    .grid { display: grid; grid-template-columns: 1fr; gap: 20px; }
    @media (min-width: 720px) { .grid { grid-template-columns: 1fr 1fr; } }
    .btn { display: block; width: 100%; text-align: center; padding: 14px 16px; border-radius: 10px; font-size: 0.95rem; font-weight: 600; text-decoration: none; border: none; cursor: pointer; transition: background 0.15s, transform 0.05s; margin-bottom: 12px; }
    .btn-group { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; margin-bottom: 12px; }
    .btn-primary { background: #4f46e5; color: #fff; }
    .btn-primary:hover { background: #4338ca; }
    .btn-secondary { background: #eef2ff; color: #4f46e5; }
    .btn-secondary:hover { background: #e0e7ff; }
    .btn-success { background: #059669; color: #fff; }
    .btn-success:hover { background: #047857; }
    .btn-danger { background: #dc2626; color: #fff; }
    .btn-danger:hover { background: #b91c1c; }
    .btn-wide { grid-column: span 2; }
    .btn-default { background: #fff; color: #374151; border: 1px solid #d1d5db; }
    .btn-default:hover { background: #f9fafb; }
    .field { margin-bottom: 18px; }
    label { display: block; font-size: 0.9rem; font-weight: 500; color: #374151; margin-bottom: 6px; }
    .readonly { font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; display: block; width: 100%; padding: 12px 14px; background: #f9fafb; border: 1px solid #e5e7eb; border-radius: 10px; font-size: 0.8rem; color: #4b5563; word-break: break-all; line-height: 1.4; }
    .readonly-label { display: block; font-size: 0.65rem; font-weight: 700; color: #9ca3af; text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 6px; }
    .kv { display: flex; justify-content: space-between; padding: 12px 0; border-bottom: 1px solid #f3f4f6; }
    .kv:last-child { border-bottom: none; }
    .kv .k { font-size: 0.9rem; color: #6b7280; }
    .kv .v { font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace; font-size: 0.85rem; color: #111827; font-weight: 600; }
    select, input[type="text"] { width: 100%; padding: 13px 14px; border: 1px solid #d1d5db; border-radius: 10px; font-size: 0.95rem; background: #fff; color: #111827; appearance: none; -webkit-appearance: none; }
    select:focus, input[type="text"]:focus { outline: none; border-color: #4f46e5; box-shadow: 0 0 0 3px rgba(79,70,229,0.12); }
    .select-wrap { position: relative; }
    .select-wrap::after { content: ""; position: absolute; right: 16px; top: 50%; width: 10px; height: 10px; border-right: 2px solid #9ca3af; border-bottom: 2px solid #9ca3af; transform: translateY(-60%) rotate(45deg); pointer-events: none; }
    footer { text-align: center; font-size: 0.7rem; font-weight: 700; color: #9ca3af; text-transform: uppercase; letter-spacing: 0.12em; padding: 8px 0 24px; }
  </style>
</head>)UIPART";
}

static void sendHtmlResult(const String& title, const String& message, const String& detail = "")
{
    String page;
    page.reserve(2200);

    page += htmlHead(title);
    page += R"UIPART(
<body>
  <div class="wrap">
    <header class="card">
      <h1>)UIPART";
    page += title;
    page += R"UIPART(</h1>
    </header>
    <section class="card">
      <p style="font-size:1rem;color:#374151;">)UIPART";
    page += message;
    page += R"UIPART(</p>
)UIPART";

    if (detail.length() > 0) {
        page += R"UIPART(<p style="font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:0.85rem;color:#6b7280;margin-top:12px;">)UIPART";
        page += detail;
        page += "</p>";
    }

    page += R"UIPART(
    </section>
    <section class="card">
      <a href="/" class="btn btn-primary">Back</a>
    </section>
  </div>
</body>
</html>)UIPART";

    webServer.send(200, "text/html", page);
}

void handleWebRoot()
{
    markWebActivity();

    String page;
    page.reserve(5200);

    page += htmlHead("JJY Transmitter");

    page += R"UIPART(
<body>
  <div class="wrap">
    <header class="card">
      <h1>JJY Transmitter</h1>
      <span class="badge )UIPART";

    page += currentModeBadge();

    page += R"UIPART(</span>
    </header>
)UIPART";

    if (modeState.broadcastMode == BroadcastMode::ON) {
        page += R"UIPART(
    <div class="alert red"><strong>Permanent broadcast is active.</strong> Device stays awake and the carrier transmits continuously. Wi-Fi is kept on.</div>
)UIPART";
    }

    if (coldBootBroadcastActive) {
        page += R"UIPART(
    <div class="alert amber"><strong>Cold boot broadcast active.</strong> Auto deep sleep is paused while the window runs.</div>
)UIPART";
    }

    page += R"UIPART(
    <div class="grid">
      <section class="card">
        <h2>Mode</h2>
        <h3 style="font-size:0.875rem;color:#6b7280;margin:0 0 12px;">Broadcast</h3>
        <div class="btn-group">
          <a href="/mode/broadcast?value=auto" class="btn btn-success">Auto</a>
          <a href="/mode/broadcast?value=on" class="btn btn-danger">On</a>
        </div>
        <h3 style="font-size:0.875rem;color:#6b7280;margin:18px 0 12px;">Wi-Fi</h3>
        <div class="btn-group">
          <a href="/mode/wifi?value=auto" class="btn btn-success">Auto</a>
          <a href="/mode/wifi?value=on" class="btn btn-danger">On</a>
        </div>
        <h3 style="font-size:0.875rem;color:#6b7280;margin:18px 0 12px;">Deep Sleep</h3>
        <div class="btn-group">
          <a href="/mode/sleep?value=auto" class="btn btn-success">Auto</a>
          <a href="/mode/sleep?value=off" class="btn btn-danger">Stay Awake</a>
        </div>
        <a href="/save" class="btn btn-primary">Save</a>
        <a href="/sleep" class="btn btn-default">Sleep Now</a>
        <a href="/status" class="btn btn-default">View Status</a>
      </section>

      <section class="card">
        <h2>Timezone Settings</h2>
        <div class="field">
          <span class="readonly-label">Active rule</span>
          <code class="readonly">)UIPART";

    page += currentTzRule;

    page += R"UIPART(</code>
        </div>
        <form action="/set_tz" method="GET">
          <div class="field">
            <label for="tz_sel">Select Timezone</label>
            <div class="select-wrap">
              <select id="tz_sel" name="tz" onchange="updateTzVisibility()">
                <option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Sydney AEDT</option>
                <option value="JST-9">Japan (JST-9)</option>
                <option value="UTC0">UTC (UTC0)</option>
                <option value="custom">Custom...</option>
              </select>
            </div>
          </div>
          <div id="tz_custom_div" style="display:none;" class="field">
            <label for="tz_custom">POSIX Rule</label>
            <input type="text" id="tz_custom" name="tz_custom" placeholder="e.g. JST-9">
          </div>
          <button type="submit" onclick="var s=document.getElementById('tz_sel');if(s.value==='custom'){s.value=document.getElementById('tz_custom').value;}" class="btn btn-primary">Apply Timezone</button>
        </form>
      </section>

      <section class="card">
        <h2>Daily Windows</h2>
        <div class="readonly-label">Scheduled times</div>
        <div class="readonly" style="line-height:2;">)UIPART";
    for (int i = 0; i < static_cast<int>(SCHEDULED_SLOT_COUNT); ++i) {
        if (i > 0) {
            page += "<br>";
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d ",
            SCHEDULED_SLOT_HOUR[i],
            SCHEDULED_SLOT_MINUTE,
            SCHEDULED_SLOT_SECOND);
        page += buf;
    }
    page += R"UIPART(</div>
      </section>

      <section class="card">
        <h2>Saved Settings</h2>
        <div class="readonly-label">Wi-Fi mode</div>
        <div class="readonly" style="margin-bottom:12px;">)UIPART";
    page += wifiModeLabel(modeState.wifiMode);
    page += R"UIPART(</div>
        <div class="readonly-label">Broadcast mode</div>
        <div class="readonly" style="margin-bottom:12px;">)UIPART";
    page += broadcastModeLabel(modeState.broadcastMode);
    page += R"UIPART(</div>
        <div class="readonly-label">Deep sleep mode</div>
        <div class="readonly" style="margin-bottom:12px;">)UIPART";
    page += (modeState.sleepMode == SleepMode::OFF) ? "Stay awake" : "Auto";
    page += R"UIPART(</div>
        <div class="readonly-label">Carrier frequency</div>
        <div class="readonly">)UIPART";
    page += String(currentCarrierHz) + " Hz";
    page += R"UIPART(</div>
      </section>
    </div>

    <script>function updateTzVisibility(){var s=document.getElementById('tz_sel'),c=document.getElementById('tz_custom_div');c.style.display=(s.value==='custom')?'block':'none';}</script>
    <footer>)UIPART";
    page += buildScheduleFooter();
    page += R"UIPART(</footer>
  </div>
</body>
</html>)UIPART";

    webServer.send(200, "text/html", page);
}

static void sendJsonStatus()
{
    const time_t nowEpoch = time(nullptr);

    time_t nextSlotEpoch = 0;
    int nextSlotIndex = SLOT_INDEX_NONE;
    const bool haveNextSlot = findNextSlotEpoch(nowEpoch, nextSlotEpoch, nextSlotIndex);
    const time_t nextWakeEpoch = haveNextSlot ? (nextSlotEpoch - static_cast<time_t>(WIFI_WAKE_LEAD_SECONDS)) : 0;

    uint32_t coldBootBroadcastRemainingSec = 0;
    if (coldBootBroadcastActive && isValidEpoch(nowEpoch) && txWindowEndEpoch > nowEpoch) {
        coldBootBroadcastRemainingSec = static_cast<uint32_t>(txWindowEndEpoch - nowEpoch);
    }

    String json = "{";
    json += "\"mode\":\"" + internalModeLabel() + "\",";
    json += "\"wifi_mode\":\"" + String(wifiModeLabel(modeState.wifiMode)) + "\",";
    json += "\"broadcast_mode\":\"" + String(broadcastModeLabel(modeState.broadcastMode)) + "\",";
    json += "\"sleep_mode\":\"" + String(sleepModeLabel(modeState.sleepMode)) + "\",";
    json += "\"time\":\"" + formatEpochLocal(nowEpoch) + "\",";
    json += "\"tz_rule\":\"" + currentTzRule + "\",";
    json += "\"next_slot\":\"" + (haveNextSlot ? formatEpochLocal(nextSlotEpoch) : String("n/a")) + "\",";
    json += "\"next_wake\":\"" + (haveNextSlot ? formatEpochLocal(nextWakeEpoch) : String("n/a")) + "\",";
    json += "\"next_slot_index\":" + String(nextSlotIndex) + ",";
    json += "\"tx_active\":" + String(txWindowActive ? "true" : "false") + ",";
    json += "\"cold_boot_startup_pending\":" + String(coldBootStartupPending ? "true" : "false") + ",";
    json += "\"cold_boot_broadcast_active\":" + String(coldBootBroadcastActive ? "true" : "false") + ",";
    json += "\"cold_boot_broadcast_remaining_sec\":" + String(coldBootBroadcastRemainingSec) + ",";
    json += "\"recent_web_activity\":" + String(isRecentWebActivity() ? "true" : "false");
    json += "}";
    webServer.send(200, "application/json", json);
}

void handleWebStatus()
{
    markWebActivity();

    if (wantsJson()) {
        sendJsonStatus();
        return;
    }

    const time_t nowEpoch = time(nullptr);

    time_t nextSlotEpoch = 0;
    int nextSlotIndex = SLOT_INDEX_NONE;
    const bool haveNextSlot = findNextSlotEpoch(nowEpoch, nextSlotEpoch, nextSlotIndex);
    const time_t nextWakeEpoch = haveNextSlot ? (nextSlotEpoch - static_cast<time_t>(WIFI_WAKE_LEAD_SECONDS)) : 0;

    uint32_t coldBootBroadcastRemainingSec = 0;
    if (coldBootBroadcastActive && isValidEpoch(nowEpoch) && txWindowEndEpoch > nowEpoch) {
        coldBootBroadcastRemainingSec = static_cast<uint32_t>(txWindowEndEpoch - nowEpoch);
    }

    String page;
    page.reserve(4200);

    page += htmlHead("JJY Status");
    page += R"UIPART(
<meta http-equiv="refresh" content="10">
<body>
  <div class="wrap">
    <header class="card">
      <h1>JJY Status</h1>
      <span class="badge )UIPART";

    page += currentModeBadge();

    page += R"UIPART(</span>
    </header>

    <section class="card">
      <h2>Current State</h2>
      <div class="kv"><span class="k">Mode</span><span class="v">)UIPART";
    page += currentModeName();
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Local Time</span><span class="v">)UIPART";
    page += formatEpochLocal(nowEpoch);
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Timezone Rule</span><span class="v">)UIPART";
    page += currentTzRule;
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Carrier Frequency</span><span class="v">)UIPART";
    page += String(currentCarrierHz) + " Hz";
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Broadcast Active</span><span class="v">)UIPART";
    page += txWindowActive ? "Yes" : "No";
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Wi-Fi Mode</span><span class="v">)UIPART";
    page += wifiModeLabel(modeState.wifiMode);
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Broadcast Mode</span><span class="v">)UIPART";
    page += broadcastModeLabel(modeState.broadcastMode);
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Sleep Mode</span><span class="v">)UIPART";
    page += sleepModeLabel(modeState.sleepMode);
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Next Slot</span><span class="v">)UIPART";
    page += haveNextSlot ? formatEpochLocal(nextSlotEpoch) : "n/a";
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Next Wake</span><span class="v">)UIPART";
    page += haveNextSlot ? formatEpochLocal(nextWakeEpoch) : "n/a";
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Cold Boot Pending</span><span class="v">)UIPART";
    page += coldBootStartupPending ? "Yes" : "No";
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Cold Boot Broadcast</span><span class="v">)UIPART";
    page += coldBootBroadcastActive ? (formatSecondsAsHhMmSs(coldBootBroadcastRemainingSec) + " remaining") : "No";
    page += R"UIPART(</span></div>
      <div class="kv"><span class="k">Recent Web Activity</span><span class="v">)UIPART";
    page += isRecentWebActivity() ? "Yes" : "No";
    page += R"UIPART(</span></div>
    </section>

    <section class="card">
      <a href="/" class="btn btn-primary">Back</a>
    </section>

    <footer>)UIPART";
    page += buildScheduleFooter();
    page += R"UIPART(</footer>
  </div>
</body>
</html>)UIPART";

    webServer.send(200, "text/html", page);
}

static void handleWebSetTz()
{
    markWebActivity();

    if (!webServer.hasArg("tz")) {
        if (wantsJson()) {
            webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing tz parameter\"}");
        } else {
            sendHtmlResult("Timezone Error", "Missing timezone parameter.", "Please go back and select a timezone.");
        }
        return;
    }

    const String newTz = webServer.arg("tz");
    if (newTz.length() == 0) {
        if (wantsJson()) {
            webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"empty tz parameter\"}");
        } else {
            sendHtmlResult("Timezone Error", "Empty timezone parameter.", "Please go back and select a timezone.");
        }
        return;
    }

    currentTzRule = newTz;
    if (prefsReady) {
        prefs.putString(NVS_KEY_TZ, currentTzRule);
    }

    Serial.printf("[WEB] Timezone updated to: %s\n", currentTzRule.c_str());

    configTzTime(currentTzRule.c_str(), NTP1, NTP2);

    if (txWindowActive) {
        stopTxWindow("timezone changed");
    }

    if (wantsJson()) {
        webServer.send(200, "application/json", "{\"ok\":true,\"tz_rule\":\"" + currentTzRule + "\"}");
        return;
    }

    sendHtmlResult("Timezone Updated", "Timezone rule applied:", currentTzRule);
}

void handleWebMode()
{
    markWebActivity();

    const String target = webServer.uri().substring(6);

    if (!webServer.hasArg("value")) {
        if (wantsJson()) {
            webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"missing value\"}");
        } else {
            sendHtmlResult("Mode Error", "Missing mode value.", "Use auto, on, or off.");
        }
        return;
    }

    String value = webServer.arg("value");
    value.toLowerCase();

    ModeState next = modeState;

    if (target == "wifi") {
        if (value == "on" || value == "1" || value == "true") {
            next.wifiMode = WifiMode::ON;
        } else if (value == "auto") {
            next.wifiMode = WifiMode::AUTO;
        } else {
            if (wantsJson()) {
                webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"value must be auto|on\"}");
            } else {
                sendHtmlResult("Mode Error", "Invalid Wi-Fi mode value.", "Use auto or on.");
            }
            return;
        }
    } else if (target == "broadcast") {
        if (value == "on" || value == "1" || value == "true") {
            next.broadcastMode = BroadcastMode::ON;
        } else if (value == "off" || value == "0" || value == "false" || value == "auto") {
            next.broadcastMode = BroadcastMode::AUTO;
        } else {
            if (wantsJson()) {
                webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"value must be auto|on|off\"}");
            } else {
                sendHtmlResult("Mode Error", "Invalid broadcast mode value.", "Use auto, on, or off.");
            }
            return;
        }
    } else if (target == "sleep") {
        if (value == "on" || value == "1" || value == "true" || value == "off") {
            next.sleepMode = SleepMode::OFF;
        } else if (value == "auto") {
            next.sleepMode = SleepMode::AUTO;
        } else {
            if (wantsJson()) {
                webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"value must be auto|on\"}");
            } else {
                sendHtmlResult("Mode Error", "Invalid sleep mode value.", "Use auto or on (stay awake).");
            }
            return;
        }
    } else {
        if (wantsJson()) {
            webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"unknown mode endpoint\"}");
        } else {
            sendHtmlResult("Mode Error", "Unknown mode endpoint.", "Use /mode/wifi, /mode/broadcast, or /mode/sleep.");
        }
        return;
    }

    applyModeState(next);
    persistModeState(next);

    const String modeLabel = internalModeLabel();
    const String modeName = currentModeName();

    if (wantsJson()) {
        webServer.send(200, "application/json", "{\"ok\":true,\"mode\":\"" + modeLabel + "\"}");
        return;
    }

    String message = "Mode set to <strong>" + modeName + "</strong>.";
    if (modeState.broadcastMode == BroadcastMode::ON) {
        message += " The carrier will transmit continuously and the device will stay awake.";
    } else if (modeState.wifiMode == WifiMode::ON) {
        message += " Wi-Fi will stay on and deep sleep is paused.";
    } else if (modeState.sleepMode == SleepMode::OFF) {
        message += " Deep sleep is disabled; the device will stay awake between scheduled windows.";
    } else {
        message += " The device will follow the daily schedule and sleep between windows.";
    }

    sendHtmlResult("Mode Updated", message);
}

void handleWebSave()
{
    markWebActivity();
    persistModeState(modeState);

    const String modeLabel = internalModeLabel();
    const String modeName = currentModeName();

    Serial.printf("[WEB] Mode saved to NVS: %s\n", modeLabel.c_str());

    if (wantsJson()) {
        webServer.send(200, "application/json", "{\"ok\":true,\"mode\":\"" + modeLabel + "\"}");
        return;
    }

    const String message = "Current mode <strong>" + modeName + "</strong> saved to NVS.";
    sendHtmlResult("Settings Saved", message);
}

void handleWebSleep()
{
    markWebActivity();

    if (wantsJson()) {
        webServer.send(200, "application/json", "{\"ok\":true,\"sleep_request\":\"accepted\"}");
    } else {
        sendHtmlResult("Sleep Requested", "The device will try to enter deep sleep now.", "If sleep is not possible, the page will remain reachable.");
    }
    delay(25);

    if (txWindowActive) {
        stopTxWindow("web sleep request");
    }
    coldBootStartupPending = false;
    coldBootBroadcastActive = false;

    const time_t nowEpoch = time(nullptr);
    if (!enterDeepSleepUntilNextSlot(nowEpoch, "WEB_REQUEST")) {
        Serial.println("[WEB] Sleep request could not enter deep sleep.");
    }
}

void setupWebServer()
{
    webServer.on("/", HTTP_GET, handleWebRoot);
    webServer.on("/status", HTTP_GET, handleWebStatus);
    webServer.on("/mode/wifi", HTTP_GET, handleWebMode);
    webServer.on("/mode/broadcast", HTTP_GET, handleWebMode);
    webServer.on("/mode/sleep", HTTP_GET, handleWebMode);
    webServer.on("/save", HTTP_GET, handleWebSave);
    webServer.on("/sleep", HTTP_GET, handleWebSleep);
    webServer.on("/set_tz", HTTP_GET, handleWebSetTz);

    webServer.begin();
    webServerReady = true;
    Serial.println("[WEB] Server started (HTTP :80)");
}

// main.cpp — WinShunBright: a Win顺 resident plugin.
//
// Gestures: Ctrl+Alt+wheel adjusts every adjustable display (WMI for the
// built-in panel, DDC/CI for external ones) and shows the average in the
// overlay; Ctrl+Alt+left-click toggles the warm gamma filter. Recognized but
// unusable gestures are swallowed and explained. Everything else — including
// Ctrl-only wheel, which belongs to the volume plugin — passes through.
//
// Keyboard: the `bright` keyword lists each monitor with its channel and
// current percentage; a number sets them all. Enter / Ctrl+Enter / Shift+Enter
// step the selected monitor by +10 / -10, or jump to 100.
#include "winshun.h"

#include "bright_logic.h"
#include "bright_worker.h"
#include "display_controller.h"
#include "hotkey_thread.h"
#include "mouse_hook.h"
#include "settings.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

using winshun::ActionRequest;
using winshun::Effects;
using winshun::Item;
using winshun::Items;
using winshun::Json;
using winshun::Query;
namespace effect = winshun::effect;

namespace {

constexpr const wchar_t* kStateFile = L"state.json";

std::wstring statePath(const std::string& dataDir)
{
    if (dataDir.empty())
        return {};
    return winshun::wide(dataDir) + L"\\" + kStateFile;
}

// Persisted user state; a missing or broken file is not an error.
struct PersistedState {
    bool warmEnabled = false;
    int warmStrength = 0;
    bool easterEggDiscovered = false;
    bool easterEggEnabled = true;
};

PersistedState loadState(const std::wstring& path)
{
    PersistedState state;
    if (path.empty())
        return state;
    FILE* f = nullptr;
    if (::_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return state;
    std::string text;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof buffer, f)) > 0)
        text.append(buffer, got);
    std::fclose(f);
    const auto parsed = Json::parse(text);
    if (parsed && parsed->isObject()) {
        state.warmEnabled = (*parsed)["warmEnabled"].toBool(false);
        const int strength = static_cast<int>((*parsed)["warmStrength"].toInt(0));
        state.warmStrength = strength < 0 ? 0 : (strength > 100 ? 100 : strength);
        state.easterEggDiscovered = (*parsed)["easterEggDiscovered"].toBool(false);
        state.easterEggEnabled = (*parsed)["easterEggEnabled"].toBool(true);
    }
    return state;
}

void saveState(const std::wstring& path, const PersistedState& state)
{
    if (path.empty())
        return;
    const Json json(Json::Object {
        { "warmEnabled", state.warmEnabled },
        { "warmStrength", state.warmStrength },
        { "easterEggDiscovered", state.easterEggDiscovered },
        { "easterEggEnabled", state.easterEggEnabled },
    });
    FILE* f = nullptr;
    if (::_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return;
    const std::string text = json.dump();
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

std::wstring channelText(const br::MonitorInfo& m, bool zh)
{
    switch (m.channel) {
    case br::Channel::Wmi:
        return L"WMI";
    case br::Channel::Ddc:
        return L"DDC";
    case br::Channel::None:
    default:
        return zh ? L"不支持" : L"unsupported";
    }
}

std::wstring monitorSubtitle(const br::MonitorInfo& m, bool zh, const std::wstring& percentText,
    const br::Settings& settings, bool remote)
{
    const std::wstring type = m.internal ? (zh ? L"内屏" : L"internal")
                                        : (zh ? L"外屏" : L"external");
    // A panel the layer simulates rather than the hardware: said plainly so a
    // dimmed screen is not mistaken for a DDC panel that suddenly works. In a
    // remote session every panel is simulated (remoteDim), the hardware never
    // answers there.
    const bool simulated = remote
        ? settings.remoteDim
        : (settings.softwareBrightness == br::SoftwareDim::Always
            || (settings.softwareBrightness == br::SoftwareDim::UnsupportedOnly && !m.adjustable));
    std::wstring text;
    if (simulated) {
        text = type + (zh ? L" · 软件模拟" : L" · software");
        if (!percentText.empty())
            text += L" · " + percentText;
        if (remote)
            text += zh ? L" · 远程会话内可用" : L" · works over a remote session";
    } else if (!m.adjustable) {
        // An unsupported display needs the type once, not the channel and the
        // percentage repeating the same word.
        text = type + (zh ? L" · 不支持" : L" · unsupported");
        // A remote session is another reason, not a second defect: name it.
        if (remote)
            text += zh ? L"（远程会话）" : L" (remote session)";
    } else {
        text = type + L" · " + channelText(m, zh);
        if (!percentText.empty())
            text += L" · " + percentText;
    }
    // The EDID name is often "Generic PnP Monitor" on every panel; the GDI
    // device name tells them apart.
    if (!m.id.empty())
        text += L" · " + m.id;
    return text;
}

} // namespace

class BrightPlugin : public winshun::Plugin {
public:
    BrightPlugin()
    {
        enablePerMonitorDpi();
        s_instance = this;
        m_worker.start();
    }

    ~BrightPlugin() override
    {
        m_hook.stop();
        m_ownHotkeys.stop();
        m_worker.stop();
        m_protocolBackend.reset(); // COM object of the protocol thread
        if (s_instance == this)
            s_instance = nullptr;
    }

protected:
    std::string name() const override { return "winshun-bright"; }

    // initialize carries the merged settings (defaults + the user's values);
    // settingsChanged only follows a later edit.
    void initialized() override
    {
        m_statePath = statePath(dataDir());
        m_persisted = loadState(m_statePath);
        br::Settings initial = br::parseSettings(settings());
        initial.mushroomEgg = br::effectiveEggEnabled(
            m_persisted.easterEggDiscovered, initial.mushroomEgg, m_persisted.easterEggEnabled);
        applySettings(initial);
        m_worker.setWarmSink([this](bool on) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_persisted.warmEnabled = on;
            m_persisted.warmStrength = warmStrengthSnapshot();
            saveState(m_statePath, m_persisted);
        });
        m_worker.setEggDiscoveredSink([this] {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                if (!m_persisted.easterEggDiscovered) {
                    m_persisted.easterEggDiscovered = true;
                    saveState(m_statePath, m_persisted);
                    changed = true;
                }
            }
            if (changed)
                refresh();
        });
        if (m_settings.warmRemember && m_persisted.warmEnabled && m_persisted.warmStrength > 0)
            m_worker.setWarmEnabled(true);
    }

    void appearanceChanged() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_settings.chinese = chinese();
        m_worker.applySettings(m_settings);
    }

    Items query(const Query& q) override
    {
        const bool zh = chinese();
        if (m_worker.eggRunning()) {
            Item blocked("egg-running", zh ? "彩蛋播放中" : "Easter egg is playing",
                zh ? "点击鼠标停止彩蛋后再调节亮度" : "Click to stop the easter egg before changing brightness");
            blocked.invalid();
            return { blocked };
        }
        const bool found = discovered();
        const std::string text = lowerAscii(q.text);
        if (found && (text == "egg" || text == "easter egg" || text == "彩蛋"
                         || text == "egg on" || text == "egg off"
                         || text == "彩蛋 开" || text == "彩蛋 关"))
            return { eggToggleItem(zh) };
        br::IDisplayBackend* backend = protocolBackend();
        if (!backend) {
            Item error("display-unavailable",
                zh ? "无法读取显示器" : "Display information unavailable",
                zh ? "WMI 与 DDC/CI 均不可用，请确认显示器连接后重试"
                   : "Neither WMI nor DDC/CI answered; check the display connection and retry");
            error.invalid();
            return { error };
        }

        const br::BrightnessQuery parsed = br::parseBrightnessQuery(q.text);
        if (parsed.kind == br::BrightnessQuery::Kind::Invalid) {
            Item bad("bad-value",
                zh ? "无法识别的亮度值" : "Unrecognized brightness value",
                zh ? "输入 0–100 的数字，或留空列出显示器"
                   : "Type a number 0-100, or leave it empty to list the displays");
            bad.invalid();
            return { bad };
        }

        // A remote session never sees the physical panels: saying "not
        // supported" sends the user hunting for a driver problem that is not
        // there. The rows still list what the session can enumerate.
        const bool remote = br::isRemoteSession();
        const br::Settings settings = settingsSnapshot();

        backend->refreshValues();
        // A copy: setPercent() may rebuild the cache, which would invalidate
        // a reference into it.
        const std::vector<br::MonitorInfo> monitors = backend->monitors();

        if (parsed.kind == br::BrightnessQuery::Kind::Set) {
            const int target = br::clampPercent(parsed.percent);
            const bool clamped = target != parsed.percent;
            Items items;
            bool any = false;
            for (const br::MonitorInfo& m : monitors) {
                if (!m.adjustable)
                    continue;
                int applied = target;
                const bool ok = backend->setPercent(m.id, target, applied);
                if (!ok) {
                    Item failed("failed:" + winshun::utf8(m.id),
                        winshun::utf8(m.name),
                        zh ? "设置失败" : "Failed to set");
                    failed.invalid();
                    failed.withData(Json::Object { { "monitor", winshun::utf8(m.id) } });
                    items.push_back(std::move(failed));
                    continue;
                }
                any = true;
                std::wstring text = std::to_wstring(applied) + L"%";
                if (clamped)
                    text += zh ? (L"（" + std::to_wstring(parsed.percent) + L" 越界，已限制）")
                               : (L" (clamped from " + std::to_wstring(parsed.percent) + L")");
                Item it("monitor:" + winshun::utf8(m.id), winshun::utf8(m.name),
                    winshun::utf8(monitorSubtitle(m, zh, text, settings, remote)));
                it.withData(Json::Object { { "monitor", winshun::utf8(m.id) } });
                items.push_back(std::move(it));
            }
            if (!any && items.empty()) {
                Item none("no-display",
                    zh ? "没有可调节的显示器" : "No adjustable display",
                    remote
                        ? (zh ? "远程桌面会话只暴露一块虚拟屏，亮度到不了物理显示器"
                              : "A remote session exposes one virtual display; brightness never reaches the physical monitor")
                        : (zh ? "内屏需要驱动提供 WMI 亮度类，外屏需要支持 DDC/CI"
                              : "An internal panel needs the WMI brightness class; an external one needs DDC/CI"));
                none.invalid();
                items.push_back(std::move(none));
            }
            return items;
        }

        // Empty query: one row per monitor.
        Items items;
        for (const br::MonitorInfo& m : monitors) {
            const bool simulatedHere = remote
                ? settings.remoteDim
                : (settings.softwareBrightness == br::SoftwareDim::Always
                    || (settings.softwareBrightness == br::SoftwareDim::UnsupportedOnly && !m.adjustable));
            std::wstring percent;
            if (m.adjustable && m.percent >= 0)
                percent = std::to_wstring(m.percent) + L"%";
            else if (!simulatedHere)
                percent = zh ? L"不支持" : L"unsupported";
            // A simulated panel has no reading of its own; the layer's value
            // lives on the worker thread.

            Item it("monitor:" + winshun::utf8(m.id),
                winshun::utf8(m.name),
                winshun::utf8(monitorSubtitle(m, zh, percent, settings, remote)));
            it.withData(Json::Object {
                { "monitor", winshun::utf8(m.id) },
                { "percent", m.percent },
                { "adjustable", m.adjustable },
                { "simulated", simulatedHere },
                { "bounds", Json::Object {
                      { "left", m.bounds.left },
                      { "top", m.bounds.top },
                      { "right", m.bounds.right },
                      { "bottom", m.bounds.bottom },
                  } },
            });
            if (!m.adjustable && !simulatedHere)
                it.invalid();
            items.push_back(std::move(it));
        }
        if (items.empty()) {
            Item none("no-display",
                zh ? "没有检测到显示器" : "No display detected",
                zh ? "请确认显示器已连接后重试" : "Connect a display and retry");
            none.invalid();
            items.push_back(std::move(none));
        }
        if (discovered())
            items.push_back(eggToggleItem(zh));
        if (remote) {
            // One explaining row at the end, so the list itself is not lying
            // about the panels the session cannot reach. With the layer on
            // (explicitly, or by the remote default), the session can still
            // be dimmed — say that instead.
            const bool layerOn = settings.softwareBrightness != br::SoftwareDim::Off
                || settings.remoteDim;
            Item note("remote-session",
                zh ? "远程桌面会话" : "Remote desktop session",
                layerOn
                    ? (zh ? "软件模拟可以正常使用；硬件通道（WMI/DDC/伽马）不会到达物理显示器"
                          : "The software layer still works; WMI, DDC and gamma never reach the physical monitor")
                    : (zh ? "软件模拟已按远程设置关闭，亮度不可用；可在设置里开启“远程会话启用软件模拟”"
                          : "The software layer is off for remote sessions, so brightness is unavailable; enable it in the settings"));
            note.invalid();
            items.push_back(std::move(note));
        }
        return items;
    }

    Effects action(const ActionRequest& a) override
    {
        const bool zh = chinese();
        if (a.item["id"].toString() == "egg-toggle") {
            if (!discovered())
                return { effect::notify(zh ? "先触发一次彩蛋即可解锁开关" : "Trigger the easter egg once to unlock this toggle"), effect::keep() };
            if (a.action == "play" || a.action == "default") {
                if (!eggEnabled())
                    return { effect::notify(zh ? "彩蛋已关闭，请先开启" : "Easter egg is off; enable it first"), effect::keep() };
                POINT cursor {};
                ::GetCursorPos(&cursor);
                br::BrightOp play;
                play.kind = br::BrightOp::Kind::PlayEgg;
                play.x = cursor.x;
                play.y = cursor.y;
                m_worker.post(std::move(play));
                return { effect::keep() };
            }
            if (a.action == "on" || a.action == "off") {
                setEggEnabled(a.action == "on");
                refresh();
                return { effect::notify(a.action == "on"
                    ? (zh ? "彩蛋已开启" : "Easter egg enabled")
                    : (zh ? "彩蛋已关闭" : "Easter egg disabled")), effect::keep() };
            }
            return { effect::notify(zh ? "请选择彩蛋操作" : "Choose an easter egg action"), effect::keep() };
        }
        if (m_worker.eggRunning())
            return { effect::notify(zh ? "彩蛋播放中，请先点击鼠标停止" : "The easter egg is playing; click to stop it first"), effect::keep() };
        const std::string monitor = a.item["data"]["monitor"].toString();
        if (monitor.empty())
            return { effect::notify(zh ? "不支持的显示器" : "Unknown monitor"), effect::hide() };

        br::IDisplayBackend* backend = protocolBackend();
        if (!backend)
            return { effect::notify(zh ? "无法读取显示器" : "Display information unavailable"),
                effect::hide() };

        const std::wstring id = winshun::wide(monitor);
        const std::optional<int> current = backend->readPercent(id);
        if (!current)
            return { effect::notify(zh ? "该显示器不可调节或已断开"
                                       : "This display is not adjustable or is disconnected"),
                effect::hide() };

        int steps = 0;
        if (a.action == "up")
            steps = 1;
        else if (a.action == "down")
            steps = -1;
        else if (a.action == "max") {
            int applied = 100;
            const bool ok = backend->setPercent(id, 100, applied);
            return { effect::notify(ok ? (zh ? "亮度 · 100%" : "Brightness · 100%")
                                       : (zh ? "调整亮度失败" : "Failed to change the brightness")),
                effect::hide() };
        } else
            return { effect::notify(zh ? "不支持的操作" : "Unsupported action"), effect::hide() };

        // The keyboard actions step by a fixed 10%; percentPerStep is the
        // wheel gesture's step.
        const int next = br::clampPercent(*current + steps * 10);
        int applied = next;
        if (!backend->setPercent(id, next, applied))
            return { effect::notify(zh ? "调整亮度失败" : "Failed to change the brightness"),
                effect::hide() };
        return { effect::notify((zh ? "亮度 · " : "Brightness · ") + std::to_string(applied) + "%"),
            effect::hide() };
    }

    void settingsChanged(const Json& s) override
    {
        br::Settings incoming = br::parseSettings(s);
        const bool found = discovered();
        if (found) {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_persisted.easterEggEnabled = incoming.mushroomEgg;
            saveState(m_statePath, m_persisted);
        }
        incoming.mushroomEgg = br::effectiveEggEnabled(
            found, incoming.mushroomEgg, eggEnabled());
        applySettings(incoming);
    }

    // A host-registered hotkey was pressed (capabilities.hostHotkeys).
    void hotkey(const std::string& key) override
    {
        onHotkey(winshun::wide(key));
    }

    // Released before the host ends the process, so the keys do not linger
    // registered until it notices the exit.
    void shuttingDown() override
    {
        m_hook.stop();
        m_ownHotkeys.stop();
        if (hostHotkeys())
            tool("register_hotkeys", Json::Object { { "keys", Json::Array {} } });
    }

private:
    bool discovered()
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_persisted.easterEggDiscovered;
    }

    bool eggEnabled()
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        return m_persisted.easterEggEnabled;
    }

    void setEggEnabled(bool enabled)
    {
        br::Settings settings;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_settings.mushroomEgg = enabled;
            settings = m_settings;
        }
        {
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_persisted.easterEggEnabled = enabled;
            saveState(m_statePath, m_persisted);
        }
        m_worker.applySettings(settings);
    }

    Item eggToggleItem(bool zh)
    {
        const bool enabled = eggEnabled();
        Item item("egg-toggle",
            zh ? "蘑菇彩蛋" : "Mushroom easter egg",
            enabled ? (zh ? "已开启 · 回车播放；也可管理开关" : "Enabled · Enter to play; more actions to manage")
                    : (zh ? "已关闭 · 开启后可播放" : "Disabled · enable it to play"));
        item.withActions({
            { "play", zh ? "播放彩蛋" : "Play easter egg", "Enter" },
            { "on", zh ? "开启" : "Enable", "" },
            { "off", zh ? "关闭" : "Disable", "" },
        });
        item.withData(Json::Object { { "enabled", enabled } });
        return item;
    }

    static std::string lowerAscii(std::string text)
    {
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');
        }
        return text;
    }

    void applySettings(const br::Settings& in)
    {
        br::Settings settings = in;
        settings.chinese = chinese();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_settings = settings;
        }
        m_worker.applySettings(settings);

        // Start/stop threads outside the lock: stop() joins the hook thread.
        if (settings.gestureEnabled) {
            if (!m_hook.active()) {
                m_hook.start([](const br::GestureDecision& d) {
                    if (s_instance)
                        s_instance->dispatchGesture(d);
                }, settings.gestureModifiers);
            } else {
                // Live: only the chord changed, no need to reinstall the hook.
                m_hook.setChord(settings.gestureModifiers);
            }
        } else {
            m_hook.stop();
        }

        applyHotkeys(settings);
    }

    // The three step hotkeys. A host that offers register_hotkeys replaces
    // this plugin's registrations (docs/plugins.md 快捷键), so a single key
    // pressed over an elevated window still arrives; an older host gets the
    // RegisterHotKey path on our own thread.
    void applyHotkeys(const br::Settings& settings)
    {
        std::vector<std::wstring> keys;
        for (const std::wstring* key : { &settings.hotkeyUp, &settings.hotkeyDown, &settings.hotkeyMax }) {
            if (!key->empty())
                keys.push_back(*key);
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_hotkeys = keys;
        }

        if (hostHotkeys()) {
            if (m_ownHotkeys.active())
                m_ownHotkeys.stop(); // the host owns the keys now
            winshun::Json::Array array;
            for (const std::wstring& key : keys)
                array.push_back(winshun::utf8(key));
            const Json result = tool("register_hotkeys", Json::Object { { "keys", Json(std::move(array)) } });
            if (result.isObject() && result["error"].isObject()) {
                winshun::debug("WinShunBright: register_hotkeys failed: "
                    + result["error"]["message"].toString());
            }
            return;
        }

        if (keys.empty()) {
            if (m_ownHotkeys.active())
                m_ownHotkeys.setKeys({});
            return;
        }
        if (!m_ownHotkeys.active()) {
            m_ownHotkeys.start(
                [](const std::wstring& key) {
                    if (s_instance)
                        s_instance->onHotkey(key);
                },
                [](const std::wstring& text) { winshun::debug(winshun::utf8(text)); });
        }
        m_ownHotkeys.setKeys(keys);
    }

    // One COM backend for the protocol thread (query/action); creating one
    // per call re-ran CoInitializeEx and WMI/DDC enumeration on every request.
    br::IDisplayBackend* protocolBackend()
    {
        if (!m_protocolBackend) {
            m_protocolBackend = br::createBackend();
            if (m_protocolBackend && !m_protocolBackend->init())
                m_protocolBackend.reset(); // retried on the next request
        }
        return m_protocolBackend.get();
    }

    int warmStrengthSnapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_settings.warmStrength;
    }

    br::Settings settingsSnapshot()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_settings;
    }

    static void enablePerMonitorDpi()
    {
        using SetCtxFn = BOOL(WINAPI*)(HANDLE);
        if (const HMODULE u32 = ::GetModuleHandleW(L"user32.dll")) {
            if (const auto fn = reinterpret_cast<SetCtxFn>(
                    ::GetProcAddress(u32, "SetProcessDpiAwarenessContext"))) {
                // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
                if (fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
                    return;
            }
        }
        ::SetProcessDPIAware();
    }

    void dispatchGesture(const br::GestureDecision& d)
    {
        // Snapshot under the lock: this runs on the hook thread while a
        // settings change may be rewriting m_settings.
        bool all = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            all = m_settings.gestureTarget == br::GestureTarget::All;
        }
        // Stop notification is independent of the configured gesture chord
        // and never swallows the physical click.
        if (d.stopEgg) {
            br::BrightOp stop;
            stop.kind = br::BrightOp::Kind::StopEgg;
            stop.x = d.x;
            stop.y = d.y;
            m_worker.post(std::move(stop));
        }
        br::BrightOp op;
        op.x = d.x;
        op.y = d.y;
        op.eventTimeMs = d.eventTimeMs;
        if (d.kind == br::GestureKind::Wheel) {
            op.kind = br::BrightOp::Kind::Wheel;
            op.steps = d.steps;
        } else if (d.kind == br::GestureKind::Click) {
            op.kind = br::BrightOp::Kind::ToggleWarm;
        } else {
            return;
        }
        // The pointer position is taken at the gesture, so the target panel is
        // the one the user was looking at even if the pointer moves on.
        op.target = all ? br::BrightOp::Target::All : br::BrightOp::Target::UnderMouse;
        m_worker.post(std::move(op));
    }

    // A step hotkey press: named by the spelling that was registered, so a
    // changed key needs no re-mapping. A hotkey has no pointer position, so
    // it always acts on the pointer's panel (or on all, per gestureTarget).
    void onHotkey(const std::wstring& key)
    {
        br::Settings settings;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            settings = m_settings;
        }
        if (key.empty())
            return;
        POINT cursor;
        ::GetCursorPos(&cursor);
        br::BrightOp op;
        op.x = cursor.x;
        op.y = cursor.y;
        op.target = settings.gestureTarget == br::GestureTarget::All
            ? br::BrightOp::Target::All
            : br::BrightOp::Target::UnderMouse;
        if (key == settings.hotkeyUp) {
            op.kind = br::BrightOp::Kind::Wheel;
            op.steps = 1;
        } else if (key == settings.hotkeyDown) {
            op.kind = br::BrightOp::Kind::Wheel;
            op.steps = -1;
        } else if (key == settings.hotkeyMax) {
            op.kind = br::BrightOp::Kind::SetAll;
            op.percent = 100;
        } else {
            return;
        }
        m_worker.post(std::move(op));
    }

    static BrightPlugin* s_instance;

    std::mutex m_mutex;
    std::mutex m_stateMutex;
    PersistedState m_persisted;
    br::Settings m_settings;
    std::vector<std::wstring> m_hotkeys; // registered spellings, protocol thread
    std::wstring m_statePath;
    std::unique_ptr<br::IDisplayBackend> m_protocolBackend; // protocol thread only
    br::BrightWorker m_worker;
    br::MouseHook m_hook;
    br::HotkeyThread m_ownHotkeys; // only when the host cannot register them
};

BrightPlugin* BrightPlugin::s_instance = nullptr;

int main()
{
    BrightPlugin plugin;
    return plugin.serve();
}

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

// {warmEnabled, warmStrength}; a missing or broken file is not an error.
struct PersistedState {
    bool warmEnabled = false;
    int warmStrength = 0;
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

std::wstring monitorSubtitle(const br::MonitorInfo& m, bool zh, const std::wstring& percentText)
{
    const std::wstring type = m.internal ? (zh ? L"内屏" : L"internal")
                                        : (zh ? L"外屏" : L"external");
    std::wstring text;
    // An unsupported display needs the type once, not the channel and the
    // percentage repeating the same word.
    if (!m.adjustable)
        text = type + (zh ? L" · 不支持" : L" · unsupported");
    else {
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
        m_worker.setWarmSink([this](bool on) {
            PersistedState state;
            state.warmEnabled = on;
            state.warmStrength = warmStrengthSnapshot();
            saveState(m_statePath, state);
        });
        m_worker.start();
    }

    ~BrightPlugin() override
    {
        m_hook.stop();
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
        applySettings(br::parseSettings(settings()));
        m_worker.setWarmSink([this](bool on) {
            PersistedState state;
            state.warmEnabled = on;
            state.warmStrength = warmStrengthSnapshot();
            saveState(m_statePath, state);
        });
        if (m_settings.warmRemember) {
            const PersistedState saved = loadState(m_statePath);
            if (saved.warmEnabled && saved.warmStrength > 0)
                m_worker.setWarmEnabled(true);
        }
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
                    winshun::utf8(monitorSubtitle(m, zh, text)));
                it.withData(Json::Object { { "monitor", winshun::utf8(m.id) } });
                items.push_back(std::move(it));
            }
            if (!any && items.empty()) {
                Item none("no-display",
                    zh ? "没有可调节的显示器" : "No adjustable display",
                    zh ? "内屏需要驱动提供 WMI 亮度类，外屏需要支持 DDC/CI"
                       : "An internal panel needs the WMI brightness class; an external one needs DDC/CI");
                none.invalid();
                items.push_back(std::move(none));
            }
            return items;
        }

        // Empty query: one row per monitor.
        Items items;
        for (const br::MonitorInfo& m : monitors) {
            std::wstring percent;
            if (m.adjustable && m.percent >= 0)
                percent = std::to_wstring(m.percent) + L"%";
            else
                percent = zh ? L"不支持" : L"unsupported";

            Item it("monitor:" + winshun::utf8(m.id),
                winshun::utf8(m.name),
                winshun::utf8(monitorSubtitle(m, zh, percent)));
            it.withData(Json::Object { { "monitor", winshun::utf8(m.id) } });
            if (!m.adjustable)
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
        return items;
    }

    Effects action(const ActionRequest& a) override
    {
        const bool zh = chinese();
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
        applySettings(br::parseSettings(s));
    }

private:
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
                });
            }
        } else {
            m_hook.stop();
        }
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
        br::BrightOp op;
        if (d.kind == br::GestureKind::Wheel) {
            op.kind = br::BrightOp::Kind::Wheel;
            op.steps = d.steps;
        } else if (d.kind == br::GestureKind::Click) {
            op.kind = br::BrightOp::Kind::ToggleWarm;
        } else {
            return;
        }
        m_worker.post(std::move(op));
    }

    static BrightPlugin* s_instance;

    std::mutex m_mutex;
    br::Settings m_settings;
    std::wstring m_statePath;
    std::unique_ptr<br::IDisplayBackend> m_protocolBackend; // protocol thread only
    br::BrightWorker m_worker;
    br::MouseHook m_hook;
};

BrightPlugin* BrightPlugin::s_instance = nullptr;

int main()
{
    BrightPlugin plugin;
    return plugin.serve();
}

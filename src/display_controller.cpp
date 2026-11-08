// display_controller.cpp — WMI (built-in panel), DDC/CI (external monitor)
// and gamma-ramp warm filter. One instance per thread: COM objects are
// apartment-bound, so the worker and the protocol thread never share one.
#include "display_controller.h"

#include <windows.h>

#include <lowlevelmonitorconfigurationapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <physicalmonitorenumerationapi.h>
#include <wbemidl.h>

#include <algorithm>
#include <map>
#include <vector>

namespace br {

namespace {

// ---- small helpers -------------------------------------------------------------

std::wstring hardwareIdOf(const std::wstring& instance)
{
    // "DISPLAY\BOE0A1F\5&2B1A..._0" and "MONITOR\BOE0A1F\{...}\0001" both
    // carry the panel id in the second segment.
    const std::size_t first = instance.find(L'\\');
    if (first == std::wstring::npos)
        return {};
    const std::size_t second = instance.find(L'\\', first + 1);
    if (second == std::wstring::npos)
        return instance.substr(first + 1);
    return instance.substr(first + 1, second - first - 1);
}

std::wstring escapeWmi(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size() + 8);
    for (const wchar_t c : s) {
        if (c == L'\\' || c == L'"')
            out += L'\\';
        out += c;
    }
    return out;
}

// The EDID descriptor 0xFC holds the monitor name; it is the only friendly
// name available for a plain DDC monitor.
std::wstring edidName(const std::wstring& deviceId)
{
    if (deviceId.empty())
        return {};
    const std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\" + deviceId + L"\\Device Parameters";
    HKEY hkey = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS)
        return {};
    BYTE buffer[512] = {};
    DWORD size = sizeof buffer;
    DWORD type = 0;
    std::wstring out;
    if (::RegQueryValueExW(hkey, L"EDID", nullptr, &type, buffer, &size) == ERROR_SUCCESS
        && type == REG_BINARY && size >= 128) {
        for (int i = 0; i < 4; ++i) {
            const BYTE* d = buffer + 54 + i * 18;
            if (d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0xFC) {
                for (int k = 5; k < 18; ++k) {
                    const wchar_t c = static_cast<wchar_t>(d[k]);
                    if (c == 0 || c == L'\n' || c == L'\r')
                        break;
                    out += c;
                }
                break;
            }
        }
    }
    ::RegCloseKey(hkey);
    while (!out.empty() && (out.back() == L' ' || out.back() == L'\t'))
        out.pop_back();
    return out;
}

// ---- WMI -----------------------------------------------------------------------

struct WmiPanel {
    std::wstring instance;
    std::wstring hardwareId;
    std::wstring name;
    int current = -1;
};

std::wstring stringProp(IWbemClassObject* obj, const wchar_t* name)
{
    VARIANT v;
    ::VariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR && v.bstrVal)
        out = v.bstrVal;
    ::VariantClear(&v);
    return out;
}

int intProp(IWbemClassObject* obj, const wchar_t* name)
{
    VARIANT v;
    ::VariantInit(&v);
    int out = -1;
    if (SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr))) {
        switch (v.vt) {
        case VT_UI1: out = v.bVal; break;
        case VT_I1: out = v.cVal; break;
        case VT_UI2: out = v.uiVal; break;
        case VT_I2: out = v.iVal; break;
        case VT_UI4: out = static_cast<int>(v.ulVal); break;
        case VT_I4: out = v.lVal; break;
        default: break;
        }
    }
    ::VariantClear(&v);
    return out;
}

// WmiMonitorID.UserFriendlyName is a uint16 array, not a BSTR.
std::wstring u16ArrayProp(IWbemClassObject* obj, const wchar_t* name)
{
    VARIANT v;
    ::VariantInit(&v);
    std::wstring out;
    if (SUCCEEDED(obj->Get(name, 0, &v, nullptr, nullptr)) && (v.vt & VT_ARRAY) && v.parray) {
        VARTYPE element = VT_EMPTY;
        if (SUCCEEDED(::SafeArrayGetVartype(v.parray, &element))
            && (element == VT_UI2 || element == VT_I2)) {
            LONG lower = 0;
            LONG upper = -1;
            if (SUCCEEDED(::SafeArrayGetLBound(v.parray, 1, &lower))
                && SUCCEEDED(::SafeArrayGetUBound(v.parray, 1, &upper))) {
                unsigned short* data = nullptr;
                if (SUCCEEDED(::SafeArrayAccessData(v.parray, reinterpret_cast<void**>(&data)))) {
                    for (LONG i = lower; i <= upper; ++i) {
                        const wchar_t c = static_cast<wchar_t>(data[i - lower]);
                        if (c == 0)
                            break;
                        out += c;
                    }
                    ::SafeArrayUnaccessData(v.parray);
                }
            }
        }
    }
    ::VariantClear(&v);
    return out;
}

class WmiBrightness {
public:
    ~WmiBrightness() { disconnect(); }

    bool connect()
    {
        if (m_services)
            return true;
        IWbemLocator* locator = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
            IID_IWbemLocator, reinterpret_cast<void**>(&locator));
        if (FAILED(hr) || !locator)
            return false;
        BSTR ns = ::SysAllocString(L"ROOT\\WMI");
        hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &m_services);
        ::SysFreeString(ns);
        locator->Release();
        if (FAILED(hr) || !m_services) {
            m_services = nullptr;
            return false;
        }
        ::CoSetProxyBlanket(m_services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        return true;
    }

    void disconnect()
    {
        if (m_services) {
            m_services->Release();
            m_services = nullptr;
        }
    }

    bool connected() const { return m_services != nullptr; }

    std::vector<WmiPanel> panels()
    {
        std::vector<WmiPanel> out;
        if (!m_services)
            return out;

        std::map<std::wstring, std::wstring> names;
        IEnumWbemClassObject* enumerator = nullptr;
        BSTR lang = ::SysAllocString(L"WQL");
        BSTR query = ::SysAllocString(L"SELECT * FROM WmiMonitorID");
        if (SUCCEEDED(m_services->ExecQuery(lang, query,
                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator))
            && enumerator) {
            IWbemClassObject* obj = nullptr;
            ULONG returned = 0;
            while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK && returned == 1) {
                const std::wstring instance = stringProp(obj, L"InstanceName");
                if (!instance.empty())
                    names[instance] = u16ArrayProp(obj, L"UserFriendlyName");
                obj->Release();
                obj = nullptr;
            }
            enumerator->Release();
        }
        ::SysFreeString(lang);
        ::SysFreeString(query);

        enumerator = nullptr;
        lang = ::SysAllocString(L"WQL");
        query = ::SysAllocString(L"SELECT * FROM WmiMonitorBrightness");
        if (SUCCEEDED(m_services->ExecQuery(lang, query,
                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumerator))
            && enumerator) {
            IWbemClassObject* obj = nullptr;
            ULONG returned = 0;
            while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK && returned == 1) {
                WmiPanel panel;
                panel.instance = stringProp(obj, L"InstanceName");
                panel.current = intProp(obj, L"CurrentBrightness");
                panel.hardwareId = hardwareIdOf(panel.instance);
                const auto it = names.find(panel.instance);
                if (it != names.end())
                    panel.name = it->second;
                if (!panel.instance.empty())
                    out.push_back(std::move(panel));
                obj->Release();
                obj = nullptr;
            }
            enumerator->Release();
        }
        ::SysFreeString(lang);
        ::SysFreeString(query);
        return out;
    }

    bool setBrightness(const std::wstring& instance, int percent)
    {
        if (!m_services || instance.empty())
            return false;

        IWbemClassObject* cls = nullptr;
        BSTR clsName = ::SysAllocString(L"WmiMonitorBrightnessMethods");
        HRESULT hr = m_services->GetObject(clsName, 0, nullptr, &cls, nullptr);
        ::SysFreeString(clsName);
        if (FAILED(hr) || !cls)
            return false;

        IWbemClassObject* inParams = nullptr;
        hr = cls->GetMethod(L"WmiSetBrightness", 0, &inParams, nullptr);
        cls->Release();
        if (FAILED(hr) || !inParams)
            return false;

        VARIANT v;
        ::VariantInit(&v);
        v.vt = VT_UI4;
        v.ulVal = 0; // timeout
        inParams->Put(L"Timeout", 0, &v, 0);
        ::VariantClear(&v);
        ::VariantInit(&v);
        v.vt = VT_UI1;
        v.bVal = static_cast<BYTE>(clampPercent(percent));
        inParams->Put(L"Brightness", 0, &v, 0);
        ::VariantClear(&v);

        const std::wstring path = L"WmiMonitorBrightnessMethods.InstanceName=\""
            + escapeWmi(instance) + L"\"";
        BSTR pathB = ::SysAllocString(path.c_str());
        BSTR methodB = ::SysAllocString(L"WmiSetBrightness");
        hr = m_services->ExecMethod(pathB, methodB, 0, nullptr, inParams, nullptr, nullptr);
        ::SysFreeString(pathB);
        ::SysFreeString(methodB);
        inParams->Release();
        return SUCCEEDED(hr);
    }

private:
    IWbemServices* m_services = nullptr;
};

// ---- GDI monitor enumeration ---------------------------------------------------

struct GdiMonitor {
    HMONITOR hmon = nullptr;
    std::wstring device;
    std::wstring deviceId;
    std::wstring deviceString;
};

BOOL CALLBACK enumMonitorProc(HMONITOR hmon, HDC, LPRECT, LPARAM lp)
{
    auto* list = reinterpret_cast<std::vector<GdiMonitor>*>(lp);
    MONITORINFOEXW mi {};
    mi.cbSize = sizeof mi;
    if (!::GetMonitorInfoW(hmon, &mi))
        return TRUE;
    GdiMonitor g;
    g.hmon = hmon;
    g.device = mi.szDevice;
    DISPLAY_DEVICEW dd {};
    dd.cb = sizeof dd;
    if (::EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) {
        g.deviceId = dd.DeviceID;
        g.deviceString = dd.DeviceString;
    }
    list->push_back(std::move(g));
    return TRUE;
}

struct DdcBinding {
    std::wstring id;
    std::vector<PHYSICAL_MONITOR> phys;
};

struct GammaTarget {
    HDC dc = nullptr;
    WORD original[3][256] {};
    bool saved = false;
};

} // namespace

// ---- the real backend ----------------------------------------------------------

class DisplayBackend final : public IDisplayBackend {
public:
    ~DisplayBackend() override
    {
        destroyGamma();
        destroyDdc();
        m_wmi.disconnect();
        if (m_coInit)
            ::CoUninitialize();
    }

    bool init() override
    {
        if (!m_coInit) {
            const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (SUCCEEDED(hr))
                m_coInit = true;
            else if (hr != RPC_E_CHANGED_MODE)
                return false; // already initialized on this thread is fine
        }
        return true;
    }

    void invalidate() override { m_dirty = true; }

    const std::vector<MonitorInfo>& monitors() override
    {
        if (m_dirty)
            rebuild();
        return m_monitors;
    }

    void refreshValues() override
    {
        if (m_dirty)
            rebuild();
        if (m_wmi.connected()) {
            const std::vector<WmiPanel> panels = m_wmi.panels();
            for (MonitorInfo& m : m_monitors) {
                if (m.channel != Channel::Wmi)
                    continue;
                const WmiPanel* panel = panelFor(m.id, panels);
                if (panel)
                    m.percent = panel->current;
            }
        }
        for (MonitorInfo& m : m_monitors) {
            if (m.channel != Channel::Ddc)
                continue;
            HANDLE h = ddcHandle(m.id);
            if (!h)
                continue;
            DWORD current = 0;
            DWORD maximum = 0;
            if (::GetVCPFeatureAndVCPFeatureReply(h, 0x10, nullptr, &current, &maximum)
                && maximum > 0) {
                m.vcpMax = static_cast<int>(maximum);
                m.percent = vcpToPercent(static_cast<int>(current), static_cast<int>(maximum));
            }
        }
    }

    std::optional<int> readPercent(const std::wstring& id) override
    {
        if (m_dirty)
            rebuild();
        for (MonitorInfo& m : m_monitors) {
            if (m.id != id)
                continue;
            if (m.channel == Channel::Wmi) {
                const std::wstring instance = instanceFor(id);
                if (instance.empty())
                    return std::nullopt;
                for (const WmiPanel& p : m_wmi.panels()) {
                    if (p.instance == instance) {
                        m.percent = p.current;
                        break;
                    }
                }
                return m.percent >= 0 ? std::optional<int>(m.percent) : std::nullopt;
            }
            if (m.channel == Channel::Ddc) {
                HANDLE h = ddcHandle(id);
                if (!h)
                    return std::nullopt;
                DWORD current = 0;
                DWORD maximum = 0;
                if (::GetVCPFeatureAndVCPFeatureReply(h, 0x10, nullptr, &current, &maximum)
                    && maximum > 0) {
                    m.vcpMax = static_cast<int>(maximum);
                    m.percent = vcpToPercent(static_cast<int>(current), static_cast<int>(maximum));
                    return m.percent;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    bool setPercent(const std::wstring& id, int percent, int& applied) override
    {
        if (m_dirty)
            rebuild();
        const int target = clampPercent(percent);
        for (MonitorInfo& m : m_monitors) {
            if (m.id != id)
                continue;
            if (m.channel == Channel::Wmi) {
                if (!m_wmi.setBrightness(instanceFor(id), target))
                    return false;
                m.percent = target;
                applied = target;
                return true;
            }
            if (m.channel == Channel::Ddc) {
                HANDLE h = ddcHandle(id);
                if (!h)
                    return false;
                if (!::SetVCPFeature(h, 0x10,
                        static_cast<DWORD>(percentToVcp(target, m.vcpMax))))
                    return false;
                // DDC settles tens of milliseconds later; the worker's
                // read-back timer calibrates the overlay.
                m.percent = target;
                applied = target;
                return true;
            }
            return false;
        }
        return false;
    }

    bool setWarm(int strength) override
    {
        int s = strength;
        if (s < 0)
            s = 0;
        if (s > 100)
            s = 100;
        if (s == 0) {
            restoreWarm();
            return true;
        }
        ensureGamma();
        if (m_gamma.empty())
            return false;

        WORD lut[3][256];
        buildWarmLut(s, lut);
        bool any = false;
        for (GammaTarget& t : m_gamma) {
            if (!t.saved) {
                if (!::GetDeviceGammaRamp(t.dc, t.original))
                    continue;
                t.saved = true;
            }
            if (::SetDeviceGammaRamp(t.dc, lut))
                any = true;
        }
        if (any) {
            m_warmActive = true;
            m_warmStrength = s;
        }
        return any;
    }

    void restoreWarm() override
    {
        for (GammaTarget& t : m_gamma) {
            if (t.saved) {
                ::SetDeviceGammaRamp(t.dc, t.original);
                t.saved = false;
            }
        }
        m_warmActive = false;
    }

    bool warmActive() const override { return m_warmActive; }

private:
    const WmiPanel* panelFor(const std::wstring& id, const std::vector<WmiPanel>& panels) const
    {
        const std::wstring instance = instanceFor(id);
        if (instance.empty())
            return nullptr;
        for (const WmiPanel& p : panels) {
            if (p.instance == instance)
                return &p;
        }
        return nullptr;
    }

    std::wstring instanceFor(const std::wstring& id) const
    {
        const auto it = m_wmiInstances.find(id);
        return it == m_wmiInstances.end() ? std::wstring() : it->second;
    }

    void rebuild()
    {
        // A display change invalidates the gamma DCs too; re-apply the filter
        // afterwards so a hot-plug does not silently drop it.
        const bool warm = m_warmActive;
        const int warmStrength = m_warmStrength;
        destroyGamma();
        destroyDdc();
        m_monitors.clear();
        m_wmiInstances.clear();

        if (!m_wmi.connected())
            m_wmi.connect();
        const std::vector<WmiPanel> panels = m_wmi.connected() ? m_wmi.panels() : std::vector<WmiPanel> {};

        std::vector<GdiMonitor> gdi;
        ::EnumDisplayMonitors(nullptr, nullptr, enumMonitorProc, reinterpret_cast<LPARAM>(&gdi));

        std::vector<MonitorInfo> monitors;
        monitors.reserve(gdi.size());
        for (const GdiMonitor& g : gdi) {
            const std::wstring hardware = hardwareIdOf(g.deviceId);
            const WmiPanel* panel = nullptr;
            for (const WmiPanel& p : panels) {
                if (!hardware.empty() && !p.hardwareId.empty()
                    && _wcsicmp(hardware.c_str(), p.hardwareId.c_str()) == 0) {
                    panel = &p;
                    break;
                }
            }

            MonitorInfo info;
            info.id = g.device;
            info.name = edidName(g.deviceId);
            if (info.name.empty() && panel && !panel->name.empty())
                info.name = panel->name;

            if (panel) {
                info.internal = true;
                info.channel = Channel::Wmi;
                info.percent = panel->current;
                info.adjustable = true; // the method exists even if the read failed
                m_wmiInstances[info.id] = panel->instance;
            } else {
                bindDdc(g, info);
            }

            if (info.name.empty())
                info.name = g.deviceString;
            if (info.name.empty())
                info.name = g.device;
            monitors.push_back(std::move(info));
        }

        m_monitors = normalizeMonitors(std::move(monitors));
        m_dirty = false;
        if (warm)
            setWarm(warmStrength);
    }

    void bindDdc(const GdiMonitor& g, MonitorInfo& info)
    {
        DWORD count = 0;
        if (!::GetNumberOfPhysicalMonitorsFromHMONITOR(g.hmon, &count) || count == 0)
            return;
        std::vector<PHYSICAL_MONITOR> phys(count);
        if (!::GetPhysicalMonitorsFromHMONITOR(g.hmon, count, phys.data()))
            return;

        for (DWORD i = 0; i < count; ++i) {
            DWORD current = 0;
            DWORD maximum = 0;
            if (::GetVCPFeatureAndVCPFeatureReply(phys[i].hPhysicalMonitor, 0x10,
                    nullptr, &current, &maximum)
                && maximum > 0) {
                info.channel = Channel::Ddc;
                info.vcpMax = static_cast<int>(maximum);
                info.percent = vcpToPercent(static_cast<int>(current), static_cast<int>(maximum));
                info.adjustable = true;
                if (info.name.empty())
                    info.name = phys[i].szPhysicalMonitorDescription;
                DdcBinding binding;
                binding.id = g.device;
                binding.phys = std::move(phys);
                m_ddc.push_back(std::move(binding));
                return;
            }
        }
        // No physical monitor of this HMONITOR answered VCP 0x10 (a dock, a
        // converter or a KVM in between): release the handles and stay
        // unadjustable.
        ::DestroyPhysicalMonitors(count, phys.data());
    }

    HANDLE ddcHandle(const std::wstring& id) const
    {
        for (const DdcBinding& b : m_ddc) {
            if (b.id == id && !b.phys.empty())
                return b.phys[0].hPhysicalMonitor;
        }
        return nullptr;
    }

    void destroyDdc()
    {
        for (DdcBinding& b : m_ddc) {
            if (!b.phys.empty())
                ::DestroyPhysicalMonitors(static_cast<DWORD>(b.phys.size()), b.phys.data());
        }
        m_ddc.clear();
    }

    void ensureGamma()
    {
        if (!m_gamma.empty())
            return;
        DISPLAY_DEVICEW dd {};
        for (DWORD i = 0;; ++i) {
            dd.cb = sizeof dd;
            if (!::EnumDisplayDevicesW(nullptr, i, &dd, 0))
                break;
            if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP))
                continue;
            if (dd.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER)
                continue;
            HDC dc = ::CreateDCW(dd.DeviceName, nullptr, nullptr, nullptr);
            if (!dc)
                continue;
            GammaTarget target;
            target.dc = dc;
            m_gamma.push_back(target);
        }
    }

    void destroyGamma()
    {
        restoreWarm();
        for (GammaTarget& t : m_gamma) {
            if (t.dc)
                ::DeleteDC(t.dc);
        }
        m_gamma.clear();
    }

    WmiBrightness m_wmi;
    std::map<std::wstring, std::wstring> m_wmiInstances; // monitor id -> WMI instance
    std::vector<MonitorInfo> m_monitors;
    std::vector<DdcBinding> m_ddc;
    std::vector<GammaTarget> m_gamma;
    bool m_dirty = true;
    bool m_coInit = false;
    bool m_warmActive = false;
    int m_warmStrength = 0;
};

std::unique_ptr<IDisplayBackend> createBackend()
{
    return std::make_unique<DisplayBackend>();
}

} // namespace br

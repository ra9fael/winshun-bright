// hotkey_thread.cpp — see hotkey_thread.h.
#include "hotkey_thread.h"

#include "winshun.h"

#include <algorithm>

namespace br {

namespace {

constexpr UINT kApplyMessage = WM_APP + 11;

std::wstring lowerText(const std::wstring& s)
{
    std::wstring out = s;
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return out;
}

std::wstring trimText(const std::wstring& s)
{
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t'))
        ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t'))
        --b;
    return s.substr(a, b - a);
}

} // namespace

bool parseHotkey(const std::wstring& text, UINT& modifiers, UINT& vk)
{
    modifiers = MOD_NOREPEAT;
    vk = 0;
    const std::wstring trimmed = trimText(text);
    if (trimmed.empty())
        return false;
    std::size_t start = 0;
    for (;;) {
        const std::size_t plus = trimmed.find(L'+', start);
        const std::wstring part = lowerText(trimText(trimmed.substr(start,
            plus == std::wstring::npos ? std::wstring::npos : plus - start)));
        if (part == L"ctrl" || part == L"control")
            modifiers |= MOD_CONTROL;
        else if (part == L"alt")
            modifiers |= MOD_ALT;
        else if (part == L"shift")
            modifiers |= MOD_SHIFT;
        else if (part == L"win" || part == L"meta")
            modifiers |= MOD_WIN;
        else if (part == L"space")
            vk = VK_SPACE;
        else if (part == L"tab")
            vk = VK_TAB;
        else if (part == L"enter" || part == L"return")
            vk = VK_RETURN;
        else if (part == L"printscreen" || part == L"print" || part == L"prtsc")
            vk = VK_SNAPSHOT;
        else if (part.size() == 1 && part[0] < 128
            && ((part[0] >= L'0' && part[0] <= L'9') || (part[0] >= L'a' && part[0] <= L'z')))
            vk = static_cast<UINT>(towupper(part[0]));
        else if (part.size() >= 2 && part.size() <= 3 && part[0] == L'f') {
            const int n = _wtoi(part.c_str() + 1);
            if (n < 1 || n > 24)
                return false;
            vk = static_cast<UINT>(VK_F1 + n - 1);
        } else
            return false;
        if (plus == std::wstring::npos)
            break;
        start = plus + 1;
    }
    return vk != 0;
}

bool HotkeyThread::start(Sink sink, Logger logger)
{
    if (m_running.load())
        return true;
    if (m_thread.joinable())
        m_thread.join();
    m_sink = std::move(sink);
    m_logger = std::move(logger);
    m_running.store(true);
    try {
        m_thread = std::thread([this] { run(); });
    } catch (...) {
        m_running.store(false);
        return false;
    }
    return true;
}

void HotkeyThread::stop()
{
    if (m_thread.joinable()) {
        m_running.store(false);
        ::PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
        m_thread.join();
    }
}

void HotkeyThread::setKeys(const std::vector<std::wstring>& keys)
{
    if (!m_running.load())
        return;
    {
        m_keys = keys;
        m_keysDirty.store(true);
    }
    ::PostThreadMessageW(m_threadId, kApplyMessage, 0, 0);
}

void HotkeyThread::run()
{
    m_threadId = ::GetCurrentThreadId();
    MSG msg;
    // Force the message queue into existence so setKeys() can post to it.
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    std::vector<std::pair<std::wstring, int>> registered; // spelling -> id
    int nextId = 1;

    const auto release = [&registered] {
        for (const auto& [key, id] : registered)
            ::UnregisterHotKey(nullptr, id);
        registered.clear();
    };
    const auto applyKeys = [&] {
        release();
        nextId = 1;
        for (const std::wstring& key : m_keys) {
            if (key.empty())
                continue;
            UINT modifiers = 0;
            UINT vk = 0;
            if (!parseHotkey(key, modifiers, vk)) {
                if (m_logger)
                    m_logger(L"WinShunBright: cannot parse hotkey " + key);
                continue;
            }
            const int id = nextId++;
            if (!::RegisterHotKey(nullptr, id, modifiers, vk)) {
                // Another program holds it: say so once, at registration.
                if (m_logger)
                    m_logger(L"WinShunBright: RegisterHotKey failed for " + key);
                continue;
            }
            registered.emplace_back(key, id);
        }
        m_keysDirty.store(false);
    };

    while (m_running.load()) {
        const BOOL got = ::GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0)
            break;
        if (msg.message == kApplyMessage) {
            applyKeys();
            continue;
        }
        if (msg.message == WM_HOTKEY) {
            const int id = static_cast<int>(msg.wParam);
            for (const auto& [key, registeredId] : registered) {
                if (registeredId == id) {
                    if (m_sink)
                        m_sink(key);
                    break;
                }
            }
            continue;
        }
    }
    release();
    m_running.store(false);
}

} // namespace br

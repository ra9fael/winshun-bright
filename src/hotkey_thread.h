// hotkey_thread.h — the fallback for hosts without register_hotkeys: a
// dedicated thread that owns its own message queue and registers the plugin's
// global hotkeys with RegisterHotKey.
//
// Contract:
//  * setKeys() may be called from any thread; the keys replace the previous
//    set and registration happens on the hotkey thread.
//  * A press is handed to the sink as the spelling that was registered, so
//    the caller maps it back to the action ("up"/"down"/"max").
//  * Nothing is registered for a key Windows refuses (already taken by another
//    program); the failure is reported through the sink's logger instead of
//    leaving the plugin silently hotkey-less.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace br {

class HotkeyThread {
public:
    using Sink = std::function<void(const std::wstring& key)>;
    using Logger = std::function<void(const std::wstring& text)>;

    HotkeyThread() = default;
    ~HotkeyThread() { stop(); }
    HotkeyThread(const HotkeyThread&) = delete;
    HotkeyThread& operator=(const HotkeyThread&) = delete;

    bool start(Sink sink, Logger logger);
    void stop();
    bool active() const { return m_running.load(); }

    // Thread-safe. Empty entries are skipped (a key nobody set).
    void setKeys(const std::vector<std::wstring>& keys);

private:
    void run();
    void apply(const std::vector<std::wstring>& keys);

    std::thread m_thread;
    std::atomic<bool> m_running { false };
    DWORD m_threadId = 0;
    Sink m_sink;
    Logger m_logger;
    std::mutex m_mutex; // guards m_keys against setKeys() while apply() reads it
    std::vector<std::wstring> m_keys;
    std::atomic<bool> m_keysDirty { false };
};

// "Ctrl+Alt+F1" as RegisterHotKey takes it. False when the spelling names no
// key this plugin understands.
bool parseHotkey(const std::wstring& text, UINT& modifiers, UINT& vk);

} // namespace br

// display_controller.h — brightness channels (WMI for built-in panels,
// DDC/CI for external ones) and the gamma-ramp warm filter.
//
// The interface exists so the worker and the protocol thread can each own an
// instance (COM objects are apartment-bound) and so tests could substitute a
// fake. Every COM/WMI/DDC header stays inside display_controller.cpp behind
// the factory.
#pragma once

#include "bright_logic.h"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace br {

class IDisplayBackend {
public:
    virtual ~IDisplayBackend() = default;

    // Prepares COM (WMI). Safe to call once per thread.
    virtual bool init() = 0;

    // Marks the cached monitor list stale; the next monitors() rebuilds it.
    virtual void invalidate() = 0;

    // The cached list, rebuilt when stale. Values are those read at rebuild
    // time; call refreshValues() for a fresh read.
    virtual const std::vector<MonitorInfo>& monitors() = 0;

    // Re-reads every monitor's current brightness into the cache.
    virtual void refreshValues() = 0;

    // Re-reads one monitor; nullopt when it is not adjustable.
    virtual std::optional<int> readPercent(const std::wstring& id) = 0;

    // Sets one monitor. `applied` is the value the channel accepted (the
    // target for DDC, whose read-back settles later). False on failure.
    virtual bool setPercent(const std::wstring& id, int percent, int& applied) = 0;

    // Applies the warm filter at `strength` (0 restores the original ramps).
    virtual bool setWarm(int strength) = 0;
    virtual void restoreWarm() = 0;
    virtual bool warmActive() const = 0;
};

// The real backend. One instance per thread; call init() on that thread.
std::unique_ptr<IDisplayBackend> createBackend();

} // namespace br

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

struct RendererActivityState {
    bool logAvailable{false};
    bool eventSeen{false};
    bool pendingInactive{false};
    std::string activeThreadId;
};

class RendererActivityMonitor {
public:
    void Poll();
    void Reset();

    const RendererActivityState& State() const {
        return state_;
    }

private:
    std::filesystem::path FindLogFile() const;
    void SwitchLog(const std::filesystem::path& path);
    void ReadNewData();
    void ProcessLine(const std::string& line);
    void ApplyPendingInactive();

    std::filesystem::path logPath_;
    std::uintmax_t offset_{0};
    bool dropInitialPartialLine_{false};
    std::string carry_;

    RendererActivityState state_{};
    std::string pendingInactiveThread_;
    std::chrono::steady_clock::time_point pendingInactiveDeadline_{};
};

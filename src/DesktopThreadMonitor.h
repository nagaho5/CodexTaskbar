#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "SimpleJson.h"

struct ThreadMetrics {
    bool connected{false};
    bool valid{false};
    bool activeTurn{false};

    std::string threadId;
    std::string turnId;

    bool hasContext{false};
    int contextPercent{0};
    std::int64_t contextUsedTokens{0};
    std::int64_t contextWindowTokens{0};

    bool hasCache{false};
    int cachePercent{0};
    std::int64_t inputTokens{0};
    std::int64_t cachedInputTokens{0};

    bool hasTurnTiming{false};
    std::int64_t turnStartedAtMs{0};
    std::int64_t turnDurationMs{-1};

    bool hasDiff{false};
    bool diffExact{false};
    int changedFiles{0};
    int addedLines{0};
    int removedLines{0};
};

class DesktopThreadMonitor {
public:
    DesktopThreadMonitor(HWND notifyWindow, UINT notifyMessage);
    ~DesktopThreadMonitor();

    DesktopThreadMonitor(const DesktopThreadMonitor&) = delete;
    DesktopThreadMonitor& operator=(const DesktopThreadMonitor&) = delete;

    void Start();
    void Stop();
    void SetActive(bool active);
    void UpdateConfig(std::string manualThreadId, bool automaticMode);

private:
    struct ThreadState {
        JsonValue document;
        bool hasDocument{false};
        std::int64_t revision{-1};
        std::string ownerClientId;
        std::string activeTurnId;
        bool hasTurnActivity{false};
        bool turnActive{false};
        int lastFiles{0};
        int lastAdded{0};
        int lastRemoved{0};
        bool hasLastDiff{false};
        std::uint64_t updateSequence{0};
    };

    void WorkerMain();
    bool ConnectPipe();
    void DisconnectPipe();
    bool InitializeClient();
    void ApplyCurrentConfig(bool force);
    void FollowThread(const std::string& threadId, bool following);
    bool SendFollowFrame(
        const std::string& threadId,
        bool following,
        const std::string& targetClientId = {});
    bool SendFrame(const std::string& json);
    bool PumpPipe(DWORD timeoutMs);
    void ProcessFrame(const std::string& json);
    void ProcessStreamState(const JsonValue& root);
    void ProcessFollowingBroadcast(const JsonValue& root);
    void SelectAndPublish();
    ThreadMetrics DeriveMetrics(const std::string& threadId, ThreadState& state);
    void Publish(const ThreadMetrics& metrics);
    void PublishUnavailable();

    static bool IsTargetedToUs(const JsonValue& root, const std::string& clientId);
    static std::string EscapeJson(const std::string& value);

    HWND notifyWindow_{nullptr};
    UINT notifyMessage_{0};

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_{false};
    bool active_{false};
    bool automaticMode_{false};
    std::string manualThreadId_;
    std::uint64_t configGeneration_{0};

    HANDLE pipe_{INVALID_HANDLE_VALUE};
    std::string clientId_;
    std::vector<unsigned char> readBuffer_;
    std::map<std::string, ThreadState, std::less<>> states_;
    std::set<std::string, std::less<>> followed_;
    std::string autoSelectedThread_;
    std::string latestRouteThread_;
    std::uint64_t lastAppliedConfigGeneration_{0};
    std::uint64_t updateSequence_{0};
};

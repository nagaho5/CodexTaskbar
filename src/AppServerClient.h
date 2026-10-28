#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

struct QuotaWindow {
    bool valid{false};
    int remainingPercent{0};
    std::int64_t resetsAtUnix{0};
};

struct QuotaSnapshot {
    QuotaWindow fiveHour;
    QuotaWindow sevenDay;
};

class AppServerClient {
public:
    AppServerClient() = default;
    ~AppServerClient();

    AppServerClient(const AppServerClient&) = delete;
    AppServerClient& operator=(const AppServerClient&) = delete;

    bool Connect();
    void Disconnect();
    bool IsConnected() const;
    bool ReadQuota(QuotaSnapshot& snapshot);

private:
    bool StartProcess(const std::wstring& codexPath);
    bool SendLine(const std::string& line);
    bool ReadLine(std::string& line, DWORD timeoutMs);
    bool WaitForResponseId(std::uint64_t id, std::string& line, DWORD timeoutMs);
    static std::optional<std::wstring> FindCodexBinary();
    static bool ParseQuotaResponse(const std::string& json, QuotaSnapshot& snapshot);

    HANDLE process_{nullptr};
    HANDLE processThread_{nullptr};
    HANDLE stdinWrite_{nullptr};
    HANDLE stdoutRead_{nullptr};
    std::string readBuffer_;
    std::uint64_t nextRequestId_{2};
};

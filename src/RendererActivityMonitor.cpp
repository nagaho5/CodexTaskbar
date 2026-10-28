#include "RendererActivityMonitor.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::uintmax_t kInitialTailBytes = 256 * 1024;
constexpr std::size_t kMaxCarryBytes = 64 * 1024;

std::filesystem::path LocalAppDataPath() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0) return {};

    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        value.data(),
        required);
    if (written == 0 || written >= required) return {};

    value.resize(written);
    return std::filesystem::path(value);
}

bool StartsWith(const std::wstring& value, std::wstring_view prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

std::string TrimToken(std::string value) {
    while (!value.empty()) {
        const char ch = value.back();
        if (ch == ',' || ch == '}' || ch == ']' || ch == '"' || ch == '\r') {
            value.pop_back();
            continue;
        }
        break;
    }
    return value;
}

std::string ExtractSpaceDelimitedValue(
    std::string_view line,
    std::string_view key) {
    const std::size_t start = line.find(key);
    if (start == std::string_view::npos) return {};

    const std::size_t valueStart = start + key.size();
    std::size_t end = valueStart;
    while (end < line.size()) {
        const char ch = line[end];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') break;
        ++end;
    }
    return TrimToken(std::string(line.substr(valueStart, end - valueStart)));
}

std::string ExtractJsonString(
    std::string_view line,
    std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\":\"";
    const std::size_t start = line.find(needle);
    if (start == std::string_view::npos) return {};

    const std::size_t valueStart = start + needle.size();
    const std::size_t end = line.find('"', valueStart);
    if (end == std::string_view::npos) return {};
    return std::string(line.substr(valueStart, end - valueStart));
}

bool LooksLikeThreadId(std::string_view value) {
    if (value.size() < 8 || value.size() > 128) return false;
    for (const char ch : value) {
        const bool ok =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_';
        if (!ok) return false;
    }
    return true;
}

} // namespace

void RendererActivityMonitor::Poll() {
    std::filesystem::path candidate = logPath_;

    std::error_code existenceEc;
    if (candidate.empty() ||
        !std::filesystem::is_regular_file(candidate, existenceEc)) {
        candidate = FindLogFile();
    }

    if (candidate.empty()) {
        state_.logAvailable = false;
        ApplyPendingInactive();
        return;
    }

    state_.logAvailable = true;

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(candidate, ec);
    if (ec) {
        state_.logAvailable = false;
        ApplyPendingInactive();
        return;
    }

    if (candidate != logPath_ || size < offset_) {
        SwitchLog(candidate);
    }

    ReadNewData();
    ApplyPendingInactive();
}

void RendererActivityMonitor::Reset() {
    logPath_.clear();
    offset_ = 0;
    dropInitialPartialLine_ = false;
    carry_.clear();
    state_ = {};
    pendingInactiveThread_.clear();
    pendingInactiveDeadline_ = {};
}

std::filesystem::path RendererActivityMonitor::FindLogFile() const {
    const std::filesystem::path localAppData = LocalAppDataPath();
    if (localAppData.empty()) return {};

    std::vector<std::filesystem::path> logDirs;
    logDirs.push_back(
        localAppData /
        L"Packages" /
        L"OpenAI.Codex_2p2nqsd0c76g0" /
        L"LocalCache" /
        L"Local" /
        L"Codex" /
        L"Logs");

    std::error_code ec;
    const std::filesystem::path packages = localAppData / L"Packages";
    if (std::filesystem::is_directory(packages, ec)) {
        for (std::filesystem::directory_iterator it(packages, ec), end;
             !ec && it != end;
             it.increment(ec)) {
            if (!it->is_directory(ec)) continue;
            const std::wstring name = it->path().filename().wstring();
            if (!StartsWith(name, L"OpenAI.Codex_")) continue;

            const auto dir =
                it->path() / L"LocalCache" / L"Local" / L"Codex" / L"Logs";
            if (std::find(logDirs.begin(), logDirs.end(), dir) == logDirs.end()) {
                logDirs.push_back(dir);
            }
        }
    }

    // Non-MSIX fallback for future/alternate desktop packaging.
    logDirs.push_back(localAppData / L"Codex" / L"Logs");

    std::filesystem::path bestNamed;
    std::filesystem::file_time_type bestNamedTime{};
    bool haveNamed = false;
    std::filesystem::path bestFallback;
    std::filesystem::file_time_type bestFallbackTime{};
    bool haveFallback = false;

    auto consider = [](
        const std::filesystem::path& path,
        std::filesystem::path& best,
        std::filesystem::file_time_type& bestTime,
        bool& haveBest) {
        std::error_code localEc;
        if (!std::filesystem::is_regular_file(path, localEc)) return;
        const auto time = std::filesystem::last_write_time(path, localEc);
        if (localEc) return;
        if (!haveBest || time > bestTime) {
            best = path;
            bestTime = time;
            haveBest = true;
        }
    };

    for (const auto& dir : logDirs) {
        std::error_code localEc;
        if (!std::filesystem::is_directory(dir, localEc)) continue;

        // The Desktop process normally writes renderer activity to Codex.log.
        // Prefer that file across all candidate package roots.
        consider(
            dir / L"Codex.log",
            bestNamed,
            bestNamedTime,
            haveNamed);

        for (std::filesystem::directory_iterator it(dir, localEc), end;
             !localEc && it != end;
             it.increment(localEc)) {
            if (!it->is_regular_file(localEc)) continue;
            consider(
                it->path(),
                bestFallback,
                bestFallbackTime,
                haveFallback);
        }
    }

    return haveNamed ? bestNamed : bestFallback;
}

void RendererActivityMonitor::SwitchLog(const std::filesystem::path& path) {
    logPath_ = path;
    carry_.clear();
    pendingInactiveThread_.clear();
    state_.pendingInactive = false;

    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(logPath_, ec);
    if (ec) {
        offset_ = 0;
        dropInitialPartialLine_ = false;
        return;
    }

    if (size > kInitialTailBytes) {
        offset_ = size - kInitialTailBytes;
        dropInitialPartialLine_ = true;
    } else {
        offset_ = 0;
        dropInitialPartialLine_ = false;
    }
}

void RendererActivityMonitor::ReadNewData() {
    if (logPath_.empty()) return;

    std::ifstream input(logPath_, std::ios::binary);
    if (!input) {
        state_.logAvailable = false;
        return;
    }

    input.seekg(static_cast<std::streamoff>(offset_), std::ios::beg);
    if (!input) return;

    std::string chunk(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    offset_ += static_cast<std::uintmax_t>(chunk.size());

    if (chunk.empty()) return;

    if (dropInitialPartialLine_) {
        const std::size_t newline = chunk.find('\n');
        if (newline == std::string::npos) {
            return;
        }
        chunk.erase(0, newline + 1);
        dropInitialPartialLine_ = false;
    }

    carry_ += chunk;

    std::size_t start = 0;
    while (true) {
        const std::size_t newline = carry_.find('\n', start);
        if (newline == std::string::npos) break;

        ProcessLine(carry_.substr(start, newline - start));
        start = newline + 1;
    }

    if (start > 0) {
        carry_.erase(0, start);
    }
    if (carry_.size() > kMaxCarryBytes) {
        carry_.erase(0, carry_.size() - kMaxCarryBytes);
    }
}

void RendererActivityMonitor::ProcessLine(const std::string& line) {
    constexpr std::string_view marker = "thread_stream_view_activity_changed";
    if (line.find(marker) == std::string::npos) return;

    std::string threadId =
        ExtractSpaceDelimitedValue(line, "conversationId=");
    if (threadId.empty()) {
        threadId = ExtractJsonString(line, "conversationId");
    }
    if (!LooksLikeThreadId(threadId)) return;

    bool active = false;
    bool haveActive = false;
    if (line.find("active=true") != std::string::npos ||
        line.find("\"active\":true") != std::string::npos) {
        active = true;
        haveActive = true;
    } else if (line.find("active=false") != std::string::npos ||
               line.find("\"active\":false") != std::string::npos) {
        active = false;
        haveActive = true;
    }
    if (!haveActive) return;

    state_.eventSeen = true;

    if (active) {
        state_.activeThreadId = std::move(threadId);
        pendingInactiveThread_.clear();
        state_.pendingInactive = false;
        return;
    }

    if (threadId == state_.activeThreadId) {
        pendingInactiveThread_ = std::move(threadId);
        pendingInactiveDeadline_ =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        state_.pendingInactive = true;
    }
}

void RendererActivityMonitor::ApplyPendingInactive() {
    if (pendingInactiveThread_.empty()) return;
    if (std::chrono::steady_clock::now() < pendingInactiveDeadline_) return;

    if (state_.activeThreadId == pendingInactiveThread_) {
        state_.activeThreadId.clear();
    }
    pendingInactiveThread_.clear();
    state_.pendingInactive = false;
}

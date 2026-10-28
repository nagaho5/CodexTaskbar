#include "AppServerClient.h"

#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cwchar>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

namespace {

void CloseHandleSafe(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}

std::optional<std::string_view> ExtractObject(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto keyPos = json.find(needle);
    if (keyPos == std::string_view::npos) {
        return std::nullopt;
    }

    auto pos = json.find(':', keyPos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '{') {
        return std::nullopt;
    }

    const std::size_t begin = pos;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                inString = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return json.substr(begin, pos - begin + 1);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> ExtractInteger(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto keyPos = json.find(needle);
    if (keyPos == std::string_view::npos) {
        return std::nullopt;
    }

    auto pos = json.find(':', keyPos + needle.size());
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json.substr(pos, 4) == "null") {
        return std::nullopt;
    }

    bool negative = false;
    if (json[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') {
        return std::nullopt;
    }

    std::int64_t value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        const int digit = json[pos] - '0';
        if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
        ++pos;
    }
    return negative ? -value : value;
}

bool HasTopLevelId(const std::string& json, std::uint64_t expected) {
    const auto id = ExtractInteger(json, "id");
    return id.has_value() && static_cast<std::uint64_t>(*id) == expected;
}

struct ParsedWindow {
    bool present{false};
    int usedPercent{0};
    std::int64_t windowMinutes{0};
    std::int64_t resetsAt{0};
};

ParsedWindow ParseWindow(const std::optional<std::string_view>& object) {
    ParsedWindow result{};
    if (!object) {
        return result;
    }

    const auto used = ExtractInteger(*object, "usedPercent");
    if (!used) {
        return result;
    }

    result.present = true;
    result.usedPercent = static_cast<int>(std::clamp<std::int64_t>(*used, 0, 100));
    result.windowMinutes = ExtractInteger(*object, "windowDurationMins").value_or(0);
    result.resetsAt = ExtractInteger(*object, "resetsAt").value_or(0);
    return result;
}

void AssignWindow(const ParsedWindow& source, QuotaWindow& target) {
    if (!source.present) {
        return;
    }
    target.valid = true;
    target.remainingPercent = std::clamp(100 - source.usedPercent, 0, 100);
    target.resetsAtUnix = source.resetsAt;
}

} // namespace

AppServerClient::~AppServerClient() {
    Disconnect();
}

bool AppServerClient::Connect() {
    Disconnect();

    const auto codexPath = FindCodexBinary();
    if (!codexPath || !StartProcess(*codexPath)) {
        return false;
    }

    const std::string initialize =
        R"({"method":"initialize","id":1,"params":{"clientInfo":{"name":"codex-taskbar","title":"Codex Taskbar","version":"0.1.0"},"capabilities":{"experimentalApi":false,"requestAttestation":false}}})";
    if (!SendLine(initialize)) {
        Disconnect();
        return false;
    }

    std::string response;
    if (!WaitForResponseId(1, response, 8000)) {
        Disconnect();
        return false;
    }

    if (!SendLine(R"({"method":"initialized"})")) {
        Disconnect();
        return false;
    }

    nextRequestId_ = 2;
    return true;
}

void AppServerClient::Disconnect() {
    if (stdinWrite_) {
        FlushFileBuffers(stdinWrite_);
    }
    CloseHandleSafe(stdinWrite_);
    CloseHandleSafe(stdoutRead_);

    if (process_) {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 1500);
        }
    }

    CloseHandleSafe(processThread_);
    CloseHandleSafe(process_);
    readBuffer_.clear();
    nextRequestId_ = 2;
}

bool AppServerClient::IsConnected() const {
    return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

bool AppServerClient::ReadQuota(QuotaSnapshot& snapshot) {
    snapshot = {};
    if (!IsConnected()) {
        return false;
    }

    const auto requestId = nextRequestId_++;
    const std::string request =
        "{\"method\":\"account/rateLimits/read\",\"id\":" + std::to_string(requestId) + "}";
    if (!SendLine(request)) {
        return false;
    }

    std::string response;
    if (!WaitForResponseId(requestId, response, 10000)) {
        return false;
    }

    return ParseQuotaResponse(response, snapshot);
}

bool AppServerClient::StartProcess(const std::wstring& codexPath) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE childStdoutRead = nullptr;
    HANDLE childStdoutWrite = nullptr;
    HANDLE childStdinRead = nullptr;
    HANDLE childStdinWrite = nullptr;

    if (!CreatePipe(&childStdoutRead, &childStdoutWrite, &sa, 0)) {
        return false;
    }
    if (!SetHandleInformation(childStdoutRead, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(childStdoutRead);
        CloseHandle(childStdoutWrite);
        return false;
    }
    if (!CreatePipe(&childStdinRead, &childStdinWrite, &sa, 0)) {
        CloseHandle(childStdoutRead);
        CloseHandle(childStdoutWrite);
        return false;
    }
    if (!SetHandleInformation(childStdinWrite, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(childStdoutRead);
        CloseHandle(childStdoutWrite);
        CloseHandle(childStdinRead);
        CloseHandle(childStdinWrite);
        return false;
    }

    HANDLE nullHandle = CreateFileW(
        L"NUL",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childStdinRead;
    startup.hStdOutput = childStdoutWrite;
    startup.hStdError = nullHandle != INVALID_HANDLE_VALUE ? nullHandle : childStdoutWrite;

    PROCESS_INFORMATION processInfo{};
    std::wstring commandLine = L"\"" + codexPath + L"\" app-server";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    const BOOL started = CreateProcessW(
        codexPath.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &startup,
        &processInfo);

    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);
    if (nullHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(nullHandle);
    }

    if (!started) {
        CloseHandle(childStdoutRead);
        CloseHandle(childStdinWrite);
        return false;
    }

    process_ = processInfo.hProcess;
    processThread_ = processInfo.hThread;
    stdoutRead_ = childStdoutRead;
    stdinWrite_ = childStdinWrite;
    return true;
}

bool AppServerClient::SendLine(const std::string& line) {
    if (!stdinWrite_) {
        return false;
    }

    std::string payload = line;
    payload.push_back('\n');

    DWORD written = 0;
    const BOOL ok = WriteFile(
        stdinWrite_,
        payload.data(),
        static_cast<DWORD>(payload.size()),
        &written,
        nullptr);
    return ok && written == payload.size();
}

bool AppServerClient::ReadLine(std::string& line, DWORD timeoutMs) {
    line.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto newline = readBuffer_.find('\n');
        if (newline != std::string::npos) {
            line = readBuffer_.substr(0, newline);
            readBuffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }

        if (!stdoutRead_) {
            return false;
        }
        if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
            return false;
        }

        DWORD available = 0;
        if (!PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }

        if (available > 0) {
            char buffer[4096];
            DWORD read = 0;
            const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer)));
            if (!ReadFile(stdoutRead_, buffer, wanted, &read, nullptr) || read == 0) {
                return false;
            }
            readBuffer_.append(buffer, read);
            continue;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return false;
}

bool AppServerClient::WaitForResponseId(
    std::uint64_t id,
    std::string& line,
    DWORD timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (!ReadLine(line, static_cast<DWORD>(std::max<std::int64_t>(1, remaining.count())))) {
            return false;
        }
        if (HasTopLevelId(line, id)) {
            return line.find("\"result\"") != std::string::npos;
        }
    }
    return false;
}

std::optional<std::wstring> AppServerClient::FindCodexBinary() {
    wchar_t path[MAX_PATH]{};
    const DWORD found = SearchPathW(nullptr, L"codex.exe", nullptr, MAX_PATH, path, nullptr);
    if (found > 0 && found < MAX_PATH) {
        return std::wstring(path);
    }

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::optional<std::wstring> result;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"codex.exe") != 0) {
                continue;
            }

            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) {
                continue;
            }

            wchar_t imagePath[32768]{};
            DWORD length = static_cast<DWORD>(std::size(imagePath));
            if (QueryFullProcessImageNameW(process, 0, imagePath, &length) && length > 0) {
                result = std::wstring(imagePath, length);
                CloseHandle(process);
                break;
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

bool AppServerClient::ParseQuotaResponse(const std::string& json, QuotaSnapshot& snapshot) {
    const auto resultObject = ExtractObject(json, "result");
    if (!resultObject) {
        return false;
    }
    const auto rateLimits = ExtractObject(*resultObject, "rateLimits");
    if (!rateLimits) {
        return false;
    }

    const ParsedWindow primary = ParseWindow(ExtractObject(*rateLimits, "primary"));
    const ParsedWindow secondary = ParseWindow(ExtractObject(*rateLimits, "secondary"));

    auto assignByDuration = [&snapshot](const ParsedWindow& window) {
        if (!window.present) {
            return;
        }
        if (window.windowMinutes == 300) {
            AssignWindow(window, snapshot.fiveHour);
        } else if (window.windowMinutes == 10080) {
            AssignWindow(window, snapshot.sevenDay);
        }
    };

    assignByDuration(primary);
    assignByDuration(secondary);

    // The historical Codex payload uses primary=5h and secondary=7d.
    // Keep that as a compatibility fallback if a backend omits duration metadata.
    if (!snapshot.fiveHour.valid && primary.present) {
        AssignWindow(primary, snapshot.fiveHour);
    }
    if (!snapshot.sevenDay.valid && secondary.present) {
        AssignWindow(secondary, snapshot.sevenDay);
    }

    return snapshot.fiveHour.valid || snapshot.sevenDay.valid;
}

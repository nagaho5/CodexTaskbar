#include "DesktopThreadMonitor.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace {

constexpr wchar_t kCodexPipe[] = L"\\\\.\\pipe\\codex-ipc";
constexpr std::uint32_t kMaxFrameBytes = 64u * 1024u * 1024u;

const JsonValue* FindAny(const JsonValue& object, std::initializer_list<std::string_view> keys) {
    if (!object.IsObject()) return nullptr;
    for (std::string_view key : keys) {
        if (const JsonValue* value = object.Find(key)) return value;
    }
    return nullptr;
}

std::string StringField(const JsonValue& object, std::initializer_list<std::string_view> keys) {
    const JsonValue* value = FindAny(object, keys);
    return value && value->IsString() ? value->AsString() : std::string();
}

std::int64_t IntegerField(
    const JsonValue& object,
    std::initializer_list<std::string_view> keys,
    std::int64_t fallback = 0) {
    const JsonValue* value = FindAny(object, keys);
    return value && value->IsNumber() ? value->AsInt64(fallback) : fallback;
}

std::string StatusField(const JsonValue& object) {
    const JsonValue* value = FindAny(object, {"status", "state"});
    if (!value) return {};
    if (value->IsString()) return value->AsString();
    if (value->IsObject()) {
        return StringField(*value, {"type", "status", "state"});
    }
    return {};
}

bool IsActiveStatus(std::string value) {
    value.erase(
        std::remove_if(value.begin(), value.end(), [](char ch) {
            return ch == '-' || ch == '_' || ch == ' ';
        }),
        value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "active" || value == "inprogress" || value == "running" ||
           value == "streaming" || value == "thinking" || value == "editing" ||
           value == "writing";
}

const JsonValue* NormalizeUsageRoot(const JsonValue* candidate) {
    if (!candidate || !candidate->IsObject()) return nullptr;

    // Some Desktop builds keep the canonical ThreadTokenUsage under an "info"
    // wrapper. Other builds expose it directly as latestTokenUsageInfo.
    if (const JsonValue* info = candidate->Find("info"); info && info->IsObject()) {
        candidate = info;
    }

    if (FindAny(*candidate, {"modelContextWindow", "model_context_window", "contextWindow", "context_window"}) ||
        FindAny(*candidate, {"last", "lastTokenUsage", "last_token_usage"}) ||
        FindAny(*candidate, {"total", "totalTokenUsage", "total_token_usage"})) {
        return candidate;
    }
    return nullptr;
}

const JsonValue* FindUsageObject(const JsonValue& value, int depth = 0) {
    if (depth > 12) return nullptr;
    if (value.IsObject()) {
        for (std::string_view key : {"latestTokenUsageInfo", "tokenUsage", "token_usage"}) {
            if (const JsonValue* normalized = NormalizeUsageRoot(value.Find(key))) {
                return normalized;
            }
        }
        for (const auto& [_, child] : value.AsObject()) {
            if (const JsonValue* found = FindUsageObject(child, depth + 1)) return found;
        }
    } else if (value.IsArray()) {
        for (const JsonValue& child : value.AsArray()) {
            if (const JsonValue* found = FindUsageObject(child, depth + 1)) return found;
        }
    }
    return nullptr;
}

std::string ReadExactTurnDiff(const JsonValue& turn) {
    const JsonValue* diff = turn.Find("diff");
    if (!diff) return {};

    // This is the field Codex Desktop itself updates from turn/diff/updated.
    // Accept only a direct turn-level aggregate. Never recurse into fileChange
    // fragments because those are intermediate edits, not the net turn diff.
    if (diff->IsString()) {
        return diff->AsString();
    }

    if (diff->IsObject()) {
        for (std::string_view key : {"unifiedDiff", "unified_diff"}) {
            const JsonValue* unified = diff->Find(key);
            if (unified && unified->IsString()) {
                return unified->AsString();
            }
        }
    }

    return {};
}

bool LooksLikeTurnEntity(const JsonValue& value) {
    if (!value.IsObject()) return false;

    // Canonical Desktop IPC turns use turnId; legacy app-server-shaped turns
    // may use id. Do not require status to be a string: newer Desktop builds
    // can represent status as an object such as {type:"active"}.
    const std::string turnId = StringField(value, {"turnId", "turn_id"});
    const std::string legacyId = StringField(value, {"id"});
    if (turnId.empty() && legacyId.empty()) return false;

    // Thread items can also have id/status. Require a turn-shaped field.
    return value.Find("items") != nullptr ||
           value.Find("params") != nullptr ||
           value.Find("diff") != nullptr ||
           value.Find("rootTurnId") != nullptr ||
           value.Find("root_turn_id") != nullptr ||
           value.Find("startedAt") != nullptr ||
           value.Find("started_at") != nullptr ||
           value.Find("completedAt") != nullptr ||
           value.Find("completed_at") != nullptr ||
           value.Find("turnStartedAtMs") != nullptr;
}

const JsonValue* FindCanonicalLatestTurn(const JsonValue& conversation) {
    // Current Codex Desktop stores ordered history here. entries[] can point to
    // several entity types, so walk backward until an entity is actually a Turn.
    const JsonValue* turnHistory = conversation.Find("turnHistory");
    const JsonValue* history =
        turnHistory && turnHistory->IsObject() ? turnHistory->Find("history") : nullptr;
    const JsonValue* entities =
        history && history->IsObject() ? history->Find("entitiesByKey") : nullptr;
    const JsonValue* islands =
        history && history->IsObject() ? history->Find("islands") : nullptr;

    if (entities && entities->IsObject() && islands && islands->IsArray()) {
        const auto& islandArray = islands->AsArray();
        for (auto islandIt = islandArray.rbegin(); islandIt != islandArray.rend(); ++islandIt) {
            if (!islandIt->IsObject()) continue;
            const JsonValue* entries = islandIt->Find("entries");
            if (!entries || !entries->IsArray()) continue;

            const auto& entryArray = entries->AsArray();
            for (auto entryIt = entryArray.rbegin(); entryIt != entryArray.rend(); ++entryIt) {
                if (!entryIt->IsObject()) continue;
                // Current canonical history entries carry both key and value.
                // The entity lookup key is commonly entry.value; entry.key can be
                // an ordering/React key. Resolve value first when it exists in
                // entitiesByKey, then fall back to key for older builds.
                const auto& object = entities->AsObject();
                std::string entityKey = StringField(*entryIt, {"value"});
                auto found = entityKey.empty() ? object.end() : object.find(entityKey);
                if (found == object.end()) {
                    entityKey = StringField(*entryIt, {"key"});
                    found = entityKey.empty() ? object.end() : object.find(entityKey);
                }
                if (found != object.end() && LooksLikeTurnEntity(found->second)) {
                    return &found->second;
                }
            }
        }
    }

    // Legacy Desktop state keeps ordered turns directly in an array.
    const JsonValue* turns = conversation.Find("turns");
    if (turns && turns->IsArray() && !turns->AsArray().empty()) {
        for (auto it = turns->AsArray().rbegin(); it != turns->AsArray().rend(); ++it) {
            if (LooksLikeTurnEntity(*it)) return &*it;
        }
    }

    return nullptr;
}

void CountUnifiedDiff(const std::string& diff, int& files, int& added, int& removed) {
    files = 0;
    added = 0;
    removed = 0;
    int fallbackFileHeaders = 0;

    std::size_t start = 0;
    while (start <= diff.size()) {
        const std::size_t end = diff.find('\n', start);
        const std::string_view line(
            diff.data() + start,
            (end == std::string::npos ? diff.size() : end) - start);

        if (line.starts_with("diff --git ")) {
            ++files;
        } else if (line.starts_with("--- ")) {
            ++fallbackFileHeaders;
        } else if (line.starts_with("+") && !line.starts_with("+++")) {
            ++added;
        } else if (line.starts_with("-") && !line.starts_with("---")) {
            ++removed;
        }

        if (end == std::string::npos) break;
        start = end + 1;
    }

    if (files == 0) {
        files = fallbackFileHeaders;
    }
}

bool CountCompletedFileChangeFallback(
    const JsonValue& turn,
    int& files,
    int& added,
    int& removed) {
    files = 0;
    added = 0;
    removed = 0;

    const JsonValue* items = turn.Find("items");
    if (!items || !items->IsArray()) return false;

    std::set<std::string, std::less<>> changedPaths;
    bool sawCompletedFileChange = false;

    for (const JsonValue& item : items->AsArray()) {
        if (!item.IsObject() ||
            StringField(item, {"type"}) != "fileChange" ||
            StringField(item, {"status"}) != "completed") {
            continue;
        }

        const JsonValue* changes = item.Find("changes");
        if (!changes || !changes->IsArray()) continue;
        sawCompletedFileChange = true;

        for (const JsonValue& change : changes->AsArray()) {
            if (!change.IsObject()) continue;

            std::string path = StringField(change, {"path"});
            std::string kindType;
            std::string movePath;
            if (const JsonValue* kind = change.Find("kind"); kind && kind->IsObject()) {
                kindType = StringField(*kind, {"type"});
                movePath = StringField(*kind, {"move_path", "movePath"});
            }
            if (!movePath.empty()) path = movePath;
            if (!path.empty()) changedPaths.insert(path);

            const std::string diff = StringField(change, {"diff"});
            if (diff.empty()) continue;

            if ((kindType == "add" || kindType == "delete") &&
                diff.find("@@ ") == std::string::npos) {
                int lineCount = 0;
                std::size_t start = 0;
                while (start < diff.size()) {
                    ++lineCount;
                    const std::size_t end = diff.find('\n', start);
                    if (end == std::string::npos) break;
                    start = end + 1;
                    if (start == diff.size()) break;
                }
                if (kindType == "add") added += lineCount;
                else removed += lineCount;
                continue;
            }

            std::size_t start = 0;
            bool inHunk = false;
            while (start <= diff.size()) {
                const std::size_t end = diff.find('\n', start);
                const std::string_view line(
                    diff.data() + start,
                    (end == std::string::npos ? diff.size() : end) - start);

                if (line.starts_with("@@ ")) {
                    inHunk = true;
                } else if (inHunk && line.starts_with("+") && !line.starts_with("+++")) {
                    ++added;
                } else if (inHunk && line.starts_with("-") && !line.starts_with("---")) {
                    ++removed;
                }

                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
    }

    files = static_cast<int>(changedPaths.size());
    return sawCompletedFileChange &&
           (files > 0 || added > 0 || removed > 0);
}

int Percent(std::int64_t numerator, std::int64_t denominator) {
    if (numerator < 0 || denominator <= 0) return 0;
    const double raw = 100.0 * static_cast<double>(numerator) / static_cast<double>(denominator);
    return std::clamp(static_cast<int>(std::lround(raw)), 0, 100);
}

std::uint32_t ReadLe32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) |
           (static_cast<std::uint32_t>(bytes[3]) << 24);
}

std::string MakeRequestId() {
    static std::atomic<std::uint64_t> counter{1};
    return "codextaskbar-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(counter.fetch_add(1));
}

} // namespace

DesktopThreadMonitor::DesktopThreadMonitor(HWND notifyWindow, UINT notifyMessage)
    : notifyWindow_(notifyWindow), notifyMessage_(notifyMessage) {}

DesktopThreadMonitor::~DesktopThreadMonitor() {
    Stop();
}

void DesktopThreadMonitor::Start() {
    if (worker_.joinable()) return;
    worker_ = std::thread([this] { WorkerMain(); });
}

void DesktopThreadMonitor::Stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        active_ = false;
        ++configGeneration_;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    DisconnectPipe();
}

void DesktopThreadMonitor::SetActive(bool active) {
    {
        std::lock_guard lock(mutex_);
        if (active_ == active) return;
        active_ = active;
        ++configGeneration_;
    }
    cv_.notify_all();
}

void DesktopThreadMonitor::UpdateConfig(std::string manualThreadId, bool automaticMode) {
    manualThreadId.erase(
        std::remove_if(manualThreadId.begin(), manualThreadId.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }),
        manualThreadId.end());

    {
        std::lock_guard lock(mutex_);
        manualThreadId_ = std::move(manualThreadId);
        automaticMode_ = automaticMode;
        ++configGeneration_;
    }
    cv_.notify_all();
}

void DesktopThreadMonitor::WorkerMain() {
    for (;;) {
        bool active = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(250), [this] {
                return stop_ || active_;
            });
            if (stop_) break;
            active = active_;
        }

        if (!active) {
            DisconnectPipe();
            PublishUnavailable();
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || active_; });
            if (stop_) break;
            continue;
        }

        if (pipe_ == INVALID_HANDLE_VALUE) {
            if (!ConnectPipe() || !InitializeClient()) {
                DisconnectPipe();
                PublishUnavailable();
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::seconds(2), [this] {
                    return stop_ || !active_ || configGeneration_ != lastAppliedConfigGeneration_;
                });
                if (stop_) break;
                continue;
            }
            lastAppliedConfigGeneration_ = 0;
        }

        ApplyCurrentConfig(false);
        if (!PumpPipe(250)) {
            DisconnectPipe();
            PublishUnavailable();
            continue;
        }

        bool stillActive = false;
        {
            std::lock_guard lock(mutex_);
            if (stop_) break;
            stillActive = active_;
        }
        if (!stillActive) {
            DisconnectPipe();
            PublishUnavailable();
        }
    }
}

bool DesktopThreadMonitor::ConnectPipe() {
    if (!WaitNamedPipeW(kCodexPipe, 750)) return false;

    pipe_ = CreateFileW(
        kCodexPipe,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (pipe_ == INVALID_HANDLE_VALUE) return false;

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe_, &mode, nullptr, nullptr);
    readBuffer_.clear();
    states_.clear();
    followed_.clear();
    clientId_.clear();
    autoSelectedThread_.clear();
    latestRouteThread_.clear();
    return true;
}

void DesktopThreadMonitor::DisconnectPipe() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    readBuffer_.clear();
    clientId_.clear();
    states_.clear();
    followed_.clear();
    autoSelectedThread_.clear();
    latestRouteThread_.clear();
}

bool DesktopThreadMonitor::InitializeClient() {
    const std::string requestId = MakeRequestId();
    const std::string frame =
        "{\"type\":\"request\",\"requestId\":\"" + requestId +
        "\",\"sourceClientId\":\"initializing\",\"version\":0,\"method\":\"initialize\","
        "\"params\":{\"clientType\":\"codex-taskbar\"}}";

    if (!SendFrame(frame)) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available == 0) {
            Sleep(10);
            continue;
        }

        unsigned char chunk[8192];
        DWORD read = 0;
        if (!ReadFile(pipe_, chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr) || read == 0) {
            return false;
        }
        readBuffer_.insert(readBuffer_.end(), chunk, chunk + read);

        while (readBuffer_.size() >= 4) {
            const std::uint32_t size = ReadLe32(readBuffer_.data());
            if (size == 0 || size > kMaxFrameBytes) return false;
            if (readBuffer_.size() < 4ull + size) break;

            std::string json(
                reinterpret_cast<const char*>(readBuffer_.data() + 4),
                static_cast<std::size_t>(size));
            readBuffer_.erase(readBuffer_.begin(), readBuffer_.begin() + static_cast<std::ptrdiff_t>(4ull + size));

            auto root = JsonValue::Parse(json);
            if (!root || !root->IsObject()) continue;
            if (StringField(*root, {"type"}) != "response") continue;
            if (StringField(*root, {"requestId"}) != requestId) continue;

            const JsonValue* result = root->Find("result");
            if (!result || !result->IsObject()) return false;
            clientId_ = StringField(*result, {"clientId"});
            return !clientId_.empty();
        }
    }
    return false;
}

void DesktopThreadMonitor::ApplyCurrentConfig(bool force) {
    std::string manualThread;
    bool automatic = false;
    std::uint64_t generation = 0;

    {
        std::lock_guard lock(mutex_);
        manualThread = manualThreadId_;
        automatic = automaticMode_;
        generation = configGeneration_;
    }

    if (!force && generation == lastAppliedConfigGeneration_) return;
    lastAppliedConfigGeneration_ = generation;

    if (!manualThread.empty()) {
        if (force && followed_.contains(manualThread)) {
            SendFollowFrame(manualThread, true);
        } else {
            FollowThread(manualThread, true);
        }
    }

    if (!automatic) {
        autoSelectedThread_.clear();
        latestRouteThread_.clear();
        for (auto it = followed_.begin(); it != followed_.end();) {
            if (*it != manualThread) {
                const std::string old = *it;
                ++it;
                FollowThread(old, false);
            } else {
                ++it;
            }
        }
    }

    SelectAndPublish();
}

void DesktopThreadMonitor::FollowThread(const std::string& threadId, bool following) {
    if (threadId.empty() || clientId_.empty() || pipe_ == INVALID_HANDLE_VALUE) return;

    if (following) {
        if (!followed_.insert(threadId).second) return;
    } else {
        if (followed_.erase(threadId) == 0) return;
        states_.erase(threadId);
    }

    if (!SendFollowFrame(threadId, following) && following) {
        followed_.erase(threadId);
    }
}

bool DesktopThreadMonitor::SendFollowFrame(
    const std::string& threadId,
    bool following,
    const std::string& targetClientId) {
    if (threadId.empty() || clientId_.empty() || pipe_ == INVALID_HANDLE_VALUE) return false;

    std::string frame =
        "{\"type\":\"broadcast\",\"method\":\"thread-stream-following-changed\","
        "\"sourceClientId\":\"" + EscapeJson(clientId_) +
        "\",\"version\":1,\"params\":{\"conversationId\":\"" + EscapeJson(threadId) +
        "\",\"hostId\":\"local\",\"following\":" + (following ? "true" : "false") + "}";

    if (!targetClientId.empty()) {
        frame += ",\"targetClientIds\":[\"" + EscapeJson(targetClientId) + "\"]";
    }
    frame += "}";
    return SendFrame(frame);
}

bool DesktopThreadMonitor::SendFrame(const std::string& json) {
    if (pipe_ == INVALID_HANDLE_VALUE || json.empty() || json.size() > kMaxFrameBytes) return false;

    const std::uint32_t size = static_cast<std::uint32_t>(json.size());
    unsigned char header[4]{
        static_cast<unsigned char>(size & 0xFF),
        static_cast<unsigned char>((size >> 8) & 0xFF),
        static_cast<unsigned char>((size >> 16) & 0xFF),
        static_cast<unsigned char>((size >> 24) & 0xFF),
    };

    DWORD written = 0;
    if (!WriteFile(pipe_, header, sizeof(header), &written, nullptr) || written != sizeof(header)) {
        return false;
    }
    if (!WriteFile(pipe_, json.data(), size, &written, nullptr) || written != size) {
        return false;
    }
    return true;
}

bool DesktopThreadMonitor::PumpPipe(DWORD timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        bool configChanged = false;
        {
            std::lock_guard lock(mutex_);
            if (stop_ || !active_) return true;
            configChanged = configGeneration_ != lastAppliedConfigGeneration_;
        }
        if (configChanged) {
            ApplyCurrentConfig(true);
        }

        DWORD available = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) return false;

        if (available == 0) {
            if (std::chrono::steady_clock::now() >= deadline) return true;
            Sleep(10);
            continue;
        }

        unsigned char chunk[16384];
        DWORD read = 0;
        if (!ReadFile(pipe_, chunk, std::min<DWORD>(available, sizeof(chunk)), &read, nullptr) || read == 0) {
            return false;
        }
        readBuffer_.insert(readBuffer_.end(), chunk, chunk + read);

        while (readBuffer_.size() >= 4) {
            const std::uint32_t size = ReadLe32(readBuffer_.data());
            if (size == 0 || size > kMaxFrameBytes) return false;
            if (readBuffer_.size() < 4ull + size) break;

            std::string json(
                reinterpret_cast<const char*>(readBuffer_.data() + 4),
                static_cast<std::size_t>(size));
            readBuffer_.erase(
                readBuffer_.begin(),
                readBuffer_.begin() + static_cast<std::ptrdiff_t>(4ull + size));
            ProcessFrame(json);
        }
    }
}

void DesktopThreadMonitor::ProcessFrame(const std::string& json) {
    auto root = JsonValue::Parse(json);
    if (!root || !root->IsObject()) return;

    const std::string type = StringField(*root, {"type"});

    if (type == "client-discovery-request") {
        const std::string requestId = StringField(*root, {"requestId"});
        if (!requestId.empty()) {
            SendFrame(
                "{\"type\":\"client-discovery-response\",\"requestId\":\"" +
                EscapeJson(requestId) +
                "\",\"response\":{\"canHandle\":false}}");
        }
        return;
    }

    if (type != "broadcast") return;
    const std::string method = StringField(*root, {"method"});

    if (method == "turn/started") {
        const JsonValue* params = root->Find("params");
        if (!params || !params->IsObject()) return;

        const std::string threadId = StringField(*params, {"threadId", "conversationId"});
        const JsonValue* turn = params->Find("turn");
        const std::string turnId =
            turn && turn->IsObject() ? StringField(*turn, {"id", "turnId", "turn_id"}) : std::string();
        if (threadId.empty() || !followed_.contains(threadId)) return;

        ThreadState& state = states_[threadId];
        if (!turnId.empty() && turnId != state.activeTurnId) {
            state.activeTurnId = turnId;
            state.hasLastDiff = false;
            state.lastDiffExact = false;
            state.lastFiles = state.lastAdded = state.lastRemoved = 0;
        }
        state.hasTurnActivity = true;
        state.turnActive = true;
        state.updateSequence = ++updateSequence_;
        SelectAndPublish();
        return;
    }

    if (method == "turn/completed") {
        const JsonValue* params = root->Find("params");
        if (!params || !params->IsObject()) return;

        const std::string threadId = StringField(*params, {"threadId", "conversationId"});
        const JsonValue* turn = params->Find("turn");
        const std::string turnId =
            turn && turn->IsObject() ? StringField(*turn, {"id", "turnId", "turn_id"}) : std::string();
        if (threadId.empty() || !followed_.contains(threadId)) return;

        ThreadState& state = states_[threadId];
        if (state.activeTurnId.empty() && !turnId.empty()) {
            state.activeTurnId = turnId;
        }
        if (turnId.empty() || state.activeTurnId.empty() || turnId == state.activeTurnId) {
            state.hasTurnActivity = true;
            state.turnActive = false;
        }
        state.updateSequence = ++updateSequence_;
        SelectAndPublish();
        return;
    }

    if (method == "turn/diff/updated") {
        const JsonValue* params = root->Find("params");
        if (!params || !params->IsObject()) return;

        const std::string threadId = StringField(*params, {"threadId", "conversationId"});
        const std::string turnId = StringField(*params, {"turnId", "turn_id"});
        const JsonValue* diffValue = params->Find("diff");
        if (threadId.empty() || !followed_.contains(threadId) ||
            !diffValue || !diffValue->IsString()) {
            return;
        }

        ThreadState& state = states_[threadId];
        if (!turnId.empty() && !state.activeTurnId.empty() &&
            turnId != state.activeTurnId) {
            state.hasLastDiff = false;
            state.lastDiffExact = false;
            state.lastFiles = state.lastAdded = state.lastRemoved = 0;
        }
        if (!turnId.empty()) state.activeTurnId = turnId;
        state.hasTurnActivity = true;
        state.turnActive = true;

        CountUnifiedDiff(
            diffValue->AsString(),
            state.lastFiles,
            state.lastAdded,
            state.lastRemoved);
        state.hasLastDiff =
            state.lastFiles > 0 || state.lastAdded > 0 || state.lastRemoved > 0;
        state.lastDiffExact = state.hasLastDiff;
        state.updateSequence = ++updateSequence_;
        SelectAndPublish();
        return;
    }

    if (method == "thread-stream-state-changed") {
        if (!IsTargetedToUs(*root, clientId_)) return;
        ProcessStreamState(*root);
        return;
    }

    if (method == "thread-stream-following-changed") {
        ProcessFollowingBroadcast(*root);
        return;
    }

    if (method == "thread-stream-following-status-requested") {
        if (IntegerField(*root, {"version"}, -1) != 1) return;
        const JsonValue* params = root->Find("params");
        if (!params || !params->IsObject() || StringField(*params, {"hostId"}) != "local") return;
        const std::string threadId = StringField(*params, {"conversationId"});
        const std::string requester = StringField(*root, {"sourceClientId"});
        if (!threadId.empty() && !requester.empty() && followed_.contains(threadId)) {
            SendFollowFrame(threadId, true, requester);
        }
        return;
    }

    if (method == "client-status-changed") {
        if (IntegerField(*root, {"version"}, -1) != 0) return;
        const JsonValue* params = root->Find("params");
        if (!params || !params->IsObject()) return;

        const std::string source = StringField(*root, {"sourceClientId"});
        const std::string changedClient = StringField(*params, {"clientId"});
        const std::string status = StringField(*params, {"status"});
        if (source.empty() || source != changedClient || changedClient.empty()) return;

        if (status == "connected") {
            const JsonValue* isSelf = params->Find("isSelf");
            if (!(isSelf && isSelf->IsBool() && isSelf->AsBool())) {
                for (const std::string& threadId : followed_) {
                    SendFollowFrame(threadId, true, changedClient);
                }
            }
        } else if (status == "disconnected") {
            for (auto& [_, state] : states_) {
                if (state.ownerClientId == changedClient) {
                    state.hasDocument = false;
                    state.revision = -1;
                    state.ownerClientId.clear();
                }
            }
            SelectAndPublish();
        }
        return;
    }

    if (method == "ipc-connection-reset") {
        if (IntegerField(*root, {"version"}, -1) != 1) return;
        states_.clear();
        autoSelectedThread_.clear();
        for (const std::string& threadId : followed_) {
            SendFollowFrame(threadId, true);
        }
        SelectAndPublish();
    }
}

void DesktopThreadMonitor::ProcessFollowingBroadcast(const JsonValue& root) {
    bool automatic = false;
    {
        std::lock_guard lock(mutex_);
        automatic = automaticMode_;
    }
    if (!automatic) return;

    const std::string source = StringField(root, {"sourceClientId"});
    if (source.empty() || source == clientId_) return;

    // Targeted frames are retained subscription/status replies, not fresh desktop routing hints.
    if (const JsonValue* targets = root.Find("targetClientIds"); targets && targets->IsArray() && !targets->AsArray().empty()) {
        return;
    }

    const JsonValue* params = root.Find("params");
    if (!params || !params->IsObject()) return;
    if (StringField(*params, {"hostId"}) != "local") return;
    const JsonValue* followingValue = params->Find("following");
    if (!followingValue || !followingValue->IsBool() || !followingValue->AsBool()) return;

    const std::string threadId = StringField(*params, {"conversationId"});
    if (threadId.empty()) return;

    latestRouteThread_ = threadId;
    FollowThread(threadId, true);

    // Bound experimental auto subscriptions. Manual binding is never evicted.
    std::string manualThread;
    {
        std::lock_guard lock(mutex_);
        manualThread = manualThreadId_;
    }
    while (followed_.size() > 6) {
        auto it = std::find_if(followed_.begin(), followed_.end(), [&](const std::string& candidate) {
            return candidate != manualThread && candidate != latestRouteThread_ && candidate != autoSelectedThread_;
        });
        if (it == followed_.end()) break;
        const std::string old = *it;
        FollowThread(old, false);
    }
}

void DesktopThreadMonitor::ProcessStreamState(const JsonValue& root) {
    const JsonValue* params = root.Find("params");
    if (!params || !params->IsObject()) return;
    if (StringField(*params, {"hostId"}) != "local") return;

    const std::string threadId = StringField(*params, {"conversationId"});
    if (threadId.empty() || !followed_.contains(threadId)) return;

    const std::string owner = StringField(root, {"sourceClientId"});
    const JsonValue* change = params->Find("change");
    if (!change || !change->IsObject()) return;

    const std::string changeType = StringField(*change, {"type"});
    const std::int64_t revision = IntegerField(*change, {"revision"}, -1);
    ThreadState& state = states_[threadId];

    if (changeType == "snapshot") {
        const JsonValue* conversation = change->Find("conversationState");
        if (!conversation || !conversation->IsObject() || revision < 0) return;
        state.document = *conversation;
        state.hasDocument = true;
        state.revision = revision;
        state.ownerClientId = owner;
    } else if (changeType == "patches") {
        if (!state.hasDocument || revision < 0) {
            FollowThread(threadId, false);
            FollowThread(threadId, true);
            return;
        }
        const std::int64_t baseRevision = IntegerField(*change, {"baseRevision"}, -1);
        const JsonValue* patches = change->Find("patches");
        if (baseRevision != state.revision || !patches || !ApplyJsonPatch(state.document, *patches)) {
            FollowThread(threadId, false);
            FollowThread(threadId, true);
            return;
        }
        state.revision = revision;
        if (!owner.empty()) state.ownerClientId = owner;
    } else {
        return;
    }

    state.updateSequence = ++updateSequence_;
    ThreadMetrics metrics = DeriveMetrics(threadId, state);

    bool automatic = false;
    {
        std::lock_guard lock(mutex_);
        automatic = automaticMode_;
    }
    if (automatic && metrics.activeTurn) {
        autoSelectedThread_ = threadId;
    }

    SelectAndPublish();
}

void DesktopThreadMonitor::SelectAndPublish() {
    std::string manualThread;
    bool automatic = false;
    {
        std::lock_guard lock(mutex_);
        manualThread = manualThreadId_;
        automatic = automaticMode_;
    }

    std::string selected;
    if (automatic) {
        if (!autoSelectedThread_.empty() && states_.contains(autoSelectedThread_)) {
            selected = autoSelectedThread_;
        } else {
            std::uint64_t newest = 0;
            for (auto& [threadId, state] : states_) {
                ThreadMetrics metrics = DeriveMetrics(threadId, state);
                if (metrics.activeTurn && state.updateSequence >= newest) {
                    newest = state.updateSequence;
                    selected = threadId;
                }
            }
            if (selected.empty() && !latestRouteThread_.empty() && states_.contains(latestRouteThread_)) {
                selected = latestRouteThread_;
            }
        }
    }

    if (selected.empty() && !manualThread.empty() && states_.contains(manualThread)) {
        selected = manualThread;
    }

    if (selected.empty()) {
        ThreadMetrics empty;
        empty.connected = pipe_ != INVALID_HANDLE_VALUE && !clientId_.empty();
        empty.threadId = automatic ? latestRouteThread_ : manualThread;
        Publish(empty);
        return;
    }

    ThreadMetrics metrics = DeriveMetrics(selected, states_[selected]);
    metrics.connected = true;
    Publish(metrics);
}

ThreadMetrics DesktopThreadMonitor::DeriveMetrics(const std::string& threadId, ThreadState& state) {
    ThreadMetrics metrics;
    metrics.connected = true;
    metrics.threadId = threadId;
    if (!state.hasDocument) return metrics;

    metrics.valid = true;

    if (const JsonValue* usage = FindUsageObject(state.document)) {
        const JsonValue* last = FindAny(*usage, {"last", "lastTokenUsage", "last_token_usage"});
        const JsonValue* cumulative = FindAny(*usage, {"total", "totalTokenUsage", "total_token_usage"});

        // Context load must use the latest request's active-context usage. Using
        // lifetime cumulative tokens here can exceed the model context window.
        if (last && last->IsObject()) {
            const std::int64_t window =
                IntegerField(*usage, {"modelContextWindow", "model_context_window", "contextWindow", "context_window"}, 0);
            const std::int64_t activeContext =
                IntegerField(*last, {"totalTokens", "total_tokens"}, 0);
            if (window > 0 && activeContext >= 0) {
                metrics.hasContext = true;
                metrics.contextUsedTokens = activeContext;
                metrics.contextWindowTokens = window;
                metrics.contextPercent = Percent(activeContext, window);
            }
        }

        // Cache hit rate is intentionally cumulative for the bound conversation.
        // A single newest request can legitimately be ~100% cached and is much
        // noisier than the stable conversation-wide ratio shown by status tools.
        if (cumulative && cumulative->IsObject()) {
            const std::int64_t input =
                IntegerField(*cumulative, {"inputTokens", "input_tokens"}, 0);
            const std::int64_t cached =
                IntegerField(*cumulative, {"cachedInputTokens", "cached_input_tokens"}, 0);
            if (input > 0 && cached >= 0) {
                metrics.hasCache = true;
                metrics.inputTokens = input;
                metrics.cachedInputTokens = cached;
                metrics.cachePercent = Percent(cached, input);
            }
        }
    }

    const JsonValue* latestTurn = FindCanonicalLatestTurn(state.document);
    if (latestTurn) {
        const std::string turnId = StringField(*latestTurn, {"turnId", "turn_id", "id"});
        const std::string status = StatusField(*latestTurn);
        const bool snapshotActive = IsActiveStatus(status);

        // Live turn/* notifications are authoritative once observed. A follower
        // snapshot may briefly lag behind them; never let a stale history entity
        // erase the exact aggregate received from turn/diff/updated.
        if (state.activeTurnId.empty() && !turnId.empty()) {
            state.activeTurnId = turnId;
        } else if (!turnId.empty() && turnId != state.activeTurnId &&
                   snapshotActive && (!state.hasTurnActivity || !state.turnActive)) {
            // Recovery path when CodexTaskbar attached after turn/started.
            state.activeTurnId = turnId;
            state.hasLastDiff = false;
            state.lastDiffExact = false;
            state.lastFiles = state.lastAdded = state.lastRemoved = 0;
            state.hasTurnActivity = false;
        }

        metrics.turnId = state.activeTurnId.empty() ? turnId : state.activeTurnId;
        metrics.activeTurn = state.hasTurnActivity
            ? state.turnActive
            : (turnId == state.activeTurnId && snapshotActive);

        const std::int64_t turnStartedAtMs =
            IntegerField(*latestTurn, {"turnStartedAtMs", "turn_started_at_ms"}, 0);
        const std::int64_t firstWorkItemStartedAtMs =
            IntegerField(*latestTurn, {"firstTurnWorkItemStartedAtMs", "first_turn_work_item_started_at_ms"}, 0);
        const std::int64_t durationMs =
            IntegerField(*latestTurn, {"durationMs", "duration_ms"}, -1);
        metrics.turnStartedAtMs =
            turnStartedAtMs > 0 ? turnStartedAtMs : firstWorkItemStartedAtMs;
        metrics.turnDurationMs = durationMs;
        metrics.hasTurnTiming =
            metrics.turnStartedAtMs > 0 || metrics.turnDurationMs >= 0;

        // Snapshot hydration is useful when the taskbar starts mid-turn or after
        // completion, but only for the same confirmed Turn.
        if (state.activeTurnId.empty() || turnId == state.activeTurnId) {
            const std::string exactDiff = ReadExactTurnDiff(*latestTurn);
            if (!exactDiff.empty()) {
                CountUnifiedDiff(
                    exactDiff,
                    state.lastFiles,
                    state.lastAdded,
                    state.lastRemoved);
                state.hasLastDiff =
                    state.lastFiles > 0 || state.lastAdded > 0 || state.lastRemoved > 0;
                state.lastDiffExact = state.hasLastDiff;
            } else {
                // Desktop can keep edits only as completed fileChange items and
                // omit turn.diff for follower clients. Use a current-turn-only
                // fallback so live progress remains visible; exact diff wins
                // automatically whenever it is published.
                int fallbackFiles = 0;
                int fallbackAdded = 0;
                int fallbackRemoved = 0;
                if (CountCompletedFileChangeFallback(
                        *latestTurn,
                        fallbackFiles,
                        fallbackAdded,
                        fallbackRemoved)) {
                    state.lastFiles = fallbackFiles;
                    state.lastAdded = fallbackAdded;
                    state.lastRemoved = fallbackRemoved;
                    state.hasLastDiff = true;
                    state.lastDiffExact = false;
                }
            }
        }
    } else if (state.hasTurnActivity) {
        metrics.turnId = state.activeTurnId;
        metrics.activeTurn = state.turnActive;
    }

    if (state.hasLastDiff) {
        metrics.hasDiff = true;
        metrics.diffExact = state.lastDiffExact;
        metrics.changedFiles = state.lastFiles;
        metrics.addedLines = state.lastAdded;
        metrics.removedLines = state.lastRemoved;
    }

    return metrics;
}

void DesktopThreadMonitor::Publish(const ThreadMetrics& metrics) {
    if (!notifyWindow_ || !IsWindow(notifyWindow_)) return;
    auto* payload = new ThreadMetrics(metrics);
    if (!PostMessageW(notifyWindow_, notifyMessage_, 0, reinterpret_cast<LPARAM>(payload))) {
        delete payload;
    }
}

void DesktopThreadMonitor::PublishUnavailable() {
    ThreadMetrics metrics;
    Publish(metrics);
}

bool DesktopThreadMonitor::IsTargetedToUs(const JsonValue& root, const std::string& clientId) {
    const JsonValue* targets = root.Find("targetClientIds");
    if (!targets || !targets->IsArray() || targets->AsArray().empty()) return true;
    for (const JsonValue& value : targets->AsArray()) {
        if (value.IsString() && value.AsString() == clientId) return true;
    }
    return false;
}

std::string DesktopThreadMonitor::EscapeJson(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(hex[(ch >> 4) & 0xF]);
                result.push_back(hex[ch & 0xF]);
            } else {
                result.push_back(static_cast<char>(ch));
            }
        }
    }
    return result;
}

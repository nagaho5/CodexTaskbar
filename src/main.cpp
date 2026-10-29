#include "AppServerClient.h"
#include "DesktopThreadMonitor.h"
#include "RendererActivityMonitor.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kMainClass[] = L"CodexTaskbar.Main";
constexpr wchar_t kWidgetClass[] = L"CodexTaskbar.Widget";
constexpr wchar_t kHoverClass[] = L"CodexTaskbar.Hover";
constexpr wchar_t kSettingsClass[] = L"CodexTaskbar.Settings";
constexpr wchar_t kSettingsKey[] = L"Software\\Nagaho5\\CodexTaskbar";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"CodexTaskbar";
constexpr wchar_t kTrafficMonitorTitle[] = L"TrafficMonitorTaskbarWindow";

constexpr UINT kTrayId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kQuotaMessage = WM_APP + 2;
constexpr UINT kQuotaErrorMessage = WM_APP + 3;
constexpr UINT kThreadMetricsMessage = WM_APP + 4;
constexpr UINT_PTR kTickTimer = 1;
constexpr UINT_PTR kHoverTimer = 2;
constexpr UINT_PTR kBlinkTimer = 3;
constexpr UINT_PTR kAnchorReadyTimer = 4;

constexpr UINT kMenuSettings = 1001;
constexpr UINT kMenuAutoStart = 1002;
constexpr UINT kMenuReconnect = 1003;
constexpr UINT kMenuAbout = 1004;
constexpr UINT kMenuExit = 1005;
constexpr UINT kMenuDisplayPrimary = 1050;
constexpr UINT kMenuDisplaySecondary = 1051;
constexpr UINT kMenuThreadBase = 1100;
constexpr UINT kMenuThreadLimit = 50;

constexpr int kEditFontSize = 2001;
constexpr int kCheckAutoStart = 2002;
constexpr int kCheckShowFiveHourPercent = 2003;
constexpr int kCheckShowFiveHourTime = 2004;
constexpr int kCheckShowSevenDayPercent = 2005;
constexpr int kCheckShowContext = 2006;
constexpr int kCheckShowCache = 2007;
constexpr int kCheckShowDiff = 2008;
constexpr int kRadioManualThread = 2009;
constexpr int kRadioAutoThread = 2010;
constexpr int kEditThreadId = 2011;
constexpr int kCheckShowDiffFiles = 2012;
constexpr int kCheckShowTaskDuration = 2013;
constexpr int kButtonWorkingColor = 2014;
constexpr int kStaticWorkingColorPreview = 2015;
constexpr int kCheckContextWarningColors = 2016;
constexpr int kCheckQuotaWarningColors = 2017;
constexpr int kCheckDurationUnits = 2018;
constexpr int kCheckBlinkOnUserInput = 2019;
constexpr int kCheckHidePercentSigns = 2020;
constexpr int kCheckNarrowFont = 2021;
constexpr int kCheckAutoFitFont = 2022;
constexpr int kComboCacheDisplayMode = 2023;
constexpr int kCheckShowGenerationSpeed = 2024;
constexpr int kComboFontChoice = 2025;
constexpr int kEditSavedThreads = 2026;
constexpr int kComboGenerationSpeedMode = 2027;
constexpr int kCheckCompactHourUnderline = 2028;

constexpr COLORREF kDefaultWorkingColor = RGB(79, 195, 247);
constexpr COLORREF kWarningYellow = RGB(255, 196, 64);
constexpr COLORREF kWarningRed = RGB(242, 88, 88);

struct ColoredSegment {
    std::wstring text;
    COLORREF color{RGB(255, 255, 255)};
    bool workingUnderline{false};
    bool hidden{false};
};

struct RowFitCache {
    std::wstring signature;
    int availableWidth{-1};
    HFONT font{nullptr};
};

struct QuotaPayload {
    QuotaSnapshot snapshot;
};

struct SavedThreadEntry {
    std::wstring name;
    std::wstring id;
};

struct GenerationSpeedSample {
    double speed{0.0};
    std::int64_t intervalMs{0};
};

struct TaskbarCandidate {
    HWND window{nullptr};
    bool primary{false};
    std::wstring deviceName;
};

std::vector<SavedThreadEntry> ParseSavedThreads(const std::wstring& value) {
    std::vector<SavedThreadEntry> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find_first_of(L"\r\n", start);
        std::wstring line = value.substr(
            start,
            end == std::wstring::npos ? std::wstring::npos : end - start);

        while (!line.empty() && iswspace(line.front())) line.erase(line.begin());
        while (!line.empty() && iswspace(line.back())) line.pop_back();

        if (!line.empty()) {
            const std::size_t separator = line.find(L'|');
            if (separator != std::wstring::npos) {
                std::wstring name = line.substr(0, separator);
                std::wstring id = line.substr(separator + 1);
                while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
                while (!name.empty() && iswspace(name.back())) name.pop_back();
                while (!id.empty() && iswspace(id.front())) id.erase(id.begin());
                while (!id.empty() && iswspace(id.back())) id.pop_back();
                if (!name.empty() && !id.empty()) {
                    result.push_back({std::move(name), std::move(id)});
                }
            }
        }

        if (end == std::wstring::npos) break;
        start = end + 1;
        while (start < value.size() && (value[start] == L'\r' || value[start] == L'\n')) {
            ++start;
        }
    }
    if (result.size() > kMenuThreadLimit) result.resize(kMenuThreadLimit);
    return result;
}

std::wstring SerializeSavedThreads(const std::vector<SavedThreadEntry>& entries) {
    std::wstring result;
    for (const auto& entry : entries) {
        if (entry.name.empty() || entry.id.empty()) continue;
        if (!result.empty()) result += L"\r\n";
        result += entry.name + L"|" + entry.id;
    }
    return result;
}

std::wstring ModulePath() {
    wchar_t buffer[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return len > 0 ? std::wstring(buffer, len) : std::wstring();
}

bool IsCodexProcess(DWORD processId) {
    if (processId == 0) return false;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return false;

    wchar_t imagePath[32768]{};
    DWORD length = static_cast<DWORD>(std::size(imagePath));
    const bool queried =
        QueryFullProcessImageNameW(process, 0, imagePath, &length) && length > 0;
    CloseHandle(process);
    if (!queried) return false;

    std::wstring path(imagePath, length);
    std::transform(path.begin(), path.end(), path.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });

    return path.find(L"\\windowsapps\\openai.codex_") != std::wstring::npos ||
           path.find(L"\\openai\\codex\\") != std::wstring::npos;
}

HWND FindCodexMainWindow() {
    struct SearchState {
        HWND window{nullptr};
    } state;

    EnumWindows(
        [](HWND hwnd, LPARAM value) -> BOOL {
            auto* state = reinterpret_cast<SearchState*>(value);

            // Only consider real top-level application windows. Electron may
            // leave helper/background processes and hidden utility windows alive
            // after the user closes the Codex Desktop window.
            if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

            const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            if ((exStyle & WS_EX_TOOLWINDOW) != 0) return TRUE;

            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);
            if (!IsCodexProcess(processId)) return TRUE;

            // A minimized main window still counts as open. A fully hidden
            // non-minimized helper window does not.
            if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) return TRUE;

            RECT rect{};
            if (!GetWindowRect(hwnd, &rect)) return TRUE;
            if (!IsIconic(hwnd) &&
                (rect.right - rect.left < 120 || rect.bottom - rect.top < 80)) {
                return TRUE;
            }

            wchar_t title[256]{};
            GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
            if (title[0] == L'\0' && !IsIconic(hwnd)) return TRUE;

            state->window = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&state));

    return state.window;
}

bool IsCodexDesktopRunning() {
    // Visibility follows the lifetime of the real Codex Desktop window, not
    // package-process lifetime. This keeps the widget visible when Codex is
    // minimized, while still hiding it when the window is closed even if
    // ChatGPT.exe/Codex.exe background processes remain alive.
    return FindCodexMainWindow() != nullptr;
}

std::vector<TaskbarCandidate> EnumerateTaskbars() {
    std::vector<TaskbarCandidate> result;

    EnumWindows(
        [](HWND hwnd, LPARAM value) -> BOOL {
            auto* result =
                reinterpret_cast<std::vector<TaskbarCandidate>*>(value);

            wchar_t className[128]{};
            if (!GetClassNameW(
                    hwnd,
                    className,
                    static_cast<int>(std::size(className)))) {
                return TRUE;
            }

            const bool taskbarClass =
                wcscmp(className, L"Shell_TrayWnd") == 0 ||
                wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
            if (!taskbarClass) return TRUE;

            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
            if (!monitor) return TRUE;

            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(monitor, &info)) return TRUE;

            result->push_back({
                hwnd,
                (info.dwFlags & MONITORINFOF_PRIMARY) != 0,
                info.szDevice});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&result));

    std::sort(
        result.begin(),
        result.end(),
        [](const TaskbarCandidate& left, const TaskbarCandidate& right) {
            if (left.primary != right.primary) return left.primary > right.primary;
            return left.deviceName < right.deviceName;
        });
    return result;
}

HWND FindDescendantByTitle(HWND root, const wchar_t* title) {
    struct Context {
        const wchar_t* title;
        HWND found;
    } context{title, nullptr};

    EnumChildWindows(
        root,
        [](HWND hwnd, LPARAM value) -> BOOL {
            auto* context = reinterpret_cast<Context*>(value);
            wchar_t text[256]{};
            GetWindowTextW(hwnd, text, static_cast<int>(std::size(text)));
            if (wcscmp(text, context->title) == 0) {
                context->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));

    return context.found;
}

HWND FindDescendantByClass(HWND root, const wchar_t* className) {
    struct Context {
        const wchar_t* className;
        HWND found;
    } context{className, nullptr};

    EnumChildWindows(
        root,
        [](HWND hwnd, LPARAM value) -> BOOL {
            auto* context = reinterpret_cast<Context*>(value);
            wchar_t name[256]{};
            GetClassNameW(hwnd, name, static_cast<int>(std::size(name)));
            if (wcscmp(name, context->className) == 0) {
                context->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));

    return context.found;
}

RECT ScreenRectToClient(HWND parent, RECT rect) {
    POINT points[2]{{rect.left, rect.top}, {rect.right, rect.bottom}};
    MapWindowPoints(HWND_DESKTOP, parent, points, 2);
    return RECT{points[0].x, points[0].y, points[1].x, points[1].y};
}

std::wstring FormatResetTime(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) {
        return L"--:--";
    }

    const time_t value = static_cast<time_t>(unixSeconds);
    tm local{};
    if (localtime_s(&local, &value) != 0) {
        return L"--:--";
    }

    wchar_t text[16]{};
    swprintf_s(text, L"%02d:%02d", local.tm_hour, local.tm_min);
    return text;
}

int ReadRegistryDword(const wchar_t* name, int fallback) {
    HKEY key = nullptr;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        const LONG result = RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(&value),
            &size);
        RegCloseKey(key);
        if (result == ERROR_SUCCESS && type == REG_DWORD) {
            return static_cast<int>(value);
        }
    }
    return fallback;
}

void WriteRegistryDword(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kSettingsKey,
            0,
            nullptr,
            0,
            KEY_WRITE,
            nullptr,
            &key,
            nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(
            key,
            name,
            0,
            REG_DWORD,
            reinterpret_cast<const BYTE*>(&value),
            sizeof(value));
        RegCloseKey(key);
    }
}

std::wstring ReadRegistryString(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }

    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(wchar_t)) {
        RegCloseKey(key);
        return {};
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(
            key,
            name,
            nullptr,
            &type,
            reinterpret_cast<BYTE*>(value.data()),
            &bytes) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return {};
    }
    RegCloseKey(key);

    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

void WriteRegistryString(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kSettingsKey,
            0,
            nullptr,
            0,
            KEY_WRITE,
            nullptr,
            &key,
            nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(
            key,
            name,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int chars = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring result(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), chars);
    return result;
}

std::wstring CompactThreadId(const std::string& value) {
    if (value.empty()) return L"暂无";
    const std::wstring wide = Utf8ToWide(value);
    if (wide.size() <= 18) return wide;
    return wide.substr(0, 8) + L"…" + wide.substr(wide.size() - 6);
}

std::int64_t CurrentUnixMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring FormatDurationCompact(std::int64_t milliseconds) {
    if (milliseconds < 0) return {};
    const std::int64_t totalSeconds = milliseconds / 1000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds % 3600) / 60;
    const std::int64_t seconds = totalSeconds % 60;

    wchar_t text[32]{};
    if (hours > 0) {
        swprintf_s(text, L"%lld:%02lld:%02lld",
            static_cast<long long>(hours),
            static_cast<long long>(minutes),
            static_cast<long long>(seconds));
    } else {
        swprintf_s(text, L"%lld:%02lld",
            static_cast<long long>(minutes),
            static_cast<long long>(seconds));
    }
    return text;
}

std::wstring FormatDurationVerbose(std::int64_t milliseconds) {
    if (milliseconds < 0) return {};
    const std::int64_t totalSeconds = milliseconds / 1000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds % 3600) / 60;
    const std::int64_t seconds = totalSeconds % 60;

    std::wstring result;
    if (hours > 0) {
        result += std::to_wstring(hours) + L"小时";
    }
    if (minutes > 0 || hours > 0) {
        result += std::to_wstring(minutes) + L"分";
    }
    result += std::to_wstring(seconds) + L"秒";
    return result;
}

std::wstring FormatDurationUnits(std::int64_t milliseconds) {
    if (milliseconds < 0) return {};
    const std::int64_t totalSeconds = milliseconds / 1000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds % 3600) / 60;
    const std::int64_t seconds = totalSeconds % 60;

    wchar_t text[32]{};
    if (hours > 0) {
        // Compact hour form: no leading zero for hours/minutes, seconds stay
        // two digits, and the trailing "s" is omitted (e.g. 1h5m02).
        swprintf_s(text, L"%lldh%lldm%02lld",
            static_cast<long long>(hours),
            static_cast<long long>(minutes),
            static_cast<long long>(seconds));
    } else {
        // Sub-hour h/m/s mode intentionally keeps its existing format.
        swprintf_s(text, L"%lldm%02llds",
            static_cast<long long>(minutes),
            static_cast<long long>(seconds));
    }
    return text;
}

std::wstring FormatElapsedAge(std::int64_t timestampMs) {
    if (timestampMs <= 0) return L"暂无成功记录";
    const std::int64_t deltaMs = std::max<std::int64_t>(
        0,
        CurrentUnixMilliseconds() - timestampMs);
    const std::int64_t totalSeconds = deltaMs / 1000;
    if (totalSeconds < 60) {
        return std::to_wstring(totalSeconds) + L"秒前";
    }
    const std::int64_t totalMinutes = totalSeconds / 60;
    if (totalMinutes < 60) {
        return std::to_wstring(totalMinutes) + L"分钟前";
    }
    const std::int64_t hours = totalMinutes / 60;
    const std::int64_t minutes = totalMinutes % 60;
    if (minutes == 0) {
        return std::to_wstring(hours) + L"小时前";
    }
    return std::to_wstring(hours) + L"小时" + std::to_wstring(minutes) + L"分钟前";
}

std::wstring FormatInteger(std::int64_t value) {
    std::wstring raw = std::to_wstring(value);
    std::wstring result;
    result.reserve(raw.size() + raw.size() / 3);
    const std::size_t digits = raw.size();
    for (std::size_t i = 0; i < digits; ++i) {
        if (i > 0 && (digits - i) % 3 == 0) result.push_back(L',');
        result.push_back(raw[i]);
    }
    return result;
}

bool SetAutoStartRegistry(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled) {
        std::wstring path = ModulePath();
        if (path.empty()) {
            RegCloseKey(key);
            return false;
        }
        std::wstring command = L"\"" + path + L"\"";
        result = RegSetValueExW(
            key,
            kRunValue,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        result = RegDeleteValueW(key, kRunValue);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

} // namespace

class CodexTaskbarApp {
public:
    explicit CodexTaskbarApp(HINSTANCE instance) : instance_(instance) {}

    ~CodexTaskbarApp() {
        if (threadMonitor_) {
            threadMonitor_->Stop();
        }
        ShutdownWorker();
        RemoveTrayIcon();
        if (font_) {
            DeleteObject(font_);
        }
        ClearRowFitCaches();
        if (widgetFont_ && widgetFont_ != font_) {
            DeleteObject(widgetFont_);
        }
        if (darkBackgroundBrush_) {
            DeleteObject(darkBackgroundBrush_);
        }
        if (darkControlBrush_) {
            DeleteObject(darkControlBrush_);
        }
        if (hoverWindow_ && IsWindow(hoverWindow_)) {
            DestroyWindow(hoverWindow_);
        }
        if (darkMenuBrush_) {
            DeleteObject(darkMenuBrush_);
        }
    }

    bool Initialize() {
        const int savedFontTenths = ReadRegistryDword(L"FontSizeTenths", -1);
        fontSize_ = static_cast<double>(std::clamp(
            savedFontTenths >= 0
                ? savedFontTenths
                : ReadRegistryDword(L"FontSize", 10) * 10,
            80,
            240)) / 10.0;
        autoStart_ = ReadRegistryDword(L"AutoStart", 1) != 0;
        showFiveHourPercent_ = ReadRegistryDword(L"ShowFiveHourPercent", 1) != 0;
        showFiveHourTime_ = ReadRegistryDword(L"ShowFiveHourTime", 1) != 0;
        showSevenDayPercent_ = ReadRegistryDword(L"ShowSevenDayPercent", 1) != 0;
        showContext_ = ReadRegistryDword(L"ShowContext", 1) != 0;
        showCache_ = ReadRegistryDword(L"ShowCache", 0) != 0;
        showDiff_ = ReadRegistryDword(L"ShowDiff", 1) != 0;
        showDiffFiles_ = ReadRegistryDword(L"ShowDiffFiles", 1) != 0;
        showTaskDuration_ = ReadRegistryDword(L"ShowTaskDuration", 0) != 0;
        workingColor_ = static_cast<COLORREF>(
            ReadRegistryDword(L"WorkingColor", static_cast<int>(kDefaultWorkingColor))) & 0x00FFFFFF;
        pendingWorkingColor_ = workingColor_;
        contextWarningColors_ = ReadRegistryDword(L"ContextWarningColors", 0) != 0;
        quotaWarningColors_ = ReadRegistryDword(L"QuotaWarningColors", 0) != 0;
        durationUnits_ = ReadRegistryDword(L"DurationUnits", 0) != 0;
        blinkOnUserInput_ = ReadRegistryDword(L"BlinkOnUserInput", 1) != 0;
        hidePercentSigns_ = ReadRegistryDword(L"HidePercentSigns", 0) != 0;
        const int savedFontChoice = ReadRegistryDword(L"FontChoice", -1);
        fontChoice_ = savedFontChoice >= 0
            ? std::clamp(savedFontChoice, 0, 4)
            : (ReadRegistryDword(L"NarrowFont", 0) != 0 ? 4 : 0);
        autoFitFont_ = ReadRegistryDword(L"AutoFitFont", 0) != 0;
        cacheDisplayMode_ = std::clamp(ReadRegistryDword(L"CacheDisplayMode", 0), 0, 3);
        showGenerationSpeed_ = ReadRegistryDword(L"ShowGenerationSpeed", 0) != 0;
        generationSpeedMode_ = std::clamp(ReadRegistryDword(L"GenerationSpeedMode", 0), 0, 1);
        compactHourUnderline_ = ReadRegistryDword(L"CompactHourUnderline", 0) != 0;
        displayTarget_ = std::clamp(ReadRegistryDword(L"DisplayTarget", 0), 0, 1);
        automaticThreadMode_ = ReadRegistryDword(L"AutomaticThreadMode", 0) != 0;
        manualThreadId_ = ReadRegistryString(L"ManualThreadId");
        savedThreads_ = ParseSavedThreads(ReadRegistryString(L"SavedThreads"));

        INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_WIN95_CLASSES};
        InitCommonControlsEx(&commonControls);

        darkBackgroundBrush_ = CreateSolidBrush(RGB(24, 24, 24));
        darkControlBrush_ = CreateSolidBrush(RGB(36, 36, 36));
        darkMenuBrush_ = CreateSolidBrush(RGB(24, 24, 24));
        if (!darkBackgroundBrush_ || !darkControlBrush_ || !darkMenuBrush_) {
            return false;
        }

        if (!RegisterWindowClasses()) {
            return false;
        }

        mainWindow_ = CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kMainClass,
            L"CodexTaskbar",
            WS_POPUP,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            instance_,
            this);
        if (!mainWindow_) {
            return false;
        }

        taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
        RecreateFont();
        AddTrayIcon();

        if (autoStart_) {
            SetAutoStartRegistry(true);
        }

        threadMonitor_ = std::make_unique<DesktopThreadMonitor>(mainWindow_, kThreadMetricsMessage);
        threadMonitor_->UpdateConfig(WideToUtf8(manualThreadId_), automaticThreadMode_);
        threadMonitor_->Start();

        StartWorker();
        OnTick();
        SetTimer(mainWindow_, kTickTimer, 1000, nullptr);
        return true;
    }

    int Run() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

    void OnTick() {
        ++tickCount_;

        // Experimental renderer-log diagnostics stay completely dormant in
        // manual mode.
        if (automaticThreadMode_ && codexRunning_) {
            rendererActivityMonitor_.Poll();
        }

        if (threadMetrics_.hasTurnTiming && threadMetrics_.activeTurn) {
            InvalidateWidget();
        }
        if (hoverPopupVisible_) {
            UpdateTooltipText();
        }

        const bool shouldCheckRunning =
            !codexRunningKnown_ || (tickCount_ % 2) == 1;
        if (shouldCheckRunning) {
            const bool running = IsCodexDesktopRunning();
            codexRunningKnown_ = true;
            if (running != codexRunning_) {
                codexRunning_ = running;
                SetWorkerActive(running);
                if (threadMonitor_) {
                    threadMonitor_->SetActive(running);
                }

                if (running) {
                    BeginAnchorReadiness();
                } else {
                    HideWidget();
                    quotaValid_ = false;
                    threadMetrics_ = {};
                    generationSpeedValid_ = false;
                    fiveHourRateBaselineValid_ = false;
                    fiveHourRateStoredValid_ = false;
                    rendererActivityMonitor_.Reset();
                    UpdateBlinkTimer();
                    InvalidateWidget();
                    UpdateTooltipText();
                }
            }
        }

        // Taskbar geometry almost never changes. Keep a low-frequency fallback
        // for Explorer/TrafficMonitor changes instead of repositioning every
        // second.
        if (codexRunning_ && widgetVisible_ && !anchorReadyActive_ &&
            (tickCount_ % 10) == 0) {
            PositionWidget(false);
        }
    }

    void ApplyQuota(const QuotaSnapshot& snapshot) {
        const std::int64_t nowMs = CurrentUnixMilliseconds();

        if (fiveHourRateBaselineValid_ &&
            snapshot.fiveHour.valid &&
            snapshot.fiveHour.resetsAtUnix != fiveHourRateResetAtUnix_) {
            // The 5h window rolled over while the task was still running.
            // Restart the rate baseline at the new window instead of mixing
            // percentages from two different windows.
            fiveHourRateBaselineValid_ = false;
        }

        quota_ = snapshot;
        quotaValid_ = true;
        lastQuotaSuccessMs_ = nowMs;

        if (threadMetrics_.activeTurn && !fiveHourRateBaselineValid_) {
            StartFiveHourRateBaseline(threadMetrics_, nowMs);
        }

        if (const auto rate = CurrentFiveHourConsumptionRate()) {
            fiveHourRateStoredValue_ = *rate;
            fiveHourRateStoredValid_ = true;
        }

        InvalidateWidget();
        UpdateTooltipText();
        UpdateTrayTip();
    }

    void MarkQuotaUnavailable() {
        quotaValid_ = false;
        InvalidateWidget();
        UpdateTooltipText();
        UpdateTrayTip();
    }

    void StartFiveHourRateBaseline(
        const ThreadMetrics& metrics,
        std::int64_t nowMs) {
        (void)nowMs;

        // The rate baseline must be an actually observed pair: quota
        // percentage + the timestamp of that same quota snapshot. Never pair a
        // current percentage with a historical task-start timestamp.
        if (!metrics.activeTurn ||
            metrics.firstTurnWorkItemStartedAtMs <= 0 ||
            !quotaValid_ ||
            !quota_.fiveHour.valid ||
            lastQuotaSuccessMs_ <= 0 ||
            lastQuotaSuccessMs_ < metrics.firstTurnWorkItemStartedAtMs) {
            return;
        }

        fiveHourRateBaselineValid_ = true;
        fiveHourRateStartRemaining_ = quota_.fiveHour.remainingPercent;
        fiveHourRateResetAtUnix_ = quota_.fiveHour.resetsAtUnix;
        fiveHourRateStartMs_ = lastQuotaSuccessMs_;
        fiveHourRateThreadId_ = metrics.threadId;
        fiveHourRateTurnId_ = metrics.turnId;

        // A new task has now reached real work and CodexTaskbar has observed a
        // post-work quota snapshot. Only at this matched baseline do we retire
        // the previous task's frozen final rate.
        fiveHourRateStoredValid_ = false;
    }

    std::optional<double> CurrentFiveHourConsumptionRate() const {
        if (!fiveHourRateBaselineValid_ ||
            !threadMetrics_.activeTurn ||
            !quotaValid_ ||
            !quota_.fiveHour.valid ||
            quota_.fiveHour.resetsAtUnix != fiveHourRateResetAtUnix_ ||
            fiveHourRateStartMs_ <= 0 ||
            lastQuotaSuccessMs_ <= fiveHourRateStartMs_) {
            return std::nullopt;
        }

        const int consumed =
            fiveHourRateStartRemaining_ - quota_.fiveHour.remainingPercent;
        if (consumed < 0) return std::nullopt;

        // The numerator is sampled only when quota refreshes (normally every
        // 60s), so use that same sample timestamp for the denominator. Using
        // "now" would make the rate decay between quota refreshes.
        const std::int64_t elapsedMs =
            lastQuotaSuccessMs_ - fiveHourRateStartMs_;
        if (elapsedMs <= 0) return std::nullopt;

        const double elapsedMinutes =
            static_cast<double>(elapsedMs) / 60000.0;
        return static_cast<double>(consumed) /
            std::max(0.001, elapsedMinutes);
    }

    std::optional<double> DisplayFiveHourConsumptionRate() const {
        if (const auto live = CurrentFiveHourConsumptionRate()) {
            return live;
        }
        if (fiveHourRateStoredValid_) {
            return fiveHourRateStoredValue_;
        }
        return std::nullopt;
    }

    std::int64_t GenerationGapThresholdMs() const {
        if (generationSpeedRecent_.size() < 3) {
            return 5000;
        }

        std::vector<std::int64_t> intervals;
        intervals.reserve(generationSpeedRecent_.size());
        for (const auto& sample : generationSpeedRecent_) {
            if (sample.intervalMs > 0) {
                intervals.push_back(sample.intervalMs);
            }
        }
        if (intervals.size() < 3) return 5000;

        std::sort(intervals.begin(), intervals.end());
        const std::int64_t median =
            intervals[intervals.size() / 2];
        return std::max<std::int64_t>(3000, median * 4);
    }

    std::optional<double> RecentGenerationAverage() const {
        if (generationSpeedRecent_.size() < 5) {
            return std::nullopt;
        }

        std::vector<double> speeds;
        speeds.reserve(generationSpeedRecent_.size());
        for (const auto& sample : generationSpeedRecent_) {
            speeds.push_back(sample.speed);
        }
        std::sort(speeds.begin(), speeds.end());

        std::size_t begin = 0;
        std::size_t end = speeds.size();
        if (speeds.size() >= 5) {
            begin = 1;
            end = speeds.size() - 1;
        }
        if (begin >= end) return std::nullopt;

        double sum = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            sum += speeds[i];
        }
        return sum / static_cast<double>(end - begin);
    }

    void ApplyThreadMetrics(const ThreadMetrics& metrics) {
        const int oldRows = VisibleRowCount();
        const std::int64_t nowMs = CurrentUnixMilliseconds();

        const bool identityChanged =
            metrics.threadId != generationSpeedThreadId_ ||
            metrics.turnId != generationSpeedTurnId_;
        const bool rateIdentityChanged =
            metrics.threadId != fiveHourRateThreadId_ ||
            metrics.turnId != fiveHourRateTurnId_;

        if (rateIdentityChanged) {
            fiveHourRateBaselineValid_ = false;
            fiveHourRateThreadId_ = metrics.threadId;
            fiveHourRateTurnId_ = metrics.turnId;
            if (metrics.activeTurn) {
                StartFiveHourRateBaseline(metrics, nowMs);
            }
        } else if (metrics.activeTurn && !fiveHourRateBaselineValid_) {
            StartFiveHourRateBaseline(metrics, nowMs);
        }

        // If the current task just completed, keep the latest measured value.
        // It remains available in hover until the next task reaches its first
        // real work item, at which point StartFiveHourRateBaseline clears it.
        if (threadMetrics_.activeTurn && !metrics.activeTurn &&
            fiveHourRateStoredValid_) {
            fiveHourRateBaselineValid_ = false;
        }

        if (identityChanged) {
            generationSpeedThreadId_ = metrics.threadId;
            generationSpeedTurnId_ = metrics.turnId;
            generationSpeedValid_ = false;
            generationSpeed_ = 0.0;
            generationSpeedRecent_.clear();
            generationSpeedLastOutputTokens_ =
                metrics.hasTokenUsage ? metrics.outputTokens : 0;
            generationSpeedLastSampleMs_ = nowMs;
        } else if (metrics.activeTurn && metrics.hasTokenUsage) {
            if (metrics.outputTokens < generationSpeedLastOutputTokens_) {
                generationSpeedValid_ = false;
                generationSpeedRecent_.clear();
                generationSpeedLastOutputTokens_ = metrics.outputTokens;
                generationSpeedLastSampleMs_ = nowMs;
            } else if (metrics.outputTokens > generationSpeedLastOutputTokens_) {
                const std::int64_t deltaTokens =
                    metrics.outputTokens - generationSpeedLastOutputTokens_;
                const std::int64_t deltaMs =
                    nowMs - generationSpeedLastSampleMs_;
                const std::int64_t longGapThreshold =
                    GenerationGapThresholdMs();

                // Buffered IPC frames can arrive almost back-to-back and create
                // absurd multi-thousand token/s spikes. Tool/network pauses do
                // the opposite. Both are baseline-only events.
                const bool timingValid =
                    deltaMs >= 250 &&
                    deltaMs <= longGapThreshold;

                if (timingValid && deltaTokens > 0) {
                    generationSpeed_ =
                        static_cast<double>(deltaTokens) * 1000.0 /
                        static_cast<double>(deltaMs);
                    generationSpeedValid_ = true;

                    generationSpeedRecent_.push_back({
                        generationSpeed_,
                        deltaMs});
                    if (generationSpeedRecent_.size() > 8) {
                        generationSpeedRecent_.erase(
                            generationSpeedRecent_.begin());
                    }
                }

                generationSpeedLastOutputTokens_ = metrics.outputTokens;
                generationSpeedLastSampleMs_ = nowMs;
            }
        }

        threadMetrics_ = metrics;
        const int newRows = VisibleRowCount();
        if (widgetVisible_ && oldRows != newRows) {
            PositionWidget();
        }
        UpdateBlinkTimer();
        InvalidateWidget();
        UpdateTooltipText();
        UpdateTrayTip();
    }

    void DrawCenteredSegments(
        HDC dc,
        const RECT& row,
        const std::vector<ColoredSegment>& segments) const {
        int totalWidth = 0;
        std::vector<SIZE> sizes;
        sizes.reserve(segments.size());

        for (const auto& segment : segments) {
            SIZE size{};
            if (!segment.text.empty()) {
                GetTextExtentPoint32W(
                    dc,
                    segment.text.c_str(),
                    static_cast<int>(segment.text.size()),
                    &size);
            }
            sizes.push_back(size);
            totalWidth += size.cx;
        }

        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        int x = static_cast<int>(row.left) +
            std::max(0, (static_cast<int>(row.right - row.left) - totalWidth) / 2);
        const int y = static_cast<int>(row.top) +
            std::max(0, (static_cast<int>(row.bottom - row.top) - static_cast<int>(tm.tmHeight)) / 2);

        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (segments[i].text.empty()) continue;

            if (!segments[i].hidden) {
                SetTextColor(dc, segments[i].color);
                TextOutW(
                    dc,
                    x,
                    y,
                    segments[i].text.c_str(),
                    static_cast<int>(segments[i].text.size()));

                if (segments[i].workingUnderline && sizes[i].cx > 0) {
                    const int underlineY = std::min(
                        static_cast<int>(row.bottom) - 1,
                        y + static_cast<int>(tm.tmHeight));
                    HPEN pen = CreatePen(PS_SOLID, 1, workingColor_);
                    HGDIOBJ oldPen = pen ? SelectObject(dc, pen) : nullptr;
                    MoveToEx(dc, x, underlineY, nullptr);
                    LineTo(dc, x + sizes[i].cx, underlineY);
                    if (oldPen) SelectObject(dc, oldPen);
                    if (pen) DeleteObject(pen);
                }
            }

            x += sizes[i].cx;
        }
        SetTextColor(dc, RGB(255, 255, 255));
    }

    bool WaitingInputBlinkHidden() const {
        if (!blinkOnUserInput_ || !threadMetrics_.waitingForUserInput) {
            return false;
        }
        return ((GetTickCount64() / 500) % 2) != 0;
    }

    void UpdateWaitingInputBlink() {
        const bool hidden = WaitingInputBlinkHidden();
        if (hidden == lastWaitingInputBlinkHidden_) return;
        lastWaitingInputBlinkHidden_ = hidden;
        InvalidateWidget();
    }

    std::wstring TaskbarPercentText(int value, bool valid) const {
        if (!valid) return hidePercentSigns_ ? L"--" : L"--%";
        return std::to_wstring(value) + (hidePercentSigns_ ? L"" : L"%");
    }

    double ExactCachePercent() const {
        if (!threadMetrics_.hasCache || threadMetrics_.inputTokens <= 0) return 0.0;
        const double value =
            100.0 * static_cast<double>(threadMetrics_.cachedInputTokens) /
            static_cast<double>(threadMetrics_.inputTokens);
        return std::clamp(value, 0.0, 100.0);
    }

    std::wstring FixedCachePercent(double value, int decimals) const {
        wchar_t text[32]{};
        if (decimals <= 0) {
            swprintf_s(text, L"%d", static_cast<int>(std::lround(value)));
        } else if (decimals == 1) {
            swprintf_s(text, L"%.1f", value);
        } else {
            swprintf_s(text, L"%.2f", value);
        }
        std::wstring result = text;
        if (!hidePercentSigns_) result.push_back(L'%');
        return result;
    }

    std::wstring CacheTaskbarText() const {
        if (!threadMetrics_.hasCache) {
            return hidePercentSigns_ ? L"--" : L"--%";
        }

        const double exact = ExactCachePercent();
        if (cacheDisplayMode_ == 1) {
            return FixedCachePercent(exact, 1);
        }
        if (cacheDisplayMode_ == 2) {
            return FixedCachePercent(exact, 2);
        }
        if (cacheDisplayMode_ == 3) {
            const int hundredths = std::clamp(
                static_cast<int>(std::lround(exact * 100.0)),
                0,
                10000);
            if (hundredths >= 9000 && hundredths < 10000) {
                wchar_t compact[8]{};
                // 90.04 -> 004, 95.82 -> 582, 99.04 -> 904.
                swprintf_s(compact, L"%03d", hundredths - 9000);
                return compact;
            }
            if (hundredths >= 10000) {
                return L"100";
            }
            // Below 90%, fall back to an unambiguous two-decimal display.
            return FixedCachePercent(exact, 2);
        }
        return TaskbarPercentText(
            threadMetrics_.cachePercent,
            threadMetrics_.hasCache);
    }

    bool SevenDayOnFirstLine() const {
        return showSevenDayPercent_ && !showFiveHourTime_;
    }

    COLORREF QuotaPercentColor(int remainingPercent, bool valid, bool allowWorkingColor) const {
        if (!valid) return RGB(255, 255, 255);
        if (quotaWarningColors_) {
            if (remainingPercent <= 10) return kWarningRed;
            if (remainingPercent <= 20) return kWarningYellow;
        }
        if (allowWorkingColor && threadMetrics_.activeTurn) {
            return workingColor_;
        }
        return RGB(255, 255, 255);
    }

    COLORREF ContextPercentColor(bool valid) const {
        if (!valid) return RGB(255, 255, 255);
        if (contextWarningColors_) {
            if (threadMetrics_.contextPercent >= 90) return kWarningRed;
            if (threadMetrics_.contextPercent >= 80) return kWarningYellow;
        }
        return threadMetrics_.activeTurn ? workingColor_ : RGB(255, 255, 255);
    }

    std::vector<ColoredSegment> FirstLineSegments() const {
        std::vector<ColoredSegment> segments;
        if (showFiveHourPercent_) {
            const bool valid = quotaValid_ && quota_.fiveHour.valid;
            const bool quotaWarning =
                quotaWarningColors_ && valid &&
                quota_.fiveHour.remainingPercent <= 20;
            segments.push_back({
                TaskbarPercentText(
                    quota_.fiveHour.remainingPercent,
                    valid),
                QuotaPercentColor(
                    quota_.fiveHour.remainingPercent,
                    valid,
                    true),
                threadMetrics_.activeTurn && quotaWarning,
                valid && WaitingInputBlinkHidden()});
        }
        if (showFiveHourTime_) {
            if (hidePercentSigns_ && !segments.empty()) {
                segments.push_back({L" ", RGB(255, 255, 255)});
            }
            segments.push_back({
                quotaValid_ && quota_.fiveHour.valid
                    ? FormatResetTime(quota_.fiveHour.resetsAtUnix)
                    : L"--:--",
                RGB(255, 255, 255)});
        } else if (showSevenDayPercent_) {
            const bool valid = quotaValid_ && quota_.sevenDay.valid;
            if (!segments.empty()) {
                segments.push_back({L" ", RGB(255, 255, 255)});
            }
            segments.push_back({
                TaskbarPercentText(
                    quota_.sevenDay.remainingPercent,
                    valid),
                QuotaPercentColor(
                    quota_.sevenDay.remainingPercent,
                    valid,
                    false)});
        }
        return segments;
    }

    std::vector<ColoredSegment> SecondLineSegments() const {
        std::vector<ColoredSegment> segments;
        auto append = [&](std::wstring text, COLORREF color) {
            if (!segments.empty()) {
                segments.push_back({L" ", RGB(255, 255, 255)});
            }
            segments.push_back({std::move(text), color});
        };

        if (showSevenDayPercent_ && !SevenDayOnFirstLine()) {
            const bool valid = quotaValid_ && quota_.sevenDay.valid;
            append(
                TaskbarPercentText(
                    quota_.sevenDay.remainingPercent,
                    valid),
                QuotaPercentColor(
                    quota_.sevenDay.remainingPercent,
                    valid,
                    false));
        }
        if (showContext_) {
            const bool valid = threadMetrics_.hasContext;
            const bool contextWarning =
                contextWarningColors_ && valid &&
                threadMetrics_.contextPercent >= 80;
            if (!segments.empty()) {
                segments.push_back({L" ", RGB(255, 255, 255), false});
            }
            segments.push_back({
                TaskbarPercentText(
                    threadMetrics_.contextPercent,
                    valid),
                ContextPercentColor(valid),
                threadMetrics_.activeTurn && contextWarning,
                valid && WaitingInputBlinkHidden()});
        }
        if (showCache_) {
            append(
                CacheTaskbarText(),
                RGB(255, 255, 255));
        }
        return segments;
    }

    void PaintWidget(HWND hwnd) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        FillRect(dc, &client, black);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HFONT baseFont = widgetFont_ ? widgetFont_ : font_;
        HFONT oldFont = baseFont
            ? static_cast<HFONT>(SelectObject(dc, baseFont))
            : nullptr;

        if (verticalTaskbar_) {
            const std::wstring first = FirstLine();
            const std::wstring second = SecondLine();
            const std::wstring duration = TaskDurationLine();
            const std::wstring diffText = DiffLineText();

            struct Row {
                int kind;
                std::wstring text;
            };
            std::vector<Row> rows;
            if (!first.empty()) rows.push_back({2, first});
            if (!second.empty()) rows.push_back({3, second});
            if (!diffText.empty()) rows.push_back({1, diffText});
            if (!duration.empty()) rows.push_back({0, duration});

            if (rows.empty()) rows.push_back({0, L""});
            const int rowHeight = std::max(
                1,
                static_cast<int>(client.bottom - client.top) /
                    static_cast<int>(rows.size()));

            for (std::size_t index = 0; index < rows.size(); ++index) {
                RECT row{
                    client.left,
                    client.top + static_cast<LONG>(index * rowHeight),
                    client.right,
                    index + 1 == rows.size()
                        ? client.bottom
                        : client.top + static_cast<LONG>((index + 1) * rowHeight)};

                HFONT rowFont = FittedRowFont(
                    dc,
                    rows[index].kind,
                    rows[index].text,
                    static_cast<int>(row.right - row.left));
                HFONT prior = rowFont
                    ? static_cast<HFONT>(SelectObject(dc, rowFont))
                    : nullptr;

                if (rows[index].kind == 1) {
                    DrawDiffRow(dc, row);
                } else if (rows[index].kind == 2) {
                    DrawCenteredSegments(dc, row, FirstLineSegments());
                } else if (rows[index].kind == 3) {
                    DrawCenteredSegments(dc, row, SecondLineSegments());
                } else {
                    DrawCenteredSegments(
                        dc,
                        row,
                        TaskDurationSegments());
                }

                if (prior) SelectObject(dc, prior);
            }
        } else {
            std::vector<ColoredSegment> segments;
            auto appendGroup = [&](const std::vector<ColoredSegment>& group) {
                if (group.empty()) return;
                if (!segments.empty()) {
                    segments.push_back({L"  ", RGB(255, 255, 255)});
                }
                segments.insert(segments.end(), group.begin(), group.end());
            };

            appendGroup(FirstLineSegments());
            appendGroup(SecondLineSegments());

            if (IsDiffRowVisible()) {
                std::vector<ColoredSegment> diffSegments;
                if (showDiffFiles_) {
                    diffSegments.push_back({
                        std::to_wstring(threadMetrics_.changedFiles),
                        RGB(255, 255, 255)});
                }
                diffSegments.push_back({
                    std::to_wstring(threadMetrics_.addedLines),
                    RGB(66, 214, 116)});
                diffSegments.push_back({
                    std::to_wstring(threadMetrics_.removedLines),
                    RGB(242, 88, 88)});
                appendGroup(diffSegments);
            }

            const auto durationSegments = TaskDurationSegments();
            if (!durationSegments.empty()) {
                appendGroup(durationSegments);
            }

            const std::wstring wholeLine = HorizontalLine();
            HFONT fitted = FittedRowFont(
                dc,
                4,
                wholeLine,
                static_cast<int>(client.right - client.left));
            HFONT prior = fitted
                ? static_cast<HFONT>(SelectObject(dc, fitted))
                : nullptr;
            DrawCenteredSegments(dc, client, segments);
            if (prior) SelectObject(dc, prior);
        }

        if (oldFont) SelectObject(dc, oldFont);
        EndPaint(hwnd, &ps);
    }

    void DrawDiffRow(HDC dc, const RECT& row) const {
        const std::wstring files = std::to_wstring(threadMetrics_.changedFiles);
        const std::wstring added = std::to_wstring(threadMetrics_.addedLines);
        const std::wstring removed = std::to_wstring(threadMetrics_.removedLines);
        const int spacing = 0;

        SIZE fileSize{}, addSize{}, removeSize{};
        if (showDiffFiles_) {
            GetTextExtentPoint32W(dc, files.c_str(), static_cast<int>(files.size()), &fileSize);
        }
        GetTextExtentPoint32W(dc, added.c_str(), static_cast<int>(added.size()), &addSize);
        GetTextExtentPoint32W(dc, removed.c_str(), static_cast<int>(removed.size()), &removeSize);

        const int itemCount = showDiffFiles_ ? 3 : 2;
        const int totalWidth =
            (showDiffFiles_ ? fileSize.cx : 0) +
            addSize.cx +
            removeSize.cx +
            spacing * (itemCount - 1);
        int x = static_cast<int>(row.left) +
                std::max(0, (static_cast<int>(row.right - row.left) - totalWidth) / 2);

        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        const int y = static_cast<int>(row.top) +
                      std::max(0, (static_cast<int>(row.bottom - row.top) - static_cast<int>(tm.tmHeight)) / 2);

        if (showDiffFiles_) {
            SetTextColor(dc, RGB(255, 255, 255));
            TextOutW(dc, x, y, files.c_str(), static_cast<int>(files.size()));
            x += fileSize.cx + spacing;
        }

        SetTextColor(dc, RGB(66, 214, 116));
        TextOutW(dc, x, y, added.c_str(), static_cast<int>(added.size()));
        x += addSize.cx + spacing;

        SetTextColor(dc, RGB(242, 88, 88));
        TextOutW(dc, x, y, removed.c_str(), static_cast<int>(removed.size()));
        SetTextColor(dc, RGB(255, 255, 255));
    }

    void ActivateCodexWindow() {
        HWND window = FindCodexMainWindow();
        if (!window) return;

        if (IsIconic(window)) {
            ShowWindow(window, SW_RESTORE);
        } else {
            ShowWindow(window, SW_SHOW);
        }
        SetForegroundWindow(window);
    }

    void SelectSavedThread(std::size_t index) {
        if (index >= savedThreads_.size()) return;

        manualThreadId_ = savedThreads_[index].id;
        automaticThreadMode_ = false;
        rendererActivityMonitor_.Reset();

        generationSpeedValid_ = false;
        generationSpeed_ = 0.0;
        generationSpeedRecent_.clear();
        generationSpeedThreadId_.clear();
        generationSpeedTurnId_.clear();
        fiveHourRateBaselineValid_ = false;
        fiveHourRateStoredValid_ = false;
        fiveHourRateThreadId_.clear();
        fiveHourRateTurnId_.clear();

        WriteRegistryDword(L"AutomaticThreadMode", 0);
        WriteRegistryString(L"ManualThreadId", manualThreadId_);

        if (threadMonitor_) {
            threadMonitor_->UpdateConfig(WideToUtf8(manualThreadId_), false);
        }

        UpdateTooltipText();
        UpdateTrayTip();
    }

    void SelectDisplayTarget(int target) {
        target = std::clamp(target, 0, 1);
        if (target == 1 && !SecondaryTaskbarAvailable()) return;

        displayTarget_ = target;
        WriteRegistryDword(
            L"DisplayTarget",
            static_cast<DWORD>(displayTarget_));

        if (codexRunning_) {
            BeginAnchorReadiness();
        }
    }

    void ShowContextMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }

        MENUINFO menuInfo{};
        menuInfo.cbSize = sizeof(menuInfo);
        menuInfo.fMask = MIM_BACKGROUND;
        menuInfo.hbrBack = darkMenuBrush_;
        SetMenuInfo(menu, &menuInfo);

        AppendMenuW(
            menu,
            MF_OWNERDRAW,
            kMenuSettings,
            reinterpret_cast<LPCWSTR>(L"设置..."));

        HMENU threadsMenu = CreatePopupMenu();
        if (threadsMenu) {
            MENUINFO childInfo{};
            childInfo.cbSize = sizeof(childInfo);
            childInfo.fMask = MIM_BACKGROUND;
            childInfo.hbrBack = darkMenuBrush_;
            SetMenuInfo(threadsMenu, &childInfo);

            const std::size_t count = std::min<std::size_t>(
                savedThreads_.size(),
                kMenuThreadLimit);
            if (count == 0) {
                AppendMenuW(
                    threadsMenu,
                    MF_OWNERDRAW | MF_GRAYED,
                    0,
                    reinterpret_cast<LPCWSTR>(L"暂无已保存对话"));
            } else {
                for (std::size_t i = 0; i < count; ++i) {
                    UINT flags = MF_OWNERDRAW;
                    if (!automaticThreadMode_ &&
                        savedThreads_[i].id == manualThreadId_) {
                        flags |= MF_CHECKED;
                    }
                    AppendMenuW(
                        threadsMenu,
                        flags,
                        kMenuThreadBase + static_cast<UINT>(i),
                        reinterpret_cast<LPCWSTR>(savedThreads_[i].name.c_str()));
                }
            }

            AppendMenuW(
                menu,
                MF_POPUP | MF_OWNERDRAW,
                reinterpret_cast<UINT_PTR>(threadsMenu),
                reinterpret_cast<LPCWSTR>(L"选择对话  ▶"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }

        HMENU displayMenu = CreatePopupMenu();
        if (displayMenu) {
            MENUINFO childInfo{};
            childInfo.cbSize = sizeof(childInfo);
            childInfo.fMask = MIM_BACKGROUND;
            childInfo.hbrBack = darkMenuBrush_;
            SetMenuInfo(displayMenu, &childInfo);

            AppendMenuW(
                displayMenu,
                MF_OWNERDRAW |
                    (displayTarget_ == 0 ? MF_CHECKED : MF_UNCHECKED),
                kMenuDisplayPrimary,
                reinterpret_cast<LPCWSTR>(L"主屏"));
            AppendMenuW(
                displayMenu,
                MF_OWNERDRAW |
                    (displayTarget_ == 1 ? MF_CHECKED : MF_UNCHECKED) |
                    (SecondaryTaskbarAvailable() ? MF_ENABLED : MF_GRAYED),
                kMenuDisplaySecondary,
                reinterpret_cast<LPCWSTR>(L"副屏"));

            AppendMenuW(
                menu,
                MF_POPUP | MF_OWNERDRAW,
                reinterpret_cast<UINT_PTR>(displayMenu),
                reinterpret_cast<LPCWSTR>(L"显示位置  ▶"));
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }

        AppendMenuW(
            menu,
            MF_OWNERDRAW | (autoStart_ ? MF_CHECKED : MF_UNCHECKED),
            kMenuAutoStart,
            reinterpret_cast<LPCWSTR>(L"开机自动启动"));
        AppendMenuW(
            menu,
            MF_OWNERDRAW,
            kMenuReconnect,
            reinterpret_cast<LPCWSTR>(L"重新连接"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(
            menu,
            MF_OWNERDRAW,
            kMenuAbout,
            reinterpret_cast<LPCWSTR>(L"关于"));
        AppendMenuW(
            menu,
            MF_OWNERDRAW,
            kMenuExit,
            reinterpret_cast<LPCWSTR>(L"退出"));

        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(mainWindow_);
        const UINT command = TrackPopupMenu(
            menu,
            TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN,
            point.x,
            point.y,
            0,
            mainWindow_,
            nullptr);
        DestroyMenu(menu);

        HandleMenuCommand(command);
        PostMessageW(mainWindow_, WM_NULL, 0, 0);
    }

    void HandleTrayMessage(LPARAM event) {
        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            ShowContextMenu();
        } else if (event == WM_LBUTTONDBLCLK) {
            OpenSettings();
        }
    }

    void HandleTaskbarCreated() {
        AddTrayIcon();
        ResetTaskbarHandles();
        if (codexRunning_) {
            BeginAnchorReadiness();
        }
    }

    void OpenSettings() {
        if (settingsWindow_ && IsWindow(settingsWindow_)) {
            ShowWindow(settingsWindow_, SW_SHOW);
            SetForegroundWindow(settingsWindow_);
            return;
        }

        pendingWorkingColor_ = workingColor_;

        settingsWindow_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
            kSettingsClass,
            L"CodexTaskbar 设置",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            780,
            700,
            mainWindow_,
            nullptr,
            instance_,
            this);
        if (!settingsWindow_) {
            return;
        }

        ApplyDarkWindowTheme(settingsWindow_);

        RECT rect{};
        GetWindowRect(settingsWindow_, &rect);
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(
            settingsWindow_,
            HWND_TOP,
            (screenWidth - width) / 2,
            (screenHeight - height) / 2,
            0,
            0,
            SWP_NOSIZE | SWP_SHOWWINDOW);
    }

    void CreateSettingsControls(HWND hwnd) {
        auto checkbox = [&](int id, const wchar_t* text, int x, int y, bool checked, int width = 190) {
            HWND control = CreateWindowW(
                L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                x, y, width, 24, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
            SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        };

        // Font controls span the full width.
        CreateWindowW(L"STATIC", L"字号 (pt)：", WS_CHILD | WS_VISIBLE,
            24, 20, 82, 24, hwnd, nullptr, instance_, nullptr);
        HWND fontEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            106, 16, 86, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditFontSize)), instance_, nullptr);
        wchar_t fontValue[16]{};
        swprintf_s(fontValue, L"%.1f", fontSize_);
        SetWindowTextW(fontEdit, fontValue);

        CreateWindowW(L"STATIC", L"字体：", WS_CHILD | WS_VISIBLE,
            232, 20, 52, 24, hwnd, nullptr, instance_, nullptr);
        HWND fontCombo = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            284, 16, 310, 180, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboFontChoice)), instance_, nullptr);
        SendMessageW(fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Segoe UI Semibold（原版）"));
        SendMessageW(fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bahnschrift Regular"));
        SendMessageW(fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Segoe UI Regular"));
        SendMessageW(fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Tahoma Regular"));
        SendMessageW(fontCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Bahnschrift SemiCondensed"));
        SendMessageW(fontCombo, CB_SETCURSEL, static_cast<WPARAM>(fontChoice_), 0);

        // Left column: what appears on the taskbar.
        CreateWindowW(L"STATIC", L"任务栏显示", WS_CHILD | WS_VISIBLE,
            24, 62, 180, 24, hwnd, nullptr, instance_, nullptr);
        checkbox(kCheckShowFiveHourPercent, L"5小时百分比", 24, 88, showFiveHourPercent_, 200);
        checkbox(kCheckShowFiveHourTime, L"5小时恢复时间", 24, 116, showFiveHourTime_, 200);
        checkbox(kCheckShowSevenDayPercent, L"7天百分比", 24, 144, showSevenDayPercent_, 200);
        checkbox(kCheckShowContext, L"Context百分比", 24, 172, showContext_, 200);
        checkbox(kCheckShowCache, L"Cache命中率", 24, 200, showCache_, 200);
        checkbox(kCheckShowDiff, L"代码改动", 24, 228, showDiff_, 130);
        checkbox(kCheckShowDiffFiles, L"显示文件数", 164, 228, showDiffFiles_, 130);
        checkbox(kCheckShowTaskDuration, L"任务耗时", 24, 256, showTaskDuration_, 130);
        checkbox(kCheckShowGenerationSpeed, L"生成速度", 164, 256, showGenerationSpeed_, 130);

        CreateWindowW(L"STATIC", L"速度显示：", WS_CHILD | WS_VISIBLE,
            24, 292, 80, 24, hwnd, nullptr, instance_, nullptr);
        HWND speedMode = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            106, 287, 180, 120, hwnd,
            reinterpret_cast<HMENU>(
                static_cast<INT_PTR>(kComboGenerationSpeedMode)),
            instance_, nullptr);
        SendMessageW(
            speedMode, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"实时速度"));
        SendMessageW(
            speedMode, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(L"最近平均"));
        SendMessageW(
            speedMode, CB_SETCURSEL,
            static_cast<WPARAM>(generationSpeedMode_), 0);

        // Right column: presentation and alert behavior.
        CreateWindowW(L"STATIC", L"显示样式与提醒", WS_CHILD | WS_VISIBLE,
            400, 62, 200, 24, hwnd, nullptr, instance_, nullptr);

        CreateWindowW(L"STATIC", L"运行中颜色：", WS_CHILD | WS_VISIBLE,
            400, 90, 96, 24, hwnd, nullptr, instance_, nullptr);
        CreateWindowW(
            L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            496, 84, 82, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonWorkingColor)), instance_, nullptr);
        CreateWindowW(
            L"STATIC", L"76%", WS_CHILD | WS_VISIBLE | SS_CENTER,
            590, 90, 54, 24, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStaticWorkingColorPreview)), instance_, nullptr);

        checkbox(kCheckContextWarningColors, L"Context警告色 (80/90%)",
            400, 122, contextWarningColors_, 250);
        checkbox(kCheckQuotaWarningColors, L"配额警告色 (20/10%)",
            400, 150, quotaWarningColors_, 250);
        checkbox(kCheckDurationUnits, L"耗时使用 h/m/s 格式",
            400, 178, durationUnits_, 250);
        checkbox(kCheckBlinkOnUserInput, L"等待输入时闪烁任务百分比",
            400, 206, blinkOnUserInput_, 280);
        checkbox(kCheckHidePercentSigns, L"任务栏隐藏 %",
            400, 234, hidePercentSigns_, 220);
        checkbox(kCheckAutoFitFont, L"溢出时逐行微缩 (最多 2pt)",
            400, 262, autoFitFont_, 280);

        CreateWindowW(L"STATIC", L"Cache显示：", WS_CHILD | WS_VISIBLE,
            400, 294, 92, 24, hwnd, nullptr, instance_, nullptr);
        HWND cacheMode = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            494, 289, 170, 150, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kComboCacheDisplayMode)), instance_, nullptr);
        SendMessageW(cacheMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"整数"));
        SendMessageW(cacheMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"1位小数"));
        SendMessageW(cacheMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"2位小数"));
        SendMessageW(cacheMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"90+紧凑"));
        SendMessageW(cacheMode, CB_SETCURSEL, static_cast<WPARAM>(cacheDisplayMode_), 0);

        checkbox(
            kCheckCompactHourUnderline,
            L"小时用下划线压缩",
            400, 322, compactHourUnderline_, 250);

        // Session section spans the whole lower half.
        CreateWindowW(L"STATIC", L"会话来源", WS_CHILD | WS_VISIBLE,
            24, 350, 180, 24, hwnd, nullptr, instance_, nullptr);

        HWND manualRadio = CreateWindowW(
            L"BUTTON", L"手动绑定 Thread ID",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            24, 376, 220, 24, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRadioManualThread)), instance_, nullptr);
        HWND autoRadio = CreateWindowW(
            L"BUTTON", L"自动跟随活动任务（实验）",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            270, 376, 250, 24, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRadioAutoThread)), instance_, nullptr);
        SendMessageW(manualRadio, BM_SETCHECK, automaticThreadMode_ ? BST_UNCHECKED : BST_CHECKED, 0);
        SendMessageW(autoRadio, BM_SETCHECK, automaticThreadMode_ ? BST_CHECKED : BST_UNCHECKED, 0);

        CreateWindowW(L"STATIC", L"当前 Thread ID：", WS_CHILD | WS_VISIBLE,
            24, 412, 112, 24, hwnd, nullptr, instance_, nullptr);
        CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", manualThreadId_.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            138, 408, 600, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditThreadId)), instance_, nullptr);

        CreateWindowW(
            L"STATIC",
            L"保存的对话（供右键“选择对话”使用；每行：名称|Thread ID）：",
            WS_CHILD | WS_VISIBLE,
            24, 450, 620, 24, hwnd, nullptr, instance_, nullptr);
        const std::wstring savedText = SerializeSavedThreads(savedThreads_);
        CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", savedText.c_str(),
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
            24, 476, 714, 104, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditSavedThreads)), instance_, nullptr);

        CreateWindowW(
            L"STATIC",
            L"右键“选择对话”会在右侧展开保存清单；切换时只跟踪选中的一个 Thread，其它保存项不产生后台开销。",
            WS_CHILD | WS_VISIBLE,
            24, 586, 714, 42, hwnd, nullptr, instance_, nullptr);

        checkbox(kCheckAutoStart, L"开机自动启动", 24, 630, autoStart_, 180);

        CreateWindowW(
            L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            574, 626, 78, 30, hwnd,
            reinterpret_cast<HMENU>(IDOK), instance_, nullptr);
        CreateWindowW(
            L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            660, 626, 78, 30, hwnd,
            reinterpret_cast<HMENU>(IDCANCEL), instance_, nullptr);

        EnumChildWindows(
            hwnd,
            [](HWND child, LPARAM) -> BOOL {
                SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
                return TRUE;
            },
            0);
    }

    void ChooseWorkingColor(HWND owner) {
        CHOOSECOLORW choose{};
        choose.lStructSize = sizeof(choose);
        choose.hwndOwner = owner;
        choose.rgbResult = pendingWorkingColor_;
        choose.lpCustColors = customColors_.data();
        choose.Flags = CC_FULLOPEN | CC_RGBINIT;

        if (ChooseColorW(&choose)) {
            pendingWorkingColor_ = choose.rgbResult & 0x00FFFFFF;
            HWND preview = GetDlgItem(owner, kStaticWorkingColorPreview);
            if (preview) {
                InvalidateRect(preview, nullptr, TRUE);
            }
        }
    }

    COLORREF PendingWorkingColor() const {
        return pendingWorkingColor_;
    }

    void ApplyDarkWindowTheme(HWND hwnd) {
        if (!hwnd || !IsWindow(hwnd)) return;
        BOOL enabled = TRUE;
        if (FAILED(DwmSetWindowAttribute(
                hwnd,
                static_cast<DWMWINDOWATTRIBUTE>(20),
                &enabled,
                sizeof(enabled)))) {
            DwmSetWindowAttribute(
                hwnd,
                static_cast<DWMWINDOWATTRIBUTE>(19),
                &enabled,
                sizeof(enabled));
        }
        SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    }

    bool MeasureContextMenuItem(MEASUREITEMSTRUCT* item) const {
        if (!item || item->CtlType != ODT_MENU) return false;
        UINT dpi = mainWindow_ && IsWindow(mainWindow_) ? GetDpiForWindow(mainWindow_) : 96;
        if (dpi == 0) dpi = 96;
        item->itemWidth = static_cast<UINT>(MulDiv(220, static_cast<int>(dpi), 96));
        item->itemHeight = static_cast<UINT>(MulDiv(28, static_cast<int>(dpi), 96));
        return true;
    }

    bool DrawContextMenuItem(DRAWITEMSTRUCT* item) const {
        if (!item || item->CtlType != ODT_MENU) return false;

        const bool selected = (item->itemState & ODS_SELECTED) != 0;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const COLORREF background = selected ? RGB(58, 58, 58) : RGB(24, 24, 24);
        HBRUSH brush = CreateSolidBrush(background);
        if (brush) {
            FillRect(item->hDC, &item->rcItem, brush);
            DeleteObject(brush);
        }

        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, disabled ? RGB(145, 145, 145) : RGB(242, 242, 242));

        UINT dpi = mainWindow_ && IsWindow(mainWindow_) ? GetDpiForWindow(mainWindow_) : 96;
        if (dpi == 0) dpi = 96;
        const int leftPadding = MulDiv(30, static_cast<int>(dpi), 96);
        const int checkPadding = MulDiv(8, static_cast<int>(dpi), 96);
        const int checkWidth = MulDiv(16, static_cast<int>(dpi), 96);

        RECT textRect = item->rcItem;
        textRect.left += leftPadding;
        const auto* text = reinterpret_cast<const wchar_t*>(item->itemData);
        if (text) {
            DrawTextW(
                item->hDC,
                text,
                -1,
                &textRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        if ((item->itemState & ODS_CHECKED) != 0) {
            RECT checkRect = item->rcItem;
            checkRect.left += checkPadding;
            checkRect.right = checkRect.left + checkWidth;
            DrawTextW(
                item->hDC,
                L"✓",
                -1,
                &checkRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        return true;
    }

    bool DrawSettingsButton(DRAWITEMSTRUCT* item) const {
        if (!item || item->CtlType != ODT_BUTTON) return false;

        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const COLORREF background = pressed ? RGB(62, 62, 62) : RGB(42, 42, 42);
        HBRUSH backgroundBrush = CreateSolidBrush(background);
        if (backgroundBrush) {
            FillRect(item->hDC, &item->rcItem, backgroundBrush);
            DeleteObject(backgroundBrush);
        }

        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(92, 92, 92));
        HGDIOBJ oldPen = borderPen ? SelectObject(item->hDC, borderPen) : nullptr;
        HGDIOBJ oldBrush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
        Rectangle(
            item->hDC,
            item->rcItem.left,
            item->rcItem.top,
            item->rcItem.right,
            item->rcItem.bottom);
        SelectObject(item->hDC, oldBrush);
        if (oldPen) SelectObject(item->hDC, oldPen);
        if (borderPen) DeleteObject(borderPen);

        wchar_t label[64]{};
        GetWindowTextW(item->hwndItem, label, static_cast<int>(std::size(label)));
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, disabled ? RGB(145, 145, 145) : RGB(242, 242, 242));
        RECT textRect = item->rcItem;
        DrawTextW(
            item->hDC,
            label,
            -1,
            &textRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if ((item->itemState & ODS_FOCUS) != 0) {
            RECT focus = item->rcItem;
            InflateRect(&focus, -3, -3);
            DrawFocusRect(item->hDC, &focus);
        }
        return true;
    }

    void SaveSettingsFromWindow(HWND hwnd) {
        wchar_t text[64]{};
        GetDlgItemTextW(hwnd, kEditFontSize, text, static_cast<int>(std::size(text)));
        wchar_t* end = nullptr;
        const double requested = std::wcstod(text, &end);
        if (end != text) {
            fontSize_ = std::clamp(
                std::round(requested * 10.0) / 10.0,
                8.0,
                24.0);
        }

        auto checked = [&](int id) {
            return SendDlgItemMessageW(hwnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
        };

        autoStart_ = checked(kCheckAutoStart);
        showFiveHourPercent_ = checked(kCheckShowFiveHourPercent);
        showFiveHourTime_ = checked(kCheckShowFiveHourTime);
        showSevenDayPercent_ = checked(kCheckShowSevenDayPercent);
        showContext_ = checked(kCheckShowContext);
        showCache_ = checked(kCheckShowCache);
        showDiff_ = checked(kCheckShowDiff);
        showDiffFiles_ = checked(kCheckShowDiffFiles);
        showTaskDuration_ = checked(kCheckShowTaskDuration);
        showGenerationSpeed_ = checked(kCheckShowGenerationSpeed);
        compactHourUnderline_ =
            checked(kCheckCompactHourUnderline);
        workingColor_ = pendingWorkingColor_;
        contextWarningColors_ = checked(kCheckContextWarningColors);
        quotaWarningColors_ = checked(kCheckQuotaWarningColors);
        durationUnits_ = checked(kCheckDurationUnits);
        blinkOnUserInput_ = checked(kCheckBlinkOnUserInput);
        hidePercentSigns_ = checked(kCheckHidePercentSigns);
        autoFitFont_ = checked(kCheckAutoFitFont);

        const LRESULT fontSelection =
            SendDlgItemMessageW(hwnd, kComboFontChoice, CB_GETCURSEL, 0, 0);
        fontChoice_ = fontSelection == CB_ERR
            ? 0
            : std::clamp(static_cast<int>(fontSelection), 0, 4);

        const LRESULT cacheSelection =
            SendDlgItemMessageW(hwnd, kComboCacheDisplayMode, CB_GETCURSEL, 0, 0);
        cacheDisplayMode_ = cacheSelection == CB_ERR
            ? 0
            : std::clamp(static_cast<int>(cacheSelection), 0, 3);

        const LRESULT speedSelection =
            SendDlgItemMessageW(
                hwnd,
                kComboGenerationSpeedMode,
                CB_GETCURSEL,
                0,
                0);
        generationSpeedMode_ = speedSelection == CB_ERR
            ? 0
            : std::clamp(static_cast<int>(speedSelection), 0, 1);

        const bool previousAutomaticMode = automaticThreadMode_;
        automaticThreadMode_ = checked(kRadioAutoThread);
        if (previousAutomaticMode != automaticThreadMode_) {
            rendererActivityMonitor_.Reset();
        }

        wchar_t threadText[256]{};
        GetDlgItemTextW(hwnd, kEditThreadId, threadText, static_cast<int>(std::size(threadText)));
        manualThreadId_ = threadText;
        while (!manualThreadId_.empty() && iswspace(manualThreadId_.front())) manualThreadId_.erase(manualThreadId_.begin());
        while (!manualThreadId_.empty() && iswspace(manualThreadId_.back())) manualThreadId_.pop_back();

        std::vector<wchar_t> savedBuffer(8192, L'\0');
        GetDlgItemTextW(
            hwnd,
            kEditSavedThreads,
            savedBuffer.data(),
            static_cast<int>(savedBuffer.size()));
        savedThreads_ = ParseSavedThreads(savedBuffer.data());

        const int fontSizeTenths = static_cast<int>(std::lround(fontSize_ * 10.0));
        WriteRegistryDword(L"FontSizeTenths", static_cast<DWORD>(fontSizeTenths));
        WriteRegistryDword(L"FontSize", static_cast<DWORD>(std::lround(fontSize_)));
        WriteRegistryDword(L"AutoStart", autoStart_ ? 1u : 0u);
        WriteRegistryDword(L"ShowFiveHourPercent", showFiveHourPercent_ ? 1u : 0u);
        WriteRegistryDword(L"ShowFiveHourTime", showFiveHourTime_ ? 1u : 0u);
        WriteRegistryDword(L"ShowSevenDayPercent", showSevenDayPercent_ ? 1u : 0u);
        WriteRegistryDword(L"ShowContext", showContext_ ? 1u : 0u);
        WriteRegistryDword(L"ShowCache", showCache_ ? 1u : 0u);
        WriteRegistryDword(L"ShowDiff", showDiff_ ? 1u : 0u);
        WriteRegistryDword(L"ShowDiffFiles", showDiffFiles_ ? 1u : 0u);
        WriteRegistryDword(L"ShowTaskDuration", showTaskDuration_ ? 1u : 0u);
        WriteRegistryDword(L"ShowGenerationSpeed", showGenerationSpeed_ ? 1u : 0u);
        WriteRegistryDword(
            L"GenerationSpeedMode",
            static_cast<DWORD>(generationSpeedMode_));
        WriteRegistryDword(
            L"CompactHourUnderline",
            compactHourUnderline_ ? 1u : 0u);
        WriteRegistryDword(L"WorkingColor", static_cast<DWORD>(workingColor_));
        WriteRegistryDword(L"ContextWarningColors", contextWarningColors_ ? 1u : 0u);
        WriteRegistryDword(L"QuotaWarningColors", quotaWarningColors_ ? 1u : 0u);
        WriteRegistryDword(L"DurationUnits", durationUnits_ ? 1u : 0u);
        WriteRegistryDword(L"BlinkOnUserInput", blinkOnUserInput_ ? 1u : 0u);
        WriteRegistryDword(L"HidePercentSigns", hidePercentSigns_ ? 1u : 0u);
        WriteRegistryDword(L"FontChoice", static_cast<DWORD>(fontChoice_));
        WriteRegistryDword(L"NarrowFont", fontChoice_ == 4 ? 1u : 0u);
        WriteRegistryDword(L"AutoFitFont", autoFitFont_ ? 1u : 0u);
        WriteRegistryDword(L"CacheDisplayMode", static_cast<DWORD>(cacheDisplayMode_));
        WriteRegistryDword(L"AutomaticThreadMode", automaticThreadMode_ ? 1u : 0u);
        WriteRegistryString(L"ManualThreadId", manualThreadId_);
        WriteRegistryString(L"SavedThreads", SerializeSavedThreads(savedThreads_));
        SetAutoStartRegistry(autoStart_);

        if (threadMonitor_) {
            threadMonitor_->UpdateConfig(WideToUtf8(manualThreadId_), automaticThreadMode_);
        }

        RecreateFont();
        UpdateBlinkTimer();
        if (widgetVisible_) {
            PositionWidget();
            InvalidateWidget();
        }
        UpdateTooltipText();
        DestroyWindow(hwnd);
    }

    void SettingsDestroyed(HWND hwnd) {
        if (settingsWindow_ == hwnd) {
            settingsWindow_ = nullptr;
        }
    }

    void WidgetDestroyed(HWND hwnd) {
        if (widgetWindow_ == hwnd) {
            HideHoverPopup();
            hoverInside_ = false;
            hoverStartTick_ = 0;
            widgetWindow_ = nullptr;
            widgetVisible_ = false;
        }
    }

    UINT TaskbarCreatedMessage() const {
        return taskbarCreatedMessage_;
    }

    HBRUSH DarkBackgroundBrush() const {
        return darkBackgroundBrush_;
    }

    HBRUSH DarkControlBrush() const {
        return darkControlBrush_;
    }

private:
    static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<CodexTaskbarApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<CodexTaskbarApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }

        if (app && message == app->TaskbarCreatedMessage()) {
            app->HandleTaskbarCreated();
            return 0;
        }

        switch (message) {
        case WM_TIMER:
            if (app && wParam == kTickTimer) {
                app->OnTick();
                return 0;
            }
            if (app && wParam == kHoverTimer) {
                app->ShowHoverAfterDelay();
                return 0;
            }
            if (app && wParam == kBlinkTimer) {
                app->UpdateWaitingInputBlink();
                return 0;
            }
            if (app && wParam == kAnchorReadyTimer) {
                app->TryAnchorReadiness();
                return 0;
            }
            break;

        case WM_DISPLAYCHANGE:
            if (app && app->codexRunning_) {
                app->BeginAnchorReadiness();
            }
            return 0;

        case kTrayMessage:
            if (app) {
                app->HandleTrayMessage(lParam);
            }
            return 0;

        case kQuotaMessage:
            if (app) {
                std::unique_ptr<QuotaPayload> payload(reinterpret_cast<QuotaPayload*>(lParam));
                if (payload) {
                    app->ApplyQuota(payload->snapshot);
                }
            }
            return 0;

        case kQuotaErrorMessage:
            if (app) {
                app->MarkQuotaUnavailable();
            }
            return 0;

        case kThreadMetricsMessage:
            if (app) {
                std::unique_ptr<ThreadMetrics> payload(reinterpret_cast<ThreadMetrics*>(lParam));
                if (payload) {
                    app->ApplyThreadMetrics(*payload);
                }
            }
            return 0;

        case WM_MEASUREITEM:
            if (app && app->MeasureContextMenuItem(
                    reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            break;

        case WM_DRAWITEM:
            if (app && app->DrawContextMenuItem(
                    reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            break;

        case WM_DESTROY:
            KillTimer(hwnd, kTickTimer);
            KillTimer(hwnd, kHoverTimer);
            KillTimer(hwnd, kBlinkTimer);
            KillTimer(hwnd, kAnchorReadyTimer);
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK WidgetWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<CodexTaskbarApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<CodexTaskbarApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }

        switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            if (app) {
                app->PaintWidget(hwnd);
                return 0;
            }
            break;

        case WM_DPICHANGED_AFTERPARENT:
            if (app) {
                app->RecreateFont();
                app->PositionWidget();
                return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (app) {
                app->HandleWidgetMouseMove(hwnd);
                return 0;
            }
            break;

        case WM_MOUSELEAVE:
            if (app) {
                app->HandleWidgetMouseLeave();
                return 0;
            }
            break;

        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
            if (app) {
                app->ResetHoverState();
            }
            break;

        case WM_LBUTTONUP:
            if (app) {
                app->ResetHoverState();
                app->ActivateCodexWindow();
                return 0;
            }
            break;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            if (app) {
                app->ResetHoverState();
                app->ShowContextMenu();
                return 0;
            }
            break;

        case WM_NCDESTROY:
            if (app) {
                app->WidgetDestroyed(hwnd);
            }
            break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK HoverWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<CodexTaskbarApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<CodexTaskbarApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }

        switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT:
            if (app) {
                app->PaintHoverPopup(hwnd);
                return 0;
            }
            break;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_NCHITTEST:
            return HTTRANSPARENT;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* app = reinterpret_cast<CodexTaskbarApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            app = static_cast<CodexTaskbarApp*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }

        switch (message) {
        case WM_CREATE:
            if (app) {
                app->ApplyDarkWindowTheme(hwnd);
                app->CreateSettingsControls(hwnd);
            }
            return 0;

        case WM_ERASEBKGND:
            if (app && app->DarkBackgroundBrush()) {
                RECT rect{};
                GetClientRect(hwnd, &rect);
                FillRect(reinterpret_cast<HDC>(wParam), &rect, app->DarkBackgroundBrush());
                return 1;
            }
            break;

        case WM_CTLCOLORDLG:
        case WM_CTLCOLORBTN:
            if (app && app->DarkBackgroundBrush()) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, RGB(242, 242, 242));
                SetBkColor(dc, RGB(24, 24, 24));
                return reinterpret_cast<LRESULT>(app->DarkBackgroundBrush());
            }
            break;

        case WM_CTLCOLORSTATIC:
            if (app && app->DarkBackgroundBrush()) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                const HWND control = reinterpret_cast<HWND>(lParam);
                const bool preview =
                    control && GetDlgCtrlID(control) == kStaticWorkingColorPreview;
                SetTextColor(
                    dc,
                    preview ? app->PendingWorkingColor() : RGB(242, 242, 242));
                SetBkColor(dc, RGB(24, 24, 24));
                return reinterpret_cast<LRESULT>(app->DarkBackgroundBrush());
            }
            break;

        case WM_CTLCOLOREDIT:
            if (app && app->DarkControlBrush()) {
                HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, RGB(242, 242, 242));
                SetBkColor(dc, RGB(36, 36, 36));
                return reinterpret_cast<LRESULT>(app->DarkControlBrush());
            }
            break;

        case WM_DRAWITEM:
            if (app && app->DrawSettingsButton(
                    reinterpret_cast<DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            break;

        case WM_COMMAND:
            if (!app) {
                break;
            }
            if (LOWORD(wParam) == kButtonWorkingColor) {
                app->ChooseWorkingColor(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDOK) {
                app->SaveSettingsFromWindow(hwnd);
                return 0;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_NCDESTROY:
            if (app) {
                app->SettingsDestroyed(hwnd);
            }
            break;
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool RegisterWindowClasses() {
        WNDCLASSEXW mainClass{};
        mainClass.cbSize = sizeof(mainClass);
        mainClass.hInstance = instance_;
        mainClass.lpfnWndProc = MainWndProc;
        mainClass.lpszClassName = kMainClass;
        mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&mainClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        WNDCLASSEXW widgetClass{};
        widgetClass.cbSize = sizeof(widgetClass);
        widgetClass.hInstance = instance_;
        widgetClass.lpfnWndProc = WidgetWndProc;
        widgetClass.lpszClassName = kWidgetClass;
        widgetClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        widgetClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        if (!RegisterClassExW(&widgetClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        WNDCLASSEXW hoverClass{};
        hoverClass.cbSize = sizeof(hoverClass);
        hoverClass.hInstance = instance_;
        hoverClass.lpfnWndProc = HoverWndProc;
        hoverClass.lpszClassName = kHoverClass;
        hoverClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hoverClass.hbrBackground = darkBackgroundBrush_;
        if (!RegisterClassExW(&hoverClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        WNDCLASSEXW settingsClass{};
        settingsClass.cbSize = sizeof(settingsClass);
        settingsClass.hInstance = instance_;
        settingsClass.lpfnWndProc = SettingsWndProc;
        settingsClass.lpszClassName = kSettingsClass;
        settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        settingsClass.hbrBackground = darkBackgroundBrush_;
        if (!RegisterClassExW(&settingsClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        return true;
    }

    UINT FontDpi() const {
        UINT dpi = 96;
        HWND dpiWindow = nullptr;
        if (widgetWindow_ && IsWindow(widgetWindow_)) {
            dpiWindow = widgetWindow_;
        } else if (taskbarWindow_ && IsWindow(taskbarWindow_)) {
            dpiWindow = taskbarWindow_;
        } else if (mainWindow_ && IsWindow(mainWindow_)) {
            dpiWindow = mainWindow_;
        }
        if (dpiWindow) {
            const UINT windowDpi = GetDpiForWindow(dpiWindow);
            if (windowDpi > 0) dpi = windowDpi;
        }
        return dpi;
    }

    HFONT CreatePointFont(double pointSize, bool widgetFont) const {
        const UINT dpi = FontDpi();
        const int height = -std::max(
            1,
            static_cast<int>(std::lround(
                pointSize * static_cast<double>(dpi) / 72.0)));

        const wchar_t* face = L"Segoe UI";
        int weight = FW_SEMIBOLD;
        if (widgetFont) {
            switch (fontChoice_) {
            case 1:
                face = L"Bahnschrift";
                weight = FW_NORMAL;
                break;
            case 2:
                face = L"Segoe UI";
                weight = FW_NORMAL;
                break;
            case 3:
                face = L"Tahoma";
                weight = FW_NORMAL;
                break;
            case 4:
                face = L"Bahnschrift SemiCondensed";
                weight = FW_NORMAL;
                break;
            default:
                face = L"Segoe UI";
                weight = FW_SEMIBOLD;
                break;
            }
        }

        return CreateFontW(
            height,
            0,
            0,
            0,
            weight,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            face);
    }

    void ClearRowFitCaches() {
        for (auto& cache : rowFitCaches_) {
            if (cache.font) {
                DeleteObject(cache.font);
                cache.font = nullptr;
            }
            cache.signature.clear();
            cache.availableWidth = -1;
        }
    }

    void RecreateFont() {
        ClearRowFitCaches();

        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        if (widgetFont_) {
            DeleteObject(widgetFont_);
            widgetFont_ = nullptr;
        }

        font_ = CreatePointFont(fontSize_, false);
        widgetFont_ = CreatePointFont(fontSize_, true);
        if (!widgetFont_) {
            widgetFont_ = CreatePointFont(fontSize_, false);
        }
    }

    int MeasureTextWidth(HDC dc, const std::wstring& text) const {
        if (!dc || text.empty()) return 0;
        SIZE size{};
        GetTextExtentPoint32W(
            dc,
            text.c_str(),
            static_cast<int>(text.size()),
            &size);
        return size.cx;
    }

    std::wstring NormalizeFitSignature(const std::wstring& text) const {
        std::wstring result = text;
        for (wchar_t& ch : result) {
            if (iswdigit(ch)) ch = L'#';
        }
        return result;
    }

    HFONT FittedRowFont(
        HDC dc,
        int cacheIndex,
        const std::wstring& text,
        int availableWidth) {
        if (!autoFitFont_ || !dc || !widgetFont_ || text.empty()) {
            return widgetFont_ ? widgetFont_ : font_;
        }

        cacheIndex = std::clamp(cacheIndex, 0, 4);
        RowFitCache& cache = rowFitCaches_[static_cast<std::size_t>(cacheIndex)];
        const std::wstring signature = NormalizeFitSignature(text);
        const int fitWidth = std::max(1, availableWidth - 2);

        if (cache.availableWidth == fitWidth &&
            cache.signature == signature) {
            return cache.font ? cache.font : widgetFont_;
        }

        if (cache.font) {
            DeleteObject(cache.font);
            cache.font = nullptr;
        }
        cache.availableWidth = fitWidth;
        cache.signature = signature;

        HFONT previous = static_cast<HFONT>(SelectObject(dc, widgetFont_));
        const int baseWidth = MeasureTextWidth(dc, text);
        SelectObject(dc, previous);

        if (baseWidth <= fitWidth) {
            return widgetFont_;
        }

        const double minimum = std::max(8.0, fontSize_ - 2.0);
        double target = std::floor(
            std::clamp(
                fontSize_ * static_cast<double>(fitWidth) /
                    static_cast<double>(std::max(1, baseWidth)),
                minimum,
                fontSize_) *
            10.0) / 10.0;

        while (target >= minimum - 0.04) {
            HFONT candidate = CreatePointFont(target, true);
            if (!candidate) break;

            previous = static_cast<HFONT>(SelectObject(dc, candidate));
            const int width = MeasureTextWidth(dc, text);
            SelectObject(dc, previous);

            if (width <= fitWidth || target <= minimum + 0.04) {
                cache.font = candidate;
                return cache.font;
            }

            DeleteObject(candidate);
            target = std::max(minimum, target - 0.1);
            target = std::round(target * 10.0) / 10.0;
        }

        return widgetFont_;
    }

    void AddTrayIcon() {
        if (!mainWindow_) {
            return;
        }

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = mainWindow_;
        data.uID = kTrayId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kTrayMessage;
        data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(data.szTip, L"CodexTaskbar");
        Shell_NotifyIconW(NIM_ADD, &data);
        // Keep the legacy callback contract: lParam is the mouse message itself.
        // NOTIFYICON_VERSION_4 repacks the event into LOWORD/HIWORD and adds no
        // value for this deliberately tiny context-menu-only tray icon.
        trayAdded_ = true;
    }

    void RemoveTrayIcon() {
        if (!trayAdded_ || !mainWindow_) {
            return;
        }

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = mainWindow_;
        data.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        trayAdded_ = false;
    }

    void UpdateTrayTip() {
        if (!mainWindow_) {
            return;
        }

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = mainWindow_;
        data.uID = kTrayId;
        data.uFlags = NIF_TIP;

        std::wstring tip = L"CodexTaskbar";
        if (codexRunning_) {
            tip += L" · ";
            tip += quotaValid_ ? FirstLine() + L" · " + SecondLine() : L"正在连接";
        } else {
            tip += L" · Codex 未运行";
        }

        wcsncpy_s(data.szTip, tip.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    TaskbarCandidate ResolveTaskbarCandidate() const {
        const auto candidates = EnumerateTaskbars();
        if (candidates.empty()) return {};

        if (displayTarget_ == 1) {
            for (const auto& candidate : candidates) {
                if (!candidate.primary) return candidate;
            }
        }

        for (const auto& candidate : candidates) {
            if (candidate.primary) return candidate;
        }
        return candidates.front();
    }

    bool SecondaryTaskbarAvailable() const {
        const auto candidates = EnumerateTaskbars();
        return std::any_of(
            candidates.begin(),
            candidates.end(),
            [](const TaskbarCandidate& candidate) {
                return !candidate.primary;
            });
    }

    void ResetTaskbarHandles() {
        taskbarWindow_ = nullptr;
        rebarWindow_ = nullptr;
        activeTaskbarDevice_.clear();
    }

    void EnsureWidget() {
        const TaskbarCandidate target = ResolveTaskbarCandidate();
        if (!target.window) return;

        const bool taskbarChanged =
            taskbarWindow_ != target.window ||
            activeTaskbarDevice_ != target.deviceName;
        taskbarWindow_ = target.window;
        activeTaskbarDevice_ = target.deviceName;

        if (taskbarChanged) {
            rebarWindow_ = nullptr;
        }

        if (!rebarWindow_ || !IsWindow(rebarWindow_)) {
            rebarWindow_ =
                FindWindowExW(taskbarWindow_, nullptr, L"ReBarWindow32", nullptr);
            if (!rebarWindow_) {
                rebarWindow_ =
                    FindWindowExW(taskbarWindow_, nullptr, L"WorkerW", nullptr);
            }
            if (!rebarWindow_) {
                rebarWindow_ = taskbarWindow_;
            }
        }

        bool dpiContextChanged = false;
        if (!widgetWindow_ || !IsWindow(widgetWindow_)) {
            widgetWindow_ = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                kWidgetClass,
                L"CodexTaskbarWidget",
                WS_CHILD,
                0,
                0,
                1,
                1,
                rebarWindow_,
                nullptr,
                instance_,
                this);
            dpiContextChanged = widgetWindow_ != nullptr;
        } else if (GetParent(widgetWindow_) != rebarWindow_) {
            SetParent(widgetWindow_, rebarWindow_);
            dpiContextChanged = true;
        }

        if (dpiContextChanged) {
            RecreateFont();
        }
    }

    bool PositionWidget(bool requireTrafficMonitor = false) {
        EnsureWidget();
        if (!widgetWindow_ || !rebarWindow_ || !taskbarWindow_) {
            return false;
        }

        RECT taskbarRect{};
        GetWindowRect(taskbarWindow_, &taskbarRect);
        verticalTaskbar_ =
            (taskbarRect.bottom - taskbarRect.top) >
            (taskbarRect.right - taskbarRect.left);

        RECT rebarClient{};
        GetClientRect(rebarWindow_, &rebarClient);

        int width = 0;
        if (verticalTaskbar_) {
            width = std::max(
                40,
                static_cast<int>(rebarClient.right - rebarClient.left) - 4);
        } else {
            width = std::max(
                150,
                static_cast<int>(std::lround(fontSize_ * 15.0)));
        }

        HDC dc = GetDC(widgetWindow_);
        HFONT oldFont = dc ? static_cast<HFONT>(SelectObject(
            dc,
            widgetFont_ ? widgetFont_ : font_)) : nullptr;
        TEXTMETRICW metrics{};
        if (dc) {
            GetTextMetricsW(dc, &metrics);
            if (oldFont) SelectObject(dc, oldFont);
            ReleaseDC(widgetWindow_, dc);
        }

        const int lineHeight =
            std::max(14, static_cast<int>(metrics.tmHeight) + 1);
        int height = 0;
        if (verticalTaskbar_) {
            height =
                lineHeight * std::max(1, VisibleRowCount()) + 4;
        } else {
            height = std::max(
                24,
                std::min(
                    static_cast<int>(
                        rebarClient.bottom - rebarClient.top),
                    lineHeight + 6));
        }

        HWND trafficMonitor =
            FindDescendantByTitle(taskbarWindow_, kTrafficMonitorTitle);
        HWND trayNotify =
            FindDescendantByClass(taskbarWindow_, L"TrayNotifyWnd");

        if (verticalTaskbar_) {
            if (requireTrafficMonitor && !trafficMonitor) {
                return false;
            }

            int anchorTop = rebarClient.bottom;
            HWND anchor = trafficMonitor ? trafficMonitor : trayNotify;
            if (anchor) {
                RECT anchorRect{};
                GetWindowRect(anchor, &anchorRect);
                anchorRect =
                    ScreenRectToClient(rebarWindow_, anchorRect);
                anchorTop = anchorRect.top;
            }

            const int y = std::max(
                static_cast<int>(rebarClient.top),
                anchorTop - height - 1);
            const int x = std::max(
                0,
                (static_cast<int>(
                    rebarClient.right - rebarClient.left) - width) / 2);

            SetWindowPos(
                widgetWindow_,
                HWND_TOP,
                x,
                y,
                width,
                height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            RECT trayRect{};
            int x = rebarClient.right - width - 2;
            if (trayNotify && GetWindowRect(trayNotify, &trayRect)) {
                trayRect =
                    ScreenRectToClient(rebarWindow_, trayRect);
                x = trayRect.left - width - 2;
            }
            x = std::max(static_cast<int>(rebarClient.left), x);
            const int y = std::max(
                static_cast<int>(rebarClient.top),
                (static_cast<int>(
                    rebarClient.bottom - rebarClient.top) - height) / 2);
            SetWindowPos(
                widgetWindow_,
                HWND_TOP,
                x,
                y,
                width,
                height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }

        InvalidateWidget();
        return true;
    }

    void StopAnchorReadiness() {
        if (mainWindow_) {
            KillTimer(mainWindow_, kAnchorReadyTimer);
        }
        anchorReadyActive_ = false;
    }

    void TryAnchorReadiness() {
        if (!anchorReadyActive_ || !codexRunning_) return;

        if (PositionWidget(true)) {
            StopAnchorReadiness();
            ShowWindow(widgetWindow_, SW_SHOWNOACTIVATE);
            widgetVisible_ = true;
            UpdateBlinkTimer();
            return;
        }

        if (GetTickCount64() >= anchorReadyDeadline_) {
            PositionWidget(false);
            StopAnchorReadiness();
            if (widgetWindow_) {
                ShowWindow(widgetWindow_, SW_SHOWNOACTIVATE);
                widgetVisible_ = true;
                UpdateBlinkTimer();
            }
        }
    }

    void BeginAnchorReadiness() {
        StopAnchorReadiness();
        ResetTaskbarHandles();

        if (widgetWindow_ && IsWindow(widgetWindow_)) {
            ShowWindow(widgetWindow_, SW_HIDE);
        }
        widgetVisible_ = false;

        anchorReadyActive_ = true;
        anchorReadyDeadline_ = GetTickCount64() + 5000;
        SetTimer(mainWindow_, kAnchorReadyTimer, 200, nullptr);
        TryAnchorReadiness();
    }

    void HideWidget() {
        ResetHoverState();
        if (widgetWindow_ && IsWindow(widgetWindow_)) {
            ShowWindow(widgetWindow_, SW_HIDE);
        }
        widgetVisible_ = false;
        UpdateTrayTip();
    }

    std::wstring FirstLine() const {
        std::wstring line;
        if (showFiveHourPercent_) {
            line += TaskbarPercentText(
                quota_.fiveHour.remainingPercent,
                quotaValid_ && quota_.fiveHour.valid);
        }
        if (showFiveHourTime_) {
            if (hidePercentSigns_ && !line.empty()) line.push_back(L' ');
            line += quotaValid_ && quota_.fiveHour.valid
                ? FormatResetTime(quota_.fiveHour.resetsAtUnix)
                : L"--:--";
        } else if (showSevenDayPercent_) {
            if (!line.empty()) line.push_back(L' ');
            line += TaskbarPercentText(
                quota_.sevenDay.remainingPercent,
                quotaValid_ && quota_.sevenDay.valid);
        }
        return line;
    }

    std::wstring SecondLine() const {
        std::vector<std::wstring> values;

        if (showSevenDayPercent_ && !SevenDayOnFirstLine()) {
            values.push_back(TaskbarPercentText(
                quota_.sevenDay.remainingPercent,
                quotaValid_ && quota_.sevenDay.valid));
        }
        if (showContext_) {
            values.push_back(TaskbarPercentText(
                threadMetrics_.contextPercent,
                threadMetrics_.hasContext));
        }
        if (showCache_) {
            values.push_back(CacheTaskbarText());
        }

        std::wstring line;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) line.push_back(L' ');
            line += values[i];
        }
        return line;
    }

    std::int64_t CurrentTaskDurationMs() const {
        if (!threadMetrics_.hasTurnTiming) return -1;

        if (threadMetrics_.activeTurn && threadMetrics_.turnStartedAtMs > 0) {
            return std::max<std::int64_t>(
                0,
                CurrentUnixMilliseconds() - threadMetrics_.turnStartedAtMs);
        }

        if (threadMetrics_.turnDurationMs >= 0) {
            return threadMetrics_.turnDurationMs;
        }
        return -1;
    }

    std::wstring GenerationSpeedText() const {
        if (!generationSpeedValid_) return L"--";

        double value = generationSpeed_;
        if (generationSpeedMode_ == 1) {
            if (const auto average = RecentGenerationAverage()) {
                value = *average;
            }
        }

        wchar_t text[32]{};
        swprintf_s(text, L"%.1f", value);
        return text;
    }

    std::vector<ColoredSegment> TaskDurationSegments() const {
        std::vector<ColoredSegment> segments;
        if (!threadMetrics_.activeTurn) return segments;

        if (showTaskDuration_) {
            const std::int64_t durationMs = CurrentTaskDurationMs();
            const std::int64_t totalSeconds =
                durationMs >= 0 ? durationMs / 1000 : -1;
            const std::int64_t hours =
                totalSeconds >= 0 ? totalSeconds / 3600 : -1;
            const std::int64_t minutes =
                totalSeconds >= 0 ? (totalSeconds / 60) % 60 : -1;
            const std::int64_t seconds =
                totalSeconds >= 0 ? totalSeconds % 60 : -1;

            if (compactHourUnderline_ &&
                hours >= 1 &&
                hours <= 2 &&
                minutes >= 0 &&
                seconds >= 0) {
                wchar_t secondsText[8]{};
                swprintf_s(
                    secondsText,
                    L"%02lld",
                    static_cast<long long>(seconds));

                if (durationUnits_) {
                    segments.push_back({
                        std::to_wstring(minutes),
                        RGB(255, 255, 255),
                        hours == 1});
                    segments.push_back({
                        L"m",
                        RGB(255, 255, 255)});
                    segments.push_back({
                        secondsText,
                        RGB(255, 255, 255),
                        hours == 2});
                } else {
                    segments.push_back({
                        std::to_wstring(minutes),
                        RGB(255, 255, 255),
                        hours == 1});
                    segments.push_back({
                        L":",
                        RGB(255, 255, 255)});
                    segments.push_back({
                        secondsText,
                        RGB(255, 255, 255),
                        hours == 2});
                }
            } else {
                const std::wstring durationText = durationUnits_
                    ? FormatDurationUnits(durationMs)
                    : FormatDurationCompact(durationMs);
                if (!durationText.empty()) {
                    segments.push_back({
                        durationText,
                        RGB(255, 255, 255)});
                }
            }
        }

        if (showGenerationSpeed_) {
            if (!segments.empty()) {
                segments.push_back({
                    L" ",
                    RGB(255, 255, 255)});
            }
            segments.push_back({
                GenerationSpeedText(),
                RGB(255, 255, 255)});
        }

        return segments;
    }

    std::wstring TaskDurationLine() const {
        std::wstring line;
        for (const auto& segment : TaskDurationSegments()) {
            line += segment.text;
        }
        return line;
    }

    bool IsDiffRowVisible() const {
        return threadMetrics_.activeTurn && showDiff_ && threadMetrics_.hasDiff &&
            (threadMetrics_.changedFiles > 0 ||
             threadMetrics_.addedLines > 0 ||
             threadMetrics_.removedLines > 0);
    }

    std::wstring DiffLineText() const {
        if (!IsDiffRowVisible()) return {};
        std::wstring text;
        if (showDiffFiles_) text += std::to_wstring(threadMetrics_.changedFiles);
        text += std::to_wstring(threadMetrics_.addedLines);
        text += std::to_wstring(threadMetrics_.removedLines);
        return text;
    }

    int VisibleRowCount() const {
        int rows = 0;
        if (!FirstLine().empty()) ++rows;
        if (!SecondLine().empty()) ++rows;
        if (!TaskDurationLine().empty()) ++rows;
        if (IsDiffRowVisible()) ++rows;
        return rows;
    }

    std::wstring HorizontalLine() const {
        std::wstring result;
        const std::wstring first = FirstLine();
        const std::wstring second = SecondLine();
        if (!first.empty()) result += first;
        if (!second.empty()) {
            if (!result.empty()) result += L"  ";
            result += second;
        }
        if (IsDiffRowVisible()) {
            if (!result.empty()) result += L"  ";
            if (showDiffFiles_) {
                result += std::to_wstring(threadMetrics_.changedFiles) + L" ";
            }
            result += std::to_wstring(threadMetrics_.addedLines) + L" " +
                      std::to_wstring(threadMetrics_.removedLines);
        }
        const std::wstring duration = TaskDurationLine();
        if (!duration.empty()) {
            if (!result.empty()) result += L"  ";
            result += duration;
        }
        return result;
    }

    void EnsureHoverPopup() {
        if (hoverWindow_ && IsWindow(hoverWindow_)) return;

        hoverWindow_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kHoverClass,
            L"CodexTaskbarHover",
            WS_POPUP,
            0,
            0,
            1,
            1,
            mainWindow_,
            nullptr,
            instance_,
            this);
    }

    void PaintHoverPopup(HWND hwnd) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, darkBackgroundBrush_);

        HPEN border = CreatePen(PS_SOLID, 1, RGB(82, 82, 82));
        HGDIOBJ oldPen = border ? SelectObject(dc, border) : nullptr;
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, client.left, client.top, client.right, client.bottom);
        SelectObject(dc, oldBrush);
        if (oldPen) SelectObject(dc, oldPen);
        if (border) DeleteObject(border);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(242, 242, 242));
        HFONT oldFont = font_ ? static_cast<HFONT>(SelectObject(dc, font_)) : nullptr;

        RECT textRect = client;
        const int padding = 10;
        InflateRect(&textRect, -padding, -padding);
        DrawTextW(
            dc,
            tooltipBuffer_.data(),
            -1,
            &textRect,
            DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);

        if (oldFont) SelectObject(dc, oldFont);
        EndPaint(hwnd, &ps);
    }

    void ShowHoverPopup(const POINT& cursor) {
        EnsureHoverPopup();
        if (!hoverWindow_ || !IsWindow(hoverWindow_)) return;

        UpdateTooltipText();

        HDC dc = GetDC(hoverWindow_);
        if (!dc) return;
        HFONT oldFont = font_ ? static_cast<HFONT>(SelectObject(dc, font_)) : nullptr;
        RECT measure{0, 0, 520, 0};
        DrawTextW(
            dc,
            tooltipBuffer_.data(),
            -1,
            &measure,
            DT_CALCRECT | DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        if (oldFont) SelectObject(dc, oldFont);
        ReleaseDC(hoverWindow_, dc);

        const int padding = 10;
        const int width = std::clamp(static_cast<int>(measure.right - measure.left) + padding * 2, 280, 560);
        const int height = std::max(80, static_cast<int>(measure.bottom - measure.top) + padding * 2);

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(monitor, &monitorInfo);

        RECT widgetRect{};
        RECT taskbarRect{};
        GetWindowRect(widgetWindow_, &widgetRect);
        GetWindowRect(taskbarWindow_, &taskbarRect);

        int x = cursor.x + 12;
        int y = cursor.y + 12;

        const bool vertical =
            (taskbarRect.bottom - taskbarRect.top) > (taskbarRect.right - taskbarRect.left);
        if (vertical) {
            const int taskbarCenter =
                taskbarRect.left + (taskbarRect.right - taskbarRect.left) / 2;
            const int monitorCenter =
                monitorInfo.rcMonitor.left +
                (monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left) / 2;
            if (taskbarCenter <= monitorCenter) {
                x = widgetRect.right + 8;
            } else {
                x = widgetRect.left - width - 8;
            }
            y = cursor.y + 8;
        } else {
            const int taskbarCenter =
                taskbarRect.top + (taskbarRect.bottom - taskbarRect.top) / 2;
            const int monitorCenter =
                monitorInfo.rcMonitor.top +
                (monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top) / 2;
            x = cursor.x + 8;
            y = taskbarCenter >= monitorCenter
                ? widgetRect.top - height - 8
                : widgetRect.bottom + 8;
        }

        x = std::clamp(
            x,
            static_cast<int>(monitorInfo.rcMonitor.left),
            std::max(static_cast<int>(monitorInfo.rcMonitor.left),
                     static_cast<int>(monitorInfo.rcMonitor.right) - width));
        y = std::clamp(
            y,
            static_cast<int>(monitorInfo.rcMonitor.top),
            std::max(static_cast<int>(monitorInfo.rcMonitor.top),
                     static_cast<int>(monitorInfo.rcMonitor.bottom) - height));

        SetWindowPos(
            hoverWindow_,
            HWND_TOPMOST,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        hoverPopupVisible_ = true;
        InvalidateRect(hoverWindow_, nullptr, TRUE);
    }

    void HideHoverPopup() {
        if (hoverWindow_ && IsWindow(hoverWindow_)) {
            ShowWindow(hoverWindow_, SW_HIDE);
        }
        hoverPopupVisible_ = false;
    }

    void ResetHoverState() {
        hoverInside_ = false;
        hoverStartTick_ = 0;
        KillTimer(mainWindow_, kHoverTimer);
        HideHoverPopup();
    }

    void HandleWidgetMouseMove(HWND hwnd) {
        if (hoverInside_) return;

        hoverInside_ = true;
        hoverStartTick_ = GetTickCount64();

        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        TrackMouseEvent(&track);

        // One-shot-style timer: it is killed on firing or mouse leave.
        SetTimer(mainWindow_, kHoverTimer, 250, nullptr);
    }

    void HandleWidgetMouseLeave() {
        ResetHoverState();
    }

    void ShowHoverAfterDelay() {
        KillTimer(mainWindow_, kHoverTimer);
        if (!hoverInside_ || !widgetVisible_ || !widgetWindow_ ||
            !IsWindowVisible(widgetWindow_)) {
            return;
        }

        POINT cursor{};
        RECT widgetRect{};
        if (!GetCursorPos(&cursor) ||
            !GetWindowRect(widgetWindow_, &widgetRect) ||
            !PtInRect(&widgetRect, cursor)) {
            ResetHoverState();
            return;
        }

        ShowHoverPopup(cursor);
    }

    void UpdateBlinkTimer() {
        const bool shouldRun =
            widgetVisible_ && blinkOnUserInput_ &&
            threadMetrics_.waitingForUserInput;
        if (shouldRun && !blinkTimerActive_) {
            SetTimer(mainWindow_, kBlinkTimer, 500, nullptr);
            blinkTimerActive_ = true;
        } else if (!shouldRun && blinkTimerActive_) {
            KillTimer(mainWindow_, kBlinkTimer);
            blinkTimerActive_ = false;
            lastWaitingInputBlinkHidden_ = false;
            InvalidateWidget();
        }
    }

    void UpdateTooltipText() {
        std::wstring text;

        if (quotaValid_ && quota_.fiveHour.valid) {
            text += L"5小时：剩余 " + std::to_wstring(quota_.fiveHour.remainingPercent) +
                    L"% · 恢复 " + FormatResetTime(quota_.fiveHour.resetsAtUnix);
        } else {
            text += L"5小时：暂无数据";
        }

        const bool liveFiveHourRate =
            CurrentFiveHourConsumptionRate().has_value();
        text += liveFiveHourRate
            ? L"\r\n5h消耗速度（估算）："
            : L"\r\n上次5h消耗速度（估算）：";
        if (const auto rate = DisplayFiveHourConsumptionRate()) {
            wchar_t rateText[48]{};
            swprintf_s(rateText, L"%.3f%%/分钟", *rate);
            text += rateText;
        } else {
            text += L"暂无";
        }

        text += L"\r\n";
        if (quotaValid_ && quota_.sevenDay.valid) {
            text += L"7天：剩余 " + std::to_wstring(quota_.sevenDay.remainingPercent) + L"%";
        } else {
            text += L"7天：暂无数据";
        }

        text += L"\r\n";
        if (threadMetrics_.hasContext) {
            text += L"上下文：" + std::to_wstring(threadMetrics_.contextPercent) + L"% 已用 (" +
                    FormatInteger(threadMetrics_.contextUsedTokens) + L" / " +
                    FormatInteger(threadMetrics_.contextWindowTokens) + L")";
        } else {
            text += L"上下文：暂无数据";
        }

        text += L"\r\n";
        if (threadMetrics_.hasCache) {
            wchar_t cachePercent[32]{};
            swprintf_s(cachePercent, L"%.5f", ExactCachePercent());
            text += L"缓存：" + std::wstring(cachePercent) + L"% (" +
                    FormatInteger(threadMetrics_.cachedInputTokens) + L" / " +
                    FormatInteger(threadMetrics_.inputTokens) + L")";
        } else {
            text += L"缓存：暂无数据";
        }

        text += L"\r\n";
        if (threadMetrics_.hasTokenUsage) {
            text += L"总 Token：" + FormatInteger(threadMetrics_.totalTokens) +
                    L" · 输出 " + FormatInteger(threadMetrics_.outputTokens);
        } else {
            text += L"总 Token：暂无数据";
        }

        text += L"\r\n生成速度：";
        if (generationSpeedValid_) {
            wchar_t speed[32]{};
            swprintf_s(speed, L"%.1f token/s", generationSpeed_);
            text += speed;
        } else {
            text += L"暂无";
        }

        text += L"\r\n最近平均：";
        if (const auto averageValue = RecentGenerationAverage()) {
            wchar_t average[32]{};
            swprintf_s(
                average,
                L"%.1f token/s",
                *averageValue);
            text += average;
        } else {
            text += L"样本不足";
        }

        text += L"\r\n最近速度：";
        if (!generationSpeedRecent_.empty()) {
            for (std::size_t i = 0; i < generationSpeedRecent_.size(); ++i) {
                if (i > 0) text += L" / ";
                wchar_t sample[24]{};
                swprintf_s(
                    sample,
                    L"%.1f",
                    generationSpeedRecent_[i].speed);
                text += sample;
            }
        } else {
            text += L"暂无";
        }

        text += L"\r\n";
        if (quotaValid_) {
            text += L"配额连接：正常 · " + FormatElapsedAge(lastQuotaSuccessMs_) + L"更新";
        } else {
            text += L"配额连接：异常";
            if (lastQuotaSuccessMs_ > 0) {
                text += L" · 上次成功 " + FormatElapsedAge(lastQuotaSuccessMs_);
            }
        }
        text += L"\r\n会话连接：";
        text += threadMetrics_.connected ? L"已连接" : L"未连接";

        text += L"\r\n任务状态：";
        if (!threadMetrics_.valid) {
            text += L"暂无";
        } else if (threadMetrics_.waitingForUserInput) {
            text += L"等待输入";
        } else if (threadMetrics_.activeTurn) {
            text += L"运行中";
        } else {
            text += L"空闲";
        }

        text += L"\r\n";
        const std::int64_t taskDurationMs = CurrentTaskDurationMs();
        if (taskDurationMs >= 0) {
            text += L"任务耗时：" + FormatDurationVerbose(taskDurationMs);
            if (threadMetrics_.activeTurn) {
                text += L"（进行中）";
            }
        } else {
            text += L"任务耗时：暂无";
        }

        text += L"\r\n";
        if (threadMetrics_.hasDiff) {
            text += threadMetrics_.diffExact ? L"代码改动：" : L"代码改动（实时估算）：";
            text += std::to_wstring(threadMetrics_.changedFiles) +
                    L" 个文件 · +" + FormatInteger(threadMetrics_.addedLines) +
                    L" · -" + FormatInteger(threadMetrics_.removedLines);
        } else {
            text += L"代码改动：本轮暂无";
        }

        text += L"\r\n会话：";
        if (!threadMetrics_.threadId.empty()) {
            text += Utf8ToWide(threadMetrics_.threadId);
        } else if (!manualThreadId_.empty()) {
            text += manualThreadId_;
        } else {
            text += L"未绑定";
        }
        text += automaticThreadMode_ ? L"（自动/实验）" : L"（手动）";

        if (automaticThreadMode_) {
            const RendererActivityState& renderer = rendererActivityMonitor_.State();
            text += L"\r\nRenderer：";
            if (!renderer.logAvailable) {
                text += L"日志未找到";
            } else if (!renderer.eventSeen) {
                text += L"等待 activity 事件";
            } else if (renderer.activeThreadId.empty()) {
                text += L"暂无 active Thread";
            } else {
                text += CompactThreadId(renderer.activeThreadId);
                if (renderer.pendingInactive) {
                    text += L"（inactive待确认）";
                }
            }

            text += L"\r\nIPC显示：" + CompactThreadId(threadMetrics_.threadId);
            if (renderer.eventSeen &&
                !renderer.activeThreadId.empty() &&
                !threadMetrics_.threadId.empty()) {
                text += renderer.activeThreadId == threadMetrics_.threadId
                    ? L" · 一致"
                    : L" · 不一致";
            } else {
                text += L" · 暂无法判断";
            }
        }

        wcsncpy_s(tooltipBuffer_.data(), tooltipBuffer_.size(), text.c_str(), _TRUNCATE);

        if (hoverPopupVisible_ && hoverWindow_ && IsWindow(hoverWindow_)) {
            InvalidateRect(hoverWindow_, nullptr, TRUE);
        }
    }

    void InvalidateWidget() {
        if (widgetWindow_ && IsWindow(widgetWindow_)) {
            InvalidateRect(widgetWindow_, nullptr, FALSE);
        }
    }

    void HandleMenuCommand(UINT command) {
        if (command >= kMenuThreadBase &&
            command < kMenuThreadBase + kMenuThreadLimit) {
            SelectSavedThread(
                static_cast<std::size_t>(command - kMenuThreadBase));
            return;
        }

        switch (command) {
        case kMenuSettings:
            OpenSettings();
            break;

        case kMenuDisplayPrimary:
            SelectDisplayTarget(0);
            break;

        case kMenuDisplaySecondary:
            SelectDisplayTarget(1);
            break;

        case kMenuAutoStart:
            autoStart_ = !autoStart_;
            WriteRegistryDword(L"AutoStart", autoStart_ ? 1u : 0u);
            SetAutoStartRegistry(autoStart_);
            break;

        case kMenuReconnect:
            RequestReconnect();
            break;

        case kMenuAbout:
            MessageBoxW(
                mainWindow_,
                L"CodexTaskbar v0.3.8\n\n"
                L"在 Windows 10 任务栏显示 Codex 配额、绑定会话上下文/缓存和本轮代码改动。\n"
                L"配额来自官方 app-server；会话指标通过只读 Codex Desktop IPC follower 获取。",
                L"关于 CodexTaskbar",
                MB_OK | MB_ICONINFORMATION);
            break;

        case kMenuExit:
            DestroyWindow(mainWindow_);
            break;

        default:
            break;
        }
    }

    void StartWorker() {
        workerThread_ = std::thread([this] {
            AppServerClient client;

            for (;;) {
                bool active = false;
                bool reconnect = false;

                {
                    std::unique_lock lock(workerMutex_);
                    workerCv_.wait(lock, [this] {
                        return workerStop_ || workerActive_;
                    });
                    if (workerStop_) {
                        break;
                    }
                    active = workerActive_;
                    reconnect = reconnectRequested_;
                    reconnectRequested_ = false;
                }

                if (!active) {
                    client.Disconnect();
                    continue;
                }

                if (reconnect) {
                    client.Disconnect();
                }

                if (!client.IsConnected() && !client.Connect()) {
                    PostMessageW(mainWindow_, kQuotaErrorMessage, 0, 0);
                    std::unique_lock lock(workerMutex_);
                    workerCv_.wait_for(lock, std::chrono::seconds(5), [this] {
                        return workerStop_ || !workerActive_ || reconnectRequested_;
                    });
                    if (!workerActive_) {
                        client.Disconnect();
                    }
                    continue;
                }

                QuotaSnapshot snapshot{};
                bool quotaReadSucceeded = false;
                if (client.ReadQuota(snapshot)) {
                    quotaReadSucceeded = true;
                    auto* payload = new QuotaPayload{snapshot};
                    if (!PostMessageW(
                            mainWindow_,
                            kQuotaMessage,
                            0,
                            reinterpret_cast<LPARAM>(payload))) {
                        delete payload;
                    }
                } else {
                    client.Disconnect();
                    PostMessageW(mainWindow_, kQuotaErrorMessage, 0, 0);
                }

                // Normal refresh stays conservative at 60 s. Once a quota read
                // fails, keep the visible "--" offline signal but retry every
                // 5 s until a successful read restores the normal cadence.
                const auto retryDelay = quotaReadSucceeded
                    ? std::chrono::seconds(60)
                    : std::chrono::seconds(5);
                std::unique_lock lock(workerMutex_);
                workerCv_.wait_for(lock, retryDelay, [this] {
                    return workerStop_ || !workerActive_ || reconnectRequested_;
                });
                if (!workerActive_) {
                    client.Disconnect();
                }
            }

            client.Disconnect();
        });
    }

    void ShutdownWorker() {
        {
            std::lock_guard lock(workerMutex_);
            workerStop_ = true;
            workerActive_ = false;
            reconnectRequested_ = true;
        }
        workerCv_.notify_all();
        if (workerThread_.joinable()) {
            workerThread_.join();
        }
    }

    void SetWorkerActive(bool active) {
        {
            std::lock_guard lock(workerMutex_);
            workerActive_ = active;
            if (!active) {
                reconnectRequested_ = false;
            }
        }
        workerCv_.notify_all();
        UpdateTrayTip();
    }

    void RequestReconnect() {
        {
            std::lock_guard lock(workerMutex_);
            reconnectRequested_ = true;
        }
        quotaValid_ = false;
        // A manual reconnect starts a new observation interval. Preserve the
        // previous final value in hover, but require a fresh matched quota/time
        // pair before reporting a new live 5h rate.
        fiveHourRateBaselineValid_ = false;
        if (threadMonitor_) {
            threadMonitor_->UpdateConfig(WideToUtf8(manualThreadId_), automaticThreadMode_);
        }
        InvalidateWidget();
        workerCv_.notify_all();
    }

    HINSTANCE instance_{nullptr};
    HWND mainWindow_{nullptr};
    HWND widgetWindow_{nullptr};
    HWND settingsWindow_{nullptr};
    HWND taskbarWindow_{nullptr};
    HWND rebarWindow_{nullptr};
    HWND hoverWindow_{nullptr};
    std::wstring activeTaskbarDevice_;
    bool anchorReadyActive_{false};
    ULONGLONG anchorReadyDeadline_{0};
    HFONT font_{nullptr};
    HFONT widgetFont_{nullptr};
    HBRUSH darkBackgroundBrush_{nullptr};
    HBRUSH darkControlBrush_{nullptr};
    HBRUSH darkMenuBrush_{nullptr};

    UINT taskbarCreatedMessage_{0};
    bool trayAdded_{false};
    bool codexRunning_{false};
    bool codexRunningKnown_{false};
    bool widgetVisible_{false};
    bool hoverInside_{false};
    bool hoverPopupVisible_{false};
    ULONGLONG hoverStartTick_{0};
    bool verticalTaskbar_{true};
    bool quotaValid_{false};
    std::int64_t lastQuotaSuccessMs_{0};
    bool autoStart_{true};
    bool showFiveHourPercent_{true};
    bool showFiveHourTime_{true};
    bool showSevenDayPercent_{true};
    bool showContext_{true};
    bool showCache_{false};
    bool showDiff_{true};
    bool showDiffFiles_{true};
    bool showTaskDuration_{false};
    COLORREF workingColor_{kDefaultWorkingColor};
    COLORREF pendingWorkingColor_{kDefaultWorkingColor};
    std::array<COLORREF, 16> customColors_{};
    bool contextWarningColors_{false};
    bool quotaWarningColors_{false};
    bool durationUnits_{false};
    bool blinkOnUserInput_{true};
    bool lastWaitingInputBlinkHidden_{false};
    bool blinkTimerActive_{false};
    bool hidePercentSigns_{false};
    int fontChoice_{0};
    bool autoFitFont_{false};
    int cacheDisplayMode_{0};
    bool showGenerationSpeed_{false};
    int generationSpeedMode_{0};
    bool compactHourUnderline_{false};
    int displayTarget_{0};
    bool automaticThreadMode_{false};
    double fontSize_{10.0};
    std::array<RowFitCache, 5> rowFitCaches_{};
    unsigned tickCount_{0};

    QuotaSnapshot quota_{};
    ThreadMetrics threadMetrics_{};
    std::wstring manualThreadId_;
    std::vector<SavedThreadEntry> savedThreads_;

    bool generationSpeedValid_{false};
    double generationSpeed_{0.0};
    std::vector<GenerationSpeedSample> generationSpeedRecent_;
    std::int64_t generationSpeedLastOutputTokens_{0};
    std::int64_t generationSpeedLastSampleMs_{0};
    std::string generationSpeedThreadId_;
    std::string generationSpeedTurnId_;

    bool fiveHourRateBaselineValid_{false};
    bool fiveHourRateStoredValid_{false};
    double fiveHourRateStoredValue_{0.0};
    int fiveHourRateStartRemaining_{0};
    std::int64_t fiveHourRateResetAtUnix_{0};
    std::int64_t fiveHourRateStartMs_{0};
    std::string fiveHourRateThreadId_;
    std::string fiveHourRateTurnId_;

    std::array<wchar_t, 1024> tooltipBuffer_{};
    std::unique_ptr<DesktopThreadMonitor> threadMonitor_;
    RendererActivityMonitor rendererActivityMonitor_;

    std::thread workerThread_;
    std::mutex workerMutex_;
    std::condition_variable workerCv_;
    bool workerStop_{false};
    bool workerActive_{false};
    bool reconnectRequested_{false};
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\CodexTaskbar.Singleton");
    if (!mutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    CodexTaskbarApp app(instance);
    if (!app.Initialize()) {
        MessageBoxW(
            nullptr,
            L"CodexTaskbar 初始化失败。",
            L"CodexTaskbar",
            MB_OK | MB_ICONERROR);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    const int result = app.Run();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return result;
}

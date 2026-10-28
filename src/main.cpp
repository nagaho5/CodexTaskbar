#include "AppServerClient.h"
#include "DesktopThreadMonitor.h"

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
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <memory>
#include <mutex>
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

constexpr UINT kMenuSettings = 1001;
constexpr UINT kMenuAutoStart = 1002;
constexpr UINT kMenuReconnect = 1003;
constexpr UINT kMenuAbout = 1004;
constexpr UINT kMenuExit = 1005;

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

constexpr COLORREF kDefaultWorkingColor = RGB(79, 195, 247);

struct ColoredSegment {
    std::wstring text;
    COLORREF color{RGB(255, 255, 255)};
};

struct QuotaPayload {
    QuotaSnapshot snapshot;
};

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

bool IsCodexDesktopRunning() {
    // The Store/MSIX build currently uses ChatGPT.exe as its Electron shell.
    // Keep the taskbar widget visible while that Codex package process exists,
    // including when its window is minimized. Do not use codex-ipc lifetime:
    // the pipe may outlive the UI after Codex has actually exited.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            const bool candidate =
                _wcsicmp(entry.szExeFile, L"ChatGPT.exe") == 0 ||
                _wcsicmp(entry.szExeFile, L"Codex.exe") == 0 ||
                _wcsicmp(entry.szExeFile, L"OpenAI.Codex.exe") == 0;
            if (candidate && IsCodexProcess(entry.th32ProcessID)) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
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
        swprintf_s(text, L"%02lld:%02lld",
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
        fontSize_ = std::clamp(ReadRegistryDword(L"FontSize", 10), 8, 24);
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
        automaticThreadMode_ = ReadRegistryDword(L"AutomaticThreadMode", 0) != 0;
        manualThreadId_ = ReadRegistryString(L"ManualThreadId");

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
        SetTimer(mainWindow_, kHoverTimer, 200, nullptr);
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

        // Elapsed turn time changes even when the IPC state itself does not.
        // Refresh its taskbar/hover presentation once per second while timing
        // information is available.
        if (threadMetrics_.hasTurnTiming && threadMetrics_.activeTurn) {
            InvalidateWidget();
            if (hoverPopupVisible_) {
                UpdateTooltipText();
            }
        }

        if ((tickCount_ % 2) != 1 && codexRunningKnown_) {
            if (widgetVisible_) {
                PositionWidget();
            }
            return;
        }

        const bool running = IsCodexDesktopRunning();
        codexRunningKnown_ = true;
        if (running != codexRunning_) {
            codexRunning_ = running;
            SetWorkerActive(running);
            if (threadMonitor_) {
                threadMonitor_->SetActive(running);
            }

            if (running) {
                EnsureWidget();
                PositionWidget();
                ShowWindow(widgetWindow_, SW_SHOWNOACTIVATE);
                widgetVisible_ = true;
            } else {
                HideWidget();
                quotaValid_ = false;
                threadMetrics_ = {};
                InvalidateWidget();
                UpdateTooltipText();
            }
        } else if (running && widgetVisible_) {
            PositionWidget();
        }
    }

    void ApplyQuota(const QuotaSnapshot& snapshot) {
        quota_ = snapshot;
        quotaValid_ = true;
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

    void ApplyThreadMetrics(const ThreadMetrics& metrics) {
        threadMetrics_ = metrics;
        if (widgetVisible_) {
            PositionWidget();
        }
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
            SetTextColor(dc, segments[i].color);
            TextOutW(
                dc,
                x,
                y,
                segments[i].text.c_str(),
                static_cast<int>(segments[i].text.size()));
            x += sizes[i].cx;
        }
        SetTextColor(dc, RGB(255, 255, 255));
    }

    std::vector<ColoredSegment> FirstLineSegments() const {
        std::vector<ColoredSegment> segments;
        if (showFiveHourPercent_) {
            const bool valid = quotaValid_ && quota_.fiveHour.valid;
            segments.push_back({
                valid
                    ? std::to_wstring(quota_.fiveHour.remainingPercent) + L"%"
                    : L"--%",
                threadMetrics_.activeTurn && valid
                    ? workingColor_
                    : RGB(255, 255, 255)});
        }
        if (showFiveHourTime_) {
            segments.push_back({
                quotaValid_ && quota_.fiveHour.valid
                    ? FormatResetTime(quota_.fiveHour.resetsAtUnix)
                    : L"--:--",
                RGB(255, 255, 255)});
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

        if (showSevenDayPercent_) {
            append(
                quotaValid_ && quota_.sevenDay.valid
                    ? std::to_wstring(quota_.sevenDay.remainingPercent) + L"%"
                    : L"--%",
                RGB(255, 255, 255));
        }
        if (showContext_) {
            const bool valid = threadMetrics_.hasContext;
            append(
                valid
                    ? std::to_wstring(threadMetrics_.contextPercent) + L"%"
                    : L"--%",
                threadMetrics_.activeTurn && valid
                    ? workingColor_
                    : RGB(255, 255, 255));
        }
        if (showCache_) {
            append(
                threadMetrics_.hasCache
                    ? std::to_wstring(threadMetrics_.cachePercent) + L"%"
                    : L"--%",
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
        HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font_));

        if (verticalTaskbar_) {
            const std::wstring first = FirstLine();
            const std::wstring second = SecondLine();
            const std::wstring duration = TaskDurationLine();
            const bool diffVisible = IsDiffRowVisible();

            struct Row {
                int kind;
                std::wstring text;
            };
            std::vector<Row> rows;
            if (!first.empty()) rows.push_back({2, first});
            if (!second.empty()) rows.push_back({3, second});
            if (diffVisible) rows.push_back({1, {}});
            if (!duration.empty()) rows.push_back({0, duration});

            if (rows.empty()) rows.push_back({0, L""});
            const int rowHeight = std::max(
                1,
                static_cast<int>(client.bottom - client.top) / static_cast<int>(rows.size()));

            for (std::size_t index = 0; index < rows.size(); ++index) {
                RECT row{
                    client.left,
                    client.top + static_cast<LONG>(index * rowHeight),
                    client.right,
                    index + 1 == rows.size()
                        ? client.bottom
                        : client.top + static_cast<LONG>((index + 1) * rowHeight)};

                if (rows[index].kind == 1) {
                    DrawDiffRow(dc, row);
                } else if (rows[index].kind == 2) {
                    DrawCenteredSegments(dc, row, FirstLineSegments());
                } else if (rows[index].kind == 3) {
                    DrawCenteredSegments(dc, row, SecondLineSegments());
                } else {
                    SetTextColor(dc, RGB(255, 255, 255));
                    DrawTextW(
                        dc,
                        rows[index].text.c_str(),
                        -1,
                        &row,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
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
                        std::to_wstring(threadMetrics_.changedFiles) + L" ",
                        RGB(255, 255, 255)});
                }
                diffSegments.push_back({
                    std::to_wstring(threadMetrics_.addedLines) + L" ",
                    RGB(66, 214, 116)});
                diffSegments.push_back({
                    std::to_wstring(threadMetrics_.removedLines),
                    RGB(242, 88, 88)});
                appendGroup(diffSegments);
            }

            const std::wstring duration = TaskDurationLine();
            if (!duration.empty()) {
                appendGroup({{duration, RGB(255, 255, 255)}});
            }
            DrawCenteredSegments(dc, client, segments);
        }

        SelectObject(dc, oldFont);
        EndPaint(hwnd, &ps);
    }

    void DrawDiffRow(HDC dc, const RECT& row) const {
        const std::wstring files = std::to_wstring(threadMetrics_.changedFiles);
        const std::wstring added = std::to_wstring(threadMetrics_.addedLines);
        const std::wstring removed = std::to_wstring(threadMetrics_.removedLines);
        const int spacing = 5;

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
        taskbarWindow_ = nullptr;
        rebarWindow_ = nullptr;
        if (codexRunning_) {
            EnsureWidget();
            PositionWidget();
            ShowWindow(widgetWindow_, SW_SHOWNOACTIVATE);
            widgetVisible_ = true;
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
            470,
            455,
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
        CreateWindowW(L"STATIC", L"字号 (pt)：", WS_CHILD | WS_VISIBLE,
            24, 20, 90, 24, hwnd, nullptr, instance_, nullptr);

        HWND fontEdit = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
            118, 16, 72, 26, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditFontSize)), instance_, nullptr);
        wchar_t fontValue[16]{};
        swprintf_s(fontValue, L"%d", fontSize_);
        SetWindowTextW(fontEdit, fontValue);

        CreateWindowW(L"STATIC", L"任务栏显示：", WS_CHILD | WS_VISIBLE,
            24, 54, 100, 22, hwnd, nullptr, instance_, nullptr);

        auto checkbox = [&](int id, const wchar_t* text, int x, int y, bool checked, int width = 150) {
            HWND control = CreateWindowW(
                L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                x, y, width, 22, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
            SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
        };

        checkbox(kCheckShowFiveHourPercent, L"5小时百分比", 24, 78, showFiveHourPercent_);
        checkbox(kCheckShowFiveHourTime, L"5小时恢复时间", 210, 78, showFiveHourTime_, 180);
        checkbox(kCheckShowSevenDayPercent, L"7天百分比", 24, 103, showSevenDayPercent_);
        checkbox(kCheckShowContext, L"Context百分比", 210, 103, showContext_, 180);
        checkbox(kCheckShowCache, L"Cache命中率", 24, 128, showCache_);
        checkbox(kCheckShowDiff, L"代码改动", 210, 128, showDiff_, 100);
        checkbox(kCheckShowDiffFiles, L"显示文件数", 318, 128, showDiffFiles_, 112);
        checkbox(kCheckShowTaskDuration, L"任务耗时", 24, 153, showTaskDuration_);

        CreateWindowW(
            L"STATIC", L"运行中颜色：", WS_CHILD | WS_VISIBLE,
            210, 153, 88, 22, hwnd, nullptr, instance_, nullptr);
        CreateWindowW(
            L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            298, 149, 76, 26, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonWorkingColor)), instance_, nullptr);
        CreateWindowW(
            L"STATIC", L"76%", WS_CHILD | WS_VISIBLE | SS_CENTER,
            382, 153, 48, 22, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStaticWorkingColorPreview)), instance_, nullptr);

        CreateWindowW(L"STATIC", L"会话来源：", WS_CHILD | WS_VISIBLE,
            24, 191, 100, 22, hwnd, nullptr, instance_, nullptr);

        HWND manualRadio = CreateWindowW(
            L"BUTTON", L"手动绑定 Thread ID", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            24, 215, 180, 22, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRadioManualThread)), instance_, nullptr);
        HWND autoRadio = CreateWindowW(
            L"BUTTON", L"自动跟随活动任务（实验）", WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            210, 215, 220, 22, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRadioAutoThread)), instance_, nullptr);
        SendMessageW(manualRadio, BM_SETCHECK, automaticThreadMode_ ? BST_UNCHECKED : BST_CHECKED, 0);
        SendMessageW(autoRadio, BM_SETCHECK, automaticThreadMode_ ? BST_CHECKED : BST_UNCHECKED, 0);

        CreateWindowW(L"STATIC", L"Thread ID：", WS_CHILD | WS_VISIBLE,
            24, 251, 80, 22, hwnd, nullptr, instance_, nullptr);
        CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", manualThreadId_.c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            104, 247, 326, 26, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditThreadId)), instance_, nullptr);

        checkbox(kCheckAutoStart, L"开机自动启动", 24, 293, autoStart_);

        CreateWindowW(
            L"STATIC",
            L"提示：隐藏的项目仍会在鼠标悬浮提示中显示完整信息。",
            WS_CHILD | WS_VISIBLE,
            24, 327, 410, 22, hwnd, nullptr, instance_, nullptr);

        CreateWindowW(
            L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            270, 369, 76, 28, hwnd,
            reinterpret_cast<HMENU>(IDOK), instance_, nullptr);
        CreateWindowW(
            L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            358, 369, 76, 28, hwnd,
            reinterpret_cast<HMENU>(IDCANCEL), instance_, nullptr);

        EnumChildWindows(
            hwnd,
            [](HWND child, LPARAM) -> BOOL {
                // DarkMode_Explorer is available on supported Windows 10 builds;
                // WM_CTLCOLOR below is the fallback for controls that ignore it.
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
        item->itemWidth = static_cast<UINT>(MulDiv(176, static_cast<int>(dpi), 96));
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
        const int requested = _wtoi(text);
        fontSize_ = std::clamp(requested, 8, 24);

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
        workingColor_ = pendingWorkingColor_;
        automaticThreadMode_ = checked(kRadioAutoThread);

        wchar_t threadText[256]{};
        GetDlgItemTextW(hwnd, kEditThreadId, threadText, static_cast<int>(std::size(threadText)));
        manualThreadId_ = threadText;
        while (!manualThreadId_.empty() && iswspace(manualThreadId_.front())) manualThreadId_.erase(manualThreadId_.begin());
        while (!manualThreadId_.empty() && iswspace(manualThreadId_.back())) manualThreadId_.pop_back();

        WriteRegistryDword(L"FontSize", static_cast<DWORD>(fontSize_));
        WriteRegistryDword(L"AutoStart", autoStart_ ? 1u : 0u);
        WriteRegistryDword(L"ShowFiveHourPercent", showFiveHourPercent_ ? 1u : 0u);
        WriteRegistryDword(L"ShowFiveHourTime", showFiveHourTime_ ? 1u : 0u);
        WriteRegistryDword(L"ShowSevenDayPercent", showSevenDayPercent_ ? 1u : 0u);
        WriteRegistryDword(L"ShowContext", showContext_ ? 1u : 0u);
        WriteRegistryDword(L"ShowCache", showCache_ ? 1u : 0u);
        WriteRegistryDword(L"ShowDiff", showDiff_ ? 1u : 0u);
        WriteRegistryDword(L"ShowDiffFiles", showDiffFiles_ ? 1u : 0u);
        WriteRegistryDword(L"ShowTaskDuration", showTaskDuration_ ? 1u : 0u);
        WriteRegistryDword(L"WorkingColor", static_cast<DWORD>(workingColor_));
        WriteRegistryDword(L"AutomaticThreadMode", automaticThreadMode_ ? 1u : 0u);
        WriteRegistryString(L"ManualThreadId", manualThreadId_);
        SetAutoStartRegistry(autoStart_);

        if (threadMonitor_) {
            threadMonitor_->UpdateConfig(WideToUtf8(manualThreadId_), automaticThreadMode_);
        }

        RecreateFont();
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
                app->UpdateHoverState();
                return 0;
            }
            break;

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

        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN:
            if (app) {
                app->ResetHoverState();
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

    void RecreateFont() {
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }

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
            if (windowDpi > 0) {
                dpi = windowDpi;
            }
        }

        const int height = -MulDiv(fontSize_, static_cast<int>(dpi), 72);
        font_ = CreateFontW(
            height,
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
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

    void EnsureWidget() {
        taskbarWindow_ = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!taskbarWindow_) {
            return;
        }

        rebarWindow_ = FindWindowExW(taskbarWindow_, nullptr, L"ReBarWindow32", nullptr);
        if (!rebarWindow_) {
            rebarWindow_ = FindWindowExW(taskbarWindow_, nullptr, L"WorkerW", nullptr);
        }
        if (!rebarWindow_) {
            rebarWindow_ = taskbarWindow_;
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

        // SetParent can move a PerMonitorV2 child into Explorer's DPI context.
        // Rebuild the font only after the widget has its final taskbar parent.
        if (dpiContextChanged) {
            RecreateFont();
        }

    }

    void PositionWidget() {
        EnsureWidget();
        if (!widgetWindow_ || !rebarWindow_) {
            return;
        }

        RECT taskbarRect{};
        GetWindowRect(taskbarWindow_, &taskbarRect);
        verticalTaskbar_ =
            (taskbarRect.bottom - taskbarRect.top) > (taskbarRect.right - taskbarRect.left);

        RECT rebarClient{};
        GetClientRect(rebarWindow_, &rebarClient);

        HDC dc = GetDC(widgetWindow_);
        HFONT oldFont = dc ? static_cast<HFONT>(SelectObject(dc, font_)) : nullptr;
        TEXTMETRICW metrics{};
        if (dc) {
            GetTextMetricsW(dc, &metrics);
            if (oldFont) {
                SelectObject(dc, oldFont);
            }
            ReleaseDC(widgetWindow_, dc);
        }

        const int lineHeight = std::max(14, static_cast<int>(metrics.tmHeight) + 1);
        int width = 0;
        int height = 0;

        if (verticalTaskbar_) {
            width = std::max(40, static_cast<int>(rebarClient.right - rebarClient.left) - 4);
            height = lineHeight * std::max(1, VisibleRowCount()) + 4;
        } else {
            width = std::max(150, fontSize_ * 15);
            height = std::max(24, std::min(static_cast<int>(rebarClient.bottom - rebarClient.top), lineHeight + 6));
        }

        HWND trafficMonitor = FindDescendantByTitle(taskbarWindow_, kTrafficMonitorTitle);
        HWND trayNotify = FindDescendantByClass(taskbarWindow_, L"TrayNotifyWnd");

        if (verticalTaskbar_) {
            int anchorTop = rebarClient.bottom;

            HWND anchor = trafficMonitor ? trafficMonitor : trayNotify;
            if (anchor) {
                RECT anchorRect{};
                GetWindowRect(anchor, &anchorRect);
                anchorRect = ScreenRectToClient(rebarWindow_, anchorRect);
                anchorTop = anchorRect.top;
            }

            const int y = std::max(static_cast<int>(rebarClient.top), anchorTop - height - 1);
            const int x = std::max(0, (static_cast<int>(rebarClient.right - rebarClient.left) - width) / 2);

            // Never resize or reposition the Windows task-list region. This widget is
            // deliberately overlay-only so a layout mistake cannot hide task buttons.
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
                trayRect = ScreenRectToClient(rebarWindow_, trayRect);
                x = trayRect.left - width - 2;
            }
            x = std::max(static_cast<int>(rebarClient.left), x);
            const int y = std::max(static_cast<int>(rebarClient.top), (static_cast<int>(rebarClient.bottom - rebarClient.top) - height) / 2);
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
            line += quotaValid_ && quota_.fiveHour.valid
                ? std::to_wstring(quota_.fiveHour.remainingPercent) + L"%"
                : L"--%";
        }
        if (showFiveHourTime_) {
            line += quotaValid_ && quota_.fiveHour.valid
                ? FormatResetTime(quota_.fiveHour.resetsAtUnix)
                : L"--:--";
        }
        return line;
    }

    std::wstring SecondLine() const {
        std::vector<std::wstring> values;

        if (showSevenDayPercent_) {
            values.push_back(
                quotaValid_ && quota_.sevenDay.valid
                    ? std::to_wstring(quota_.sevenDay.remainingPercent) + L"%"
                    : L"--%");
        }
        if (showContext_) {
            values.push_back(
                threadMetrics_.hasContext
                    ? std::to_wstring(threadMetrics_.contextPercent) + L"%"
                    : L"--%");
        }
        if (showCache_) {
            values.push_back(
                threadMetrics_.hasCache
                    ? std::to_wstring(threadMetrics_.cachePercent) + L"%"
                    : L"--%");
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

    std::wstring TaskDurationLine() const {
        if (!showTaskDuration_ || !threadMetrics_.activeTurn) return {};
        return FormatDurationCompact(CurrentTaskDurationMs());
    }

    bool IsDiffRowVisible() const {
        return threadMetrics_.activeTurn && showDiff_ && threadMetrics_.hasDiff &&
            (threadMetrics_.changedFiles > 0 ||
             threadMetrics_.addedLines > 0 ||
             threadMetrics_.removedLines > 0);
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
        HideHoverPopup();
    }

    void UpdateHoverState() {
        if (!widgetVisible_ || !widgetWindow_ || !IsWindow(widgetWindow_) ||
            !IsWindowVisible(widgetWindow_)) {
            ResetHoverState();
            return;
        }

        POINT cursor{};
        RECT widgetRect{};
        if (!GetCursorPos(&cursor) || !GetWindowRect(widgetWindow_, &widgetRect)) {
            ResetHoverState();
            return;
        }

        const bool inside = PtInRect(&widgetRect, cursor) != FALSE;
        if (!inside) {
            ResetHoverState();
            return;
        }

        const ULONGLONG now = GetTickCount64();
        if (!hoverInside_) {
            hoverInside_ = true;
            hoverStartTick_ = now;
            return;
        }

        if (!hoverPopupVisible_ && now - hoverStartTick_ >= 250) {
            ShowHoverPopup(cursor);
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
            text += L"缓存：" + std::to_wstring(threadMetrics_.cachePercent) + L"% (" +
                    FormatInteger(threadMetrics_.cachedInputTokens) + L" / " +
                    FormatInteger(threadMetrics_.inputTokens) + L")";
        } else {
            text += L"缓存：暂无数据";
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
        switch (command) {
        case kMenuSettings:
            OpenSettings();
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
                L"CodexTaskbar v0.2.7\n\n"
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
    HFONT font_{nullptr};
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
    bool automaticThreadMode_{false};
    int fontSize_{10};
    unsigned tickCount_{0};

    QuotaSnapshot quota_{};
    ThreadMetrics threadMetrics_{};
    std::wstring manualThreadId_;
    std::array<wchar_t, 1024> tooltipBuffer_{};
    std::unique_ptr<DesktopThreadMonitor> threadMonitor_;

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

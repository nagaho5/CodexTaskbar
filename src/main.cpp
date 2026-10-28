#include "AppServerClient.h"

#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kMainClass[] = L"CodexTaskbar.Main";
constexpr wchar_t kWidgetClass[] = L"CodexTaskbar.Widget";
constexpr wchar_t kSettingsClass[] = L"CodexTaskbar.Settings";
constexpr wchar_t kSettingsKey[] = L"Software\\Nagaho5\\CodexTaskbar";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"CodexTaskbar";
constexpr wchar_t kTrafficMonitorTitle[] = L"TrafficMonitorTaskbarWindow";

constexpr UINT kTrayId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kQuotaMessage = WM_APP + 2;
constexpr UINT kQuotaErrorMessage = WM_APP + 3;
constexpr UINT_PTR kTickTimer = 1;

constexpr UINT kMenuSettings = 1001;
constexpr UINT kMenuAutoStart = 1002;
constexpr UINT kMenuReconnect = 1003;
constexpr UINT kMenuAbout = 1004;
constexpr UINT kMenuExit = 1005;

constexpr int kEditFontSize = 2001;
constexpr int kCheckAutoStart = 2002;

struct QuotaPayload {
    QuotaSnapshot snapshot;
};

std::wstring ModulePath() {
    wchar_t buffer[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return len > 0 ? std::wstring(buffer, len) : std::wstring();
}

bool IsCodexDesktopRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"OpenAI.Codex.exe") == 0 ||
                _wcsicmp(entry.szExeFile, L"OpenAI.Codex") == 0) {
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
        ShutdownWorker();
        RestoreTaskList();
        RemoveTrayIcon();
        if (font_) {
            DeleteObject(font_);
        }
    }

    bool Initialize() {
        fontSize_ = std::clamp(ReadRegistryDword(L"FontSize", 10), 8, 24);
        autoStart_ = ReadRegistryDword(L"AutoStart", 1) != 0;

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

            if (running) {
                EnsureWidget();
                PositionWidget();
                ShowWindow(widgetWindow_, SW_SHOWNOACTIVATE);
                widgetVisible_ = true;
            } else {
                HideWidget();
                quotaValid_ = false;
                InvalidateWidget();
            }
        } else if (running && widgetVisible_) {
            PositionWidget();
        }
    }

    void ApplyQuota(const QuotaSnapshot& snapshot) {
        quota_ = snapshot;
        quotaValid_ = true;
        InvalidateWidget();
        UpdateTrayTip();
    }

    void MarkQuotaUnavailable() {
        quotaValid_ = false;
        InvalidateWidget();
        UpdateTrayTip();
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
            const int middle = (client.top + client.bottom) / 2;
            RECT first{client.left, client.top, client.right, middle};
            RECT second{client.left, middle, client.right, client.bottom};
            const std::wstring line1 = FirstLine();
            const std::wstring line2 = SecondLine();
            DrawTextW(dc, line1.c_str(), -1, &first, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            DrawTextW(dc, line2.c_str(), -1, &second, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            const std::wstring line = HorizontalLine();
            DrawTextW(dc, line.c_str(), -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }

        SelectObject(dc, oldFont);
        EndPaint(hwnd, &ps);
    }

    void ShowContextMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }

        AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置...");
        AppendMenuW(
            menu,
            MF_STRING | (autoStart_ ? MF_CHECKED : MF_UNCHECKED),
            kMenuAutoStart,
            L"开机自动启动");
        AppendMenuW(menu, MF_STRING, kMenuReconnect, L"重新连接");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuAbout, L"关于");
        AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

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
        taskListWindow_ = nullptr;
        reservedTaskList_ = false;
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

        settingsWindow_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW,
            kSettingsClass,
            L"CodexTaskbar 设置",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            310,
            170,
            mainWindow_,
            nullptr,
            instance_,
            this);
        if (!settingsWindow_) {
            return;
        }

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
        CreateWindowW(
            L"STATIC",
            L"字号 (pt)：",
            WS_CHILD | WS_VISIBLE,
            24,
            24,
            90,
            24,
            hwnd,
            nullptr,
            instance_,
            nullptr);

        HWND edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            nullptr,
            WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
            118,
            20,
            72,
            26,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditFontSize)),
            instance_,
            nullptr);

        wchar_t value[16]{};
        swprintf_s(value, L"%d", fontSize_);
        SetWindowTextW(edit, value);

        HWND check = CreateWindowW(
            L"BUTTON",
            L"开机自动启动",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            24,
            60,
            180,
            24,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCheckAutoStart)),
            instance_,
            nullptr);
        SendMessageW(check, BM_SETCHECK, autoStart_ ? BST_CHECKED : BST_UNCHECKED, 0);

        CreateWindowW(
            L"BUTTON",
            L"确定",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            116,
            100,
            76,
            28,
            hwnd,
            reinterpret_cast<HMENU>(IDOK),
            instance_,
            nullptr);

        CreateWindowW(
            L"BUTTON",
            L"取消",
            WS_CHILD | WS_VISIBLE,
            204,
            100,
            76,
            28,
            hwnd,
            reinterpret_cast<HMENU>(IDCANCEL),
            instance_,
            nullptr);
    }

    void SaveSettingsFromWindow(HWND hwnd) {
        wchar_t text[32]{};
        GetDlgItemTextW(hwnd, kEditFontSize, text, static_cast<int>(std::size(text)));
        const int requested = _wtoi(text);
        const int newSize = std::clamp(requested, 8, 24);
        const bool newAutoStart =
            SendDlgItemMessageW(hwnd, kCheckAutoStart, BM_GETCHECK, 0, 0) == BST_CHECKED;

        fontSize_ = newSize;
        autoStart_ = newAutoStart;
        WriteRegistryDword(L"FontSize", static_cast<DWORD>(fontSize_));
        WriteRegistryDword(L"AutoStart", autoStart_ ? 1u : 0u);
        SetAutoStartRegistry(autoStart_);

        RecreateFont();
        if (widgetVisible_) {
            PositionWidget();
            InvalidateWidget();
        }
        DestroyWindow(hwnd);
    }

    void SettingsDestroyed(HWND hwnd) {
        if (settingsWindow_ == hwnd) {
            settingsWindow_ = nullptr;
        }
    }

    void WidgetDestroyed(HWND hwnd) {
        if (widgetWindow_ == hwnd) {
            widgetWindow_ = nullptr;
            widgetVisible_ = false;
        }
    }

    UINT TaskbarCreatedMessage() const {
        return taskbarCreatedMessage_;
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

        case WM_DESTROY:
            KillTimer(hwnd, kTickTimer);
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

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            if (app) {
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
                app->CreateSettingsControls(hwnd);
            }
            return 0;

        case WM_COMMAND:
            if (!app) {
                break;
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

        WNDCLASSEXW settingsClass{};
        settingsClass.cbSize = sizeof(settingsClass);
        settingsClass.hInstance = instance_;
        settingsClass.lpfnWndProc = SettingsWndProc;
        settingsClass.lpszClassName = kSettingsClass;
        settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        settingsClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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

        HDC dc = GetDC(nullptr);
        const int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc) {
            ReleaseDC(nullptr, dc);
        }

        const int height = -MulDiv(fontSize_, dpi, 72);
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
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
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
        } else if (GetParent(widgetWindow_) != rebarWindow_) {
            SetParent(widgetWindow_, rebarWindow_);
        }

        taskListWindow_ = FindWindowExW(rebarWindow_, nullptr, L"MSTaskSwWClass", nullptr);
        if (!taskListWindow_) {
            taskListWindow_ = FindWindowExW(rebarWindow_, nullptr, L"MSTaskListWClass", nullptr);
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
            width = std::max(40, (rebarClient.right - rebarClient.left) - 4);
            height = lineHeight * 2 + 4;
        } else {
            width = std::max(150, fontSize_ * 15);
            height = std::max(24, std::min(rebarClient.bottom - rebarClient.top, lineHeight + 6));
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

            const int y = std::max(rebarClient.top, anchorTop - height - 1);
            const int x = std::max(0, ((rebarClient.right - rebarClient.left) - width) / 2);

            ReserveTaskListTo(y);
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
            x = std::max(rebarClient.left, x);
            const int y = std::max(rebarClient.top, ((rebarClient.bottom - rebarClient.top) - height) / 2);
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

    void ReserveTaskListTo(int widgetTop) {
        if (!taskListWindow_ || !IsWindow(taskListWindow_) || !rebarWindow_) {
            return;
        }

        RECT current{};
        GetWindowRect(taskListWindow_, &current);
        current = ScreenRectToClient(rebarWindow_, current);

        if (!reservedTaskList_) {
            originalTaskListRect_ = current;
            reservedTaskList_ = true;
        }

        const int desiredHeight = std::max(1, widgetTop - current.top - 1);
        if ((current.bottom - current.top) != desiredHeight) {
            SetWindowPos(
                taskListWindow_,
                nullptr,
                current.left,
                current.top,
                current.right - current.left,
                desiredHeight,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    void RestoreTaskList() {
        if (!reservedTaskList_ || !taskListWindow_ || !IsWindow(taskListWindow_) || !rebarWindow_) {
            reservedTaskList_ = false;
            return;
        }

        HWND trafficMonitor = taskbarWindow_ ? FindDescendantByTitle(taskbarWindow_, kTrafficMonitorTitle) : nullptr;
        if (trafficMonitor) {
            RECT current{};
            RECT traffic{};
            GetWindowRect(taskListWindow_, &current);
            GetWindowRect(trafficMonitor, &traffic);
            current = ScreenRectToClient(rebarWindow_, current);
            traffic = ScreenRectToClient(rebarWindow_, traffic);

            const int height = std::max(1, traffic.top - current.top - 1);
            SetWindowPos(
                taskListWindow_,
                nullptr,
                current.left,
                current.top,
                current.right - current.left,
                height,
                SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            SetWindowPos(
                taskListWindow_,
                nullptr,
                originalTaskListRect_.left,
                originalTaskListRect_.top,
                originalTaskListRect_.right - originalTaskListRect_.left,
                originalTaskListRect_.bottom - originalTaskListRect_.top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }

        reservedTaskList_ = false;
    }

    void HideWidget() {
        if (widgetWindow_ && IsWindow(widgetWindow_)) {
            ShowWindow(widgetWindow_, SW_HIDE);
        }
        widgetVisible_ = false;
        RestoreTaskList();
        UpdateTrayTip();
    }

    std::wstring FirstLine() const {
        if (!quotaValid_ || !quota_.fiveHour.valid) {
            return L"--% --:--";
        }
        return std::to_wstring(quota_.fiveHour.remainingPercent) +
               L"% " +
               FormatResetTime(quota_.fiveHour.resetsAtUnix);
    }

    std::wstring SecondLine() const {
        if (!quotaValid_ || !quota_.sevenDay.valid) {
            return L"--%";
        }
        return std::to_wstring(quota_.sevenDay.remainingPercent) + L"%";
    }

    std::wstring HorizontalLine() const {
        return FirstLine() + L"  ·  " + SecondLine();
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
                L"CodexTaskbar v0.1\n\n"
                L"在 Windows 10 任务栏显示 Codex 5 小时与 7 天剩余额度。\n"
                L"数据来自官方 Codex app-server 的 account/rateLimits/read。",
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
                if (client.ReadQuota(snapshot)) {
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

                std::unique_lock lock(workerMutex_);
                workerCv_.wait_for(lock, std::chrono::seconds(60), [this] {
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
        InvalidateWidget();
        workerCv_.notify_all();
    }

    HINSTANCE instance_{nullptr};
    HWND mainWindow_{nullptr};
    HWND widgetWindow_{nullptr};
    HWND settingsWindow_{nullptr};
    HWND taskbarWindow_{nullptr};
    HWND rebarWindow_{nullptr};
    HWND taskListWindow_{nullptr};
    HFONT font_{nullptr};

    UINT taskbarCreatedMessage_{0};
    bool trayAdded_{false};
    bool codexRunning_{false};
    bool codexRunningKnown_{false};
    bool widgetVisible_{false};
    bool verticalTaskbar_{true};
    bool quotaValid_{false};
    bool autoStart_{true};
    bool reservedTaskList_{false};
    int fontSize_{10};
    unsigned tickCount_{0};

    RECT originalTaskListRect_{};
    QuotaSnapshot quota_{};

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

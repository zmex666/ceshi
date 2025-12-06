#include <Windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <Psapi.h>
#include <shellapi.h>
#include <CommCtrl.h>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")

// 全局变量
std::vector<std::wstring> g_blacklist;
std::mutex g_blacklistMutex;
std::atomic<bool> g_isMonitoring(false);
std::thread g_monitorThread;
HWND g_hMainWindow = NULL;
HWND g_hListBox = NULL;
HWND g_hStartButton = NULL;
HWND g_hStopButton = NULL;
HWND g_hAddButton = NULL;
HWND g_hRemoveButton = NULL;
HWND g_hStatusStatic = NULL;
NOTIFYICONDATA g_notifyIconData;

// 函数声明
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void CreateMainWindow();
void CreateControls(HWND hwnd);
void StartMonitoring();
void StopMonitoring();
void MonitorThread();
bool IsWindowFullscreen(HWND hwnd);
bool GetProcessInfo(HWND hwnd, DWORD& processId, std::wstring& processName, std::wstring& exePath);
bool TerminateProcessById(DWORD processId);
void AddCurrentAppToBlacklist();
void RemoveSelectedFromBlacklist();
void UpdateListBox();
void ShowNotification(const std::wstring& title, const std::wstring& message);
void ShowError(const std::wstring& message);
void AddToBlacklist(const std::wstring& processName);
bool IsInBlacklist(const std::wstring& processName);
void RemoveFromBlacklist(const std::wstring& processName);
void SaveBlacklist();
void LoadBlacklist();
LRESULT CALLBACK MonitorWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

// 主函数
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // 初始化公共控件
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
    InitCommonControlsEx(&icex);

    // 加载黑名单
    LoadBlacklist();

    // 创建主窗口
    CreateMainWindow();

    // 消息循环
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 清理
    StopMonitoring();
    Shell_NotifyIcon(NIM_DELETE, &g_notifyIconData);

    return (int)msg.wParam;
}

// 窗口过程
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE:
        CreateControls(hwnd);
        UpdateListBox();
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_START_BUTTON:
            StartMonitoring();
            break;
        case IDC_STOP_BUTTON:
            StopMonitoring();
            break;
        case IDC_ADD_BUTTON:
            AddCurrentAppToBlacklist();
            break;
        case IDC_REMOVE_BUTTON:
            RemoveSelectedFromBlacklist();
            break;
        case IDC_BLACKLIST_LIST:
            if (HIWORD(wParam) == LBN_DBLCLK) {
                RemoveSelectedFromBlacklist();
            }
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    case WM_SYSCOMMAND:
        if (wParam == SC_MINIMIZE) {
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_TRAYICON:
        switch (lParam) {
        case WM_LBUTTONDOWN:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            break;
        case WM_RBUTTONDOWN:
            // 可以添加右键菜单
            break;
        }
        break;

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

// 创建主窗口
void CreateMainWindow() {
    const wchar_t CLASS_NAME[] = L"FullscreenMonitorClass";

    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLASS_NAME;

    RegisterClassEx(&wc);

    g_hMainWindow = CreateWindowEx(
        0,
        CLASS_NAME,
        L"全屏应用监控器",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 400,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    if (g_hMainWindow == NULL) {
        ShowError(L"创建窗口失败");
        exit(1);
    }

    // 创建系统托盘图标
    ZeroMemory(&g_notifyIconData, sizeof(NOTIFYICONDATA));
    g_notifyIconData.cbSize = sizeof(NOTIFYICONDATA);
    g_notifyIconData.hWnd = g_hMainWindow;
    g_notifyIconData.uID = 1;
    g_notifyIconData.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_notifyIconData.uCallbackMessage = WM_TRAYICON;
    g_notifyIconData.hIcon = LoadIcon(GetModuleHandle(NULL), IDI_APPLICATION);
    wcscpy_s(g_notifyIconData.szTip, L"全屏应用监控器");

    Shell_NotifyIcon(NIM_ADD, &g_notifyIconData);

    ShowWindow(g_hMainWindow, SW_SHOW);
    UpdateWindow(g_hMainWindow);
}

// 创建控件
void CreateControls(HWND hwnd) {
    // 状态标签
    g_hStatusStatic = CreateWindow(
        L"STATIC",
        L"监控状态: 未监控",
        WS_CHILD | WS_VISIBLE | SS_CENTER,
        20, 20, 460, 30,
        hwnd, (HMENU)IDC_STATUS_STATIC,
        GetModuleHandle(NULL), NULL
    );

    // 黑名单列表
    CreateWindow(
        L"STATIC",
        L"黑名单应用:",
        WS_CHILD | WS_VISIBLE,
        20, 70, 100, 20,
        hwnd, NULL,
        GetModuleHandle(NULL), NULL
    );

    g_hListBox = CreateWindow(
        L"LISTBOX",
        L"",
        WS_CHILD | WS_VISIBLE | LBS_STANDARD | WS_VSCROLL,
        20, 90, 460, 200,
        hwnd, (HMENU)IDC_BLACKLIST_LIST,
        GetModuleHandle(NULL), NULL
    );

    // 按钮
    g_hStartButton = CreateWindow(
        L"BUTTON",
        L"开始监控",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        20, 310, 100, 30,
        hwnd, (HMENU)IDC_START_BUTTON,
        GetModuleHandle(NULL), NULL
    );

    g_hStopButton = CreateWindow(
        L"BUTTON",
        L"停止监控",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
        130, 310, 100, 30,
        hwnd, (HMENU)IDC_STOP_BUTTON,
        GetModuleHandle(NULL), NULL
    );

    g_hAddButton = CreateWindow(
        L"BUTTON",
        L"添加当前应用",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        240, 310, 120, 30,
        hwnd, (HMENU)IDC_ADD_BUTTON,
        GetModuleHandle(NULL), NULL
    );

    g_hRemoveButton = CreateWindow(
        L"BUTTON",
        L"移除选中",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        370, 310, 110, 30,
        hwnd, (HMENU)IDC_REMOVE_BUTTON,
        GetModuleHandle(NULL), NULL
    );
}

// 开始监控
void StartMonitoring() {
    if (g_blacklist.empty()) {
        MessageBox(g_hMainWindow, L"请先添加需要监控的应用到黑名单", L"警告", MB_ICONWARNING);
        return;
    }

    if (!g_isMonitoring) {
        g_isMonitoring = true;
        g_monitorThread = std::thread(MonitorThread);
        
        SetWindowText(g_hStatusStatic, L"监控状态: 监控中...");
        EnableWindow(g_hStartButton, FALSE);
        EnableWindow(g_hStopButton, TRUE);
        EnableWindow(g_hAddButton, FALSE);
        EnableWindow(g_hRemoveButton, FALSE);
    }
}

// 停止监控
void StopMonitoring() {
    if (g_isMonitoring) {
        g_isMonitoring = false;
        if (g_monitorThread.joinable()) {
            g_monitorThread.join();
        }
        
        SetWindowText(g_hStatusStatic, L"监控状态: 未监控");
        EnableWindow(g_hStartButton, TRUE);
        EnableWindow(g_hStopButton, FALSE);
        EnableWindow(g_hAddButton, TRUE);
        EnableWindow(g_hRemoveButton, TRUE);
    }
}

// 监控线程
void MonitorThread() {
    while (g_isMonitoring) {
        try {
            // 枚举所有顶级窗口
            EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                if (IsWindowVisible(hwnd) && IsWindowFullscreen(hwnd)) {
                    DWORD processId;
                    std::wstring processName, exePath;
                    
                    if (GetProcessInfo(hwnd, processId, processName, exePath)) {
                        std::lock_guard<std::mutex> lock(g_blacklistMutex);
                        if (IsInBlacklist(processName)) {
                            if (TerminateProcessById(processId)) {
                                std::wstring message = L"已关闭全屏应用: " + processName;
                                ShowNotification(L"全屏监控器", message);
                            }
                        }
                    }
                }
                return TRUE;
            }, 0);
        }
        catch (...) {
            // 忽略异常，继续监控
        }
        
        // 每秒检查一次
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// 检查窗口是否全屏
bool IsWindowFullscreen(HWND hwnd) {
    try {
        RECT windowRect;
        if (!GetWindowRect(hwnd, &windowRect)) {
            return false;
        }

        // 获取显示器信息
        MONITORINFO monitorInfo = { 0 };
        monitorInfo.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitorInfo)) {
            return false;
        }

        // 检查窗口是否覆盖整个显示器
        return (windowRect.left == monitorInfo.rcMonitor.left &&
                windowRect.top == monitorInfo.rcMonitor.top &&
                windowRect.right == monitorInfo.rcMonitor.right &&
                windowRect.bottom == monitorInfo.rcMonitor.bottom);
    }
    catch (...) {
        return false;
    }
}

// 获取进程信息
bool GetProcessInfo(HWND hwnd, DWORD& processId, std::wstring& processName, std::wstring& exePath) {
    try {
        // 获取进程ID
        if (!GetWindowThreadProcessId(hwnd, &processId)) {
            return false;
        }

        // 打开进程
        HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
        if (hProcess == NULL) {
            return false;
        }

        // 获取进程名称
        WCHAR szProcessName[MAX_PATH] = { 0 };
        if (GetModuleBaseName(hProcess, NULL, szProcessName, MAX_PATH)) {
            processName = szProcessName;
        }

        // 获取可执行文件路径
        WCHAR szExePath[MAX_PATH] = { 0 };
        if (GetModuleFileNameEx(hProcess, NULL, szExePath, MAX_PATH)) {
            exePath = szExePath;
        }

        CloseHandle(hProcess);
        return !processName.empty();
    }
    catch (...) {
        return false;
    }
}

// 终止进程
bool TerminateProcessById(DWORD processId) {
    try {
        HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, processId);
        if (hProcess == NULL) {
            return false;
        }

        bool success = TerminateProcess(hProcess, 0) != 0;
        CloseHandle(hProcess);
        
        // 等待进程完全终止
        if (success) {
            DWORD exitCode;
            while (GetExitCodeProcess(hProcess, &exitCode) && exitCode == STILL_ACTIVE) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        
        return success;
    }
    catch (...) {
        return false;
    }
}

// 添加当前应用到黑名单
void AddCurrentAppToBlacklist() {
    try {
        HWND hwnd = GetForegroundWindow();
        if (hwnd == g_hMainWindow) {
            MessageBox(g_hMainWindow, L"请先切换到要添加的应用窗口", L"提示", MB_ICONINFORMATION);
            return;
        }

        DWORD processId;
        std::wstring processName, exePath;
        
        if (GetProcessInfo(hwnd, processId, processName, exePath)) {
            std::lock_guard<std::mutex> lock(g_blacklistMutex);
            if (!IsInBlacklist(processName)) {
                AddToBlacklist(processName);
                UpdateListBox();
                SaveBlacklist();
                MessageBox(g_hMainWindow, (L"已添加 " + processName + L" 到黑名单").c_str(), L"成功", MB_ICONINFORMATION);
            }
            else {
                MessageBox(g_hMainWindow, L"该应用已在黑名单中", L"提示", MB_ICONINFORMATION);
            }
        }
    }
    catch (const std::exception& e) {
        ShowError(std::wstring(L"添加失败: ") + std::wstring(e.what(), e.what() + strlen(e.what())));
    }
}

// 移除选中的应用
void RemoveSelectedFromBlacklist() {
    int selectedIndex = SendMessage(g_hListBox, LB_GETCURSEL, 0, 0);
    if (selectedIndex != LB_ERR) {
        int itemLength = SendMessage(g_hListBox, LB_GETTEXTLEN, selectedIndex, 0);
        if (itemLength > 0) {
            std::wstring processName(itemLength + 1, L'\0');
            SendMessage(g_hListBox, LB_GETTEXT, selectedIndex, (LPARAM)processName.data());
            
            std::lock_guard<std::mutex> lock(g_blacklistMutex);
            RemoveFromBlacklist(processName);
            UpdateListBox();
            SaveBlacklist();
        }
    }
}

// 更新列表框
void UpdateListBox() {
    SendMessage(g_hListBox, LB_RESETCONTENT, 0, 0);
    
    std::lock_guard<std::mutex> lock(g_blacklistMutex);
    for (const auto& processName : g_blacklist) {
        SendMessage(g_hListBox, LB_ADDSTRING, 0, (LPARAM)processName.c_str());
    }
}

// 显示通知
void ShowNotification(const std::wstring& title, const std::wstring& message) {
    // 更新托盘图标提示
    wcscpy_s(g_notifyIconData.szTip, message.c_str());
    Shell_NotifyIcon(NIM_MODIFY, &g_notifyIconData);

    // 显示气球提示
    g_notifyIconData.uFlags |= NIF_INFO;
    wcscpy_s(g_notifyIconData.szInfoTitle, title.c_str());
    wcscpy_s(g_notifyIconData.szInfo, message.c_str());
    g_notifyIconData.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIcon(NIM_MODIFY, &g_notifyIconData);

    // 恢复原始提示
    std::this_thread::sleep_for(std::chrono::seconds(3));
    wcscpy_s(g_notifyIconData.szTip, L"全屏应用监控器");
    g_notifyIconData.uFlags &= ~NIF_INFO;
    Shell_NotifyIcon(NIM_MODIFY, &g_notifyIconData);
}

// 显示错误
void ShowError(const std::wstring& message) {
    MessageBox(g_hMainWindow, message.c_str(), L"错误", MB_ICONERROR);
}

// 添加到黑名单
void AddToBlacklist(const std::wstring& processName) {
    g_blacklist.push_back(processName);
}

// 检查是否在黑名单中
bool IsInBlacklist(const std::wstring& processName) {
    return std::find(g_blacklist.begin(), g_blacklist.end(), processName) != g_blacklist.end();
}

// 从黑名单中移除
void RemoveFromBlacklist(const std::wstring& processName) {
    auto it = std::find(g_blacklist.begin(), g_blacklist.end(), processName);
    if (it != g_blacklist.end()) {
        g_blacklist.erase(it);
    }
}

// 保存黑名单
void SaveBlacklist() {
    try {
        WCHAR szAppDataPath[MAX_PATH];
        if (SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, szAppDataPath) == S_OK) {
            std::wstring configPath = szAppDataPath;
            configPath += L"\\FullscreenMonitor\\blacklist.txt";

            // 创建目录
            CreateDirectory((configPath.substr(0, configPath.find_last_of(L'\\'))).c_str(), NULL);

            // 写入文件
            FILE* file = NULL;
            if (_wfopen_s(&file, configPath.c_str(), L"w") == 0 && file != NULL) {
                std::lock_guard<std::mutex> lock(g_blacklistMutex);
                for (const auto& processName : g_blacklist) {
                    fwprintf(file, L"%s\n", processName.c_str());
                }
                fclose(file);
            }
        }
    }
    catch (...) {
        // 忽略保存失败
    }
}

// 加载黑名单
void LoadBlacklist() {
    try {
        WCHAR szAppDataPath[MAX_PATH];
        if (SHGetFolderPath(NULL, CSIDL_APPDATA, NULL, 0, szAppDataPath) == S_OK) {
            std::wstring configPath = szAppDataPath;
            configPath += L"\\FullscreenMonitor\\blacklist.txt";

            FILE* file = NULL;
            if (_wfopen_s(&file, configPath.c_str(), L"r") == 0 && file != NULL) {
                WCHAR szLine[MAX_PATH];
                while (fgetws(szLine, MAX_PATH, file) != NULL) {
                    // 移除换行符
                    std::wstring line = szLine;
                    size_t pos = line.find_last_not_of(L"\r\n");
                    if (pos != std::wstring::npos) {
                        line = line.substr(0, pos + 1);
                        if (!line.empty()) {
                            AddToBlacklist(line);
                        }
                    }
                }
                fclose(file);
            }
        }
    }
    catch (...) {
        // 忽略加载失败
    }
}

// 控件ID定义
#define IDC_STATUS_STATIC    1001
#define IDC_BLACKLIST_LIST   1002
#define IDC_START_BUTTON     1003
#define IDC_STOP_BUTTON      1004
#define IDC_ADD_BUTTON       1005
#define IDC_REMOVE_BUTTON    1006
#define WM_TRAYICON          WM_USER + 1

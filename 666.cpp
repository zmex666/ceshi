// FullScreenMonitor.cpp - 全屏程序监控器
// 编译: g++ -o FullScreenMonitor.exe FullScreenMonitor.cpp -luser32 -lpsapi -lgdi32

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <ctime>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

// 配置参数
const int CHECK_INTERVAL = 3000;  // 检查间隔(毫秒)
const bool LOG_TO_FILE = true;    // 是否记录日志到文件
const bool SHOW_NOTIFICATIONS = true;  // 是否显示通知

// 白名单进程（不关闭这些进程）
const char* WHITELIST[] = {
    "explorer.exe",
    "System",
    "ApplicationFrameHost.exe",
    "SearchUI.exe",
    "StartMenuExperienceHost.exe",
    "ShellExperienceHost.exe",
    ""
};

// 全局变量
HHOOK g_hKeyboardHook = NULL;
bool g_running = true;
std::string g_logFile = "FullScreenMonitor.log";

// 日志函数
void LogMessage(const std::string& message) {
    if (!LOG_TO_FILE) return;
    
    FILE* f = fopen(g_logFile.c_str(), "a");
    if (f) {
        time_t now = time(0);
        char* dt = ctime(&now);
        dt[24] = 0;  // 移除换行符
        fprintf(f, "[%s] %s\n", dt, message.c_str());
        fclose(f);
    }
}

// 显示通知
void ShowNotification(const std::string& title, const std::string& message) {
    if (!SHOW_NOTIFICATIONS) return;
    
    MessageBox(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

// 检查进程是否在白名单中
bool IsWhitelisted(const std::string& processName) {
    for (int i = 0; !WHITELIST[i].empty(); i++) {
        if (_stricmp(processName.c_str(), WHITELIST[i]) == 0) {
            return true;
        }
    }
    return false;
}

// 获取进程名
std::string GetProcessName(DWORD processId) {
    std::string name;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
    if (hProcess) {
        char buf[MAX_PATH];
        if (GetModuleBaseNameA(hProcess, NULL, buf, MAX_PATH)) {
            name = buf;
        }
        CloseHandle(hProcess);
    }
    return name;
}

// 获取窗口进程名
std::string GetWindowProcessName(HWND hwnd) {
    DWORD pid;
    GetWindowThreadProcessId(hwnd, &pid);
    return GetProcessName(pid);
}

// 检查窗口是否为全屏
bool IsWindowFullScreen(HWND hwnd) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    
    // 检查窗口样式
    LONG style = GetWindowLong(hwnd, GWL_STYLE);
    if (style & WS_CHILD) return false;
    
    // 获取窗口矩形
    RECT windowRect;
    GetWindowRect(hwnd, &windowRect);
    
    // 获取窗口所在的显示器信息
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo;
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfo(hMonitor, &monitorInfo);
    
    // 检查窗口是否覆盖整个工作区
    return (windowRect.left <= monitorInfo.rcMonitor.left &&
            windowRect.top <= monitorInfo.rcMonitor.top &&
            windowRect.right >= monitorInfo.rcMonitor.right &&
            windowRect.bottom >= monitorInfo.rcMonitor.bottom);
}

// 检查Win键是否被阻塞
bool IsWinKeyBlocked() {
    // 尝试注册Win键热键
    static bool checked = false;
    static bool blocked = false;
    
    if (!checked) {
        if (!RegisterHotKey(NULL, 9999, MOD_WIN, VK_SPACE)) {
            blocked = true;
        } else {
            UnregisterHotKey(NULL, 9999);
            blocked = false;
        }
        checked = true;
    }
    
    return blocked;
}

// 键盘钩子回调
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* p = (KBDLLHOOKSTRUCT*)lParam;
        
        // 检测Win键按下
        if (p->vkCode == VK_LWIN || p->vkCode == VK_RWIN) {
            if (IsWinKeyBlocked()) {
                // 获取前台窗口
                HWND fgWindow = GetForegroundWindow();
                if (fgWindow) {
                    std::string procName = GetWindowProcessName(fgWindow);
                    if (!IsWhitelisted(procName)) {
                        char msg[256];
                        sprintf(msg, "检测到程序 %s 阻塞Win键，正在关闭...", procName.c_str());
                        LogMessage(msg);
                        
                        // 尝试正常关闭
                        PostMessage(fgWindow, WM_CLOSE, 0, 0);
                        Sleep(1000);
                        
                        // 如果窗口还在，强制终止
                        if (IsWindow(fgWindow)) {
                            DWORD pid;
                            GetWindowThreadProcessId(fgWindow, &pid);
                            HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                            if (hProc) {
                                TerminateProcess(hProc, 0);
                                CloseHandle(hProc);
                                LogMessage("已强制终止进程");
                            }
                        }
                    }
                }
                return 1;  // 阻止Win键传递
            }
        }
    }
    return CallNextHookEx(g_hKeyboardHook, nCode, wParam, lParam);
}

// 枚举窗口回调
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    std::vector<HWND>* pWindows = (std::vector<HWND>*)lParam;
    
    if (IsWindowFullScreen(hwnd)) {
        pWindows->push_back(hwnd);
    }
    return TRUE;
}

// 查找全屏窗口
std::vector<HWND> FindFullScreenWindows() {
    std::vector<HWND> windows;
    EnumWindows(EnumWindowsProc, (LPARAM)&windows);
    return windows;
}

// 关闭全屏窗口
void CloseFullScreenWindow(HWND hwnd) {
    std::string procName = GetWindowProcessName(hwnd);
    
    if (IsWhitelisted(procName)) {
        return;
    }
    
    char logMsg[256];
    sprintf(logMsg, "发现全屏程序: %s", procName.c_str());
    LogMessage(logMsg);
    
    // 显示警告
    if (SHOW_NOTIFICATIONS) {
        char msg[512];
        sprintf(msg, "检测到全屏程序: %s\n该程序可能会影响系统操作，将尝试关闭。", procName.c_str());
        ShowNotification("全屏程序警告", msg);
    }
    
    // 先尝试正常关闭
    PostMessage(hwnd, WM_CLOSE, 0, 0);
    Sleep(2000);
    
    // 如果窗口还在，强制终止
    if (IsWindow(hwnd)) {
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (hProc) {
            TerminateProcess(hProc, 0);
            CloseHandle(hProc);
            LogMessage("已强制终止全屏程序");
        }
    }
}

// 创建系统托盘图标
void CreateTrayIcon(HINSTANCE hInstance) {
    NOTIFYICONDATA nid = {0};
    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = GetConsoleWindow();
    nid.uID = 100;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    nid.uCallbackMessage = WM_USER + 1;
    nid.hIcon = LoadIcon(NULL, IDI_WARNING);
    strcpy(nid.szTip, "全屏程序监控器 - 正在运行");
    strcpy(nid.szInfoTitle, "全屏程序监控器");
    strcpy(nid.szInfo, "监控器已启动，正在监控全屏程序...");
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout = 3000;
    
    Shell_NotifyIcon(NIM_ADD, &nid);
}

// 主监控循环
void MonitorLoop() {
    LogMessage("全屏程序监控器启动");
    
    // 安装键盘钩子
    g_hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    
    // 主循环
    while (g_running) {
        // 检查全屏窗口
        std::vector<HWND> fullScreenWindows = FindFullScreenWindows();
        
        for (HWND hwnd : fullScreenWindows) {
            CloseFullScreenWindow(hwnd);
        }
        
        // 检查Win键是否被阻塞
        if (IsWinKeyBlocked()) {
            HWND fgWindow = GetForegroundWindow();
            if (fgWindow) {
                std::string procName = GetWindowProcessName(fgWindow);
                if (!IsWhitelisted(procName)) {
                    char msg[256];
                    sprintf(msg, "检测到程序 %s 阻塞Win键", procName.c_str());
                    LogMessage(msg);
                    CloseFullScreenWindow(fgWindow);
                }
            }
        }
        
        // 等待下次检查
        Sleep(CHECK_INTERVAL);
    }
    
    // 清理
    if (g_hKeyboardHook) {
        UnhookWindowsHookEx(g_hKeyboardHook);
    }
    
    LogMessage("全屏程序监控器停止");
}

// 控制台控制处理
BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

// 主函数
int main() {
    printf("全屏程序监控器 v1.0\n");
    printf("==========================\n");
    printf("功能:\n");
    printf("1. 自动检测并关闭全屏程序\n");
    printf("2. 检测并关闭阻塞Win键的程序\n");
    printf("3. 系统白名单保护\n");
    printf("4. 日志记录到文件\n");
    printf("\n按Ctrl+C退出程序\n");
    printf("==========================\n\n");
    
    // 设置控制台控制处理器
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    
    // 尝试提升权限
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
        CloseHandle(hToken);
    }
    
    // 隐藏控制台窗口（可选）
    // ShowWindow(GetConsoleWindow(), SW_HIDE);
    
    // 开始监控
    MonitorLoop();
    
    return 0;
}

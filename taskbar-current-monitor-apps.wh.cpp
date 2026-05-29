// ==WindhawkMod==
// @id taskbar-current-monitor-apps
// @name Taskbar Current Monitor Apps
// @description Show running app buttons only on their monitor, with an experimental option to expose pinned taskbar items on secondary taskbars.
// @version 1.4.3
// @author SUlTlUS + ChatGPT
// @github https://github.com/SUlTlUS/Taskbar-Current-Monitor-Apps
// @include explorer.exe
// @architecture x86-64
// @compilerOptions -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -ladvapi32 -luser32 -lversion
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Current Monitor Apps

这个 Windhawk 插件用于 Windows 多显示器任务栏：

- 运行中的窗口按钮只显示在窗口所在显示器的任务栏上。
- `showPinnedOnAllTaskbars` 会尝试让副屏任务栏也按主任务栏方式处理固定项。
- 默认不持久写入注册表，关闭 mod 后会让 Explorer 重新读取真实设置。

## 重要设置

如果启用 `showPinnedOnAllTaskbars`，v1.4.3 会自动使用安全组合：

```text
keepEnforced = true
writeRegistry = false
```

这样不会再把 `MMTaskbarMode=2` 永久写死到注册表。

## 固定项开关说明

`showPinnedOnAllTaskbars` 不是注册表开关。Windows 的 `MMTaskbarMode` 主要控制运行窗口按钮，并不能可靠控制固定项是否显示在副屏任务栏。

所以从 v1.4 开始，这个开关会额外 hook `CTaskListWnd::IsOnPrimaryTaskband`，让副屏任务栏在 Explorer 内部尽量被当作“主任务栏”处理。这样更有可能显示固定项，同时仍保持 `MMTaskbarMode = 2` 来让运行窗口按钮只显示在所在屏幕。

这是实验功能，可能随 Windows 版本变化而失效。启用或修改此开关后，请重启 `explorer.exe`，因为副屏任务栏的固定项列表通常不会在运行中完整重建。

## 调试

DbgViewMini 里搜索：

```text
[TCMA]
```

v1.4.3 会额外记录以下内部函数是否被调用：

- `CTaskListWnd::IsOnPrimaryTaskband`
- `CTaskListWnd::HandleTaskGroupPinned`
- `CTaskListWnd::_CreateTBGroup`
- `CTaskListWnd::_TaskCreated`

如果 `IsOnPrimaryTaskband` 成功 hook 但完全不被调用，说明当前 Windows 版本的副屏固定项不是由这条路径控制，需要继续 hook 更深的任务栏模型或 XAML 数据源。

## 如果旧版本已经把任务栏改乱了

先关闭这个 mod，然后运行仓库里的脚本：

- `scripts/restore-all-taskbars.cmd`：恢复为所有任务栏显示所有运行窗口按钮。
- `scripts/restore-main-taskbar-only.cmd`：恢复为只显示主任务栏。
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- showPinnedOnAllTaskbars: false
  $name: Show pinned items on all taskbars
  $description: Experimental. Makes secondary taskbars report themselves as primary taskbars when Explorer checks CTaskListWnd::IsOnPrimaryTaskband. Restart explorer.exe after changing this setting.
- keepEnforced: true
  $name: Keep taskbar mode enforced
  $description: Keep returning the selected taskbar mode when Explorer reads the taskbar registry values. Forced on when Show pinned items on all taskbars is enabled.
- writeRegistry: false
  $name: Also write registry values
  $description: Optional legacy mode. Forced off when Show pinned items on all taskbars is enabled, to avoid permanently writing MMTaskbarMode=2.
- restoreOnUnload: true
  $name: Restore registry values on unload if registry was written
  $description: Only used when writeRegistry is enabled. Restores the registry values captured when the mod was loaded.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>

static void DebugLog(PCWSTR format, ...) {
    WCHAR buffer[1024];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, ARRAYSIZE(buffer), _TRUNCATE, format, args);
    va_end(args);

    WCHAR output[1200];
    _snwprintf_s(output, ARRAYSIZE(output), _TRUNCATE, L"[TCMA] %s\r\n", buffer);
    OutputDebugStringW(output);
    Wh_Log(L"%s", output);
}

static constexpr const wchar_t* kExplorerAdvancedKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

static constexpr const wchar_t* kValueTaskbarEnabled = L"MMTaskbarEnabled";
static constexpr const wchar_t* kValueTaskbarMode = L"MMTaskbarMode";

static constexpr DWORD kShowTaskbarOnAllDisplays = 1;
static constexpr DWORD kShowButtonsOnlyWhereWindowIsOpen = 2;

struct Settings {
    bool showPinnedOnAllTaskbars = false;
    bool keepEnforced = true;
    bool writeRegistry = false;
    bool restoreOnUnload = true;
};

struct BackupValue {
    bool captured = false;
    bool existed = false;
    DWORD value = 0;
};

static Settings g_settings;
static BackupValue g_backupEnabled;
static BackupValue g_backupMode;
static bool g_registryWasWritten = false;
static bool g_taskbarSymbolsHooked = false;

using RegGetValueW_t = LSTATUS(WINAPI*)(
    HKEY hkey,
    LPCWSTR lpSubKey,
    LPCWSTR lpValue,
    DWORD dwFlags,
    LPDWORD pdwType,
    PVOID pvData,
    LPDWORD pcbData);

using RegQueryValueExW_t = LSTATUS(WINAPI*)(
    HKEY hKey,
    LPCWSTR lpValueName,
    LPDWORD lpReserved,
    LPDWORD lpType,
    LPBYTE lpData,
    LPDWORD lpcbData);

using CTaskListWnd_IsOnPrimaryTaskband_t = int(WINAPI*)(PVOID pThis);
using CTaskListWnd_HandleTaskGroupPinned_t = void(WINAPI*)(PVOID pThis, PVOID taskGroup);
using CTaskListWnd__CreateTBGroup_t = PVOID(WINAPI*)(PVOID pThis, PVOID taskGroup, int index);
using CTaskListWnd__TaskCreated_t = LONG_PTR(WINAPI*)(PVOID pThis, PVOID taskGroup, PVOID taskItem, int param3);

static RegGetValueW_t RegGetValueW_Original;
static RegQueryValueExW_t RegQueryValueExW_Original;
static CTaskListWnd_IsOnPrimaryTaskband_t CTaskListWnd_IsOnPrimaryTaskband_Original;
static CTaskListWnd_HandleTaskGroupPinned_t CTaskListWnd_HandleTaskGroupPinned_Original;
static CTaskListWnd__CreateTBGroup_t CTaskListWnd__CreateTBGroup_Original;
static CTaskListWnd__TaskCreated_t CTaskListWnd__TaskCreated_Original;

static bool WideEquals(PCWSTR a, PCWSTR b) {
    return a && b && _wcsicmp(a, b) == 0;
}

static bool IsForcedValueName(PCWSTR valueName, DWORD* forcedValue) {
    if (WideEquals(valueName, kValueTaskbarEnabled)) {
        *forcedValue = kShowTaskbarOnAllDisplays;
        return true;
    }

    if (WideEquals(valueName, kValueTaskbarMode)) {
        *forcedValue = kShowButtonsOnlyWhereWindowIsOpen;
        return true;
    }

    return false;
}

static int WINAPI CTaskListWnd_IsOnPrimaryTaskband_Hook(PVOID pThis) {
    static int logCount = 0;

    if (g_settings.showPinnedOnAllTaskbars) {
        if (logCount < 100) {
            DebugLog(L"IsOnPrimaryTaskband hook called, forced TRUE, this=%p", pThis);
            logCount++;
        }
        return TRUE;
    }

    if (logCount < 10) {
        DebugLog(L"IsOnPrimaryTaskband hook called, passthrough, this=%p", pThis);
        logCount++;
    }

    return CTaskListWnd_IsOnPrimaryTaskband_Original(pThis);
}

static void WINAPI CTaskListWnd_HandleTaskGroupPinned_Hook(PVOID pThis, PVOID taskGroup) {
    static int logCount = 0;
    if (logCount < 100) {
        DebugLog(L"HandleTaskGroupPinned called, this=%p, taskGroup=%p", pThis, taskGroup);
        logCount++;
    }

    CTaskListWnd_HandleTaskGroupPinned_Original(pThis, taskGroup);
}

static PVOID WINAPI CTaskListWnd__CreateTBGroup_Hook(PVOID pThis, PVOID taskGroup, int index) {
    static int logCount = 0;
    if (logCount < 120) {
        DebugLog(L"_CreateTBGroup called, this=%p, taskGroup=%p, index=%d", pThis, taskGroup, index);
        logCount++;
    }

    return CTaskListWnd__CreateTBGroup_Original(pThis, taskGroup, index);
}

static LONG_PTR WINAPI CTaskListWnd__TaskCreated_Hook(PVOID pThis, PVOID taskGroup, PVOID taskItem, int param3) {
    static int logCount = 0;
    if (logCount < 120) {
        DebugLog(L"_TaskCreated called, this=%p, taskGroup=%p, taskItem=%p, param3=%d", pThis, taskGroup, taskItem, param3);
        logCount++;
    }

    return CTaskListWnd__TaskCreated_Original(pThis, taskGroup, taskItem, param3);
}

static HMODULE GetTaskbarModuleForSymbols() {
    HMODULE module = GetModuleHandleW(L"taskbar.dll");
    if (module) {
        DebugLog(L"taskbar.dll already loaded: %p", module);
        return module;
    }

    module = LoadLibraryExW(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module) {
        DebugLog(L"taskbar.dll loaded by mod: %p", module);
        return module;
    }

    HMODULE explorerModule = GetModuleHandleW(nullptr);
    DebugLog(L"taskbar.dll not found, falling back to explorer.exe module: %p", explorerModule);
    return explorerModule;
}

static bool HookTaskbarSymbols() {
    HMODULE module = GetTaskbarModuleForSymbols();
    if (!module) {
        DebugLog(L"Couldn't find taskbar module, pinned-item hook unavailable");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: virtual int __cdecl CTaskListWnd::IsOnPrimaryTaskband(void))"},
            &CTaskListWnd_IsOnPrimaryTaskband_Original,
            CTaskListWnd_IsOnPrimaryTaskband_Hook,
        },
        {
            {LR"(public: virtual void __cdecl CTaskListWnd::HandleTaskGroupPinned(struct ITaskGroup *))"},
            &CTaskListWnd_HandleTaskGroupPinned_Original,
            CTaskListWnd_HandleTaskGroupPinned_Hook,
            true,
        },
        {
            {LR"(protected: struct ITaskBtnGroup * __cdecl CTaskListWnd::_CreateTBGroup(struct ITaskGroup *,int))"},
            &CTaskListWnd__CreateTBGroup_Original,
            CTaskListWnd__CreateTBGroup_Hook,
            true,
        },
        {
            {LR"(protected: long __cdecl CTaskListWnd::_TaskCreated(struct ITaskGroup *,struct ITaskItem *,int))"},
            &CTaskListWnd__TaskCreated_Original,
            CTaskListWnd__TaskCreated_Hook,
            true,
        },
    };

    if (!HookSymbols(module, symbolHooks, ARRAYSIZE(symbolHooks))) {
        DebugLog(L"HookSymbols for taskbar internals failed");
        return false;
    }

    g_taskbarSymbolsHooked = true;
    DebugLog(L"Hooked taskbar internals for pinned item diagnostics");
    return true;
}

static LSTATUS ReturnForcedDword(
    DWORD forcedValue,
    DWORD requestedFlags,
    LPDWORD typeOut,
    PVOID dataOut,
    LPDWORD dataSizeInOut) {
    constexpr DWORD RRF_RT_ANY_KNOWN = 0x0000ffff;
    constexpr DWORD RRF_RT_REG_DWORD_CONST = 0x00000010;

    DWORD typeFilter = requestedFlags & RRF_RT_ANY_KNOWN;
    if (typeFilter && !(typeFilter & RRF_RT_REG_DWORD_CONST)) {
        return ERROR_FILE_NOT_FOUND;
    }

    if (typeOut) {
        *typeOut = REG_DWORD;
    }

    if (dataSizeInOut) {
        if (!dataOut) {
            *dataSizeInOut = sizeof(DWORD);
            return ERROR_SUCCESS;
        }

        if (*dataSizeInOut < sizeof(DWORD)) {
            *dataSizeInOut = sizeof(DWORD);
            return ERROR_MORE_DATA;
        }

        *dataSizeInOut = sizeof(DWORD);
    }

    if (dataOut) {
        *reinterpret_cast<DWORD*>(dataOut) = forcedValue;
    }

    return ERROR_SUCCESS;
}

static LSTATUS WINAPI RegGetValueW_Hook(
    HKEY hkey,
    LPCWSTR lpSubKey,
    LPCWSTR lpValue,
    DWORD dwFlags,
    LPDWORD pdwType,
    PVOID pvData,
    LPDWORD pcbData) {
    if (g_settings.keepEnforced) {
        DWORD forcedValue = 0;
        if (IsForcedValueName(lpValue, &forcedValue)) {
            return ReturnForcedDword(forcedValue, dwFlags, pdwType, pvData, pcbData);
        }
    }

    return RegGetValueW_Original(hkey, lpSubKey, lpValue, dwFlags, pdwType, pvData, pcbData);
}

static LSTATUS WINAPI RegQueryValueExW_Hook(
    HKEY hKey,
    LPCWSTR lpValueName,
    LPDWORD lpReserved,
    LPDWORD lpType,
    LPBYTE lpData,
    LPDWORD lpcbData) {
    if (g_settings.keepEnforced) {
        DWORD forcedValue = 0;
        if (IsForcedValueName(lpValueName, &forcedValue)) {
            return ReturnForcedDword(forcedValue, 0, lpType, lpData, lpcbData);
        }
    }

    return RegQueryValueExW_Original(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
}

static bool ReadDwordFromAdvancedKey(PCWSTR valueName, BackupValue* backup) {
    backup->captured = true;

    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerAdvancedKey, 0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) {
        backup->existed = false;
        return false;
    }

    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);

    status = RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);

    if (status == ERROR_SUCCESS && type == REG_DWORD && size == sizeof(DWORD)) {
        backup->existed = true;
        backup->value = value;
        return true;
    }

    backup->existed = false;
    return false;
}

static bool WriteDwordToAdvancedKey(PCWSTR valueName, DWORD value) {
    HKEY key = nullptr;
    DWORD disposition = 0;

    LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, kExplorerAdvancedKey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, &disposition);
    if (status != ERROR_SUCCESS) {
        DebugLog(L"RegCreateKeyExW failed: %ld", status);
        return false;
    }

    status = RegSetValueExW(key, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);

    if (status != ERROR_SUCCESS) {
        DebugLog(L"RegSetValueExW(%s) failed: %ld", valueName, status);
        return false;
    }

    return true;
}

static void DeleteAdvancedValue(PCWSTR valueName) {
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerAdvancedKey, 0, KEY_SET_VALUE, &key);
    if (status == ERROR_SUCCESS) {
        RegDeleteValueW(key, valueName);
        RegCloseKey(key);
    }
}

static void NotifyExplorerSettingsChanged() {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"), SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &result);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"), SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &result);
}

static void ApplyTaskbarMode() {
    if (g_settings.writeRegistry) {
        bool okEnabled = WriteDwordToAdvancedKey(kValueTaskbarEnabled, kShowTaskbarOnAllDisplays);
        bool okMode = WriteDwordToAdvancedKey(kValueTaskbarMode, kShowButtonsOnlyWhereWindowIsOpen);
        g_registryWasWritten = g_registryWasWritten || okEnabled || okMode;

        DebugLog(
            L"ApplyTaskbarMode: MMTaskbarEnabled=%u (%s), MMTaskbarMode=%u (%s), showPinnedOnAllTaskbars=%d, taskbarSymbolsHooked=%d",
            kShowTaskbarOnAllDisplays,
            okEnabled ? L"ok" : L"failed",
            kShowButtonsOnlyWhereWindowIsOpen,
            okMode ? L"ok" : L"failed",
            g_settings.showPinnedOnAllTaskbars,
            g_taskbarSymbolsHooked);
    } else {
        DebugLog(
            L"ApplyTaskbarMode: registry write disabled, hook-only mode, MMTaskbarMode=%u, showPinnedOnAllTaskbars=%d, taskbarSymbolsHooked=%d",
            kShowButtonsOnlyWhereWindowIsOpen,
            g_settings.showPinnedOnAllTaskbars,
            g_taskbarSymbolsHooked);
    }

    NotifyExplorerSettingsChanged();
}

static void RestoreBackupValue(PCWSTR valueName, const BackupValue& backup) {
    if (!backup.captured) {
        return;
    }

    if (backup.existed) {
        WriteDwordToAdvancedKey(valueName, backup.value);
    } else {
        DeleteAdvancedValue(valueName);
    }
}

static void LoadSettings() {
    g_settings.showPinnedOnAllTaskbars = Wh_GetIntSetting(L"showPinnedOnAllTaskbars") != 0;
    g_settings.keepEnforced = Wh_GetIntSetting(L"keepEnforced") != 0;
    g_settings.writeRegistry = Wh_GetIntSetting(L"writeRegistry") != 0;
    g_settings.restoreOnUnload = Wh_GetIntSetting(L"restoreOnUnload") != 0;

    if (g_settings.showPinnedOnAllTaskbars) {
        if (!g_settings.keepEnforced || g_settings.writeRegistry) {
            DebugLog(L"showPinnedOnAllTaskbars enabled: forcing keepEnforced=1 and writeRegistry=0 for safe experimental mode");
        }
        g_settings.keepEnforced = true;
        g_settings.writeRegistry = false;
    }

    DebugLog(
        L"Settings: showPinnedOnAllTaskbars=%d, keepEnforced=%d, writeRegistry=%d, restoreOnUnload=%d",
        g_settings.showPinnedOnAllTaskbars,
        g_settings.keepEnforced,
        g_settings.writeRegistry,
        g_settings.restoreOnUnload);
}

BOOL Wh_ModInit() {
    DebugLog(L"Wh_ModInit: Taskbar Current Monitor Apps init, pid=%lu", GetCurrentProcessId());

    LoadSettings();

    ReadDwordFromAdvancedKey(kValueTaskbarEnabled, &g_backupEnabled);
    ReadDwordFromAdvancedKey(kValueTaskbarMode, &g_backupMode);

    DebugLog(
        L"Backup: MMTaskbarEnabled existed=%d value=%u, MMTaskbarMode existed=%d value=%u",
        g_backupEnabled.existed,
        g_backupEnabled.value,
        g_backupMode.existed,
        g_backupMode.value);

    HookTaskbarSymbols();

    HMODULE advapi32 = GetModuleHandleW(L"advapi32.dll");
    if (!advapi32) {
        advapi32 = LoadLibraryW(L"advapi32.dll");
    }

    if (!advapi32) {
        DebugLog(L"Couldn't load advapi32.dll");
        return FALSE;
    }

    void* regGetValue = reinterpret_cast<void*>(GetProcAddress(advapi32, "RegGetValueW"));
    void* regQueryValueEx = reinterpret_cast<void*>(GetProcAddress(advapi32, "RegQueryValueExW"));

    if (!regGetValue || !regQueryValueEx) {
        DebugLog(L"Couldn't find registry APIs");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(regGetValue, reinterpret_cast<void*>(RegGetValueW_Hook), reinterpret_cast<void**>(&RegGetValueW_Original))) {
        DebugLog(L"Wh_SetFunctionHook(RegGetValueW) failed");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(regQueryValueEx, reinterpret_cast<void*>(RegQueryValueExW_Hook), reinterpret_cast<void**>(&RegQueryValueExW_Original))) {
        DebugLog(L"Wh_SetFunctionHook(RegQueryValueExW) failed");
        return FALSE;
    }

    ApplyTaskbarMode();
    return TRUE;
}

void Wh_ModAfterInit() {
    DebugLog(L"Wh_ModAfterInit");
    ApplyTaskbarMode();
}

void Wh_ModSettingsChanged() {
    DebugLog(L"Wh_ModSettingsChanged");
    LoadSettings();
    ApplyTaskbarMode();
}

void Wh_ModUninit() {
    DebugLog(L"Wh_ModUninit");

    g_settings.keepEnforced = false;
    g_settings.showPinnedOnAllTaskbars = false;

    if (g_settings.restoreOnUnload && g_registryWasWritten) {
        RestoreBackupValue(kValueTaskbarEnabled, g_backupEnabled);
        RestoreBackupValue(kValueTaskbarMode, g_backupMode);
    }

    NotifyExplorerSettingsChanged();
}

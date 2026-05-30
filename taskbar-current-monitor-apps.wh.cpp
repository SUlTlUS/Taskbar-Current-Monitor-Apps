// ==WindhawkMod==
// @id taskbar-current-monitor-apps
// @name Taskbar Current Monitor Apps
// @description Make running taskbar buttons appear only on the monitor where the window is open. Pinned taskbar items are not modified.
// @version 2.0
// @author SUlTlUS + ChatGPT
// @github https://github.com/SUlTlUS/Taskbar-Current-Monitor-Apps
// @include explorer.exe
// @architecture x86-64
// @compilerOptions -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -ladvapi32 -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Current Monitor Apps

稳定版目标：多显示器任务栏只过滤“运行窗口按钮”。

- 运行窗口按钮只显示在窗口所在显示器的任务栏上。
- 不处理固定项，不复制固定项，不隐藏固定项，不修改固定项顺序。
- 默认只在 Explorer 读取设置时临时返回目标值，不持久写入注册表。
- 关闭 / 卸载 mod 后通知 Explorer 重新读取真实设置。

## 固定项说明

Windows 11 新任务栏的固定项渲染不只受 `MMTaskbarMode` 控制。之前的调试日志显示：旧任务栏层里副屏已经存在固定项任务组，但 Windows 11 UI 层仍不渲染。因此从 v2.0 起放弃“固定项在所有任务栏显示”的实验功能。

## 实现方式

运行时强制 Explorer 看到：

```text
MMTaskbarEnabled = 1
MMTaskbarMode    = 2
```

含义：

- `MMTaskbarEnabled = 1`：在所有显示器上显示任务栏。
- `MMTaskbarMode = 2`：运行窗口按钮只显示在窗口所在显示器。

## 建议

保持默认设置即可：

```text
keepEnforced = true
writeRegistry = false
restoreOnUnload = true
```

如果之前旧版本把任务栏状态改乱，先关闭 mod，然后运行仓库里的恢复脚本：

- `scripts/restore-all-taskbars.cmd`
- `scripts/restore-main-taskbar-only.cmd`
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- keepEnforced: true
  $name: Keep taskbar mode enforced
  $description: Keep returning the per-monitor taskbar mode when Explorer reads the taskbar registry values.
- writeRegistry: false
  $name: Also write registry values
  $description: Optional. Writes Explorer taskbar settings to the registry while enabled. Keep this off if you want disabling the mod to restore automatically.
- restoreOnUnload: true
  $name: Restore registry values on unload if registry was written
  $description: Only used when writeRegistry is enabled. Restores registry values captured when the mod was loaded.
- debugOutput: false
  $name: Enable debug output
  $description: Print [TCMA] diagnostic lines to Windhawk logs and OutputDebugString.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>

static constexpr const wchar_t* kExplorerAdvancedKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
static constexpr const wchar_t* kValueTaskbarEnabled = L"MMTaskbarEnabled";
static constexpr const wchar_t* kValueTaskbarMode = L"MMTaskbarMode";

static constexpr DWORD kShowTaskbarOnAllDisplays = 1;
static constexpr DWORD kShowButtonsOnlyWhereWindowIsOpen = 2;

struct Settings {
    bool keepEnforced = true;
    bool writeRegistry = false;
    bool restoreOnUnload = true;
    bool debugOutput = false;
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

static RegGetValueW_t RegGetValueW_Original;
static RegQueryValueExW_t RegQueryValueExW_Original;

static void DebugLog(PCWSTR format, ...) {
    if (!g_settings.debugOutput) {
        return;
    }

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
            DebugLog(L"Forced RegGetValueW: %s=%u", lpValue, forcedValue);
            return ReturnForcedDword(forcedValue, dwFlags, pdwType, pvData, pcbData);
        }
    }

    return RegGetValueW_Original(
        hkey,
        lpSubKey,
        lpValue,
        dwFlags,
        pdwType,
        pvData,
        pcbData);
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
            DebugLog(L"Forced RegQueryValueExW: %s=%u", lpValueName, forcedValue);
            return ReturnForcedDword(forcedValue, 0, lpType, lpData, lpcbData);
        }
    }

    return RegQueryValueExW_Original(
        hKey,
        lpValueName,
        lpReserved,
        lpType,
        lpData,
        lpcbData);
}

static bool ReadDwordFromAdvancedKey(PCWSTR valueName, BackupValue* backup) {
    backup->captured = true;

    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kExplorerAdvancedKey,
        0,
        KEY_QUERY_VALUE,
        &key);

    if (status != ERROR_SUCCESS) {
        backup->existed = false;
        return false;
    }

    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    status = RegQueryValueExW(
        key,
        valueName,
        nullptr,
        &type,
        reinterpret_cast<LPBYTE>(&value),
        &size);

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

    LSTATUS status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        kExplorerAdvancedKey,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        &disposition);

    if (status != ERROR_SUCCESS) {
        DebugLog(L"RegCreateKeyExW failed: %ld", status);
        return false;
    }

    status = RegSetValueExW(
        key,
        valueName,
        0,
        REG_DWORD,
        reinterpret_cast<const BYTE*>(&value),
        sizeof(value));

    RegCloseKey(key);

    if (status != ERROR_SUCCESS) {
        DebugLog(L"RegSetValueExW(%s) failed: %ld", valueName, status);
        return false;
    }

    return true;
}

static void DeleteAdvancedValue(PCWSTR valueName) {
    HKEY key = nullptr;
    LSTATUS status = RegOpenKeyExW(
        HKEY_CURRENT_USER,
        kExplorerAdvancedKey,
        0,
        KEY_SET_VALUE,
        &key);

    if (status == ERROR_SUCCESS) {
        RegDeleteValueW(key, valueName);
        RegCloseKey(key);
    }
}

static void NotifyExplorerSettingsChanged() {
    DWORD_PTR result = 0;

    SendMessageTimeoutW(
        HWND_BROADCAST,
        WM_SETTINGCHANGE,
        0,
        reinterpret_cast<LPARAM>(L"TraySettings"),
        SMTO_ABORTIFHUNG | SMTO_NORMAL,
        2000,
        &result);

    SendMessageTimeoutW(
        HWND_BROADCAST,
        WM_SETTINGCHANGE,
        0,
        reinterpret_cast<LPARAM>(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"),
        SMTO_ABORTIFHUNG | SMTO_NORMAL,
        2000,
        &result);
}

static BOOL CALLBACK RefreshTaskbarEnumProc(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }

    WCHAR className[64]{};
    if (!GetClassNameW(hwnd, className, ARRAYSIZE(className))) {
        return TRUE;
    }

    if (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
        DebugLog(L"RefreshTaskbar: %s hwnd=%p", className, hwnd);
        SendMessageW(hwnd, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"));
        PostMessageW(hwnd, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"));
    }

    return TRUE;
}

static void RefreshTaskbarWindows() {
    EnumWindows(RefreshTaskbarEnumProc, 0);
    Sleep(250);
    EnumWindows(RefreshTaskbarEnumProc, 0);
}

static void ApplyTaskbarMode() {
    if (g_settings.writeRegistry) {
        bool okEnabled = WriteDwordToAdvancedKey(
            kValueTaskbarEnabled,
            kShowTaskbarOnAllDisplays);
        bool okMode = WriteDwordToAdvancedKey(
            kValueTaskbarMode,
            kShowButtonsOnlyWhereWindowIsOpen);

        g_registryWasWritten = g_registryWasWritten || okEnabled || okMode;

        DebugLog(
            L"ApplyTaskbarMode registry: enabled=%s mode=%s",
            okEnabled ? L"ok" : L"fail",
            okMode ? L"ok" : L"fail");
    } else {
        DebugLog(L"ApplyTaskbarMode hook-only: mode=2");
    }

    NotifyExplorerSettingsChanged();
    RefreshTaskbarWindows();
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
    g_settings.keepEnforced = Wh_GetIntSetting(L"keepEnforced") != 0;
    g_settings.writeRegistry = Wh_GetIntSetting(L"writeRegistry") != 0;
    g_settings.restoreOnUnload = Wh_GetIntSetting(L"restoreOnUnload") != 0;
    g_settings.debugOutput = Wh_GetIntSetting(L"debugOutput") != 0;

    DebugLog(
        L"Settings: keep=%d write=%d restore=%d debug=%d",
        g_settings.keepEnforced,
        g_settings.writeRegistry,
        g_settings.restoreOnUnload,
        g_settings.debugOutput);
}

BOOL Wh_ModInit() {
    LoadSettings();
    DebugLog(L"Wh_ModInit pid=%lu", GetCurrentProcessId());

    ReadDwordFromAdvancedKey(kValueTaskbarEnabled, &g_backupEnabled);
    ReadDwordFromAdvancedKey(kValueTaskbarMode, &g_backupMode);

    HMODULE advapi32 = GetModuleHandleW(L"advapi32.dll");
    if (!advapi32) {
        advapi32 = LoadLibraryW(L"advapi32.dll");
    }

    if (!advapi32) {
        DebugLog(L"No advapi32");
        return FALSE;
    }

    void* regGetValue = reinterpret_cast<void*>(GetProcAddress(advapi32, "RegGetValueW"));
    void* regQueryValueEx = reinterpret_cast<void*>(GetProcAddress(advapi32, "RegQueryValueExW"));

    if (!regGetValue || !regQueryValueEx) {
        DebugLog(L"No registry APIs");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            regGetValue,
            reinterpret_cast<void*>(RegGetValueW_Hook),
            reinterpret_cast<void**>(&RegGetValueW_Original))) {
        DebugLog(L"RegGetValueW hook failed");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            regQueryValueEx,
            reinterpret_cast<void*>(RegQueryValueExW_Hook),
            reinterpret_cast<void**>(&RegQueryValueExW_Original))) {
        DebugLog(L"RegQueryValueExW hook failed");
        return FALSE;
    }

    ApplyTaskbarMode();
    return TRUE;
}

void Wh_ModAfterInit() {
    ApplyTaskbarMode();
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    ApplyTaskbarMode();
}

void Wh_ModUninit() {
    DebugLog(L"Wh_ModUninit");

    g_settings.keepEnforced = false;

    if (g_settings.restoreOnUnload && g_registryWasWritten) {
        RestoreBackupValue(kValueTaskbarEnabled, g_backupEnabled);
        RestoreBackupValue(kValueTaskbarMode, g_backupMode);
    }

    NotifyExplorerSettingsChanged();
    RefreshTaskbarWindows();
}

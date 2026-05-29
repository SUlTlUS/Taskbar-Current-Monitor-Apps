// ==WindhawkMod==
// @id taskbar-current-monitor-apps
// @name Taskbar Current Monitor Apps
// @description Keep pinned taskbar items visible on every taskbar, while running app buttons are shown only on the monitor that owns the window.
// @version 1.2
// @author SUlTlUS + ChatGPT
// @github https://github.com/SUlTlUS/Taskbar-Current-Monitor-Apps
// @include explorer.exe
// @architecture x86-64
// @compilerOptions -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -ladvapi32 -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Current Monitor Apps

这个 Windhawk 插件用于 Windows 多显示器任务栏：

- 不过滤任务栏固定项目，固定项目继续由 Explorer 原生逻辑显示在所有任务栏上。
- 运行中的窗口按钮只显示在窗口所在显示器的任务栏上。
- 默认不持久写入注册表，关闭 mod 后会让 Explorer 重新读取真实设置。

## 实现方式

插件不会枚举并删除任务栏按钮，也不会硬改固定项目列表。它只在 Explorer 读取多显示器任务栏设置时临时返回：

- `MMTaskbarEnabled = 1`
- `MMTaskbarMode = 2`

这样可以避免误伤固定项目，也避免关闭 mod 后任务栏状态回不去。

## 如果旧版本已经把任务栏改乱了

先关闭这个 mod，然后运行仓库里的脚本：

- `scripts/restore-all-taskbars.cmd`：恢复为所有任务栏显示所有运行窗口按钮。
- `scripts/restore-main-taskbar-only.cmd`：恢复为只显示主任务栏。
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- keepEnforced: true
  $name: Keep taskbar mode enforced
  $description: Keep returning the per-monitor taskbar mode when Explorer reads the taskbar registry values.
- writeRegistry: false
  $name: Also write registry values
  $description: Optional legacy mode. Writes Explorer taskbar settings to the registry while enabled. Keep this off if you want disabling the mod to restore automatically.
- restoreOnUnload: true
  $name: Restore registry values on unload if registry was written
  $description: Only used when writeRegistry is enabled. Restores the registry values captured when the mod was loaded.
*/
// ==/WindhawkModSettings==

#include <windows.h>

static constexpr const wchar_t* kExplorerAdvancedKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

static constexpr const wchar_t* kValueTaskbarEnabled = L"MMTaskbarEnabled";
static constexpr const wchar_t* kValueTaskbarMode = L"MMTaskbarMode";

// Windows multi-monitor taskbar mode:
//   MMTaskbarEnabled = 1: show taskbar on all displays.
//   MMTaskbarMode = 0: show running taskbar buttons on all taskbars.
//   MMTaskbarMode = 1: show running buttons on the main taskbar and where the window is open.
//   MMTaskbarMode = 2: show running buttons only where the window is open.
//
// Pinned taskbar items are deliberately not filtered by this mod. They are not
// enumerated or removed here; Explorer keeps rendering them with its normal
// pinned-item logic, which avoids destroying the user's pinned taskbar layout.
static constexpr DWORD kShowTaskbarOnAllDisplays = 1;
static constexpr DWORD kShowButtonsOnlyWhereWindowIsOpen = 2;

struct Settings {
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
            return ReturnForcedDword(
                forcedValue,
                dwFlags,
                pdwType,
                pvData,
                pcbData);
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
            return ReturnForcedDword(
                forcedValue,
                0,
                lpType,
                lpData,
                lpcbData);
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
        Wh_Log(L"RegCreateKeyExW failed: %ld", status);
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
        Wh_Log(L"RegSetValueExW(%s) failed: %ld", valueName, status);
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

static void ApplyTaskbarMode() {
    if (g_settings.writeRegistry) {
        bool okEnabled = WriteDwordToAdvancedKey(
            kValueTaskbarEnabled,
            kShowTaskbarOnAllDisplays);

        bool okMode = WriteDwordToAdvancedKey(
            kValueTaskbarMode,
            kShowButtonsOnlyWhereWindowIsOpen);

        g_registryWasWritten = g_registryWasWritten || okEnabled || okMode;

        Wh_Log(
            L"ApplyTaskbarMode: MMTaskbarEnabled=%u (%s), MMTaskbarMode=%u (%s)",
            kShowTaskbarOnAllDisplays,
            okEnabled ? L"ok" : L"failed",
            kShowButtonsOnlyWhereWindowIsOpen,
            okMode ? L"ok" : L"failed");
    } else {
        Wh_Log(L"ApplyTaskbarMode: registry write disabled, using hook-only mode");
    }

    // Even in hook-only mode, Explorer needs a settings notification so it reads
    // the values again while this mod's registry hook is active.
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
    g_settings.keepEnforced = Wh_GetIntSetting(L"keepEnforced") != 0;
    g_settings.writeRegistry = Wh_GetIntSetting(L"writeRegistry") != 0;
    g_settings.restoreOnUnload = Wh_GetIntSetting(L"restoreOnUnload") != 0;

    Wh_Log(
        L"Settings: keepEnforced=%d, writeRegistry=%d, restoreOnUnload=%d",
        g_settings.keepEnforced,
        g_settings.writeRegistry,
        g_settings.restoreOnUnload);
}

BOOL Wh_ModInit() {
    Wh_Log(L"Taskbar Current Monitor Apps init");

    LoadSettings();

    ReadDwordFromAdvancedKey(kValueTaskbarEnabled, &g_backupEnabled);
    ReadDwordFromAdvancedKey(kValueTaskbarMode, &g_backupMode);

    HMODULE advapi32 = GetModuleHandleW(L"advapi32.dll");
    if (!advapi32) {
        advapi32 = LoadLibraryW(L"advapi32.dll");
    }

    if (!advapi32) {
        Wh_Log(L"Couldn't load advapi32.dll");
        return FALSE;
    }

    void* regGetValue = reinterpret_cast<void*>(
        GetProcAddress(advapi32, "RegGetValueW"));
    void* regQueryValueEx = reinterpret_cast<void*>(
        GetProcAddress(advapi32, "RegQueryValueExW"));

    if (!regGetValue || !regQueryValueEx) {
        Wh_Log(L"Couldn't find registry APIs");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            regGetValue,
            reinterpret_cast<void*>(RegGetValueW_Hook),
            reinterpret_cast<void**>(&RegGetValueW_Original))) {
        Wh_Log(L"Wh_SetFunctionHook(RegGetValueW) failed");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(
            regQueryValueEx,
            reinterpret_cast<void*>(RegQueryValueExW_Hook),
            reinterpret_cast<void**>(&RegQueryValueExW_Original))) {
        Wh_Log(L"Wh_SetFunctionHook(RegQueryValueExW) failed");
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
    Wh_Log(L"Taskbar Current Monitor Apps uninit");

    // Stop returning forced values before notifying Explorer, otherwise Explorer
    // can refresh while the hook still reports the modded values.
    g_settings.keepEnforced = false;

    if (g_settings.restoreOnUnload && g_registryWasWritten) {
        RestoreBackupValue(kValueTaskbarEnabled, g_backupEnabled);
        RestoreBackupValue(kValueTaskbarMode, g_backupMode);
    }

    // Always notify Explorer on unload. In the default hook-only mode this is the
    // actual restore path: Explorer will read the real registry values again.
    NotifyExplorerSettingsChanged();
}

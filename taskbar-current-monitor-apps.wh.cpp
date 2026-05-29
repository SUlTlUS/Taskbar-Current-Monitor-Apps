// ==WindhawkMod==
// @id taskbar-current-monitor-apps
// @name Taskbar Current Monitor Apps
// @description Multi-monitor taskbar experiment: show pinned items on all taskbars and diagnose per-monitor running-window filtering.
// @version 1.6.1
// @author SUlTlUS + ChatGPT
// @github https://github.com/SUlTlUS/Taskbar-Current-Monitor-Apps
// @include explorer.exe
// @architecture x86-64
// @compilerOptions -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -ladvapi32 -luser32 -lversion
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Taskbar Current Monitor Apps

v1.6.1 keeps the v1.6 diagnostic hybrid mode and adds registry-read diagnostics.

Previous logs showed:

- the mod is loaded;
- `showPinnedOnAllTaskbars = true` forces `forcedMode = 0` internally;
- old taskbar pinned groups exist on both task lists.

The missing piece is whether Explorer actually re-reads `MMTaskbarMode` after the
hook is active. v1.6.1 logs every forced registry read, limited to avoid spam.

Expected log when the hook is working:

```text
[TCMA] Forced registry read: api=... value=MMTaskbarMode forced=0
```

If you see `forcedMode=0` but no forced registry-read lines, Explorer hasn't re-read
that setting in the current session. Restart `explorer.exe` after enabling the mod.

When `showPinnedOnAllTaskbars` is enabled, the mod forces:

```text
keepEnforced = true
writeRegistry = false
```
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- showPinnedOnAllTaskbars: false
  $name: Show pinned items on all taskbars
  $description: Diagnostic hybrid mode. Forces MMTaskbarMode=0 at runtime so Windows can render pinned items on all taskbars. Running apps may also appear on all taskbars in this test version.
- keepEnforced: true
  $name: Keep taskbar mode enforced
  $description: Forced on when Show pinned items on all taskbars is enabled.
- writeRegistry: false
  $name: Also write registry values
  $description: Forced off when Show pinned items on all taskbars is enabled.
- restoreOnUnload: true
  $name: Restore registry values on unload if registry was written
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
static constexpr DWORD kShowButtonsOnAllTaskbars = 0;
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

using RegGetValueW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
using RegQueryValueExW_t = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using CTaskListWnd_IsOnPrimaryTaskband_t = int(WINAPI*)(PVOID);
using CTaskListWnd__GetTBGroupFromGroup_t = PVOID(WINAPI*)(PVOID, PVOID, int*);
using CTaskListWnd__TaskCreated_t = LONG_PTR(WINAPI*)(PVOID, PVOID, PVOID, int);

static RegGetValueW_t RegGetValueW_Original;
static RegQueryValueExW_t RegQueryValueExW_Original;
static CTaskListWnd_IsOnPrimaryTaskband_t CTaskListWnd_IsOnPrimaryTaskband_Original;
static CTaskListWnd__GetTBGroupFromGroup_t CTaskListWnd__GetTBGroupFromGroup_Original;
static CTaskListWnd__TaskCreated_t CTaskListWnd__TaskCreated_Original;

static bool WideEquals(PCWSTR a, PCWSTR b) {
    return a && b && _wcsicmp(a, b) == 0;
}

static DWORD GetForcedTaskbarMode() {
    return g_settings.showPinnedOnAllTaskbars
        ? kShowButtonsOnAllTaskbars
        : kShowButtonsOnlyWhereWindowIsOpen;
}

static bool IsForcedValueName(PCWSTR valueName, DWORD* forcedValue) {
    if (WideEquals(valueName, kValueTaskbarEnabled)) {
        *forcedValue = kShowTaskbarOnAllDisplays;
        return true;
    }

    if (WideEquals(valueName, kValueTaskbarMode)) {
        *forcedValue = GetForcedTaskbarMode();
        return true;
    }

    return false;
}

static void LogForcedRegistryRead(PCWSTR apiName, PCWSTR valueName, DWORD forcedValue) {
    static int logCount = 0;
    if (logCount < 100) {
        DebugLog(
            L"Forced registry read: api=%s value=%s forced=%u pinned=%d keep=%d write=%d",
            apiName,
            valueName ? valueName : L"(null)",
            forcedValue,
            g_settings.showPinnedOnAllTaskbars,
            g_settings.keepEnforced,
            g_settings.writeRegistry);
        logCount++;
    }
}

static int WINAPI CTaskListWnd_IsOnPrimaryTaskband_Hook(PVOID pThis) {
    if (g_settings.showPinnedOnAllTaskbars) {
        static int n = 0;
        if (n < 50) {
            DebugLog(L"IsOnPrimaryTaskband called, returning TRUE, this=%p", pThis);
            n++;
        }
        return TRUE;
    }

    return CTaskListWnd_IsOnPrimaryTaskband_Original(pThis);
}

static LONG_PTR WINAPI CTaskListWnd__TaskCreated_Hook(
    PVOID pThis,
    PVOID taskGroup,
    PVOID taskItem,
    int param3) {
    static int n = 0;
    if (n < 200) {
        DebugLog(
            L"_TaskCreated called, this=%p, taskGroup=%p, taskItem=%p, param3=%d",
            pThis,
            taskGroup,
            taskItem,
            param3);
        n++;
    }

    LONG_PTR ret = CTaskListWnd__TaskCreated_Original(pThis, taskGroup, taskItem, param3);

    if (g_settings.showPinnedOnAllTaskbars && !taskItem && taskGroup &&
        CTaskListWnd__GetTBGroupFromGroup_Original) {
        int foundIndex = -1;
        PVOID existing = CTaskListWnd__GetTBGroupFromGroup_Original(pThis, taskGroup, &foundIndex);

        static int c = 0;
        if (c < 200) {
            DebugLog(
                L"Pinned check: this=%p, taskGroup=%p, existing=%p, foundIndex=%d, ret=%lld",
                pThis,
                taskGroup,
                existing,
                foundIndex,
                ret);
            c++;
        }
    }

    return ret;
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
    DebugLog(L"taskbar.dll not found, using explorer.exe: %p", explorerModule);
    return explorerModule;
}

static bool HookTaskbarSymbols() {
    HMODULE module = GetTaskbarModuleForSymbols();
    if (!module) {
        DebugLog(L"No taskbar module");
        return false;
    }

    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: virtual int __cdecl CTaskListWnd::IsOnPrimaryTaskband(void))"},
            &CTaskListWnd_IsOnPrimaryTaskband_Original,
            CTaskListWnd_IsOnPrimaryTaskband_Hook,
            true,
        },
        {
            {LR"(protected: struct ITaskBtnGroup * __cdecl CTaskListWnd::_GetTBGroupFromGroup(struct ITaskGroup *,int *))"},
            &CTaskListWnd__GetTBGroupFromGroup_Original,
            nullptr,
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
        DebugLog(L"HookSymbols failed");
        return false;
    }

    g_taskbarSymbolsHooked = true;
    DebugLog(
        L"Hooked taskbar symbols: IsPrimary=%p GetTBGroup=%p TaskCreated=%p",
        CTaskListWnd_IsOnPrimaryTaskband_Original,
        CTaskListWnd__GetTBGroupFromGroup_Original,
        CTaskListWnd__TaskCreated_Original);
    return true;
}

static LSTATUS ReturnForcedDword(
    PCWSTR apiName,
    PCWSTR valueName,
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

    LogForcedRegistryRead(apiName, valueName, forcedValue);

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
            return ReturnForcedDword(L"RegGetValueW", lpValue, forcedValue, dwFlags, pdwType, pvData, pcbData);
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
            return ReturnForcedDword(L"RegQueryValueExW", lpValueName, forcedValue, 0, lpType, lpData, lpcbData);
        }
    }

    return RegQueryValueExW_Original(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
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
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kExplorerAdvancedKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
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

static BOOL CALLBACK RefreshTaskbarEnumProc(HWND hWnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hWnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }

    WCHAR className[64]{};
    if (!GetClassNameW(hWnd, className, ARRAYSIZE(className))) {
        return TRUE;
    }

    if (_wcsicmp(className, L"Shell_TrayWnd") == 0 ||
        _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0) {
        DebugLog(L"RefreshTaskbar: sending WM_SETTINGCHANGE to %s hwnd=%p", className, hWnd);
        SendMessageW(hWnd, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"));
        PostMessageW(hWnd, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"));
    }

    return TRUE;
}

static void RefreshTaskbarWindows() {
    EnumWindows(RefreshTaskbarEnumProc, 0);
    Sleep(250);
    EnumWindows(RefreshTaskbarEnumProc, 0);
}

static void ApplyTaskbarMode() {
    DWORD forcedMode = GetForcedTaskbarMode();

    if (g_settings.writeRegistry) {
        bool okEnabled = WriteDwordToAdvancedKey(kValueTaskbarEnabled, kShowTaskbarOnAllDisplays);
        bool okMode = WriteDwordToAdvancedKey(kValueTaskbarMode, forcedMode);
        g_registryWasWritten = g_registryWasWritten || okEnabled || okMode;
        DebugLog(
            L"ApplyTaskbarMode registry: enabled=%s mode=%s forcedMode=%u pinned=%d symbols=%d",
            okEnabled ? L"ok" : L"fail",
            okMode ? L"ok" : L"fail",
            forcedMode,
            g_settings.showPinnedOnAllTaskbars,
            g_taskbarSymbolsHooked);
    } else {
        DebugLog(
            L"ApplyTaskbarMode hook-only: forcedMode=%u pinned=%d symbols=%d",
            forcedMode,
            g_settings.showPinnedOnAllTaskbars,
            g_taskbarSymbolsHooked);
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
    g_settings.showPinnedOnAllTaskbars = Wh_GetIntSetting(L"showPinnedOnAllTaskbars") != 0;
    g_settings.keepEnforced = Wh_GetIntSetting(L"keepEnforced") != 0;
    g_settings.writeRegistry = Wh_GetIntSetting(L"writeRegistry") != 0;
    g_settings.restoreOnUnload = Wh_GetIntSetting(L"restoreOnUnload") != 0;

    if (g_settings.showPinnedOnAllTaskbars) {
        if (!g_settings.keepEnforced || g_settings.writeRegistry) {
            DebugLog(L"Pinned mode: use keepEnforced=1 and writeRegistry=0");
        }
        g_settings.keepEnforced = true;
        g_settings.writeRegistry = false;
    }

    DebugLog(
        L"Settings: pinned=%d keep=%d write=%d restore=%d forcedMode=%u",
        g_settings.showPinnedOnAllTaskbars,
        g_settings.keepEnforced,
        g_settings.writeRegistry,
        g_settings.restoreOnUnload,
        GetForcedTaskbarMode());
}

BOOL Wh_ModInit() {
    DebugLog(L"Wh_ModInit pid=%lu", GetCurrentProcessId());

    LoadSettings();

    ReadDwordFromAdvancedKey(kValueTaskbarEnabled, &g_backupEnabled);
    ReadDwordFromAdvancedKey(kValueTaskbarMode, &g_backupMode);
    DebugLog(
        L"Backup: enabled existed=%d value=%u, mode existed=%d value=%u",
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
        DebugLog(L"No advapi32");
        return FALSE;
    }

    void* regGetValue = reinterpret_cast<void*>(GetProcAddress(advapi32, "RegGetValueW"));
    void* regQueryValueEx = reinterpret_cast<void*>(GetProcAddress(advapi32, "RegQueryValueExW"));
    if (!regGetValue || !regQueryValueEx) {
        DebugLog(L"No registry APIs");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(regGetValue, reinterpret_cast<void*>(RegGetValueW_Hook), reinterpret_cast<void**>(&RegGetValueW_Original))) {
        DebugLog(L"RegGetValueW hook failed");
        return FALSE;
    }

    if (!Wh_SetFunctionHook(regQueryValueEx, reinterpret_cast<void*>(RegQueryValueExW_Hook), reinterpret_cast<void**>(&RegQueryValueExW_Original))) {
        DebugLog(L"RegQueryValueExW hook failed");
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
    RefreshTaskbarWindows();
}

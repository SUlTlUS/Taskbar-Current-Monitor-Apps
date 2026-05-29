// ==WindhawkMod==
// @id taskbar-current-monitor-apps
// @name Taskbar Current Monitor Apps
// @description Keep pinned taskbar items visible on every taskbar, while running app buttons are shown only on the monitor that owns the window.
// @version 1.1
// @author SUlTlUS + ChatGPT
// @github https://github.com/SUlTlUS/Taskbar-Current-Monitor-Apps
// @include explorer.exe
// @architecture x86-64
// @compilerOptions -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -ladvapi32 -luser32
// ==/WindhawkMod==

#include <windows.h>

static constexpr const wchar_t* kExplorerAdvancedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
static constexpr const wchar_t* kValueTaskbarEnabled = L"MMTaskbarEnabled";
static constexpr const wchar_t* kValueTaskbarMode = L"MMTaskbarMode";
static constexpr DWORD kShowTaskbarOnAllDisplays = 1;
static constexpr DWORD kShowButtonsOnlyWhereWindowIsOpen = 2;

struct Settings { bool keepEnforced=true; bool writeRegistry=true; bool restoreOnUnload=true; };
struct BackupValue { bool captured=false; bool existed=false; DWORD value=0; };
static Settings g_settings; static BackupValue g_backupEnabled, g_backupMode;
using RegGetValueW_t=LSTATUS(WINAPI*)(HKEY,LPCWSTR,LPCWSTR,DWORD,LPDWORD,PVOID,LPDWORD);
using RegQueryValueExW_t=LSTATUS(WINAPI*)(HKEY,LPCWSTR,LPDWORD,LPDWORD,LPBYTE,LPDWORD);
static RegGetValueW_t RegGetValueW_Original; static RegQueryValueExW_t RegQueryValueExW_Original;
static bool WideEquals(PCWSTR a, PCWSTR b){return a&&b&&_wcsicmp(a,b)==0;}
static bool IsForcedValueName(PCWSTR v,DWORD* f){if(WideEquals(v,kValueTaskbarEnabled)){*f=kShowTaskbarOnAllDisplays;return true;}if(WideEquals(v,kValueTaskbarMode)){*f=kShowButtonsOnlyWhereWindowIsOpen;return true;}return false;}
static LSTATUS ReturnForcedDword(DWORD f,DWORD flags,LPDWORD t,PVOID d,LPDWORD s){constexpr DWORD RRF_RT_ANY=0x0000ffff,RRF_RT_REG_DWORD_CONST=0x00000010;DWORD typeFilter=flags&RRF_RT_ANY;if(typeFilter&&!(typeFilter&RRF_RT_REG_DWORD_CONST))return ERROR_FILE_NOT_FOUND;if(t)*t=REG_DWORD;if(s){if(!d){*s=sizeof(DWORD);return ERROR_SUCCESS;}if(*s<sizeof(DWORD)){*s=sizeof(DWORD);return ERROR_MORE_DATA;}*s=sizeof(DWORD);}if(d)*reinterpret_cast<DWORD*>(d)=f;return ERROR_SUCCESS;}
static LSTATUS WINAPI RegGetValueW_Hook(HKEY h,LPCWSTR s,LPCWSTR v,DWORD f,LPDWORD t,PVOID d,LPDWORD sz){if(g_settings.keepEnforced){DWORD fv=0;if(IsForcedValueName(v,&fv))return ReturnForcedDword(fv,f,t,d,sz);}return RegGetValueW_Original(h,s,v,f,t,d,sz);}
static LSTATUS WINAPI RegQueryValueExW_Hook(HKEY h,LPCWSTR v,LPDWORD r,LPDWORD t,LPBYTE d,LPDWORD s){if(g_settings.keepEnforced){DWORD fv=0;if(IsForcedValueName(v,&fv))return ReturnForcedDword(fv,0,t,d,s);}return RegQueryValueExW_Original(h,v,r,t,d,s);}
static bool ReadDwordFromAdvancedKey(PCWSTR v,BackupValue* b){b->captured=true;HKEY key=nullptr;LSTATUS st=RegOpenKeyExW(HKEY_CURRENT_USER,kExplorerAdvancedKey,0,KEY_QUERY_VALUE,&key);if(st!=ERROR_SUCCESS){b->existed=false;return false;}DWORD type=0,value=0,size=sizeof(value);st=RegQueryValueExW(key,v,nullptr,&type,reinterpret_cast<LPBYTE>(&value),&size);RegCloseKey(key);if(st==ERROR_SUCCESS&&type==REG_DWORD&&size==sizeof(DWORD)){b->existed=true;b->value=value;return true;}b->existed=false;return false;}
static bool WriteDwordToAdvancedKey(PCWSTR v,DWORD value){HKEY key=nullptr;DWORD d=0;LSTATUS st=RegCreateKeyExW(HKEY_CURRENT_USER,kExplorerAdvancedKey,0,nullptr,REG_OPTION_NON_VOLATILE,KEY_SET_VALUE,nullptr,&key,&d);if(st!=ERROR_SUCCESS){Wh_Log(L"RegCreateKeyExW failed: %ld",st);return false;}st=RegSetValueExW(key,v,0,REG_DWORD,reinterpret_cast<const BYTE*>(&value),sizeof(value));RegCloseKey(key);if(st!=ERROR_SUCCESS){Wh_Log(L"RegSetValueExW(%s) failed: %ld",v,st);return false;}return true;}
static void DeleteAdvancedValue(PCWSTR v){HKEY key=nullptr;LSTATUS st=RegOpenKeyExW(HKEY_CURRENT_USER,kExplorerAdvancedKey,0,KEY_SET_VALUE,&key);if(st==ERROR_SUCCESS){RegDeleteValueW(key,v);RegCloseKey(key);}}
static void NotifyExplorerSettingsChanged(){DWORD_PTR r=0;SendMessageTimeoutW(HWND_BROADCAST,WM_SETTINGCHANGE,0,reinterpret_cast<LPARAM>(L"TraySettings"),SMTO_ABORTIFHUNG|SMTO_NORMAL,2000,&r);SendMessageTimeoutW(HWND_BROADCAST,WM_SETTINGCHANGE,0,reinterpret_cast<LPARAM>(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"),SMTO_ABORTIFHUNG|SMTO_NORMAL,2000,&r);}
static void ApplyTaskbarMode(){if(!g_settings.writeRegistry)return;bool ok1=WriteDwordToAdvancedKey(kValueTaskbarEnabled,kShowTaskbarOnAllDisplays);bool ok2=WriteDwordToAdvancedKey(kValueTaskbarMode,kShowButtonsOnlyWhereWindowIsOpen);Wh_Log(L"ApplyTaskbarMode: MMTaskbarEnabled=%u (%s), MMTaskbarMode=%u (%s)",kShowTaskbarOnAllDisplays,ok1?L"ok":L"failed",kShowButtonsOnlyWhereWindowIsOpen,ok2?L"ok":L"failed");NotifyExplorerSettingsChanged();}
static void RestoreBackupValue(PCWSTR v,const BackupValue& b){if(!b.captured)return;if(b.existed)WriteDwordToAdvancedKey(v,b.value);else DeleteAdvancedValue(v);}
static void LoadSettings(){g_settings.keepEnforced=Wh_GetIntSetting(L"keepEnforced")!=0;g_settings.writeRegistry=Wh_GetIntSetting(L"writeRegistry")!=0;g_settings.restoreOnUnload=Wh_GetIntSetting(L"restoreOnUnload")!=0;Wh_Log(L"Settings: keepEnforced=%d, writeRegistry=%d, restoreOnUnload=%d",g_settings.keepEnforced,g_settings.writeRegistry,g_settings.restoreOnUnload);}
BOOL Wh_ModInit(){Wh_Log(L"Taskbar Current Monitor Apps init");LoadSettings();ReadDwordFromAdvancedKey(kValueTaskbarEnabled,&g_backupEnabled);ReadDwordFromAdvancedKey(kValueTaskbarMode,&g_backupMode);HMODULE advapi32=GetModuleHandleW(L"advapi32.dll");if(!advapi32)advapi32=LoadLibraryW(L"advapi32.dll");if(!advapi32){Wh_Log(L"Couldn't load advapi32.dll");return FALSE;}void* regGetValue=reinterpret_cast<void*>(GetProcAddress(advapi32,"RegGetValueW"));void* regQueryValueEx=reinterpret_cast<void*>(GetProcAddress(advapi32,"RegQueryValueExW"));if(!regGetValue||!regQueryValueEx){Wh_Log(L"Couldn't find registry APIs");return FALSE;}if(!Wh_SetFunctionHook(regGetValue,reinterpret_cast<void*>(RegGetValueW_Hook),reinterpret_cast<void**>(&RegGetValueW_Original))){Wh_Log(L"Wh_SetFunctionHook(RegGetValueW) failed");return FALSE;}if(!Wh_SetFunctionHook(regQueryValueEx,reinterpret_cast<void*>(RegQueryValueExW_Hook),reinterpret_cast<void**>(&RegQueryValueExW_Original))){Wh_Log(L"Wh_SetFunctionHook(RegQueryValueExW) failed");return FALSE;}ApplyTaskbarMode();return TRUE;}
void Wh_ModAfterInit(){ApplyTaskbarMode();}
void Wh_ModSettingsChanged(){LoadSettings();ApplyTaskbarMode();}
void Wh_ModUninit(){Wh_Log(L"Taskbar Current Monitor Apps uninit");if(g_settings.restoreOnUnload){g_settings.keepEnforced=false;RestoreBackupValue(kValueTaskbarEnabled,g_backupEnabled);RestoreBackupValue(kValueTaskbarMode,g_backupMode);NotifyExplorerSettingsChanged();}}
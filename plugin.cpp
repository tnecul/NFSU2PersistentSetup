// SPDX-License-Identifier: CC0-1.0
//
// NFSU2PersistentSetup — persists the last dyno-committed setup per car across
// race types and sessions, and suppresses the game's race-type auto-switching.
//
// Targets SPEED2.EXE (sha256 bc34932b36bb19194000690ec26edf964176856c31fb0978e01e61ee814eee9d).
//
// Evidence map (virtual addresses, EXE base 0x400000):
//   0x50FC20  FUN_0050FC20(this=ECX, slot)   dyno commit: editor buffer -> slot bank
//   0x516AC0  FUN_00516AC0(this=ECX, dstCar, mode) race/dyno entry copy:
//             this+0x348+mode*0x29  -> dstCar+0x398 (41 bytes)
//   0x850078  app mode global (1 = career gameplay, 2 = main menu/quick race,
//             4 = 2P, 8/0x100 = online, 0x200 = special)
//   0x863480  active car id (stable per car across launches, verified runtime)
//   0x83A63C  setup slot selector (0xFF = no dyno editing session open)
//
// Runtime-only install: patches prologs with JMP into naked stubs in hooks.S
// which run our logic then jump to hand-built trampolines. No game files on
// disk are modified.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <map>
#include <string>

extern "C" {
    void* g_trampSave = nullptr;    // jump target set by InstallHooks()
    void* g_trampEntry = nullptr;
}

// ---------------------------------------------------------------------------
// Game addresses: computed from the runtime module base so ASLR would not
// matter. Observed and verified base is 0x400000.
// ---------------------------------------------------------------------------
static const uint32_t OFF_SAVE_FN   = 0x0010FC20; // 0x50FC20 - 0x400000
static const uint32_t OFF_ENTRY_FN  = 0x00116AC0; // 0x516AC0 - 0x400000
static const uint32_t OFF_GAME_MODE = 0x00450078; // 0x850078
static const uint32_t OFF_CAR_ID    = 0x00463480; // 0x863480
static const uint32_t OFF_SLOT_SEL  = 0x0043A63C; // 0x83A63C

static uintptr_t g_base      = 0;
static bool      g_careerOnly = true;
static bool      g_debugLog  = false;
static char      g_iniPath[MAX_PATH] = { 0 };
static char      g_logPath[MAX_PATH] = { 0 };

static inline bool IsCareerGameplay() {
    return *(int32_t*)(g_base + OFF_GAME_MODE) == 1;
}

static inline bool PluginActive() {
    return !g_careerOnly || IsCareerGameplay();
}

static inline uint32_t ReadCarId() {
    if (!g_base) return 0;
    uint32_t id = *(volatile uint32_t*)(g_base + OFF_CAR_ID);
    if (id == 0 || id == 0xFFFFFFFFu) return 0;
    return id;
}

static inline bool DynoInactive() {
    // 0xFF counts as "no dyno editing session"; read 32-bit to be safe.
    return *(volatile uint32_t*)(g_base + OFF_SLOT_SEL) == 0xFF;
}

static void DebugLogLine(const char* fmt, ...) {
    if (!g_debugLog || !g_logPath[0]) return;
    FILE* f = fopen(g_logPath, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------------------
// Persistent setup store: NFSU2PersistentSetup.ini next to the plugin in scripts\.
//
//   [General]
//   CareerOnly=1
//   DebugLog=0
//
//   [Car_319A8CA1]
//   Payload=<82 hex chars, 41 bytes>
// ---------------------------------------------------------------------------
static const size_t kPayloadSize = 41;

struct SetupEntry {
    bool    valid;
    uint8_t data[kPayloadSize];
};

static std::map<uint32_t, SetupEntry> g_cache;
static bool g_cacheLoaded = false;

static void BuildPaths(HMODULE pluginModule) {
    char modulePath[MAX_PATH];
    GetModuleFileNameA(pluginModule, modulePath, MAX_PATH);
    // strip filename, keep directory (the ASI itself lives in scripts\)
    char* lastSlash = modulePath;
    for (char* p = modulePath; *p; ++p) {
        if (*p == '\\' || *p == '/') lastSlash = p + 1;
    }
    *lastSlash = '\0';
    lstrcpyA(g_iniPath, modulePath);
    lstrcatA(g_iniPath, "NFSU2PersistentSetup.ini");
    lstrcpyA(g_logPath, modulePath);
    lstrcatA(g_logPath, "NFSU2PersistentSetup.log");

    // Generate the config when absent: General on top, CareerOnly on.
    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringA("General", "CareerOnly", "1", g_iniPath);
        WritePrivateProfileStringA("General", "DebugLog", "0", g_iniPath);
    }
}

static bool HexDecode(const char* hex, uint8_t* out, size_t bytes) {
    if (!hex) return false;
    if (lstrlenA(hex) != (int)(bytes * 2)) return false;
    for (size_t i = 0; i < bytes; ++i) {
        int hi = hex[2 * i], lo = hex[2 * i + 1];
        int vh, vl;
        if (hi >= '0' && hi <= '9') vh = hi - '0';
        else if (hi >= 'a' && hi <= 'f') vh = hi - 'a' + 10;
        else if (hi >= 'A' && hi <= 'F') vh = hi - 'A' + 10;
        else return false;
        if (lo >= '0' && lo <= '9') vl = lo - '0';
        else if (lo >= 'a' && lo <= 'f') vl = lo - 'a' + 10;
        else if (lo >= 'A' && lo <= 'F') vl = lo - 'A' + 10;
        else return false;
        out[i] = (uint8_t)((vh << 4) | vl);
    }
    return true;
}

static void HexEncode(const uint8_t* in, size_t bytes, char* out /* 2*bytes+1 */) {
    static const char* digits = "0123456789ABCDEF";
    for (size_t i = 0; i < bytes; ++i) {
        out[2 * i]     = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0xF];
    }
    out[bytes * 2] = '\0';
}

static void LoadCache() {
    g_cacheLoaded = true;
    char sectionNames[16384];
    sectionNames[0] = '\0';
    sectionNames[1] = '\0';
    GetPrivateProfileSectionNamesA(sectionNames, sizeof(sectionNames), g_iniPath);

    const char* p = sectionNames;
    while (*p) {
        if (!strncmp(p, "Car_", 4) && lstrlenA(p) == 12) {
            uint32_t carId = (uint32_t)strtoul(p + 4, nullptr, 16);
            if (carId) {
                char payloadHex[160] = { 0 };
                GetPrivateProfileStringA(p, "Payload", "", payloadHex,
                                         sizeof(payloadHex), g_iniPath);
                SetupEntry e;
                e.valid = false;
                memset(e.data, 0, sizeof(e.data));
                if (HexDecode(payloadHex, e.data, kPayloadSize)) {
                    e.valid = true;
                    g_cache[carId] = e;
                    DebugLogLine("load: car %08X payload ok", carId);
                }
                else {
                    DebugLogLine("load: car %08X payload rejected (hexlen=%d)",
                                 carId, lstrlenA(payloadHex));
                }
            }
        }
        p += lstrlenA(p) + 1;
    }
}

// Cached entry pointer or nullptr; loads the INI lazily on first use.
static SetupEntry* GetEntry(uint32_t carId) {
    if (!g_cacheLoaded) LoadCache();
    auto it = g_cache.find(carId);
    if (it == g_cache.end()) return nullptr;
    return it->second.valid ? &it->second : nullptr;
}

static void SaveEntry(uint32_t carId, const uint8_t* data) {
    if (!carId) return;

    SetupEntry e;
    e.valid = true;
    memcpy(e.data, data, kPayloadSize);
    g_cache[carId] = e;                 // force hook picks this up immediately

    char section[32];
    wsprintfA(section, "Car_%08X", carId);

    char hex[2 * kPayloadSize + 1];
    HexEncode(data, kPayloadSize, hex);
    WritePrivateProfileStringA(section, "Payload", hex, g_iniPath);

    char first[3] = { 0 }, last[3] = { 0 };
    HexEncode(data, 1, first);
    HexEncode(data + kPayloadSize - 1, 1, last);
    DebugLogLine("save: car %08X committed (first=%s last=%s)", carId, first, last);
}

// ---------------------------------------------------------------------------
// Hook helpers, called from naked stubs in hooks.S.
//   __fastcall on GCC i386: 1st arg -> ECX, 2nd -> EDX, rest pushed on the
//   stack right-to-left and popped by the callee. The stubs only push the
//   stack args; ECX holds `this` from the game call site and EDX is garbage,
//   which is fine because arg 2 is ignored.
// ---------------------------------------------------------------------------

// Hook of FUN_0050FC20 (dyno commit). ECX = setup manager this, stack arg = slot.
extern "C" __attribute__((fastcall)) void CaptureCommit(void* thiz, void* /*edx*/, int slot) {
    if (slot < 0 || slot == 0xFF) return;          // mirrors the game's own no-op guard
    if (!PluginActive()) return;                   // CareerOnly: only capture in career

    unsigned char* thizB = (unsigned char*)thiz;
    uintptr_t editorBuf = *(uintptr_t*)(thizB + 0x900);
    if (!editorBuf) return;                        // mirrors game guard

    uint32_t carId = ReadCarId();
    if (!carId) { DebugLogLine("save: skipped, no car id"); return; }

    // The 41 bytes being committed live at editorBuf + 0x14 (see FUN_0050FC20).
    SaveEntry(carId, (const uint8_t*)(editorBuf + 0x14));
}

// Hook of FUN_00516AC0 (race/dyno entry copy). Runs BEFORE the original:
// swaps the source bank's contents with the persisted payload so the original
// copy, and its follow-up call, observe our bytes.
// ECX = source object, stack args = (dstCar, mode).
extern "C" __attribute__((fastcall)) void PreEntryPatch(void* srcThiz, void* /*edx*/,
                                                        int dstCar, int mode) {
    (void)dstCar;
    if (mode < 0 || mode > 4) return;              // only the 5 known setup modes
    if (!DynoInactive()) return;                   // never fight a live dyno editing session
    if (!PluginActive()) return;                   // CareerOnly: only force in career

    if (!g_base) return;

    uint32_t carId = ReadCarId();
    if (!carId) return;
    DebugLogLine("entry: car %08X mode %d %s", carId, mode,
                 g_careerOnly && IsCareerGameplay() ? "(career)" : "(non-career)");

    SetupEntry* e = GetEntry(carId);
    if (!e) { DebugLogLine("entry: no persisted setup, vanilla behavior"); return; }

    unsigned char* srcB = (unsigned char*)srcThiz;
    uint8_t* bank = srcB + 0x348 + (size_t)mode * 0x29;
    memcpy(bank, e->data, kPayloadSize);
    DebugLogLine("entry: patched bank mode %d with persisted setup", mode);
}

// Hook stubs and trampoline plumbing live here; defined in hooks.S.
extern "C" void HookSave();
extern "C" void HookEntry();

// ---------------------------------------------------------------------------
// Installation: hand-built trampolines + prolog JMP patches.
// ---------------------------------------------------------------------------

// Applies patch and flushes the code cache.
static void WriteCodeProt(void* addr, const void* bytes, size_t len) {
    DWORD old = 0;
    if (!VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) {
        DebugLogLine("install: VirtualProtect failed at %p", addr);
        return;
    }
    memcpy(addr, bytes, len);
    DWORD keep = 0;
    VirtualProtect(addr, len, old, &keep);
    FlushInstructionCache(GetCurrentProcess(), addr, len);
}

// Allocate executable RWX within +-2 GB of target (trampoline distance).
static void* AllocNear(uintptr_t target) {
    for (uintptr_t base = 0x08000000; base < 0x50000000; base += 0x10000) {
        void* p = VirtualAlloc((LPVOID)base, 0x10000,
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (p) {
            // Ensure patched targets stay inside the near-jmp window.
            long long diff = (long long)((uintptr_t)base) - (long long)target;
            if (diff > -0x7FF00000LL && diff < 0x7FF00000LL) return p;
            VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    return nullptr;
}

static bool InstallHooks() {
    // Build one trampoline region for each hook.
    void* regionSave  = AllocNear(g_base + OFF_SAVE_FN);
    void* regionEntry = AllocNear(g_base + OFF_ENTRY_FN);
    if (!regionSave || !regionEntry) {
        DebugLogLine("install: trampoline allocation failed");
        return false;
    }
    memset(regionSave,  0x90, 0x10000);
    memset(regionEntry, 0x90, 0x10000);

    // --- FUN_0050FC20 -----------------------------------------------
    // Original prolog (5 bytes, offsets relative):
    //   0x50FC20  83 39 00        cmp dword ptr [ecx], 0
    //   0x50FC23  74 30           jz 0x50FC53
    // Trampoline: same cmp, jz re-encoded rel32, then jmp back to 0x50FC25.
    {
        uint8_t* t = (uint8_t*)regionSave;
        t[0] = 0x83; t[1] = 0x39; t[2] = 0x00;              // cmp [ecx],0
        t[3] = 0x0F; t[4] = 0x84;                            // jz rel32
        int32_t zRel = (int32_t)(0x50FC53 - ((uintptr_t)t + 3 + 6));
        *(int32_t*)(t + 5) = zRel;
        t[9] = 0xE9;                                         // jmp back to 0x50FC25
        *(int32_t*)(t + 10) = (int32_t)((g_base + OFF_SAVE_FN + 5) - ((uintptr_t)t + 9 + 5));
        g_trampSave = t;
    }

    // --- FUN_00516AC0 -----------------------------------------------
    // Original prolog (7 bytes, position independent):
    //   0x516AC0  8B 44 24 04    mov eax, [esp+4]
    //   0x516AC4  83 EC 2C       sub esp, 0x2C
    // Trampoline: copy 7 bytes verbatim, then jmp back to 0x516AC7.
    {
        uint8_t* t = (uint8_t*)regionEntry;
        memcpy(t, (void*)(g_base + OFF_ENTRY_FN), 7);
        t[7] = 0xE9;
        *(int32_t*)(t + 8) = (int32_t)((g_base + OFF_ENTRY_FN + 7) - ((uintptr_t)t + 7 + 5));
        g_trampEntry = t;
    }

    // Patch prologs: E9 rel32 to hook stubs.
    {
        uint8_t jmp[5];
        jmp[0] = 0xE9;
        *(int32_t*)(jmp + 1) = (int32_t)((uintptr_t)(void*)HookSave -
                                         (g_base + OFF_SAVE_FN) - 5);
        WriteCodeProt((void*)(g_base + OFF_SAVE_FN), jmp, 5);
    }
    {
        uint8_t jmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        *(int32_t*)(jmp + 1) = (int32_t)((uintptr_t)(void*)HookEntry -
                                         (g_base + OFF_ENTRY_FN) - 5);
        WriteCodeProt((void*)(g_base + OFF_ENTRY_FN), jmp, 7);
    }

    DebugLogLine("install: hooks live. tramps save=%p entry=%p", regionSave, regionEntry);
    return true;
}

// ---------------------------------------------------------------------------
static void ReadConfig() {
    g_careerOnly = GetPrivateProfileIntA("General", "CareerOnly", 1, g_iniPath) != 0;
    g_debugLog   = GetPrivateProfileIntA("General", "DebugLog", 0, g_iniPath) != 0;
}

BOOL WINAPI DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    (void)module;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_base = (uintptr_t)GetModuleHandleA(nullptr);
        BuildPaths(module);
        ReadConfig();

        SYSTEMTIME st;
        GetLocalTime(&st);
        DebugLogLine("=== NFSU2PersistentSetup attach: base=%08X CareerOnly=%d "
                     "(%04d-%02d-%02d) ===",
                     (uint32_t)g_base, g_careerOnly ? 1 : 0,
                     st.wYear, st.wMonth, st.wDay);

        if (g_base != 0x400000llu) {
            DebugLogLine("install: unexpected module base %08X (expected 400000); aborting hooks",
                         (uint32_t)g_base);
            return TRUE;
        }
        InstallHooks();
    }
    return TRUE;
}

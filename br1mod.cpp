// br1mod — dinput8.dll proxy for BloodRayne: Terminal Cut (GOG)
// Adds a "All powers and combos: Yes/No" cycler row at the top of the
// native New Game menu, persisted to br1mod.ini.
//
// Injection: rayne1.exe imports ONLY DirectInput8Create from dinput8.dll.
// Our dll in the game dir shadows System32's; the .def forwards the export.
//
// Hooks: 2 inline patches via `push addr; ret`:
//   A) 0x4C89AE  inside the New Game menu builder (jump-table case 2),
//      right before the Training row is appended -> we append our row.
//   B) 0x4CCD46  action dispatch load (edi <- selected item action) ->
//      our action code gets handled by us instead of the native switch.
//
// ABI of the menu helpers (verified by disassembly):
//   Localize   @0x4E7840 : const char* __cdecl (const char* key) —
//                        binary-searches the loaded msglist; returns the
//                        key pointer itself on miss.
//   AddText0   @0x4C4C40 : ecx=label text, eax=action code, esi=0
//                        (esi !=0 would be copied as a second column).
//   Item array @0x1D20068, stride 0xE8, count @0x1D27B40:
//     +0 type, +4 action, +8 label[].
//   Menu id    @0x1D27B24 (2 = New Game). Builder @0x4C88D0 clears the
//     item list and rebuilds from the current id — the case-0 action
//     handler at 0x4CE0C6 calls it, so unhandled codes land on a
//     menu rebuild; we reuse that path after toggling.
//
// Build (x86 Native Tools prompt):
//   cl /O2 /MD /LD br1mod.cpp /link /DEF:dinput8.def /OUT:dinput8.dll

#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <intrin.h>

// ----------------------------- addresses ----------------------------------
// rayne1.exe has .reloc + DYNAMIC_BASE and gets ASLR'd (seen loading at
// 0xC20000 instead of 0x400000). Everything below is an RVA from the
// preferred base; g_exeBase + RVA() is resolved at runtime.
#define RVA(x) (g_exeBase + ((DWORD)(x) - 0x400000))

#define FN_LOCALIZE    0x4E7840
#define FN_ADD_ITEM    0x4C4C40

#define SITE_MENU_INJ  0x4C89AE   // cmp byte [0x62e8c9],3 (7B) in New Game case
#define RET_MENU_INJ   0x4C89B5
#define SITE_DISPATCH  0x4CCD46   // mov edi,[eax+0x1d2006c] (6B)
#define RET_DISPATCH   0x4CCD4C
#define RET_HANDLED    0x4CE0C6   // case-0 handler (rebuild + epilogue)
#define G_PROG_BYTE    0x62E8C9   // campaign progression, gates Act rows
#define G_ITEMS_OFF4   0x1D2006C  // item array base +4 (action field)

// Script command fetch inside the level-script executor (0x510E40):
// esi = [cmdEntry+4] = NUL-terminated command line. Fires once per command
// execution — setup commands run once per level, so rewriting here applies
// the powers recipe exactly "at level start".
#define SITE_CMD_FETCH 0x510EC2   // mov esi,[ecx+4] + mov [ebp-0x3fc],eax (9B)
#define RET_CMD_FETCH  0x510ECB   // inc eax
#define RET_CMD_REPLAY 0x510ECC   // past the inc: slot index stays -> same slot runs again
#define G_CMD_MODE     0x1E4B570  // 1=syntax pass, 2=syntaxCheckOff, 0=real run
#define G_HERO_A       0x1C4AECC
#define G_HERO_B       0x1D1F530
#define FN_FIND_HERO   0x40E200   // cdecl(a,b) -> hero*; handlers write hero+0x5b48..0x5b50
#define FN_FLAG_GET    0x45DDE0   // thiscall(mgr, name) -> flag value
#define G_FLAG_MGR     0x62CB18

#define ACTION_POWERS  0x150

#define INI_FILE       ".\\br1mod.ini"

// ----------------------------- state --------------------------------------
static DWORD g_exeBase = 0;
static int   g_powers = 0;
static char  g_label[160];

// Rebased values the naked hooks read as memory operands.
static DWORD g_fnLocalize, g_fnAddItem, g_progByte, g_itemsOff4;
static DWORD g_retMenuInj, g_retDispatch, g_retHandled, g_retCmdFetch, g_retCmdFetchReplay;
static int   g_replay;

typedef const char* (__cdecl *Localize_t)(const char*);
static const char* Localize(const char* key) {
    return ((Localize_t)g_fnLocalize)(key);
}

// ------------------------- localization -----------------------------------
enum { L_EN, L_ES, L_FR, L_IT, L_RU, L_JA };
static int g_lang = L_EN;
static int g_langDetected = 0;

// Called lazily from the menu-build hook — the msglist is guaranteed to be
// loaded by then. Calling Localize during early startup would deref a table
// that isn't populated yet and kill the process.
static void DetectLang() {
    const char* y = Localize("Yes");
    if (!y) return;
    const unsigned char* u = (const unsigned char*)y;
    if (!strcmp(y, "Oui"))            g_lang = L_FR;
    else if (u[0] == 0x53 && u[1] == 0xED) g_lang = L_ES;   // "Sí"
    else if (u[0] == 0x53 && u[1] == 0xEC) g_lang = L_IT;   // "Sì"
    else if (u[0] == 0x82)            g_lang = L_JA;        // "はい" (SJIS)
    else if (u[0] >= 0x80)            g_lang = L_RU;        // "Да"
    else                              g_lang = L_EN;
    g_langDetected = 1;
}

// English label, then per-language. Order matches the L_* enum.
static const char* LABEL_POWERS[6] = {
    "All powers and combos",               // EN
    "Todos los poderes y combos",          // ES
    "Tous les pouvoirs et combos",         // FR
    "Tutti i poteri e le combo",           // IT
    "\xC2\xF1\xE5 \xF1\xEF\xEE\xF1\xEE\xE1\xED\xEE\xF1\xF2\xE8 \xE8 \xEA\xEE\xEC\xE1\xEE", // RU "Все способности и комбо"
    "\x82\xB7\x82\xD7\x82\xC4\x82\xCC\x83\x70\x83\x8F\x81\x5B\x82\xC6\x83\x52\x83\x93\x83\x7B", // JA "すべてのパワーとコンボ"
};

static const char* ModLocalize(const char* key) {
    const char* t = Localize(key);
    if (t && t != key) return t;
    return key;
}

// ------------------------------ logic -------------------------------------
static void ComposeLabel() {
    if (!g_langDetected) DetectLang();
    const char* lbl = ModLocalize(LABEL_POWERS[g_lang]);
    const char* val = ModLocalize(g_powers ? "Yes" : "No");
    sprintf(g_label, "%s: %s", lbl, val);
}

static void TogglePowers() {
    g_powers ^= 1;
    WritePrivateProfileStringA("Powers", "all", NULL, INI_FILE);
    WritePrivateProfileStringA("Powers", "all", g_powers ? "1" : "0", INI_FILE);
}

static void DiTrace(const char* fmt, ...) {
    FILE* f = fopen("br1mod_trace.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap); fclose(f);
}

// --------------------- script command rewriting -----------------------------
// Replicates the user's old WORLD.POD mod in memory, gated by g_powers.
// The executor (0x510E40) reads each command through the pointer at
// entry+4 (12-byte entries). Nothing is edited in place: the entry is
// repointed at our own writable NUL-terminated buffers.
//  * Slot 0 of every script: the executor is made to re-run the same slot
//    once per setup command in g_inj[] (the hook returns past the `inc eax`
//    so [script+0x3bc] stays put), then the real command runs. This adds the
//    commands the original LA_* scripts simply do not have.
//  * Restrictive commands are swapped for their unlocked equivalents.
static char g_inj[4][40] = {
    "setSlowMoAvailability(true)",
    "setZoomAvailability(true)",
    "setComboLevel(5,5)",
    "flagOn(powerMovesAreAvailable)",
};
#define N_INJ 4
static char g_rpCombo[24]   = "setComboLevel(5,5)";
static char g_rpZoom[32]    = "setZoomAvailability(true)";
static char g_rpSlow[32]    = "setSlowMoAvailability(true)";
static char g_rpHarp[32]    = "flagOff(disableHarpoon)";
static char g_rpHarpJump[32]= "flagOff(disableHarpoonJump)";

static DWORD g_cmdModePtr, g_heroGlobA, g_heroGlobB, g_fnFindHero, g_fnFlagGet, g_flagMgrPtr;

// Per-entry memory of the original text while its slot-0 setup commands run.
static struct { BYTE* entry; char* orig; } g_saved[16];
static BYTE* g_prevEntry;
static int   g_prevIdx = -1;
static int   g_logLeft = 160;

static int SavedPut(BYTE* e, char* t) {
    for (int i = 0; i < 16; ++i) if (g_saved[i].entry == e || !g_saved[i].entry) {
        g_saved[i].entry = e; g_saved[i].orig = t; return 1;
    }
    return 0;
}
static char* SavedTake(BYTE* e) {
    for (int i = 0; i < 16; ++i) if (g_saved[i].entry == e) {
        char* t = g_saved[i].orig; g_saved[i].entry = NULL; g_saved[i].orig = NULL; return t;
    }
    return NULL;
}

static int FlagGet(const char* name) {
    DWORD fn = g_fnFlagGet, mgr = *(DWORD*)g_flagMgrPtr; int r;
    __asm { mov ecx, mgr
            push name
            call fn
            mov r, eax }
    return r;
}

// Reads the hero's availability fields the engine's own handlers write
// ([hero+0x5b48] zoom, +0x5b49 slowmo, +0x5b4c/+0x5b50 combo).
static void LogHero(const char* why) {
    __try {
        DWORD a = *(DWORD*)g_heroGlobA, b = *(DWORD*)g_heroGlobB, fn = g_fnFindHero, hero;
        __asm { push b
                push a
                call fn
                add esp, 8
                mov hero, eax }
        if (!hero) { DiTrace("  [%s] hero=NULL\n", why); return; }
        DiTrace("  [%s] hero=%08X zoom=%d slow=%d combo=%d,%d powerMoves=%d\n", why, hero,
                *(BYTE*)(hero + 0x5b48), *(BYTE*)(hero + 0x5b49),
                *(int*)(hero + 0x5b4c), *(int*)(hero + 0x5b50),
                FlagGet("powerMovesAreAvailable"));
    } __except (EXCEPTION_EXECUTE_HANDLER) { DiTrace("  [%s] hero read faulted\n", why); }
}

static int NameIs(const char* t, const char* name, int n) {
    return !_strnicmp(t, name, n) && (t[n] == '(' || t[n] == ' ');
}

static void ReplaceRestrictive(char** pt) {
    char* t = *pt;
    if (NameIs(t, "setComboLevel", 13))               { *pt = g_rpCombo; return; }
    if (NameIs(t, "setZoomAvailability", 19))         { *pt = g_rpZoom;  return; }
    if (NameIs(t, "setSlowMoAvailability", 21))       { *pt = g_rpSlow;  return; }
    if (!_strnicmp(t, "flagOn", 6)) {
        const char* p = t + 6;
        while (*p == ' ') ++p;
        if (*p != '(') return;
        ++p;
        while (*p == ' ') ++p;
        if (_strnicmp(p, "disableHarpoon", 14)) return;
        *pt = _strnicmp(p + 14, "Jump", 4) ? g_rpHarp : g_rpHarpJump;
        if (g_logLeft > 0) { --g_logLeft; DiTrace("RESTRICT [%s] -> [%s]\n", t, *pt); }
    }
}

// Returns 1 to make the executor re-run the same slot (replay), 0 to go on.
// Executor modes ([0x1E4B570]): 1 = syntax-check pass at script load,
// 2 = syntaxCheckOff region, 0 = real execution (incl. initSection and the
// main run, which starts at slot 0). Only mode 0 gets the injected setup.
static int __cdecl ProcessCommand(int idx, BYTE* entry) {
    if (!g_powers) return 0;
    char** pt = (char**)(entry + 4);
    char* t = *pt;
    int mode = *(int*)g_cmdModePtr;
    int again = (g_prevEntry == entry && g_prevIdx == idx);
    g_prevEntry = entry; g_prevIdx = idx;
    if (!t) return 0;
    if (idx == 0 && mode == 0) {
        int k = -1;
        for (int i = 0; i < N_INJ; ++i) if (t == g_inj[i]) { k = i; break; }
        if (k < 0) {
            // `again`: slot 0 yielded and is being re-run — don't re-inject
            if (!again && SavedPut(entry, t)) {
                if (g_logLeft > 0) { --g_logLeft; DiTrace("INJECT entry=%p first=[%s]\n", entry, t); }
                *pt = g_inj[0];
                return 1;
            }
        } else if (k + 1 < N_INJ) {
            *pt = g_inj[k + 1];
            return 1;
        } else if (char* o = SavedTake(entry)) {
            *pt = o;
            if (g_logLeft > 0) { --g_logLeft; LogHero("after inject"); }
        }
    }
    ReplaceRestrictive(pt);
    return 0;
}

// ------------------------------ hooks -------------------------------------
static void __declspec(naked) HookMenuInj() {
    __asm {
        pushad
        call ComposeLabel
        mov ecx, offset g_label
        mov eax, ACTION_POWERS
        xor esi, esi
        mov edx, g_fnAddItem
        call edx
        popad
        push edx
        mov edx, g_progByte
        cmp byte ptr [edx], 3           // stolen: cmp byte [0x62e8c9],3
        pop edx                         // pop reg keeps flags intact
        push g_retMenuInj
        ret
    }
}

static void __declspec(naked) HookDispatch() {
    __asm {
        push edx
        mov edx, g_itemsOff4
        mov edi, dword ptr [eax + edx]  // stolen: edi = item action
        pop edx
        cmp edi, ACTION_POWERS
        jne back
        call TogglePowers
        push g_retHandled                      // case-0: rebuild + exit
        ret
    back:
        push g_retDispatch
        ret
    }
}

static void __declspec(naked) HookCmdFetch() {
    __asm {
        mov esi, dword ptr [ecx + 4]        // stolen: esi = command line
        mov dword ptr [ebp - 0x3fc], eax     // stolen
        pushad
        push ecx                             // entry
        push eax                             // slot index
        call ProcessCommand
        add esp, 8
        mov g_replay, eax
        popad
        mov esi, dword ptr [ecx + 4]        // pick up a repointed command
        cmp g_replay, 0
        jne replay
        push g_retCmdFetch
        ret
    replay:
        push g_retCmdFetchReplay
        ret
    }
}

// ------------------------- dinput8 forwarding -------------------------------
extern "C" {
typedef HRESULT (WINAPI *DI8C_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DI8C_t  g_realDI8C;
static HMODULE g_self;

// Resolve an export from a pristine on-disk image: another component (overlay)
// hooks the in-memory export of the real dinput8.dll and its stub re-enters us
// by name, so GetProcAddress would loop. Map the file as an image (separate
// copy, unpatched EAT) and take the RVA, then apply it to the loaded module.
static FARPROC CleanExport(HMODULE loaded, const char* path, const char* name) {
    HMODULE m = LoadLibraryExA(path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!m) return NULL;
    BYTE* b = (BYTE*)((DWORD)m & ~3u);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    FARPROC r = NULL;
    if (rva) {
        IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(b + rva);
        DWORD* names = (DWORD*)(b + ed->AddressOfNames);
        WORD*  ords  = (WORD*)(b + ed->AddressOfNameOrdinals);
        DWORD* funcs = (DWORD*)(b + ed->AddressOfFunctions);
        for (DWORD i = 0; i < ed->NumberOfNames; ++i)
            if (!strcmp((char*)(b + names[i]), name)) {
                r = (FARPROC)((BYTE*)loaded + funcs[ords[i]]);
                break;
            }
    }
    FreeLibrary(m);
    return r;
}

HRESULT WINAPI DirectInput8Create(
    HINSTANCE a, DWORD b, REFIID c, LPVOID* d, LPUNKNOWN e) {
    static volatile LONG depth = 0;
    LONG dp = InterlockedIncrement(&depth);
    if (dp <= 3)
        DiTrace("DI8C enter depth=%ld caller=%p self=%p real=%p\n",
                dp, _ReturnAddress(), g_self, g_realDI8C);
    if (dp > 2) { InterlockedDecrement(&depth); return (HRESULT)0x80004005L; }
    struct Leave { ~Leave() { InterlockedDecrement(&depth); } } leave;
    if (!g_realDI8C) {
        // LoadLibrary can return OUR OWN module when the loader matches by
        // base name — that made g_realDI8C point back at us and recurse to
        // a stack overflow. Reject any resolution that lands on this dll.
        char p[MAX_PATH];
        const char* cand[2];
        cand[0] = p;
        GetSystemDirectoryA(p, MAX_PATH);
        strcat(p, "\\dinput8.dll");
        static char p2[MAX_PATH];
        cand[1] = p2;
        GetWindowsDirectoryA(p2, MAX_PATH);
        strcat(p2, "\\SysWOW64\\dinput8.dll");
        for (int i = 0; i < 2 && !g_realDI8C; ++i) {
            HMODULE h = LoadLibraryA(cand[i]);
            FARPROC fp = h ? CleanExport(h, cand[i], "DirectInput8Create") : NULL;
            DiTrace("  cand[%d]=%s h=%p fp=%p ours=%p\n", i, cand[i], h, fp, &DirectInput8Create);
            if (!h || h == g_self) continue;
            if (fp && fp != (FARPROC)&DirectInput8Create)
                g_realDI8C = (DI8C_t)fp;
        }
        if (!g_realDI8C) return (HRESULT)0x80004005L;   // E_FAIL
    }
    return g_realDI8C(a, b, c, d, e);
}
}

// ------------------------- crash logger ------------------------------------
// On AV anywhere in the process, dump EIP + loaded modules to
// br1mod_crash.log so we can tell where it died.
static int SafeRead(const void* p, void* out, SIZE_T n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (!(mbi.State & MEM_COMMIT) || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    memcpy(out, p, n); return 1;
}

static LONG CALLBACK CrashLog(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    FILE* f = fopen("br1mod_crash.log", "w");
    if (!f) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    DWORD eip = c->Eip;
    fprintf(f, "AV at %08X  eip=%08X esp=%08X ebp=%08X\n",
            (DWORD)ep->ExceptionRecord->ExceptionAddress, eip, c->Esp, c->Ebp);
    DWORD* fr = (DWORD*)c->Ebp;
    for (int i = 0; i < 24 && fr; ++i) {
        DWORD w[2]; if (!SafeRead(fr, w, sizeof w)) break;
        fprintf(f, "  ret=%08X\n", w[1]);
        if (w[0] <= (DWORD)fr || w[0] - (DWORD)fr > 0x100000) break;
        fr = (DWORD*)w[0];
    }
    char* peb = (char*)__readfsdword(0x30);
    if (peb) {
        char* ldr = *(char**)(peb + 0x0C);
        LIST_ENTRY* head = (LIST_ENTRY*)(ldr + 0x0C);
        for (LIST_ENTRY* e = head->Flink; e != head; e = e->Flink) {
            char* mod = (char*)e;
            DWORD base = *(DWORD*)(mod + 0x18), size = *(DWORD*)(mod + 0x20);
            USHORT nlen = *(USHORT*)(mod + 0x2C);
            WCHAR* nbuf = *(WCHAR**)(mod + 0x30);
            char name[128] = {0};
            if (nbuf) {
                int n = nlen / 2; if (n > 120) n = 120;
                for (int i = 0; i < n; ++i)
                    name[i] = (nbuf[i] < 128) ? (char)nbuf[i] : '?';
            }
            fprintf(f, "%s mod %08X-%08X %s\n",
                    (eip >= base && eip < base + size) ? ">>>" : "   ",
                    base, base + size, name);
        }
    }
    fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------ patching ----------------------------------
// push imm32 + ret detour, only applied when the expected bytes match —
// refuses to touch the site if the exe differs from what we analyzed.
static int PatchChecked(DWORD site, const char* expected, int siteLen, void* target) {
    if (memcmp((void*)site, expected, siteLen)) {
        DiTrace("PATCH-SKIP %08X (bytes differ)\n", site);
        return 0;
    }
    DWORD old;
    if (!VirtualProtect((void*)site, siteLen, PAGE_EXECUTE_READWRITE, &old)) {
        DiTrace("PATCH-FAIL %08X vp\n", site);
        return 0;
    }
    BYTE* p = (BYTE*)site;
    p[0] = 0x68;                       // push imm32
    *(DWORD*)(p + 1) = (DWORD)target;
    p[5] = 0xC3;                       // ret
    for (int i = 6; i < siteLen; ++i) p[i] = 0x90;
    VirtualProtect((void*)site, siteLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, siteLen);
    DiTrace("PATCH-OK   %08X -> %08X\n", site, (DWORD)target);
    return 1;
}

static DWORD WINAPI Worker(LPVOID) {
    Sleep(400);
    g_exeBase    = (DWORD)GetModuleHandleA(NULL);
    g_fnLocalize = RVA(FN_LOCALIZE);
    g_fnAddItem  = RVA(FN_ADD_ITEM);
    g_progByte   = RVA(G_PROG_BYTE);
    g_itemsOff4  = RVA(G_ITEMS_OFF4);
    g_retMenuInj = RVA(RET_MENU_INJ);
    g_retDispatch= RVA(RET_DISPATCH);
    g_retHandled = RVA(RET_HANDLED);
    g_retCmdFetch= RVA(RET_CMD_FETCH);
    g_retCmdFetchReplay = RVA(RET_CMD_REPLAY);
    g_cmdModePtr = RVA(G_CMD_MODE);
    g_heroGlobA  = RVA(G_HERO_A);
    g_heroGlobB  = RVA(G_HERO_B);
    g_fnFindHero = RVA(FN_FIND_HERO);
    g_fnFlagGet  = RVA(FN_FLAG_GET);
    g_flagMgrPtr = RVA(G_FLAG_MGR);
    g_powers = GetPrivateProfileIntA("Powers", "all", 0, INI_FILE) ? 1 : 0;
    DiTrace("WORKER base=%08X powers=%d\n", g_exeBase, g_powers);
    // Expected-byte patterns carry absolute VAs that the loader rebases
    // (the disp32 fields), so they are built at runtime from g_exeBase.
    char expA[7] = { '\x80', '\x3d', 0,0,0,0, '\x03' };   // cmp byte [disp32],3
    memcpy(expA + 2, &g_progByte, 4);
    char expB[6] = { '\x8b', '\xb8', 0,0,0,0 };           // mov edi,[eax+disp32]
    memcpy(expB + 2, &g_itemsOff4, 4);
    PatchChecked(RVA(SITE_MENU_INJ), expA, 7, HookMenuInj);
    PatchChecked(RVA(SITE_DISPATCH), expB, 6, HookDispatch);
    // cmd fetch: mov esi,[ecx+4] (8B 71 04) + mov [ebp-0x3fc],eax
    // (89 85 04 FC FF FF) — no absolute addrs, plain byte check.
    PatchChecked(RVA(SITE_CMD_FETCH), "\x8b\x71\x04\x89\x85\x04\xfc\xff\xff", 9, HookCmdFetch);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        AddVectoredExceptionHandler(1, CrashLog);
        CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    }
    return TRUE;
}

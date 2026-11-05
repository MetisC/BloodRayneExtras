// br1mod — dinput8.dll proxy for BloodRayne: Terminal Cut (GOG)
// Adds rows at the top of the native New Game menu:
//   1) "All powers and combos: Yes/No" cycler, persisted to br1mod.ini
//   2-4) "Challenge Combat: Normal/Hard/Extreme" — loads the loose
//        world\challenge.msn arena (dev BOILERROOM geometry) and runs
//        timed nazi waves through the script-executor idle mailbox.
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
#define RET_MISSION_GO 0x4CD018   // push 3; callMenu; reset selection; epilogue
#define G_MISSION_NAME 0x1D27A00  // char[32] last queued mission name
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

// Actor lookup used by the isDead/createActor handlers: edi=name,
// ecx=[0x62ED00] actor manager, call 0x4D9420 -> record/actor or NULL.
// [actor+0xD4] is an engine "ping" field overwritten every tick on live
// actors — isDead writes a sentinel there and reads it back next pass.
#define FN_FIND_ACTOR  0x4D9420
#define G_ACTOR_MGR    0x62ED00
#define ACT_PING       0xD4
#define PING_MAGIC     0x78A123

#define ACTION_POWERS   0x160   // >=0x15d: outside native action range (<=0x15c)
#define ACTION_CH_N     0x161   // Challenge Combat: Normal
#define ACTION_CH_H     0x162   // Challenge Combat: Hard
#define ACTION_CH_E     0x163   // Challenge Combat: Extreme
#define ACTION_CH_B     0x164   // Boss Battle

#define INI_FILE       ".\\br1mod.ini"
#define MSN_CHALLENGE  "challenge.msn"
#define MSN_BOSSBATTLE "bossbattle.msn"

// ----------------------------- state --------------------------------------
static DWORD g_exeBase = 0;
static int   g_powers = 0;
static char  g_label[160];
static char  g_labelS[4][160];
static int   g_challenge = 0;  // 0 off, 1 armed (menu picked), 2 running in-level
static int   g_diff = 0;       // 0 Normal, 1 Hard, 2 Extreme
static int   g_mode = 0;       // 0 Challenge Combat, 1 Boss Battle
static void  DiTrace(const char* fmt, ...);
static int   g_autokill = 0;   // ini [Challenge] Autokill — test-only wave bleed

// Rebased values the naked hooks read as memory operands.
static DWORD g_fnLocalize, g_fnAddItem, g_progByte, g_itemsOff4;
static DWORD g_retMenuInj, g_retDispatch, g_retHandled, g_retCmdFetch, g_retCmdFetchReplay;
static DWORD g_retMissionGo, g_missionName, g_fnFindActor, g_actorMgrPtr;
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

static const char* LABEL_CH[4][6] = {
    { "Challenge Combat: Normal", "Desaf\xEDo: Normal", "Challenge Combat: Normal",
      "Challenge Combat: Normal", "Challenge Combat: Normal", "Challenge Combat: Normal" },
    { "Challenge Combat: Hard", "Desaf\xEDo: Dif\xED""cil", "Challenge Combat: Hard",
      "Challenge Combat: Hard", "Challenge Combat: Hard", "Challenge Combat: Hard" },
    { "Challenge Combat: Extreme", "Desaf\xEDo: Extremo", "Challenge Combat: Extreme",
      "Challenge Combat: Extreme", "Challenge Combat: Extreme", "Challenge Combat: Extreme" },
    { "Boss Battle", "Batalla con el jefe", "Boss Battle",
      "Boss Battle", "Boss Battle", "Boss Battle" },
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

static void ComposeLabelS(int d) {
    if (!g_langDetected) DetectLang();
    const char* lbl = ModLocalize(LABEL_CH[d][g_lang]);
    // One shared best time per mode — shown on every row of that mode.
    DWORD best = GetPrivateProfileIntA("Challenge",
                                       d == 3 ? "BestBoss" : "BestTime",
                                       0, INI_FILE);
    if (best)
        sprintf(g_labelS[d], "%s - %u:%02u", lbl, best / 60000, (best / 1000) % 60);
    else
        sprintf(g_labelS[d], "%s", lbl);
}

// -------------------------- appearance swap --------------------------------
// The loose mission files are ours, so model swaps happen at file level:
// when a menu action fires we rewrite the MSN's modelName lines per the
// [Appearance] ini section before the engine loads it. Class compatibility
// (from the native MSN corpus): CSoldier models share the soldier skeleton
// (SOLDIER, SS_SOLDIER, SS_OFFICER, GGG_OFFICER, GGG_ELITE, GGG_HAZMAT,
// GGG_JETPACK, STALKER, ARGSLAVE, ARGSLAVESHIRT, NAZI_BARROCK); the boss may
// additionally become a CNPC actor with an NPC-skeleton model (BUTCHERESS,
// KRISTINE, KAZI, GMONEY, HICKS, OLDMAN, BATMITE, RAYNE) via BossClass=CNPC.
static const char* NPC_MODELS[] = {
    "BUTCHERESS.DFM", "KRISTINE.DFM", "KAZI.DFM", "GMONEY.DFM",
    "HICKS.DFM", "OLDMAN.DFM", "BATMITE.DFM", "RAYNE.DFM",
    "VAMPETTE.DFM", "VAMP.DFM", NULL
};

// Rewrite "// modelName" values inside blocks whose actor name starts with
// a prefix. Blocks look like:  { CClass "Name" ... "MODEL"    // modelName }
static int MsnSwap(char* txt, const char* namePat, const char* model) {
    int n = 0, plen = strlen(namePat);
    char* p = txt;
    while ((p = strstr(p, "{ C"))) {
        // block header: { CClass "Name"
        char* q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!q2) break;
        char* close = strstr(q2, "} C");
        char* m = strstr(q2, "// modelName");
        if (!m || (close && close < m)) { p = q2 + 1; continue; }
        if (q2 - q1 - 1 < plen || _strnicmp(q1 + 1, namePat, plen)) {
            p = close ? close + 1 : m + 12; continue;
        }
        // replace the quoted model on the modelName line
        char* s = m;
        while (s > p && *s != '\n') --s;
        char* m1 = strchr(s, '"'), *m2 = m1 ? strchr(m1 + 1, '"') : NULL;
        if (!m2 || m1 > m) { p = m + 12; continue; }
        // Move the tail FIRST (it may overlap the span we are replacing),
        // then write the new model — sprintf would otherwise clobber m2+1
        // before its strlen runs when the new name is longer.
        long tail = strlen(m2 + 1) + 1;
        int mv = (int)strlen(model) + 2;
        memmove(m1 + mv, m2 + 1, tail);
        memcpy(m1, "\"", 1);
        memcpy(m1 + 1, model, mv - 2);
        memcpy(m1 + mv - 1, "\"", 1);
        p = m1 + mv; ++n;
    }
    return n;
}

// Set the class token on Boss1's declaration-list line. The decl line is
// the FIRST "Boss1" mention in the file:  <indent>ClassName "Boss1"
static void MsnSetBossDecl(char* txt, const char* newCls) {
    char* d0 = strstr(txt, "\"Boss1\"");
    if (!d0) return;
    // find the class token = last non-space run before the quote on the line
    char* ls = d0;
    while (ls > txt && ls[-1] != '\n') --ls;
    char* ce = ls;
    while (ce < d0 && *ce != ' ' && *ce != '\t') ++ce;
    char old[48];
    int olen = ce - ls;
    if (olen <= 0 || olen >= 48) return;
    memcpy(old, ls, olen); old[olen] = 0;
    int nlen = strlen(newCls);
    // splice: [ls..ce) -> newCls
    memmove(ls + nlen, ce, strlen(ce) + 1);
    memcpy(ls, newCls, nlen);
}

// Rewrite the whole "{ <anything> \"Boss1\" ... } Class \"Boss1\"" block
// with a provided template, fixing the declaration line too.
// kind: 0=CSoldier(model), 1=CNPC(model), 2=CMainCharacter(heroType)
static int MsnBossBlock(char* txt, int kind, const char* arg) {
    static const char* CLSS[] = { "CSoldier", "CNPC", "CMainCharacter" };
    MsnSetBossDecl(txt, CLSS[kind]);
    char* start = strstr(txt, "{ CSoldier \"Boss1\"");
    if (!start) start = strstr(txt, "{ CNPC \"Boss1\"");
    if (!start) start = strstr(txt, "{ CMainCharacter \"Boss1\"");
    if (!start) return 0;
    char* end = strstr(start, "} C");
    if (!end) return 0;
    end = strchr(end, '\n');
    if (!end) return 0;
    ++end;
    char blk[4096];
    if (kind == 2) {
        // Dhampir boss: CMainCharacter with heroType. Field layout mirrors
        // the native MentorBoss0 block (DE_C2START.MSN). arg = heroType num.
        int ht = atoi(arg);
        sprintf(blk,
            "{ CMainCharacter \"Boss1\"\n"
            "\t0,222.287,0.0,361.000    // location\n"
            "\t0,0,0    // orient\n"
            "\t0    // enabled\n"
            "\t\"GB1\"    // group\n"
            "\t0    // selfIlluminated\n"
            "\t0    // attachCount\n"
            "\t1200    // hitPoints\n"
            "\t0    // numCarryHands\n"
            "\t{ // clothList\n"
            "\t\t0 // clothCount\n"
            "\t}\n"
            "\t1    // sizeScale\n"
            "\t1200    // maxHitPoints\n"
            "\t0    // spectralFlag\n"
            "\t0    // invulnerableFlag\n"
            "\t0    // etheralFlag\n"
            "\t\"(none)\"\t\t// scriptWalkDest\n"
            "\t3    // scriptWalkDist\n"
            "\t1e+030    // scriptRunDist\n"
            "\t-1    // scriptWalkTimer\n"
            "\t0    // scriptWalkState\n"
            "\t\"(none)\"\t\t// scriptDoorToOpen\n"
            "\t\"(none)\"\t\t// whoIsControllingMe\n"
            "\t1    // canInfest\n"
            "\t{ // motion state\n"
            "\t\t\"standalert\",0 // current motion name, curFrameNumber\n"
            "\t}\n"
            "\t{ // partStatus\n"
            "\t\t23\n"
            "\t\t1 0 0 1 1 0 0 1 1 0 1 1 1 1 1 1 1 1 1 1 1 1 1 \n"
            "\t}\n"
            "\t458848    // controlType\n"
            "\t%d    // heroType\n"
            "} CMainCharacter \"Boss1\"\n", ht);
    } else if (kind == 1) {
        sprintf(blk,
            "{ CNPC \"Boss1\"\n"
            "\t0,222.287,0.0,361.000    // location\n"
            "\t0,0,0    // orient\n"
            "\t0    // enabled\n"
            "\t\"GB1\"    // group\n"
            "\t0    // selfIlluminated\n"
            "\t0    // attachCount\n"
            "\t1200    // hitPoints\n"
            "\t0    // numCarryHands\n"
            "\t{ // clothList\n"
            "\t\t0 // clothCount\n"
            "\t}\n"
            "\t1.15    // sizeScale\n"
            "\t1200    // maxHitPoints\n"
            "\t0    // spectralFlag\n"
            "\t0    // invulnerableFlag\n"
            "\t0    // etheralFlag\n"
            "\t\"(none)\"\t\t// scriptWalkDest\n"
            "\t3    // scriptWalkDist\n"
            "\t1e+030    // scriptRunDist\n"
            "\t-1    // scriptWalkTimer\n"
            "\t0    // scriptWalkState\n"
            "\t\"(none)\"\t\t// scriptDoorToOpen\n"
            "\t\"(none)\"\t\t// whoIsControllingMe\n"
            "\t1    // canInfest\n"
            "\t0.9    // speed\n"
            "\t\"%s\"    // modelName\n"
            "\t{ // motion state\n"
            "\t\t\"standambient\",0 // current motion name, curFrameNumber\n"
            "\t}\n"
            "\t0    // postMode\n"
            "\t\"(none)\"\t\t// ourPost\n"
            "\t0    // holdPosFlag\n"
            "\t\"none\"    // holdPosCond\n"
            "} CNPC \"Boss1\"\n", arg);
    } else {
        // restore the CSoldier template (mirrors gen_boss.py output)
        sprintf(blk,
            "{ CSoldier \"Boss1\"\n"
            "\t0,222.287,0.0,361.000    // location\n"
            "\t0,0,0    // orient\n"
            "\t0    // enabled\n"
            "\t\"GB1\"    // group\n"
            "\t0    // selfIlluminated\n"
            "\t0    // attachCount\n"
            "\t1200    // hitPoints\n"
            "\t1    // numCarryHands\n"
            "\t1    // carry hand\n"
            "\t\"GunB1\"\t\t// carry actor\n"
            "\t0.307564,-0.132776,0.130445    // carry pos\n"
            "\t0.0158968,1.42652,1.55902    // carry orient\n"
            "\t{ // clothList\n"
            "\t\t0 // clothCount\n"
            "\t}\n"
            "\t1    // sizeScale\n"
            "\t1200    // maxHitPoints\n"
            "\t0    // spectralFlag\n"
            "\t0    // invulnerableFlag\n"
            "\t0    // etheralFlag\n"
            "\t\"(none)\"\t\t// scriptWalkDest\n"
            "\t3    // scriptWalkDist\n"
            "\t1e+030    // scriptRunDist\n"
            "\t-1    // scriptWalkTimer\n"
            "\t0    // scriptWalkState\n"
            "\t\"(none)\"\t\t// scriptDoorToOpen\n"
            "\t\"(none)\"\t\t// whoIsControllingMe\n"
            "\t1    // canInfest\n"
            "\t0.99122    // speed\n"
            "\t0    // poolMe\n"
            "\t1    // randomizeMe\n"
            "\t\"(none)\"\t\t// scriptVictim\n"
            "\t\"\"    // patrolNamePattern\n"
            "\t200    // guardDistance\n"
            "\t150    // victimHeight\n"
            "\t\"(none)\"\t\t// victim\n"
            "\t10    // smellDist\n"
            "\t1    // loquaciousness\n"
            "\t0.914121    // evasiveness\n"
            "\t0.77485    // courage\n"
            "\t0.57181    // loyalty\n"
            "\t0.627583    // marksmanship\n"
            "\t0.5    // awareness\n"
            "\t0.5    // perception\n"
            "\t\"(none)\"\t\t// ourPost\n"
            "\t0.732719    // intelligence\n"
            "\t0.716666    // aggression\n"
            "\t0.686956    // valor\n"
            "\t0    // postModeFlag\n"
            "\t\"none\"    // postModeCond\n"
            "\t0    // holdPosFlag\n"
            "\t\"none\"    // holdPosCond\n"
            "\t-1    // largestPatrolPointIndex\n"
            "\t-1    // currentPatrolPointIndex\n"
            "\t\"(none)\"\t\t// nextPatrolPoint\n"
            "\t2    // spawnType\n"
            "\t\"%s\"    // modelName\n"
            "\t{ // motion state\n"
            "\t\t\"standambient\",0 // current motion name, curFrameNumber\n"
            "\t}\n"
            "\t1\n"
            "\t0\n"
            "\t2\n"
            "} CSoldier \"Boss1\"\n", arg);
    }
    size_t rest = strlen(end) + 1;
    int blen = strlen(blk);
    memmove(start + blen, end, rest);
    memcpy(start, blk, blen);
    return 1;
}

static void ApplyAppearance() {
    char sold[64], eli[64], boss[64], bcls[16];
    GetPrivateProfileStringA("Appearance", "Soldier", "", sold, 64, INI_FILE);
    GetPrivateProfileStringA("Appearance", "Elite",   "", eli,  64, INI_FILE);
    GetPrivateProfileStringA("Appearance", "Boss",    "", boss, 64, INI_FILE);
    GetPrivateProfileStringA("Appearance", "BossClass", "", bcls, 16, INI_FILE);
    if (!*sold && !*eli && !*boss && !*bcls) return;

    const char* msn = g_mode == 1 ? "world\\bossbattle.msn" : "world\\challenge.msn";
    FILE* f = fopen(msn, "rb");
    if (!f) { DiTrace("APPEAR open fail %s\n", msn); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* txt = (char*)malloc(sz + 0x8000);
    fread(txt, 1, sz, f); fclose(f);
    txt[sz] = 0;

    int n = 0;
    if (*sold) n += MsnSwap(txt, "SoldierS", sold);
    if (*eli)  n += MsnSwap(txt, "EliteS",  eli);
    if (g_mode == 1) {
        // Boss selector: a bare hero name picks the CMainCharacter class
        // with that heroType (full dhampir AI + own model/animations);
        // a .DFM name keeps the CSoldier block or converts to CNPC for
        // NPC-skeleton models.
        static const char* HEROS[] = { "Rayne", "Mynce", "Butcheress", "RayneLA" };
        static const int    HTS[]   = { 1, 2, 3, 4 };
        int kind = -1; const char* barg = boss;
        char htbuf[8];
        if (*bcls && !_stricmp(bcls, "CNPC")) kind = 1;
        for (int i = 0; *boss && kind < 0 && HEROS[i]; ++i)
            if (!_stricmp(boss, HEROS[i])) {
                kind = 2;
                sprintf(htbuf, "%d", HTS[i]);
                barg = htbuf;
            }
        if (kind < 0 && *boss) {
            kind = 0;
            for (int i = 0; NPC_MODELS[i]; ++i)
                if (!_stricmp(boss, NPC_MODELS[i])) { kind = 1; break; }
        }
        if (kind >= 0) {
            if (!*boss) { strcpy(boss, "NAZI_BARROCK.DFM"); barg = boss; }
            n += MsnBossBlock(txt, kind, barg);
        }
    }
    f = fopen(msn, "wb");
    if (f) { fwrite(txt, 1, strlen(txt), f); fclose(f); }
    free(txt);
    DiTrace("APPEAR %s swaps=%d (sold=%s eli=%s boss=%s cls=%s)\n",
            msn, n, sold, eli, boss, bcls);
}

// Menu action: queue the challenge mission through the same path the
// native Act rows use, then RET_MISSION_GO runs callMenu(3)+cleanup.
// d==3 selects the Boss Battle arena instead.
static void StartChallenge(int d) {
    if (d == 3) {
        strcpy((char*)g_missionName, MSN_BOSSBATTLE);
        g_mode = 1;
        g_diff = 0;
    } else {
        strcpy((char*)g_missionName, MSN_CHALLENGE);
        g_mode = 0;
        g_diff = d;
    }
    g_challenge = 1;
    g_autokill = GetPrivateProfileIntA("Challenge", "Autokill", 0, INI_FILE);
    ApplyAppearance();
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

// (entry, original text) pairs whose slot-0 injection already completed —
// keeps the powers setup from re-firing when slot 0 sits inside the
// per-frame mainloop (AR_MB_BOILER's slot 0 is literally `idle`).
static struct { BYTE* e; char* t; } g_done[16];
static int DoneFind(BYTE* e, char* t) {
    for (int i = 0; i < 16; ++i) if (g_done[i].e == e && g_done[i].t == t) return 1;
    return 0;
}
static void DoneAdd(BYTE* e, char* t) {
    for (int i = 0; i < 16; ++i) if (!g_done[i].e) { g_done[i].e = e; g_done[i].t = t; return; }
}

// ---------------- survival mode: command queue + idle mailbox -------------
// The level script's mainloop ends every frame on an `idle` command (it is
// the executor's yield point). While our queue is non-empty we repoint an
// idle slot at the next queued command; on the following pass the same slot
// swaps to the next queued command, and once drained the original `idle`
// text is restored. Net effect: the whole queue executes inside one frame,
// then the loop yields normally — zero persistent script changes.
#define Q_MAX 96
static char g_q[Q_MAX][64];
static int  g_qHead, g_qTail;

// Command lives in our queue ring -> it is ours, never neuter it.
static int IsOurCmd(const char* t) {
    return t >= g_q[0] && t < g_q[Q_MAX - 1] + 64;
}

// Verbs that drive cinematics / native plot / native spawns. While survival
// runs they are repointed at "idle" so the slot becomes a harmless yield —
// and doubles as an extra mailbox. Our own queued commands pass through.
static int NeuterCmd(const char* t) {
    static const char* N[] = {
        "letterbox", "setcamerapathactor", "setcameramodeorbit",
        "setcameramodefollow", "setskiplabel", "dbstartsay",
        "dbtimeddisplay", "wait(", "waitfor(", "warpto($", "warpto( $", "walkto(",
        "raise(", "fade(", "addobjective", "setobjectiveactor",
        "addcharacterhealthbar", "removecharacterhealthbar",
        "slammodeltomotion", "setmodelstate", "lookat(", "snapto",
        "setvictim", "infestvictim", "settimer(", "createactor",
        "enablepathpoint", "hurtcharacter", "controlcharacter",
        "chaintomission", "endmission", "gettriggeractor",
    };
    for (unsigned i = 0; i < sizeof(N) / sizeof(N[0]); ++i)
        if (!_strnicmp(t, N[i], strlen(N[i]))) return 1;
    return 0;
}

static int QEmpty() { return g_qHead == g_qTail; }
static void QPush(const char* s) {
    if (g_qTail - g_qHead >= Q_MAX) return;
    strncpy(g_q[g_qTail % Q_MAX], s, 63);
    g_q[g_qTail % Q_MAX][63] = 0;
    ++g_qTail;
}
static char* QPop() {
    if (QEmpty()) return NULL;
    return g_q[(g_qHead++) % Q_MAX];
}

static char g_idleStr[] = "idle";

// idle slots currently pointing at a queued command.
static struct { BYTE* entry; char* orig; } g_mbox[12];
static int MboxFind(BYTE* e) {
    for (int i = 0; i < 12; ++i) if (g_mbox[i].entry == e) return i;
    return -1;
}
static void MboxSet(BYTE* e, char* orig, char* cmd) {
    int i = MboxFind(e);
    if (i < 0) { for (i = 0; i < 12 && g_mbox[i].entry; ++i); if (i == 12) return; }
    g_mbox[i].entry = e;
    if (orig) g_mbox[i].orig = orig;
    *(char**)(e + 4) = cmd;
}

// ------------------------- actor pool / wave logic ------------------------
// world\challenge.msn (loose file, BOILERROOM.PST geometry) carries our own
// actor pool: SoldierS1..40 (SOLDIER.DFM, MP34), EliteS1..8 (GGG_ELITE.DFM,
// MG42), each carrying GunS*/GunE* with matching index, plus WPS1..14 and
// WPHero waypoints and floor gun pickups.
#define N_SOLDIERS 40
#define N_ELITES   8
#define N_ELITES_BOSS 7    // bossbattle.msn: EliteS8 is rebadged as Boss1
#define N_WPS      12

// Wave campaign per mode/difficulty. Time runs from the first spawn until
// the last wave is cleared; one best time is kept per mode in the ini.
static const int CH_WAVES[2][3] = { { 8, 10, 10 }, { 6, 6, 6 } };
#define NWAVES() CH_WAVES[g_mode][g_diff]

// Boss Battle (mode 1) fixed 6-wave gauntlet — officers, then sub-officers
// joining in, ending with the boss + escort.
static const int B_SZ[6] = { 4, 5, 5, 6, 6, 2 };
static const int B_EL[6] = { 0, 0, 1, 2, 3, 2 };

static int WaveSize(int w) {   // w is 1-based
    if (g_mode == 1) return B_SZ[min(w, 6) - 1];
    if (g_diff == 2) return 11;              // Extreme: brute squads
    return min(3 + w + g_diff, 8 + 2 * g_diff);
}
static int WaveElites(int w) {
    if (g_mode == 1) return B_EL[min(w, 6) - 1];
    if (g_diff == 0) return 0;               // Normal: plain soldiers only
    if (g_diff == 2) return 3;               // Extreme: elites every wave
    return (w % 3 == 0) ? (w >= 9 ? 2 : 1) : 0;  // Hard: waves 3,6,9
}

#define SP_MAX 20
static char g_spawned[SP_MAX][32];
static int  g_nSpawned;
static int  g_wave;
static int  g_wstate;      // 0 idle, 1 spawning, 2 settle, 3 fight, 4 done
static int  g_settle;
static int  g_seenAlive;
static int  g_stallGuard;
static DWORD g_t0;
static int  g_soldierRot, g_eliteRot, g_wpRot;
static int  g_dbgAlive;   // bounded debug logging for ActorAlive decisions
static int  g_dbgInj;     // bounded debug logging for mailbox injections

// Actor manager lookup (thiscall on [0x62ED00]): record or NULL.
static DWORD FindActor(const char* name) {
    DWORD mgr = *(DWORD*)g_actorMgrPtr, fn = g_fnFindActor, r;
    if (!mgr) return 0;
    __asm { mov ecx, mgr
            push name
            call fn
            mov r, eax }
    return r;
}

// Actor liveness, mirroring the engine's own isDead handler at 0x45CDFB:
// seed the ping sentinel; if the engine refreshed the field it is ticking
// (alive); if the sentinel survived, fall back to the record's liveness
// vmethod at [vtable+0xd8] (thiscall) — return>1 means alive. 0x45CE38.
// Actor liveness for wave tracking. +0xD4 is the engine's per-frame
// heartbeat: ticking records are rewritten to 0x6F3874E each frame,
// while deactivated/killed actors keep the dormant marker 0x78A123.
// The native isDead (0x45CDEB) additionally falls back to the liveness
// vmethod (vtable+0xD8), but that call returns stale >1 verdicts for
// records that were activated and deactivated again — for wave
// purposes the heartbeat marker alone is authoritative.
static int ActorAlive(int i) {
    DWORD a = FindActor(g_spawned[i]);
    if (!a) return 0;
    DWORD d4;
    __try { d4 = *(DWORD*)(a + ACT_PING); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return (d4 != PING_MAGIC) ? 1 : 0;
}

// Same liveness check for an arbitrary record (hero gate).
static int RecordLive(DWORD a) {
    DWORD r = 0;
    if (!a) return 0;
    __try {
        DWORD vt = *(DWORD*)a, fn = *(DWORD*)(vt + 0xd8);
        __asm { mov ecx, a
                call fn
                mov r, eax }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return (r > 1) ? 1 : 0;
}

// SoldierS<n> -> group "GS<n>", EliteS<n> -> "GE<n>", Boss<n> -> "GB<n>"
// (MSN group field: each soldier and its carried gun share one group,
// native pattern).
static const char* GrpFor(const char* n) {
    static char g[32];
    if      (!_strnicmp(n, "SoldierS", 8)) sprintf(g, "GS%s", n + 8);
    else if (!_strnicmp(n, "EliteS", 6))   sprintf(g, "GE%s", n + 6);
    else if (!_strnicmp(n, "Boss", 4))     sprintf(g, "GB%s", n + 4);
    else return NULL;
    return g;
}

static void QueueSpawn(const char* name) {
    char b[64], wp[16];
    // createActorGroup enables soldier + carried gun in one shot — the same
    // path native MSNs use for reinforcement squads.
    // NB: the runtime mailbox executor has INVERTED semantics for the
    // actor commands — deleteActor(x)/deleteActorGroup(g) ACTIVATE the
    // record (arg=0 at 0x4D9620), while createActor*/createActorGroup*
    // deactivate it (arg=1). Verified against the disassembled executor.
    const char* g = GrpFor(name);
    if (g) { sprintf(b, "deleteActorGroup(%s)", g); QPush(b); }
    else   { sprintf(b, "deleteActor(%s)", name);   QPush(b); }
    sprintf(wp, "WPS%d", (g_wpRot++ % N_WPS) + 1);
    sprintf(b, "warpTo(%s,%s)", name, wp);  QPush(b);
    if (g_nSpawned < SP_MAX) {
        strncpy(g_spawned[g_nSpawned], name, 31);
        g_spawned[g_nSpawned][31] = 0;
        ++g_nSpawned;
    }
}

static void QueueWave(int w) {
    char n[24];
    g_nSpawned = 0;
    int cnt = WaveSize(w), eli = WaveElites(w);
    int nelites = (g_mode == 1) ? N_ELITES_BOSS : N_ELITES;
    for (int i = 0; i < cnt; ++i) {
        sprintf(n, "SoldierS%d", (g_soldierRot++ % N_SOLDIERS) + 1);
        QueueSpawn(n);
    }
    for (int i = 0; i < eli; ++i) {
        sprintf(n, "EliteS%d", (g_eliteRot++ % nelites) + 1);
        QueueSpawn(n);
    }
    // Boss Battle finale: the boss drops in on the last wave.
    if (g_mode == 1 && w == NWAVES()) QueueSpawn("Boss1");
    DiTrace("CHAL wave %d/%d queued (%d+%d actors, diff=%d mode=%d)\n",
            w, NWAVES(), cnt, eli, g_diff, g_mode);
}

// HUD text via the engine's own banner printer 0x4715c0 — the same call
// the cheat handler uses for "Health restored." Signature:
//   thiscall/stdcall ecx=gameObj([0x62d6d0]), (text, dur, scale, font)
// Copies text into gameObj+0x1FB4, duration into +0x20B4 (-1 = auto
// from strlen), scale into +0x20B8, font into +0x20BC. ret 0x10.
static void ShowHud(const char* text) {
    DWORD go = *(DWORD*)RVA(0x62d6d0), fn = RVA(0x4715c0);
    if (!go || !text || !*text) return;
    float dur = -1.0f, sc = 0.9f;
    DWORD font = 0;
    __try {
        __asm {
            push font
            push sc
            push dur
            push text
            mov ecx, go
            call fn }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Persist the finish time if it beats the stored record. One record per
// mode — difficulty is a selection, not a separate challenge.
static void RecordTime(DWORD ms) {
    const char* key = g_mode == 1 ? "BestBoss" : "BestTime";
    DWORD best = GetPrivateProfileIntA("Challenge", key, 0, INI_FILE);
    if (!best || ms < best) {
        char b[16]; sprintf(b, "%u", ms);
        WritePrivateProfileStringA("Challenge", key, b, INI_FILE);
        best = ms;
    }
    {
        char b[64];
        sprintf(b, "COMPLETADO!  %u:%02u", ms / 60000, (ms / 1000) % 60);
        ShowHud(b);
    }
    DiTrace("CHAL finished mode=%d diff=%d time=%.1fs best=%.1fs\n",
            g_mode, g_diff, ms / 1000.0, best / 1000.0);
}

// Runs once per idle fetch (~once per frame) while the challenge is active.
// The wave state machine never counts deaths while the spawn queue is still
// draining — that race is what made earlier builds flood the arena.
static void WaveTick() {
    // Mission buffer must still match the armed arena — cancels if the
    // player left for another mission/menu.
    const char* want = g_mode == 1 ? MSN_BOSSBATTLE : MSN_CHALLENGE;
    if (_strnicmp((char*)g_missionName, want, strlen(want))) {
        g_challenge = 0;
        g_qHead = g_qTail = 0;
        for (int i = 0; i < 12; ++i) if (g_mbox[i].entry) {
            *(char**)(g_mbox[i].entry + 4) = g_mbox[i].orig;
            g_mbox[i].entry = NULL;
        }
        return;
    }
    // Test-only keep-alive: god flag (IWORKFORTRI) plus health and
    // bloodlust top-ups — starvation kills even under god. heroObj is at
    // [0x1c4aecc]; +0x584 is HP (GIVEMEFAITH), +0x5034 is the bloodlust
    // meter capped at 30.0f (IMRAGING path through 0x4a7850).
    if (g_autokill)
        __try {
            DWORD* go = *(DWORD**)RVA(0x62d6d0);
            if (go) *(DWORD*)((BYTE*)go + 0x294) = 1;
            DWORD* ho = *(DWORD**)RVA(0x1c4aecc);
            if (ho) {
                *(float*)((BYTE*)ho + 0x584)  = 100.0f;
                *(float*)((BYTE*)ho + 0x5034) = 30.0f;
            }
            static DWORD lastKal;
            if (GetTickCount() - lastKal > 10000) {
                lastKal = GetTickCount();
                DiTrace("  kal god=%d hp=%.1f lust=%.1f\n",
                        go ? (int)*(DWORD*)((BYTE*)go + 0x294) : -1,
                        ho ? *(float*)((BYTE*)ho + 0x584) : -1.0f,
                        ho ? *(float*)((BYTE*)ho + 0x5034) : -1.0f);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    switch (g_wstate) {
    case 0:                                   // arm: hero warp + wave 1
        // Wait for the hero to actually spawn. heroGlobA points at the
        // "MainCharacter0" record from mission load, but its vitals only get
        // populated once the CHeroPlaceholder instantiates the actor —
        // record+0x64 is 100 (HP) in play and 0 before. Require nonzero.
        {
            DWORD a = *(DWORD*)g_heroGlobA;
            DWORD hp = 0;
            if (a) { __try { hp = *(DWORD*)(a + 0x64); }
                     __except (EXCEPTION_EXECUTE_HANDLER) { hp = 0; } }
            if (!hp) return;
        }
        QPush("warpTo(MainCharacter0,WPHero)");
        g_soldierRot = g_eliteRot = g_wpRot = 0;
        g_wave = 1;
        QueueWave(1);
        g_wstate = 1;
        g_t0 = GetTickCount();
        return;
    case 1:                                   // spawning: wait queue drain
        if (QEmpty()) { g_wstate = 2; g_settle = 120; }
        return;
    case 2:                                   // settle: let spawns materialize
        if (--g_settle <= 0) {
            g_wstate = 3; g_seenAlive = 0; g_stallGuard = 3600;
            {
                char b[48];
                sprintf(b, "OLEADA %d/%d", g_wave, NWAVES());
                ShowHud(b);
            }
            DiTrace("CHAL wave %d enter fight\n", g_wave);
            // Test-only control probes (autokill builds): actors NOT in
            // this wave — tells us whether spawnType=2 kept the pool
            // dormant at load.
            if (g_autokill) {
                static const char* C[] = { "SoldierS5", "SoldierS20", "EliteS1", "MainCharacter0" };
                for (int i = 0; i < 4; ++i) {
                    DWORD a = FindActor(C[i]);
                    if (!a) { DiTrace("  ctrl %s rec=NULL\n", C[i]); continue; }
                    DWORD d4 = 0; __try { d4 = *(DWORD*)(a + ACT_PING); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    DWORD r = 0xFFFFFFFF;
                    __try {
                        DWORD vt = *(DWORD*)a, fn = *(DWORD*)(vt + 0xd8);
                        __asm { mov ecx, a
                                call fn
                                mov r, eax }
                    } __except (EXCEPTION_EXECUTE_HANDLER) {}
                    DiTrace("  ctrl %s rec=%p d4=%08X vcall=%08X\n", C[i], a, d4, r);
                }
            }
        }
        return;
    case 3: {                                 // fight: count live actors
        int alive = 0, aliveIdx[SP_MAX];
        for (int i = 0; i < g_nSpawned; ++i)
            if (ActorAlive(i)) aliveIdx[alive++] = i;
        if (alive) {
            if (!g_seenAlive)
                DiTrace("CHAL wave %d alive=%d\n", g_wave, alive);
            g_seenAlive = 1;
            // Test hook (ini [Challenge] Autokill=1): bleeds one live actor
            // every ~1.5s so wave progression can be verified unattended.
            // Never enabled for real play.
            if (g_autokill) {
                static DWORD lastAl;
                if (GetTickCount() - lastAl > 10000) {
                    lastAl = GetTickCount();
                    char nm[SP_MAX * 34]; int o = 0;
                    for (int i = 0; i < alive; ++i)
                        o += sprintf(nm + o, "%s ", g_spawned[aliveIdx[i]]);
                    DiTrace("  still-alive w%d: %s\n", g_wave, nm);
                }
                // Queue one deactivate per live actor — clears the wave
                // fast so an unattended test run exposes Rayne as little
                // as possible.
                for (int i = 0; i < alive; ++i) {
                    char b[64];
                    sprintf(b, "createActor(%s)", g_spawned[aliveIdx[i]]);
                    QPush(b);
                }
            }
            return;
        }
        if (!g_seenAlive && --g_stallGuard > 0) return;
        if (!g_seenAlive)
            DiTrace("CHAL wave %d stall (no live actors seen)\n", g_wave);
        if (g_wave >= NWAVES()) {
            RecordTime(GetTickCount() - g_t0);
            g_wstate = 4;
            return;
        }
        ++g_wave;
        QueueWave(g_wave);
        g_wstate = 1;
        return;
    }
    }
}

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
    char** pt = (char**)(entry + 4);
    char* t = *pt;
    int mode = *(int*)g_cmdModePtr;
    int again = (g_prevEntry == entry && g_prevIdx == idx);
    g_prevEntry = entry; g_prevIdx = idx;
    if (!t) return 0;

    // --- idle mailbox: this entry currently runs a queued command ---
    {
        int m = MboxFind(entry);
        if (m >= 0) {
            // Always restore the original text: one queued command per
            // executor call, then the real `idle` yields again. Draining
            // the whole queue in one call trips the script watchdog
            // ("infinite loop detected").
            *(pt) = g_mbox[m].orig;
            g_mbox[m].entry = NULL;
            t = *pt;
            ReplaceRestrictive(pt);
            return 0;
        }
    }

    // --- challenge: arm on the first real-run slot-0 after the menu pick ---
    if (g_challenge == 1 && idx == 0 && mode == 0 && !again) {
        g_challenge = 2;
        g_wstate = 0;
        DoneAdd(entry, t);                    // powers come via the queue here
        if (g_powers)
            for (int i = 0; i < N_INJ; ++i) QPush(g_inj[i]);
        DiTrace("CHAL armed mode=%d diff=%d entry=%p first=[%s]\n",
                g_mode, g_diff, entry, t);
    }

    // --- challenge: native cinematics/plot/spawns become plain yields ---
    if (g_challenge == 2 && !IsOurCmd(t) && NeuterCmd(t)) {
        *pt = g_idleStr;
        t = *pt;
    }

    // --- challenge: every live `idle` = one frame tick + drain one cmd ---
    if (g_challenge == 2 && !strcmp(t, "idle")) {
        WaveTick();
        char* nxt = QPop();
        if (nxt) {
            if (g_autokill && g_dbgInj < 40) {
                ++g_dbgInj;
                DiTrace("  inject mode=%d [%s]\n", mode, nxt);
            }
            MboxSet(entry, t, nxt);
            return 0;
        }
    }

    if (!g_powers) return 0;
    if (idx == 0 && mode == 0) {
        if (MboxFind(entry) >= 0 || DoneFind(entry, t)) goto noinject;
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
            DoneAdd(entry, o);
            if (g_logLeft > 0) { --g_logLeft; LogHero("after inject"); }
        }
    }
noinject:
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
        push 0
        call ComposeLabelS
        add esp, 4
        mov ecx, offset g_labelS
        mov eax, ACTION_CH_N
        xor esi, esi
        mov edx, g_fnAddItem
        call edx
        push 1
        call ComposeLabelS
        add esp, 4
        mov ecx, offset g_labelS + 160
        mov eax, ACTION_CH_H
        xor esi, esi
        mov edx, g_fnAddItem
        call edx
        push 2
        call ComposeLabelS
        add esp, 4
        mov ecx, offset g_labelS + 320
        mov eax, ACTION_CH_E
        xor esi, esi
        mov edx, g_fnAddItem
        call edx
        push 3
        call ComposeLabelS
        add esp, 4
        mov ecx, offset g_labelS + 480
        mov eax, ACTION_CH_B
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
        jne trychal
        call TogglePowers
        push g_retHandled                      // case-0: rebuild + exit
        ret
    trychal:
        cmp edi, ACTION_CH_N
        jne tryhard
        push 0
        call StartChallenge                    // write msn name + arm
        add esp, 4
        push g_retMissionGo                    // native tail: callMenu(3)...
        ret
    tryhard:
        cmp edi, ACTION_CH_H
        jne tryext
        push 1
        call StartChallenge
        add esp, 4
        push g_retMissionGo
        ret
    tryext:
        cmp edi, ACTION_CH_E
        jne tryboss
        push 2
        call StartChallenge
        add esp, 4
        push g_retMissionGo
        ret
    tryboss:
        cmp edi, ACTION_CH_B
        jne back
        push 3
        call StartChallenge
        add esp, 4
        push g_retMissionGo
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

// ---------------------- keyboard injection (test channel) ------------------
// Session-safe keypress path for unattended testing: when SendInput can't
// reach the game, we patch the IDirectInputDevice8::GetDeviceState slot on
// the keyboard device and merge key presses read from br1mod_key.txt.
// File protocol: whitespace-separated decimal DIK scancodes; the file is
// consumed (deleted) on the first poll that sees it = one key press.
typedef HRESULT (WINAPI *CreateDevice_t)(IUnknown*, REFGUID, LPVOID*, LPUNKNOWN);
typedef HRESULT (WINAPI *GetDevState_t)(IUnknown*, DWORD, LPVOID);
static CreateDevice_t g_realCreateDevice;
static GetDevState_t  g_realGetState;
static void*          g_kbDevVtblNew[40];

static void InjectKeys(BYTE* ks) {
    FILE* f = fopen("br1mod_key.txt", "r");
    if (!f) return;
    int code, any = 0;
    while (fscanf(f, "%i", &code) == 1)
        if (code >= 0 && code < 256) { ks[code] |= 0x80; any = 1; }
    fclose(f);
    if (any) { DeleteFileA("br1mod_key.txt"); DiTrace("keyq consumed\n"); }
}

static HRESULT WINAPI HookGetDevState(IUnknown* dev, DWORD cb, LPVOID data) {
    HRESULT hr = g_realGetState(dev, cb, data);
    if (cb == 256 && SUCCEEDED(hr)) InjectKeys((BYTE*)data);
    return hr;
}

// GUID_SysKeyboard = {6F1D2B61-D5A0-11CF-BFC7-444553540000}
static const GUID GUID_SysKeyboard_local =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

static HRESULT WINAPI HookCreateDevice(IUnknown* di, REFGUID g, LPVOID* out, LPUNKNOWN outer) {
    HRESULT hr = g_realCreateDevice(di, g, out, outer);
    if (SUCCEEDED(hr) && out && *out &&
        !memcmp(&g, &GUID_SysKeyboard_local, sizeof(GUID))) {
        DWORD** dev = (DWORD**)*out;
        DWORD* oldv = *dev;
        memcpy(g_kbDevVtblNew, oldv, sizeof(g_kbDevVtblNew));
        g_realGetState = (GetDevState_t)g_kbDevVtblNew[9];
        g_kbDevVtblNew[9] = (void*)&HookGetDevState;
        *dev = (DWORD*)g_kbDevVtblNew;
        DiTrace("kbdev hooked dev=%p gds=%p\n", dev, g_realGetState);
    }
    return hr;
}

static void* g_diVtblNew[32];
static void PatchDirectInput8(LPVOID* ppv) {
    if (!ppv || !*ppv) return;
    DWORD** ifc = (DWORD**)*ppv;
    DWORD* oldv = *ifc;
    memcpy(g_diVtblNew, oldv, sizeof(g_diVtblNew));
    g_realCreateDevice = (CreateDevice_t)g_diVtblNew[3];
    g_diVtblNew[3] = (void*)&HookCreateDevice;
    *ifc = (DWORD*)g_diVtblNew;
    DiTrace("di8 hooked ifc=%p createdev=%p\n", ifc, g_realCreateDevice);
}

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
    HRESULT hr = g_realDI8C(a, b, c, d, e);
    if (SUCCEEDED(hr) && d) PatchDirectInput8(d);
    return hr;
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
    g_retMissionGo = RVA(RET_MISSION_GO);
    g_missionName = RVA(G_MISSION_NAME);
    g_fnFindActor = RVA(FN_FIND_ACTOR);
    g_actorMgrPtr = RVA(G_ACTOR_MGR);
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

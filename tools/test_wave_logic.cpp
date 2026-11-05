// Isolated tests of the actual mod code. Never opens or launches rayne1.exe.
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static DWORD testNow;
static int recordWrites;
static char recordKey[32];
static DWORD WINAPI TestClock() { return testNow; }
static UINT WINAPI TestReadIni(LPCSTR, LPCSTR, INT def, LPCSTR) { return def; }
static BOOL WINAPI TestWriteIni(LPCSTR, LPCSTR key, LPCSTR, LPCSTR) {
    ++recordWrites;
    strcpy(recordKey, key);
    return TRUE;
}
#define GetTickCount TestClock
#define GetPrivateProfileIntA TestReadIni
#define WritePrivateProfileStringA TestWriteIni
#include "../br1mod.cpp"
#undef GetTickCount
#undef GetPrivateProfileIntA
#undef WritePrivateProfileStringA

struct MockActor { DWORD data[0x160 / 4]; };
static MockActor actors[(N_SOLDIERS + N_ELITES + 6) * 2];
static int actorCount, respawns, motionResets;
static DWORD mockVtbl[0xDC / 4];
static DWORD manager[0x1220 / 4];
static DWORD managerPtr = (DWORD)manager;
static DWORD hero[0x100 / 4];
static DWORD heroPtr = (DWORD)hero;
static char mission[32];
static int scriptMode;

static DWORD __declspec(naked) MockDeathState() {
    __asm { mov eax, [ecx + 0x150]
            ret }
}
static void __declspec(naked) MockRespawn() {
    __asm { mov dword ptr [ecx + 0x150], 0
            inc respawns
            ret 8 }
}
static void __declspec(naked) MockMotion() {
    __asm { inc motionResets
            ret 8 }
}
static void LinkActors(int count) {
    manager[0x1218 / 4] = count ? (DWORD)&actors[0] : 0;
    for (int i = 0; i < count; ++i)
        actors[i].data[0x148 / 4] = i + 1 < count ? (DWORD)&actors[i + 1] : 0;
}
static void MakeActor(int i, const char* name, bool enabled, DWORD death) {
    memset(&actors[i], 0, sizeof(actors[i]));
    actors[i].data[0] = (DWORD)mockVtbl;
    strcpy((char*)&actors[i].data[1], name);
    actors[i].data[ACT_ENABLED / 4] = enabled ? ACT_ENABLED_MAGIC : 0x6F3874E;
    actors[i].data[0x150 / 4] = death;
}
static void DrainQueue() { while (!QEmpty()) QPop(); }
static void Reset(int mode = 0, int diff = 0) {
    g_mode = mode; g_diff = diff;
    g_challenge = 2; g_wstate = 0; g_autokill = 0;
    g_nSpawned = 0; g_qHead = g_qTail = 0;
    g_soldierRot = g_eliteRot = g_wpRot = 0;
    g_seenActors = 0; g_lastAlive = -1;
    g_t0 = testNow = 10000;
    g_stateSince = 0;
    recordWrites = 0; recordKey[0] = 0;
    actorCount = respawns = motionResets = 0;
    strcpy(mission, mode == 2 ? MSN_MONSTER
                    : mode == 3 ? MSN_BOSSRUSH : MSN_CHALLENGE);
    LinkActors(0);
}
static void MaterializeWave() {
    for (int i = 0; i < g_nSpawned; ++i) {
        bool exists = false;
        for (int j = 0; j < actorCount; ++j)
            if (!strcmp((char*)&actors[j].data[1], g_spawned[i])) exists = true;
        if (exists) continue;  // retain old corpses; production must revive them
        // Put a disabled declaration BEFORE the enabled instance.
        MakeActor(actorCount++, g_spawned[i], false, 2);
        MakeActor(actorCount++, g_spawned[i], true, 0);
    }
    LinkActors(actorCount);
}
static void KillWave() {
    for (int i = 0; i < g_nSpawned; ++i)
        for (int j = 1; j < actorCount; j += 2)
            if (!strcmp((char*)&actors[j].data[1], g_spawned[i]))
                actors[j].data[0x150 / 4] = i % 2 ? 1 : 2;
}
static void EnterFight() {
    MaterializeWave();
    DrainQueue();
    WaveTick(); assert(g_wstate == 2);
    testNow += SPAWN_SETTLE_MS - 1;
    WaveTick(); assert(g_wstate == 2);
    ++testNow;
    WaveTick(); assert(g_wstate == 3);
    WaveTick(); assert(g_wstate == 3 && g_lastAlive == g_nSpawned);
    DrainQueue();
}

int main() {
    // A private zeroed game image makes ShowHud see a null game object.
    // All actor memory and virtual calls below belong to this test process.
    g_exeBase = (DWORD)VirtualAlloc(NULL, 0x2000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(g_exeBase);
    g_actorMgrPtr = (DWORD)&managerPtr;
    g_heroGlobA = (DWORD)&heroPtr;
    hero[0x64 / 4] = 100;
    g_missionName = (DWORD)mission;
    g_cmdModePtr = (DWORD)&scriptMode;
    mockVtbl[0xD8 / 4] = (DWORD)MockDeathState;
    mockVtbl[0x6C / 4] = (DWORD)MockRespawn;
    g_fnRespawnMotion = (DWORD)MockMotion;

    Reset();
    MakeActor(0, "SoldierS1", false, 2);
    MakeActor(1, "SoldierS1", true, 0);
    LinkActors(2);
    int seen = 0;
    assert(NameAlive("SoldierS1", &seen) == 1 && seen);
    actors[1].data[0x150 / 4] = 1;
    assert(NameAlive("SoldierS1", &seen) == 0);  // enabled, dying
    actors[1].data[0x150 / 4] = 2;
    assert(NameAlive("SoldierS1", &seen) == 0);  // enabled corpse retained
    MakeActor(2, "SoldierS1", true, 0); LinkActors(3);
    assert(NameAlive("SoldierS1", &seen) == 1);  // reused name + old corpse
    seen = 0;
    assert(NameAlive("NeverSpawned", &seen) == 0 && !seen);
    puts("PASS: duplicate declarations, enabled corpses, dying and reused names");

    Reset(); WaveTick(); EnterFight();
    KillWave();
    actors[1].data[0x150 / 4] = 0;  // one enemy still alive
    testNow += 60000;
    WaveTick(); assert(g_wave == 1 && g_wstate == 3 && g_lastAlive == 1);
    KillWave();
    WaveTick(); assert(g_wave == 1 && g_wstate == 5);
    DWORD clearedAt = testNow;
    for (int i = 0; i < 10000; ++i) WaveTick();  // ticks are not seconds
    assert(g_wave == 1 && QEmpty());
    testNow = clearedAt + WAVE_GAP_MS - 1;
    WaveTick(); assert(g_wave == 1 && QEmpty());
    ++testNow; WaveTick();
    assert(g_wave == 2 && g_wstate == 1 && !QEmpty());
    puts("PASS: last enemy gates completion, next spawn starts at exactly 3500 ms");

    Reset(); WaveTick(); MaterializeWave(); KillWave();
    DrainQueue(); WaveTick(); testNow += SPAWN_SETTLE_MS;
    WaveTick(); WaveTick();
    assert(g_wstate == 5 && recordWrites == 0);
    puts("PASS: kills before fight tracking do not trigger the old 3600-tick stall");

    Reset(); WaveTick(); DrainQueue(); WaveTick();
    testNow += SPAWN_SETTLE_MS; WaveTick(); WaveTick();
    assert(g_wstate == 3);
    testNow += SPAWN_TIMEOUT_MS; WaveTick();
    assert(g_wstate == 4 && g_wave == 1 && recordWrites == 0);
    puts("PASS: missing spawns stop with an error and never award completion");

    Reset(); WaveTick(); EnterFight();
    testNow = 0xFFFFFF00; KillWave(); WaveTick();
    testNow += WAVE_GAP_MS - 1; WaveTick(); assert(g_wave == 1);
    ++testNow; WaveTick(); assert(g_wave == 2);
    puts("PASS: clock wraparound");

    const int modes[] = { 0, 0, 0, 2, 3, 0 };
    const int diffs[] = { 0, 1, 2, 0, 0, 3 };
    const char* keys[] = { "BestNormal", "BestHard", "BestExtreme",
                           "BestMonster", "BestRush", "BestWaves" };
    for (int run = 0; run < 6; ++run) {
        Reset(modes[run], diffs[run]); WaveTick();
        int limit = g_diff == 3 ? 45 : NWAVES();  // cycles soldier/elite pools
        for (int w = 1; w <= limit; ++w) {
            assert(g_wave == w && g_wstate == 1);
            EnterFight(); KillWave(); WaveTick();
            if (g_diff != 3 && w == limit) {
                assert(g_wstate == 6 && QEmpty() && recordWrites == 1);
                DWORD finishedAt = testNow;
                for (int i = 0; i < 100; ++i) WaveTick();
                assert(g_wstate == 6 && QEmpty());
                testNow = finishedAt + RESULT_MS - 1;
                WaveTick(); assert(g_wstate == 6 && QEmpty());
                ++testNow; WaveTick();
                assert(g_wstate == 4 && !QEmpty());
                char* ending = QPop();
                assert(!strcmp(ending, "raise(gameOver)") && QEmpty());
                assert(IsOurCmd(ending) && NeuterCmd(ending));
                // Exercise the real mailbox: the end event must reach
                // the engine even though normal raise commands are suppressed.
                QPush(ending);
                DWORD entry[3] = { 5, (DWORD)g_idleStr, 0 };
                ProcessCommand(4, (BYTE*)entry);
                assert(!strcmp((char*)entry[1], "raise(gameOver)"));
                ProcessCommand(4, (BYTE*)entry);
                assert(entry[1] == (DWORD)g_idleStr && QEmpty());
                WaveTick(); assert(QEmpty() && recordWrites == 1);
            } else {
                assert(g_wstate == 5);
                testNow += WAVE_GAP_MS;
                WaveTick(); assert(g_wave == w + 1 && g_wstate == 1);
            }
        }
        assert(!strcmp(recordKey, keys[run]));
        if (run <= 2 || run == 6) assert(respawns > 0);
        if (g_mode == 2) assert(motionResets == respawns && motionResets > 0);
        printf("PASS: arena %d (%d waves, %d native respawns, %s)\n",
               run + 1, limit, respawns, g_diff == 3 ? "keeps running" : "ends after result");
    }
    Reset(0, 3); WaveTick(); EnterFight();
    g_wave = 999; KillWave(); WaveTick();
    assert(g_wstate == 5);
    testNow += WAVE_GAP_MS; WaveTick();
    assert(g_wave == 1000 && g_wstate == 1 && recordWrites == 1);
    puts("PASS: endless horde passes wave 999 without finishing");
    Reset(); WaveTick(); EnterFight();
    strcpy(mission, "training.msn"); WaveTick();
    assert(g_challenge == 0 && QEmpty() && recordWrites == 0);
    puts("PASS: leaving arena cancels progression");
    Reset(); WaveTick(); EnterFight();
    g_wave = NWAVES(); KillWave(); WaveTick(); assert(g_wstate == 6);
    strcpy(mission, "training.msn");
    testNow += RESULT_MS; WaveTick();
    assert(g_challenge == 0 && QEmpty());
    puts("PASS: leaving during result cancels the delayed end event");
    VirtualFree((void*)g_exeBase, 0, MEM_RELEASE);
    puts("All isolated wave regression tests passed. Game was not launched.");
    return 0;
}

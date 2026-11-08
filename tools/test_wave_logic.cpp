// Isolated tests of the actual mod code. Never opens or launches rayne1.exe.
#define _CRT_SECURE_NO_WARNINGS
#include <Windows.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

static DWORD testNow;
static int recordWrites;
static char recordKey[32];
static DWORD recordValue;
static DWORD WINAPI TestClock() { return testNow; }
static UINT WINAPI TestReadIni(LPCSTR, LPCSTR, INT def, LPCSTR) { return def; }
static BOOL WINAPI TestWriteIni(LPCSTR, LPCSTR key, LPCSTR value, LPCSTR) {
    ++recordWrites;
    strcpy(recordKey, key);
    recordValue = value ? strtoul(value, NULL, 10) : 0;
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

static int __cdecl MockContinue() { return 4; }
static int __cdecl MockMissionExit() { return 6; }

static int __declspec(naked) DispatchContinue(DWORD action) {
    __asm {
        push edi
        lea eax, [esp + 8]
        call HookDispatch
        pop edi
        ret
    }
}

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
    g_challengePauseHero = 0;
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

static BYTE photoGame[0x2200], photoView[0x200];
static int photoDraws, photoPresents;
static bool expectPhotoCamera;
static int photoResumeCalls;

static void __fastcall MockPhotoDraw(BYTE* game) {
    assert(game == photoGame && bool(g_photoRendering) == expectPhotoCamera);
    assert(PhotoGlobal<DWORD>(MENU_ID) == 0);
    assert(PhotoField<DWORD>(game, 0x2D8) == (expectPhotoCamera ? 0u : 1u));
    assert(PhotoField<DWORD>(game, 0x1F88) == 0);
    assert(PhotoField<float>(game, 0x1F90) == 0);
    assert(PhotoField<DWORD>(photoView, 4) == (expectPhotoCamera ? 0u : 3u));
    assert(PhotoField<DWORD>(photoView, 8) == (expectPhotoCamera ? 0u : 2u));
    assert(PhotoField<DWORD>(photoView, 12) == (expectPhotoCamera ? 0u : 5u));
    assert(PhotoField<float>(photoView, 0x70) == (expectPhotoCamera ? 24.0f : 10.0f));
    assert(PhotoField<PhotoVec>(photoView, 0x74).y == (expectPhotoCamera ? 42 : 12));
    assert(PhotoField<PhotoVec>(photoView, 0x84).x == (expectPhotoCamera ? 0 : 1));
    ++photoDraws;
}

static void __cdecl MockPhotoPresent() {
    assert(!g_photoRendering && PhotoGlobal<DWORD>(MENU_ID) == PAUSE_MENU);
    assert(PhotoField<DWORD>(photoGame, 0x2D8) == 1);
    assert(PhotoField<DWORD>(photoGame, 0x1F88) == 123);
    assert(PhotoField<PhotoVec>(photoView, 0x74).y == 12);
    assert(PhotoField<PhotoVec>(photoView, 0x84).x == 1);
    assert(PhotoField<DWORD>(photoView, 4) == 3);
    assert(PhotoField<DWORD>(photoView, 8) == 2);
    assert(PhotoField<DWORD>(photoView, 12) == 5);
    assert(PhotoField<float>(photoView, 0x70) == 10);
    ++photoPresents;
}

static void __cdecl MockPhotoMenu(DWORD menu) {
    assert(menu == 0 && photoResumeCalls++ == 0);
    PhotoGlobal<DWORD>(MENU_ID) = menu;
}

static void __fastcall MockPhotoSound(void*, void*, int paused) {
    assert(paused == 0 && photoResumeCalls++ == 1);
}

static void __cdecl MockPhotoRumble() {
    assert(photoResumeCalls++ == 2);
}

static void MockPhotoCall(DWORD address, DWORD target) {
    auto* entry = reinterpret_cast<BYTE*>(RVA(address));
    DWORD oldProtect;
    assert(VirtualProtect(entry, 6, PAGE_EXECUTE_READWRITE, &oldProtect));
    entry[0] = 0x68; // push imm32; ret
    memcpy(entry + 1, &target, 4);
    entry[5] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), entry, 6);
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
    g_retDispatch = (DWORD)MockContinue;
    g_retMissionExit = (DWORD)MockMissionExit;
    g_itemsOff4 = 0;
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
    PhotoGlobal<DWORD>(MENU_ID) = GAME_OVER_MENU;
    assert(DispatchContinue(ACTION_CONTINUE) == 4 && g_challenge == 2);
    PhotoGlobal<DWORD>(MENU_ID) = 0;
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
                assert(g_wstate == WAVE_COMPLETED && !QEmpty());
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
                PhotoGlobal<DWORD>(MENU_ID) = PAUSE_MENU;
                assert(DispatchContinue(ACTION_CONTINUE) == 4 && g_challenge == 2);
                PhotoGlobal<DWORD>(MENU_ID) = GAME_OVER_MENU;
                heroPtr = 0;
                assert(DispatchContinue(ACTION_CONTINUE) == 4 && g_challenge == 2);
                heroPtr = (DWORD)hero;
                assert(DispatchContinue(ACTION_CONTINUE) == 6 && !g_challenge);
                assert(QEmpty() && recordWrites == 1);
                assert(DispatchContinue(ACTION_CONTINUE) == 4);
                PhotoGlobal<DWORD>(MENU_ID) = 0;
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
    // Pause-menu "restart level": the hero re-instantiates mid-wave and
    // every actor goes dormant. Without the fresh-hero re-arm the engine
    // counted that as a cleared wave and bumped the counter each restart.
    Reset(); WaveTick(); EnterFight();
    g_wave = 4;
    for (int i = 0; i < actorCount; ++i) actors[i].data[0x150 / 4] = 2;
    g_heroSpawnAt = testNow + 1500;
    WaveTick();                              // fresh-hero re-arm fires
    assert(g_wstate == 0 && g_heroSpawnAt == 0 && QEmpty());
    WaveTick();                              // case 0: hero is up -> wave 1
    assert(g_wstate == 1 && g_wave == 1);
    puts("PASS: restart level mid-wave re-arms to wave 1");
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
    MockPhotoCall(0x474850, (DWORD)MockPhotoDraw);
    MockPhotoCall(0x4CEDE0, (DWORD)MockPhotoPresent);
    PhotoGlobal<BYTE*>(MAIN_VIEW) = photoView;
    PhotoGlobal<DWORD>(MENU_ID) = PAUSE_MENU;
    PhotoField<DWORD>(photoGame, 0x2D8) = 1;
    PhotoField<DWORD>(photoGame, 0x1F88) = 123;
    PhotoField<float>(photoGame, 0x1F90) = 0.016f;
    PhotoField<PhotoVec>(photoView, 0x74).y = 12;
    PhotoField<PhotoVec>(photoView, 0x84).x = 1;
    PhotoField<DWORD>(photoView, 4) = 3;
    PhotoField<DWORD>(photoView, 8) = 2;
    PhotoField<DWORD>(photoView, 12) = 5;
    PhotoField<float>(photoView, 0x70) = 10.0f;
    PhotoGlobal<float>(0x5C5BE4) = 24.0f;
    g_photoPosition.y = 42;
    expectPhotoCamera = true;
    RenderPhotoFrame(photoGame, PhotoFrameView::Camera);
    expectPhotoCamera = false;
    RenderPhotoFrame(photoGame, PhotoFrameView::Gameplay);
    assert(photoDraws == 2 && photoPresents == 2);
    assert(PhotoField<float>(photoGame, 0x1F90) == 0.016f);
    puts("PASS: clean photo draw restores the original effects, FOV, camera, HUD and pause");

    MockPhotoCall(0x4CAA10, (DWORD)MockPhotoMenu);
    MockPhotoCall(0x552000, (DWORD)MockPhotoSound);
    MockPhotoCall(0x508980, (DWORD)MockPhotoRumble);
    g_photoState = PhotoReturning;
    g_photoReturnToGame = false;
    CompletePhotoExit();
    assert(g_photoState == PhotoOff && PhotoGlobal<DWORD>(MENU_ID) == PAUSE_MENU && !photoResumeCalls);
    g_photoState = PhotoReturning;
    g_photoReturnToGame = true;
    CompletePhotoExit();
    assert(g_photoState == PhotoOff && PhotoGlobal<DWORD>(MENU_ID) == 0 && photoResumeCalls == 3);
    puts("PASS: menu entry returns to pause and shortcut entry runs the native resume sequence");

    PhotoGlobal<DWORD>(G_HERO_A) = heroPtr;
    Reset(); WaveTick(); EnterFight(); KillWave(); WaveTick();
    assert(g_wstate == 5);
    testNow += 1000;
    const DWORD playedBeforePause = testNow - g_t0;
    PhotoGlobal<DWORD>(MENU_ID) = PAUSE_MENU;
    PhotoFrame(photoGame);
    testNow += 7000;
    PhotoGlobal<DWORD>(MENU_ID) = 0x15; // Options opened from Pause
    PhotoFrame(photoGame);
    testNow += 6000;
    PhotoGlobal<DWORD>(MENU_ID) = PAUSE_MENU;
    PhotoFrame(photoGame);
    BeginPhoto(photoGame, testNow, -1);
    testNow += 9000;
    CompletePhotoExit();                  // returns to the same pause
    PhotoFrame(photoGame);
    testNow += 5000;
    PhotoGlobal<DWORD>(MENU_ID) = 0;
    PhotoFrame(photoGame);
    assert(testNow - g_t0 == playedBeforePause && testNow - g_stateSince == 1000);
    PhotoFrame(photoGame);                // subsequent frames must not shift again
    testNow += WAVE_GAP_MS - 1000 - 1;
    WaveTick(); assert(g_wave == 1 && QEmpty());
    ++testNow;
    WaveTick(); assert(g_wave == 2 && g_wstate == 1);
    puts("PASS: Pause, Options and Photo share one clock and preserve the remaining wave gap");

    Reset(); WaveTick(); EnterFight();
    testNow += 1250;
    const DWORD expectedRecord = testNow - g_t0;
    PhotoGlobal<DWORD>(MENU_ID) = PAUSE_MENU;
    PhotoFrame(photoGame);
    testNow += 30000;
    PhotoGlobal<DWORD>(MENU_ID) = 0;
    PhotoFrame(photoGame);
    g_wave = NWAVES(); KillWave(); WaveTick();
    assert(recordWrites == 1 && recordValue == expectedRecord);
    puts("PASS: a long pause does not count toward a completed challenge record");
    VirtualFree((void*)g_exeBase, 0, MEM_RELEASE);
    puts("All isolated wave regression tests passed. Game was not launched.");
    return 0;
}

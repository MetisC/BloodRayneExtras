// Included after the challenge state so photo time can be excluded from
// arena timers. All hooks and input run on the game's main thread.
static int PatchChecked(DWORD site, const char* expected, int siteLen, void* target);

namespace {
constexpr DWORD PAUSE_MENU = 0x0F;
constexpr DWORD MENU_ID = 0x1D27B24;
constexpr DWORD MAIN_VIEW = 0x62D6D4;
constexpr DWORD MOUSE_X = 0x1C845E0, MOUSE_Y = 0x1C845E4;
constexpr WORD PHOTO_CHORD = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
constexpr WORD PHOTO_EXIT = XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START;

struct PhotoVec { float x, y, z; };
struct PhotoPad {
    float strafe, forward, yaw, pitch, lift;
    WORD buttons;
};
enum PhotoState { PhotoOff, PhotoActive, PhotoReturning };
enum class PhotoFrameView { Camera, Pause, Gameplay };

static PhotoState g_photoState;
static int g_photoRequested, g_photoReady, g_photoRendering;
static DWORD g_photoFrameNext, g_photoFrameDone, g_photoNativeFrame;
static DWORD g_photoMenuNext, g_photoGamePtr, g_photoMenuPtr;
static DWORD g_photoOverlayNext, g_photoOverlayDone;
static DWORD g_photoFadeNext, g_photoNativeFade;
static DWORD g_photoTintNext, g_photoTintDone, g_photoTintPtr;
static PhotoVec g_photoPosition, g_photoAngles;
static DWORD g_photoFrameAt;
static bool g_photoExitArmed, g_photoF12Down, g_photoReturnToGame;
static WORD g_photoPadPrevious[4];
static int g_photoPadSlot;
static float g_photoTimeScale[4];

template <class T> static T& PhotoGlobal(DWORD address) {
    return *reinterpret_cast<T*>(RVA(address));
}

template <class T> static T& PhotoField(BYTE* object, DWORD offset) {
    return *reinterpret_cast<T*>(object + offset);
}

static bool PhotoKey(int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; }

static float PhotoAxis(float value, float deadzone) {
    value = max(-1.0f, min(1.0f, value));
    if (value > deadzone) return (value - deadzone) / (1.0f - deadzone);
    if (value < -deadzone) return (value + deadzone) / (1.0f - deadzone);
    return 0.0f;
}

static float PhotoTrigger(BYTE value) {
    return max(0, value - XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
           / float(255 - XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
}

// Read the same XInput entry point that the game imports. When a Sony pad
// is connected through the game's DirectInput path, use its native axes
// and button state instead. L1/R1 also work for height on that path.
static bool ReadPhotoPad(int slot, PhotoPad& out) {
    typedef DWORD (WINAPI *GetState)(DWORD, XINPUT_STATE*);
    auto getState = PhotoGlobal<GetState>(0x5B9434);
    XINPUT_STATE state = {};
    if (getState && getState(slot, &state) == ERROR_SUCCESS) {
        const auto& pad = state.Gamepad;
        out = { PhotoAxis(pad.sThumbLX / 32768.0f, 0.24f),
                PhotoAxis(pad.sThumbLY / 32768.0f, 0.24f),
                PhotoAxis(pad.sThumbRX / 32768.0f, 0.26f),
                PhotoAxis(pad.sThumbRY / 32768.0f, 0.26f),
                PhotoTrigger(pad.bRightTrigger) - PhotoTrigger(pad.bLeftTrigger),
                pad.wButtons };
        return true;
    }
    auto device = PhotoGlobal<IDirectInputDevice8A*>(0x1FBCFD8 + slot * 4);
    static IDirectInputDevice8A* previous[4];
    static bool sony[4];
    if (device != previous[slot]) {
        previous[slot] = device;
        sony[slot] = false;
        if (device) {
            DIDEVICEINSTANCEA info = {};
            info.dwSize = sizeof(info);
            sony[slot] = SUCCEEDED(device->GetDeviceInfo(&info))
                      && LOWORD(info.guidProduct.Data1) == 0x054C;
        }
    }
    if (!device || !sony[slot]) return false;
    const DWORD pad = 0x1C4C220 + slot * 0x1DC;
    if (!PhotoGlobal<DWORD>(pad + 4)) return false;
    const auto* buttons = &PhotoGlobal<DWORD>(pad + 0xD8);
    out = { PhotoAxis(PhotoGlobal<float>(pad + 0x28), 0.24f),
            PhotoAxis(-PhotoGlobal<float>(pad + 0x2C), 0.24f),
            PhotoAxis(PhotoGlobal<float>(pad + 0x30), 0.26f),
            PhotoAxis(-PhotoGlobal<float>(pad + 0x3C), 0.26f),
            float((buttons[7] || buttons[5]) - (buttons[6] || buttons[4])),
            WORD((buttons[2] ? XINPUT_GAMEPAD_B : 0)
               | (buttons[9] ? XINPUT_GAMEPAD_START : 0)
               | (buttons[10] ? XINPUT_GAMEPAD_LEFT_THUMB : 0)
               | (buttons[11] ? XINPUT_GAMEPAD_RIGHT_THUMB : 0)) };
    return true;
}

static void MovePhotoCamera(const PhotoPad& pad, LONG mouseX, LONG mouseY, float dt) {
    const float lookRate = 1.9f;
    const float mouseRate = 0.0025f;
    const float pitchLimit = 1.553343f; // +/- 89 degrees
    if (pad.yaw != 0 || mouseX != 0) {
        g_photoAngles.y += pad.yaw * lookRate * dt + mouseX * mouseRate;
        g_photoAngles.y = remainderf(g_photoAngles.y, 6.2831853f);
    }
    if (pad.pitch != 0 || mouseY != 0) {
        g_photoAngles.x = max(-pitchLimit, min(pitchLimit,
            g_photoAngles.x - pad.pitch * lookRate * dt + mouseY * mouseRate));
    }

    float right = pad.strafe + PhotoKey('D') - PhotoKey('A');
    float front = pad.forward + PhotoKey('W') - PhotoKey('S');
    float lift = pad.lift + (PhotoKey(VK_SPACE) || PhotoKey('E'))
                         - (PhotoKey(VK_CONTROL) || PhotoKey('Q'));
    const float length = sqrtf(right * right + front * front + lift * lift);
    const float speed = (PhotoKey(VK_SHIFT) ? 65.0f : 24.0f) * dt / max(1.0f, length);
    const float sy = sinf(g_photoAngles.y), cy = cosf(g_photoAngles.y);
    const float cp = cosf(g_photoAngles.x), sp = sinf(g_photoAngles.x);
    g_photoPosition.x += speed * (cy * right + sy * cp * front);
    g_photoPosition.y += speed * (lift - sp * front);
    g_photoPosition.z += speed * (-sy * right + cy * cp * front);
}

static bool PhotoCanEnter(BYTE* game) {
    BYTE* view = PhotoGlobal<BYTE*>(MAIN_VIEW);
    BYTE* script = PhotoGlobal<BYTE*>(0x631BE0);
    return game && view && script && PhotoGlobal<DWORD>(G_HERO_A)
        && PhotoGlobal<DWORD>(0x631C00)
        && !PhotoField<DWORD>(game, 0x18C)
        && !PhotoField<BYTE>(script, 0x1FC)
        && isfinite(PhotoField<float>(view, 0x70))
        && PhotoField<float>(view, 0x70) > 0.0f;
}

static void RenderPhotoFrame(BYTE* game, PhotoFrameView frame) {
    typedef void (__thiscall *GameRender)(BYTE*);
    const bool camera = frame == PhotoFrameView::Camera;
    BYTE* view = PhotoGlobal<BYTE*>(MAIN_VIEW);
    DWORD& menu = PhotoGlobal<DWORD>(MENU_ID);
    const DWORD savedMenu = menu;
    float& dt = PhotoField<float>(game, 0x1F8C);
    float& realDt = PhotoField<float>(game, 0x1F90);
    DWORD& ticks = PhotoField<DWORD>(game, 0x1F88);
    DWORD& globalTicks = PhotoGlobal<DWORD>(0x1C5B82C);
    const float savedDt = dt, savedRealDt = realDt;
    const DWORD savedTicks = ticks, savedGlobalTicks = globalTicks;
    DWORD& hud = PhotoField<DWORD>(game, 0x2D8);
    DWORD& debug = PhotoField<DWORD>(game, 0x2E8);
    DWORD& debugOverlay = PhotoField<DWORD>(game, 0x1FB0);
    DWORD& outline = PhotoField<DWORD>(game, 0x2E0);
    BYTE& inset = PhotoField<BYTE>(game, 0x20C8);
    const DWORD savedHud = hud, savedDebug = debug, savedDebugOverlay = debugOverlay;
    const DWORD savedOutline = outline;
    const BYTE savedInset = inset;
    // CView::Render reads position +58, Euler angles +64, visibility eye
    // +74 and shake offset +84. Preserve the native pose even on exit.
    BYTE savedView[0x38];
    DWORD savedModes[3];
    if (camera) {
        memcpy(savedView, view + 0x58, sizeof(savedView));
        memcpy(savedModes, view + 4, sizeof(savedModes));
        // The renderer at 0x51CFB0 selects the vision pass from view+4.
        // Clear the view's transient modes and use its native normal FOV
        // for the photo draw, without changing the hero or power timers.
        memset(view + 4, 0, sizeof(savedModes));
        PhotoField<float>(view, 0x70) = PhotoGlobal<float>(0x5C5BE4);
        PhotoField<PhotoVec>(view, 0x58) = g_photoPosition;
        PhotoField<PhotoVec>(view, 0x64) = g_photoAngles;
        PhotoField<PhotoVec>(view, 0x74) = g_photoPosition;
        PhotoField<PhotoVec>(view, 0x84) = {};
        hud = debug = debugOverlay = outline = 0;
        inset = 0;
    }
    if (frame != PhotoFrameView::Pause)
        menu = 0; // select the scene render path for this call only
    dt = realDt = 0.0f;
    ticks = globalTicks = 0;
    g_photoRendering = camera;
    __try {
        reinterpret_cast<GameRender>(RVA(0x474850))(game);
    } __finally {
        g_photoRendering = 0;
        if (camera) {
            memcpy(view + 0x58, savedView, sizeof(savedView));
            memcpy(view + 4, savedModes, sizeof(savedModes));
            hud = savedHud;
            debug = savedDebug;
            debugOverlay = savedDebugOverlay;
            outline = savedOutline;
            inset = savedInset;
        }
        menu = savedMenu;
        dt = savedDt;
        realDt = savedRealDt;
        ticks = savedTicks;
        globalTicks = savedGlobalTicks;
    }
    // Present runs the native window message pump and input sampling.
    reinterpret_cast<void (__cdecl*)()>(RVA(0x4CEDE0))();
}

static void BeginPhoto(BYTE* game, DWORD now, int padSlot) {
    BYTE* view = PhotoGlobal<BYTE*>(MAIN_VIEW);
    g_photoPosition = PhotoField<PhotoVec>(view, 0x74);
    g_photoAngles = PhotoField<PhotoVec>(view, 0x64);
    g_photoPadSlot = padSlot;
    g_photoFrameAt = now;
    g_photoExitArmed = false;
    g_photoReturnToGame = PhotoGlobal<DWORD>(MENU_ID) == 0;
    memcpy(g_photoTimeScale, game + 0x1F9C, sizeof(g_photoTimeScale));
    g_photoState = PhotoActive;
    PhotoGlobal<LONG>(MOUSE_X) = PhotoGlobal<LONG>(MOUSE_Y) = 0;

    if (g_photoReturnToGame) {
        UpdateChallengePause(now, true, PhotoGlobal<DWORD>(G_HERO_A));
        // Native pause path from CGame::CheckPause (0x475388).
        PhotoField<DWORD>(game, 0x2C0) = 0;
        typedef void (__thiscall *PauseSound)(void*, int);
        reinterpret_cast<PauseSound>(RVA(0x552000))(PhotoGlobal<void*>(0x6324A0), 1);
        reinterpret_cast<void (__cdecl*)()>(RVA(0x5089A0))();
        reinterpret_cast<void (__cdecl*)(DWORD)>(RVA(0x4CAA10))(PAUSE_MENU);
    }
    DiTrace("PHOTO entered\n");
}

static void CompletePhotoExit() {
    g_photoState = PhotoOff;
    if (g_photoReturnToGame) {
        // Native Resume sequence at 0x47533C. Keep the release frame
        // paused so the exit button cannot immediately pause again.
        reinterpret_cast<void (__cdecl*)(DWORD)>(RVA(0x4CAA10))(0);
        typedef void (__thiscall *PauseSound)(void*, int);
        reinterpret_cast<PauseSound>(RVA(0x552000))(PhotoGlobal<void*>(0x6324A0), 0);
        reinterpret_cast<void (__cdecl*)()>(RVA(0x508980))();
    }
    DiTrace("PHOTO returned to %s\n", g_photoReturnToGame ? "gameplay" : "pause");
}

static int __cdecl PhotoFrame(BYTE* game) {
    const DWORD menu = PhotoGlobal<DWORD>(MENU_ID);
    const DWORD now = GetTickCount();
    UpdateChallengePause(now, menu != 0, PhotoGlobal<DWORD>(G_HERO_A));
    if (!g_photoReady) return 0;
    if (g_photoState != PhotoOff)
        memcpy(game + 0x1F9C, g_photoTimeScale, sizeof(g_photoTimeScale));
    if (PhotoGlobal<DWORD>(0x1FBB2A0) || (g_photoState != PhotoOff && menu != PAUSE_MENU)) {
        g_photoState = PhotoOff;
        g_photoRequested = 0;
        return 0;
    }

    const HWND window = PhotoGlobal<HWND>(0x1FBA970);
    const bool focused = window && GetForegroundWindow() == window;
    const bool f12 = focused && PhotoKey(VK_F12);
    const bool f12Pressed = f12 && !g_photoF12Down;
    g_photoF12Down = f12;
    PhotoPad pads[4] = {};
    int firstPad = -1, chordSlot = -1;
    bool padExit = false;
    if (focused && (menu == 0 || menu == PAUSE_MENU)) {
        for (int i = 0; i < 4; ++i) {
            if (ReadPhotoPad(i, pads[i]) && firstPad < 0) firstPad = i;
            const WORD buttons = pads[i].buttons;
            if ((buttons & PHOTO_CHORD) == PHOTO_CHORD
                    && (g_photoPadPrevious[i] & PHOTO_CHORD) != PHOTO_CHORD) chordSlot = i;
            g_photoPadPrevious[i] = buttons;
            padExit |= (buttons & PHOTO_EXIT) != 0;
        }
    }
    if (g_photoState == PhotoOff) {
        const bool requested = (g_photoRequested && menu == PAUSE_MENU)
                            || (menu == 0 && (f12Pressed || chordSlot >= 0));
        g_photoRequested = 0;
        if (!requested || !PhotoCanEnter(game)) return 0;
        BeginPhoto(game, now, chordSlot >= 0 ? chordSlot : firstPad);
    }
    const bool exitHeld = padExit || PhotoKey(VK_ESCAPE);
    if (g_photoState == PhotoActive) {
        if (focused && g_photoExitArmed && exitHeld) {
            g_photoState = PhotoReturning;
        }
        if (focused && !exitHeld) g_photoExitArmed = true;
    }

    LONG& mouseX = PhotoGlobal<LONG>(MOUSE_X);
    LONG& mouseY = PhotoGlobal<LONG>(MOUSE_Y);
    const float dt = min(now - g_photoFrameAt, 50ul) * 0.001f;
    g_photoFrameAt = now;
    if (focused && g_photoState == PhotoActive) {
        const int slot = g_photoPadSlot >= 0 ? g_photoPadSlot : firstPad;
        MovePhotoCamera(slot >= 0 ? pads[slot] : PhotoPad{}, mouseX, mouseY, dt);
    }
    mouseX = mouseY = 0;
    const auto frame = g_photoState == PhotoActive ? PhotoFrameView::Camera
        : g_photoReturnToGame ? PhotoFrameView::Gameplay : PhotoFrameView::Pause;
    RenderPhotoFrame(game, frame);
    if (g_photoState == PhotoReturning && focused && !exitHeld) {
        // Pump a released frame before returning control to the game.
        mouseX = mouseY = 0;
        CompletePhotoExit();
    }
    return 1;
}

static const char* __cdecl PhotoMenuLabel() {
    static const char* labels[] = { "Photo Mode", "Modo Foto", "Mode Photo", "Modalit\xE0 Foto",
        "\xD4\xEE\xF2\xEE\xF0\xE5\xE6\xE8\xEC", "\x83\x74\x83\x48\x83\x67\x83\x82\x81\x5B\x83\x68" };
    if (!g_photoReady || !PhotoCanEnter(PhotoGlobal<BYTE*>(0x62D6D0))) return nullptr;
    if (!g_langDetected) DetectLang();
    return labels[g_lang];
}

static void __cdecl OrderPhotoMenu() {
    // In a cinematic the native builder puts Skip Scene before Resume.
    // Keep Resume first and Photo second there as well.
    if (PhotoGlobal<DWORD>(0x1D27B40) != 3) return;
    constexpr size_t itemSize = 0xE8;
    BYTE* items = reinterpret_cast<BYTE*>(RVA(0x1D20068));
    if (PhotoField<DWORD>(items, 4) != 0x27) return;
    BYTE skip[itemSize];
    memcpy(skip, items, itemSize);
    memmove(items, items + itemSize, 2 * itemSize);
    memcpy(items + 2 * itemSize, skip, itemSize);
}

static void __declspec(naked) HookPhotoMenu() {
    __asm {
        pushad
        call PhotoMenuLabel
        test eax, eax
        jz done
        mov ecx, eax
        mov eax, ACTION_PHOTO
        xor esi, esi
        call g_fnAddItem
        call OrderPhotoMenu
    done:
        popad
        mov ecx, g_photoGamePtr
        mov ecx, [ecx]
        push g_photoMenuNext
        ret
    }
}

static void __declspec(naked) HookPhotoFrame() {
    __asm {
        pushad
        push esi
        call PhotoFrame
        add esp, 4
        test eax, eax
        popad
        jnz handled
        mov ecx, esi
        call g_photoNativeFrame
        push g_photoFrameNext
        ret
    handled:
        push g_photoFrameDone
        ret
    }
}

static void __declspec(naked) HookPhotoOverlay() {
    __asm {
        cmp g_photoRendering, 0
        jne hidden
        push edx
        mov edx, g_photoMenuPtr
        cmp dword ptr [edx], edi
        pop edx
        push g_photoOverlayNext
        ret
    hidden:
        push g_photoOverlayDone
        ret
    }
}

static void __declspec(naked) HookPhotoFade() {
    __asm {
        cmp g_photoRendering, 0
        jne hidden
        mov ecx, esi
        call g_photoNativeFade
    hidden:
        push g_photoFadeNext
        ret
    }
}

static void __declspec(naked) HookPhotoTint() {
    __asm {
        cmp g_photoRendering, 0
        jne hidden
        push eax
        mov eax, g_photoTintPtr
        fld dword ptr [eax]
        pop eax
        push g_photoTintNext
        ret
    hidden:
        push g_photoTintDone
        ret
    }
}

static void InstallPhotoMode() {
    g_photoGamePtr = RVA(0x62D6D0);
    g_photoMenuPtr = RVA(MENU_ID);
    g_photoMenuNext = RVA(0x4C9210);
    g_photoNativeFrame = RVA(0x4739A0);
    g_photoFrameNext = RVA(0x477697);
    g_photoFrameDone = RVA(0x477996);
    g_photoOverlayNext = RVA(0x474AA9);
    g_photoOverlayDone = RVA(0x474AC4);
    // Skip the final focus fade and the view renderer's screen tint.
    // These are separate passes after the world and scripted overlays.
    g_photoFadeNext = RVA(0x474FD4);
    g_photoNativeFade = RVA(0x471B30);
    g_photoTintNext = RVA(0x51D022);
    g_photoTintDone = RVA(0x51D12D);
    g_photoTintPtr = RVA(0x631C04);
    char menu[6] = { '\x8b', '\x0d', 0, 0, 0, 0 };
    char overlay[6] = { '\x39', '\x3d', 0, 0, 0, 0 };
    char tint[6] = { '\xd9', '\x05', 0, 0, 0, 0 };
    memcpy(menu + 2, &g_photoGamePtr, 4);
    memcpy(overlay + 2, &g_photoMenuPtr, 4);
    memcpy(tint + 2, &g_photoTintPtr, 4);
    // The common frame hook also keeps arena timing independent of the
    // optional photo render patches.
    const int frameReady = PatchChecked(RVA(0x477690),
        "\x8b\xce\xe8\x09\xc3\xff\xff", 7, HookPhotoFrame);
    g_photoReady = frameReady &&
        PatchChecked(RVA(0x4C920A), menu, sizeof(menu), HookPhotoMenu) &&
        PatchChecked(RVA(0x474AA3), overlay, sizeof(overlay), HookPhotoOverlay) &&
        PatchChecked(RVA(0x474FCD), "\x8b\xce\xe8\x5c\xcb\xff\xff", 7, HookPhotoFade) &&
        PatchChecked(RVA(0x51D01C), tint, sizeof(tint), HookPhotoTint);
}
} // namespace

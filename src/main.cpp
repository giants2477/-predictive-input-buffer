// Click Before Frames — Ultra-Low-Overhead External Window Master Edition (Zero-Spike Edition)
// Zero in-game visual/audio footprint. Zero disk I/O. Near-zero per-click heap allocations.
// < 1 us/tick idle overhead | 0.75 ms/click soft cap, 1.5 ms hard cap | GD 2.2081 (Geode v5.11.2)

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/RingObject.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr int SPIDER_LAND = 10;
    constexpr int SPAM_STREAK = 3;
    constexpr int MAX_RESUMES = 1;

    constexpr int ALLOWED_TICKS[5] = { 2, 4, 8, 16, 32 };
    constexpr int STAGE_COUNT = 5;

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    constexpr uint64_t COLLIDING_TYPES =
        typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) |
        typeBit(GameObjectType::Slope) | typeBit(GameObjectType::CollisionObject) |
        typeBit(GameObjectType::AnimatedHazard) | typeBit(GameObjectType::Special);

    constexpr uint64_t HAZARD_TYPES =
        typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t INTERACT_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) |
        typeBit(GameObjectType::RedJumpPad) | typeBit(GameObjectType::GravityPad) |
        typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) |
        typeBit(GameObjectType::GravityRing) | typeBit(GameObjectType::GreenRing) |
        typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) |
        typeBit(GameObjectType::SpiderOrb) | typeBit(GameObjectType::InverseGravityPortal) |
        typeBit(GameObjectType::NormalGravityPortal) | typeBit(GameObjectType::GravityTogglePortal);

    constexpr uint64_t STOP_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) |
        typeBit(GameObjectType::BallPortal) | typeBit(GameObjectType::UfoPortal) |
        typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) |
        typeBit(GameObjectType::DualPortal) | typeBit(GameObjectType::SoloPortal) |
        typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

    constexpr uint64_t MODE_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) |
        typeBit(GameObjectType::BallPortal) | typeBit(GameObjectType::UfoPortal) |
        typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal);

    constexpr uint64_t SIZE_TYPES =
        typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal);

    float speedPortalValue(int id) {
        switch (id) {
            case 200: return 0.7f;
            case 201: return 0.9f;
            case 202: return 1.1f;
            case 203: return 1.3f;
            case 1334: return 1.6f;
            default: return 0.f;
        }
    }

    struct QueuedInput {
        int ticksLeft = 0;
        bool down = false;
        bool resume = false;
        int from = 1;
        int elapsed = 0;
        int resumes = 0;
    };

    struct ObjSnapshot {
        GameObject* object = nullptr;
        bool isActivated = false;
        EffectGameObject* effect = nullptr;
        bool activated = false;
        bool byP1 = false;
        bool byP2 = false;
        bool animated = false;
    };

    struct NodeMark {
        CCNode* node = nullptr;
        unsigned count = 0;
    };

    enum class InputKind { Tap, Wave, Robot, Other };

    struct ChannelState {
        std::deque<QueuedInput> queue;
        int shift = 0;
        int releaseShift = 0;
        bool realInput = false;
        bool appliedInput = false;
        int sinceInput = 999;
        int rapid = 0;
        int pressTick = -1;
        InputKind pressKind = InputKind::Other;

        void resetPending() {
            queue.clear();
            shift = 0;
            releaseShift = 0;
        }

        void fullReset() {
            resetPending();
            realInput = false;
            appliedInput = false;
            sinceInput = 999;
            rapid = 0;
            pressTick = -1;
            pressKind = InputKind::Other;
        }
    };

    struct Buffer {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLayer* ghostLayer = nullptr;
        unsigned ghostLayerBase = 0;
        PlayLayer* owner = nullptr;

        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;
        bool ghostSwitched = false;
        bool lastRunSwitched = false;
        bool ghostSolo = false;
        bool simSecond = false;
        bool ghostNeedsModeRestore1 = false;
        bool ghostNeedsModeRestore2 = false;
        bool haveStartRects = false;
        bool g1IsRealP2 = false;

        std::atomic<int> stageIndex { 1 }; // Default: 4 ticks
        std::atomic<int> ticksAll { 4 };
        std::atomic<int> ticksWave { 4 };

        ChannelState chan[2];
        float tickLength = TICK;

        // Strict sub-millisecond frame budget + anti-cascade continuation
        static constexpr double budgetMs = 0.75;
        static constexpr int groundHorizon = 110;
        static constexpr int waveHorizon = 60;
        static constexpr bool assumeOrbs = true;
        static constexpr bool tryBothOrbs = true;
        static constexpr bool orbTiming = true;
        static constexpr bool orbAlign = true;
        static constexpr int orbWindow = 20;
        static constexpr int spamGap = 18;
        static constexpr int shipSpamGap = 6;
        static constexpr int lateCrash = 90;
        static constexpr bool portalPredict = true;
        static constexpr bool predictMovers = true;
        static constexpr bool predictTriggers = true;
        static constexpr bool waveMargin = true;
        static constexpr bool nextClick = true;
        static constexpr bool noHitch = true;
        static constexpr bool ignoreTouching = true;
        static constexpr bool cbfSync = true;
        static constexpr bool spiderSmart = true;
        static constexpr bool ufoSmart = true;
        static constexpr bool ballSmart = true;
        static constexpr bool swingSmart = true;
        static constexpr bool robotSmart = true;

        // Transient simulation state
        bool held1 = false, held2 = false;
        bool auto1 = false, auto2 = false;
        int autoAt1 = -1, autoAt2 = -1;
        int autoHold1 = 0, autoHold2 = 0;
        int orbClickDelay = 0;
        bool orbClick = true;
        bool touchedOrb = false;
        bool orbAfterFirst = false;
        bool laterOrbTouched = false;
        bool skipPress = false;
        bool haveRealRect = false;
        CCRect realRect1, realRect2;
        float ghostShiftX = 0.f;
        float firstStep = 1.f;
        bool inRealUpdate = false;
        float tickStartCoord[2] = { 0.f, 0.f };
        float tickDeltaCoord[2] = { 0.f, 0.f };
        int retapAt = -1;
        int simTick = 0;
        int currentDelay = 0;
        int firstOrbTick = -1;
        int lastOrbTick = -1;
        int orbUsedTick = -1;
        bool captureBase = false;
        bool baseTouchedDown = false;
        bool ghostUsedParticles = false;

        std::vector<ObjSnapshot> snapshots;
        std::vector<std::pair<GameObject*, uint8_t>> simUsed;
        std::vector<std::pair<GameObject*, uint8_t>> orbsUsed;
        std::vector<GameObject*> simDisabled;
        std::vector<GameObject*> simEnabled;
        std::vector<GameObject*> harmless;
        NodeMark levelMarks[3] = {};
        int levelMarkCount = 0;
    } buf;

    template <typename T>
    inline bool vecContains(std::vector<T> const& v, T const& item) {
        return std::find(v.begin(), v.end(), item) != v.end();
    }

    // -------------------------------------------------------------------------
    // External Win32 GUI Window (Zero Gameplay PostMessage Traffic)
    // -------------------------------------------------------------------------
    std::atomic<HWND> g_extHwnd { nullptr };
    HWND g_radioBtns[STAGE_COUNT] = {};
    HWND g_statusLabel = nullptr;

    constexpr UINT WM_APP_SYNC_STAGE = WM_APP + 1;

    void setStageByIndex(int idx);

    LRESULT CALLBACK ExtWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_CREATE: {
                HFONT hFontTitle = CreateFontA(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
                HFONT hFont = CreateFontA(16, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

                HWND title = CreateWindowExA(0, "STATIC", "Select Buffer Window (Ticks):",
                    WS_CHILD | WS_VISIBLE, 16, 14, 255, 22, hwnd, nullptr, nullptr, nullptr);
                SendMessageA(title, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

                char const* labels[STAGE_COUNT] = {
                    "2 Ticks  (0.5 Frames @ 60Hz)",
                    "4 Ticks  (1.0 Frame  @ 60Hz)",
                    "8 Ticks  (2.0 Frames @ 60Hz)",
                    "16 Ticks (4.0 Frames @ 60Hz)",
                    "32 Ticks (8.0 Frames @ 60Hz)"
                };

                int currentIdx = std::clamp(buf.stageIndex.load(), 0, STAGE_COUNT - 1);
                for (int i = 0; i < STAGE_COUNT; ++i) {
                    DWORD style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0);
                    g_radioBtns[i] = CreateWindowExA(0, "BUTTON", labels[i], style,
                        20, 44 + i * 28, 245, 24, hwnd, (HMENU)(intptr_t)(100 + i), nullptr, nullptr);
                    SendMessageA(g_radioBtns[i], WM_SETFONT, (WPARAM)hFont, TRUE);
                    SendMessageA(g_radioBtns[i], BM_SETCHECK, (i == currentIdx) ? BST_CHECKED : BST_UNCHECKED, 0);
                }

                g_statusLabel = CreateWindowExA(0, "STATIC", "Active: 4 Ticks (F6 to cycle)",
                    WS_CHILD | WS_VISIBLE, 16, 190, 260, 20, hwnd, nullptr, nullptr, nullptr);
                SendMessageA(g_statusLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(wParam);
                if (id >= 100 && id < 100 + STAGE_COUNT && HIWORD(wParam) == BN_CLICKED) {
                    setStageByIndex(id - 100);
                }
                return 0;
            }
            case WM_APP_SYNC_STAGE: {
                int idx = std::clamp(static_cast<int>(wParam), 0, STAGE_COUNT - 1);
                for (int i = 0; i < STAGE_COUNT; ++i) {
                    if (g_radioBtns[i]) {
                        SendMessageA(g_radioBtns[i], BM_SETCHECK, (i == idx) ? BST_CHECKED : BST_UNCHECKED, 0);
                    }
                }
                if (g_statusLabel) {
                    auto txt = fmt::format("Active: {} Ticks (F6 to cycle)", ALLOWED_TICKS[idx]);
                    SetWindowTextA(g_statusLabel, txt.c_str());
                }
                return 0;
            }
            case WM_CLOSE:
                ShowWindow(hwnd, SW_MINIMIZE);
                return 0;
            default:
                return DefWindowProcA(hwnd, msg, wParam, lParam);
        }
    }

    void spawnExternalWindow() {
        std::thread([]() {
            HINSTANCE hInst = GetModuleHandleA(nullptr);
            WNDCLASSEXA wc = {};
            wc.cbSize = sizeof(WNDCLASSEXA);
            wc.lpfnWndProc = ExtWndProc;
            wc.hInstance = hInst;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = "CBFExternalMasterWnd";
            RegisterClassExA(&wc);

            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT rc = { 0, 0, 290, 224 };
            AdjustWindowRect(&rc, style, FALSE);

            HWND hwnd = CreateWindowExA(
                0, wc.lpszClassName, "Click Before Frames", style,
                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                nullptr, nullptr, hInst, nullptr
            );

            if (hwnd) {
                g_extHwnd.store(hwnd, std::memory_order_relaxed);
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                UpdateWindow(hwnd);
                PostMessageA(hwnd, WM_APP_SYNC_STAGE, buf.stageIndex.load(std::memory_order_relaxed), 0);

                MSG msg;
                while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
            }
        }).detach();
    }

    void setStageByIndex(int idx) {
        idx = std::clamp(idx, 0, STAGE_COUNT - 1);
        buf.stageIndex.store(idx, std::memory_order_relaxed);
        int ticks = ALLOWED_TICKS[idx];
        buf.ticksAll.store(ticks, std::memory_order_relaxed);
        buf.ticksWave.store(ticks >= 24 ? 16 : ticks, std::memory_order_relaxed);
        if (HWND hwnd = g_extHwnd.load(std::memory_order_relaxed)) {
            PostMessageA(hwnd, WM_APP_SYNC_STAGE, idx, 0);
        }
    }

    void cycleStage() {
        int nextIdx = (buf.stageIndex.load(std::memory_order_relaxed) + 1) % STAGE_COUNT;
        setStageByIndex(nextIdx);
    }

    // -------------------------------------------------------------------------
    // Timing, Rate Scaling & Basic Ghost Helpers
    // -------------------------------------------------------------------------
    std::chrono::steady_clock::time_point clickStart;
    std::chrono::steady_clock::time_point lastResumeTime {};
    int clickTimerDepth = 0;
    int tickCount = 0;

    struct ClickTimer {
        ClickTimer() {
            if (++clickTimerDepth == 1) {
                clickStart = std::chrono::steady_clock::now();
            }
        }
        ~ClickTimer() {
            --clickTimerDepth;
        }
    };

    inline double clickMs() {
        return clickTimerDepth > 0
            ? std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clickStart).count()
            : 0.0;
    }

    inline bool clickOverTime() {
        return clickMs() >= Buffer::budgetMs;
    }

    inline double searchBudget() {
        return clickTimerDepth > 0 ? std::max(0.0, Buffer::budgetMs - clickMs()) : Buffer::budgetMs;
    }

    float stepScale() {
        float dt = buf.tickLength;
        if (!(dt > 0.05f && dt < 1.f)) {
            return 1.f;
        }
        return std::clamp(TICK / dt, 0.5f, 2.f);
    }

    int st(int ticks240) {
        if (ticks240 <= 0) return ticks240;
        return std::max(1, static_cast<int>(std::lround(ticks240 * stepScale())));
    }

    bool isGhost(PlayerObject* p) {
        return p && (p == buf.ghost1 || p == buf.ghost2);
    }

    float travelCoord(PlayerObject* p) {
        auto pos = p->getPosition();
        return p->m_isSideways ? pos.y : pos.x;
    }

    bool gameHolds(PlayerObject* player) {
        if (!player) return false;
        auto it = player->m_holdingButtons.find(static_cast<int>(PlayerButton::Jump));
        return it != player->m_holdingButtons.end() && it->second;
    }

    void syncHeld(PlayLayer* layer) {
        buf.chan[0].resetPending();
        buf.chan[1].resetPending();
        buf.chan[0].realInput = buf.chan[0].appliedInput = gameHolds(layer->m_player1);
        buf.chan[1].realInput = buf.chan[1].appliedInput = gameHolds(layer->m_player2);
    }

    // -------------------------------------------------------------------------
    // L1-Cache Contiguous Mover Radar & Zero-Alloc Trigger Engine
    // -------------------------------------------------------------------------
    struct IndexedCollider {
        GameObject* object = nullptr;
        double startX = 0.0;
    };

    struct WatchedEntry {
        GameObject* object = nullptr;
        double x = 0.0, y = 0.0;
        double vx = 0.0, vy = 0.0;
        double pvx = 0.0, pvy = 0.0;
        double rot = 0.0, vrot = 0.0;
        int movedTick = -200;
    };

    struct Mover {
        GameObject* object = nullptr;
        double x = 0.0, y = 0.0, vx = 0.0, vy = 0.0;
        double turn = 0.0;
        double cosTurn = 1.0, sinTurn = 0.0;
        double rotX = 0.0, rotY = 0.0, vrot = 0.0;
        double ax = 0.0, ay = 0.0;
        int accelSteps = 0;
        int curStep = 0;
        double curDx = 0.0, curDy = 0.0;
        double stepVx = 0.0, stepVy = 0.0;
        bool dirty = false;
    };

    enum class TriggerKind : uint8_t { Move, Rotate, Toggle, Spawn };

    struct LevelTrigger {
        EffectGameObject* trigger = nullptr;
        double x = 0.0;
        TriggerKind kind = TriggerKind::Move;
        bool touchTriggered = false;
        CCRect touchRect {};
    };

    struct FiredMove {
        EffectGameObject* trigger = nullptr;
        int tick = 0;
    };

    struct ScriptedMove {
        TriggerKind kind = TriggerKind::Move;
        bool touchTriggered = false;
        bool lockPlayerX = false, lockPlayerY = false;
        bool hasCenter = false;
        bool lockRot = false;
        bool on = false;
        bool initArmed = false;
        bool armed = false;
        bool toggled = false;
        int initFireStep = -1;
        int fireStep = -1;
        int delay = 0;
        int easing = 0;
        double x = 0.0;
        CCRect touchRect {};
        double dx = 0.0, dy = 0.0;
        double anchorPlayerX = 0.0, anchorPlayerY = 0.0;
        double angle = 0.0;
        double cx = 0.0, cy = 0.0;
        double steps = 0.0;
        double rate = 0.0;
        double from = 0.0;
        CCArray* group = nullptr;

        // Precomputed per-tick values so easeMove/sin/cos run once per trigger, not per object
        bool stepActive = false;
        bool stepActiveLock = false;
        double stepF = 0.0;
        double stepAngle = 0.0;
        double stepCos = 1.0, stepSin = 0.0;
    };

    struct ScriptedObject {
        GameObject* object = nullptr;
        double x = 0.0, y = 0.0;
        CCPoint node {};
        float rotX = 0.f, rotY = 0.f;
        bool mover = false;
        bool placed = false;
        bool rotated = false;
        uint8_t moveCount = 0;
        uint8_t moves[8] = {};
        double lastX = -1e30, lastY = -1e30, lastSpin = -1e30;
    };

    struct PendingSpawn {
        int group = 0;
        int at = 0;
    };

    constexpr size_t ACTIVE_WATCH_CAP = 64;
    constexpr int SCRIPTED_TRIGGER_LIMIT = 32;
    constexpr int SCRIPTED_OBJECT_LIMIT = 96;

    std::vector<WatchedEntry> watched;
    std::vector<Mover> movers;
    std::vector<IndexedCollider> levelColliders;
    std::vector<IndexedCollider> dynamicColliders;
    std::vector<LevelTrigger> levelTriggers;
    GJBaseGameLayer* indexedLayer = nullptr;
    bool levelIsIndexed = false;

    std::vector<FiredMove> firedMoves;
    std::vector<PendingSpawn> pendingSpawns;
    std::vector<ScriptedMove> scriptedMoves;
    std::vector<ScriptedObject> scriptedObjects;
    int clickWorldDepth = 0;
    int lastPruneTick = -1;
    size_t dynScanCursor = 0;

    void forgetLevelIndex() {
        watched.clear();
        movers.clear();
        levelColliders.clear();
        dynamicColliders.clear();
        levelTriggers.clear();
        indexedLayer = nullptr;
        levelIsIndexed = false;
        firedMoves.clear();
        pendingSpawns.clear();
        scriptedMoves.clear();
        scriptedObjects.clear();
        clickWorldDepth = 0;
        lastPruneTick = -1;
        dynScanCursor = 0;
    }

    void pruneExpiredTriggers() {
        if (!firedMoves.empty()) {
            float tl = (buf.tickLength > 0.05f && buf.tickLength < 1.f) ? buf.tickLength : TICK;
            std::erase_if(firedMoves, [&](FiredMove const& fm) {
                int elapsed = tickCount - fm.tick;
                return !fm.trigger || elapsed < 0 || elapsed * tl > fm.trigger->m_duration * 60.f + 1.f;
            });
        }
        if (!pendingSpawns.empty()) {
            std::erase_if(pendingSpawns, [](PendingSpawn const& p) { return p.at < tickCount; });
        }
    }

    bool plainTrigger(EffectGameObject* t, bool rotate) {
        if (t->m_targetGroupID <= 0 || t->m_isDynamicMode) return false;
        if (rotate) {
            return t->m_rotationTargetID <= 0 && (t->m_rotationDegrees != 0.f || t->m_times360 != 0);
        }
        if (t->m_useMoveTarget || t->m_lockToCameraX || t->m_lockToCameraY) return false;
        return t->m_lockToPlayerX || t->m_lockToPlayerY || t->m_moveOffset.x != 0.f || t->m_moveOffset.y != 0.f;
    }

    bool plainSpawn(EffectGameObject* t) {
        auto* s = static_cast<SpawnTriggerGameObject*>(t);
        return t->m_targetGroupID > 0 && s->m_delayRange <= 0.f && s->m_remapObjects.empty() && !t->m_spawnOrdered;
    }

    int spawnSteps(EffectGameObject* t, float tickLength) {
        return static_cast<int>(std::lround(std::max(0.f, static_cast<SpawnTriggerGameObject*>(t)->m_spawnDelay) * 60.0 / tickLength));
    }

    void ensureLevelIndexed(GJBaseGameLayer* layer) {
        if (layer == indexedLayer && levelIsIndexed) return;
        unsigned count = layer->m_objects ? layer->m_objects->count() : 0;
        if (count == 0) return;

        indexedLayer = layer;
        levelIsIndexed = true;
        levelColliders.clear();
        dynamicColliders.clear();
        levelTriggers.clear();
        watched.clear();
        dynScanCursor = 0;

        constexpr uint64_t trackMask = COLLIDING_TYPES | INTERACT_TYPES;
        levelColliders.reserve(std::min<unsigned>(count / 2 + 64, 16384));
        std::vector<int> moveGroups;

        for (auto* object : CCArrayExt<GameObject*>(layer->m_objects)) {
            if (!object) continue;
            int id = object->m_objectID;
            if (id == 901 || id == 1346 || id == 1049 || id == 1268) {
                auto* t = static_cast<EffectGameObject*>(object);
                if ((id == 901 || id == 1346) && t->m_targetGroupID > 0) {
                    moveGroups.push_back(t->m_targetGroupID);
                }
                if (!t->m_isSpawnTriggered) {
                    bool touch = t->m_isTouchTriggered;
                    CCRect tr = touch ? t->getObjectRect() : CCRect {};
                    if (id == 1049 && t->m_targetGroupID > 0) {
                        levelTriggers.push_back({ t, t->m_positionX, TriggerKind::Toggle, touch, tr });
                    } else if (id == 1268 && plainSpawn(t)) {
                        levelTriggers.push_back({ t, t->m_positionX, TriggerKind::Spawn, touch, tr });
                    } else if ((id == 901 || id == 1346) && plainTrigger(t, id == 1346)) {
                        levelTriggers.push_back({ t, t->m_positionX, id == 1346 ? TriggerKind::Rotate : TriggerKind::Move, touch, tr });
                    }
                }
            }
            if (!object->m_isDecoration && object->m_objectType != GameObjectType::Decoration) {
                auto type = static_cast<unsigned>(object->m_objectType);
                uint64_t bit = type < 64 ? (1ull << type) : 0;
                if (trackMask & bit) {
                    levelColliders.push_back({ object, object->m_positionX });
                }
            }
        }
        std::sort(levelTriggers.begin(), levelTriggers.end(), [](auto const& a, auto const& b) { return a.x < b.x; });
        std::sort(levelColliders.begin(), levelColliders.end(), [](IndexedCollider const& a, IndexedCollider const& b) {
            return a.startX < b.startX;
        });
        std::sort(moveGroups.begin(), moveGroups.end());
        moveGroups.erase(std::unique(moveGroups.begin(), moveGroups.end()), moveGroups.end());
        for (int gid : moveGroups) {
            auto* group = layer->getGroup(gid);
            if (!group) continue;
            for (auto* object : CCArrayExt<GameObject*>(group)) {
                if (!object || object->m_isDecoration || object->m_objectType == GameObjectType::Decoration) continue;
                auto type = static_cast<unsigned>(object->m_objectType);
                uint64_t bit = type < 64 ? (1ull << type) : 0;
                if (!(trackMask & bit)) continue;
                if (dynamicColliders.size() >= 2048) break;
                dynamicColliders.push_back({ object, object->m_positionX });
            }
        }
        std::sort(dynamicColliders.begin(), dynamicColliders.end(), [](IndexedCollider const& a, IndexedCollider const& b) {
            return a.object < b.object;
        });
        dynamicColliders.erase(std::unique(dynamicColliders.begin(), dynamicColliders.end(),
            [](IndexedCollider const& a, IndexedCollider const& b) { return a.object == b.object; }), dynamicColliders.end());
    }

    void updateWatched(GJBaseGameLayer* layer) {
        if ((tickCount & 15) == 0 && lastPruneTick != tickCount) {
            lastPruneTick = tickCount;
            pruneExpiredTriggers();
        }
        if (!Buffer::predictMovers || !layer || !layer->m_player1) return;
        ensureLevelIndexed(layer);
        if (levelColliders.empty()) return;

        double px = layer->m_player1->getPosition().x;
        double py = layer->m_player1->getPosition().y;
        bool left = layer->m_player1->m_isGoingLeft;

        if ((tickCount & 15) == 0 || watched.empty()) {
            double minX = left ? (px - 380.0) : (px - 80.0);
            double maxX = left ? (px + 80.0) : (px + 380.0);

            std::erase_if(watched, [&](WatchedEntry const& e) {
                return !e.object || e.object->m_isDisabled ||
                       e.object->m_positionX < minX - 90.0 || e.object->m_positionX > maxX + 90.0 ||
                       std::abs(e.object->m_positionY - py) > 300.0;
            });

            double searchMinX = minX - 160.0;
            double searchMaxX = maxX + 160.0;
            auto lo = std::lower_bound(levelColliders.begin(), levelColliders.end(), searchMinX,
                [](IndexedCollider const& c, double x) { return c.startX < x; });

            int scanned = 0;
            for (auto it = lo; it != levelColliders.end() && it->startX <= searchMaxX && watched.size() < ACTIVE_WATCH_CAP; ++it) {
                if (++scanned > 220) break;
                auto* obj = it->object;
                if (obj->m_isDisabled || obj->m_positionX < minX || obj->m_positionX > maxX || std::abs(obj->m_positionY - py) > 260.0) {
                    continue;
                }
                bool exists = false;
                for (auto const& w : watched) {
                    if (w.object == obj) { exists = true; break; }
                }
                if (!exists) {
                    watched.push_back({ obj, obj->m_positionX, obj->m_positionY, 0.0, 0.0, 0.0, 0.0, obj->getRotationX(), 0.0, -200 });
                }
            }

            // Bounded rolling scan over dynamicColliders (max 128 checks per 16 ticks)
            if (!dynamicColliders.empty() && watched.size() < ACTIVE_WATCH_CAP) {
                size_t const totalDyn = dynamicColliders.size();
                size_t const maxCheck = std::min<size_t>(totalDyn, 128);
                for (size_t i = 0; i < maxCheck && watched.size() < ACTIVE_WATCH_CAP; ++i) {
                    dynScanCursor = (dynScanCursor + 1) % totalDyn;
                    auto* obj = dynamicColliders[dynScanCursor].object;
                    if (!obj || obj->m_isDisabled || obj->m_positionX < minX || obj->m_positionX > maxX ||
                        std::abs(obj->m_positionY - py) > 260.0) {
                        continue;
                    }
                    bool exists = false;
                    for (auto const& w : watched) {
                        if (w.object == obj) { exists = true; break; }
                    }
                    if (!exists) {
                        watched.push_back({ obj, obj->m_positionX, obj->m_positionY, 0.0, 0.0, 0.0, 0.0, obj->getRotationX(), 0.0, -200 });
                    }
                }
            }
        }

        for (auto& w : watched) {
            auto* object = w.object;
            double rot = object->getRotationX();
            if (object->m_positionX != w.x || object->m_positionY != w.y || rot != w.rot) {
                bool consecutive = (tickCount - w.movedTick == 1);
                w.pvx = consecutive ? w.vx : 0.0;
                w.pvy = consecutive ? w.vy : 0.0;
                w.vx = object->m_positionX - w.x;
                w.vy = object->m_positionY - w.y;
                w.vrot = std::remainder(rot - w.rot, 360.0);
                w.x = object->m_positionX;
                w.y = object->m_positionY;
                w.rot = rot;
                w.movedTick = tickCount;
            } else if (tickCount - w.movedTick > 1) {
                w.vx = w.vy = w.pvx = w.pvy = w.vrot = 0.0;
            }
        }
    }

    void collectMoversOnce() {
        movers.clear();
        if (!Buffer::predictMovers) return;
        for (auto const& w : watched) {
            if ((w.vx != 0.0 || w.vy != 0.0 || w.vrot != 0.0) && tickCount - w.movedTick <= 1) {
                auto* object = w.object;
                Mover m;
                m.object = object;
                m.x = object->m_positionX;
                m.y = object->m_positionY;
                m.vx = w.vx;
                m.vy = w.vy;
                m.stepVx = w.vx;
                m.stepVy = w.vy;
                double speed = std::hypot(w.vx, w.vy);
                double prev = std::hypot(w.pvx, w.pvy);
                if (speed > 1e-6 && prev > 1e-6) {
                    double turn = std::remainder(std::atan2(w.vy, w.vx) - std::atan2(w.pvy, w.pvx), 6.283185307179586);
                    double ratio = speed / prev;
                    if (std::abs(turn) > 1e-4 && std::abs(turn) < 0.5 && ratio > 0.8 && ratio < 1.25) {
                        m.turn = turn;
                        m.cosTurn = std::cos(turn);
                        m.sinTurn = std::sin(turn);
                    } else if (std::abs(turn) <= 1e-4 && std::abs(ratio - 1.0) > 1e-3 && ratio > 0.5 && ratio < 2.0) {
                        m.ax = w.vx - w.pvx;
                        m.ay = w.vy - w.pvy;
                        double along = (m.ax * w.vx + m.ay * w.vy) / speed;
                        double steps = along < 0.0 ? speed / -along : 2.0 * speed / along;
                        m.accelSteps = static_cast<int>(std::min(steps, 1000.0));
                    }
                }
                m.rotX = object->getRotationX();
                m.rotY = object->getRotationY();
                m.vrot = w.vrot;
                movers.push_back(m);
            }
        }
    }

    void placeMovers(int steps, double ghostX = 0.0, double ghostY = 0.0) {
        for (auto& m : movers) {
            if (steps == 0) {
                m.curStep = 0;
                m.curDx = 0.0;
                m.curDy = 0.0;
                m.stepVx = m.vx;
                m.stepVy = m.vy;
                if (!m.dirty) continue;
                m.dirty = false;
                if (m.vrot != 0.0) {
                    m.object->setRotationX(static_cast<float>(m.rotX));
                    m.object->setRotationY(static_cast<float>(m.rotY));
                }
                m.object->setPosition({ static_cast<float>(m.x), static_cast<float>(m.y) });
                m.object->m_positionX = m.x;
                m.object->m_positionY = m.y;
                m.object->setObjectRectDirty(true);
                m.object->m_isOrientedBoxDirty = true;
                continue;
            }

            double x = m.x + m.vx * steps;
            double y = m.y + m.vy * steps;
            if (m.accelSteps > 0) {
                int n = std::min(steps, m.accelSteps);
                double grow = 0.5 * n * (n + 1);
                x = m.x + m.vx * steps + m.ax * (grow + static_cast<double>(n) * (steps - n));
                y = m.y + m.vy * steps + m.ay * (grow + static_cast<double>(n) * (steps - n));
            } else if (m.turn != 0.0) {
                if (steps == m.curStep + 1) {
                    double nvx = m.stepVx * m.cosTurn - m.stepVy * m.sinTurn;
                    double nvy = m.stepVx * m.sinTurn + m.stepVy * m.cosTurn;
                    m.stepVx = nvx;
                    m.stepVy = nvy;
                    m.curDx += nvx;
                    m.curDy += nvy;
                    m.curStep = steps;
                } else {
                    m.curDx = 0.0;
                    m.curDy = 0.0;
                    m.stepVx = m.vx;
                    m.stepVy = m.vy;
                    for (int s = 0; s < steps; ++s) {
                        double nvx = m.stepVx * m.cosTurn - m.stepVy * m.sinTurn;
                        double nvy = m.stepVx * m.sinTurn + m.stepVy * m.cosTurn;
                        m.stepVx = nvx;
                        m.stepVy = nvy;
                        m.curDx += nvx;
                        m.curDy += nvy;
                    }
                    m.curStep = steps;
                }
                x = m.x + m.curDx;
                y = m.y + m.curDy;
            }

            m.object->m_positionX = x;
            m.object->m_positionY = y;
            m.object->setObjectRectDirty(true);
            m.object->m_isOrientedBoxDirty = true;
            m.dirty = true;

            // Only update CCNode transform if within collision proximity of ghost
            if (std::abs(x - ghostX) <= 220.0 && std::abs(y - ghostY) <= 220.0) {
                if (m.vrot != 0.0) {
                    double spun = m.vrot * steps;
                    m.object->setRotationX(static_cast<float>(m.rotX + spun));
                    m.object->setRotationY(static_cast<float>(m.rotY + spun));
                }
                m.object->setPosition({ static_cast<float>(x), static_cast<float>(y) });
            }
        }
    }

    double easeMove(int type, double rate, double t) {
        constexpr double PI = 3.141592653589793;
        t = std::clamp(t, 0.0, 1.0);
        if (t <= 0.0 || t >= 1.0) return t;
        rate = rate > 0.0 ? rate : 2.0;
        auto bounce = [](double u) {
            if (u < 1.0 / 2.75) return 7.5625 * u * u;
            if (u < 2.0 / 2.75) { u -= 1.5 / 2.75; return 7.5625 * u * u + 0.75; }
            if (u < 2.5 / 2.75) { u -= 2.25 / 2.75; return 7.5625 * u * u + 0.9375; }
            u -= 2.625 / 2.75;
            return 7.5625 * u * u + 0.984375;
        };
        double period = (rate > 0.0 && rate < 2.0) ? rate : 0.3;
        double s = period / 4.0;
        constexpr double BACK = 1.70158;
        switch (type) {
            case 1: {
                double u = t * 2.0;
                return u < 1.0 ? 0.5 * std::pow(u, rate) : 1.0 - 0.5 * std::pow(2.0 - u, rate);
            }
            case 2: return std::pow(t, rate);
            case 3: return std::pow(t, 1.0 / rate);
            case 4: {
                double u = t * 2.0 - 1.0;
                return u < 0.0 ? -0.5 * std::pow(2.0, 10.0 * u) * std::sin((u - s) * 2.0 * PI / period)
                               : std::pow(2.0, -10.0 * u) * std::sin((u - s) * 2.0 * PI / period) * 0.5 + 1.0;
            }
            case 5: {
                double u = t - 1.0;
                return -std::pow(2.0, 10.0 * u) * std::sin((u - s) * 2.0 * PI / period);
            }
            case 6: return std::pow(2.0, -10.0 * t) * std::sin((t - s) * 2.0 * PI / period) + 1.0;
            case 7: return t < 0.5 ? (1.0 - bounce(1.0 - t * 2.0)) * 0.5 : bounce(t * 2.0 - 1.0) * 0.5 + 0.5;
            case 8: return 1.0 - bounce(1.0 - t);
            case 9: return bounce(t);
            case 10: {
                double u = t * 2.0;
                return u < 1.0 ? 0.5 * std::pow(2.0, 10.0 * (u - 1.0)) : 0.5 * (2.0 - std::pow(2.0, -10.0 * (u - 1.0)));
            }
            case 11: return std::pow(2.0, 10.0 * (t - 1.0));
            case 12: return 1.0 - std::pow(2.0, -10.0 * t);
            case 13: return -0.5 * (std::cos(PI * t) - 1.0);
            case 14: return 1.0 - std::cos(t * PI / 2.0);
            case 15: return std::sin(t * PI / 2.0);
            case 16: {
                double o = BACK * 1.525;
                double u = t * 2.0;
                if (u < 1.0) return u * u * ((o + 1.0) * u - o) / 2.0;
                u -= 2.0;
                return u * u * ((o + 1.0) * u + o) / 2.0 + 1.0;
            }
            case 17: return t * t * ((BACK + 1.0) * t - BACK);
            case 18: {
                double u = t - 1.0;
                return u * u * ((BACK + 1.0) * u + BACK) + 1.0;
            }
            default: return t;
        }
    }

    GameObject* groupParentOf(GJBaseGameLayer* layer, int group) {
        if (group <= 0 || !layer->m_parentGroupsDict) return nullptr;
        return typeinfo_cast<GameObject*>(layer->m_parentGroupsDict->objectForKey(group));
    }

    void fillMove(GJBaseGameLayer* layer, EffectGameObject* t, TriggerKind kind, float tickLength, ScriptedMove& m) {
        m.kind = kind;
        m.steps = t->m_duration * 60.0 / tickLength;
        m.easing = static_cast<int>(t->m_easingType);
        m.rate = t->m_easingRate;
        if (kind == TriggerKind::Move) {
            m.dx = t->m_moveOffset.x;
            m.dy = t->m_moveOffset.y;
            m.lockPlayerX = t->m_lockToPlayerX;
            m.lockPlayerY = t->m_lockToPlayerY;
            return;
        }
        m.angle = t->m_rotationDegrees + 360.0 * t->m_times360;
        m.lockRot = t->m_lockObjectRotation;
        GameObject* pivot = nullptr;
        if (t->m_centerGroupID > 0) {
            pivot = groupParentOf(layer, t->m_centerGroupID);
            if (!pivot) {
                auto* center = layer->getGroup(t->m_centerGroupID);
                if (center && center->count() > 0) {
                    pivot = static_cast<GameObject*>(center->objectAtIndex(0));
                }
            }
        } else {
            pivot = groupParentOf(layer, t->m_targetGroupID);
        }
        if (pivot) {
            m.hasCenter = true;
            m.cx = pivot->m_positionX;
            m.cy = pivot->m_positionY;
        }
    }

    void simToggle(CCArray* group, bool on) {
        int seen = 0;
        for (auto* object : CCArrayExt<GameObject*>(group)) {
            if (!object || ++seen > 180) break;
            if (!on) {
                if (buf.simDisabled.size() < 128 && !vecContains(buf.simDisabled, object)) {
                    buf.simDisabled.push_back(object);
                }
                continue;
            }
            std::erase(buf.simDisabled, object);
            if (object->m_isGroupDisabled && buf.simEnabled.size() < 96 && !vecContains(buf.simEnabled, object)) {
                buf.simEnabled.push_back(object);
            }
        }
    }

    void collectScriptedOnce(GJBaseGameLayer* layer, PlayerObject* refPlayer, int maxTotalTicks) {
        scriptedMoves.clear();
        scriptedObjects.clear();
        if (!Buffer::predictTriggers || !refPlayer) return;

        ensureLevelIndexed(layer);
        bool goingLeft = refPlayer->m_isGoingLeft;
        double px = refPlayer->getPositionX();
        double py = refPlayer->getPositionY();
        double speedStep = std::max(static_cast<double>(std::abs(buf.tickDeltaCoord[0])), 2.2);
        double reach = (maxTotalTicks + 4) * speedStep + 35.0;
        float tickLength = (buf.tickLength > 0.05f && buf.tickLength < 1.f) ? buf.tickLength : TICK;

        double pathMinX = goingLeft ? (px - reach - 60.0) : (px - 60.0);
        double pathMaxX = goingLeft ? (px + 60.0) : (px + reach + 60.0);
        double pathMinY = py - 260.0, pathMaxY = py + 260.0;

        int totalGroupInspected = 0;
        int totalChainVisited = 0;

        auto nearPath = [&](GameObject* object, ScriptedMove const& m) {
            if (m.lockPlayerX || m.lockPlayerY) {
                return std::abs(object->m_positionX - px) <= 320.0 && std::abs(object->m_positionY - py) <= 260.0;
            }
            double x = object->m_positionX, y = object->m_positionY;
            double x0 = x, x1 = x, y0 = y, y1 = y;
            if (m.kind == TriggerKind::Move) {
                x0 = std::min(x, x + m.dx); x1 = std::max(x, x + m.dx);
                y0 = std::min(y, y + m.dy); y1 = std::max(y, y + m.dy);
            } else if (m.hasCenter) {
                double r = std::hypot(x - m.cx, y - m.cy);
                x0 = m.cx - r; x1 = m.cx + r;
                y0 = m.cy - r; y1 = m.cy + r;
            }
            constexpr double SIZE = 55.0;
            return x1 + SIZE >= pathMinX && x0 - SIZE <= pathMaxX && y1 + SIZE >= pathMinY && y0 - SIZE <= pathMaxY;
        };

        auto findOrAddScriptedObj = [&](GameObject* object) -> ScriptedObject* {
            for (auto& so : scriptedObjects) {
                if (so.object == object) return &so;
            }
            if (scriptedObjects.size() >= SCRIPTED_OBJECT_LIMIT) return nullptr;
            bool isMover = std::any_of(movers.begin(), movers.end(), [&](Mover const& mv) { return mv.object == object; });
            scriptedObjects.push_back({ object, object->m_positionX, object->m_positionY, object->getPosition(),
                object->getRotationX(), object->getRotationY(), isMover });
            return &scriptedObjects.back();
        };

        auto addMove = [&](EffectGameObject* t, double x, TriggerKind kind, bool armed, int when, bool touch, CCRect const& tr) {
            if (static_cast<int>(scriptedMoves.size()) >= SCRIPTED_TRIGGER_LIMIT ||
                totalGroupInspected > 700 || clickMs() > Buffer::budgetMs * 0.22) {
                return;
            }
            int elapsed = (armed && when < 0) ? -when : -1;
            auto* group = layer->getGroup(t->m_targetGroupID);
            if (!group || group->count() == 0) return;

            ScriptedMove m;
            m.kind = kind;
            m.x = x;
            m.touchTriggered = touch;
            m.touchRect = tr;
            m.initArmed = armed;
            m.armed = armed;
            m.initFireStep = armed ? when : -1;
            m.fireStep = m.initFireStep;
            m.delay = armed ? 0 : when;
            m.anchorPlayerX = px;
            m.anchorPlayerY = py;

            if (kind == TriggerKind::Toggle) {
                m.group = group;
                m.on = t->m_activateGroup;
                scriptedMoves.push_back(m);
                return;
            }
            fillMove(layer, t, kind, tickLength, m);
            if (elapsed >= 0) {
                if (m.steps < 0.5 || elapsed >= m.steps) return;
                m.from = easeMove(m.easing, m.rate, elapsed / m.steps);
            }
            uint8_t moveIdx = static_cast<uint8_t>(scriptedMoves.size());
            bool any = false;
            int groupSeen = 0;
            for (auto* object : CCArrayExt<GameObject*>(group)) {
                if (!object || ++groupSeen > 96 || ++totalGroupInspected > 700) break;
                if (object == t || object->m_isDecoration || object->m_objectType == GameObjectType::Decoration || !nearPath(object, m)) {
                    continue;
                }
                bool isMover = std::any_of(movers.begin(), movers.end(), [&](Mover const& mv) { return mv.object == object; });
                if (armed && when < 0 && isMover && !m.lockPlayerX && !m.lockPlayerY) {
                    continue;
                }
                if (auto* so = findOrAddScriptedObj(object)) {
                    if (so->moveCount < 8) {
                        so->moves[so->moveCount++] = moveIdx;
                        any = true;
                    }
                }
            }
            if (any) {
                scriptedMoves.push_back(m);
            }
        };

        for (auto const& fm : firedMoves) {
            auto* t = fm.trigger;
            if (!t) continue;
            int elapsed = tickCount - fm.tick;
            if (elapsed < 0 || elapsed * tickLength > t->m_duration * 60.f + 1.f) {
                continue;
            }
            bool rotate = (t->m_objectID == 1346);
            if (plainTrigger(t, rotate)) {
                addMove(t, 0.0, rotate ? TriggerKind::Rotate : TriggerKind::Move, true, -elapsed, false, {});
            }
        }

        auto spawnChain = [&](auto&& self, int groupID, double x, bool armed, int when, int depth, bool touch, CCRect const& tr) -> void {
            if (depth > 2 || totalChainVisited > 140 || totalGroupInspected > 700 ||
                static_cast<int>(scriptedMoves.size()) >= SCRIPTED_TRIGGER_LIMIT ||
                clickMs() > Buffer::budgetMs * 0.22) {
                return;
            }
            auto* group = layer->getGroup(groupID);
            if (!group) return;
            int seen = 0;
            for (auto* object : CCArrayExt<GameObject*>(group)) {
                if (!object || ++seen > 64 || ++totalChainVisited > 140) break;
                if (static_cast<int>(scriptedMoves.size()) >= SCRIPTED_TRIGGER_LIMIT) break;
                int id = object->m_objectID;
                if (id != 901 && id != 1346 && id != 1049 && id != 1268) continue;
                auto* c = static_cast<EffectGameObject*>(object);
                if (!c->m_isSpawnTriggered) continue;
                if (id == 1268) {
                    if (plainSpawn(c)) {
                        self(self, c->m_targetGroupID, x, armed, when + spawnSteps(c, tickLength), depth + 1, touch, tr);
                    }
                } else if (id == 1049) {
                    if (c->m_targetGroupID > 0) addMove(c, x, TriggerKind::Toggle, armed, when, touch, tr);
                } else if (plainTrigger(c, id == 1346)) {
                    addMove(c, x, id == 1346 ? TriggerKind::Rotate : TriggerKind::Move, armed, when, touch, tr);
                }
            }
        };

        if (!pendingSpawns.empty()) {
            for (auto const& p : pendingSpawns) {
                if (p.at >= tickCount) {
                    spawnChain(spawnChain, p.group, 0.0, true, p.at - tickCount, 0, false, {});
                }
            }
        }

        double trigMinX = goingLeft ? (px - reach) : (px - 2.0);
        double trigMaxX = goingLeft ? (px + 2.0) : (px + reach);
        auto it = std::lower_bound(levelTriggers.begin(), levelTriggers.end(), trigMinX,
            [](auto const& m, double x) { return m.x < x; });
        for (; it != levelTriggers.end() && it->x <= trigMaxX; ++it) {
            if (static_cast<int>(scriptedMoves.size()) >= SCRIPTED_TRIGGER_LIMIT ||
                totalGroupInspected > 700 || clickMs() > Buffer::budgetMs * 0.22) {
                break;
            }
            if (it->trigger->m_activated) continue;
            if (it->kind == TriggerKind::Spawn) {
                spawnChain(spawnChain, it->trigger->m_targetGroupID, it->x, false, spawnSteps(it->trigger, tickLength), 0, it->touchTriggered, it->touchRect);
            } else {
                addMove(it->trigger, it->x, it->kind, false, 0, it->touchTriggered, it->touchRect);
            }
        }

        if (scriptedMoves.empty()) {
            scriptedObjects.clear();
        }
    }

    inline void resetScriptedForRun(double ghostStartX, double ghostStartY) {
        for (auto& m : scriptedMoves) {
            m.armed = m.initArmed;
            m.fireStep = m.initFireStep;
            m.toggled = false;
            m.stepActive = false;
            if (m.armed && (m.lockPlayerX || m.lockPlayerY)) {
                m.anchorPlayerX = ghostStartX;
                m.anchorPlayerY = ghostStartY;
            }
        }
    }

    inline void restoreScriptedRunPositions() {
        for (auto& o : scriptedObjects) {
            if (o.placed) {
                if (!o.mover) {
                    o.object->setPosition(o.node);
                    o.object->m_positionX = o.x;
                    o.object->m_positionY = o.y;
                }
                if (o.rotated) {
                    o.object->setRotationX(o.rotX);
                    o.object->setRotationY(o.rotY);
                    o.rotated = false;
                }
                o.object->setObjectRectDirty(true);
                o.object->m_isOrientedBoxDirty = true;
            }
            o.placed = false;
            o.lastX = o.lastY = o.lastSpin = -1e30;
        }
    }

    void fireScripted(PlayerObject* g1, PlayerObject* g2, bool second, int tick) {
        double gx = g1->getPositionX();
        double gy = g1->getPositionY();
        bool left = g1->m_isGoingLeft;
        bool haveRects = false;
        CCRect r1, r2;
        int nextStep = tick + 1;

        for (auto& m : scriptedMoves) {
            if (!m.armed) {
                bool reached = false;
                if (m.touchTriggered) {
                    if (!haveRects) {
                        r1 = g1->getObjectRect();
                        if (second && g2) r2 = g2->getObjectRect();
                        haveRects = true;
                    }
                    reached = r1.intersectsRect(m.touchRect) || (second && g2 && r2.intersectsRect(m.touchRect));
                } else {
                    reached = left ? (gx <= m.x) : (gx >= m.x);
                }
                if (reached) {
                    m.armed = true;
                    m.fireStep = tick + m.delay;
                    m.anchorPlayerX = gx;
                    m.anchorPlayerY = gy;
                }
            }
            if (m.kind == TriggerKind::Toggle) {
                if (m.armed && !m.toggled && tick >= m.fireStep && m.group) {
                    m.toggled = true;
                    simToggle(m.group, m.on);
                }
                m.stepActive = false;
                continue;
            }

            if (!m.armed || nextStep <= m.fireStep) {
                m.stepActive = false;
                continue;
            }
            int elapsed = nextStep - m.fireStep;
            m.stepF = (m.steps < 0.5 ? 1.0 : easeMove(m.easing, m.rate, elapsed / m.steps)) - m.from;
            m.stepActiveLock = (m.lockPlayerX || m.lockPlayerY) && (m.steps < 0.5 || elapsed <= m.steps);
            if (m.stepF == 0.0 && !m.stepActiveLock) {
                m.stepActive = false;
                continue;
            }
            m.stepActive = true;
            if (m.kind == TriggerKind::Rotate) {
                m.stepAngle = m.angle * m.stepF;
                if (m.hasCenter) {
                    double th = -m.stepAngle * 3.141592653589793 / 180.0;
                    m.stepCos = std::cos(th);
                    m.stepSin = std::sin(th);
                }
            }
        }
    }

    void placeScripted(double ghostX, double ghostY) {
        for (auto& o : scriptedObjects) {
            double x = o.mover ? o.object->m_positionX : o.x;
            double y = o.mover ? o.object->m_positionY : o.y;
            double spin = 0.0;
            bool any = false;
            for (uint8_t idx = 0; idx < o.moveCount; ++idx) {
                auto const& m = scriptedMoves[o.moves[idx]];
                if (!m.stepActive) continue;
                any = true;
                if (m.kind == TriggerKind::Move) {
                    x += m.dx * m.stepF;
                    y += m.dy * m.stepF;
                    if (m.stepActiveLock) {
                        if (m.lockPlayerX) x += (ghostX - m.anchorPlayerX);
                        if (m.lockPlayerY) y += (ghostY - m.anchorPlayerY);
                    }
                } else if (m.kind == TriggerKind::Rotate) {
                    if (m.hasCenter) {
                        double rx = x - m.cx, ry = y - m.cy;
                        x = m.cx + rx * m.stepCos - ry * m.stepSin;
                        y = m.cy + rx * m.stepSin + ry * m.stepCos;
                    }
                    if (!m.lockRot) {
                        spin += m.stepAngle;
                    }
                }
            }
            if (!any && !o.placed) continue;
            if (o.placed && !o.mover && x == o.lastX && y == o.lastY && spin == o.lastSpin) continue;
            o.lastX = x;
            o.lastY = y;
            o.lastSpin = spin;
            o.object->m_positionX = x;
            o.object->m_positionY = y;
            o.object->setObjectRectDirty(true);
            o.object->m_isOrientedBoxDirty = true;
            o.placed = true;

            if (std::abs(x - ghostX) <= 220.0 && std::abs(y - ghostY) <= 220.0) {
                o.object->setPosition({ static_cast<float>(x), static_cast<float>(y) });
                if (spin != 0.0 || o.rotated) {
                    o.object->setRotationX(static_cast<float>(o.rotX + spin));
                    o.object->setRotationY(static_cast<float>(o.rotY + spin));
                    o.rotated = true;
                }
            }
        }
    }

    void markTopLevelNodes(PlayLayer* layer) {
        buf.levelMarkCount = 0;
        auto add = [](CCNode* n) {
            if (n && buf.levelMarkCount < 3) {
                buf.levelMarks[buf.levelMarkCount++] = { n, n->getChildrenCount() };
            }
        };
        add(layer);
        add(layer->m_objectLayer);
        if (layer->m_aboveShaderObjectLayer && layer->m_aboveShaderObjectLayer != layer->m_objectLayer) {
            add(layer->m_aboveShaderObjectLayer);
        }
    }

    void sweepTopLevelNodes() {
        for (int i = 0; i < buf.levelMarkCount; ++i) {
            auto const& m = buf.levelMarks[i];
            auto* kids = m.node->getChildren();
            while (kids && kids->count() > m.count) {
                m.node->removeChild(static_cast<CCNode*>(kids->lastObject()), true);
            }
        }
        buf.levelMarkCount = 0;
    }

    struct ScopedClickWorld {
        ScopedClickWorld(PlayLayer* layer, PlayerObject* p1, int maxTicks) {
            if (++clickWorldDepth == 1) {
                buf.ghostUsedParticles = false;
                markTopLevelNodes(layer);
                collectMoversOnce();
                collectScriptedOnce(layer, p1, maxTicks);
            }
        }
        ~ScopedClickWorld() {
            if (--clickWorldDepth == 0) {
                restoreScriptedRunPositions();
                if (!movers.empty()) {
                    placeMovers(0);
                    movers.clear();
                }
                scriptedObjects.clear();
                scriptedMoves.clear();
                if (buf.ghost1) {
                    buf.ghost1->stopAllActions();
                    if (buf.ghostUsedParticles) buf.ghost1->stopParticles();
                }
                if (buf.ghost2) {
                    buf.ghost2->stopAllActions();
                    if (buf.ghostUsedParticles) buf.ghost2->stopParticles();
                }
                sweepTopLevelNodes();
            }
        }
    };

    // -------------------------------------------------------------------------
    // Ghost Lifecycle, State Copy & Portal Transitions
    // -------------------------------------------------------------------------
    void dropGhosts() {
        for (auto** g : { &buf.ghost1, &buf.ghost2 }) {
            if (*g) {
                (*g)->removeFromParentAndCleanup(true);
                (*g)->release();
                *g = nullptr;
            }
        }
        if (buf.ghostLayer) {
            buf.ghostLayer->removeAllChildrenWithCleanup(true);
            buf.ghostLayer->release();
            buf.ghostLayer = nullptr;
        }
        buf.simulating = false;
        buf.injecting = false;
        buf.ghostNeedsModeRestore1 = false;
        buf.ghostNeedsModeRestore2 = false;
        buf.owner = nullptr;
        forgetLevelIndex();
        buf.chan[0].fullReset();
        buf.chan[1].fullReset();
    }

    PlayerObject* makeGhost(PlayLayer* layer) {
        if (!buf.ghostLayer) {
            buf.ghostLayer = CCLayer::create();
            buf.ghostLayer->retain();
        }
        auto* ghost = PlayerObject::create(1, 1, layer, buf.ghostLayer, true);
        ghost->retain();
        ghost->setPosition(CCPoint(0.f, 105.f));
        ghost->setVisible(false);
        if (ghost->m_waveTrail) ghost->m_waveTrail->setVisible(false);
        if (ghost->m_regularTrail) ghost->m_regularTrail->setVisible(false);
        buf.ghostLayer->addChild(ghost);
        return ghost;
    }

    void sweepGhostLayer() {
        if (!buf.ghostLayer) return;
        auto* children = buf.ghostLayer->getChildren();
        while (children && children->count() > buf.ghostLayerBase) {
            auto* node = static_cast<CCNode*>(children->lastObject());
            buf.ghostLayer->removeChild(node, true);
        }
    }

    void sendButton(PlayLayer* layer, int chIdx, bool down) {
        buf.injecting = true;
        buf.chan[chIdx].appliedInput = down;
        layer->handleButton(down, 1, chIdx == 0);
        buf.injecting = false;
    }

    void pressGhost(PlayerObject* ghost, bool down) {
        (ghost == buf.ghost1 ? buf.held1 : buf.held2) = down;
        if (down) {
            if (ghost->m_controlsDisabled || ghost->m_isLocked) return;
            ghost->pushButton(PlayerButton::Jump);
        } else {
            ghost->releaseButton(PlayerButton::Jump);
        }
    }

    bool setGhostMode(PlayerObject* ghost, GameObjectType type) {
        using T = GameObjectType;
        switch (type) {
            case T::CubePortal:
            case T::ShipPortal:
            case T::BallPortal:
            case T::UfoPortal:
            case T::WavePortal:
            case T::RobotPortal:
            case T::SpiderPortal:
            case T::SwingPortal:
                break;
            default:
                return false;
        }
        bool now = (type == T::ShipPortal && ghost->m_isShip) ||
                   (type == T::BallPortal && ghost->m_isBall) ||
                   (type == T::UfoPortal && ghost->m_isBird) ||
                   (type == T::WavePortal && ghost->m_isDart) ||
                   (type == T::RobotPortal && ghost->m_isRobot) ||
                   (type == T::SpiderPortal && ghost->m_isSpider) ||
                   (type == T::SwingPortal && ghost->m_isSwing) ||
                   (type == T::CubePortal && !ghost->m_isShip && !ghost->m_isBall && !ghost->m_isBird &&
                    !ghost->m_isDart && !ghost->m_isRobot && !ghost->m_isSpider && !ghost->m_isSwing);
        if (now) return true;

        buf.ghostSwitched = true;
        (ghost == buf.ghost1 ? buf.ghostNeedsModeRestore1 : buf.ghostNeedsModeRestore2) = true;
        if (ghost->m_isShip) ghost->toggleFlyMode(false, true);
        if (ghost->m_isBall) ghost->toggleRollMode(false, true);
        if (ghost->m_isBird) ghost->toggleBirdMode(false, true);
        if (ghost->m_isDart) ghost->toggleDartMode(false, true);
        if (ghost->m_isRobot) ghost->toggleRobotMode(false, true);
        if (ghost->m_isSpider) ghost->toggleSpiderMode(false, true);
        if (ghost->m_isSwing) ghost->toggleSwingMode(false, true);
        switch (type) {
            case T::ShipPortal: ghost->toggleFlyMode(true, true); break;
            case T::BallPortal: ghost->toggleRollMode(true, true); break;
            case T::UfoPortal: ghost->toggleBirdMode(true, true); break;
            case T::WavePortal: ghost->toggleDartMode(true, true); break;
            case T::RobotPortal: ghost->toggleRobotMode(true, true); break;
            case T::SpiderPortal: ghost->toggleSpiderMode(true, true); break;
            case T::SwingPortal: ghost->toggleSwingMode(true, true); break;
            default: break;
        }
        return true;
    }

    void syncGhostModeToReal(PlayerObject* ghost, PlayerObject* real) {
        using T = GameObjectType;
        T realType = real->m_isShip   ? T::ShipPortal
                   : real->m_isBall   ? T::BallPortal
                   : real->m_isBird   ? T::UfoPortal
                   : real->m_isDart   ? T::WavePortal
                   : real->m_isRobot  ? T::RobotPortal
                   : real->m_isSpider ? T::SpiderPortal
                   : real->m_isSwing  ? T::SwingPortal
                                      : T::CubePortal;
        bool savedSwitch = buf.ghostSwitched;
        setGhostMode(ghost, realType);
        buf.ghostSwitched = savedSwitch;
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        bool& needsRestore = (ghost == buf.ghost1) ? buf.ghostNeedsModeRestore1 : buf.ghostNeedsModeRestore2;
        if (needsRestore) {
            syncGhostModeToReal(ghost, real);
            needsRestore = false;
        }
        if (ghost->m_touchingRings && ghost->m_touchingRings->count() > 0) {
            ghost->m_touchingRings->removeAllObjects();
        }
        ghost->copyAttributes(real);
        if (real->m_touchingRings && ghost->m_touchingRings && real->m_touchingRings->count() > 0) {
            ghost->m_touchingRings->addObjectsFromArray(real->m_touchingRings);
        }
        ghost->setScaleX(real->getScaleX());
        ghost->setScaleY(real->getScaleY());
        ghost->setRotation(real->getRotation());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing;
        ghost->m_controlsDisabled = real->m_controlsDisabled;
        ghost->m_isLocked = real->m_isLocked;
        ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;

        CCPoint pos = real->getPosition();
        if (buf.ghostShiftX != 0.f) {
            if (real->m_isSideways) {
                pos.y += (real->m_isGoingLeft ? -buf.ghostShiftX : buf.ghostShiftX);
            } else {
                pos.x += (real->m_isGoingLeft ? -buf.ghostShiftX : buf.ghostShiftX);
            }
        }
        ghost->setPosition(pos);
        ghost->m_position = pos;
        ghost->m_lastPosition = real->m_lastPosition;

        ghost->m_holdingButtons = real->m_holdingButtons;
        ghost->m_jumpBuffered = real->m_jumpBuffered;
        ghost->m_wasJumpBuffered = real->m_wasJumpBuffered;
        ghost->m_stateJumpBuffered = real->m_stateJumpBuffered;
        ghost->m_stateRingJump = real->m_stateRingJump;
        ghost->m_stateRingJump2 = real->m_stateRingJump2;
        ghost->m_wasRobotJump = real->m_wasRobotJump;
        ghost->m_touchedRing = real->m_touchedRing;
        ghost->m_touchedPad = real->m_touchedPad;
        ghost->m_touchedGravityPortal = real->m_touchedGravityPortal;

        ghost->m_isShip = real->m_isShip;
        ghost->m_isBird = real->m_isBird;
        ghost->m_isBall = real->m_isBall;
        ghost->m_isDart = real->m_isDart;
        ghost->m_isRobot = real->m_isRobot;
        ghost->m_isSpider = real->m_isSpider;
        ghost->m_isSwing = real->m_isSwing;
        ghost->m_isGoingLeft = real->m_isGoingLeft;
        ghost->m_isSideways = real->m_isSideways;
        ghost->m_reverseSync = real->m_reverseSync;
        ghost->m_reverseRelated = real->m_reverseRelated;
        ghost->m_maybeReverseSpeed = real->m_maybeReverseSpeed;
        ghost->m_maybeReverseAcceleration = real->m_maybeReverseAcceleration;
        ghost->m_speedMultiplier = real->m_speedMultiplier;
        ghost->m_gravity = real->m_gravity;
        ghost->m_yStart = real->m_yStart;
        ghost->m_rotationSpeed = real->m_rotationSpeed;
        ghost->m_fixGravityBug = real->m_fixGravityBug;
        ghost->m_fixRobotJump = real->m_fixRobotJump;
        ghost->m_enable22Changes = real->m_enable22Changes;
        ghost->m_enableImpulseFix = real->m_enableImpulseFix;
        ghost->m_totalTime = real->m_totalTime;

        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_fallSpeed = real->m_fallSpeed;
        ghost->m_yVelocityBeforeSlope = real->m_yVelocityBeforeSlope;
        ghost->m_yVelocityRelated = real->m_yVelocityRelated;
        ghost->m_yVelocityRelated3 = real->m_yVelocityRelated3;
        ghost->m_groundYVelocity = real->m_groundYVelocity;
        ghost->m_accelerationOrSpeed = real->m_accelerationOrSpeed;
        ghost->m_isAccelerating = real->m_isAccelerating;
        ghost->m_maybeIsFalling = real->m_maybeIsFalling;
        ghost->m_isOnGround2 = real->m_isOnGround2;
        ghost->m_isOnGround3 = real->m_isOnGround3;
        ghost->m_isOnGround4 = real->m_isOnGround4;
        ghost->m_stateOnGround = real->m_stateOnGround;
        ghost->m_lastLandTime = real->m_lastLandTime;
        ghost->m_lastJumpTime = real->m_lastJumpTime;
        ghost->m_lastFlipTime = real->m_lastFlipTime;
        ghost->m_lastSpiderFlipTime = real->m_lastSpiderFlipTime;
        ghost->m_lastGroundedPos = real->m_lastGroundedPos;
        ghost->m_blackOrbRelated = real->m_blackOrbRelated;
        ghost->m_padRingRelated = real->m_padRingRelated;
        ghost->m_ringJumpRelated = real->m_ringJumpRelated;

        ghost->m_isOnSlope = real->m_isOnSlope;
        ghost->m_wasOnSlope = real->m_wasOnSlope;
        ghost->m_slopeVelocity = real->m_slopeVelocity;
        ghost->m_maybeUpsideDownSlope = real->m_maybeUpsideDownSlope;
        ghost->m_currentSlope = real->m_currentSlope;
        ghost->m_currentSlope2 = real->m_currentSlope2;
        ghost->m_currentPotentialSlope = real->m_currentPotentialSlope;
        ghost->m_isCollidingWithSlope = real->m_isCollidingWithSlope;
        ghost->m_collidingWithSlopeId = real->m_collidingWithSlopeId;
        ghost->m_slopeFlipGravityRelated = real->m_slopeFlipGravityRelated;
        ghost->m_slopeAngle = real->m_slopeAngle;
        ghost->m_slopeAngleRadians = real->m_slopeAngleRadians;
        ghost->m_slopeRotation = real->m_slopeRotation;
        ghost->m_currentSlopeYVelocity = real->m_currentSlopeYVelocity;
        ghost->m_isCurrentSlopeTop = real->m_isCurrentSlopeTop;
        ghost->m_slopeStartTime = real->m_slopeStartTime;
        ghost->m_slopeEndTime = real->m_slopeEndTime;
        ghost->m_slopeSlidingMaybeRotated = real->m_slopeSlidingMaybeRotated;
        ghost->m_stateDartSlide = real->m_stateDartSlide;

        ghost->m_lastGroundObject = real->m_lastGroundObject;
        ghost->m_preLastGroundObject = real->m_preLastGroundObject;
        ghost->m_maybeLastGroundObject = real->m_maybeLastGroundObject;
        ghost->m_collidedObject = real->m_collidedObject;
        ghost->m_objectSnappedTo = real->m_objectSnappedTo;
        ghost->m_snapDistance = real->m_snapDistance;
        ghost->m_lastCollisionBottom = real->m_lastCollisionBottom;
        ghost->m_lastCollisionTop = real->m_lastCollisionTop;
        ghost->m_lastCollisionLeft = real->m_lastCollisionLeft;
        ghost->m_lastCollisionRight = real->m_lastCollisionRight;
        ghost->m_collidedTopMinY = real->m_collidedTopMinY;
        ghost->m_collidedBottomMaxY = real->m_collidedBottomMaxY;
        ghost->m_collidedLeftMaxX = real->m_collidedLeftMaxX;
        ghost->m_collidedRightMinX = real->m_collidedRightMinX;

        ghost->m_dashX = real->m_dashX;
        ghost->m_dashY = real->m_dashY;
        ghost->m_dashAngle = real->m_dashAngle;
        ghost->m_dashStartTime = real->m_dashStartTime;
        ghost->m_dashRing = real->m_dashRing;
        ghost->m_dashFireFrame = real->m_dashFireFrame;

        ghost->m_stateForce = real->m_stateForce;
        ghost->m_stateForceVector = real->m_stateForceVector;
        ghost->m_affectedByForces = real->m_affectedByForces;
        ghost->m_stateBoostX = real->m_stateBoostX;
        ghost->m_stateBoostY = real->m_stateBoostY;
        ghost->m_stateNoStickX = real->m_stateNoStickX;
        ghost->m_stateNoStickY = real->m_stateNoStickY;
        ghost->m_stateHitHead = real->m_stateHitHead;
        ghost->m_stateFlipGravity = real->m_stateFlipGravity;
        ghost->m_stateNoAutoJump = real->m_stateNoAutoJump;
        ghost->m_stateScale = real->m_stateScale;
        ghost->m_maybeStateForce2 = real->m_maybeStateForce2;

        ghost->setObjectRectDirty(true);
        ghost->m_isOrientedBoxDirty = true;
    }

    bool ghostTeleport(PlayerObject* g, GameObject* object) {
        if (object->m_objectType != GameObjectType::TeleportPortal) return false;
        if (object->m_objectID != 747) {
            return object->m_objectID == 749;
        }
        auto* t = static_cast<TeleportPortalObject*>(object);
        if (t->m_staticForceEnabled || t->m_redirectForceEnabled || t->m_redirectDash) {
            return false;
        }
        double dx = 0.0, dy = t->m_teleportYOffset;
        if (t->m_orangePortal && t->m_orangePortal != t) {
            dx = t->m_orangePortal->m_positionX - t->m_positionX;
            dy = t->m_orangePortal->m_positionY - t->m_positionY;
        }
        if (t->m_ignoreX) dx = 0.0;
        if (t->m_ignoreY) dy = 0.0;

        if (dx != 0.0 || dy != 0.0) {
            CCPoint shift { static_cast<float>(dx), static_cast<float>(dy) };
            CCPoint pos = g->getPosition() + shift;
            g->setPosition(pos);
            g->m_position = pos;
            g->m_lastPosition = g->m_lastPosition + shift;
            g->m_isOnGround = false;
            g->setObjectRectDirty(true);
            g->m_isOrientedBoxDirty = true;
        }
        if (t->m_gravityMode == 1) {
            g->flipGravity(false, true);
        } else if (t->m_gravityMode == 2) {
            g->flipGravity(true, true);
        } else if (t->m_gravityMode == 3) {
            g->flipGravity(!g->m_isUpsideDown, true);
        }
        buf.ghostSwitched = true;
        return true;
    }

    bool ghostSoloPortal(GameObject* object) {
        if (object->m_objectType != GameObjectType::SoloPortal) return false;
        buf.ghostSolo = true;
        buf.simSecond = false;
        return true;
    }

    bool ghostDualPortal(PlayerObject* g, GameObject* object) {
        if (object->m_objectType != GameObjectType::DualPortal) return false;
        if (buf.simSecond) return true;
        if (g != buf.ghost1 || !buf.ghost2) return false;
        auto* g2 = buf.ghost2;
        float savedShift = buf.ghostShiftX;
        buf.ghostShiftX = 0.f;
        copyState(g2, g);
        buf.ghostShiftX = savedShift;
        buf.ghostNeedsModeRestore2 = true;
        g2->setPosition(g->getPosition());
        g2->m_position = g->m_position;
        g2->m_lastPosition = g->m_lastPosition;
        g2->flipGravity(!g->m_isUpsideDown, true);
        pressGhost(g2, buf.held1);
        buf.simSecond = true;
        buf.ghostSolo = false;
        buf.ghostSwitched = true;
        return true;
    }

    // -------------------------------------------------------------------------
    // Hold Statistics & Per-Ghost Object Snapshots
    // -------------------------------------------------------------------------
    constexpr int HOLD_SAMPLES = 8;

    struct HoldStats {
        int samples[HOLD_SAMPLES] = {};
        int count = 0;
        int index = 0;

        void add(int ticks) {
            samples[index] = std::clamp(ticks, 1, 240);
            index = (index + 1) % HOLD_SAMPLES;
            count = std::min(count + 1, HOLD_SAMPLES);
        }

        int pick(int fallback, int percent) const {
            if (count == 0) return fallback;
            int tmp[HOLD_SAMPLES];
            std::copy(samples, samples + count, tmp);
            int at = std::min(count - 1, count * percent / 100);
            std::nth_element(tmp, tmp + at, tmp + count);
            return tmp[at];
        }
    };

    HoldStats tapHold, robotHold;

    InputKind kindOf(PlayerObject* p) {
        if (!p || p->m_isShip) return InputKind::Other;
        if (p->m_isDart) return InputKind::Wave;
        return p->m_isRobot ? InputKind::Robot : InputKind::Tap;
    }

    PlayerObject* controlledPlayer(PlayLayer* layer, bool isPlayer1) {
        if (!layer) return nullptr;
        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        if (twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2) {
            bool flip2P = GameManager::get()->getGameVariable("0010");
            bool controlsP1 = flip2P ? !isPlayer1 : isPlayer1;
            return controlsP1 ? layer->m_player1 : layer->m_player2;
        }
        if (layer->m_gameState.m_isDualMode && layer->m_player2 &&
            layer->m_player1 && layer->m_player1->m_controlsDisabled && !layer->m_player2->m_controlsDisabled) {
            return layer->m_player2;
        }
        return layer->m_player1;
    }

    void recordRealInput(bool down, bool isPlayer1, PlayLayer* layer) {
        auto& ch = buf.chan[isPlayer1 ? 0 : 1];
        PlayerObject* target = controlledPlayer(layer, isPlayer1);
        if (down) {
            ch.pressTick = tickCount;
            ch.pressKind = kindOf(target);
            return;
        }
        if (ch.pressTick < 0) return;
        int held = tickCount - ch.pressTick;
        ch.pressTick = -1;
        switch (ch.pressKind) {
            case InputKind::Tap: tapHold.add(held); break;
            case InputKind::Robot: robotHold.add(held); break;
            default: break;
        }
    }

    int holdEstimate() {
        return tapHold.pick(st(12), 50);
    }

    ObjSnapshot* findSnapshot(GameObject* object) {
        for (auto& s : buf.snapshots) {
            if (s.object == object) return &s;
        }
        return nullptr;
    }

    ObjSnapshot& snapshotObject(GameObject* object) {
        if (auto* existing = findSnapshot(object)) {
            return *existing;
        }
        ObjSnapshot s { object, object->m_isActivated, nullptr, false, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect;
            s.activated = effect->m_activated;
            s.byP1 = effect->m_activatedByPlayer1;
            s.byP2 = effect->m_activatedByPlayer2;
            s.animated = effect->m_animationTriggered;
        }
        buf.snapshots.push_back(s);
        return buf.snapshots.back();
    }

    bool isSimUsedByGhost(GameObject* obj, PlayerObject* ghost) {
        uint8_t mask = (ghost == buf.ghost1) ? 1 : 2;
        for (auto const& p : buf.simUsed) {
            if (p.first == obj) return (p.second & mask) != 0;
        }
        return false;
    }

    void markSimUsedByGhost(GameObject* obj, PlayerObject* ghost) {
        uint8_t mask = (ghost == buf.ghost1) ? 1 : 2;
        for (auto& p : buf.simUsed) {
            if (p.first == obj) { p.second |= mask; return; }
        }
        buf.simUsed.push_back({ obj, mask });
    }

    bool isOrbUsedByGhost(GameObject* ring, PlayerObject* ghost) {
        uint8_t mask = (ghost == buf.ghost1) ? 1 : 2;
        for (auto const& p : buf.orbsUsed) {
            if (p.first == ring) return (p.second & mask) != 0;
        }
        return false;
    }

    void markOrbUsedByGhost(GameObject* ring, PlayerObject* ghost) {
        uint8_t mask = (ghost == buf.ghost1) ? 1 : 2;
        for (auto& p : buf.orbsUsed) {
            if (p.first == ring) { p.second |= mask; return; }
        }
        buf.orbsUsed.push_back({ ring, mask });
    }

    void restoreObjects() {
        for (auto const& s : buf.snapshots) {
            s.object->m_isActivated = s.isActivated;
            if (s.effect) {
                s.effect->m_activated = s.activated;
                s.effect->m_activatedByPlayer1 = s.byP1;
                s.effect->m_activatedByPlayer2 = s.byP2;
                s.effect->m_animationTriggered = s.animated;
            }
        }
        buf.snapshots.clear();
        buf.simUsed.clear();
        buf.orbsUsed.clear();
        buf.simDisabled.clear();
        buf.simEnabled.clear();
    }

    inline void advanceGhost(PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
    }

    inline void checkGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        if (ghost->m_collisionLogTop->count() > 0) ghost->m_collisionLogTop->removeAllObjects();
        if (ghost->m_collisionLogBottom->count() > 0) ghost->m_collisionLogBottom->removeAllObjects();
        if (ghost->m_collisionLogLeft->count() > 0) ghost->m_collisionLogLeft->removeAllObjects();
        if (ghost->m_collisionLogRight->count() > 0) ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
    }

    bool isFlying(PlayerObject* player) {
        return player->m_isDart || player->m_isBird || player->m_isSwing || player->m_isShip;
    }

    void releaseAutoClicks(int maxDashHold) {
        if (buf.auto1) {
            if (buf.held1) {
                buf.auto1 = false;
                buf.autoHold1 = 0;
            } else if (buf.ghost1) {
                if (!buf.ghost1->m_isDashing || ++buf.autoHold1 >= maxDashHold) {
                    buf.ghost1->releaseButton(PlayerButton::Jump);
                    buf.auto1 = false;
                    buf.autoHold1 = 0;
                }
            }
        }
        if (buf.auto2) {
            if (buf.held2) {
                buf.auto2 = false;
                buf.autoHold2 = 0;
            } else if (buf.ghost2) {
                if (!buf.ghost2->m_isDashing || ++buf.autoHold2 >= maxDashHold) {
                    buf.ghost2->releaseButton(PlayerButton::Jump);
                    buf.auto2 = false;
                    buf.autoHold2 = 0;
                }
            }
        }
    }

    enum class SimEnd { Ok, StopSearch };

    // -------------------------------------------------------------------------
    // Core Ghost Simulation Engine (Zero Heap Allocation Hot Loop)
    // -------------------------------------------------------------------------
    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, bool down, int horizon, int releaseAfter, int stopAt, SimEnd* end) {
        *end = SimEnd::Ok;
        if (!buf.ghost1 || !p1) {
            return horizon;
        }
        buf.simulating = true;
        buf.ghostDied = false;
        buf.ghostHitPortal = false;
        buf.ghostSwitched = false;
        buf.ghostSolo = false;
        buf.firstOrbTick = -1;
        buf.lastOrbTick = -1;
        buf.laterOrbTouched = false;
        buf.orbUsedTick = -1;
        buf.currentDelay = delay;
        buf.g1IsRealP2 = (p1 == layer->m_player2);

        bool second = dual && buf.ghost2 && p2;
        buf.simSecond = second;
        buf.haveRealRect = Buffer::ignoreTouching;
        buf.haveStartRects = true;
        buf.harmless.clear();
        buf.realRect1 = p1->getObjectRect();
        buf.realRect2 = second ? p2->getObjectRect() : buf.realRect1;

        copyState(buf.ghost1, p1);
        if (second) {
            copyState(buf.ghost2, p2);
        }
        buf.held1 = buf.held2 = initialHeld;
        buf.auto1 = buf.auto2 = false;
        buf.autoAt1 = buf.autoAt2 = -1;
        buf.autoHold1 = buf.autoHold2 = 0;

        float dt = buf.tickLength;
        if (!(dt > 0.05f && dt < 1.0f)) {
            dt = TICK;
        }

        bool fly1 = isFlying(p1) && !(Buffer::ufoSmart && p1->m_isBird);
        bool fly2 = second && isFlying(p2) && !(Buffer::ufoSmart && p2->m_isBird);
        bool hasGroundPlayer = !fly1 || (second && !fly2);

        bool spider1 = Buffer::spiderSmart && p1->m_isSpider;
        bool spider2 = second && Buffer::spiderSmart && p2->m_isSpider;
        bool ball1 = Buffer::ballSmart && p1->m_isBall;
        bool ball2 = second && Buffer::ballSmart && p2->m_isBall;

        int maxDashHold = holdEstimate();
        int total = delay + horizon;
        bool extendedForPortal = false;
        int survived = 0;
        int airTicks1 = 0, groundTicks1 = 0;
        int airTicks2 = 0, groundTicks2 = 0;
        bool landed1 = false, landed2 = !second;
        int tick = 0;
        bool died = false;
        bool safe = false;

        if (!scriptedMoves.empty()) {
            resetScriptedForRun(buf.ghost1->getPositionX(), buf.ghost1->getPositionY());
        }

        for (; tick < total; ++tick) {
            if (delay > 0 && ((tick - delay) & 15) == 15 && clickOverTime()) {
                break;
            }
            if ((tick & 31) == 31 && clickMs() >= Buffer::budgetMs * 2.0) {
                break;
            }

            second = buf.simSecond;
            if (!second) landed2 = true;
            buf.simTick = tick;

            double gx = buf.ghost1->getPositionX();
            double gy = buf.ghost1->getPositionY();
            if (!movers.empty()) {
                placeMovers(tick + 1, gx, gy);
            }
            if (!scriptedMoves.empty()) {
                fireScripted(buf.ghost1, buf.ghost2, second, tick);
                placeScripted(gx, gy);
            }

            if (buf.autoAt1 >= 0 && tick >= buf.autoAt1) {
                if (!buf.held1 && buf.ghost1->m_touchingRings && buf.ghost1->m_touchingRings->count() > 0) {
                    buf.ghost1->pushButton(PlayerButton::Jump);
                    buf.auto1 = true;
                    buf.autoHold1 = 0;
                }
                buf.autoAt1 = -2;
            }
            if (second && buf.autoAt2 >= 0 && tick >= buf.autoAt2) {
                if (!buf.held2 && buf.ghost2->m_touchingRings && buf.ghost2->m_touchingRings->count() > 0) {
                    buf.ghost2->pushButton(PlayerButton::Jump);
                    buf.auto2 = true;
                    buf.autoHold2 = 0;
                }
                buf.autoAt2 = -2;
            }

            if (tick == delay && !buf.skipPress) {
                pressGhost(buf.ghost1, down);
                if (second) {
                    pressGhost(buf.ghost2, down);
                }
            }
            if (buf.retapAt >= 0) {
                int t = tick - delay - buf.retapAt;
                if (t == 0 || t == 1 || t == 5) {
                    pressGhost(buf.ghost1, t == 1);
                    if (second) {
                        pressGhost(buf.ghost2, t == 1);
                    }
                }
            }
            if (down && releaseAfter >= 0 && tick == delay + releaseAfter &&
                (buf.ghost1->m_isDashing || (buf.ghostSwitched && (buf.ghost1->m_isDart || buf.ghost1->m_isShip)))) {
                ++releaseAfter;
            }
            if (releaseAfter >= 0 && tick == delay + releaseAfter) {
                pressGhost(buf.ghost1, !down);
                if (second) {
                    pressGhost(buf.ghost2, !down);
                }
            }

            float stepDt = (tick == 0) ? dt * buf.firstStep : dt;
            advanceGhost(buf.ghost1, stepDt);
            if (second) {
                advanceGhost(buf.ghost2, stepDt);
            }
            releaseAutoClicks(maxDashHold);
            if (buf.ghostDied) {
                died = true;
                break;
            }

            checkGhost(layer, buf.ghost1, dt);
            if (!buf.ghostDied && buf.simSecond) {
                checkGhost(layer, buf.ghost2, dt);
            }
            if (buf.ghostDied) {
                died = true;
                break;
            }
            if (buf.ghostHitPortal) {
                safe = tick >= delay;
                break;
            }
            if (buf.ghostSwitched && !extendedForPortal && total - tick < st(26)) {
                int addTicks = st(26) - (total - tick);
                total += addTicks;
                stopAt += addTicks;
                extendedForPortal = true;
            }
            if (tick < delay) {
                continue;
            }
            ++survived;
            if (survived >= stopAt) {
                break;
            }
            if (!hasGroundPlayer || buf.ghostSwitched) {
                continue;
            }

            if (!fly1) {
                if (buf.ghost1->m_isOnGround) {
                    if (airTicks1 > 0 && buf.captureBase) {
                        buf.baseTouchedDown = true;
                    }
                    if (spider1) {
                        if (++groundTicks1 >= st(SPIDER_LAND)) landed1 = true;
                    } else if (airTicks1 >= st(ball1 ? 2 : 6)) {
                        if (++groundTicks1 > st(ball1 ? 8 : 2)) landed1 = true;
                    }
                } else {
                    ++airTicks1;
                    groundTicks1 = 0;
                    landed1 = false;
                }
            } else if (survived >= st(42)) {
                landed1 = true;
            }

            if (buf.simSecond) {
                if (!fly2) {
                    if (buf.ghost2->m_isOnGround) {
                        if (spider2) {
                            if (++groundTicks2 >= st(SPIDER_LAND)) landed2 = true;
                        } else if (airTicks2 >= st(ball2 ? 2 : 6)) {
                            if (++groundTicks2 > st(ball2 ? 8 : 2)) landed2 = true;
                        }
                    } else {
                        ++airTicks2;
                        groundTicks2 = 0;
                        landed2 = false;
                    }
                } else if (survived >= st(42)) {
                    landed2 = true;
                }
            }

            if (landed1 && landed2) {
                safe = true;
                break;
            }
        }

        if (!scriptedObjects.empty()) {
            restoreScriptedRunPositions();
        }
        if (!movers.empty()) {
            placeMovers(0);
        }
        if ((died || buf.ghostHitPortal) && tick < delay) {
            *end = SimEnd::StopSearch;
        }
        restoreObjects();
        if (buf.firstOrbTick >= 0) {
            buf.ghostUsedParticles = true;
        }
        buf.lastRunSwitched = buf.ghostSwitched;
        buf.simulating = false;
        return safe ? horizon : survived;
    }

    // -------------------------------------------------------------------------
    // Multi-Timing Orbs, Two-Clicks-Ahead, and Per-Gamemode Scorers
    // -------------------------------------------------------------------------
    bool quickScore = false;

    int simulateOrbs(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, bool down, int horizon, int releaseAfter, int stopAt, SimEnd* end, bool fastSubCheck = false) {
        buf.orbClick = Buffer::assumeOrbs;
        buf.touchedOrb = false;
        int first = simulate(layer, p1, p2, dual, initialHeld, delay, down, horizon, releaseAfter, stopAt, end);
        if (fastSubCheck || quickScore || !Buffer::tryBothOrbs || !buf.touchedOrb || first >= stopAt || clickOverTime()) {
            return first;
        }
        SimEnd firstEnd = *end;
        buf.orbClick = !Buffer::assumeOrbs;
        int second = simulate(layer, p1, p2, dual, initialHeld, delay, down, horizon, releaseAfter, stopAt, end);
        buf.orbClick = Buffer::assumeOrbs;
        if (firstEnd != SimEnd::StopSearch) {
            *end = SimEnd::Ok;
        }
        int best = std::max(first, second);
        if (Buffer::orbTiming && delay == 0 && best < stopAt && !clickOverTime()) {
            for (int late : { 3, 6 }) {
                if (clickOverTime()) break;
                buf.orbClick = true;
                buf.orbClickDelay = st(late);
                SimEnd lateEnd;
                int ret = simulate(layer, p1, p2, dual, initialHeld, delay, down, horizon, releaseAfter, stopAt, &lateEnd);
                buf.orbClickDelay = 0;
                buf.orbClick = Buffer::assumeOrbs;
                best = std::max(best, ret);
                if (best >= stopAt) break;
            }
        }
        return best;
    }

    int fullWaveHorizon(bool mini) {
        return st(mini ? (Buffer::waveHorizon - 14) : Buffer::waveHorizon);
    }

    int nextClickAt(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, bool down, int horizon, int releaseAfter, int ret) {
        if (!Buffer::nextClick || ret >= horizon || ret < st(32) || clickOverTime()) return -1;
        int length = std::min(horizon, ret + st(38));
        for (int margin : { 8, 18 }) {
            int at = ret - st(margin);
            if (at < st(24) || clickOverTime()) break;
            buf.retapAt = at;
            SimEnd e;
            int r = simulate(layer, p1, p2, dual, initialHeld, delay, down, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (r >= length) return at;
        }
        return -1;
    }

    int alignOrb(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int horizon, int releaseAfter, int maxDelay, int knownTouchTick, int knownLeaveTick, int* landed) {
        if (clickOverTime()) return 0;
        int window = std::min(st(Buffer::orbWindow), maxDelay);
        SimEnd end;
        buf.orbClick = false;
        buf.orbAfterFirst = true;
        struct Reset {
            ~Reset() {
                buf.orbClick = Buffer::assumeOrbs;
                buf.orbAfterFirst = false;
                buf.orbClickDelay = 0;
                buf.ghostShiftX = 0.f;
            }
        } reset;

        auto tryAt = [&](int k, bool* used) {
            int best = -1;
            *used = false;
            for (int late : { 0, 4, 8 }) {
                if (late > 0 && clickOverTime()) break;
                buf.orbClickDelay = st(late);
                int ret = simulate(layer, p1, p2, dual, initialHeld, k, true, horizon, releaseAfter, horizon, &end);
                buf.orbClickDelay = 0;
                if (late == 0) *used = (buf.orbUsedTick >= 0);
                best = std::max(best, ret);
                if (ret >= horizon || buf.orbUsedTick < 0 || !buf.laterOrbTouched || !Buffer::orbTiming ||
                    end == SimEnd::StopSearch || clickOverTime()) {
                    break;
                }
            }
            return best;
        };

        int touch = knownTouchTick;
        int leave = knownLeaveTick;
        if (touch < 0) {
            buf.skipPress = true;
            buf.orbAfterFirst = false;
            int probeLen = std::min(maxDelay, std::max(window, st(24))) + 2;
            simulate(layer, p1, p2, dual, initialHeld, 0, true, probeLen, -1, probeLen, &end);
            buf.skipPress = false;
            buf.orbAfterFirst = true;
            touch = buf.firstOrbTick;
            leave = buf.lastOrbTick;
        }
        if (touch < 0 || touch > window || clickOverTime()) {
            return 0;
        }

        bool nowUsed = false;
        int now = tryAt(0, &nowUsed);
        if (nowUsed && now >= horizon) {
            return 0;
        }

        int from = nowUsed ? 1 : std::max(1, touch - 1);
        int to = std::min(maxDelay, std::max(leave, touch + st(5)));
        int safeStart = -1, safeEnd = -1;
        int bestRet = -1, bestK = 0, bestAt = -1;

        for (int k = from; k <= to && !clickOverTime(); ++k) {
            bool used = false;
            int ret = tryAt(k, &used);
            if (end == SimEnd::StopSearch) break;
            if (!used) {
                if (safeStart >= 0) break;
                continue;
            }
            if (ret >= horizon) {
                if (safeStart < 0) safeStart = k;
                safeEnd = k;
            } else if (safeStart >= 0) {
                break;
            }
            if (k + ret > bestAt) {
                bestAt = k + ret;
                bestRet = ret;
                bestK = k;
            }
        }

        if (safeStart >= 0) {
            *landed = horizon;
            return (safeStart + safeEnd) / 2;
        }
        if (nowUsed && now >= 0 && nextClickAt(layer, p1, p2, dual, initialHeld, 0, true, horizon, releaseAfter, now) >= 0) {
            return 0;
        }
        if (bestK > 0 && bestAt >= std::max(now, 0) + st(nowUsed ? 28 : 12)) {
            *landed = bestRet;
            return bestK;
        }
        return 0;
    }

    int waveScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, bool down, int horizon, int target, SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore || clickOverTime()) {
            return ret;
        }
        bool mini = (p1->m_isDart && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);
        int minTurn = st(mini ? 3 : 5);
        for (int margin : { 4, 9 }) {
            int turnAt = ret - st(margin);
            if (turnAt < minTurn || clickOverTime()) break;
            int length = std::min(horizon, turnAt + st(15));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, down, length, turnAt, length, &turnEnd, true);
            if (turned >= length) return horizon;
        }
        return ret;
    }

    constexpr int ROBOT_HOLDS[] = { 3, 8, 15, 24, 36, 52 };
    int robotUsedHold = -1;

    int robotScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, int horizon, int target, SimEnd* end) {
        int usual = robotHold.pick(st(20), 50);
        int best = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, horizon, usual, target, end);
        robotUsedHold = usual;
        if (best >= target || *end == SimEnd::StopSearch || quickScore || clickOverTime()) {
            return best;
        }
        int holds[std::size(ROBOT_HOLDS)];
        std::copy(std::begin(ROBOT_HOLDS), std::end(ROBOT_HOLDS), holds);
        std::stable_sort(std::begin(holds), std::end(holds), [&](int a, int b) {
            return std::abs(st(a) - usual) < std::abs(st(b) - usual);
        });
        for (int hold : holds) {
            if (std::abs(st(hold) - usual) < st(3)) continue;
            if (clickOverTime()) break;
            SimEnd e;
            int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, horizon, st(hold), target, &e, true);
            if (ret > best) {
                best = ret;
                robotUsedHold = st(hold);
            }
            if (best >= target) break;
        }
        return best;
    }

    int spiderScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, int horizon, int releaseAfter, int target, SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore || clickOverTime()) {
            return ret;
        }
        for (int margin : { 4, 9 }) {
            int at = ret - st(margin);
            if (at < st(4) || clickOverTime()) break;
            int length = std::min(horizon, at + st(15));
            buf.retapAt = at;
            SimEnd e;
            int retapped = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, length, releaseAfter, length, &e, true);
            buf.retapAt = -1;
            if (retapped >= length) return horizon;
        }
        return ret;
    }

    int shipScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, bool down, int horizon, int target, SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore || clickOverTime()) {
            return ret;
        }
        for (int margin : { 6, 14 }) {
            int turnAt = ret - st(margin);
            if (turnAt < st(4) || clickOverTime()) break;
            int length = std::min(horizon, turnAt + st(15));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, down, length, turnAt, length, &turnEnd, true);
            if (turned >= length) return horizon;
        }
        return ret;
    }

    int ufoScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, int horizon, int releaseAfter, int target, SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore || clickOverTime()) {
            return ret;
        }
        for (int margin : { 5, 12 }) {
            int at = ret - st(margin);
            if (at < st(4) || clickOverTime()) break;
            int length = std::min(horizon, at + st(18));
            buf.retapAt = at;
            SimEnd e;
            int reflapped = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, length, releaseAfter, length, &e, true);
            buf.retapAt = -1;
            if (reflapped >= length) return horizon;
        }
        return ret;
    }

    // -------------------------------------------------------------------------
    // Stride-2 Coarse-to-Fine Search + Wave Margin + No-Hitch Resume
    // -------------------------------------------------------------------------
    constexpr int SAME_CRASH_TRIES = 3;
    int searchFrom = 1;
    int budgetCutK = 0;
    bool resumeNow = false;
    QueuedInput resumeEntry {};

    bool sameCrash(int k, int ret, int base, int& streak) {
        if (streak >= 0 && std::abs(k + ret - base) <= 1) {
            ++streak;
        } else {
            streak = -1;
        }
        return streak >= SAME_CRASH_TRIES;
    }

    struct ScoreFn {
        void* ctx;
        int (*call)(void*, int, int, SimEnd*);
        template <typename F>
        ScoreFn(F& f)
            : ctx(const_cast<void*>(static_cast<const void*>(&f))),
              call([](void* c, int k, int t, SimEnd* e) -> int { return (*static_cast<F*>(c))(k, t, e); }) {}
        int operator()(int k, int t, SimEnd* e) const { return call(ctx, k, t, e); }
    };

    int searchFast(int horizon, int maxDelay, int base, int* landed,
        ScoreFn const& score, double budget, bool needFull, bool applyMargin = false) {
        budget = std::min(budget, searchBudget());
        if (budget <= 0.02) return 0;

        auto start = std::chrono::steady_clock::now();
        int target = needFull ? horizon : std::min(horizon, base + st(12));
        budgetCutK = 0;
        int streak = 0;
        int prevTested = searchFrom - 1;

        for (int k = searchFrom; k <= maxDelay;) {
            if (clickOverTime() || std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() >= budget) {
                budgetCutK = k;
                return 0;
            }
            SimEnd end;
            int ret = score(k, target, &end);
            if (end == SimEnd::StopSearch) return 0;
            if (ret >= target) {
                int bestK = k;
                if (k - prevTested > 1 && !clickOverTime()) {
                    SimEnd prevEnd;
                    int prevRet = score(k - 1, target, &prevEnd);
                    if (prevRet >= target) {
                        bestK = k - 1;
                        ret = prevRet;
                    }
                }
                *landed = ret;
                if (applyMargin && Buffer::waveMargin && bestK + 2 <= maxDelay && !clickOverTime()) {
                    SimEnd e1, e2;
                    if (score(bestK + 1, target, &e1) >= target && !clickOverTime() && score(bestK + 2, target, &e2) >= target) {
                        return bestK + 1;
                    }
                }
                return bestK;
            }
            if (sameCrash(k, ret, base, streak)) return 0;
            prevTested = k;
            int step = (k >= 2 && maxDelay >= 8) ? 2 : 1;
            if (k < maxDelay && k + step > maxDelay) {
                k = maxDelay;
            } else {
                k += step;
            }
        }
        return 0;
    }

    int searchTwoPass(int horizon, int maxDelay, int base, int* landed,
        ScoreFn const& score, bool needFull, bool applyMargin = false) {
        quickScore = true;
        int k = searchFast(horizon, maxDelay, base, landed, score, searchBudget() * 0.65, needFull, applyMargin);
        quickScore = false;
        if (k > 0) return k;
        double left = searchBudget();
        if (left < 0.18) return 0;
        return searchFast(horizon, maxDelay, base, landed, score, left, needFull, applyMargin);
    }

    // -------------------------------------------------------------------------
    // Input Interception & Queue Delivery
    // -------------------------------------------------------------------------
    bool intercept(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        ClickTimer timer;
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer || layer->m_isPlatformer) {
            return false;
        }

        int chIdx = isPlayer1 ? 0 : 1;
        auto& ch = buf.chan[chIdx];

        bool resuming = resumeNow;
        searchFrom = resuming ? std::max(1, resumeEntry.from) : 1;
        budgetCutK = 0;
        struct SearchFromReset {
            ~SearchFromReset() { searchFrom = 1; }
        } searchFromReset;

        if (!resuming) {
            ch.realInput = down;
            for (auto& q : ch.queue) {
                q.resume = false;
            }
        }

        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        bool inDual = layer->m_gameState.m_isDualMode && layer->m_player2 != nullptr;

        PlayerObject* p1 = self->m_player1;
        PlayerObject* p2 = nullptr;
        bool simDual = false;
        int pIdx = 0;

        if (inDual && twoPlayer) {
            bool flip2P = GameManager::get()->getGameVariable("0010");
            bool controlsP1 = flip2P ? !isPlayer1 : isPlayer1;
            p1 = controlsP1 ? layer->m_player1 : layer->m_player2;
            pIdx = controlsP1 ? 0 : 1;
            simDual = false;
        } else if (inDual && !twoPlayer) {
            if (layer->m_player1 && layer->m_player1->m_controlsDisabled && !layer->m_player2->m_controlsDisabled) {
                p1 = layer->m_player2;
                pIdx = 1;
                simDual = false;
            } else {
                p1 = layer->m_player1;
                p2 = (layer->m_player2 && !layer->m_player2->m_controlsDisabled) ? layer->m_player2 : nullptr;
                simDual = (p2 != nullptr);
            }
        }

        if (!p1 || p1->m_isDead || p1->m_controlsDisabled) {
            ch.resetPending();
            ch.appliedInput = down;
            return false;
        }

        if (!resuming && ch.queue.empty() && down == ch.appliedInput && (!down || ch.releaseShift == 0)) {
            return false;
        }

        float midTick = 0.f;
        if (Buffer::cbfSync && buf.inRealUpdate && std::abs(buf.tickDeltaCoord[pIdx]) > 0.01f && !resuming) {
            midTick = std::clamp((travelCoord(p1) - buf.tickStartCoord[pIdx]) / buf.tickDeltaCoord[pIdx], 0.f, 0.999f);
        }
        buf.firstStep = 1.f - midTick;
        struct FirstStepReset {
            ~FirstStepReset() { buf.firstStep = 1.f; }
        } firstStepReset;
        int early = midTick > 0.f ? 1 : 0;

        if (down && ch.queue.empty() && !resuming) {
            ch.releaseShift = 0;
        }

        bool p1Wave = p1->m_isDart;
        bool anyWave = p1Wave || (p2 && p2->m_isDart);
        bool anyShip = p1->m_isShip || (p2 && p2->m_isShip);
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        int spamGap = st(anyShip && !anyWave ? Buffer::shipSpamGap : Buffer::spamGap);
        if (!resuming) {
            if (ch.sinceInput < spamGap) {
                ++ch.rapid;
            } else {
                ch.rapid = 0;
            }
            ch.sinceInput = 0;
        }

        bool anyUfo = p1->m_isBird || (p2 && p2->m_isBird);
        bool anyBall = p1->m_isBall || (p2 && p2->m_isBall);
        bool anySwing = p1->m_isSwing || (p2 && p2->m_isSwing);
        bool spamActive = (anyWave || anyShip || anyUfo || anyBall || anySwing) && ch.rapid >= SPAM_STREAK;

        if (spamActive && !ch.queue.empty() && !resuming) {
            for (size_t i = 0; i < ch.queue.size(); ++i) {
                ch.queue[i].ticksLeft = static_cast<int>(i);
                ch.queue[i].resume = false;
            }
            if (ch.queue.back().down != down) {
                ch.queue.push_back({ static_cast<int>(ch.queue.size()), down });
            }
            ch.shift = 0;
            return true;
        }

        if (!ch.queue.empty() && !resuming) {
            int after = ch.queue.back().ticksLeft;
            int minNext = (ch.queue.back().down != down) ? (after + 1) : after;
            ch.queue.push_back({ std::max(ch.shift - early, minNext), down });
            return true;
        }

        bool anyRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        bool dashing = p1->m_isDashing || (p2 && p2->m_isDashing);
        bool grounded = p1->m_isOnGround && (!p2 || p2->m_isOnGround);

        bool checkStaircaseRelease = false;
        if (!anyWave && !anyShip && !anyRobot && !dashing && !down) {
            if (ch.releaseShift > 0) {
                ch.shift = ch.releaseShift;
                ch.releaseShift = 0;
                ch.queue.push_back({ std::max(0, ch.shift - early), false });
                return true;
            }
            if (!grounded && !spamActive) {
                checkStaircaseRelease = true;
            } else {
                ch.appliedInput = false;
                return false;
            }
        }

        bool anyFlyer = isFlying(p1) || (p2 && isFlying(p2));
        int horizon = st(anyFlyer ? 46 : Buffer::groundHorizon);
        if (anyWave) {
            horizon = fullWaveHorizon(mini);
        }

        int rawMax = anyWave ? buf.ticksWave.load(std::memory_order_relaxed)
                             : buf.ticksAll.load(std::memory_order_relaxed);
        int maxDelay = st(rawMax);

        if (spamActive) {
            horizon = st(18);
            maxDelay = std::min(maxDelay, st(3));
        }
        if (resuming) {
            maxDelay -= resumeEntry.elapsed;
            if (maxDelay < searchFrom) maxDelay = 0;
        }
        if (maxDelay <= 0) {
            ch.appliedInput = down;
            return false;
        }

        ScopedClickWorld clickWorld(layer, p1, maxDelay + horizon + st(26));

        int releaseAfter = -1;
        if (down && !anyWave && !anyShip) {
            releaseAfter = anyRobot ? robotHold.pick(-1, 50) : holdEstimate();
        }

        bool initialHeld = ch.appliedInput;
        SimEnd end;

        if (checkStaircaseRelease) {
            int releaseNow = simulateOrbs(layer, p1, p2, simDual, true, 0, false, horizon, -1, horizon, &end, true);
            if (releaseNow >= horizon) {
                ch.appliedInput = false;
                return false;
            }
            int cap = std::min(maxDelay, st(6));
            for (int k = 1; k <= cap && !clickOverTime(); ++k) {
                SimEnd e;
                int holdMore = simulateOrbs(layer, p1, p2, simDual, true, k, false, horizon, -1, horizon, &e, true);
                if (e == SimEnd::StopSearch) break;
                if (holdMore >= horizon) {
                    ch.shift = k;
                    ch.queue.push_back({ std::max(0, k - early), false });
                    return true;
                }
            }
            ch.appliedInput = false;
            return false;
        }

        bool cubeStyleWave = anyWave;
        bool robotPress = down && anyRobot && !anyWave && Buffer::robotSmart;
        bool robotRelease = !down && anyRobot && !anyWave && !anyShip && Buffer::robotSmart;
        bool anySpider = p1->m_isSpider || (p2 && p2->m_isSpider);
        bool spiderPress = down && anySpider && !anyWave && !robotPress && Buffer::spiderSmart;
        bool ufoPress = down && (anyUfo || anySwing) && !anyWave && !anyShip;
        bool shipInput = anyShip && !anyWave;
        bool groundPress = down && !anyFlyer && !anyRobot && !anySpider && !anyWave;

        // Evaluate base (delay = 0) quickly first before running multi-hold or orb-align searches
        quickScore = true;
        buf.captureBase = true;
        buf.baseTouchedDown = false;
        int base = cubeStyleWave ? waveScore(layer, p1, p2, simDual, initialHeld, 0, down, horizon, horizon, &end)
                 : robotPress    ? robotScore(layer, p1, p2, simDual, initialHeld, 0, horizon, horizon, &end)
                 : spiderPress   ? spiderScore(layer, p1, p2, simDual, initialHeld, 0, horizon, releaseAfter, horizon, &end)
                 : ufoPress      ? ufoScore(layer, p1, p2, simDual, initialHeld, 0, horizon, releaseAfter, horizon, &end)
                 : shipInput     ? shipScore(layer, p1, p2, simDual, initialHeld, 0, down, horizon, horizon, &end)
                                 : simulateOrbs(layer, p1, p2, simDual, initialHeld, 0, down, horizon, releaseAfter, horizon, &end);
        quickScore = spamActive;
        struct QuickReset { ~QuickReset() { quickScore = false; } } quickReset;

        bool baseSwitched = buf.lastRunSwitched;
        int baseFirstOrb = buf.firstOrbTick;
        int baseLastOrb = buf.lastOrbTick;
        int baseOrbUsed = buf.orbUsedTick;
        buf.captureBase = false;

        int orbWin = std::min(st(Buffer::orbWindow), maxDelay);
        bool orbAheadNotUsedAt0 = (baseFirstOrb > 0 && baseFirstOrb <= orbWin && baseOrbUsed != 0);

        if (base >= horizon && !orbAheadNotUsedAt0) {
            ch.appliedInput = down;
            return false;
        }

        if (down && !anyWave && !anyShip && !spamActive && Buffer::orbAlign && !resuming && !clickOverTime()) {
            if (orbAheadNotUsedAt0 || (!grounded && (baseFirstOrb >= 0 || ufoPress || spiderPress || robotPress))) {
                int orbLanded = 0;
                int k = alignOrb(layer, p1, p2, simDual, initialHeld, horizon, releaseAfter, maxDelay,
                    baseFirstOrb, baseLastOrb, &orbLanded);
                if (k > 0) {
                    ch.shift = k;
                    ch.queue.push_back({ std::max(0, k - early), down });
                    return true;
                }
            }
        }

        if (base >= horizon) {
            ch.appliedInput = down;
            return false;
        }

        if (baseSwitched && !cubeStyleWave && !shipInput && !spamActive && end != SimEnd::StopSearch && !clickOverTime()) {
            if (nextClickAt(layer, p1, p2, simDual, initialHeld, 0, down, horizon, releaseAfter, base) >= 0) {
                ch.appliedInput = down;
                return false;
            }
        }

        if (groundPress && Buffer::lateCrash > 0 && base >= st(Buffer::lateCrash) && buf.baseTouchedDown) {
            ch.appliedInput = down;
            return false;
        }

        auto scoreAt = [&](int k, int target, SimEnd* e) {
            return cubeStyleWave ? waveScore(layer, p1, p2, simDual, initialHeld, k, down, horizon, target, e)
                 : robotPress    ? robotScore(layer, p1, p2, simDual, initialHeld, k, horizon, target, e)
                 : spiderPress   ? spiderScore(layer, p1, p2, simDual, initialHeld, k, horizon, releaseAfter, target, e)
                 : ufoPress      ? ufoScore(layer, p1, p2, simDual, initialHeld, k, horizon, releaseAfter, target, e)
                 : shipInput     ? shipScore(layer, p1, p2, simDual, initialHeld, k, down, horizon, target, e)
                                 : simulateOrbs(layer, p1, p2, simDual, initialHeld, k, down, horizon, releaseAfter, target, e);
        };

        bool needFull = (groundPress || robotPress || robotRelease || ufoPress);
        int landed = 0;
        int saved = 0;

        if (spamActive) {
            saved = searchFast(horizon, maxDelay, base, &landed, scoreAt, searchBudget(), false);
        } else {
            saved = searchTwoPass(horizon, maxDelay, base, &landed, scoreAt, needFull, cubeStyleWave);
        }

        int resumes = resuming ? resumeEntry.resumes : 0;
        auto now = std::chrono::steady_clock::now();
        bool canResume = std::chrono::duration<double, std::milli>(now - lastResumeTime).count() >= 16.0;

        if (saved <= 0 && Buffer::noHitch && !spamActive && canResume && budgetCutK > 0 && budgetCutK <= maxDelay && resumes < MAX_RESUMES) {
            lastResumeTime = now;
            QueuedInput next {
                std::max(0, 1 - early), down, true,
                budgetCutK, (resuming ? resumeEntry.elapsed : 0) + 1, resumes + 1
            };
            ch.shift = 1;
            ch.queue.push_back(next);
            return true;
        }

        if (saved <= 0) {
            ch.appliedInput = down;
            return false;
        }

        ch.shift = saved;
        ch.queue.push_back({ std::max(0, saved - early), down });
        return true;
    }

    void resumeClick(PlayLayer* layer, int chIdx, QueuedInput const& q) {
        ClickTimer resumeTimer;
        resumeNow = true;
        resumeEntry = q;
        bool held = intercept(layer, q.down, chIdx == 0);
        resumeNow = false;
        sweepGhostLayer();
        if (!held) {
            sendButton(layer, chIdx, q.down);
        }
    }

    void deliverChannel(PlayLayer* layer, int chIdx) {
        auto& ch = buf.chan[chIdx];
        if (ch.sinceInput <= 998) {
            ++ch.sinceInput;
        }
        if (layer->m_player1 && layer->m_player1->m_isDead) {
            return;
        }
        if (ch.queue.empty()) {
            if (ch.appliedInput != ch.realInput) {
                sendButton(layer, chIdx, ch.realInput);
            }
            return;
        }
        while (!ch.queue.empty() && ch.queue.front().ticksLeft <= 0) {
            if (ch.queue.front().resume) {
                auto q = ch.queue.front();
                ch.queue.pop_front();
                resumeClick(layer, chIdx, q);
                continue;
            }
            bool down = ch.queue.front().down;
            ch.queue.pop_front();
            ch.releaseShift = down ? ch.shift : 0;
            if (down != ch.appliedInput) {
                sendButton(layer, chIdx, down);
            }
        }
        for (auto& q : ch.queue) {
            --q.ticksLeft;
        }
        if (ch.queue.empty()) {
            ch.shift = 0;
        }
    }

    inline void deliverPending(PlayLayer* layer) {
        updateWatched(layer);
        deliverChannel(layer, 0);
        deliverChannel(layer, 1);
    }
}

// -----------------------------------------------------------------------------
// Mod Initialization & Hooks
// -----------------------------------------------------------------------------
$on_mod(Loaded) {
    watched.reserve(ACTIVE_WATCH_CAP);
    movers.reserve(ACTIVE_WATCH_CAP);
    firedMoves.reserve(64);
    pendingSpawns.reserve(64);
    scriptedMoves.reserve(SCRIPTED_TRIGGER_LIMIT);
    scriptedObjects.reserve(SCRIPTED_OBJECT_LIMIT);
    buf.snapshots.reserve(64);
    buf.simUsed.reserve(64);
    buf.orbsUsed.reserve(32);
    buf.simDisabled.reserve(128);
    buf.simEnabled.reserve(96);
    buf.harmless.reserve(32);

    setStageByIndex(1); // Starts on 4 Ticks (1.0 Frame @ 60Hz)
    spawnExternalWindow();
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::spawnCircle", HOOK_PRIORITY);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (buf.simulating && isGhost(player)) {
            buf.ghostDied = true;
            return;
        }
        bool wasDead = player && player->m_isDead;
        PlayLayer::destroyPlayer(player, object);
        if (!player || wasDead || !player->m_isDead) {
            return;
        }
        if (buf.owner == this) {
            if (buf.chan[0].appliedInput) sendButton(this, 0, false);
            if (buf.chan[1].appliedInput) sendButton(this, 1, false);
        }
        buf.chan[0].fullReset();
        buf.chan[1].fullReset();
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        dropGhosts();
        buf.ghost1 = makeGhost(this);
        buf.ghost2 = makeGhost(this);
        buf.ghostLayerBase = buf.ghostLayer->getChildrenCount();
        buf.chan[0].fullReset();
        buf.chan[1].fullReset();
        buf.owner = this;
        ensureLevelIndexed(this);
        return true;
    }

    void onQuit() {
        dropGhosts();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        if (buf.owner != this) {
            PlayLayer::resetLevel();
            return;
        }
        if (buf.chan[0].appliedInput && !buf.chan[0].realInput) sendButton(this, 0, false);
        if (buf.chan[1].appliedInput && !buf.chan[1].realInput) sendButton(this, 1, false);
        buf.chan[0].resetPending();
        buf.chan[1].resetPending();
        buf.chan[0].pressTick = -1;
        buf.chan[1].pressTick = -1;
        buf.chan[0].rapid = buf.chan[1].rapid = 0;
        buf.chan[0].sinceInput = buf.chan[1].sinceInput = 999;
        firedMoves.clear();
        pendingSpawns.clear();
        watched.clear();
        movers.clear();
        dynScanCursor = 0;

        if (buf.ghost1) {
            buf.ghost1->stopAllActions();
            buf.ghost1->stopParticles();
        }
        if (buf.ghost2) {
            buf.ghost2->stopAllActions();
            buf.ghost2->stopParticles();
        }
        sweepGhostLayer();

        PlayLayer::resetLevel();
        syncHeld(this);
    }

    void playEndAnimationToPos(CCPoint position) {
        if (buf.simulating) return;
        PlayLayer::playEndAnimationToPos(position);
    }

    void spawnCircle() {
        if (buf.simulating) return;
        PlayLayer::spawnCircle();
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        dropGhosts();
        return ok;
    }
};

class $modify(BufHardStreak, HardStreak) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("HardStreak::addPoint", HOOK_PRIORITY);
    }

    void addPoint(CCPoint point) {
        if (buf.simulating ||
            (buf.ghost1 && this == buf.ghost1->m_waveTrail) ||
            (buf.ghost2 && this == buf.ghost2->m_waveTrail)) {
            return;
        }
        HardStreak::addPoint(point);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", HOOK_PRIORITY);
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (buf.simulating) return;
        if ((m_objectID == 901 || m_objectID == 1346) && Buffer::predictTriggers && m_duration > 0.f) {
            bool updated = false;
            for (auto& fm : firedMoves) {
                if (fm.trigger == this) {
                    fm.tick = tickCount;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                if (firedMoves.size() >= 48 && lastPruneTick != tickCount) {
                    lastPruneTick = tickCount;
                    pruneExpiredTriggers();
                }
                if (firedMoves.size() < 48) {
                    firedMoves.push_back({ this, tickCount });
                }
            }
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }

    void playTriggerEffect() {
        if (buf.simulating) return;
        EffectGameObject::playTriggerEffect();
    }

    void animationTriggered() {
        if (buf.simulating) return;
        EffectGameObject::animationTriggered();
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", HOOK_PRIORITY);
    }

    void playShineEffect() {
        if (buf.simulating) return;
        GameObject::playShineEffect();
    }

    void playDestroyObjectAnim(GJBaseGameLayer* layer) {
        if (buf.simulating) return;
        GameObject::playDestroyObjectAnim(layer);
    }
};

class $modify(BufRingObject, RingObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("RingObject::powerOnObject", HOOK_PRIORITY);
        (void)self.setHookPriority("RingObject::spawnCircle", HOOK_PRIORITY);
        (void)self.setHookPriority("RingObject::triggerActivated", HOOK_PRIORITY);
    }

    void powerOnObject(int state) {
        if (buf.simulating) return;
        RingObject::powerOnObject(state);
    }

    void spawnCircle() {
        if (buf.simulating) return;
        RingObject::spawnCircle();
    }

    void triggerActivated(float xPosition) {
        if (buf.simulating) return;
        RingObject::triggerActivated(xPosition);
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::update", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", HOOK_PRIORITY);
    }

    void update(float dt) {
        if (buf.simulating) {
            PlayerObject::update(dt);
            return;
        }
        auto* layer = GameManager::get()->m_playLayer;
        bool isP1 = layer && this == layer->m_player1;
        bool isP2 = layer && this == layer->m_player2;

        if (isP1) {
            ++tickCount;
            deliverPending(layer);
        }

        int pIdx = isP1 ? 0 : (isP2 ? 1 : -1);
        if (pIdx >= 0) {
            buf.tickStartCoord[pIdx] = travelCoord(this);
            buf.inRealUpdate = true;
        }

        PlayerObject::update(dt);

        if (pIdx >= 0) {
            buf.inRealUpdate = false;
            float delta = travelCoord(this) - buf.tickStartCoord[pIdx];
            if (std::abs(delta) > 0.01f) {
                buf.tickDeltaCoord[pIdx] = delta;
            }
            // Ignore tiny end-of-frame remainder substeps so stepScale() never spikes
            if (isP1 && dt >= 0.08f && dt <= 0.6f) {
                float avg = buf.tickLength;
                buf.tickLength = (avg > 0.05f) ? (avg * 0.9f + dt * 0.1f) : dt;
            }
        }
    }

    void incrementJumps() {
        if (buf.simulating) return;
        PlayerObject::incrementJumps();
    }

    bool ghostEffect() {
        return buf.simulating && isGhost(this);
    }

    void spawnCircle() { if (!ghostEffect()) PlayerObject::spawnCircle(); }
    void spawnDualCircle() { if (!ghostEffect()) PlayerObject::spawnDualCircle(); }
    void spawnPortalCircle(ccColor3B color, float startRadius) { if (!ghostEffect()) PlayerObject::spawnPortalCircle(color, startRadius); }
    void spawnScaleCircle() { if (!ghostEffect()) PlayerObject::spawnScaleCircle(); }
    void playBumpEffect(int objectType, GameObject* object) { if (!ghostEffect()) PlayerObject::playBumpEffect(objectType, object); }
    void playSpawnEffect() { if (!ghostEffect()) PlayerObject::playSpawnEffect(); }
    void createFadeOutDartStreak() { if (!ghostEffect()) PlayerObject::createFadeOutDartStreak(); }
    void fadeOutStreak2(float duration) { if (!ghostEffect()) PlayerObject::fadeOutStreak2(duration); }
    void playDynamicSpiderRun() { if (!ghostEffect()) PlayerObject::playDynamicSpiderRun(); }
    void animatePlatformerJump(float scale) { if (!ghostEffect()) PlayerObject::animatePlatformerJump(scale); }
    void runBallRotation(float speed) { if (!ghostEffect()) PlayerObject::runBallRotation(speed); }
    void runBallRotation2() { if (!ghostEffect()) PlayerObject::runBallRotation2(); }
    void runNormalRotation(bool notNormalMode, float speed) { if (!ghostEffect()) PlayerObject::runNormalRotation(notNormalMode, speed); }
    void toggleGhostEffect(GhostType type) { if (!ghostEffect()) PlayerObject::toggleGhostEffect(type); }
    void activateStreak() { if (!ghostEffect()) PlayerObject::activateStreak(); }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (buf.simulating) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (buf.simulating) {
            if (!isGhost(this) || isOrbUsedByGhost(ring, this)) {
                return;
            }
            snapshotObject(ring);
            double vy = m_yVelocity;
            bool upsideDown = m_isUpsideDown;
            bool dashing = m_isDashing;
            CCPoint oldPos = getPosition();
            PlayerObject::ringJump(ring, skipCheck);
            if (m_yVelocity != vy || m_isUpsideDown != upsideDown || m_isDashing != dashing || getPosition() != oldPos) {
                markOrbUsedByGhost(ring, this);
                if (buf.orbUsedTick < 0) {
                    buf.orbUsedTick = buf.simTick;
                }
            }
            return;
        }
        PlayerObject::ringJump(ring, skipCheck);
    }

    void startDashing(DashRingObject* ring) {
        if (buf.simulating && isGhost(this)) {
            markOrbUsedByGhost(ring, this);
            if (buf.orbUsedTick < 0) {
                buf.orbUsedTick = buf.simTick;
            }
        }
        PlayerObject::startDashing(ring);
    }
};

class $modify(BufKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat) {
            if (key == KEY_F6) {
                cycleStage();
            }
        }
        return CCKeyboardDispatcher::dispatchKeyboardMSG(key, isKeyDown, isKeyRepeat, timestamp);
    }
};

class $modify(BufBaseGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::claimParticle", HOOK_PRIORITY);
        (void)self.setHookPriority("GJBaseGameLayer::spawnParticle", HOOK_PRIORITY);
    }

    void claimParticle(gd::string key, int tag, cocos2d::CCParticleSystemQuad* particle, int zOrder, cocos2d::tCCPositionType posType) {
        if (buf.simulating) return;
        GJBaseGameLayer::claimParticle(key, tag, particle, zOrder, posType);
    }

    cocos2d::CCParticleSystemQuad* spawnParticle(char const* plist, int tag, cocos2d::tCCPositionType posType, cocos2d::CCPoint pos) {
        if (buf.simulating) return nullptr;
        return GJBaseGameLayer::spawnParticle(plist, tag, posType, pos);
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (buf.simulating) {
            if (!isGhost(player) || !object) return false;
            if (typeinfo_cast<RingObject*>(object)) {
                if (isOrbUsedByGhost(object, player)) return false;
                snapshotObject(object);
                return true;
            }
            if (isSimUsedByGhost(object, player) || player->m_lastActivatedPortal == object) {
                return false;
            }
            auto& snap = snapshotObject(object);
            if (buf.simTick <= 1) {
                bool realAlreadyTriggered = (player == buf.ghost1)
                    ? (buf.g1IsRealP2 ? snap.byP2 : snap.byP1)
                    : snap.byP2;
                if (realAlreadyTriggered) {
                    markSimUsedByGhost(object, player);
                    return false;
                }
            }
            markSimUsedByGhost(object, player);
            return true;
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!buf.simulating) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        static gd::vector<GameObject*> relevant;
        relevant.clear();
        if (static_cast<int>(relevant.capacity()) < objectCount + 32) {
            relevant.reserve(objectCount + 32);
        }

        constexpr uint64_t keep = COLLIDING_TYPES | INTERACT_TYPES;
        CCPoint const gPos = player->getPosition();
        double const gx = gPos.x;
        double const gy = gPos.y;
        bool const isSpider = player->m_isSpider;
        bool const isSideways = player->m_isSideways;
        bool haveRect = false;
        CCRect ghostRect;
        auto& list = *objects;

        for (int i = 0; i < objectCount; ++i) {
            auto* object = list[i];
            if (!object || object->m_isDisabled || object->m_isGroupDisabled || object->m_isDecoration) {
                continue;
            }

            if (object->m_width <= 45.f && object->m_height <= 45.f &&
                std::abs(object->m_scaleX) <= 2.5f && std::abs(object->m_scaleY) <= 2.5f) {
                if (!isSideways && std::abs(object->m_positionX - gx) > 105.0) continue;
                if (!isSpider && !isSideways && std::abs(object->m_positionY - gy) > 105.0) continue;
            }

            if (!buf.simDisabled.empty() && vecContains(buf.simDisabled, object)) {
                continue;
            }

            auto type = static_cast<unsigned>(object->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            if (keep & bit) {
                if (buf.haveRealRect && (bit & HAZARD_TYPES)) {
                    if (!buf.harmless.empty() && vecContains(buf.harmless, object)) continue;
                    if (buf.simTick <= 1) {
                        auto const& r = object->getObjectRect();
                        if (r.intersectsRect(buf.realRect1) || r.intersectsRect(buf.realRect2)) {
                            buf.harmless.push_back(object);
                            continue;
                        }
                    }
                }
                relevant.push_back(object);
                continue;
            }

            bool stop = (STOP_TYPES & bit) != 0;
            bool size = (SIZE_TYPES & bit) != 0;
            float speed = (stop || size) ? 0.f : speedPortalValue(object->m_objectID);
            if (!stop && !size && speed <= 0.f) continue;

            if (!haveRect) {
                ghostRect = player->getObjectRect();
                haveRect = true;
            }
            if (ghostRect.intersectsRect(object->getObjectRect())) {
                if (stop) {
                    if (!buf.harmless.empty() && vecContains(buf.harmless, object)) continue;
                    auto const& r = object->getObjectRect();
                    if (buf.haveStartRects && buf.simTick <= 1 && object->m_objectType != GameObjectType::TeleportOrb &&
                        (r.intersectsRect(buf.realRect1) || r.intersectsRect(buf.realRect2))) {
                        buf.harmless.push_back(object);
                        continue;
                    }
                    if (Buffer::portalPredict && isGhost(player) &&
                        (((MODE_TYPES & bit) && setGhostMode(player, object->m_objectType)) ||
                         ghostTeleport(player, object) ||
                         ghostSoloPortal(object) ||
                         ghostDualPortal(player, object))) {
                        buf.harmless.push_back(object);
                        haveRect = false;
                        continue;
                    }
                    buf.ghostHitPortal = true;
                } else if (size) {
                    bool mini = object->m_objectType == GameObjectType::MiniSizePortal;
                    if ((player->m_vehicleSize < 1.f) != mini) {
                        player->togglePlayerScale(mini, true);
                        haveRect = false;
                    }
                } else {
                    player->m_playerSpeed = speed;
                }
            }
        }

        if (!movers.empty() || !scriptedObjects.empty()) {
            auto inject = [&](GameObject* object) {
                if (!object || object->m_isDisabled || object->m_isGroupDisabled || object->m_isDecoration) return;
                auto type = static_cast<unsigned>(object->m_objectType);
                uint64_t bit = type < 64 ? (1ull << type) : 0;
                if (!(keep & bit)) return;
                double reach = 130.0 + std::max(object->m_width, object->m_height);
                if (!isSideways && std::abs(object->m_positionX - gx) > reach) return;
                if (!isSpider && !isSideways && std::abs(object->m_positionY - gy) > reach) return;
                if (!buf.simDisabled.empty() && vecContains(buf.simDisabled, object)) return;
                if (buf.haveRealRect && (bit & HAZARD_TYPES)) {
                    if (!buf.harmless.empty() && vecContains(buf.harmless, object)) continue;
                    if (buf.simTick <= 1) {
                        auto const& r = object->getObjectRect();
                        if (r.intersectsRect(buf.realRect1) || r.intersectsRect(buf.realRect2)) {
                            buf.harmless.push_back(object);
                            return;
                        }
                    }
                }
                if (std::find(relevant.begin(), relevant.end(), object) == relevant.end()) {
                    relevant.push_back(object);
                }
            };
            for (auto const& m : movers) inject(m.object);
            for (auto const& o : scriptedObjects) {
                if (!o.mover) inject(o.object);
            }
        }
        static std::vector<GameObject*> woke;
        woke.clear();
        if (!buf.simEnabled.empty() && player) {
            for (auto* object : buf.simEnabled) {
                auto type = static_cast<unsigned>(object->m_objectType);
                uint64_t bit = type < 64 ? (1ull << type) : 0;
                if (!(keep & bit) || vecContains(buf.simDisabled, object) ||
                    std::abs(object->m_positionX - gx) > 160.0 || std::abs(object->m_positionY - gy) > 160.0) {
                    continue;
                }
                if (object->m_isGroupDisabled) {
                    object->m_isGroupDisabled = false;
                    woke.push_back(object);
                }
                relevant.push_back(object);
            }
        }

        if (!relevant.empty()) {
            GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
        }
        for (auto* object : woke) {
            object->m_isGroupDisabled = true;
        }
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!buf.simulating && button == 1 && !buf.injecting) {
            auto* layer = GameManager::get()->m_playLayer;
            recordRealInput(down, isPlayer1, layer);
            bool held = intercept(this, down, isPlayer1);
            sweepGhostLayer();
            if (held) return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (buf.simulating) {
            if (!isGhost(player)) return;
            snapshotObject(ring);
            buf.touchedOrb = true;
            if (buf.firstOrbTick < 0) buf.firstOrbTick = buf.simTick;
            buf.lastOrbTick = buf.simTick;
            if (buf.orbUsedTick >= 0 && !isOrbUsedByGhost(ring, player)) {
                buf.laterOrbTouched = true;
            }
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);

        bool autoOrb = buf.orbClick || (buf.orbAfterFirst && buf.orbUsedTick >= 0 && !isOrbUsedByGhost(ring, player));
        if (buf.simulating && autoOrb && !isOrbUsedByGhost(ring, player)) {
            bool first = (player == buf.ghost1);
            bool& held = first ? buf.held1 : buf.held2;
            bool& autoClick = first ? buf.auto1 : buf.auto2;
            int& autoAt = first ? buf.autoAt1 : buf.autoAt2;
            int& autoHold = first ? buf.autoHold1 : buf.autoHold2;

            if (buf.orbClickDelay > 0) {
                if (!held && autoAt == -1) {
                    autoAt = buf.simTick + buf.orbClickDelay;
                }
            } else if (!autoClick) {
                if (held && buf.simTick > buf.currentDelay && !player->m_isDashing) {
                    player->releaseButton(PlayerButton::Jump);
                }
                if (!held || (buf.simTick > buf.currentDelay && !player->m_isDashing)) {
                    player->pushButton(PlayerButton::Jump);
                    autoClick = true;
                    autoHold = 0;
                }
            }
        }
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (buf.simulating) {
            if (isGhost(player)) player->flipGravity(flip, true);
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (buf.simulating) return;
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (buf.simulating) {
            if (auto* objects = this->getGroup(group)) {
                simToggle(objects, activate);
            }
            return;
        }
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (buf.simulating) return;
        if (Buffer::predictTriggers && delay > 0.0 && !ordered && remapKeys.empty() && group > 0) {
            if (pendingSpawns.size() >= 48 && lastPruneTick != tickCount) {
                lastPruneTick = tickCount;
                pruneExpiredTriggers();
            }
            if (pendingSpawns.size() < 64) {
                float tickLength = (buf.tickLength > 0.05f && buf.tickLength < 1.f) ? buf.tickLength : TICK;
                pendingSpawns.push_back({ group, tickCount + static_cast<int>(std::lround(delay * 60.0 / tickLength)) });
            }
        }
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void toggleGroup(int id, bool activate) {
        if (buf.simulating) return;
        GJBaseGameLayer::toggleGroup(id, activate);
    }

    void pickupItem(EffectGameObject* object) {
        if (buf.simulating) return;
        GJBaseGameLayer::pickupItem(object);
    }

    void updateCounters(int itemId, int value) {
        if (buf.simulating) return;
        GJBaseGameLayer::updateCounters(itemId, value);
    }

    void activateItemEditTrigger(ItemTriggerGameObject* object) {
        if (buf.simulating) return;
        GJBaseGameLayer::activateItemEditTrigger(object);
    }

    void activateItemCompareTrigger(ItemTriggerGameObject* object, gd::vector<int> const& remapKeys) {
        if (buf.simulating) return;
        GJBaseGameLayer::activateItemCompareTrigger(object, remapKeys);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (buf.simulating) {
            buf.ghostHitPortal = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }
};

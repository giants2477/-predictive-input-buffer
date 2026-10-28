// Click Before Frames — External Window Edition
// Zero in-game visual/audio footprint. Separate Win32 control window on launch.
// Selectable ticks: 2, 4, 8, 16. All other settings locked to optimal configuration.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr int SPIDER_LAND = 12;
    constexpr int SPAM_STREAK = 3;

    constexpr int ALLOWED_TICKS[4] = { 2, 4, 8, 16 };
    constexpr int STAGE_COUNT = 4;

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    constexpr uint64_t COLLIDING_TYPES =
        typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) |
        typeBit(GameObjectType::Slope) | typeBit(GameObjectType::CollisionObject) |
        typeBit(GameObjectType::AnimatedHazard);

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
        int ticksLeft;
        bool down;
    };

    struct ObjSnapshot {
        GameObject* object;
        bool isActivated;
        EffectGameObject* effect;
        bool activated;
        bool byP1;
        bool byP2;
        bool usedByG1;
        bool usedByG2;
    };

    enum class InputKind { Tap, Wave, Robot, Other };

    // Per-player input channel (0 = P1 side, 1 = P2 side) for full 2-Player Mode support
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

        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;

        std::atomic<int> stageIndex { 1 }; // Default index 1 -> 4 ticks
        std::atomic<int> ticksAll { 4 };
        std::atomic<int> ticksWave { 4 };

        ChannelState chan[2];

        float tickLength = TICK;

        // Locked settings matching your screenshots:
        static constexpr bool smart = true;
        static constexpr double userBudget = 12.0;
        double budgetMs = 12.0;
        static constexpr bool autoBudget = true;
        static constexpr bool practiceOnly = false;
        static constexpr int groundHorizon = 130;
        static constexpr int waveHorizon = 80;
        static constexpr bool assumeOrbs = true;
        static constexpr bool tryBothOrbs = true;
        static constexpr bool avoidMovers = false;
        static constexpr bool autoRelease = false;
        static constexpr int autoWindow = 8;
        static constexpr int spamGap = 24;
        static constexpr bool orbAlign = true;
        static constexpr int orbWindow = 4;
        static constexpr bool waveV46 = false;
        static constexpr bool waveMargin = true;
        static constexpr bool orbAlignGround = false;
        static constexpr bool fullState = true;
        static constexpr bool robotSmart = true;
        static constexpr bool robotAutoRelease = false;
        static constexpr bool learn = true;
        static constexpr bool autoNormalRuns = true;
        static constexpr bool spiderSmart = true;
        static constexpr bool dashAutoRelease = false;
        static constexpr bool shipBuffer = true;
        static constexpr int shipMaxDelay = 4;
        static constexpr bool orbTiming = true;
        static constexpr int lateCrash = 90;
        static constexpr bool predictMovers = false;
        static constexpr bool robustSaves = true;
        static constexpr bool ignoreTouching = true;
        static constexpr bool cbfSync = true;
        static constexpr bool lowLatencyQueue = true;
        static constexpr bool ufoSmart = true;
        static constexpr bool moreTime = false;
        static constexpr int shipSpamGap = 24;
        static constexpr bool ballSmart = true;
        static constexpr bool swingSmart = true;
        static constexpr bool twoPlayerSupport = true;

        // Simulation transient state
        bool held1 = false, held2 = false;
        bool auto1 = false, auto2 = false;
        int orbClickDelay = 0;
        int autoAt1 = -1, autoAt2 = -1;
        bool orbClick = true;
        bool touchedOrb = false;
        bool skipPress = false;
        bool haveRealRect = false;
        CCRect realRect1, realRect2;
        float ghostShiftX = 0.f;
        float firstStep = 1.f;
        bool inRealUpdate = false;
        float tickStartX[2] = { 0.f, 0.f };
        float tickDx[2] = { 0.f, 0.f };
        int retapAt = -1;
        int simTick = 0;
        int firstOrbTick = -1;
        int lastOrbTick = -1;
        bool orbAfterFirst = false;
        bool laterOrbTouched = false;
        int orbUsedTick = -1;
        bool captureBase = false;
        bool baseTouchedDown = false;

        // Fast contiguous vectors instead of heap-allocating std::unordered_set per tick
        std::vector<ObjSnapshot> snapshots;
        std::vector<std::pair<GameObject*, uint8_t>> orbsUsed; // bit 1 = G1, bit 2 = G2
        std::vector<GameObject*> simDisabled;
        std::vector<GameObject*> harmless;
    } buf;

    template <typename T>
    inline bool vecContains(std::vector<T> const& v, T const& item) {
        return std::find(v.begin(), v.end(), item) != v.end();
    }

    // -------------------------------------------------------------------------
    // External Win32 GUI Window (Only 2, 4, 8, 16 Ticks Selectable)
    // -------------------------------------------------------------------------
    HWND g_extHwnd = nullptr;
    HWND g_radioBtns[4] = {};
    HWND g_statusLabel = nullptr;
    constexpr UINT WM_APP_SYNC_STAGE = WM_APP + 1;

    void setStageByIndex(int idx, bool saveToDisk = true);

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
                    WS_CHILD | WS_VISIBLE, 16, 14, 230, 22, hwnd, nullptr, nullptr, nullptr);
                SendMessageA(title, WM_SETFONT, (WPARAM)hFontTitle, TRUE);

                char const* labels[4] = {
                    "2 Ticks  (0.5 Frames @ 60Hz)",
                    "4 Ticks  (1.0 Frame  @ 60Hz)",
                    "8 Ticks  (2.0 Frames @ 60Hz)",
                    "16 Ticks (4.0 Frames @ 60Hz)"
                };

                int currentIdx = std::clamp(buf.stageIndex.load(), 0, 3);
                for (int i = 0; i < 4; ++i) {
                    DWORD style = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0);
                    g_radioBtns[i] = CreateWindowExA(0, "BUTTON", labels[i], style,
                        20, 44 + i * 28, 225, 24, hwnd, (HMENU)(intptr_t)(100 + i), nullptr, nullptr);
                    SendMessageA(g_radioBtns[i], WM_SETFONT, (WPARAM)hFont, TRUE);
                    SendMessageA(g_radioBtns[i], BM_SETCHECK, (i == currentIdx) ? BST_CHECKED : BST_UNCHECKED, 0);
                }

                g_statusLabel = CreateWindowExA(0, "STATIC", "Active: 4 Ticks (F6 to cycle)",
                    WS_CHILD | WS_VISIBLE, 16, 162, 235, 20, hwnd, nullptr, nullptr, nullptr);
                SendMessageA(g_statusLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
                return 0;
            }
            case WM_COMMAND: {
                int id = LOWORD(wParam);
                if (id >= 100 && id < 104 && HIWORD(wParam) == BN_CLICKED) {
                    setStageByIndex(id - 100, true);
                }
                return 0;
            }
            case WM_APP_SYNC_STAGE: {
                int idx = std::clamp(static_cast<int>(wParam), 0, 3);
                for (int i = 0; i < 4; ++i) {
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
                // Keep window alive while GD is open; minimize instead of closing
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
            wc.hbrBackground = (HBRUSH)(COLOR_WINDOW);
            wc.lpszClassName = "CBFExternalControlWnd";
            RegisterClassExA(&wc);

            DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            RECT rc = { 0, 0, 265, 198 };
            AdjustWindowRect(&rc, style, FALSE);

            g_extHwnd = CreateWindowExA(
                0, wc.lpszClassName, "Click Before Frames", style,
                CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                nullptr, nullptr, hInst, nullptr
            );

            if (g_extHwnd) {
                ShowWindow(g_extHwnd, SW_SHOWNOACTIVATE);
                UpdateWindow(g_extHwnd);
                PostMessageA(g_extHwnd, WM_APP_SYNC_STAGE, buf.stageIndex.load(), 0);

                MSG msg;
                while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
            }
        }).detach();
    }

    void setStageByIndex(int idx, bool saveToDisk) {
        idx = std::clamp(idx, 0, STAGE_COUNT - 1);
        buf.stageIndex.store(idx);
        int ticks = ALLOWED_TICKS[idx];
        buf.ticksAll.store(ticks);
        buf.ticksWave.store(ticks);
        if (saveToDisk) {
            Mod::get()->setSavedValue("stage-idx-locked", idx);
        }
        if (g_extHwnd) {
            PostMessageA(g_extHwnd, WM_APP_SYNC_STAGE, idx, 0);
        }
    }

    void cycleStage() {
        int nextIdx = (buf.stageIndex.load() + 1) % STAGE_COUNT;
        setStageByIndex(nextIdx, true);
    }

    // -------------------------------------------------------------------------
    // Core Physics & Scaling Helpers
    // -------------------------------------------------------------------------
    float stepScale() {
        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.f)) {
            return 1.f;
        }
        return std::clamp(TICK / dt, 0.25f, 8.f);
    }

    int st(int ticks240) {
        if (ticks240 <= 0) {
            return ticks240;
        }
        return std::max(1, static_cast<int>(std::lround(ticks240 * stepScale())));
    }

    bool isGhost(PlayerObject* p) {
        return p && (p == buf.ghost1 || p == buf.ghost2);
    }

    void resetAllPending() {
        buf.chan[0].resetPending();
        buf.chan[1].resetPending();
    }

    void clickBudget() {
        double budget = Buffer::userBudget; // 12.0 ms
        if (Buffer::autoBudget) {
            auto* director = CCDirector::get();
            double interval = director->getAnimationInterval() * 1000.0;
            double frame = director->getDeltaTime() * 1000.0;
            // Only throttle budget if the game is actively dropping frames
            if (interval > 0.0 && frame > std::max(interval * 2.0, 20.0)) {
                budget = std::min(budget, 4.5);
            }
        }
        buf.budgetMs = budget;
    }

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
        buf.ghostLayer->addChild(ghost);
        return ghost;
    }

    void sweepGhostLayer() {
        if (!buf.ghostLayer) {
            return;
        }
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
            if (ghost->m_controlsDisabled || ghost->m_isLocked) {
                return;
            }
            ghost->pushButton(PlayerButton::Jump);
        } else {
            ghost->releaseButton(PlayerButton::Jump);
        }
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->resetTouchedRings(true);
        if (real->m_touchingRings && ghost->m_touchingRings) {
            for (auto* ring : CCArrayExt<GameObject*>(real->m_touchingRings)) {
                ghost->m_touchingRings->addObject(ring);
            }
        }
        ghost->copyAttributes(real);
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
        pos.x += buf.ghostShiftX;
        ghost->setPosition(pos);
        ghost->m_position = pos;

        CCPoint lastPos = real->m_lastPosition;
        lastPos.x += buf.ghostShiftX;
        ghost->m_lastPosition = lastPos;

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

    // -------------------------------------------------------------------------
    // Hold Statistics (Tap & Robot Hold Estimation)
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
            if (count == 0) {
                return fallback;
            }
            int tmp[HOLD_SAMPLES];
            std::copy(samples, samples + count, tmp);
            int at = std::min(count - 1, count * percent / 100);
            std::nth_element(tmp, tmp + at, tmp + count);
            return tmp[at];
        }
    };

    HoldStats tapHold, robotHold;
    int tickCount = 0;

    InputKind kindOf(PlayerObject* p) {
        if (!p || p->m_isShip) {
            return InputKind::Other;
        }
        if (p->m_isDart) {
            return InputKind::Wave;
        }
        return p->m_isRobot ? InputKind::Robot : InputKind::Tap;
    }

    // Determines which PlayerObject a button input controls (supports 2-Player & Flip 2P Controls)
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
        if (ch.pressTick < 0) {
            return;
        }
        int held = tickCount - ch.pressTick;
        ch.pressTick = -1;
        switch (ch.pressKind) {
            case InputKind::Tap: tapHold.add(held); break;
            case InputKind::Robot: robotHold.add(held); break;
            default: break;
        }
    }

    void forgetPresses() {
        buf.chan[0].pressTick = -1;
        buf.chan[1].pressTick = -1;
    }

    int holdEstimate() {
        return tapHold.pick(st(12), 50);
    }

    // -------------------------------------------------------------------------
    // Per-Level Memory
    // -------------------------------------------------------------------------
    constexpr float SLICE = 15.f;
    constexpr int OUTCOME_TICKS = 90;

    struct Spot {
        uint32_t ok = 0;
        uint32_t bad = 0;
        uint32_t deaths = 0;
        uint32_t delaySum = 0;
    };

    std::unordered_map<int, Spot> memory;
    std::string memoryKey;
    bool memoryDirty = false;
    int memoryUpdates = 0;

    struct PendingOutcome {
        bool active = false;
        int slice = 0;
        int tick = 0;
        int delay = 0;
        bool skipped = false;
    } pending;

    int sliceOf(PlayerObject* p) {
        return static_cast<int>(std::floor(p->getPosition().x / SLICE));
    }

    Spot spotAround(int slice) {
        Spot sum;
        for (int s = slice - 1; s <= slice + 1; ++s) {
            auto it = memory.find(s);
            if (it == memory.end()) continue;
            sum.ok += it->second.ok;
            sum.bad += it->second.bad;
            sum.deaths += it->second.deaths;
            sum.delaySum += it->second.delaySum;
        }
        return sum;
    }

    std::filesystem::path memoryFile() {
        return Mod::get()->getSaveDir() / "memory" / (memoryKey + "_v3.txt");
    }

    void saveMemory() {
        if (!memoryDirty || memoryKey.empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(memoryFile().parent_path(), ec);
        std::ofstream out(memoryFile(), std::ios::trunc);
        for (auto const& [slice, spot] : memory) {
            out << slice << ' ' << spot.ok << ' ' << spot.bad << ' ' << spot.deaths << ' ' << spot.delaySum << '\n';
        }
        memoryDirty = false;
    }

    void loadMemory(GJGameLevel* level) {
        memory.clear();
        memoryDirty = false;
        memoryUpdates = 0;
        pending.active = false;
        memoryKey.clear();
        if (!level) return;

        int id = level->m_levelID.value();
        if (id > 0) {
            memoryKey = fmt::format("level_{}", id);
        } else {
            std::string name = level->m_levelName;
            for (auto& c : name) {
                if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
            }
            memoryKey = "local_" + name;
        }

        std::ifstream in(memoryFile());
        int slice;
        Spot spot;
        while (in >> slice >> spot.ok >> spot.bad >> spot.deaths >> spot.delaySum) {
            memory[slice] = spot;
        }

        std::error_code ec;
        auto written = std::filesystem::last_write_time(memoryFile(), ec);
        if (!ec && std::filesystem::file_time_type::clock::now() - written > std::chrono::hours(12)) {
            for (auto it = memory.begin(); it != memory.end();) {
                auto& s = it->second;
                uint32_t ok = s.ok * 3 / 4;
                s.delaySum = s.ok > 0 ? static_cast<uint32_t>(static_cast<uint64_t>(s.delaySum) * ok / s.ok) : 0;
                s.ok = ok;
                s.bad = s.bad * 3 / 4;
                s.deaths = s.deaths * 3 / 4;
                it = (s.ok | s.bad | s.deaths) == 0 ? memory.erase(it) : std::next(it);
            }
            memoryDirty = true;
            saveMemory();
        }
    }

    void memoryChanged() {
        memoryDirty = true;
        if (++memoryUpdates % 10 == 0) {
            saveMemory();
        }
    }

    void creditPending() {
        if (!pending.active) return;
        pending.active = false;
        if (pending.delay > 0) {
            auto& spot = memory[pending.slice];
            ++spot.ok;
            spot.delaySum += static_cast<uint32_t>(std::lround(pending.delay / stepScale()));
            memoryChanged();
        }
    }

    void memoryNote(PlayerObject* p, int delay) {
        if (memoryKey.empty() || !p) return;
        if (pending.active && tickCount - pending.tick >= st(30)) {
            creditPending();
        }
        pending = { true, sliceOf(p), tickCount, delay, false };
    }

    void memoryTick() {
        if (pending.active && tickCount - pending.tick >= st(OUTCOME_TICKS)) {
            creditPending();
        }
    }

    bool memorySaysSkip(int slice) {
        auto spot = spotAround(slice);
        return spot.bad >= 2 && spot.bad > spot.ok;
    }

    void memoryDeath() {
        if (!pending.active) return;
        pending.active = false;
        auto& spot = memory[pending.slice];
        ++spot.deaths;
        if (pending.delay > 0) {
            ++spot.bad;
        }
        if (pending.skipped) {
            int worst = pending.slice;
            for (int s = pending.slice - 1; s <= pending.slice + 1; ++s) {
                auto it = memory.find(s);
                if (it != memory.end() && it->second.bad > memory[worst].bad) {
                    worst = s;
                }
            }
            if (memory[worst].bad > 0) {
                --memory[worst].bad;
            }
        }
        memoryChanged();
    }

    int memoryDelay(int slice) {
        auto spot = spotAround(slice);
        if (spot.ok < 2) return 0;
        return st(static_cast<int>(std::lround(static_cast<double>(spot.delaySum) / spot.ok)));
    }

    // -------------------------------------------------------------------------
    // Fast Object State Snapshots & Per-Ghost Usage Tracking
    // -------------------------------------------------------------------------
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
        ObjSnapshot s { object, object->m_isActivated, nullptr, false, false, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect;
            s.activated = effect->m_activated;
            s.byP1 = effect->m_activatedByPlayer1;
            s.byP2 = effect->m_activatedByPlayer2;
        }
        buf.snapshots.push_back(s);
        return buf.snapshots.back();
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
            if (p.first == ring) {
                p.second |= mask;
                return;
            }
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
            }
        }
        buf.snapshots.clear();
        buf.orbsUsed.clear();
        buf.simDisabled.clear();
    }

    void advanceGhost(PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
    }

    void checkGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
    }

    bool isFlying(PlayerObject* player) {
        return player->m_isDart || player->m_isBird || player->m_isSwing || player->m_isShip;
    }

    void releaseAutoClicks() {
        if (buf.auto1) {
            if (!buf.held1 && buf.ghost1 && !buf.ghost1->m_isDashing) {
                buf.ghost1->releaseButton(PlayerButton::Jump);
                buf.auto1 = false;
            } else if (buf.held1) {
                buf.auto1 = false;
            }
        }
        if (buf.auto2) {
            if (!buf.held2 && buf.ghost2 && !buf.ghost2->m_isDashing) {
                buf.ghost2->releaseButton(PlayerButton::Jump);
                buf.auto2 = false;
            } else if (buf.held2) {
                buf.auto2 = false;
            }
        }
    }

    enum class SimEnd { Ok, StopSearch };

    // -------------------------------------------------------------------------
    // Ghost Simulation Engine
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
        buf.firstOrbTick = -1;
        buf.lastOrbTick = -1;
        buf.laterOrbTouched = false;
        buf.orbUsedTick = -1;

        bool second = dual && buf.ghost2 && p2;
        buf.haveRealRect = Buffer::ignoreTouching;
        buf.harmless.clear();
        if (buf.haveRealRect) {
            buf.realRect1 = p1->getObjectRect();
            buf.realRect2 = second ? p2->getObjectRect() : buf.realRect1;
        }

        copyState(buf.ghost1, p1);
        if (second) {
            copyState(buf.ghost2, p2);
        }
        buf.held1 = buf.held2 = initialHeld;
        buf.auto1 = buf.auto2 = false;
        buf.autoAt1 = buf.autoAt2 = -1;

        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.0f)) {
            dt = TICK;
        }

        bool fly1 = isFlying(p1) && !(Buffer::ufoSmart && p1->m_isBird);
        bool fly2 = second && isFlying(p2) && !(Buffer::ufoSmart && p2->m_isBird);
        bool spider1 = Buffer::spiderSmart && p1->m_isSpider;
        bool spider2 = second && Buffer::spiderSmart && p2->m_isSpider;
        bool ball1 = Buffer::ballSmart && p1->m_isBall;
        bool ball2 = second && Buffer::ballSmart && p2->m_isBall;

        int total = delay + horizon;
        int survived = 0;
        int airTicks1 = 0, groundTicks1 = 0;
        int airTicks2 = 0, groundTicks2 = 0;
        bool landed1 = false, landed2 = !second;
        int tick = 0;
        bool died = false;
        bool safe = false;

        for (; tick < total; ++tick) {
            buf.simTick = tick;

            if (buf.autoAt1 >= 0 && tick >= buf.autoAt1) {
                if (buf.ghost1->m_touchingRings->count() > 0) {
                    if (buf.held1 && tick > delay && !buf.ghost1->m_isDashing) {
                        buf.ghost1->releaseButton(PlayerButton::Jump);
                    }
                    buf.ghost1->pushButton(PlayerButton::Jump);
                    buf.auto1 = true;
                }
                buf.autoAt1 = -1;
            }
            if (second && buf.autoAt2 >= 0 && tick >= buf.autoAt2) {
                if (buf.ghost2->m_touchingRings->count() > 0) {
                    if (buf.held2 && tick > delay && !buf.ghost2->m_isDashing) {
                        buf.ghost2->releaseButton(PlayerButton::Jump);
                    }
                    buf.ghost2->pushButton(PlayerButton::Jump);
                    buf.auto2 = true;
                }
                buf.autoAt2 = -1;
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
            if (down && releaseAfter >= 0 && tick == delay + releaseAfter && buf.ghost1->m_isDashing) {
                ++releaseAfter;
            }
            if (releaseAfter >= 0 && tick == delay + releaseAfter) {
                pressGhost(buf.ghost1, !down);
                if (second) {
                    pressGhost(buf.ghost2, !down);
                }
            }

            float stepDt = tick == 0 ? dt * buf.firstStep : dt;
            advanceGhost(buf.ghost1, stepDt);
            if (second) {
                advanceGhost(buf.ghost2, stepDt);
            }
            releaseAutoClicks();
            if (buf.ghostDied) {
                died = true;
                break;
            }

            checkGhost(layer, buf.ghost1, dt);
            if (!buf.ghostDied && second) {
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
            if (tick < delay) {
                continue;
            }
            ++survived;
            if (survived >= stopAt) {
                break;
            }

            // Per-player landing detection so mixed-mode duals (e.g. Cube + Wave/Ship) work accurately
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
            } else if (survived >= st(48)) {
                landed1 = true;
            }

            if (second) {
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
                } else if (survived >= st(48)) {
                    landed2 = true;
                }
            }

            if ((!fly1 || !fly2) && landed1 && landed2) {
                safe = true;
                break;
            }
        }

        if ((died || buf.ghostHitPortal) && tick < delay) {
            *end = SimEnd::StopSearch;
        }
        restoreObjects();
        buf.ghost1->stopAllActions();
        if (buf.ghost2) {
            buf.ghost2->stopAllActions();
        }
        buf.simulating = false;
        return safe ? horizon : survived;
    }

    int simulateOrbs(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, bool down, int horizon, int releaseAfter, int stopAt, SimEnd* end) {
        buf.orbClick = Buffer::assumeOrbs;
        buf.touchedOrb = false;
        int first = simulate(layer, p1, p2, dual, initialHeld, delay, down, horizon, releaseAfter, stopAt, end);
        if (!Buffer::tryBothOrbs || !buf.touchedOrb || first >= stopAt) {
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
        if (Buffer::orbTiming && best < stopAt) {
            for (int late : { 3, 6 }) {
                buf.orbClick = true;
                buf.orbClickDelay = st(late);
                SimEnd lateEnd;
                int ret = simulate(layer, p1, p2, dual, initialHeld, delay, down, horizon, releaseAfter, stopAt, &lateEnd);
                buf.orbClickDelay = 0;
                buf.orbClick = Buffer::assumeOrbs;
                best = std::max(best, ret);
                if (best >= stopAt) {
                    break;
                }
            }
        }
        return best;
    }

    std::chrono::steady_clock::time_point clickStart;

    double clickMs() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clickStart).count();
    }

    int fullWaveHorizon(bool mini) {
        int wh = Buffer::waveHorizon; // 80
        return st(mini ? std::max(20, wh - 15) : wh);
    }

    int alignOrb(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int horizon, int releaseAfter, int learned, int maxDelay, int* landed) {
        int window = st(Buffer::orbWindow); // 4 ticks
        int found = 0;
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
            for (int late : { 0, 3, 6, 10 }) {
                buf.orbClickDelay = st(late);
                int ret = simulate(layer, p1, p2, dual, initialHeld, k, true, horizon, releaseAfter, horizon, &end);
                buf.orbClickDelay = 0;
                if (late == 0) {
                    *used = buf.orbUsedTick >= 0;
                }
                best = std::max(best, ret);
                if (ret >= horizon || buf.orbUsedTick < 0 || !buf.laterOrbTouched || end == SimEnd::StopSearch) {
                    break;
                }
            }
            return best;
        };

        auto robustAt = [&](int k) {
            float shift = std::max(0.3f, 0.5f * (311.58f / 240.f) * (std::abs(p1->m_playerSpeed) / 0.9f));
            for (float off : { shift, -shift }) {
                buf.ghostShiftX = off;
                bool used;
                int ret = tryAt(k, &used);
                buf.ghostShiftX = 0.f;
                if (ret < horizon || !used) {
                    return false;
                }
            }
            return true;
        };

        if (learned > 0 && learned <= maxDelay) {
            bool used;
            int ret = tryAt(learned, &used);
            if (used && ret >= horizon) {
                *landed = ret;
                return learned;
            }
        }

        buf.skipPress = true;
        buf.orbAfterFirst = false;
        int probeLen = std::min(maxDelay, std::max(window, st(40))) + 2;
        simulate(layer, p1, p2, dual, initialHeld, 0, true, probeLen, -1, probeLen, &end);
        buf.skipPress = false;
        buf.orbAfterFirst = true;
        int touch = buf.firstOrbTick;
        int leave = buf.lastOrbTick;
        if (touch < 0 || touch > window) {
            return 0;
        }

        bool nowUsed;
        int now = tryAt(0, &nowUsed);
        if (nowUsed && now >= horizon) {
            return 0;
        }

        int from = nowUsed ? 1 : std::max(1, touch - 1);
        int to = std::min(maxDelay, std::max(leave, touch + st(6)));
        int safeStart = -1, safeEnd = -1;
        int bestRet = -1, bestK = 0;

        for (int k = from; k <= to; ++k) {
            if (clickMs() > buf.budgetMs) break;
            bool used;
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
            if (ret > bestRet) {
                bestRet = ret;
                bestK = k;
            }
        }

        if (safeStart >= 0) {
            int mid = (safeStart + safeEnd) / 2;
            found = mid;
            for (int off = 0; off <= safeEnd - safeStart; ++off) {
                int k = off % 2 == 0 ? mid + off / 2 : mid - (off + 1) / 2;
                if (k < safeStart || k > safeEnd) continue;
                if (clickMs() > buf.budgetMs * 1.5 || robustAt(k)) {
                    found = k;
                    break;
                }
            }
            *landed = horizon;
        } else if (bestK > 0 && bestRet >= std::min(horizon, std::max(now, 0) + st(12))) {
            found = bestK;
            *landed = bestRet;
        }
        return found;
    }

    constexpr int WAVE_TURN_MARGINS[] = { 4, 8, 12 };
    constexpr int WAVE_MIN_TURN = 6;
    constexpr int WAVE_ESCAPE = 16;

    int waveScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, bool down, int horizon, int target, SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch) {
            return ret;
        }
        for (int margin : WAVE_TURN_MARGINS) {
            int turnAt = ret - st(margin);
            if (turnAt < st(WAVE_MIN_TURN)) break;
            int length = std::min(horizon, turnAt + st(WAVE_ESCAPE));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, down, length, turnAt, length, &turnEnd);
            if (turned >= length) {
                return horizon;
            }
        }
        return ret;
    }

    constexpr int ROBOT_HOLDS[] = { 6, 14, 26, 45 };
    int robotUsedHold = -1;
    bool quickScore = false;

    int robotScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, int horizon, int target, SimEnd* end) {
        int usual = robotHold.pick(st(20), 50);
        int best = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, horizon, usual, target, end);
        robotUsedHold = usual;
        if (best >= target || *end == SimEnd::StopSearch || quickScore) {
            return best;
        }
        for (int hold : ROBOT_HOLDS) {
            if (std::abs(st(hold) - usual) < st(3)) continue;
            SimEnd e;
            int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, horizon, st(hold), target, &e);
            if (ret > best) {
                best = ret;
                robotUsedHold = st(hold);
            }
            if (best >= target) break;
        }
        return best;
    }

    constexpr int SPIDER_RETAP[] = { 4, 8, 12 };
    constexpr int SPIDER_MIN_RETAP = 4;

    int spiderScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, int horizon, int releaseAfter, int target, SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore) {
            return ret;
        }
        for (int margin : SPIDER_RETAP) {
            int at = ret - st(margin);
            if (at < st(SPIDER_MIN_RETAP)) break;
            int length = std::min(horizon, at + st(16));
            buf.retapAt = at;
            SimEnd e;
            int retapped = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (retapped >= length) {
                return horizon;
            }
        }
        return ret;
    }

    constexpr int SAME_CRASH_TRIES = 4;

    bool sameCrash(int k, int ret, int base, int& streak) {
        if (streak >= 0 && std::abs(k + ret - base) <= 1) {
            ++streak;
        } else {
            streak = -1;
        }
        return streak >= SAME_CRASH_TRIES;
    }

    int searchSmallest(int horizon, int maxDelay, int base, int* landed,
        std::function<int(int, int, SimEnd*)> const& score, double budget = -1.0, bool needFull = false) {
        if (budget < 0.0) budget = buf.budgetMs;
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        int target = needFull ? horizon : std::min(horizon, base + st(12));
        int streak = 0;
        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = score(k, target, &end);
            if (end == SimEnd::StopSearch) return 0;
            if (ret >= target) {
                *landed = ret;
                return k;
            }
            if (sameCrash(k, ret, base, streak)) return 0;
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > budget) {
                return 0;
            }
        }
        return 0;
    }

    int searchTwoPass(int horizon, int maxDelay, int base, int* landed,
        std::function<int(int, int, SimEnd*)> const& score, bool needFull = false) {
        quickScore = true;
        int k = searchSmallest(horizon, maxDelay, base, landed, score, buf.budgetMs * 0.5, needFull);
        quickScore = false;
        if (k > 0) return k;
        double left = buf.budgetMs - clickMs();
        if (left < 0.5) return 0;
        return searchSmallest(horizon, maxDelay, base, landed, score, left, needFull);
    }

    constexpr int SHIP_RECOVER[] = { 8, 16, 24 };

    int shipScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, bool down, int horizon, int target, SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch) {
            return ret;
        }
        for (int margin : SHIP_RECOVER) {
            int turnAt = ret - st(margin);
            if (turnAt < st(WAVE_MIN_TURN)) break;
            int length = std::min(horizon, turnAt + st(WAVE_ESCAPE));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, down, length, turnAt, length, &turnEnd);
            if (turned >= length) {
                return horizon;
            }
        }
        return ret;
    }

    constexpr int UFO_REFLAP[] = { 6, 12, 18 };

    int ufoScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        int delay, int horizon, int releaseAfter, int target, SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore) {
            return ret;
        }
        for (int margin : UFO_REFLAP) {
            int at = ret - st(margin);
            if (at < st(4)) break;
            int length = std::min(horizon, at + st(20));
            buf.retapAt = at;
            SimEnd e;
            int reflapped = simulateOrbs(layer, p1, p2, dual, initialHeld, delay, true, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (reflapped >= length) {
                return horizon;
            }
        }
        return ret;
    }

    int searchWave(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        bool down, int horizon, int maxDelay, int base, int* landed) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        int target = std::min(horizon, base + st(12));
        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = waveScore(layer, p1, p2, dual, initialHeld, k, down, horizon, target, &end);
            if (end == SimEnd::StopSearch) return 0;
            if (ret >= target) {
                *landed = ret;
                if (Buffer::waveMargin && k + 2 <= maxDelay) {
                    SimEnd e1, e2;
                    if (waveScore(layer, p1, p2, dual, initialHeld, k + 1, down, horizon, target, &e1) >= target &&
                        waveScore(layer, p1, p2, dual, initialHeld, k + 2, down, horizon, target, &e2) >= target) {
                        return k + 1;
                    }
                }
                return k;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > buf.budgetMs) {
                return 0;
            }
        }
        return 0;
    }

    int searchDelay(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool initialHeld,
        bool down, int horizon, int maxDelay, int releaseAfter, int base, int* landed, bool needFull = false) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        auto overBudget = [&] {
            return std::chrono::duration<double, std::milli>(clock::now() - start).count() > buf.budgetMs;
        };

        int target = needFull ? horizon : std::min(horizon, base + st(12));
        int sameStreak = 0;
        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = simulateOrbs(layer, p1, p2, dual, initialHeld, k, down, horizon, releaseAfter, target, &end);
            if (end == SimEnd::StopSearch) break;
            if (ret < target && sameCrash(k, ret, base, sameStreak)) break;
            if (ret >= target) {
                SimEnd full;
                if (ret < horizon && simulateOrbs(layer, p1, p2, dual, initialHeld, k, down, horizon, releaseAfter, horizon, &full) < horizon) {
                    for (int j = k + 1; j <= maxDelay && !overBudget(); ++j) {
                        SimEnd e;
                        int r = simulateOrbs(layer, p1, p2, dual, initialHeld, j, down, horizon, releaseAfter, horizon, &e);
                        if (e == SimEnd::StopSearch) break;
                        if (r >= horizon) {
                            *landed = r;
                            return j;
                        }
                    }
                }
                *landed = ret;
                return k;
            }
            if (overBudget()) break;
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // Input Interception & Queue Delivery (Full 1P, Dual, and 2-Player Support)
    // -------------------------------------------------------------------------
    bool intercept(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        clickStart = std::chrono::steady_clock::now();
        clickBudget();
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer || layer->m_isPlatformer) {
            return false;
        }

        int chIdx = isPlayer1 ? 0 : 1;
        auto& ch = buf.chan[chIdx];
        ch.realInput = down;

        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        bool inDual = layer->m_gameState.m_isDualMode && layer->m_player2 != nullptr;

        // Determine which player(s) this input channel actually steers
        PlayerObject* p1 = self->m_player1;
        PlayerObject* p2 = nullptr;
        bool simDual = false;
        int pIdx = 0;

        if (inDual && twoPlayer) {
            bool flip2P = GameManager::get()->getGameVariable("0010");
            bool controlsP1 = flip2P ? !isPlayer1 : isPlayer1;
            p1 = controlsP1 ? layer->m_player1 : layer->m_player2;
            pIdx = controlsP1 ? 0 : 1;
            simDual = false; // In 2P dual mode, each input channel steers its own player independently
        } else if (inDual && !twoPlayer) {
            // Solo dual mode: if P1 has controls disabled (dummy box), predict P2 directly
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

        float midTick = 0.f;
        if (Buffer::cbfSync && buf.inRealUpdate && std::abs(buf.tickDx[pIdx]) > 0.01f) {
            midTick = std::clamp((p1->getPosition().x - buf.tickStartX[pIdx]) / buf.tickDx[pIdx], 0.f, 0.999f);
        }
        buf.firstStep = 1.f - midTick;
        struct FirstStepReset {
            ~FirstStepReset() { buf.firstStep = 1.f; }
        } firstStepReset;
        int early = midTick > 0.f ? 1 : 0;

        if (down && ch.queue.empty()) {
            ch.releaseShift = 0;
        }

        bool p1Wave = p1->m_isDart;
        bool anyWave = p1Wave || (p2 && p2->m_isDart);
        bool anyShip = p1->m_isShip || (p2 && p2->m_isShip);
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        int spamGap = st(anyShip && !anyWave ? Buffer::shipSpamGap : Buffer::spamGap); // 24 ticks
        if (ch.sinceInput < spamGap) {
            ++ch.rapid;
        } else {
            ch.rapid = 0;
        }
        ch.sinceInput = 0;

        bool anyUfo = p1->m_isBird || (p2 && p2->m_isBird);
        bool anyBall = p1->m_isBall || (p2 && p2->m_isBall);
        bool anySwing = p1->m_isSwing || (p2 && p2->m_isSwing);

        if ((anyWave || anyShip || anyUfo || anyBall || anySwing) && ch.rapid >= SPAM_STREAK) {
            if (!ch.queue.empty()) {
                for (size_t i = 0; i < ch.queue.size(); ++i) {
                    ch.queue[i].ticksLeft = static_cast<int>(i);
                }
                if (ch.queue.back().down != down) {
                    ch.queue.push_back({ static_cast<int>(ch.queue.size()), down });
                }
                ch.shift = 0;
                return true;
            }
            ch.appliedInput = down;
            return false;
        }

        // If a delay is already queued on this channel, preserve tap/hold spacing (never 0-tick collapse)
        if (!ch.queue.empty()) {
            int after = ch.queue.back().ticksLeft;
            int minNext = (ch.queue.back().down != down) ? (after + 1) : after;
            int targetTick = (Buffer::lowLatencyQueue && down) ? minNext : std::max(ch.shift - early, minNext);
            ch.queue.push_back({ targetTick, down });
            return true;
        }

        if (!p1Wave) {
            bool robot = p1->m_isRobot;
            bool wave2 = false;
            if (p2 && !robot) {
                wave2 = p2->m_isDart;
                if (!wave2) robot = p2->m_isRobot;
            }
            bool dashing = p1->m_isDashing || (p2 && p2->m_isDashing);
            if (!wave2 && !down && !robot && !dashing && !anyShip) {
                if (ch.releaseShift > 0) {
                    ch.shift = ch.releaseShift;
                    ch.releaseShift = 0;
                    ch.queue.push_back({ std::max(0, ch.shift - early), false });
                    return true;
                }
                ch.appliedInput = false;
                return false;
            }
        }

        bool anyFlyer = isFlying(p1) || (p2 && isFlying(p2));
        int horizon = st(anyFlyer ? 48 : Buffer::groundHorizon);
        if (anyWave) {
            horizon = fullWaveHorizon(mini);
        }

        int rawMax = anyWave ? buf.ticksWave.load() : (anyShip ? Buffer::shipMaxDelay : buf.ticksAll.load());
        int maxDelay = st(rawMax);
        if (maxDelay <= 0) {
            ch.appliedInput = down;
            return false;
        }

        bool anyRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        int releaseAfter = -1;
        if (down && !anyWave && !anyShip) {
            releaseAfter = anyRobot ? robotHold.pick(-1, 50) : holdEstimate();
        }

        bool grounded = p1->m_isOnGround && (!p2 || p2->m_isOnGround);
        int slice = sliceOf(p1);
        bool initialHeld = ch.appliedInput;

        if (down && !anyWave && !anyShip && Buffer::orbAlign && !grounded) {
            int orbLanded = 0;
            int k = memorySaysSkip(slice) ? 0 : alignOrb(layer, p1, p2, simDual, initialHeld, horizon, releaseAfter, memoryDelay(slice), maxDelay, &orbLanded);
            if (k > 0) {
                memoryNote(p1, k);
                ch.shift = k;
                ch.queue.push_back({ std::max(0, k - early), down });
                return true;
            }
        }

        SimEnd end;
        bool cubeStyleWave = anyWave;
        bool robotPress = down && anyRobot && !anyWave;
        bool anySpider = p1->m_isSpider || (p2 && p2->m_isSpider);
        bool spiderPress = down && anySpider && !anyWave && !robotPress;
        bool ufoPress = down && (anyUfo || anySwing) && !anyWave && !anyShip;
        bool shipInput = anyShip && !anyWave;

        buf.captureBase = true;
        buf.baseTouchedDown = false;
        int base = cubeStyleWave ? waveScore(layer, p1, p2, simDual, initialHeld, 0, down, horizon, horizon, &end)
                 : robotPress    ? robotScore(layer, p1, p2, simDual, initialHeld, 0, horizon, horizon, &end)
                 : spiderPress   ? spiderScore(layer, p1, p2, simDual, initialHeld, 0, horizon, releaseAfter, horizon, &end)
                 : ufoPress      ? ufoScore(layer, p1, p2, simDual, initialHeld, 0, horizon, releaseAfter, horizon, &end)
                 : shipInput     ? shipScore(layer, p1, p2, simDual, initialHeld, 0, down, horizon, horizon, &end)
                                 : simulateOrbs(layer, p1, p2, simDual, initialHeld, 0, down, horizon, releaseAfter, horizon, &end);
        buf.captureBase = false;

        if (base >= horizon) {
            memoryNote(p1, 0);
            ch.appliedInput = down;
            return false;
        }

        bool groundPress = down && !anyFlyer && !anyRobot && !anySpider && !anyWave;
        if (groundPress && Buffer::lateCrash > 0 && base >= st(Buffer::lateCrash) && buf.baseTouchedDown) {
            memoryNote(p1, 0);
            ch.appliedInput = down;
            return false;
        }

        if (memorySaysSkip(slice)) {
            memoryNote(p1, 0);
            pending.skipped = true;
            ch.appliedInput = down;
            return false;
        }

        int landed = 0;
        int saved = 0;

        auto scoreAt = [&](int k, int target, SimEnd* e) {
            return cubeStyleWave ? waveScore(layer, p1, p2, simDual, initialHeld, k, down, horizon, target, e)
                 : robotPress    ? robotScore(layer, p1, p2, simDual, initialHeld, k, horizon, target, e)
                 : spiderPress   ? spiderScore(layer, p1, p2, simDual, initialHeld, k, horizon, releaseAfter, target, e)
                 : ufoPress      ? ufoScore(layer, p1, p2, simDual, initialHeld, k, horizon, releaseAfter, target, e)
                 : shipInput     ? shipScore(layer, p1, p2, simDual, initialHeld, k, down, horizon, target, e)
                                 : simulateOrbs(layer, p1, p2, simDual, initialHeld, k, down, horizon, releaseAfter, target, e);
        };

        int needed = (groundPress || robotPress || ufoPress) ? horizon : std::min(horizon, base + st(12));
        int learned = memoryDelay(slice);
        if (learned > 0 && learned <= maxDelay) {
            SimEnd e;
            int ret = scoreAt(learned, needed, &e);
            if (ret >= needed) {
                saved = learned;
                landed = ret;
            }
        }

        if (saved <= 0) {
            saved = cubeStyleWave ? searchWave(layer, p1, p2, simDual, initialHeld, down, horizon, maxDelay, base, &landed)
                  : robotPress    ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return robotScore(layer, p1, p2, simDual, initialHeld, k, horizon, target, e);
                    }, true)
                  : spiderPress   ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return spiderScore(layer, p1, p2, simDual, initialHeld, k, horizon, releaseAfter, target, e);
                    })
                  : shipInput     ? searchSmallest(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return shipScore(layer, p1, p2, simDual, initialHeld, k, down, horizon, target, e);
                    })
                  : ufoPress      ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return ufoScore(layer, p1, p2, simDual, initialHeld, k, horizon, releaseAfter, target, e);
                    }, true)
                                  : searchDelay(layer, p1, p2, simDual, initialHeld, down, horizon, maxDelay, releaseAfter, base, &landed, groundPress);
        }

        if (saved <= 0) {
            memoryNote(p1, 0);
            ch.appliedInput = down;
            return false;
        }

        // Robustness check: verify survival when shifted +/- 0.5 ticks horizontally
        if (Buffer::robustSaves) {
            float shift = std::max(0.3f, 0.5f * (311.58f / 240.f) * (std::abs(p1->m_playerSpeed) / 0.9f));
            auto robust = [&](int k) {
                for (float off : { shift, -shift }) {
                    buf.ghostShiftX = off;
                    SimEnd e;
                    int ret = scoreAt(k, needed, &e);
                    buf.ghostShiftX = 0.f;
                    if (ret < needed) return false;
                }
                return true;
            };
            if (clickMs() < buf.budgetMs && !robust(saved)) {
                for (int k = saved + 1; k <= std::min(maxDelay, saved + st(3)); ++k) {
                    SimEnd e;
                    int ret = scoreAt(k, needed, &e);
                    if (ret >= needed && robust(k)) {
                        saved = k;
                        landed = ret;
                        break;
                    }
                }
            }
        }

        memoryNote(p1, saved);
        ch.shift = saved;
        ch.queue.push_back({ std::max(0, saved - early), down });
        return true;
    }

    void deliverChannel(PlayLayer* layer, int chIdx) {
        auto& ch = buf.chan[chIdx];
        if (ch.sinceInput <= 998) {
            ++ch.sinceInput;
        }
        if (ch.queue.empty()) {
            if (ch.appliedInput != ch.realInput) {
                sendButton(layer, chIdx, ch.realInput);
            }
            return;
        }
        while (!ch.queue.empty() && ch.queue.front().ticksLeft <= 0) {
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

    void deliverPending(PlayLayer* layer) {
        deliverChannel(layer, 0);
        deliverChannel(layer, 1);
        sweepGhostLayer();
    }
}

// -----------------------------------------------------------------------------
// Mod Initialization & Hooks (Zero In-Game UI Signs)
// -----------------------------------------------------------------------------
$on_mod(Loaded) {
    int savedIdx = std::clamp(static_cast<int>(Mod::get()->getSavedValue<int64_t>("stage-idx-locked", 1)), 0, STAGE_COUNT - 1);
    setStageByIndex(savedIdx, false);
    spawnExternalWindow();
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
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
        buf.chan[0].fullReset();
        buf.chan[1].fullReset();
        memoryDeath();
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        dropGhosts();
        loadMemory(level);
        buf.ghost1 = makeGhost(this);
        buf.ghost2 = makeGhost(this);
        buf.ghostLayerBase = buf.ghostLayer->getChildrenCount();
        buf.chan[0].fullReset();
        buf.chan[1].fullReset();
        return true;
    }

    void onQuit() {
        saveMemory();
        memoryKey.clear();
        dropGhosts();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        resetAllPending();
        forgetPresses();
        PlayLayer::resetLevel();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (buf.simulating) return;
        PlayLayer::playEndAnimationToPos(position);
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
        if (buf.simulating) return;
        HardStreak::addPoint(point);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("EffectGameObject::triggerObject", HOOK_PRIORITY);
    }

    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (buf.simulating) return;
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
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
            memoryTick();
            deliverPending(layer);
        }

        int pIdx = isP1 ? 0 : (isP2 ? 1 : -1);
        if (pIdx >= 0) {
            buf.tickStartX[pIdx] = getPosition().x;
            if (isP1) buf.inRealUpdate = true;
        }

        PlayerObject::update(dt);

        if (pIdx >= 0) {
            if (isP1) buf.inRealUpdate = false;
            float dx = getPosition().x - buf.tickStartX[pIdx];
            if (std::abs(dx) > 0.01f) {
                buf.tickDx[pIdx] = dx;
            }
            if (isP1 && dt > 0.f) {
                float avg = buf.tickLength;
                buf.tickLength = (avg > 0.f && std::abs(dt - avg) < avg * 0.3f) ? avg * 0.9f + dt * 0.1f : dt;
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
            } else if (key == KEY_F11 && GameManager::get()->m_playLayer) {
                memory.clear();
                pending.active = false;
                memoryDirty = true;
                saveMemory();
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
    }

    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (buf.simulating) {
            if (!isGhost(player) || !object) {
                return false;
            }
            if (typeinfo_cast<RingObject*>(object)) {
                if (isOrbUsedByGhost(object, player)) {
                    return false;
                }
                snapshotObject(object);
                return true;
            }
            auto& snap = snapshotObject(object);
            bool& usedByThisGhost = (player == buf.ghost1) ? snap.usedByG1 : snap.usedByG2;
            if (usedByThisGhost) {
                return false;
            }
            usedByThisGhost = true;
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
        if (static_cast<int>(relevant.capacity()) < objectCount) {
            relevant.reserve(objectCount);
        }

        constexpr uint64_t keep = COLLIDING_TYPES | INTERACT_TYPES;
        bool haveRect = false;
        CCRect ghostRect;
        auto& list = *objects;

        for (int i = 0; i < objectCount; ++i) {
            auto* object = list[i];
            if (!object || object->m_isDisabled || object->m_isGroupDisabled) {
                continue;
            }
            if (!buf.simDisabled.empty() && vecContains(buf.simDisabled, object)) {
                continue;
            }

            auto type = static_cast<unsigned>(object->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            if (keep & bit) {
                if (buf.haveRealRect && (bit & HAZARD_TYPES)) {
                    if (!buf.harmless.empty() && vecContains(buf.harmless, object)) {
                        continue;
                    }
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
            if (!stop && !size && speed <= 0.f) {
                continue;
            }
            if (!haveRect) {
                ghostRect = player->getObjectRect();
                haveRect = true;
            }
            if (ghostRect.intersectsRect(object->getObjectRect())) {
                if (stop) {
                    buf.ghostHitPortal = true;
                } else if (size) {
                    bool mini = object->m_objectType == GameObjectType::MiniSizePortal;
                    if ((player->m_vehicleSize < 1.f) != mini) {
                        player->togglePlayerScale(mini, true);
                    }
                } else {
                    player->m_playerSpeed = speed;
                }
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!buf.simulating && button == 1 && !buf.injecting) {
            auto* layer = GameManager::get()->m_playLayer;
            recordRealInput(down, isPlayer1, layer);
            bool held = intercept(this, down, isPlayer1);
            sweepGhostLayer();
            if (held) {
                return;
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (buf.simulating) {
            if (!isGhost(player)) return;
            snapshotObject(ring);
            buf.touchedOrb = true;
            if (buf.firstOrbTick < 0) {
                buf.firstOrbTick = buf.simTick;
            }
            buf.lastOrbTick = buf.simTick;
            if (buf.orbUsedTick >= 0 && !isOrbUsedByGhost(ring, player)) {
                buf.laterOrbTouched = true;
            }
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);

        bool autoOrb = buf.orbClick || (buf.orbAfterFirst && buf.orbUsedTick >= 0 && !isOrbUsedByGhost(ring, player));
        if (buf.simulating && autoOrb && !isOrbUsedByGhost(ring, player)) {
            bool first = player == buf.ghost1;
            bool& held = first ? buf.held1 : buf.held2;
            bool& autoClick = first ? buf.auto1 : buf.auto2;
            int& autoAt = first ? buf.autoAt1 : buf.autoAt2;

            if (buf.orbClickDelay > 0) {
                if (autoAt == -1) {
                    autoAt = buf.simTick + buf.orbClickDelay;
                }
            } else if (!autoClick) {
                if (held && buf.simTick > 0 && !player->m_isDashing) {
                    player->releaseButton(PlayerButton::Jump);
                }
                player->pushButton(PlayerButton::Jump);
                autoClick = true;
            }
        }
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (buf.simulating) {
            if (isGhost(player)) {
                player->flipGravity(flip, true);
            }
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
            if (!activate) {
                if (auto* objects = this->getGroup(group)) {
                    for (auto* object : CCArrayExt<GameObject*>(objects)) {
                        if (!vecContains(buf.simDisabled, object)) {
                            buf.simDisabled.push_back(object);
                        }
                    }
                }
            }
            return;
        }
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (buf.simulating) return;
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

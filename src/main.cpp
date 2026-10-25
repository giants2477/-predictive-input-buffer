// ===========================================================================
// Predictive Input Buffer v9.7 (Stealth Physics Engine) - Final Production
// 65-Tick Ship/Wave | 130-Tick Cube/UFO/Swing/Ball/Robot/Spider | Fast Sim
// ===========================================================================

#include <Geode/Geode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>

#ifdef GEODE_IS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr int MODE_COUNT = 8;
    constexpr double BUDGET_MS = 8.5;
    constexpr int FLYING_MAX_DELAY = 16;

    constexpr int DELAY_CAP = 31;
    constexpr int MAX_PRE = 8;
    constexpr int TRAJ_MAX = 96;
    constexpr int SIM_STEP_CAP = 192;
    constexpr int MIN_EVENT_GAP = 1;
    constexpr int WINDOW_PAD = 1;
    constexpr int SCRATCH_DEPTH = 8;
    constexpr float CLEAR_CAP = 48.0f;

    constexpr float REALITY_EPS_Y = 7.5f;
    constexpr float REALITY_EPS_X = 24.0f;
    constexpr float REALITY_EPS_SPIDER_Y = 85.0f;

    constexpr char const* MODE_NAMES[MODE_COUNT] = {
        "Cube", "Wave", "Ship", "UFO", "Swing", "Ball", "Robot", "Spider"
    };

    inline uint64_t nowMs() {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    constexpr uint64_t COLLIDING_TYPES =
        typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) |
        typeBit(GameObjectType::Slope) | typeBit(GameObjectType::CollisionObject) |
        typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t RING_TYPES =
        typeBit(GameObjectType::YellowJumpRing) | typeBit(GameObjectType::PinkJumpRing) |
        typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
        typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) |
        typeBit(GameObjectType::CustomRing) | typeBit(GameObjectType::DashRing) |
        typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb);

    constexpr uint64_t PAD_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) |
        typeBit(GameObjectType::RedJumpPad) | typeBit(GameObjectType::GravityPad) |
        typeBit(GameObjectType::SpiderPad);

    constexpr uint64_t GRAV_PORTAL_TYPES =
        typeBit(GameObjectType::InverseGravityPortal) |
        typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal);

    constexpr uint64_t INTERACT_TYPES = RING_TYPES | PAD_TYPES | GRAV_PORTAL_TYPES;

    constexpr uint64_t MODE_PORTALS =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) |
        typeBit(GameObjectType::BallPortal) | typeBit(GameObjectType::UfoPortal) |
        typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal);

    constexpr uint64_t SIZE_PORTALS =
        typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal);

    constexpr uint64_t MIRROR_PORTALS =
        typeBit(GameObjectType::InverseMirrorPortal) | typeBit(GameObjectType::NormalMirrorPortal);

    constexpr uint64_t UNSIM_PORTALS =
        typeBit(GameObjectType::DualPortal) | typeBit(GameObjectType::SoloPortal) |
        typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

    constexpr uint64_t PORTAL_TYPES = MODE_PORTALS | SIZE_PORTALS | MIRROR_PORTALS | UNSIM_PORTALS;
    constexpr uint64_t HAZARD_TYPES = typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::AnimatedHazard);

    inline bool isSpecialGameplayBlock(int id) {
        return id == 1755 || id == 1813 || id == 1829 || id == 1859 || id == 2866 || id == 2867;
    }

    float speedPortalValue(int id) {
        switch (id) {
            case 200: return 0.7f;
            case 201: return 0.9f;
            case 202: return 1.1f;
            case 203: return 1.3f;
            case 1334: return 1.6f;
            default: return 0.0f;
        }
    }

    template <class L, class P>
    bool isUsing22ShipPhysics(L* layer, P* player) {
        if (player) {
            if constexpr (requires { player->m_fixGravityBug; }) return player->m_fixGravityBug;
        }
        if (layer) {
            if constexpr (requires { layer->m_levelSettings->m_fixGravityBug; }) {
                if (layer->m_levelSettings) return layer->m_levelSettings->m_fixGravityBug;
            }
            if constexpr (requires { layer->m_level->m_gameVersion; }) {
                if (layer->m_level) return layer->m_level->m_gameVersion >= 22;
            }
        }
        return true;
    }

    template <class P>
    bool isPlayerInputLocked(P* player) {
        if (!player) return false;
        if constexpr (requires { player->m_inputsLocked; }) {
            if (player->m_inputsLocked) return true;
        }
        if constexpr (requires { player->m_isLocked; }) {
            if (player->m_isLocked) return true;
        }
        return false;
    }

    template <class P>
    bool isPlayerSideways(P* player) {
        if (!player) return false;
        if constexpr (requires { player->m_isSideways; }) return player->m_isSideways;
        return false;
    }

    template <class P>
    bool wasPlayerTeleported(P* player) {
        if (!player) return false;
        if constexpr (requires { player->m_wasTeleported; }) return player->m_wasTeleported;
        return false;
    }

    template <class R>
    bool isMultiActivateRing(R* ring) {
        if (!ring) return false;
        if constexpr (requires { ring->m_isMultiActivate; }) return ring->m_isMultiActivate;
        return false;
    }

    template <class L>
    inline bool isAntiCheatObject(L* layer, GameObject* object) {
        if (!layer || !object) return false;
        if constexpr (requires { layer->m_antiCheatObject; }) return object == layer->m_antiCheatObject;
        return false;
    }

    template <class L>
    inline bool isTwoPlayerMode(L* layer) {
        if (!layer) return false;
        if (layer->m_level && layer->m_level->m_twoPlayerMode) return true;
        if constexpr (requires { layer->m_levelSettings->m_twoPlayerMode; }) {
            if (layer->m_levelSettings && layer->m_levelSettings->m_twoPlayerMode) return true;
        }
        return false;
    }

    inline bool isTwoPlayerDualActive(PlayLayer* layer) {
        return layer && layer->m_gameState.m_isDualMode && layer->m_player2 && isTwoPlayerMode(layer);
    }

    inline bool isSymmetricDualActive(PlayLayer* layer) {
        return layer && layer->m_gameState.m_isDualMode && layer->m_player2 && !isTwoPlayerMode(layer);
    }

    // ---------------------------------------------------------------------------
    // Stages & Configuration (Default = Index 2: +8t / 2f @ 60fps)
    // ---------------------------------------------------------------------------
    struct Stage {
        bool enabled;
        int ticks;
    };

    constexpr Stage STAGES[] = {
        { true, 2 },   // Index 0 -> +2t (0.5f @ 60fps)
        { true, 4 },   // Index 1 -> +4t (1.0f @ 60fps)
        { true, 8 },   // Index 2 -> +8t (2.0f @ 60fps) [Default]
        { true, 16 },  // Index 3 -> +16t (4.0f @ 60fps)
        { false, 0 }   // Index 4 -> OFF
    };
    constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);

    struct Config {
        std::atomic<int> stage{ 2 };
        std::atomic<int> lastActive{ 2 };
    } cfg;

    std::atomic<int> g_totalSaves{ 0 };
    std::atomic<int> g_tickCount{ 0 };
    std::atomic<int> g_curMode{ 0 };
    std::atomic<bool> g_curShip22{ true };
    std::atomic<uint64_t> g_lastTickMs{ 0 };
    std::atomic<bool> g_resetRequest{ false };

    bool stageEnabled() { return STAGES[cfg.stage.load(std::memory_order_relaxed)].enabled; }
    int stageTicks() { return STAGES[cfg.stage.load(std::memory_order_relaxed)].ticks; }

    std::string stageText() {
        int idx = cfg.stage.load(std::memory_order_relaxed);
        auto const& s = STAGES[idx];
        if (!s.enabled) return "Buffer: OFF (100% Raw Input)";
        return fmt::format("STAGE {}/{}: Max delay +{}t ({:g}f @ 60fps)", idx + 1, STAGE_COUNT - 1, s.ticks, s.ticks * TICK);
    }

    void saveConfig() {
        Loader::get()->queueInMainThread([] {
            auto* mod = Mod::get();
            mod->setSavedValue<int>("stage_v9_8t", cfg.stage.load(std::memory_order_relaxed));
            if constexpr (requires { mod->saveData(); }) {
                (void)mod->saveData();
            }
        });
    }

    void loadConfig() {
        int s = Mod::get()->getSavedValue<int>("stage_v9_8t", 2);
        if (s < 0 || s >= STAGE_COUNT) s = 2;
        cfg.stage = s;
        cfg.lastActive = STAGES[s].enabled ? s : 2;
    }

    // ---------------------------------------------------------------------------
    // Non-Blocking External Console Output
    // ---------------------------------------------------------------------------
    std::mutex logMutex;
    std::condition_variable logCv;
    std::deque<std::string> logQueue;
    std::atomic<bool> logReady{ false };
    std::atomic<bool> consoleVisible{ true };
    std::atomic<bool> colorSupported{ false };

    constexpr char const* C_RESET  = "\x1b[0m";
    constexpr char const* C_GREEN  = "\x1b[92m";
    constexpr char const* C_RED    = "\x1b[91m";
    constexpr char const* C_YELLOW = "\x1b[93m";
    constexpr char const* C_CYAN   = "\x1b[96m";

    char const* col(char const* code) {
        return colorSupported.load(std::memory_order_relaxed) ? code : "";
    }

    void updateConsoleTitle() {
#ifdef GEODE_IS_WINDOWS
        std::string title = fmt::format("Predictive Input Buffer v9.7 | {} | Saves: {}", stageText(), g_totalSaves.load(std::memory_order_relaxed));
        SetConsoleTitleA(title.c_str());
#endif
    }

    void pushConsoleLog(std::string msg) {
        if (!logReady.load(std::memory_order_relaxed)) return;
        {
            std::lock_guard<std::mutex> lock(logMutex);
            if (logQueue.size() < 1000) logQueue.push_back(std::move(msg));
        }
        logCv.notify_one();
    }

    template <class... Args>
    void say(fmt::format_string<Args...> f, Args&&... args) {
        pushConsoleLog(fmt::format(f, std::forward<Args>(args)...));
    }

    void logWriterLoop() {
#ifdef GEODE_IS_WINDOWS
        HANDLE out = CreateFileA("CONOUT\x24", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        std::deque<std::string> batch;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(logMutex);
                logCv.wait(lock, [] { return !logQueue.empty(); });
                batch.swap(logQueue);
            }
            if (out != INVALID_HANDLE_VALUE) {
                for (auto& line : batch) {
                    line += "\r\n";
                    DWORD written = 0;
                    WriteConsoleA(out, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
                }
            }
            batch.clear();
            updateConsoleTitle();
        }
#endif
    }

    void setupExternalConsole() {
#ifdef GEODE_IS_WINDOWS
        AllocConsole();

        HANDLE hIn = CreateFileA("CONIN\x24", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hIn != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(hIn, &mode)) {
                mode &= ~ENABLE_QUICK_EDIT_MODE;
                mode |= ENABLE_EXTENDED_FLAGS;
                SetConsoleMode(hIn, mode);
            }
            CloseHandle(hIn);
        }

        HANDLE hOut = CreateFileA("CONOUT\x24", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD mode = 0;
            if (GetConsoleMode(hOut, &mode) && SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                colorSupported = true;
            }
            CloseHandle(hOut);
        }

        HWND con = GetConsoleWindow();
        if (con) {
            HMENU menu = GetSystemMenu(con, FALSE);
            if (menu) {
                DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
                DrawMenuBar(con);
            }
        }
        SetConsoleCtrlHandler(nullptr, TRUE);

        logReady = true;
        std::thread(logWriterLoop).detach();
#endif
    }

    void setStage(int idx) {
        idx = ((idx % STAGE_COUNT) + STAGE_COUNT) % STAGE_COUNT;
        cfg.stage = idx;
        if (STAGES[idx].enabled) cfg.lastActive = idx;
        g_resetRequest = true;
        say("{}-> Switched to {}{}", col(C_YELLOW), stageText(), col(C_RESET));
        saveConfig();
    }

    void toggleMasterBuffer() {
        if (stageEnabled()) {
            setStage(STAGE_COUNT - 1);
        } else {
            setStage(cfg.lastActive.load());
        }
    }

#ifdef GEODE_IS_WINDOWS
    bool isGameOrConsoleFocused() {
        HWND fg = GetForegroundWindow();
        if (!fg) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        return pid == GetCurrentProcessId();
    }

    struct KeyState {
        bool wasDown = false;
        bool check(bool isDown) {
            bool pressed = isDown && !wasDown;
            wasDown = isDown;
            return pressed;
        }
    };

    void hotkeyThreadLoop() {
        KeyState kF6, kB, kF7, kF8, kF9;
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));

            bool focused = isGameOrConsoleFocused();
            uint64_t last = g_lastTickMs.load(std::memory_order_relaxed);
            bool inLevel = focused && last != 0 && (nowMs() - last) < 500;
            bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

            bool f6 = kF6.check(focused && (GetAsyncKeyState(VK_F6) & 0x8000));
            bool b  = kB.check(inLevel && (GetAsyncKeyState('B') & 0x8000));
            bool f7 = kF7.check(focused && (GetAsyncKeyState(VK_F7) & 0x8000));
            bool f8 = kF8.check(focused && (GetAsyncKeyState(VK_F8) & 0x8000));
            bool f9 = kF9.check(focused && (GetAsyncKeyState(VK_F9) & 0x8000));

            if (f6 || b) setStage(cfg.stage.load() + (shift ? -1 : 1));
            if (f7) toggleMasterBuffer();
            if (f8) {
                Loader::get()->queueInMainThread([] {
                    int m = std::clamp(g_curMode.load(), 0, MODE_COUNT - 1);
                    bool s22 = g_curShip22.load();
                    say("{}--- Live Diagnostics ---{}", col(C_CYAN), col(C_RESET));
                    say("Active Vehicle: {} | Horizon: {}t | Ship Physics: {} | Saves: {}",
                        MODE_NAMES[m], (m == 1 || m == 2) ? 65 : 130,
                        s22 ? "2.2 (Symmetric)" : "2.1 (Legacy Inverted)", g_totalSaves.load());
                    say("Active State: {}", stageText());
                });
            }
            if (f9) {
                HWND con = GetConsoleWindow();
                if (con) {
                    bool show = !consoleVisible.load();
                    ShowWindow(con, show ? SW_SHOW : SW_HIDE);
                    consoleVisible = show;
                }
            }
        }
    }
#endif

    // ---------------------------------------------------------------------------
    // Adaptive Rhythm Engine
    // ---------------------------------------------------------------------------
    struct Ring {
        int v[8] = {};
        int n = 0;
        int i = 0;

        void add(int x) {
            v[i] = x;
            i = (i + 1) % 8;
            n = std::min(n + 1, 8);
        }

        int median(int fallback) const {
            if (n == 0) return fallback;
            int tmp[8];
            std::copy(v, v + n, tmp);
            std::nth_element(tmp, tmp + n / 2, tmp + n);
            return tmp[n / 2];
        }

        int adaptive(int fallback) const {
            if (n < 2) return median(fallback);
            int last1 = v[(i - 1 + 8) % 8];
            int last2 = v[(i - 2 + 8) % 8];
            if (std::abs(last1 - last2) <= 3 || (last1 < 8 && last2 < 8)) {
                return std::max(1, (last1 + last2) / 2);
            }
            return median(fallback);
        }

        std::string serialize() const {
            std::string s = fmt::format("{},{}", n, i);
            for (int j = 0; j < 8; ++j) s += fmt::format(",{}", v[j]);
            return s;
        }

        void deserialize(std::string const& str) {
            if (str.empty()) return;
            try {
                size_t pos = 0;
                auto nextInt = [&]() {
                    size_t next = str.find(',', pos);
                    std::string token = (next == std::string::npos) ? str.substr(pos) : str.substr(pos, next - pos);
                    pos = (next == std::string::npos) ? str.size() + 1 : next + 1;
                    return std::stoi(token);
                };
                n = std::clamp(nextInt(), 0, 8);
                i = std::clamp(nextInt(), 0, 7);
                for (int j = 0; j < 8; ++j) v[j] = std::clamp(nextInt(), 1, 240);
            } catch (...) {}
        }
    };

    struct RhythmModel {
        Ring holds[MODE_COUNT];
        Ring gaps[MODE_COUNT];
        int pressTick = -1;
        int releaseTick = -1;
        int learnCount = 0;

        void record(int mode, bool down, int currentTick) {
            mode = std::clamp(mode, 0, MODE_COUNT - 1);
            if (down) {
                if (releaseTick >= 0) {
                    int gap = currentTick - releaseTick;
                    if (gap <= 120) {
                        gaps[mode].add(std::max(gap, 1));
                        ++learnCount;
                    }
                }
                pressTick = currentTick;
            } else {
                if (pressTick < 0) return;
                holds[mode].add(std::clamp(currentTick - pressTick, 1, 240));
                ++learnCount;
                pressTick = -1;
                releaseTick = currentTick;
            }
        }

        void reset() {
            pressTick = -1;
            releaseTick = -1;
        }

        void persist(int ch) {
            if (learnCount <= 0) return;
            auto* mod = Mod::get();
            for (int m = 0; m < MODE_COUNT; ++m) {
                mod->setSavedValue<std::string>(fmt::format("h9_{}_{}", ch, m), holds[m].serialize());
                mod->setSavedValue<std::string>(fmt::format("g9_{}_{}", ch, m), gaps[m].serialize());
            }
            learnCount = 0;
        }

        void load(int ch) {
            auto* mod = Mod::get();
            for (int m = 0; m < MODE_COUNT; ++m) {
                std::string hStr = mod->getSavedValue<std::string>(fmt::format("h9_{}_{}", ch, m), "");
                std::string gStr = mod->getSavedValue<std::string>(fmt::format("g9_{}_{}", ch, m), "");
                if (ch == 0 && hStr.empty()) hStr = mod->getSavedValue<std::string>(fmt::format("h8_{}", m), "");
                if (ch == 0 && gStr.empty()) gStr = mod->getSavedValue<std::string>(fmt::format("g8_{}", m), "");
                holds[m].deserialize(hStr);
                gaps[m].deserialize(gStr);
            }
        }
    } g_rhythm[2];

    // ---------------------------------------------------------------------------
    // Engine State & Queued Predictions
    // ---------------------------------------------------------------------------
    struct QueuedInput {
        int ticksRemaining;
        int elapsedTicks;
        bool down;
    };

    struct ObjSnapshot {
        GameObject* object = nullptr;
        float rotation = 0.0f;
        bool isActivated = false;
        bool objHasBeenP1 = false;
        bool objHasBeenP2 = false;
        EffectGameObject* effect = nullptr;
        bool activated = false;
        bool byP1 = false;
        bool byP2 = false;
        bool effHasBeenP1 = false;
        bool effHasBeenP2 = false;
    };

    template <class T>
    void saveActivationExtras(T* obj, bool& hasP1, bool& hasP2) {
        if constexpr (requires { hasP1 = obj->m_hasBeenActivated; }) hasP1 = obj->m_hasBeenActivated;
        if constexpr (requires { hasP2 = obj->m_hasBeenActivatedP2; }) hasP2 = obj->m_hasBeenActivatedP2;
    }

    template <class T>
    void restoreActivationExtras(T* obj, bool hasP1, bool hasP2) {
        if constexpr (requires { obj->m_hasBeenActivated = hasP1; }) obj->m_hasBeenActivated = hasP1;
        if constexpr (requires { obj->m_hasBeenActivatedP2 = hasP2; }) obj->m_hasBeenActivatedP2 = hasP2;
    }

    template <class T>
    bool checkHasBeenActivated(T* obj, bool isSimulatingP1) {
        bool used = false;
        if (isSimulatingP1) {
            if constexpr (requires { used = obj->m_hasBeenActivated; }) {
                if (obj->m_hasBeenActivated) used = true;
            }
        } else {
            if constexpr (requires { used = obj->m_hasBeenActivatedP2; }) {
                if (obj->m_hasBeenActivatedP2) used = true;
            }
        }
        return used;
    }

    struct Trajectory {
        float x[2][TRAJ_MAX] = {};
        float y[2][TRAJ_MAX] = {};
        float startX[2] = {};
        float startY[2] = {};
        float stepDt = TICK;
        float accumDt = 0.0f;
        int count = 0;
        int steps = 0;
        bool dual = false;
        bool active = false;
        bool spiderActive = false;
        bool replanned = false;
    };

    struct BufferChannel {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLayer* ghostLayer = nullptr;
        RingObject* lastTouchedRing = nullptr;

        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool simUnverified = false;
        bool simHitPortalTransition = false;
        bool padOrPortalFlippedTick = false;
        bool measureClearance = true;
        bool triggerStateChanged = false;
        bool realInput = false;
        bool appliedInput = false;
        int consumedRingsCount = 0;

        int lastInterceptTick = -1;
        bool lastInterceptDown = false;
        bool lastInterceptIsP1 = true;
        bool lastInterceptBuffered = false;

        std::deque<QueuedInput> queue;

        float tickLength = TICK;
        float recentDt[4] = { TICK, TICK, TICK, TICK };
        int dtIdx = 0;
        int sinceInput = 999;
        int lastMode = -1;
        int lastShipPhysics = -1;
        int unverifiedTicks = 0;
        float tickClear = CLEAR_CAP;

        gd::vector<GameObject*> filteredScratch[SCRATCH_DEPTH];
        int collisionDepth = 0;

        std::unordered_set<GameObject*> snapshotted;
        std::unordered_set<GameObject*> consumedP1;
        std::unordered_set<GameObject*> consumedP2;
        std::vector<ObjSnapshot> snapshots;

        struct PortalSeen {
            GameObject* object;
            PlayerObject* player;
        } portalSeen[32];
        int portalSeenN = 0;

        Trajectory traj;

        bool isGhost(PlayerObject* p) const {
            return p && (p == ghost1 || p == ghost2);
        }

        void reservePools() {
            snapshotted.reserve(64);
            consumedP1.reserve(64);
            consumedP2.reserve(64);
            snapshots.reserve(64);
            for (int i = 0; i < SCRATCH_DEPTH; ++i) filteredScratch[i].reserve(128);
        }

        void recordSubstepDt(float dt) {
            if (dt >= 0.05f && dt <= 1.0f) {
                recentDt[dtIdx] = dt;
                dtIdx = (dtIdx + 1) & 3;
                float tmp[4] = { recentDt[0], recentDt[1], recentDt[2], recentDt[3] };
                std::sort(std::begin(tmp), std::end(tmp));
                tickLength = tmp[1];
            }
        }

        void clearSnapshotsAndConsumed() {
            for (auto const& s : snapshots) {
                if (!s.object) continue;
                s.object->setRotation(s.rotation);
                s.object->m_isActivated = s.isActivated;
                restoreActivationExtras(s.object, s.objHasBeenP1, s.objHasBeenP2);
                if (s.effect) {
                    s.effect->m_activated = s.activated;
                    s.effect->m_activatedByPlayer1 = s.byP1;
                    s.effect->m_activatedByPlayer2 = s.byP2;
                    restoreActivationExtras(s.effect, s.effHasBeenP1, s.effHasBeenP2);
                }
            }
            snapshots.clear();
            snapshotted.clear();
            consumedP1.clear();
            consumedP2.clear();
            consumedRingsCount = 0;
        }

        void resetQueue() { queue.clear(); }

        void resetVerification() {
            traj.active = false;
            traj.replanned = false;
            unverifiedTicks = 0;
        }

        void resetState() {
            clearSnapshotsAndConsumed();
            resetQueue();
            resetVerification();
            lastTouchedRing = nullptr;
            triggerStateChanged = false;
            appliedInput = false;
            realInput = false;
            measureClearance = true;
            sinceInput = 999;
            lastInterceptTick = -1;
            lastInterceptDown = false;
            lastInterceptIsP1 = true;
            lastInterceptBuffered = false;
            collisionDepth = 0;
            tickLength = TICK;
            recentDt[0] = recentDt[1] = recentDt[2] = recentDt[3] = TICK;
        }

        void dropGhosts() {
            clearSnapshotsAndConsumed();
            for (auto** g : { &ghost1, &ghost2 }) {
                if (*g) {
                    (*g)->removeFromParentAndCleanup(true);
                    (*g)->release();
                    *g = nullptr;
                }
            }
            if (ghostLayer) {
                ghostLayer->removeAllChildrenWithCleanup(true);
                ghostLayer->release();
                ghostLayer = nullptr;
            }
            simulating = false;
            injecting = false;
            resetState();
        }

        PlayerObject* makeGhost(PlayLayer* layer) {
            reservePools();
            if (!ghostLayer) {
                ghostLayer = CCLayer::create();
                ghostLayer->setVisible(false);
                ghostLayer->retain();
            }
            auto* ghost = PlayerObject::create(1, 1, layer, ghostLayer, true);
            ghost->retain();
            ghost->setPosition({ 0.0f, 105.0f });
            ghost->setVisible(false);
            ghostLayer->addChild(ghost);
            return ghost;
        }
    } g_buf[2];

    int getPlayerMode(PlayerObject* p) {
        if (!p) return 0;
        if (p->m_isDart) return 1;
        if (p->m_isShip) return 2;
        if (p->m_isBird) return 3;
        if (p->m_isSwing) return 4;
        if (p->m_isBall) return 5;
        if (p->m_isRobot) return 6;
        if (p->m_isSpider) return 7;
        return 0;
    }

    int portalTypeToMode(GameObjectType type) {
        switch (type) {
            case GameObjectType::CubePortal:   return 0;
            case GameObjectType::WavePortal:   return 1;
            case GameObjectType::ShipPortal:   return 2;
            case GameObjectType::UfoPortal:    return 3;
            case GameObjectType::SwingPortal:  return 4;
            case GameObjectType::BallPortal:   return 5;
            case GameObjectType::RobotPortal:  return 6;
            case GameObjectType::SpiderPortal: return 7;
            default: return -1;
        }
    }

    int getModeHorizon(PlayLayer* layer, PlayerObject* p, int mode) {
        (void)layer; (void)p;
        return (mode == 1 || mode == 2) ? 65 : 130;
    }

    int getPilotCommitWindow(PlayerObject* p, int mode, bool isBaseline) {
        bool mini = p && p->m_vehicleSize < 1.0f;
        if (!isBaseline) {
            switch (mode) {
                case 1: return mini ? 8 : 10;
                case 2: return 6;
                case 3: return 10;
                case 4: return 10;
                default: return 7;
            }
        }
        switch (mode) {
            case 1: return mini ? 13 : 16;
            case 2: return mini ? 18 : 22;
            case 3: return mini ? 22 : 26;
            case 4: return mini ? 18 : 22;
            case 5: return mini ? 20 : 24;
            case 7: return mini ? 16 : 20;
            default: return mini ? 22 : 26;
        }
    }

    template <class L>
    bool isPlatformer(L* layer) {
        if constexpr (requires { layer->m_levelSettings->m_platformerMode; }) {
            return layer->m_levelSettings && layer->m_levelSettings->m_platformerMode;
        } else if constexpr (requires { layer->m_player1->m_isPlatformer; }) {
            return layer->m_player1 && layer->m_player1->m_isPlatformer;
        }
        return false;
    }

    template <class P>
    bool applyVehicleSwitch(P* g, GameObjectType type) {
        if (!g) return false;
        int targetMode = portalTypeToMode(type);
        if (targetMode < 0) return false;
        if (getPlayerMode(g) == targetMode) return true;

        g->toggleFlyMode(false, true);
        g->toggleRollMode(false, true);
        g->toggleBirdMode(false, true);
        g->toggleDartMode(false, true);
        g->toggleRobotMode(false, true);
        g->toggleSpiderMode(false, true);
        g->toggleSwingMode(false, true);

        switch (type) {
            case GameObjectType::ShipPortal:   g->toggleFlyMode(true, true); break;
            case GameObjectType::BallPortal:   g->toggleRollMode(true, true); break;
            case GameObjectType::UfoPortal:    g->toggleBirdMode(true, true); break;
            case GameObjectType::WavePortal:   g->toggleDartMode(true, true); break;
            case GameObjectType::RobotPortal:  g->toggleRobotMode(true, true); break;
            case GameObjectType::SpiderPortal: g->toggleSpiderMode(true, true); break;
            case GameObjectType::SwingPortal:  g->toggleSwingMode(true, true); break;
            case GameObjectType::CubePortal:   break;
            default: return false;
        }
        return true;
    }

    template <class P>
    void syncOptionalFields(P* ghost, P* real) {
        if constexpr (requires { ghost->m_isHolding = real->m_isHolding; }) ghost->m_isHolding = real->m_isHolding;
        if constexpr (requires { ghost->m_hasJustHeld = real->m_hasJustHeld; }) ghost->m_hasJustHeld = real->m_hasJustHeld;
        if constexpr (requires { ghost->m_isHolding2 = real->m_isHolding2; }) ghost->m_isHolding2 = real->m_isHolding2;
        if constexpr (requires { ghost->m_hasJustHeld2 = real->m_hasJustHeld2; }) ghost->m_hasJustHeld2 = real->m_hasJustHeld2;
        if constexpr (requires { ghost->m_position = real->m_position; }) ghost->m_position = real->m_position;
        if constexpr (requires { ghost->m_lastPosition = real->m_lastPosition; }) ghost->m_lastPosition = real->m_lastPosition;
        if constexpr (requires { ghost->m_lastGroundedPos = real->m_lastGroundedPos; }) ghost->m_lastGroundedPos = real->m_lastGroundedPos;
        if constexpr (requires { ghost->m_fixGravityBug = real->m_fixGravityBug; }) ghost->m_fixGravityBug = real->m_fixGravityBug;
        if constexpr (requires { ghost->m_fixRobotJump = real->m_fixRobotJump; }) ghost->m_fixRobotJump = real->m_fixRobotJump;
        if constexpr (requires { ghost->m_inputsLocked = real->m_inputsLocked; }) ghost->m_inputsLocked = real->m_inputsLocked;
        if constexpr (requires { ghost->m_isSideways = real->m_isSideways; }) ghost->m_isSideways = real->m_isSideways;
        if constexpr (requires { ghost->m_reverseRelated = real->m_reverseRelated; }) ghost->m_reverseRelated = real->m_reverseRelated;
        if constexpr (requires { ghost->m_maybeReverseSpeed = real->m_maybeReverseSpeed; }) ghost->m_maybeReverseSpeed = real->m_maybeReverseSpeed;
        if constexpr (requires { ghost->m_maybeReverseAcceleration = real->m_maybeReverseAcceleration; }) ghost->m_maybeReverseAcceleration = real->m_maybeReverseAcceleration;
        if constexpr (requires { ghost->m_xVelocityRelated2 = real->m_xVelocityRelated2; }) ghost->m_xVelocityRelated2 = real->m_xVelocityRelated2;
        if constexpr (requires { ghost->m_reverseSync = real->m_reverseSync; }) ghost->m_reverseSync = real->m_reverseSync;
        if constexpr (requires { ghost->m_stateForce = real->m_stateForce; }) ghost->m_stateForce = real->m_stateForce;
        if constexpr (requires { ghost->m_stateForceVector = real->m_stateForceVector; }) ghost->m_stateForceVector = real->m_stateForceVector;
        if constexpr (requires { ghost->m_maybeStateForce2 = real->m_maybeStateForce2; }) ghost->m_maybeStateForce2 = real->m_maybeStateForce2;
        if constexpr (requires { ghost->m_affectedByForces = real->m_affectedByForces; }) ghost->m_affectedByForces = real->m_affectedByForces;
        if constexpr (requires { ghost->m_stateBoostX = real->m_stateBoostX; }) ghost->m_stateBoostX = real->m_stateBoostX;
        if constexpr (requires { ghost->m_stateBoostY = real->m_stateBoostY; }) ghost->m_stateBoostY = real->m_stateBoostY;
        if constexpr (requires { ghost->m_stateScale = real->m_stateScale; }) ghost->m_stateScale = real->m_stateScale;
        if constexpr (requires { ghost->m_stateNoAutoJump = real->m_stateNoAutoJump; }) ghost->m_stateNoAutoJump = real->m_stateNoAutoJump;
        if constexpr (requires { ghost->m_stateDartSlide = real->m_stateDartSlide; }) ghost->m_stateDartSlide = real->m_stateDartSlide;
        if constexpr (requires { ghost->m_stateHitHead = real->m_stateHitHead; }) ghost->m_stateHitHead = real->m_stateHitHead;
        if constexpr (requires { ghost->m_stateFlipGravity = real->m_stateFlipGravity; }) ghost->m_stateFlipGravity = real->m_stateFlipGravity;
        if constexpr (requires { ghost->m_stateOnGround = real->m_stateOnGround; }) ghost->m_stateOnGround = real->m_stateOnGround;
        if constexpr (requires { ghost->m_stateNoStickX = real->m_stateNoStickX; }) ghost->m_stateNoStickX = real->m_stateNoStickX;
        if constexpr (requires { ghost->m_stateNoStickY = real->m_stateNoStickY; }) ghost->m_stateNoStickY = real->m_stateNoStickY;
        if constexpr (requires { ghost->m_gravity = real->m_gravity; }) ghost->m_gravity = real->m_gravity;
        if constexpr (requires { ghost->m_fallSpeed = real->m_fallSpeed; }) ghost->m_fallSpeed = real->m_fallSpeed;
        if constexpr (requires { ghost->m_playerSpeedAC = real->m_playerSpeedAC; }) ghost->m_playerSpeedAC = real->m_playerSpeedAC;
        if constexpr (requires { ghost->m_shipRotation = real->m_shipRotation; }) ghost->m_shipRotation = real->m_shipRotation;
        if constexpr (requires { ghost->m_yVelocityBeforeSlope = real->m_yVelocityBeforeSlope; }) ghost->m_yVelocityBeforeSlope = real->m_yVelocityBeforeSlope;
        if constexpr (requires { ghost->m_dashStartTime = real->m_dashStartTime; }) ghost->m_dashStartTime = real->m_dashStartTime;
        if constexpr (requires { ghost->m_slopeStartTime = real->m_slopeStartTime; }) ghost->m_slopeStartTime = real->m_slopeStartTime;
        if constexpr (requires { ghost->m_lastLandTime = real->m_lastLandTime; }) ghost->m_lastLandTime = real->m_lastLandTime;
        if constexpr (requires { ghost->m_totalTime = real->m_totalTime; }) ghost->m_totalTime = real->m_totalTime;
        if constexpr (requires { ghost->m_isOnGround2 = real->m_isOnGround2; }) ghost->m_isOnGround2 = real->m_isOnGround2;
        if constexpr (requires { ghost->m_isOnGround3 = real->m_isOnGround3; }) ghost->m_isOnGround3 = real->m_isOnGround3;
        if constexpr (requires { ghost->m_isOnGround4 = real->m_isOnGround4; }) ghost->m_isOnGround4 = real->m_isOnGround4;
        if constexpr (requires { ghost->m_wasOnGround = real->m_wasOnGround; }) ghost->m_wasOnGround = real->m_wasOnGround;
        if constexpr (requires { ghost->m_isSliding = real->m_isSliding; }) ghost->m_isSliding = real->m_isSliding;
        if constexpr (requires { ghost->m_isOnSlope = real->m_isOnSlope; }) ghost->m_isOnSlope = real->m_isOnSlope;
        if constexpr (requires { ghost->m_wasOnSlope = real->m_wasOnSlope; }) ghost->m_wasOnSlope = real->m_wasOnSlope;
        if constexpr (requires { ghost->m_slopeVelocity = real->m_slopeVelocity; }) ghost->m_slopeVelocity = real->m_slopeVelocity;
        if constexpr (requires { ghost->m_isUpSlope = real->m_isUpSlope; }) ghost->m_isUpSlope = real->m_isUpSlope;
        if constexpr (requires { ghost->m_isDownSlope = real->m_isDownSlope; }) ghost->m_isDownSlope = real->m_isDownSlope;
        if constexpr (requires { ghost->m_maybeUpsideDownSlope = real->m_maybeUpsideDownSlope; }) ghost->m_maybeUpsideDownSlope = real->m_maybeUpsideDownSlope;
        if constexpr (requires { ghost->m_slopeAngle = real->m_slopeAngle; }) ghost->m_slopeAngle = real->m_slopeAngle;
        if constexpr (requires { ghost->m_slopeSlidingMaybeRotated = real->m_slopeSlidingMaybeRotated; }) ghost->m_slopeSlidingMaybeRotated = real->m_slopeSlidingMaybeRotated;
        if constexpr (requires { ghost->m_CurrentSlope = real->m_CurrentSlope; }) ghost->m_CurrentSlope = real->m_CurrentSlope;
        if constexpr (requires { ghost->m_LastSlope = real->m_LastSlope; }) ghost->m_LastSlope = real->m_LastSlope;
        if constexpr (requires { ghost->m_robotJumpTime = real->m_robotJumpTime; }) ghost->m_robotJumpTime = real->m_robotJumpTime;
        if constexpr (requires { ghost->m_wasTeleported = false; }) ghost->m_wasTeleported = false;
        if constexpr (requires { ghost->m_isDead = false; }) ghost->m_isDead = false;
        if constexpr (requires { ghost->m_isLocked = false; }) ghost->m_isLocked = false;

        if constexpr (requires { ghost->m_touchedRings->addObjectsFromArray(real->m_touchedRings); }) {
            if (ghost->m_touchedRings && real->m_touchedRings) {
                ghost->m_touchedRings->removeAllObjects();
                ghost->m_touchedRings->addObjectsFromArray(real->m_touchedRings);
            }
        } else if constexpr (requires { ghost->m_touchedRings = real->m_touchedRings; }) {
            if constexpr (!std::is_pointer_v<std::remove_reference_t<decltype(ghost->m_touchedRings)>>) {
                ghost->m_touchedRings = real->m_touchedRings;
            }
        }
    }

    template <class P>
    void clearTouchedRingField(P* ghost) {
        if constexpr (requires { ghost->m_touchedRing = nullptr; }) {
            ghost->m_touchedRing = nullptr;
        } else if constexpr (requires { ghost->m_touchedRing = false; }) {
            ghost->m_touchedRing = false;
        }
    }

    void copyPlayerState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->resetTouchedRings(true);

        applyVehicleSwitch(ghost,
            real->m_isShip   ? GameObjectType::ShipPortal :
            real->m_isBall   ? GameObjectType::BallPortal :
            real->m_isBird   ? GameObjectType::UfoPortal :
            real->m_isDart   ? GameObjectType::WavePortal :
            real->m_isRobot  ? GameObjectType::RobotPortal :
            real->m_isSpider ? GameObjectType::SpiderPortal :
            real->m_isSwing  ? GameObjectType::SwingPortal :
                               GameObjectType::CubePortal);

        ghost->copyAttributes(real);
        ghost->resetTouchedRings(true);

        ghost->m_isShip = false;
        ghost->m_isBall = false;
        ghost->m_isBird = false;
        ghost->m_isDart = false;
        ghost->m_isRobot = false;
        ghost->m_isSpider = false;
        ghost->m_isSwing = false;
        ghost->m_isOnGround = false;
        ghost->m_isDashing = false;
        clearTouchedRingField(ghost);

        if (real->m_holdingButtons[1]) {
            ghost->pushButton(PlayerButton::Jump);
        } else {
            ghost->releaseButton(PlayerButton::Jump);
        }

        ghost->m_isShip   = real->m_isShip;
        ghost->m_isBall   = real->m_isBall;
        ghost->m_isBird   = real->m_isBird;
        ghost->m_isDart   = real->m_isDart;
        ghost->m_isRobot  = real->m_isRobot;
        ghost->m_isSpider = real->m_isSpider;
        ghost->m_isSwing  = real->m_isSwing;

        ghost->m_yVelocity = real->m_yVelocity;
        ghost->m_isGoingLeft = real->m_isGoingLeft;
        ghost->m_dashX = real->m_dashX;
        ghost->m_dashY = real->m_dashY;
        ghost->m_dashAngle = real->m_dashAngle;
        ghost->m_dashRing = real->m_dashRing;

        ghost->setPosition(real->getPosition());
        ghost->setRotation(real->getRotation());
        ghost->setScale(real->getScale());
        ghost->setScaleX(real->getScaleX());
        ghost->setScaleY(real->getScaleY());
        ghost->setContentSize(real->getContentSize());

        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround;
        ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize;
        ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing;
        ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;

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

        syncOptionalFields(ghost, real);
    }

    int resolveChannelIndex(PlayLayer* layer, bool isPlayer1) {
        return (isTwoPlayerDualActive(layer) && !isPlayer1) ? 1 : 0;
    }

    void sendPlayerInput(PlayLayer* layer, int pIdx, bool down) {
        auto& b = g_buf[pIdx];
        b.injecting = true;
        b.appliedInput = down;
        if (pIdx == 0) {
            layer->handleButton(down, 1, true);
            if (isSymmetricDualActive(layer)) layer->handleButton(down, 1, false);
        } else {
            layer->handleButton(down, 1, false);
        }
        b.injecting = false;
    }

    void resyncChannel(PlayLayer* layer, int pIdx) {
        auto& b = g_buf[pIdx];
        if (b.appliedInput != b.realInput) sendPlayerInput(layer, pIdx, b.realInput);
    }

    void pressGhost(PlayerObject* ghost, bool down) {
        if (!ghost) return;
        ghost->m_holdingButtons[1] = down;
        if (down) ghost->pushButton(PlayerButton::Jump);
        else ghost->releaseButton(PlayerButton::Jump);
    }

    void clearGhostCollisionLogs(PlayerObject* ghost) {
        if (!ghost) return;
        if (ghost->m_collisionLogTop) ghost->m_collisionLogTop->removeAllObjects();
        if (ghost->m_collisionLogBottom) ghost->m_collisionLogBottom->removeAllObjects();
        if (ghost->m_collisionLogLeft) ghost->m_collisionLogLeft->removeAllObjects();
        if (ghost->m_collisionLogRight) ghost->m_collisionLogRight->removeAllObjects();
    }

    template <class G>
    bool isSlopeNegativeDiagonal(G* obj) {
        bool flipX = false, flipY = false;
        if constexpr (requires { obj->m_isFlippedX; obj->m_isFlippedY; }) {
            flipX = obj->m_isFlippedX;
            flipY = obj->m_isFlippedY;
        } else {
            flipX = obj->getScaleX() < 0.0f;
            flipY = obj->getScaleY() < 0.0f;
        }
        int quarterTurns = static_cast<int>(std::round(obj->getRotation() / 90.0f)) & 1;
        return (flipX != flipY) != (quarterTurns != 0);
    }

    float computeObjectClearanceFast(PlayerObject* player, CCRect const& pRect, GameObject* obj, unsigned type, float currentBest) {
        CCRect objRect = obj->getObjectRect();
        float dx = std::max({ objRect.getMinX() - pRect.getMaxX(), pRect.getMinX() - objRect.getMaxX(), 0.0f });
        if (dx >= currentBest) return currentBest;
        float dy = std::max({ objRect.getMinY() - pRect.getMaxY(), pRect.getMinY() - objRect.getMaxY(), 0.0f });
        if (dy >= currentBest) return currentBest;

        float distSq = dx * dx + dy * dy;
        if (distSq > 0.0f) return (distSq >= currentBest * currentBest) ? currentBest : std::sqrt(distSq);

        CCPoint pCenter = player->getPosition();
        float cx = objRect.getMidX(), cy = objRect.getMidY();
        float pRad = 0.25f * std::min(pRect.size.width, pRect.size.height);

        if (type == static_cast<unsigned>(GameObjectType::Slope)) {
            float w = std::max(1.0f, objRect.size.width);
            float h = std::max(1.0f, objRect.size.height);
            float len = std::hypot(w, h);
            bool negDiag = isSlopeNegativeDiagonal(obj);
            float diagDist = negDiag
                ? std::fabs((pCenter.x - cx) * h + (pCenter.y - cy) * w) / len
                : std::fabs((pCenter.x - cx) * h - (pCenter.y - cy) * w) / len;
            return std::max(0.05f, diagDist - pRad);
        }

        float centerDist = std::hypot(pCenter.x - cx, pCenter.y - cy);
        float minRad = pRad + 0.15f * std::min(objRect.size.width, objRect.size.height);
        return std::max(0.05f, (centerDist - minRad) * 0.35f);
    }

    void handleSimPortal(int pIdx, PlayerObject* ghost, GameObject* object) {
        auto& b = g_buf[pIdx];
        for (int i = 0; i < b.portalSeenN; ++i) {
            if (b.portalSeen[i].object == object && b.portalSeen[i].player == ghost) return;
        }
        if (b.portalSeenN >= 32) {
            b.simUnverified = true;
            return;
        }
        b.portalSeen[b.portalSeenN++] = { object, ghost };

        auto type = object->m_objectType;
        uint64_t bit = typeBit(type);
        bool ok = false;
        if (bit & MODE_PORTALS) {
            ok = applyVehicleSwitch(ghost, type);
        } else if (bit & SIZE_PORTALS) {
            bool wantMini = (type == GameObjectType::MiniSizePortal);
            bool isMini = (ghost->m_vehicleSize < 1.0f);
            if (wantMini != isMini) ghost->togglePlayerScale(wantMini, true);
            ok = true;
        } else if (bit & MIRROR_PORTALS) {
            ok = true;
        } else if (bit & UNSIM_PORTALS) {
            b.simHitPortalTransition = true;
            ok = false;
        }
        if (!ok) b.simUnverified = true;
    }

    void snapshotTarget(int pIdx, GameObject* obj) {
        auto& b = g_buf[pIdx];
        if (!obj || b.snapshotted.contains(obj)) return;
        b.snapshotted.insert(obj);

        ObjSnapshot snap;
        snap.object = obj;
        snap.rotation = obj->getRotation();
        snap.isActivated = obj->m_isActivated;
        saveActivationExtras(obj, snap.objHasBeenP1, snap.objHasBeenP2);

        if (auto* effect = typeinfo_cast<EffectGameObject*>(obj)) {
            snap.effect = effect;
            snap.activated = effect->m_activated;
            snap.byP1 = effect->m_activatedByPlayer1;
            snap.byP2 = effect->m_activatedByPlayer2;
            saveActivationExtras(effect, snap.effHasBeenP1, snap.effHasBeenP2);
        }
        b.snapshots.push_back(snap);
    }

    void restoreSnapshots(int pIdx) {
        g_buf[pIdx].clearSnapshotsAndConsumed();
    }

    enum class SimEnd { Ok, StopSearch };
    enum class SimPolicy { Sustained, Rhythm, SmartPilot };

    struct SimParams {
        int delay = 0;
        bool down = true;
        SimPolicy policy = SimPolicy::Sustained;
        int horizon = 130;
        int releaseAfter = -1;
        int stopAt = 130;
        int rHold = 0;
        int rGap = 0;
        int pilotStartOffset = 8;
        QueuedInput pre[MAX_PRE] = {};
        int preN = 0;
        Trajectory* rec = nullptr;
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    };

    struct SimResult {
        int survived = 0;
        float clear = CLEAR_CAP;
        float nearClear = CLEAR_CAP;
        float exitVel = 0.0f;
        bool safe = false;
        bool effective = true;
        bool unverified = false;
        bool p2Constrained = false;
    };

    SimResult runSimulation(int pIdx, PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, SimParams const& sp, SimEnd* end) {
        auto& b = g_buf[pIdx];
        SimResult res;
        *end = SimEnd::Ok;

        if (!b.ghost1) {
            res.survived = sp.horizon;
            res.safe = true;
            return res;
        }

        b.simulating = true;
        b.ghostDied = false;
        b.simUnverified = false;
        b.simHitPortalTransition = false;
        b.padOrPortalFlippedTick = false;
        b.consumedRingsCount = 0;
        b.portalSeenN = 0;
        b.tickClear = CLEAR_CAP;
        b.collisionDepth = 0;
        b.lastTouchedRing = nullptr;

        bool second = dual && b.ghost2 && p2;
        copyPlayerState(b.ghost1, p1);
        if (second) copyPlayerState(b.ghost2, p2);

        float dt = (b.tickLength > 0.001f && b.tickLength < 2.0f) ? b.tickLength : TICK;

        if (sp.rec) {
            sp.rec->startX[0] = b.ghost1->getPositionX();
            sp.rec->startY[0] = b.ghost1->getPositionY();
            if (second) {
                sp.rec->startX[1] = b.ghost2->getPositionX();
                sp.rec->startY[1] = b.ghost2->getPositionY();
            }
            sp.rec->stepDt = dt;
            sp.rec->accumDt = 0.0f;
            sp.rec->spiderActive = b.ghost1->m_isSpider || (second && b.ghost2->m_isSpider);
        }

        auto pressBoth = [&](bool down) {
            clearGhostCollisionLogs(b.ghost1);
            if (second) clearGhostCollisionLogs(b.ghost2);
            pressGhost(b.ghost1, down);
            if (second) pressGhost(b.ghost2, down);
        };

        auto tapBoth = [&]() {
            clearGhostCollisionLogs(b.ghost1);
            if (second) clearGhostCollisionLogs(b.ghost2);
            pressGhost(b.ghost1, false);
            if (second) pressGhost(b.ghost2, false);
            pressGhost(b.ghost1, true);
            if (second) pressGhost(b.ghost2, true);
        };

        int total = std::min(sp.delay + sp.horizon, SIM_STEP_CAP);
        int pilotGate = sp.delay + std::max(4, sp.pilotStartOffset);
        int survived = 0, tick = 0, recorded = 0, preIdx = 0;
        int nextToggle = -1, releaseTapTick = -1, lastPilotActionTick = -99;
        int assumed = TRAJ_MAX;
        bool died = false;
        bool state = b.ghost1->m_holdingButtons[1];
        bool surfaceFlipMode = false, upBefore1 = false, upBefore2 = false;
        bool onGroundAtDelay = false, actionTriggered = false;
        int ringsBeforeDelay = 0;
        float minClear = CLEAR_CAP, nearClear = CLEAR_CAP, lastCrossVel = 0.0f;

        for (; tick < total; ++tick) {
            if (!sp.rec && (tick & 15) == 15 && std::chrono::steady_clock::now() >= sp.deadline) {
                b.simUnverified = true;
                break;
            }

            float prevX = b.ghost1->getPositionX();
            float prevY = b.ghost1->getPositionY();
            b.padOrPortalFlippedTick = false;

            while (preIdx < sp.preN && sp.pre[preIdx].ticksRemaining <= tick) {
                pressBoth(sp.pre[preIdx].down);
                state = sp.pre[preIdx].down;
                ++preIdx;
            }

            if (tick == sp.delay) {
                upBefore1 = b.ghost1->m_isUpsideDown;
                upBefore2 = second ? b.ghost2->m_isUpsideDown : false;
                onGroundAtDelay = b.ghost1->m_isOnGround || (second && b.ghost2->m_isOnGround);
                ringsBeforeDelay = b.consumedRingsCount;
                surfaceFlipMode = sp.down && (b.ghost1->m_isBall || b.ghost1->m_isSpider);

                pressBoth(sp.down);
                state = sp.down;

                if (b.ghostDied) {
                    died = true;
                    break;
                }

                if (sp.policy == SimPolicy::Rhythm) {
                    if (b.ghost1->m_isSwing || b.ghost1->m_isBird) {
                        int period = (sp.rHold > 0) ? std::clamp(sp.rHold + sp.rGap, 14, 34) : 20;
                        nextToggle = tick + period;
                        if (sp.down && sp.releaseAfter > 0) {
                            releaseTapTick = tick + std::min(sp.releaseAfter, period - 2);
                        }
                        assumed = std::min(assumed, nextToggle);
                    } else if (sp.rHold > 0) {
                        nextToggle = tick + (sp.down ? sp.rHold : sp.rGap);
                        assumed = std::min(assumed, nextToggle);
                    } else if (sp.releaseAfter > 0) {
                        nextToggle = tick + sp.releaseAfter;
                        assumed = std::min(assumed, nextToggle);
                    }
                } else if (sp.policy == SimPolicy::SmartPilot) {
                    if (sp.down && b.ghost1->m_isRobot) {
                        nextToggle = tick + 4;
                        assumed = std::min(assumed, nextToggle);
                    }
                }
            }

            if (tick > sp.delay) {
                if (releaseTapTick >= 0 && tick == releaseTapTick) {
                    if (!b.ghost1->m_isDashing) {
                        state = false;
                        pressBoth(false);
                    }
                    releaseTapTick = -1;
                }

                if (sp.policy == SimPolicy::Rhythm && nextToggle >= 0 && tick == nextToggle) {
                    assumed = std::min(assumed, tick);
                    if (b.ghost1->m_isSwing || b.ghost1->m_isBird) {
                        tapBoth();
                        state = true;
                        int period = (sp.rHold > 0) ? std::clamp(sp.rHold + sp.rGap, 14, 34) : 20;
                        nextToggle = tick + period;
                        releaseTapTick = tick + 4;
                    } else if (sp.rHold > 0) {
                        state = !state;
                        pressBoth(state);
                        nextToggle = tick + (state ? sp.rHold : sp.rGap);
                    } else {
                        if (!b.ghost1->m_isDashing) {
                            state = false;
                            pressBoth(false);
                        }
                        nextToggle = -1;
                    }
                } else if (sp.policy == SimPolicy::SmartPilot) {
                    if (nextToggle >= 0 && tick == nextToggle) {
                        assumed = std::min(assumed, tick);
                        if (!b.ghost1->m_isDashing) {
                            state = false;
                            pressBoth(false);
                        }
                        nextToggle = -1;
                    }

                    int orbGate = std::max(sp.delay + 5, pilotGate - 2);
                    if (tick >= orbGate && !b.ghost1->m_isDashing && (tick - lastPilotActionTick) >= 4) {
                        auto* ring = b.lastTouchedRing;
                        if (ring && !b.consumedP1.contains(ring) && !ring->m_isActivated) {
                            assumed = std::min(assumed, tick);
                            tapBoth();
                            state = true;
                            lastPilotActionTick = tick;
                        }
                    }

                    if (b.ghost1->m_isShip && tick >= pilotGate && !b.ghost1->m_isDashing) {
                        bool upside = b.ghost1->m_isUpsideDown;
                        float signedVelInThrustDir = upside ? -lastCrossVel : lastCrossVel;
                        float deadband = (b.ghost1->m_vehicleSize < 1.0f) ? 0.35f : 0.28f;

                        bool wantHold = state;
                        if (signedVelInThrustDir > deadband) wantHold = false;
                        else if (signedVelInThrustDir < -deadband) wantHold = true;

                        if (wantHold != state) {
                            assumed = std::min(assumed, tick);
                            state = wantHold;
                            pressBoth(state);
                            lastPilotActionTick = tick;
                        }
                    }

                    if (b.ghost1->m_isSwing && tick >= pilotGate && (tick - lastPilotActionTick) >= 9 && !b.ghost1->m_isDashing) {
                        bool upside = b.ghost1->m_isUpsideDown;
                        float velInGravDir = upside ? lastCrossVel : -lastCrossVel;
                        if (velInGravDir > 2.2f || (velInGravDir > 0.25f && b.tickClear < 14.0f)) {
                            assumed = std::min(assumed, tick);
                            tapBoth();
                            state = true;
                            lastPilotActionTick = tick;
                        }
                    }

                    if (b.ghost1->m_isBird && tick >= pilotGate && (tick - lastPilotActionTick) >= 9 && !b.ghost1->m_isDashing) {
                        bool upside = b.ghost1->m_isUpsideDown;
                        float velInGravDir = upside ? lastCrossVel : -lastCrossVel;
                        if (velInGravDir > 2.4f || (velInGravDir > 0.4f && b.tickClear < 14.0f)) {
                            assumed = std::min(assumed, tick);
                            tapBoth();
                            state = true;
                            lastPilotActionTick = tick;
                        }
                    }

                    if ((b.ghost1->m_isBall || b.ghost1->m_isSpider || (!b.ghost1->m_isDart && !b.ghost1->m_isShip && !b.ghost1->m_isBird && !b.ghost1->m_isSwing)) &&
                        tick >= pilotGate && (tick - lastPilotActionTick) >= 7 &&
                        b.ghost1->m_isOnGround && !b.ghost1->m_isDashing) {
                        float triggerDist = b.ghost1->m_isSpider ? 12.0f : 18.0f;
                        if (b.tickClear < triggerDist) {
                            assumed = std::min(assumed, tick);
                            tapBoth();
                            state = true;
                            if (b.ghost1->m_isRobot) nextToggle = tick + 6;
                            lastPilotActionTick = tick;
                        }
                    }

                    if (b.ghost1->m_isDart && tick >= pilotGate && (tick - lastPilotActionTick) >= 5 &&
                        !b.ghost1->m_isDashing && b.tickClear < 8.5f) {
                        assumed = std::min(assumed, tick);
                        state = !state;
                        pressBoth(state);
                        lastPilotActionTick = tick;
                    }
                }
            }

            b.ghost1->update(dt);
            if (second) b.ghost2->update(dt);

            if (b.ghostDied) {
                died = true;
                break;
            }

            b.tickClear = CLEAR_CAP;
            b.lastTouchedRing = nullptr;
            b.measureClearance = (tick >= sp.delay) &&
                                 (sp.policy == SimPolicy::SmartPilot || survived < 36 || ((survived & 1) == 0));

            clearGhostCollisionLogs(b.ghost1);
            layer->checkCollisions(b.ghost1, dt, false);

            if (!b.ghostDied && second) {
                clearGhostCollisionLogs(b.ghost2);
                layer->checkCollisions(b.ghost2, dt, false);
                if (b.ghostDied) res.p2Constrained = true;
            }

            if (b.ghostDied) {
                died = true;
                break;
            }

            b.ghost1->updateRotation(dt);
            if (second) b.ghost2->updateRotation(dt);

            float curX = b.ghost1->getPositionX();
            float curY = b.ghost1->getPositionY();
            float dxStep = curX - prevX;
            float dyStep = curY - prevY;
            lastCrossVel = isPlayerSideways(b.ghost1) ? dxStep : dyStep;
            float stepJump = std::max(std::fabs(dxStep), std::fabs(dyStep));

            if (sp.rec && tick < TRAJ_MAX) {
                sp.rec->x[0][tick] = curX;
                sp.rec->y[0][tick] = curY;
                if (second) {
                    sp.rec->x[1][tick] = b.ghost2->getPositionX();
                    sp.rec->y[1][tick] = b.ghost2->getPositionY();
                }
                if (stepJump > 25.0f || b.ghost1->m_isSpider || (second && b.ghost2->m_isSpider)) {
                    sp.rec->spiderActive = true;
                }
                recorded = tick + 1;
            }

            if (b.simUnverified) {
                if (b.simHitPortalTransition && tick >= sp.delay + 2 && !b.ghostDied) {
                    assumed = std::min(assumed, tick);
                    survived = sp.horizon;
                    b.simUnverified = false;
                }
                break;
            }

            if (tick < sp.delay) continue;

            if (b.measureClearance) {
                minClear = std::min(minClear, b.tickClear);
                if (survived < 20) nearClear = std::min(nearClear, b.tickClear);
            }
            ++survived;

            if (surfaceFlipMode && !actionTriggered && (tick - sp.delay) <= 1) {
                bool onGroundNow = b.ghost1->m_isOnGround || (second && b.ghost2->m_isOnGround);
                bool usedOrb = (b.consumedRingsCount > ringsBeforeDelay);
                bool flippedGravity = !b.padOrPortalFlippedTick &&
                    (b.ghost1->m_isUpsideDown != upBefore1 || (second && b.ghost2->m_isUpsideDown != upBefore2));
                if (onGroundAtDelay || onGroundNow || flippedGravity || usedOrb || b.ghost1->m_isDashing || stepJump > 15.0f) {
                    actionTriggered = true;
                }
            }

            if (survived >= sp.stopAt) break;
        }

        if ((died || b.simUnverified) && tick < sp.delay) {
            *end = SimEnd::StopSearch;
        }

        if (sp.rec) {
            sp.rec->count = std::min({ assumed, recorded, TRAJ_MAX });
            sp.rec->dual = second;
            sp.rec->steps = 0;
            sp.rec->active = false;
            sp.rec->replanned = false;
        }

        restoreSnapshots(pIdx);
        b.lastTouchedRing = nullptr;
        b.measureClearance = true;
        b.simulating = false;

        res.survived = survived;
        res.safe = res.survived >= sp.stopAt;
        res.clear = minClear;
        res.nearClear = (nearClear < CLEAR_CAP) ? nearClear : minClear;
        res.exitVel = std::fabs(lastCrossVel);
        res.effective = !surfaceFlipMode || actionTriggered;
        res.unverified = b.simUnverified;
        return res;
    }

    SimResult evaluateCandidate(int pIdx, PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual,
                                SimParams const& baseParams, int k, bool isBaseline,
                                Trajectory* recOut, SimEnd* end) {
        int mode = getPlayerMode(p1);
        bool steer = (mode == 1 || mode == 2);
        bool robot = (mode == 6);

        int requiredSafeTicks = baseParams.horizon;
        int commitWindow = getPilotCommitWindow(p1, mode, isBaseline);

        bool useRhythmPrimary = (steer && baseParams.rHold > 0) || (robot && baseParams.releaseAfter > 0);
        SimPolicy primaryPolicy = useRhythmPrimary ? SimPolicy::Rhythm : SimPolicy::Sustained;

        SimParams pPrimary = baseParams;
        pPrimary.delay = k;
        pPrimary.policy = primaryPolicy;
        pPrimary.stopAt = requiredSafeTicks;
        pPrimary.rec = recOut;

        SimResult best = runSimulation(pIdx, layer, p1, p2, dual, pPrimary, end);
        if (*end == SimEnd::StopSearch || best.unverified) return best;

        if (best.safe && best.effective && (isBaseline || best.nearClear >= 4.5f)) {
            return best;
        }

        int minDivergenceTicks = 3;
        if (!useRhythmPrimary) {
            if (baseParams.rHold > 0) minDivergenceTicks = std::min(minDivergenceTicks, baseParams.rHold);
            if (baseParams.releaseAfter > 0) minDivergenceTicks = std::min(minDivergenceTicks, baseParams.releaseAfter);
        }
        if (best.survived < minDivergenceTicks && !best.safe) return best;

        auto isBetter = [](SimResult const& cand, SimResult const& cur) {
            if (!cand.effective || cand.unverified) return false;
            if (!cur.effective) return true;
            if (cand.safe != cur.safe) return cand.safe;
            if (cand.survived != cur.survived) return cand.survived > cur.survived;
            float candScore = cand.nearClear * 1.35f + cand.clear * 0.65f - cand.exitVel * 0.2f;
            float curScore  = cur.nearClear * 1.35f + cur.clear * 0.65f - cur.exitVel * 0.2f;
            return candScore > curScore;
        };

        SimPolicy secondaryPolicy = useRhythmPrimary ? SimPolicy::Sustained : SimPolicy::Rhythm;
        bool allowSecondaryOnBase = !isBaseline || steer || (baseParams.down && !robot && baseParams.releaseAfter > 0);
        if (allowSecondaryOnBase &&
            (secondaryPolicy == SimPolicy::Sustained || baseParams.releaseAfter > 0 || baseParams.rHold > 0) &&
            std::chrono::steady_clock::now() < baseParams.deadline) {
            SimParams pSec = baseParams;
            pSec.delay = k;
            if (isBaseline && (mode == 3 || mode == 4)) {
                pSec.rHold = 0;
                pSec.rGap = 0;
            }
            pSec.policy = secondaryPolicy;
            pSec.stopAt = requiredSafeTicks;
            pSec.rec = nullptr;

            Trajectory altRec;
            if (recOut) pSec.rec = &altRec;

            SimEnd end2 = SimEnd::Ok;
            SimResult r2 = runSimulation(pIdx, layer, p1, p2, dual, pSec, &end2);
            if (isBetter(r2, best)) {
                best = r2;
                if (recOut) *recOut = altRec;
            }
            if (isBaseline && best.safe && best.effective) return best;
        }

        if (isBaseline && (!best.effective || best.survived < commitWindow || best.nearClear < 3.5f)) {
            return best;
        }

        if ((!best.safe || !best.effective) && best.survived >= 4 &&
            std::chrono::steady_clock::now() < baseParams.deadline) {
            SimParams pPilot = baseParams;
            pPilot.delay = k;
            pPilot.policy = SimPolicy::SmartPilot;
            pPilot.pilotStartOffset = commitWindow;
            pPilot.stopAt = baseParams.horizon;
            pPilot.rec = nullptr;

            Trajectory altRec;
            if (recOut) pPilot.rec = &altRec;

            SimEnd end3 = SimEnd::Ok;
            SimResult r3 = runSimulation(pIdx, layer, p1, p2, dual, pPilot, &end3);
            if (isBetter(r3, best)) {
                best = r3;
                if (recOut) *recOut = altRec;
            }
        }

        return best;
    }

    struct SearchResult {
        int bestDelay = -1;
        int first = 0;
        int last = 0;
        int survived = 0;
        float clear = CLEAR_CAP;
        bool truncated = false;
        bool p2Constrained = false;
    };

    int evaluateOptimalDelay(int first, int last, bool truncated, bool shipOrSwing,
                             float const* clearScores, float const* nearClearScores, float const* exitVelScores,
                             float* outScore = nullptr) {
        int span = last - first;
        int lo = (span >= 2 && nearClearScores[first] < 3.5f) ? first + WINDOW_PAD : first;
        int hi = (span >= 2 && !truncated && nearClearScores[last] < 3.5f) ? last - WINDOW_PAD : last;
        if (hi < lo) { lo = first; hi = last; }

        int best = lo;
        float bestScore = -99999.0f;

        for (int k = lo; k <= hi; ++k) {
            float localClear = clearScores[k];
            float localNear = nearClearScores[k];
            for (int j = std::max(first, k - WINDOW_PAD); j <= std::min(last, k + WINDOW_PAD); ++j) {
                localClear = std::min(localClear, clearScores[j]);
                localNear  = std::min(localNear, nearClearScores[j]);
            }

            float safetyBonus = std::min(localNear, 12.0f) * 1.5f + std::min(localClear, 20.0f) * 0.40f;
            float delayPenalty = static_cast<float>(k - lo) * ((localNear >= 6.0f) ? 1.50f : 0.35f);
            float velPenalty = shipOrSwing ? (exitVelScores[k] * 0.25f) : 0.0f;
            float score = safetyBonus - delayPenalty - velPenalty;

            if (score > bestScore) {
                bestScore = score;
                best = k;
            }
        }

        if (outScore) *outScore = bestScore + (span * 0.35f);
        return best;
    }

    SearchResult executeSearch(int pIdx, PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual,
                               SimParams const& base, int searchStart, int skipDelay, bool steer, bool shipOrSwing,
                               int baseSurvived, bool baseEffective, int maxDelay) {
        SearchResult out;
        int horizon = base.horizon;
        int minGain = steer ? 5 : 6;

        int survScores[DELAY_CAP + 2] = {};
        float clearScores[DELAY_CAP + 2];
        float nearClearScores[DELAY_CAP + 2];
        float exitVelScores[DELAY_CAP + 2] = {};
        bool validDelay[DELAY_CAP + 2] = {};
        bool p2Flags[DELAY_CAP + 2] = {};
        std::fill(std::begin(clearScores), std::end(clearScores), CLEAR_CAP);
        std::fill(std::begin(nearClearScores), std::end(nearClearScores), CLEAR_CAP);

        int maxSurvived = baseEffective ? baseSurvived : 0;
        int lastTested = searchStart - 1;

        for (int k = searchStart; k <= maxDelay; ++k) {
            if (k == skipDelay) {
                lastTested = k;
                continue;
            }
            if (std::chrono::steady_clock::now() >= base.deadline) {
                out.truncated = true;
                break;
            }

            SimEnd end = SimEnd::Ok;
            SimResult r = evaluateCandidate(pIdx, layer, p1, p2, dual, base, k, false, nullptr, &end);
            if (end == SimEnd::StopSearch || r.unverified) {
                if (r.unverified && std::chrono::steady_clock::now() >= base.deadline) {
                    out.truncated = true;
                }
                break;
            }

            lastTested = k;
            if (r.effective) {
                survScores[k] = r.survived;
                clearScores[k] = r.clear;
                nearClearScores[k] = r.nearClear;
                exitVelScores[k] = r.exitVel;
                validDelay[k] = true;
                p2Flags[k] = r.p2Constrained;
                maxSurvived = std::max(maxSurvived, r.survived);
            }
        }

        int minEffectiveFloor = baseEffective ? (baseSurvived + minGain) : std::max(24, baseSurvived + minGain);
        bool qualifies = (maxSurvived >= horizon && (!baseEffective || baseSurvived < horizon)) ||
                         (maxSurvived >= minEffectiveFloor);
        if (!qualifies) return out;

        int winThreshold = (maxSurvived >= horizon) ? horizon : std::max(minEffectiveFloor, maxSurvived - 2);
        float bestWinScore = -99999.0f;

        int segFirst = -1, segLast = -1, segSurv = 0;
        bool segP2 = false;

        auto commitSegment = [&]() {
            if (segFirst < 0) return;
            bool segTrunc = (segLast >= maxDelay) || (out.truncated && segLast == lastTested);
            float segScore = -99999.0f;
            int candK = evaluateOptimalDelay(segFirst, segLast, segTrunc, shipOrSwing,
                                             clearScores, nearClearScores, exitVelScores, &segScore);
            if (segScore > bestWinScore) {
                bestWinScore = segScore;
                out.first = segFirst;
                out.last = segLast;
                out.bestDelay = candK;
                out.clear = clearScores[candK];
                out.survived = std::max(segSurv, survScores[candK]);
                out.p2Constrained = segP2;
                out.truncated = segTrunc;
            }
            segFirst = -1;
            segLast = -1;
            segSurv = 0;
            segP2 = false;
        };

        for (int k = searchStart; k <= lastTested; ++k) {
            if (validDelay[k] && survScores[k] >= winThreshold) {
                if (segFirst < 0) {
                    segFirst = segLast = k;
                    segSurv = survScores[k];
                    segP2 = p2Flags[k];
                } else {
                    segLast = k;
                    segSurv = std::max(segSurv, survScores[k]);
                    segP2 = segP2 || p2Flags[k];
                }
            } else {
                commitSegment();
            }
        }
        commitSegment();

        return out;
    }

    void flushChannelQueue(PlayLayer* layer, int pIdx) {
        auto& b = g_buf[pIdx];
        while (!b.queue.empty()) {
            bool down = b.queue.front().down;
            b.queue.pop_front();
            if (down != b.appliedInput) sendPlayerInput(layer, pIdx, down);
        }
        resyncChannel(layer, pIdx);
    }

    void configureRhythmParams(SimParams& sp, PlayLayer* layer, PlayerObject* p1, int pIdx, int mode, bool down) {
        auto& rhythm = g_rhythm[pIdx];
        int holdAdapted = std::clamp(rhythm.holds[mode].adaptive(10), 3, 24);
        int gapAdapted  = std::clamp(rhythm.gaps[mode].adaptive(12), 4, 24);
        bool wave = (mode == 1);
        bool ship = (mode == 2);

        if (ship) {
            int rH = std::clamp(holdAdapted, 4, 14);
            int rG = std::clamp(gapAdapted, 4, 16);
            if (p1 && p1->m_isUpsideDown && !isUsing22ShipPhysics(layer, p1)) {
                rH = std::max(3, static_cast<int>(rH * 0.80f + 0.5f));
                rG = std::max(3, static_cast<int>(rG * 0.80f + 0.5f));
            }
            sp.rHold = rH;
            sp.rGap = rG;
            sp.releaseAfter = -1;
        } else if (wave) {
            if (rhythm.holds[mode].n >= 2 && rhythm.gaps[mode].n >= 2) {
                sp.rHold = std::clamp(holdAdapted, 3, 18);
                sp.rGap  = std::clamp(gapAdapted, 3, 18);
            } else {
                sp.rHold = 0;
                sp.rGap  = 0;
            }
            sp.releaseAfter = -1;
        } else if ((mode == 3 || mode == 4) && down) {
            sp.rHold = std::clamp(holdAdapted, 4, 14);
            sp.rGap  = std::clamp(gapAdapted, 8, 22);
            sp.releaseAfter = holdAdapted;
        } else if (down) {
            sp.releaseAfter = holdAdapted;
        }
    }

    bool reevaluateQueue(PlayLayer* layer, int pIdx, PlayerObject* player, bool allowImmediateDelivery, bool recordNewTraj) {
        auto& b = g_buf[pIdx];
        if (b.queue.empty() || b.simulating || b.unverifiedTicks > 0) return false;

        bool dual = isSymmetricDualActive(layer);
        auto* p1 = player;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        if (!p1 || p1->m_isDead || (p2 && p2->m_isDead) || isPlayerInputLocked(p1)) {
            b.resetQueue();
            return false;
        }

        auto& next = b.queue.front();
        int mode = getPlayerMode(p1);
        bool steer = (mode == 1 || mode == 2);
        bool shipOrSwing = (mode == 2 || mode == 4);
        int horizon = getModeHorizon(layer, p1, mode);

        int maxDelay = std::min(stageTicks(), DELAY_CAP);
        if (steer) maxDelay = std::min(maxDelay, FLYING_MAX_DELAY);

        int minDelay = allowImmediateDelivery ? 0 : 1;
        int maxAllowedFromNow = std::max(minDelay, maxDelay - next.elapsedTicks);
        int curPlanned = std::clamp(next.ticksRemaining, minDelay, maxAllowedFromNow);

        if (maxAllowedFromNow <= minDelay) {
            next.ticksRemaining = minDelay;
            for (size_t i = 1; i < b.queue.size(); ++i) {
                b.queue[i].ticksRemaining = std::max(b.queue[i].ticksRemaining, b.queue[i - 1].ticksRemaining + MIN_EVENT_GAP);
            }
            b.traj.active = false;
            return true;
        }

        SimParams sp;
        sp.down = next.down;
        sp.horizon = horizon;
        sp.stopAt = horizon;
        sp.delay = curPlanned;
        sp.deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<int>(BUDGET_MS * 1000.0));

        for (size_t i = 1; i < b.queue.size() && sp.preN < MAX_PRE; ++i) {
            sp.pre[sp.preN++] = b.queue[i];
        }

        configureRhythmParams(sp, layer, p1, pIdx, mode, next.down);

        Trajectory rec;
        SimEnd end = SimEnd::Ok;
        SimResult base = evaluateCandidate(pIdx, layer, p1, p2, dual, sp, curPlanned, false,
                                           recordNewTraj ? &rec : nullptr, &end);
        if (base.unverified) return false;
        if (base.safe && base.effective) {
            next.ticksRemaining = curPlanned;
            for (size_t i = 1; i < b.queue.size(); ++i) {
                b.queue[i].ticksRemaining = std::max(b.queue[i].ticksRemaining, b.queue[i - 1].ticksRemaining + MIN_EVENT_GAP);
            }
            if (recordNewTraj && curPlanned > 0 && rec.count >= 2) {
                b.traj = rec;
                b.traj.steps = 1;
                b.traj.active = true;
                b.traj.replanned = true;
            } else {
                b.traj.active = false;
            }
            return true;
        }

        SearchResult found = executeSearch(pIdx, layer, p1, p2, dual, sp, minDelay, curPlanned, steer, shipOrSwing,
                                           base.survived, base.effective, maxAllowedFromNow);
        if (found.bestDelay >= minDelay) {
            int chosen = std::min(found.bestDelay, maxAllowedFromNow);
            next.ticksRemaining = chosen;
            for (size_t i = 1; i < b.queue.size(); ++i) {
                b.queue[i].ticksRemaining = std::max(b.queue[i].ticksRemaining, b.queue[i - 1].ticksRemaining + MIN_EVENT_GAP);
            }
            if (recordNewTraj && chosen > 0) {
                sp.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
                SimResult confirmed = evaluateCandidate(pIdx, layer, p1, p2, dual, sp, chosen, false, &rec, &end);
                if (!confirmed.unverified && confirmed.effective && rec.count >= 2) {
                    b.traj = rec;
                    b.traj.steps = 1;
                    b.traj.active = true;
                    b.traj.replanned = true;
                    return true;
                }
            }
            b.traj.active = false;
            return true;
        }

        b.traj.active = false;
        return false;
    }

    void performRealityCheck(PlayLayer* layer, int pIdx, float dt) {
        auto& b = g_buf[pIdx];
        auto& tr = b.traj;
        if (!tr.active) return;

        if (b.queue.empty()) {
            tr.active = false;
            return;
        }

        int stepIdx = tr.steps++;
        if (stepIdx == 0) return;

        tr.accumDt += (dt > 0.001f && dt < 2.0f) ? dt : b.tickLength;
        float stepDt = (tr.stepDt > 0.001f) ? tr.stepDt : TICK;
        float fIdx = (tr.accumDt / stepDt) - 1.0f;
        int idx = static_cast<int>(std::floor(fIdx + 0.01f));
        if (idx < 0) idx = 0;
        if (idx >= tr.count) {
            tr.active = false;
            return;
        }

        auto* pReal = (pIdx == 0) ? layer->m_player1 : layer->m_player2;
        if (!pReal) return;

        auto getExpectedPos = [&](int playerSlot, float& outX, float& outY) {
            float t = std::clamp(fIdx - static_cast<float>(idx), 0.0f, 1.0f);
            if (fIdx < 0.0f) {
                float u = std::clamp(fIdx + 1.0f, 0.0f, 1.0f);
                outX = tr.startX[playerSlot] + u * (tr.x[playerSlot][0] - tr.startX[playerSlot]);
                outY = tr.startY[playerSlot] + u * (tr.y[playerSlot][0] - tr.startY[playerSlot]);
            } else if (idx + 1 < tr.count && t > 0.01f) {
                outX = tr.x[playerSlot][idx] + t * (tr.x[playerSlot][idx + 1] - tr.x[playerSlot][idx]);
                outY = tr.y[playerSlot][idx] + t * (tr.y[playerSlot][idx + 1] - tr.y[playerSlot][idx]);
            } else {
                outX = tr.x[playerSlot][idx];
                outY = tr.y[playerSlot][idx];
            }
        };

        float expX = 0.0f, expY = 0.0f;
        getExpectedPos(0, expX, expY);

        float dx = std::fabs(pReal->getPositionX() - expX);
        float dy = std::fabs(pReal->getPositionY() - expY);

        bool sideways = isPlayerSideways(pReal);
        float gravEps = tr.spiderActive ? REALITY_EPS_SPIDER_Y : REALITY_EPS_Y;
        float maxEpsX = sideways ? gravEps : REALITY_EPS_X;
        float maxEpsY = sideways ? REALITY_EPS_X : gravEps;

        bool desync = (dy > maxEpsY) || (dx > maxEpsX) || wasPlayerTeleported(pReal);
        if (!desync && tr.dual && layer->m_player2 && pIdx == 0) {
            float expX2 = 0.0f, expY2 = 0.0f;
            getExpectedPos(1, expX2, expY2);
            float dx2 = std::fabs(layer->m_player2->getPositionX() - expX2);
            float dy2 = std::fabs(layer->m_player2->getPositionY() - expY2);
            if ((dy2 > maxEpsY) || (dx2 > maxEpsX) || wasPlayerTeleported(layer->m_player2)) {
                desync = true;
            }
        }

        if (desync) {
            bool alreadyReplanned = tr.replanned;
            tr.active = false;

            if (!alreadyReplanned && !pReal->m_isDead &&
                reevaluateQueue(layer, pIdx, pReal, true, true)) {
                return;
            }

            b.unverifiedTicks = 2;
            say("{}[REALITY] Trigger/Physics shift on P{} (dx={:.1f}u, dy={:.1f}u) - syncing queue{}",
                col(C_YELLOW), pIdx + 1, dx, dy, col(C_RESET));
            flushChannelQueue(layer, pIdx);
        }
    }

    void processPendingDelivery(PlayLayer* layer, int pIdx, PlayerObject* player) {
        auto& b = g_buf[pIdx];
        if (b.sinceInput <= 998) ++b.sinceInput;

        if (isPlayerInputLocked(player)) {
            b.resetQueue();
            b.traj.active = false;
            return;
        }

        if (b.queue.empty()) {
            b.traj.active = false;
            resyncChannel(layer, pIdx);
            return;
        }

        bool delivered = false;
        while (!b.queue.empty() && b.queue.front().ticksRemaining <= 0) {
            bool down = b.queue.front().down;
            b.queue.pop_front();
            if (down != b.appliedInput) {
                sendPlayerInput(layer, pIdx, down);
                delivered = true;
            }
        }

        if (b.queue.empty()) {
            b.traj.active = false;
            return;
        }

        if (delivered) reevaluateQueue(layer, pIdx, player, false, true);

        for (auto& q : b.queue) {
            --q.ticksRemaining;
            ++q.elapsedTicks;
        }
    }

    bool interceptInput(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer || layer != static_cast<GJBaseGameLayer*>(self)) return false;

        int pIdx = resolveChannelIndex(layer, isPlayer1);
        auto& b = g_buf[pIdx];
        int curTick = g_tickCount.load(std::memory_order_relaxed);

        if (!isTwoPlayerDualActive(layer) && b.lastInterceptTick == curTick &&
            b.lastInterceptDown == down && b.lastInterceptIsP1 != isPlayer1) {
            return b.lastInterceptBuffered;
        }
        b.lastInterceptTick = curTick;
        b.lastInterceptDown = down;
        b.lastInterceptIsP1 = isPlayer1;

        auto finishIntercept = [&](bool buffered) {
            b.lastInterceptBuffered = buffered;
            return buffered;
        };

        b.realInput = down;
        b.traj.active = false;

        if (!stageEnabled() || isPlatformer(layer)) {
            b.appliedInput = down;
            return finishIntercept(false);
        }

        bool dual = isSymmetricDualActive(layer);
        auto* p1 = (pIdx == 0) ? self->m_player1 : layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;

        if (!p1 || p1->m_isDead || (p2 && p2->m_isDead) || isPlayerInputLocked(p1)) {
            b.resetQueue();
            b.appliedInput = down;
            return finishIntercept(false);
        }

        int since = b.sinceInput;
        b.sinceInput = 0;

        int mode = getPlayerMode(p1);
        bool wave = (mode == 1);
        bool ship = (mode == 2);
        bool swing = (mode == 4);
        bool robot = (mode == 6);
        bool steer = wave || ship;
        bool shipOrSwing = ship || swing;

        int horizon = getModeHorizon(layer, p1, mode);
        int maxDelay = std::min(stageTicks(), DELAY_CAP);
        if (steer) {
            maxDelay = std::min(maxDelay, FLYING_MAX_DELAY);
        }

        int minFloor = b.queue.empty() ? 0 : b.queue.back().ticksRemaining + MIN_EVENT_GAP;
        if (!down && since == 0) minFloor = std::max(minFloor, 1);

        int naturalDelay = minFloor;
        if (!b.queue.empty()) {
            int userGap = std::max(MIN_EVENT_GAP, since);
            naturalDelay = std::clamp(b.queue.back().ticksRemaining + userGap, minFloor, std::max(minFloor, maxDelay));
        }

        auto passThrough = [&]() {
            if (b.queue.empty() && naturalDelay == 0) {
                b.appliedInput = down;
                return finishIntercept(false);
            }
            b.queue.push_back({ naturalDelay, 0, down });
            return finishIntercept(true);
        };

        if (static_cast<int>(b.queue.size()) >= 8 || b.unverifiedTicks > 0) {
            return passThrough();
        }

        if (!down && !steer && !robot) {
            bool hasActiveBuffer = p1->m_jumpBuffered || p1->m_stateJumpBuffered || p1->m_isDashing ||
                                   (p2 && (p2->m_jumpBuffered || p2->m_stateJumpBuffered || p2->m_isDashing));
            if (!hasActiveBuffer && b.queue.empty()) return passThrough();
        }

        if (maxDelay <= minFloor) return passThrough();

        SimParams sp;
        sp.down = down;
        sp.horizon = horizon;
        sp.stopAt = horizon;
        sp.delay = naturalDelay;
        sp.deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<int>(BUDGET_MS * 1000.0));
        for (auto const& q : b.queue) {
            if (sp.preN < MAX_PRE) sp.pre[sp.preN++] = q;
        }

        configureRhythmParams(sp, layer, p1, pIdx, mode, down);

        Trajectory rec;
        SimEnd end = SimEnd::Ok;
        SimResult base = evaluateCandidate(pIdx, layer, p1, p2, dual, sp, naturalDelay, true, &rec, &end);

        if (base.unverified || end == SimEnd::StopSearch || (base.safe && base.effective)) {
            if (!b.queue.empty() && base.safe && base.effective && rec.count >= 2) {
                b.traj = rec;
                b.traj.active = true;
            }
            return passThrough();
        }

        SearchResult found = executeSearch(pIdx, layer, p1, p2, dual, sp, minFloor, naturalDelay, steer, shipOrSwing,
                                           base.survived, base.effective, maxDelay);
        if (found.bestDelay < minFloor || found.bestDelay == naturalDelay) {
            return passThrough();
        }

        sp.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3);
        SimResult confirmed = evaluateCandidate(pIdx, layer, p1, p2, dual, sp, found.bestDelay, false, &rec, &end);
        if (confirmed.unverified || !confirmed.effective ||
            (base.effective && confirmed.survived <= base.survived)) {
            b.traj.active = false;
            return passThrough();
        }

        b.traj = rec;
        b.traj.active = rec.count >= 2;
        b.queue.push_back({ found.bestDelay, 0, down });

        g_totalSaves.fetch_add(1, std::memory_order_relaxed);
        float pct = layer->getCurrentPercent();
        say("{}SAVED{} P{} [{}] {} +{}t ({:g}f) @ {:.1f}% (window +{}..+{}t, clearance: {:.1f}u{})",
            col(C_GREEN), col(C_RESET), pIdx + 1, MODE_NAMES[mode], down ? "Press" : "Release",
            found.bestDelay, found.bestDelay * TICK, pct, found.first, found.last, confirmed.clear,
            found.p2Constrained ? " [P2 bounded]" : "");
        return finishIntercept(true);
    }
}

$on_mod(Loaded) {
    loadConfig();
    g_rhythm[0].load(0);
    g_rhythm[1].load(1);
    setupExternalConsole();
    say("======================================================================");
    say("{}[Predictive Input Buffer v9.7 - Final Production Engine Loaded]{}", col(C_CYAN), col(C_RESET));
    say("{}", stageText());
    say("Horizons: Ship/Wave = 65t | Cube/UFO/Swing/Ball/Robot/Spider = 130t");
    say("Hotkeys: F6/B = Stage | F7 = Toggle | F8 = Diagnostics | F9 = Hide Console");
    say("======================================================================");
#ifdef GEODE_IS_WINDOWS
    std::thread(hotkeyThreadLoop).detach();
#endif
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::updateTimeWarp", HOOK_PRIORITY);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (isAntiCheatObject(this, object)) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating && g_buf[p].isGhost(player)) {
                g_buf[p].ghostDied = true;
                return;
            }
        }
        PlayLayer::destroyPlayer(player, object);
        if (player && player->m_isDead) {
            for (int p = 0; p < 2; ++p) g_buf[p].resetState();
        }
    }

    void updateTimeWarp(float timeWarp) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) return;
        }
        PlayLayer::updateTimeWarp(timeWarp);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        for (int p = 0; p < 2; ++p) {
            g_buf[p].dropGhosts();
            g_buf[p].lastMode = -1;
            g_buf[p].lastShipPhysics = -1;
            g_rhythm[p].reset();
        }

        g_buf[0].ghost1 = g_buf[0].makeGhost(this);
        g_buf[0].ghost2 = g_buf[0].makeGhost(this);
        g_buf[1].ghost1 = g_buf[1].makeGhost(this);

        g_tickCount = 0;
        bool ship22 = isUsing22ShipPhysics(this, this->m_player1);
        g_curShip22.store(ship22, std::memory_order_relaxed);
        g_buf[0].lastShipPhysics = ship22 ? 1 : 0;

        say("{}Level initialized:{} '{}' | Ship Physics: {} | {}", col(C_CYAN), col(C_RESET),
            level ? level->m_levelName : "Unknown",
            ship22 ? "2.2 (Symmetric)" : "2.1 (Legacy Inverted)",
            stageText());
        return true;
    }

    void onQuit() {
        g_rhythm[0].persist(0);
        g_rhythm[1].persist(1);
        for (int p = 0; p < 2; ++p) g_buf[p].dropGhosts();
        PlayLayer::onQuit();
    }

    void onExit() {
        g_rhythm[0].persist(0);
        g_rhythm[1].persist(1);
        for (int p = 0; p < 2; ++p) g_buf[p].dropGhosts();
        PlayLayer::onExit();
    }

    void resetLevel() {
        for (int p = 0; p < 2; ++p) {
            g_rhythm[p].persist(p);
            g_buf[p].resetState();
            g_rhythm[p].reset();
        }
        g_tickCount = 0;
        PlayLayer::resetLevel();
    }

    void resetLevelFromStart() {
        for (int p = 0; p < 2; ++p) {
            g_rhythm[p].persist(p);
            g_buf[p].resetState();
            g_rhythm[p].reset();
        }
        g_tickCount = 0;
        PlayLayer::resetLevelFromStart();
    }

    void loadFromCheckpoint(CheckpointObject* checkpoint) {
        for (int p = 0; p < 2; ++p) {
            g_buf[p].resetState();
            g_rhythm[p].reset();
        }
        g_tickCount = 0;
        PlayLayer::loadFromCheckpoint(checkpoint);
    }

    void playEndAnimationToPos(CCPoint position) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) return;
        }
        for (int p = 0; p < 2; ++p) {
            g_rhythm[p].persist(p);
            g_buf[p].resetState();
        }
        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(BufEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        bool ok = LevelEditorLayer::init(level, noUI);
        for (int p = 0; p < 2; ++p) g_buf[p].dropGhosts();
        return ok;
    }
};

class $modify(BufHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) return;
        }
        HardStreak::addPoint(point);
    }
};

class $modify(BufEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) return;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(BufGameObject, GameObject) {
    void playShineEffect() {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) return;
        }
        GameObject::playShineEffect();
    }
};

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::flipGravity", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::reversePlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::rotateGameplay", HOOK_PRIORITY);
    }

    void update(float dt) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) {
                PlayerObject::update(dt);
                return;
            }
        }

        auto* layer = GameManager::get()->m_playLayer;
        if (layer && layer == this->m_gameLayer) {
            bool isP1 = (this == layer->m_player1);
            bool isP2 = (this == layer->m_player2);

            if (isP1 || isP2) {
                if (isP2 && !isTwoPlayerDualActive(layer)) {
                    PlayerObject::update(dt);
                    return;
                }

                int pIdx = isP1 ? 0 : 1;
                auto& b = g_buf[pIdx];

                if (isP1) {
                    ++g_tickCount;
                    g_lastTickMs.store(nowMs(), std::memory_order_relaxed);
                    g_curMode.store(getPlayerMode(this), std::memory_order_relaxed);

                    bool ship22 = isUsing22ShipPhysics(layer, this);
                    g_curShip22.store(ship22, std::memory_order_relaxed);
                    int sFlag = ship22 ? 1 : 0;
                    if (b.lastShipPhysics != -1 && b.lastShipPhysics != sFlag) {
                        say("{}[PHYSICS] Options Trigger switched Ship Physics to {}{}",
                            col(C_YELLOW), ship22 ? "2.2 (Symmetric)" : "2.1 (Legacy Inverted)", col(C_RESET));
                    }
                    b.lastShipPhysics = sFlag;
                }

                if (g_resetRequest.exchange(false)) {
                    for (int ch = 0; ch < 2; ++ch) {
                        g_buf[ch].resetQueue();
                        resyncChannel(layer, ch);
                    }
                }

                if (b.unverifiedTicks > 0) --b.unverifiedTicks;

                int mid = getPlayerMode(this);
                if (mid != b.lastMode) {
                    b.lastMode = mid;
                    g_rhythm[pIdx].reset();
                }

                if (dt > 0.0f) b.recordSubstepDt(dt);

                if (stageEnabled()) {
                    if (b.triggerStateChanged) {
                        b.triggerStateChanged = false;
                        if (!b.queue.empty()) reevaluateQueue(layer, pIdx, this, true, true);
                    } else {
                        performRealityCheck(layer, pIdx, dt);
                    }
                    processPendingDelivery(layer, pIdx, this);
                } else {
                    b.triggerStateChanged = false;
                }
            }
        }
        PlayerObject::update(dt);
    }

    void incrementJumps() {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) return;
        }
        PlayerObject::incrementJumps();
    }

    void flipGravity(bool flip, bool noEffects) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) {
                PlayerObject::flipGravity(flip, true);
                return;
            }
        }
        PlayerObject::flipGravity(flip, noEffects);
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        int simIdx = -1;
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) {
                if (!g_buf[p].isGhost(this)) return;
                simIdx = p;
                snapshotTarget(p, ring);
                break;
            }
        }

        if (simIdx < 0 || !ring) {
            PlayerObject::ringJump(ring, skipCheck);
            return;
        }

        bool prevTouched = static_cast<bool>(this->m_touchedRing);
        bool wasDashing = this->m_isDashing;
        bool prevUp = this->m_isUpsideDown;
        double prevYVel = this->m_yVelocity;
        float prevY = this->getPositionY();

        PlayerObject::ringJump(ring, skipCheck);

        bool nowTouched = static_cast<bool>(this->m_touchedRing);
        bool ringFired = ring->m_isActivated || ring->m_activated ||
                         ring->m_activatedByPlayer1 || ring->m_activatedByPlayer2 ||
                         (prevTouched && !nowTouched) ||
                         (!wasDashing && this->m_isDashing) ||
                         (this->m_isUpsideDown != prevUp) ||
                         (this->m_yVelocity != prevYVel) ||
                         (std::fabs(this->getPositionY() - prevY) > 2.0f);

        if (ringFired) {
            ++g_buf[simIdx].consumedRingsCount;
            if (!isMultiActivateRing(ring)) {
                auto& consumed = (this == g_buf[simIdx].ghost1) ? g_buf[simIdx].consumedP1 : g_buf[simIdx].consumedP2;
                consumed.insert(ring);
            }
        }
    }

    void reversePlayer(EffectGameObject* object) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) {
                if (!g_buf[p].isGhost(this)) return;
                if (object) snapshotTarget(p, object);
                PlayerObject::reversePlayer(object);
                return;
            }
        }
        PlayerObject::reversePlayer(object);

        auto* layer = GameManager::get()->m_playLayer;
        if (layer && layer == this->m_gameLayer && (this == layer->m_player1 || this == layer->m_player2)) {
            int pIdx = (this == layer->m_player2 && isTwoPlayerDualActive(layer)) ? 1 : 0;
            g_buf[pIdx].triggerStateChanged = true;
        }
    }

    void rotateGameplay(int moveDir, int groundDir, bool editVel, float velModX, float velModY, bool overrideVel, bool dontSlide) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) {
                if (!g_buf[p].isGhost(this)) return;
                PlayerObject::rotateGameplay(moveDir, groundDir, editVel, velModX, velModY, overrideVel, dontSlide);
                return;
            }
        }
        PlayerObject::rotateGameplay(moveDir, groundDir, editVel, velModX, velModY, overrideVel, dontSlide);

        auto* layer = GameManager::get()->m_playLayer;
        if (layer && layer == this->m_gameLayer && (this == layer->m_player1 || this == layer->m_player2)) {
            int pIdx = (this == layer->m_player2 && isTwoPlayerDualActive(layer)) ? 1 : 0;
            g_buf[pIdx].triggerStateChanged = true;
        }
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
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) {
                if (!g_buf[p].isGhost(player) || !object) return false;
                auto& consumed = (player == g_buf[p].ghost1) ? g_buf[p].consumedP1 : g_buf[p].consumedP2;
                if (consumed.contains(object)) return false;

                bool isSimulatingP1 = (p == 0 && player == g_buf[0].ghost1);
                bool alreadyUsedByReal = object->m_isActivated ||
                                         (isSimulatingP1 ? object->m_activatedByPlayer1 : object->m_activatedByPlayer2) ||
                                         checkHasBeenActivated(static_cast<GameObject*>(object), isSimulatingP1) ||
                                         checkHasBeenActivated(object, isSimulatingP1);
                if (alreadyUsedByReal && !isMultiActivateRing(object)) return false;

                snapshotTarget(p, object);

                uint64_t bit = typeBit(object->m_objectType);
                if ((bit & RING_TYPES) == 0) {
                    consumed.insert(object);
                    if (bit & (GRAV_PORTAL_TYPES | typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad))) {
                        g_buf[p].padOrPortalFlippedTick = true;
                    }
                }
                return true;
            }
        }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }

    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!objects || objectCount <= 0) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        int simIdx = -1;
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) { simIdx = p; break; }
        }

        if (simIdx < 0) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }

        auto& b = g_buf[simIdx];
        int depth = b.collisionDepth++;
        auto& filtered = b.filteredScratch[std::min(depth, SCRATCH_DEPTH - 1)];
        filtered.clear();
        if (static_cast<int>(filtered.capacity()) < objectCount) filtered.reserve(objectCount);

        constexpr uint64_t keepMask = COLLIDING_TYPES | INTERACT_TYPES;
        CCRect ghostOuterRect = player->getObjectRect();
        bool wantClear = (depth == 0) && b.measureClearance;
        CCRect ghostClearRect = (wantClear && player->m_isDart) ? player->getObjectRect(0.3f, 0.3f) : ghostOuterRect;
        float ghostX = player->getPositionX();
        float ghostY = player->getPositionY();
        bool isSimulatingP1 = (simIdx == 0 && player == b.ghost1);
        auto& list = *objects;

        bool dartSafeBlockNearby = false;
        if (wantClear && player->m_isDart) {
            for (int i = 0; i < objectCount; ++i) {
                auto* obj = list[i];
                if (obj && obj->m_objectID == 1755 && ghostOuterRect.intersectsRect(obj->getObjectRect())) {
                    dartSafeBlockNearby = true;
                    break;
                }
            }
        }

        for (int i = 0; i < objectCount; ++i) {
            auto* obj = list[i];
            if (!obj) continue;

            auto type = static_cast<unsigned>(obj->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            bool specialBlock = isSpecialGameplayBlock(obj->m_objectID);

            if ((keepMask & bit) || specialBlock) {
                if (specialBlock && ghostOuterRect.intersectsRect(obj->getObjectRect())) {
                    snapshotTarget(simIdx, obj);
                }
                filtered.push_back(obj);

                if (wantClear) {
                    bool isLethal = (HAZARD_TYPES & bit) != 0;
                    if (!isLethal && player->m_isDart && !dartSafeBlockNearby) {
                        isLethal = (type == static_cast<unsigned>(GameObjectType::Solid) ||
                                    type == static_cast<unsigned>(GameObjectType::Slope));
                    }

                    if (isLethal &&
                        std::fabs(obj->getPositionX() - ghostX) < (b.tickClear + 60.0f) &&
                        std::fabs(obj->getPositionY() - ghostY) < (b.tickClear + 60.0f)) {
                        b.tickClear = computeObjectClearanceFast(player, ghostClearRect, obj, type, b.tickClear);
                    }
                }
                continue;
            }

            if (depth > 0) continue;

            bool isPortal = (PORTAL_TYPES & bit) != 0;
            float speed = isPortal ? 0.0f : speedPortalValue(obj->m_objectID);
            if (!isPortal && speed <= 0.0f) continue;
            if (!ghostOuterRect.intersectsRect(obj->getObjectRect())) continue;

            bool alreadyUsedPortal = (obj == player->m_lastActivatedPortal) ||
                                     obj->m_isActivated ||
                                     checkHasBeenActivated(obj, isSimulatingP1);
            if (alreadyUsedPortal) continue;

            if (isPortal) {
                handleSimPortal(simIdx, player, obj);
            } else {
                player->m_playerSpeed = speed;
            }
        }

        GJBaseGameLayer::collisionCheckObjects(player, &filtered, static_cast<int>(filtered.size()), dt);
        --b.collisionDepth;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto* playLayer = GameManager::get()->m_playLayer;
        bool isActivePlayLayer = (playLayer && playLayer == static_cast<GJBaseGameLayer*>(this));
        bool anySim = g_buf[0].simulating || g_buf[1].simulating;
        bool anyInject = g_buf[0].injecting || g_buf[1].injecting;

        if (isActivePlayLayer && !anySim && button == 1 && !anyInject) {
            int pIdx = resolveChannelIndex(playLayer, isPlayer1);
            int curTick = g_tickCount.load(std::memory_order_relaxed);
            bool isDupMirror = !isTwoPlayerDualActive(playLayer) &&
                               g_buf[pIdx].lastInterceptTick == curTick &&
                               g_buf[pIdx].lastInterceptDown == down &&
                               g_buf[pIdx].lastInterceptIsP1 != isPlayer1;
            if (!isDupMirror) {
                auto* p = (pIdx == 0) ? this->m_player1 : this->m_player2;
                if (p && !p->m_isDead) {
                    g_rhythm[pIdx].record(getPlayerMode(p), down, curTick);
                }
            }
            if (interceptInput(this, down, isPlayer1)) return;
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) {
                if (!g_buf[p].isGhost(player)) return;
                snapshotTarget(p, ring);
                if (player == g_buf[p].ghost1 && ring) {
                    g_buf[p].lastTouchedRing = ring;
                }
            }
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) {
                if (g_buf[p].isGhost(player) && player->m_isUpsideDown != flip) {
                    player->flipGravity(flip, true);
                }
                return;
            }
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        for (int p = 0; p < 2; ++p) {
            if (g_buf[p].simulating) {
                if (g_buf[p].isGhost(player) && object) {
                    handleSimPortal(p, player, object);
                }
                return;
            }
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }
};

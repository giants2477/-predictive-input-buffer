// Click Before Frames v2.9 (cube-style wave, wave auto-release, orb alignment, full ghost state, drift check, in-game console on F9, 2-player mode support)
//
// When you click (or release, in wave / robot), a hidden "ghost" player is simulated
// ahead with your input. If the ghost dies, the input is retried with 1..N ticks of
// delay and, if a delayed version clearly survives longer, your input is held back
// and delivered that many physics ticks later. 1 tick = 1/240 s = 0.25 frames @ 60fps.
//
// v2 changes:
//  - Bigger windows: F6 stages up to 32 ticks (8 frames) plus a CUSTOM stage (settings).
//  - Smart prediction: ghost uses orbs, pads, gravity portals and speed portals; mode /
//    size / dual / mirror / teleport portals end the prediction as "safe".
//  - Input queue: every input arriving during a delay is shifted by the same amount,
//    so taps and hold lengths are preserved (no more swallowed clicks / 1-tick taps).
//  - Time budget per click so big windows don't hitch.
//  - Fixes: ghost lifetime, init using `this`, time-warp dt, F6 only when GD focused,
//    dead flipGravity hook priority, optional console, in-game console (stage + live log, hidden by default).

#include <Geode/Geode.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>
#include <map>
#include <Geode/modify/PlayerObject.hpp>
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <climits>
#include <cmath>
#include <complex>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr size_t PANEL_LINES = 8;
    constexpr int SPIDER_LAND = 12;  // v2.15: ticks on a surface after a spider input = safe
    constexpr int SPAM_STREAK = 3;   // this many fast inputs in a row = wave spam

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    // Objects the ghost always collides with.
    // v2.61: back to the v2.48 set (v2.49 left CollisionObject out; cube and wave got less consistent)
    constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                         typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t HAZARD_TYPES = typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::AnimatedHazard);

    // Objects the ghost may also interact with when smart prediction is on.
    constexpr uint64_t INTERACT_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
        typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
        typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb) |
        typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal);

    // Portals that change too much state to simulate: reaching one ends prediction as "safe".
    constexpr uint64_t STOP_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
        typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) | typeBit(GameObjectType::DualPortal) |
        typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

    // v2.8: size portals are applied to the ghost instead of ending the prediction.
    // (Mirror portals only flip the screen, so the ghost simply ignores them.)
    constexpr uint64_t SIZE_TYPES = typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal);

    bool hasType(uint64_t mask, GameObject* object) {
        auto type = static_cast<unsigned>(object->m_objectType);
        return type < 64 && ((mask >> type) & 1);
    }

    float speedPortalValue(int id) {
        switch (id) {
            case 200: return 0.7f;   // slow
            case 201: return 0.9f;   // normal
            case 202: return 1.1f;   // fast
            case 203: return 1.3f;   // faster
            case 1334: return 1.6f;  // fastest
            default: return 0.f;
        }
    }

    struct Stage {
        bool enabled;
        int ticksAll;
        int ticksWave;
        bool custom;
    };

    constexpr Stage STAGES[] = {
        { true, 2, 2, false },
        { true, 4, 4, false },
        { true, 8, 8, false },
        { true, 16, 8, false },
        { true, 24, 12, false },
        { true, 32, 16, false },
        { true, 0, 0, true },
        { false, 0, 0, false },
    };
    constexpr int STAGE_COUNT = sizeof(STAGES) / sizeof(STAGES[0]);

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
    };

    struct Buffer {
        PlayerObject* ghost1 = nullptr;
        PlayerObject* ghost2 = nullptr;
        CCLayer* ghostLayer = nullptr;
        unsigned ghostLayerBase = 0;  // v2.24: children the ghost layer has right after the ghosts are made

        CCNode* panel = nullptr;
        CCLabelBMFont* panelTitle = nullptr;                // current stage
        std::vector<CCLabelBMFont*> panelLines;             // one label per log line (colored)
        bool panelOpen = false;  // closed by default, remembered for the session
        bool panelDirty = false; // v2.71: new log lines, the panel is redrawn once on the next step
        bool perfMode = false;   // v2.71: performance mode
        double userBudget = 6.0; // v2.75: the budget from the settings (budgetMs is this click's)
        bool autoBudget = true;  // v2.75: budget follows the frame rate
        int keys[6] = { 0, 0, 0, 0, 0, 0 };  // v2.75: stage, menu, export, console, path, forget
        std::unordered_set<GameObject*> simDisabled;  // v2.75: objects a toggle orb switched off in this prediction
        std::deque<std::string> logLines;

        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;
        bool realInput = false;
        bool appliedInput = false;
        bool isPlayer1 = true;

        // 2-Player mode independent tracking for P2
        bool twoPlayerSupport = true;
        bool realInputP2 = false;
        bool appliedInputP2 = false;
        std::deque<QueuedInput> queueP2;
        int shiftP2 = 0;
        int releaseShiftP2 = 0;
        int sinceInputP2 = 999;
        int rapidP2 = 0;
        bool autoReleasedP2 = false;
        bool autoDashP2 = false;
        bool inRealUpdateP2 = false;
        float tickStartXP2 = 0.f;
        float tickDxP2 = 0.f;

        bool enabled = true;
        int stage = 0;
        int ticksAll = 2;
        int ticksWave = 2;

        std::deque<QueuedInput> queue;
        int shift = 0;  // delay applied to inputs while the queue is active

        float tickLength = TICK;
        int sinceInput = 999;
        int rapid = 0;

        bool smart = true;
        bool console = false;
        double budgetMs = 6.0;
        bool practiceOnly = true;     // v2.1: only buffer in practice mode
        int groundHorizon = 130;      // v2.1: how far ahead cube/ball/spider predictions look
        int releaseShift = 0;         // v2.1: delay of the last delivered press, reused for its release
        int waveHorizon = 65;         // v2.2: how far ahead wave predictions look (mini = 15 less)
        bool assumeOrbs = true;       // v2.2: the ghost clicks every orb it touches ("calculation mode")
        bool held1 = false, held2 = false;   // button state the ghosts are meant to be in
        bool auto1 = false, auto2 = false;   // an orb auto-click is currently held on this ghost
        bool orbTiming = true;        // v2.26: orb clicks at specific timings inside the orb
        int orbClickDelay = 0;        // v2.26: ghost orb auto-clicks wait this many ticks into the orb
        int autoAt1 = -1, autoAt2 = -1;      // v2.26: tick a delayed orb auto-click fires (-1 none, -2 done)
        bool orbWasTiming = false;    // v2.26: the last orb save re-timed a press that was already in the orb

        bool tryBothOrbs = true;      // v2.4: if a run touches an orb and dies, retry with the other orb choice
        bool orbClick = true;         // v2.4: orb choice of the current run
        bool touchedOrb = false;      // v2.4: the current run touched an orb
        bool skipPress = false;       // v2.4: run without pressing anything (auto-release hold check)
        bool avoidMovers = true;      // v2.4: don't buffer when the ghost's path has moving objects
        bool sawMover = false;        // v2.4: a run this decision touched a moving object
        bool trackMovers = false;     // v2.5.1: watch objects only in this run (not in every search run)
        bool autoRelease = false;     // v2.4: wave auto-release (practice mode only)
        int autoWindow = 6;           // v2.4: hold must die within this many ticks to auto-release
        bool autoReleased = false;    // v2.4: we released for you; your real release is swallowed
        int spamGap = 16;             // v2.5.2: inputs closer than this (ticks) count toward wave spam
        bool waveMargin = true;       // v2.10: center wave saves in the safe window (1 tick of room)
        bool waveV46 = false;         // v2.7: use the old v4.6 wave rules instead of the cube-style ones
        bool orbAlign = true;         // v2.6: hold an early press back until the ghost is inside the orb
        bool orbAlignGround = false;  // v2.11.1: also align presses made on the ground (jumps)
        bool ignoreTouching = true;   // v2.31: hazards you touch right now without dying can't kill the ghost
        bool haveRealRect = false;
        CCRect realRect1, realRect2;  // your hitboxes when the prediction starts
        std::unordered_set<GameObject*> harmless;  // hazards on you at the start of this prediction
        int baseEscape = -1;          // v2.36: press-now run only survives if you react by this tick
        int baseCrash = -1;           // v2.36: ...otherwise it crashes here
        bool lowLatency = false;      // v2.35: never chain delays, small budget and max delay
        bool explain = false;         // v2.33: log why an input was passed through untouched
        bool deathLog = false;        // v2.32: log what kills the press-now ghost on every no-save
        bool captureKiller = false;   // v2.30: record what kills the ghost in the press-now run
        bool baseTouchedDown = false; // v2.63: the press-now ghost was back on the ground at some point
        bool killerSeen = false;
        int killerTick = -1;
        int killerId = 0, killerType = -1;
        bool killerNoTouch = false, killerDisabled = false;
        CCRect killerRect, ghostDeathRect;
        bool robustSaves = true;      // v2.28: a save must also survive with you slightly off
        float ghostShiftX = 0.f;      // v2.28: horizontal offset applied to the ghost's start
        int lateCrash = 90;           // v2.27: cube / ball crashes after this many ticks aren't this click's (0 = off)
        bool predictMovers = false;   // v2.27: move objects along their current motion during predictions
        bool shipBuffer = false;      // v2.25: buffer ship inputs too
        int shipSpamGap = 6;          // v2.44: ship feathering is normal control, only taps closer than this are spam
        int shipMaxDelay = 2;         // v2.25: ship's own max delay (240/s ticks)
        bool perfLog = false;         // v2.21: log time / runs / ghost ticks per click
        int simRuns = 0;              // v2.21: predictions run for the current click
        long simTicks = 0;            // v2.21: ghost ticks stepped for the current click
        bool cbfSync = true;          // v2.18: line the ghost up with Click Between Frames' mid-tick clicks
        float firstStep = 1.f;        // v2.18: fraction of a tick the ghost's first step lasts
        bool inRealUpdate = false;    // v2.18: inside P1's physics step (a CBF click lands here)
        float tickStartX = 0.f;       // v2.18: P1's x at the start of the current tick
        float tickDx = 0.f;           // v2.18: how far P1 moved in the last full tick
        bool dashAutoRelease = false; // v2.16: let go of a dash for you when it would crash
        bool autoDash = false;        // v2.16: the current auto-release ended a dash
        std::unordered_set<GameObject*> orbsUsed;  // v2.16: orbs the ghost actually used this run
        bool moreTime = true;         // v2.43: at spots you keep dying, delay to give you more time
        bool ballSmart = true;        // v2.45: ball landing rules + spam guard
        bool swingSmart = true;       // v2.51: swing "could you have flipped back" scoring + spam guard
        bool train = false;           // v2.51: training mode: predict, never delay, show how early you were
        bool perLevel = true;         // v2.51: stage + switches remembered per level
        bool heatmap = false;         // disabled so nothing draws on the progress bar
        bool saveDing = false;        // disabled so saves don't make a sound
        CCLabelBMFont* hintLabel = nullptr;
        CCDrawNode* heatNode = nullptr;
        bool ufoSmart = true;         // v2.40: UFO landing + "could you have flapped again" scoring
        bool spiderSmart = true;      // v2.15: spider-aware landing + "could you have flipped back" scoring
        int retapAt = -1;             // v2.15: ticks after the input at which the ghost taps again (-1 = never)
        bool robotSmart = true;       // v2.12: robot presses count as safe if some hold length survives
        bool robotAutoRelease = false; // v2.12: robot auto-release (practice mode only)
        bool autoNormalRuns = false;  // v2.14: let auto-release work outside practice too
        CCLabelBMFont* autoTag = nullptr;
        CCLabelBMFont* watermark = nullptr;
        bool usedThisAttempt = false;
        bool learn = true;            // v2.13: per-level memory learned from your runs
        bool fullState = true;        // v2.11.1: copy the full v2.8 player state into the ghost
        int orbWindow = 16;           // v2.6: how far ahead (ticks) an orb may be for that
        int simTick = 0;              // v2.6: tick of the current run
        int firstOrbTick = -1;        // v2.6: first tick the current run touched an orb
        int orbUsedTick = -1;         // v2.6: first tick the current run actually used an orb
        bool driftCheck = false;      // v2.4: log how far the real player strays from the prediction
        std::vector<CCPoint>* recordPath = nullptr;  // v2.4: ghost1 positions of the current run
        bool showPath = false;        // v2.11: draw the predicted path (F10)
        CCDrawNode* pathNode = nullptr;  // v2.11: owned by the level's object layer
        int pathHoldUntil = 0;        // v2.65: live path waits while a click's path is on screen

        std::unordered_set<GameObject*> simUsed;
        std::vector<ObjSnapshot> snapshots;
    } buf;

    std::string stageText();

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

    // v2.4 moving-object guard: last known position of each object the ghost collided with.
    struct Watched {
        double x;
        double y;
        int movedTick;
        double vx = 0.0;  // v2.27: movement per physics step while it's moving
        double vy = 0.0;
        double pvx = 0.0; // v2.68: the step before (curved paths: how the direction turns)
        double pvy = 0.0;
        double rot = 0.0; // v2.68: rotation, and how much it turns per step
        double vrot = 0.0;
    };
    constexpr int MOVER_TICKS = 120;
    constexpr size_t WATCH_LIMIT = 4096;
    std::unordered_map<GameObject*, Watched> watched;

    // v2.4 drift check: the recorded ghost run for your last input and how far you strayed.
    struct Drift {
        std::vector<CCPoint> path;
        bool active = false;
        int index = -1;
        int compared = 0;
        float maxErr = 0.f;
        int maxAt = 0;
    } drift;

    constexpr float PANEL_W = 320.f;
    constexpr float PANEL_H = 178.f;

    ccColor3B lineColor(std::string const& line) {
        auto starts = [&](char const* prefix) { return line.rfind(prefix, 0) == 0; };
        if (starts("SAVED") || starts("MORE TIME")) return { 120, 255, 140 };
        if (starts("ORB") || starts("orb")) return { 90, 220, 255 };
        if (starts("AUTO")) return { 200, 150, 255 };
        if (starts("no save")) return { 175, 175, 190 };
        if (starts("skip") || starts("prediction budget")) return { 255, 180, 80 };
        if (starts("drift")) return { 255, 240, 150 };
        if (starts("ghost killed") || starts("  hitbox")) return { 255, 120, 120 };
        if (starts("perf")) return { 150, 200, 255 };
        if (starts("F6") || starts("v2") || starts("path") || starts("Click") || starts("CBF")) return { 255, 215, 90 };
        if (starts("pass")) return { 140, 150, 170 };
        if (starts("memory")) return { 255, 150, 220 };
        if (starts("console")) return { 150, 255, 200 };
        return { 255, 255, 255 };
    }

    void refreshPanel() {
        if (!buf.panel) {
            return;
        }
        buf.panel->setVisible(buf.panelOpen);
        if (!buf.panelOpen) {
            return;
        }
        buf.panelTitle->setString(stageText().c_str());
        buf.panelTitle->limitLabelWidth(PANEL_W - 20.f, 0.4f, 0.2f);
        size_t first = buf.logLines.size() > buf.panelLines.size() ? buf.logLines.size() - buf.panelLines.size() : 0;
        for (size_t i = 0; i < buf.panelLines.size(); ++i) {
            auto* label = buf.panelLines[i];
            size_t at = first + i;
            if (at < buf.logLines.size()) {
                auto const& line = buf.logLines[at];
                label->setString(line.c_str());
                label->setColor(lineColor(line));
                label->limitLabelWidth(PANEL_W - 20.f, 0.55f, 0.3f);
            }
            else {
                label->setString("");
            }
        }
    }

    constexpr size_t EXPORT_LINES = 400;
    std::deque<std::string> exportLines;
    int logClock = 0;  // physics steps since the level started, kept by the update hook

    template <class... Args>
    void say(fmt::format_string<Args...> f, Args&&... args) {
        auto msg = fmt::format(f, std::forward<Args>(args)...);
        exportLines.push_back(fmt::format("[{:>6}] {}", logClock, msg));
        while (exportLines.size() > EXPORT_LINES) {
            exportLines.pop_front();
        }
        buf.logLines.push_back(msg);
        while (buf.logLines.size() > PANEL_LINES) {
            buf.logLines.pop_front();
        }
        if (buf.console) {
            std::printf("[InputBuffer] %s\n", msg.c_str());
        }
        buf.panelDirty = buf.panelOpen;
    }

    bool isGhost(PlayerObject* p) {
        return p && (p == buf.ghost1 || p == buf.ghost2);
    }

    void resetPending() {
        buf.queue.clear();
        buf.shift = 0;
        buf.releaseShift = 0;
        buf.queueP2.clear();
        buf.shiftP2 = 0;
        buf.releaseShiftP2 = 0;
    }

    // v2.75: "F6", "G", "7", "None" -> key code (0 = unbound)
    int parseKey(std::string name) {
        for (auto& c : name) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (name.size() >= 2 && name[0] == 'F') {
            int n = std::atoi(name.c_str() + 1);
            if (n >= 1 && n <= 12) {
                return KEY_F1 + (n - 1);
            }
        }
        if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z') {
            return KEY_A + (name[0] - 'A');
        }
        if (name.size() == 1 && name[0] >= '0' && name[0] <= '9') {
            return KEY_Zero + (name[0] - '0');
        }
        return 0;
    }

    void readSettings() {
        auto* mod = Mod::get();
        buf.smart = mod->getSettingValue<bool>("smart-prediction");
        buf.budgetMs = mod->getSettingValue<double>("sim-budget-ms");
        buf.autoBudget = mod->getSettingValue<bool>("auto-budget");
        // v2.75 rebindable keys
        char const* keyNames[] = { "key-stage", "key-menu", "key-export", "key-console", "key-path", "key-forget" };
        for (int i = 0; i < 6; ++i) {
            buf.keys[i] = parseKey(mod->getSettingValue<std::string>(keyNames[i]));
        }
        buf.practiceOnly = mod->getSettingValue<bool>("practice-only");
        buf.groundHorizon = static_cast<int>(mod->getSettingValue<int64_t>("ground-horizon"));
        buf.waveHorizon = static_cast<int>(mod->getSettingValue<int64_t>("wave-horizon"));
        buf.assumeOrbs = mod->getSettingValue<bool>("orb-assume-hit");
        buf.orbClick = buf.assumeOrbs;
        buf.tryBothOrbs = mod->getSettingValue<bool>("orb-try-both");
        buf.avoidMovers = mod->getSettingValue<bool>("avoid-moving-objects");
        buf.autoRelease = mod->getSettingValue<bool>("wave-auto-release");
        buf.autoWindow = static_cast<int>(mod->getSettingValue<int64_t>("auto-release-window"));
        buf.driftCheck = mod->getSettingValue<bool>("drift-check");
        buf.spamGap = static_cast<int>(mod->getSettingValue<int64_t>("wave-spam-gap"));
        buf.orbAlign = mod->getSettingValue<bool>("orb-align");
        buf.orbTiming = mod->getSettingValue<bool>("orb-timing");
        buf.orbAlignGround = mod->getSettingValue<bool>("orb-align-ground");
        buf.fullState = mod->getSettingValue<bool>("full-ghost-state");
        buf.learn = mod->getSettingValue<bool>("level-memory");
        buf.robotSmart = mod->getSettingValue<bool>("robot-smart");
        buf.spiderSmart = mod->getSettingValue<bool>("spider-smart");
        buf.ufoSmart = mod->getSettingValue<bool>("ufo-smart");
        buf.ballSmart = mod->getSettingValue<bool>("ball-smart");
        buf.swingSmart = mod->getSettingValue<bool>("swing-smart");
        buf.train = mod->getSettingValue<bool>("training-mode");
        buf.perLevel = mod->getSettingValue<bool>("per-level-settings");
        buf.heatmap = false;
        buf.saveDing = false;
        buf.moreTime = mod->getSettingValue<bool>("more-time-death-spots");
        buf.dashAutoRelease = mod->getSettingValue<bool>("dash-auto-release");
        buf.perfLog = mod->getSettingValue<bool>("perf-log");
        buf.shipBuffer = mod->getSettingValue<bool>("ship-buffer");
        buf.twoPlayerSupport = mod->hasSetting("two-player-mode") ? mod->getSettingValue<bool>("two-player-mode") : true;
        buf.lateCrash = static_cast<int>(mod->getSettingValue<int64_t>("cube-late-crash"));
        buf.robustSaves = mod->getSettingValue<bool>("robust-saves");
        buf.ignoreTouching = mod->getSettingValue<bool>("ignore-touching-hazards");
        buf.deathLog = mod->getSettingValue<bool>("death-log");
        buf.explain = mod->getSettingValue<bool>("explain-inputs");
        buf.lowLatency = mod->getSettingValue<bool>("low-latency");
        if (buf.lowLatency) {
            buf.budgetMs = std::min(buf.budgetMs, 3.0);
            buf.robustSaves = false;
            buf.waveMargin = false;
        }
        buf.predictMovers = mod->getSettingValue<bool>("predict-moving-objects");
        buf.shipMaxDelay = static_cast<int>(mod->getSettingValue<int64_t>("ship-max-delay"));
        buf.shipSpamGap = static_cast<int>(mod->getSettingValue<int64_t>("ship-spam-gap"));
        buf.cbfSync = mod->getSettingValue<bool>("cbf-sync");
        buf.robotAutoRelease = mod->getSettingValue<bool>("robot-auto-release");
        buf.autoNormalRuns = mod->getSettingValue<bool>("auto-release-normal-runs");
        buf.waveV46 = mod->getSettingValue<bool>("wave-v46");
        buf.waveMargin = mod->getSettingValue<bool>("wave-safety-margin");
        buf.orbWindow = static_cast<int>(mod->getSettingValue<int64_t>("orb-align-ticks"));
        buf.perfMode = mod->getSettingValue<bool>("performance-mode");
        if (buf.perfMode) {
            buf.budgetMs = std::min(buf.budgetMs, 4.0);
            buf.robustSaves = false;
            buf.orbTiming = false;
            buf.driftCheck = false;
        }
        buf.userBudget = buf.budgetMs;
    }

    void clickBudget() {
        double budget = buf.userBudget;
        if (buf.autoBudget) {
            auto* director = CCDirector::get();
            double interval = director->getAnimationInterval() * 1000.0;
            double frame = director->getDeltaTime() * 1000.0;
            if (interval > 0.0 && frame > std::max(interval * 2.0, 20.0)) {
                budget = std::min(budget, 4.0);
            }
        }
        buf.budgetMs = budget;
    }

    void applyStage() {
        auto const& s = STAGES[buf.stage];
        buf.enabled = s.enabled;
        if (s.custom) {
            buf.ticksAll = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-ticks"));
            buf.ticksWave = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-wave-ticks"));
        }
        else {
            buf.ticksAll = s.ticksAll;
            buf.ticksWave = s.ticksWave;
        }
    }

    void refreshWatermark() {}

    void refreshAutoTag() {}

    size_t memorySpots();

    void exportConsole() {
        auto* mod = Mod::get();
        auto* layer = GameManager::get()->m_playLayer;
        std::string text;
        text += fmt::format("Click Before Frames {} console export\n", mod->getVersion().toVString());
        if (layer && layer->m_level) {
            text += fmt::format("Level: {} (id {}){}\n", std::string(layer->m_level->m_levelName), layer->m_level->m_levelID.value(),
                layer->m_isPracticeMode ? ", practice" : "");
        }
        float dt = buf.tickLength > 0.001f ? buf.tickLength : TICK;
        text += fmt::format("{} | physics ~{:.0f} steps/s\n", stageText(), 60.f / dt);
        text += fmt::format("budget {:g} ms | smart {} | full state {} | robust {} | late crash {} | practice only {}\n", buf.budgetMs,
            buf.smart, buf.fullState, buf.robustSaves, buf.lateCrash, buf.practiceOnly);
        text += fmt::format("orbs: align {} (ground {}, {}t) timing {} assume {} both {} | memory {} ({} spots)\n", buf.orbAlign,
            buf.orbAlignGround, buf.orbWindow, buf.orbTiming, buf.assumeOrbs, buf.tryBothOrbs, buf.learn, memorySpots());
        text += fmt::format("wave: v4.6 {} margin {} horizon {} spam gap {} | robot smart {} | spider smart {} | ship {} (max {}t) | 2P {}\n",
            buf.waveV46, buf.waveMargin, buf.waveHorizon, buf.spamGap, buf.robotSmart, buf.spiderSmart, buf.shipBuffer, buf.shipMaxDelay,
            buf.twoPlayerSupport);
        text += fmt::format("auto-release: wave {} robot {} dash {} normal runs {} window {} | movers: avoid {} predict {}\n",
            buf.autoRelease, buf.robotAutoRelease, buf.dashAutoRelease, buf.autoNormalRuns, buf.autoWindow, buf.avoidMovers,
            buf.predictMovers);
        text += fmt::format("debug: drift check {} | path view {} | perf log {} | death log {} | ignore touching {}\n", buf.driftCheck,
            buf.showPath, buf.perfLog, buf.deathLog, buf.ignoreTouching);
        text += fmt::format("explain inputs {} | low latency {} | cbf sync {}{}\n", buf.explain, buf.lowLatency, buf.cbfSync,
            Loader::get()->isModLoaded("syzzi.click_between_frames") ? " (CBF loaded)" : "");
        if (Loader::get()->isModLoaded("square3ang.cbfextrapolate")) {
            text += "CBF Extrapolate loaded (visual only)\n";
        }
        text += "----\n";
        for (auto const& line : exportLines) {
            text += line;
            text += '\n';
        }

        bool copied = geode::utils::clipboard::write(text);
        std::error_code ec;
        std::filesystem::create_directories(mod->getSaveDir(), ec);
        std::ofstream(mod->getSaveDir() / "console-export.txt", std::ios::trunc) << text;
        say("console {} ({} lines) + saved to console-export.txt", copied ? "copied to clipboard" : "NOT copied", exportLines.size());
    }

    std::string stageText() {
        if (!buf.enabled) {
            return "Buffer: OFF";
        }
        return fmt::format("Buffer{}: {:g}f / wave {:g}f{}", STAGES[buf.stage].custom ? " (custom)" : "", buf.ticksAll * TICK,
            buf.ticksWave * TICK, buf.train ? " [training]" : "");
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
        // panel nodes are owned by the PlayLayer
        buf.panel = nullptr;
        buf.panelTitle = nullptr;
        buf.panelLines.clear();
        buf.pathNode = nullptr;
        buf.autoTag = nullptr;
        buf.watermark = nullptr;
        buf.simulating = false;
        buf.injecting = false;
        buf.rapid = 0;
        buf.rapidP2 = 0;
        buf.autoReleased = false;
        buf.autoReleasedP2 = false;
        buf.recordPath = nullptr;
        watched.clear();
        drift.active = false;
        resetPending();
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

    void sendButton(PlayLayer* layer, bool down, bool isP1 = true) {
        buf.injecting = true;
        if (isP1) {
            buf.appliedInput = down;
        } else {
            buf.appliedInputP2 = down;
        }
        layer->handleButton(down, 1, isP1);
        buf.injecting = false;
    }

    void pressGhost(PlayerObject* ghost, bool down) {
        (ghost == buf.ghost1 ? buf.held1 : buf.held2) = down;
        if (down) {
            ghost->pushButton(PlayerButton::Jump);
        }
        else {
            ghost->releaseButton(PlayerButton::Jump);
        }
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false);
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setScale(real->getScale());
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
        if (buf.ghostShiftX != 0.f) {
            CCPoint pos = real->getPosition();
            pos.x += buf.ghostShiftX;
            ghost->setPosition(pos);
            ghost->m_position = pos;
        }

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
        if (!buf.fullState) {
            return;
        }

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
        ghost->m_fixGravityBug = real->m_fixGravityBug;
        ghost->m_fixRobotJump = real->m_fixRobotJump;
        ghost->m_enable22Changes = real->m_enable22Changes;
        ghost->m_enableImpulseFix = real->m_enableImpulseFix;
        ghost->m_totalTime = real->m_totalTime;

        ghost->m_lastPosition = real->m_lastPosition;

        // vertical motion
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

        // slopes
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

        // what it's standing on / touching
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

        // dash orbs
        ghost->m_dashX = real->m_dashX;
        ghost->m_dashY = real->m_dashY;
        ghost->m_dashAngle = real->m_dashAngle;
        ghost->m_dashStartTime = real->m_dashStartTime;
        ghost->m_dashRing = real->m_dashRing;
        ghost->m_dashFireFrame = real->m_dashFireFrame;

        // force / boost / state triggers already acting on the player
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
    }

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

    enum class InputKind { Tap, Wave, Robot, Other };
    int pressTick = -1;
    InputKind pressKind = InputKind::Other;

    InputKind kindOf(PlayerObject* p) {
        if (!p || p->m_isShip) {
            return InputKind::Other;
        }
        if (p->m_isDart) {
            return InputKind::Wave;
        }
        return p->m_isRobot ? InputKind::Robot : InputKind::Tap;
    }

    void recordRealInput(bool down, PlayerObject* p) {
        if (down) {
            pressTick = tickCount;
            pressKind = kindOf(p);
            return;
        }
        if (pressTick < 0) {
            return;
        }
        int held = tickCount - pressTick;
        pressTick = -1;
        if (buf.autoReleased || buf.autoReleasedP2) {
            return;
        }
        switch (pressKind) {
            case InputKind::Tap: tapHold.add(held); break;
            case InputKind::Robot: robotHold.add(held); break;
            default: break;
        }
    }

    void forgetPress() {
        pressTick = -1;
    }

    int holdEstimate() {
        return tapHold.pick(st(12), 50);  // ~50 ms, a typical tap
    }

    void watchObject(GameObject* object) {
        auto [it, added] = watched.try_emplace(object, Watched { object->m_positionX, object->m_positionY, -MOVER_TICKS });
        if (added) {
            it->second.rot = object->getRotation();
        }
        if (!added && tickCount - it->second.movedTick < st(MOVER_TICKS)) {
            buf.sawMover = true;
        }
    }

    std::vector<GameObject*> movingNow;

    void updateWatched() {
        if (watched.size() > WATCH_LIMIT) {
            watched.clear();
        }
        movingNow.clear();
        double px = 0.0;
        bool prune = false;
        if (auto* layer = GameManager::get()->m_playLayer; layer && layer->m_player1) {
            px = layer->m_player1->getPosition().x;
            prune = true;
        }
        for (auto it = watched.begin(); it != watched.end();) {
            auto* object = it->first;
            auto& w = it->second;
            if (prune && std::abs(object->m_positionX - px) > 800.0) {
                it = watched.erase(it);
                continue;
            }
            double rot = object->getRotation();
            if (object->m_positionX != w.x || object->m_positionY != w.y || rot != w.rot) {
                bool consecutive = tickCount - w.movedTick == 1;
                w.pvx = consecutive ? w.vx : 0.0;
                w.pvy = consecutive ? w.vy : 0.0;
                w.vx = object->m_positionX - w.x;
                w.vy = object->m_positionY - w.y;
                double dr = rot - w.rot;
                w.vrot = std::remainder(dr, 360.0);
                w.x = object->m_positionX;
                w.y = object->m_positionY;
                w.rot = rot;
                w.movedTick = tickCount;
                movingNow.push_back(object);
            }
            else if (tickCount - w.movedTick > 1) {
                w.vx = w.vy = w.pvx = w.pvy = w.vrot = 0.0;  // stopped
            }
            ++it;
        }
    }

    struct Mover {
        GameObject* object;
        double x, y, vx, vy;
        double turn = 0.0;  // radians the motion direction turns per step (0 = straight)
        double rotX = 0.0, rotY = 0.0, vrot = 0.0;  // both axes kept as they were (skewed objects)
        double ax = 0.0, ay = 0.0;  // v2.75: speeding up / slowing down (easing), per step
        int accelSteps = 0;         // v2.75: how long the easing is followed (until it would stop / over 3x speed)
    };
    std::vector<Mover> movers;

    void collectMovers() {
        movers.clear();
        if (!buf.predictMovers) {
            return;
        }
        for (auto* object : movingNow) {
            auto it = watched.find(object);
            if (it == watched.end()) {
                continue;
            }
            auto const& w = it->second;
            if ((w.vx != 0.0 || w.vy != 0.0 || w.vrot != 0.0) && tickCount - w.movedTick <= 1) {
                Mover m { object, object->m_positionX, object->m_positionY, w.vx, w.vy };
                double speed = std::hypot(w.vx, w.vy);
                double prev = std::hypot(w.pvx, w.pvy);
                if (speed > 1e-6 && prev > 1e-6) {
                    double turn = std::remainder(std::atan2(w.vy, w.vx) - std::atan2(w.pvy, w.pvx), 6.283185307179586);
                    double ratio = speed / prev;
                    if (std::abs(turn) > 1e-4 && std::abs(turn) < 0.5 && ratio > 0.8 && ratio < 1.25) {
                        m.turn = turn;
                    }
                    else if (std::abs(turn) <= 1e-4 && std::abs(ratio - 1.0) > 1e-3 && ratio > 0.5 && ratio < 2.0) {
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

    void placeMovers(int steps) {
        for (auto const& m : movers) {
            double x = m.x + m.vx * steps;
            double y = m.y + m.vy * steps;
            if (m.accelSteps > 0 && steps > 0) {
                int n = std::min(steps, m.accelSteps);
                double grow = 0.5 * n * (n + 1);
                x = m.x + m.vx * steps + m.ax * (grow + static_cast<double>(n) * (steps - n));
                y = m.y + m.vy * steps + m.ay * (grow + static_cast<double>(n) * (steps - n));
            }
            if (m.turn != 0.0 && steps > 0) {
                std::complex<double> v { m.vx, m.vy };
                auto rho = std::polar(1.0, m.turn);
                auto moved = v * rho * (1.0 - std::pow(rho, steps)) / (1.0 - rho);
                x = m.x + moved.real();
                y = m.y + moved.imag();
            }
            if (m.vrot != 0.0) {
                double spun = steps == 0 ? 0.0 : m.vrot * steps;
                m.object->setRotationX(static_cast<float>(m.rotX + spun));
                m.object->setRotationY(static_cast<float>(m.rotY + spun));
            }
            m.object->setPosition({ static_cast<float>(x), static_cast<float>(y) });
            m.object->m_positionX = x;
            m.object->m_positionY = y;
            m.object->setObjectRectDirty(true);
            m.object->m_isOrientedBoxDirty = true;
        }
    }

    void finishDrift(char const* why) {
        if (!drift.active) {
            return;
        }
        drift.active = false;
        if (drift.compared >= 8) {
            say("drift {:.1f}u max at t{} over {}t{}", drift.maxErr, drift.maxAt, drift.compared, why);
        }
    }

    void driftStep(PlayerObject* real) {
        if (!drift.active) {
            return;
        }
        if (drift.index >= 0) {
            if (drift.index >= static_cast<int>(drift.path.size())) {
                finishDrift("");
                return;
            }
            float err = ccpDistance(real->getPosition(), drift.path[drift.index]);
            if (err > drift.maxErr) {
                drift.maxErr = err;
                drift.maxAt = drift.index;
            }
            ++drift.compared;
        }
        ++drift.index;
    }

    constexpr float SLICE = 15.f;          // half a block
    constexpr int OUTCOME_TICKS = 90;      // alive this long after a decision = it worked

    struct Spot {
        uint32_t ok = 0;        // saves here that you survived
        uint32_t bad = 0;       // saves here followed by a death
        uint32_t deaths = 0;    // deaths after any decision here
        uint32_t delaySum = 0;  // sum of the delays of the saves that worked
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
        bool skipped = false;  // v2.64: a save was held back here because of memory
    } pending;

    int sliceOf(PlayerObject* p) {
        return static_cast<int>(std::floor(p->getPosition().x / SLICE));
    }

    Spot spotAround(int slice) {
        Spot sum;
        for (int s = slice - 1; s <= slice + 1; ++s) {
            auto it = memory.find(s);
            if (it == memory.end()) {
                continue;
            }
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

    size_t memorySpots() {
        return memory.size();
    }

    void saveMemory() {
        if (!memoryDirty || memoryKey.empty()) {
            return;
        }
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
        if (!level) {
            return;
        }
        int id = level->m_levelID.value();
        if (id > 0) {
            memoryKey = fmt::format("level_{}", id);
        }
        else {
            std::string name = level->m_levelName;
            for (auto& c : name) {
                if (!std::isalnum(static_cast<unsigned char>(c))) {
                    c = '_';
                }
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
        if (!memory.empty()) {
            say("memory: {} spots learned on this level", memory.size());
        }
    }

    void memoryChanged() {
        memoryDirty = true;
        if (++memoryUpdates % 10 == 0) {
            saveMemory();
        }
    }

    void creditPending() {
        if (!pending.active) {
            return;
        }
        pending.active = false;
        if (pending.delay > 0) {
            auto& spot = memory[pending.slice];
            ++spot.ok;
            spot.delaySum += static_cast<uint32_t>(std::lround(pending.delay / stepScale()));
            memoryChanged();
        }
    }

    void memoryNote(PlayerObject* p1, int delay) {
        if (!buf.learn || memoryKey.empty()) {
            return;
        }
        if (pending.active && tickCount - pending.tick >= st(30)) {
            creditPending();
        }
        pending = { true, sliceOf(p1), tickCount, delay, false };
    }

    void memoryTick() {
        if (pending.active && tickCount - pending.tick >= st(OUTCOME_TICKS)) {
            creditPending();
        }
    }

    bool memorySaysSkip(int slice);

    void memoryDeath() {
        if (!pending.active) {
            return;
        }
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
            if (!memorySaysSkip(pending.slice)) {
                say("memory: died with the input left alone, saves will be tried here again");
            }
        }
        memoryChanged();
    }

    bool memorySaysSkip(int slice) {
        if (!buf.learn) {
            return false;
        }
        auto spot = spotAround(slice);
        return spot.bad >= 2 && spot.bad > spot.ok;
    }

    int memoryDelay(int slice) {
        if (!buf.learn) {
            return 0;
        }
        auto spot = spotAround(slice);
        if (spot.ok < 2) {
            return 0;
        }
        return st(static_cast<int>(std::lround(static_cast<double>(spot.delaySum) / spot.ok)));
    }

    bool memoryRisky(int slice) {
        auto spot = spotAround(slice);
        return spot.deaths >= 3;
    }

    void snapshotObject(GameObject* object) {
        if (!object || buf.simUsed.contains(object)) {
            return;
        }
        buf.simUsed.insert(object);
        ObjSnapshot s { object, object->m_isActivated, nullptr, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect;
            s.activated = effect->m_activated;
            s.byP1 = effect->m_activatedByPlayer1;
            s.byP2 = effect->m_activatedByPlayer2;
        }
        buf.snapshots.push_back(s);
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
        buf.simUsed.clear();
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
            if (!buf.held1 && buf.ghost1) {
                buf.ghost1->releaseButton(PlayerButton::Jump);
            }
            buf.auto1 = false;
        }
        if (buf.auto2) {
            if (!buf.held2 && buf.ghost2) {
                buf.ghost2->releaseButton(PlayerButton::Jump);
            }
            buf.auto2 = false;
        }
    }

    enum class SimEnd { Ok, StopSearch };

    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int stopAt, SimEnd* end) {
        *end = SimEnd::Ok;
        if (!buf.ghost1) {
            return horizon;
        }
        buf.simulating = true;
        buf.ghostDied = false;
        buf.ghostHitPortal = false;
        buf.firstOrbTick = -1;
        buf.orbUsedTick = -1;

        bool second = dual && buf.ghost2 && p2;
        buf.haveRealRect = buf.ignoreTouching;
        buf.harmless.clear();
        if (buf.haveRealRect) {
            buf.realRect1 = p1->getObjectRect();
            buf.realRect2 = second ? p2->getObjectRect() : buf.realRect1;
        }
        copyState(buf.ghost1, p1);
        if (second) {
            copyState(buf.ghost2, p2);
        }
        bool curApplied = (p1 == layer->m_player2) ? buf.appliedInputP2 : buf.appliedInput;
        buf.held1 = buf.held2 = curApplied;
        buf.auto1 = buf.auto2 = false;
        buf.autoAt1 = buf.autoAt2 = -1;

        bool waveRun = p1->m_isDart || (second && p2->m_isDart);
        if (buf.waveV46 && waveRun && delay > 0 && !buf.skipPress) {
            pressGhost(buf.ghost1, !down);
            if (second) {
                pressGhost(buf.ghost2, !down);
            }
        }

        float dt = buf.tickLength;
        if (!(dt > 0.001f && dt < 2.0f)) {
            dt = TICK;
        }

        bool flying = isFlying(p1) || (dual && p2 && isFlying(p2));
        bool spiderRun = buf.spiderSmart && (p1->m_isSpider || (second && p2->m_isSpider));
        bool ufoRun = buf.ufoSmart && (p1->m_isBird || (second && p2->m_isBird));
        bool ballRun = buf.ballSmart && (p1->m_isBall || (second && p2->m_isBall));
        int total = delay + horizon;
        int survived = 0;
        int airTicks = 0;
        int groundTicks = 0;
        int tick = 0;
        bool died = false;
        bool safe = false;

        collectMovers();
        for (; tick < total; ++tick) {
            if (!movers.empty()) {
                placeMovers(tick + 1);
            }
            buf.simTick = tick;
            if (buf.autoAt1 >= 0 && tick >= buf.autoAt1) {
                if (!buf.held1 && buf.ghost1->m_touchingRings->count() > 0) {
                    buf.ghost1->pushButton(PlayerButton::Jump);
                    buf.auto1 = true;
                }
                buf.autoAt1 = -2;
            }
            if (second && buf.autoAt2 >= 0 && tick >= buf.autoAt2) {
                if (!buf.held2 && buf.ghost2->m_touchingRings->count() > 0) {
                    buf.ghost2->pushButton(PlayerButton::Jump);
                    buf.auto2 = true;
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
            if (buf.recordPath) {
                buf.recordPath->push_back(buf.ghost1->getPosition());
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
            if (flying && !ufoRun) {
                continue;
            }

            bool grounded = buf.ghost1->m_isOnGround && (!second || buf.ghost2->m_isOnGround);
            if (grounded && airTicks > 0 && buf.captureKiller) {
                buf.baseTouchedDown = true;
            }
            if (!grounded) {
                ++airTicks;
                groundTicks = 0;
                continue;
            }
            if (spiderRun) {
                if (++groundTicks >= st(SPIDER_LAND)) {
                    safe = true;
                    break;
                }
                continue;
            }
            if (airTicks < st(ballRun ? 2 : 6)) {
                continue;
            }
            if (groundTicks > st(ballRun ? 8 : 2)) {
                safe = true;
                break;
            }
            ++groundTicks;
        }

        if (!movers.empty()) {
            placeMovers(0);
            movers.clear();
        }
        ++buf.simRuns;
        buf.simTicks += tick + 1;

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

    int simulateOrbs(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter,
        int stopAt, SimEnd* end) {
        buf.orbClick = buf.assumeOrbs;
        buf.touchedOrb = false;
        int first = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, end);
        if (!buf.tryBothOrbs || !buf.smart || !buf.touchedOrb || first >= stopAt) {
            return first;
        }
        SimEnd firstEnd = *end;
        buf.orbClick = !buf.assumeOrbs;
        int second = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, end);
        buf.orbClick = buf.assumeOrbs;
        if (firstEnd != SimEnd::StopSearch) {
            *end = SimEnd::Ok;
        }
        int best = std::max(first, second);
        if (buf.orbTiming && best < stopAt) {
            for (int late : { 3, 6 }) {
                buf.orbClick = true;
                buf.orbClickDelay = st(late);
                SimEnd lateEnd;
                int ret = simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, stopAt, &lateEnd);
                buf.orbClickDelay = 0;
                buf.orbClick = buf.assumeOrbs;
                best = std::max(best, ret);
                if (best >= stopAt) {
                    break;
                }
            }
        }
        return best;
    }

    std::vector<CCPoint> recordRun(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon,
        int releaseAfter, bool* died = nullptr) {
        std::vector<CCPoint> path;
        buf.recordPath = &path;
        buf.orbClick = buf.assumeOrbs;
        SimEnd end;
        simulate(layer, p1, p2, dual, delay, down, horizon, releaseAfter, delay + horizon, &end);
        buf.recordPath = nullptr;
        if (died) {
            *died = buf.ghostDied;
        }
        return path;
    }

    void drawPath(std::vector<CCPoint> const& path, int delay, ccColor4F color, bool died) {
        for (size_t i = 1; i < path.size(); ++i) {
            buf.pathNode->drawSegment(path[i - 1], path[i], 1.2f, color);
        }
        if (delay > 0 && delay <= static_cast<int>(path.size())) {
            buf.pathNode->drawDot(path[delay - 1], 3.f, { 1.f, 1.f, 1.f, 0.9f });
        }
        if (died && !path.empty()) {
            buf.pathNode->drawDot(path.back(), 4.f, { 1.f, 0.2f, 0.2f, 1.f });
        }
    }

    void showPaths(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int chosen, bool down, int horizon, int releaseAfter) {
        if (!buf.showPath || !buf.pathNode) {
            return;
        }
        buf.pathNode->clear();
        buf.pathHoldUntil = tickCount + st(45);
        bool nowDied = false;
        auto now = recordRun(layer, p1, p2, dual, 0, down, horizon, releaseAfter, &nowDied);
        ccColor4F red { 1.f, 0.3f, 0.3f, 0.85f };
        ccColor4F green { 0.3f, 1.f, 0.5f, 0.9f };
        drawPath(now, 0, chosen > 0 ? red : (nowDied ? red : green), nowDied);
        if (chosen > 0) {
            bool died = false;
            auto delayed = recordRun(layer, p1, p2, dual, chosen, down, horizon, releaseAfter, &died);
            drawPath(delayed, chosen, green, died);
        }
        if (buf.learn && !memory.empty()) {
            int last = INT_MIN;
            for (auto const& point : now) {
                int slice = static_cast<int>(std::floor(point.x / SLICE));
                if (slice != last && memoryRisky(slice)) {
                    buf.pathNode->drawDot(point, 3.5f, { 1.f, 0.6f, 0.1f, 0.95f });
                }
                last = slice;
            }
        }
    }

    void startDrift(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter) {
        finishDrift("");
        if (!buf.driftCheck) {
            return;
        }
        drift.path = recordRun(layer, p1, p2, dual, delay, down, horizon, releaseAfter);
        drift.active = !drift.path.empty();
        drift.index = buf.firstStep < 1.f ? 0 : -1;
        drift.compared = 0;
        drift.maxErr = 0.f;
        drift.maxAt = 0;
    }

    std::chrono::steady_clock::time_point clickStart;

    double clickMs() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clickStart).count();
    }

    void decided(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter) {
        memoryNote(p1, delay);
        if (clickMs() > buf.budgetMs * 1.5) {
            finishDrift("");
            return;
        }
        startDrift(layer, p1, p2, dual, delay, down, horizon, releaseAfter);
        showPaths(layer, p1, p2, dual, delay, down, horizon, releaseAfter);
    }

    int fullWaveHorizon(bool mini) {
        int wh = std::clamp(buf.waveHorizon, 20, 160);
        return st(mini ? std::max(20, wh - 15) : wh);
    }

    int alignOrb(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int horizon, int releaseAfter, int learned, int* landed) {
        int window = st(std::clamp(buf.orbWindow, 1, 64));
        int found = 0;
        SimEnd end;
        buf.orbClick = false;
        buf.orbWasTiming = false;

        if (learned > 0 && learned <= window) {
            int ret = simulate(layer, p1, p2, dual, learned, true, horizon, releaseAfter, horizon, &end);
            if (buf.orbUsedTick >= 0 && ret >= horizon) {
                *landed = ret;
                buf.orbClick = buf.assumeOrbs;
                return learned;
            }
        }

        buf.skipPress = true;
        simulate(layer, p1, p2, dual, 0, true, window + 2, -1, window + 2, &end);
        buf.skipPress = false;
        int touch = buf.firstOrbTick;
        std::string trace;

        if (touch >= 0 && touch <= window) {
            int now = simulate(layer, p1, p2, dual, 0, true, horizon, releaseAfter, horizon, &end);
            bool nowUses = buf.orbUsedTick >= 0;
            if (nowUses && now < horizon && buf.orbTiming) {
                int bestRet = -1;
                for (int k = 1; k <= std::min(window, st(6)); ++k) {
                    int ret = simulate(layer, p1, p2, dual, k, true, horizon, releaseAfter, horizon, &end);
                    if (buf.explain) {
                        trace += fmt::format(" +{}:{}{}", k, ret, buf.orbUsedTick >= 0 ? "o" : "-");
                    }
                    if (end == SimEnd::StopSearch) {
                        break;
                    }
                    if (buf.orbUsedTick < 0 || ret < std::min(horizon, now + st(12)) || ret <= bestRet) {
                        continue;
                    }
                    bestRet = ret;
                    found = k;
                    *landed = ret;
                    buf.orbWasTiming = true;
                    if (ret >= horizon) {
                        break;
                    }
                }
            }
            else if (!nowUses) {
                int bestRet = -1;
                for (int k = std::max(1, touch - 1); k <= std::min(window, touch + st(6)); ++k) {
                    int ret = simulate(layer, p1, p2, dual, k, true, horizon, releaseAfter, horizon, &end);
                    if (buf.explain) {
                        trace += fmt::format(" +{}:{}{}", k, ret, buf.orbUsedTick >= 0 ? "o" : "-");
                    }
                    if (end == SimEnd::StopSearch) {
                        break;
                    }
                    if (buf.orbUsedTick < 0 || ret < std::min(now, horizon)) {
                        continue;
                    }
                    if (ret > bestRet) {
                        bestRet = ret;
                        found = k;
                        *landed = ret;
                    }
                    if (ret >= horizon) {
                        break;
                    }
                }
            }
            if (buf.explain) {
                say("orb: touch at t{}, press now {} -> {}t |{} => {}", touch, nowUses ? "uses it" : "misses it", now,
                    trace.empty() ? " (nothing to try)" : trace, found > 0 ? fmt::format("+{}t", found) : std::string("no pick"));
            }
        }
        buf.orbClick = buf.assumeOrbs;
        return found;
    }

    int dbgBestK = 0;
    int dbgBestRet = -1;

    constexpr int WAVE_TURN_MARGINS[] = { 4, 8, 12 };
    constexpr int WAVE_MIN_TURN = 6;
    constexpr int WAVE_ESCAPE = 16;

    int waveScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch) {
            return ret;
        }
        for (int margin : WAVE_TURN_MARGINS) {
            int turnAt = ret - st(margin);
            if (turnAt < st(WAVE_MIN_TURN)) {
                break;
            }
            int length = std::min(horizon, turnAt + st(WAVE_ESCAPE));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, delay, down, length, turnAt, length, &turnEnd);
            if (turned >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = turnAt;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }

    constexpr int ROBOT_HOLDS[] = { 6, 14, 26, 45 };
    int robotUsedHold = -1;

    bool quickScore = false;

    int robotScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, int horizon, int target, SimEnd* end) {
        int usual = robotHold.pick(st(20), 50);
        int best = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, usual, target, end);
        robotUsedHold = usual;
        if (best >= target || *end == SimEnd::StopSearch || quickScore) {
            return best;
        }
        for (int hold : ROBOT_HOLDS) {
            if (std::abs(st(hold) - usual) < st(3)) {
                continue;
            }
            SimEnd e;
            int ret = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, st(hold), target, &e);
            if (ret > best) {
                best = ret;
                robotUsedHold = st(hold);
            }
            if (best >= target) {
                break;
            }
        }
        return best;
    }

    constexpr int SPIDER_RETAP[] = { 4, 8, 12 };
    constexpr int SPIDER_MIN_RETAP = 4;

    int spiderScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, int horizon, int releaseAfter, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore) {
            return ret;
        }
        for (int margin : SPIDER_RETAP) {
            int at = ret - st(margin);
            if (at < st(SPIDER_MIN_RETAP)) {
                break;
            }
            int length = std::min(horizon, at + st(16));
            buf.retapAt = at;
            SimEnd e;
            int retapped = simulateOrbs(layer, p1, p2, dual, delay, true, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (retapped >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = at;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }

    constexpr int SAME_CRASH_TRIES = 4;
    bool crashFixed = false;

    bool sameCrash(int k, int ret, int base, int& streak) {
        if (streak >= 0 && std::abs(k + ret - base) <= 1) {
            ++streak;
        }
        else {
            streak = -1;
        }
        if (streak >= SAME_CRASH_TRIES) {
            crashFixed = true;
            return true;
        }
        return false;
    }

    int searchSmallest(int horizon, int maxDelay, int base, int* landed, std::function<int(int, int, SimEnd*)> const& score,
        double budget = -1.0, bool quiet = false, bool needFull = false) {
        if (budget < 0.0) {
            budget = buf.budgetMs;
        }
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        int target = needFull ? horizon : std::min(horizon, base + st(12));
        dbgBestK = 0;
        dbgBestRet = -1;
        int streak = 0;
        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = score(k, target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            if (ret >= target) {
                *landed = ret;
                return k;
            }
            if (sameCrash(k, ret, base, streak)) {
                return 0;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > budget) {
                if (!quiet) {
                    say("prediction budget hit at +{} ticks", k);
                }
                return 0;
            }
        }
        return 0;
    }

    int searchTwoPass(int horizon, int maxDelay, int base, int* landed, std::function<int(int, int, SimEnd*)> const& score,
        bool needFull = false) {
        quickScore = true;
        int k = searchSmallest(horizon, maxDelay, base, landed, score, buf.budgetMs * 0.5, true, needFull);
        quickScore = false;
        if (k > 0) {
            return k;
        }
        crashFixed = false;
        double left = buf.budgetMs - clickMs();
        if (left < 0.5) {
            say("prediction budget hit (quick pass only)");
            return 0;
        }
        return searchSmallest(horizon, maxDelay, base, landed, score, left, false, needFull);
    }

    constexpr int SHIP_RECOVER[] = { 8, 16, 24 };

    int shipScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, down, horizon, -1, target, end);
        if (ret >= target || *end == SimEnd::StopSearch) {
            return ret;
        }
        for (int margin : SHIP_RECOVER) {
            int turnAt = ret - st(margin);
            if (turnAt < st(WAVE_MIN_TURN)) {
                break;
            }
            int length = std::min(horizon, turnAt + st(WAVE_ESCAPE));
            SimEnd turnEnd;
            int turned = simulateOrbs(layer, p1, p2, dual, delay, down, length, turnAt, length, &turnEnd);
            if (turned >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = turnAt;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }

    constexpr int UFO_REFLAP[] = { 6, 12, 18 };

    int ufoScore(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, int horizon, int releaseAfter, int target,
        SimEnd* end) {
        int ret = simulateOrbs(layer, p1, p2, dual, delay, true, horizon, releaseAfter, target, end);
        if (ret >= target || *end == SimEnd::StopSearch || quickScore) {
            return ret;
        }
        for (int margin : UFO_REFLAP) {
            int at = ret - st(margin);
            if (at < st(4)) {
                break;
            }
            int length = std::min(horizon, at + st(20));
            buf.retapAt = at;
            SimEnd e;
            int reflapped = simulateOrbs(layer, p1, p2, dual, delay, true, length, releaseAfter, length, &e);
            buf.retapAt = -1;
            if (reflapped >= length) {
                if (buf.captureKiller && buf.baseEscape < 0) {
                    buf.baseEscape = at;
                    buf.baseCrash = ret;
                }
                return horizon;
            }
        }
        return ret;
    }

    bool marginUsed = false;

    int searchWave(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, int horizon, int maxDelay, int base,
        int* landed) {
        marginUsed = false;
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        int target = std::min(horizon, base + st(12));
        dbgBestK = 0;
        dbgBestRet = -1;
        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = waveScore(layer, p1, p2, dual, k, down, horizon, target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                return 0;
            }
            if (ret >= target) {
                *landed = ret;
                if (buf.waveMargin && k + 2 <= maxDelay) {
                    SimEnd e1, e2;
                    if (waveScore(layer, p1, p2, dual, k + 1, down, horizon, target, &e1) >= target &&
                        waveScore(layer, p1, p2, dual, k + 2, down, horizon, target, &e2) >= target) {
                        marginUsed = true;
                        return k + 1;
                    }
                }
                return k;
            }
            if (std::chrono::duration<double, std::milli>(clock::now() - start).count() > buf.budgetMs) {
                say("prediction budget hit at +{} ticks", k);
                return 0;
            }
        }
        return 0;
    }

    int searchDelay(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, bool anyWave, int horizon, int maxDelay,
        int releaseAfter, int base, int* landed, bool needFull = false) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        auto overBudget = [&] {
            return std::chrono::duration<double, std::milli>(clock::now() - start).count() > buf.budgetMs;
        };

        int target = needFull ? horizon : std::min(horizon, base + st(12));
        dbgBestK = 0;
        dbgBestRet = -1;
        int sameStreak = 0;
        int waveBestK = 0;
        int waveBestRet = base;
        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, anyWave ? horizon : target, &end);
            if (ret > dbgBestRet) {
                dbgBestRet = ret;
                dbgBestK = k;
            }
            if (end == SimEnd::StopSearch) {
                break;
            }
            if (anyWave) {
                if (ret >= horizon && ret - base >= k * 2 + 6) {
                    waveBestK = k;
                    waveBestRet = ret;
                    break;
                }
                if (ret >= base + k * 2 + 10 && ret > waveBestRet + 2) {
                    waveBestK = k;
                    waveBestRet = ret;
                    if (ret >= horizon) {
                        break;
                    }
                }
            }
            else if (ret < target && sameCrash(k, ret, base, sameStreak)) {
                break;
            }
            else if (ret >= target) {
                SimEnd full;
                if (ret < horizon && simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, horizon, &full) < horizon) {
                    for (int j = k + 1; j <= maxDelay && !overBudget(); ++j) {
                        SimEnd e;
                        int r = simulateOrbs(layer, p1, p2, dual, j, down, horizon, releaseAfter, horizon, &e);
                        if (e == SimEnd::StopSearch) {
                            break;
                        }
                        if (r >= horizon) {
                            *landed = r;
                            return j;
                        }
                    }
                }
                *landed = ret;
                return k;
            }
            if (overBudget()) {
                say("prediction budget hit at +{} ticks", k);
                break;
            }
        }
        if (anyWave && waveBestK > 0) {
            *landed = waveBestRet;
            return waveBestK;
        }
        return 0;
    }

    struct BenchRun {
        float startX = 0.f, startY = 0.f, dt = TICK;
        std::vector<std::pair<int, bool>> inputs;
        std::vector<CCPoint> track;
    };
    BenchRun benchLive;
    bool benchRecording = false;
    bool benchNeedStart = false;
    bool benchPending = false;
    std::string lastBench;

    std::filesystem::path benchFile() {
        return Mod::get()->getSaveDir() / "bench" / (memoryKey + ".txt");
    }

    size_t benchSavedTicks() {
        std::ifstream in(benchFile());
        float x, y, dt;
        size_t ticks = 0;
        if (in >> x >> y >> dt >> ticks) {
            return ticks;
        }
        return 0;
    }

    bool benchLoad(BenchRun& r) {
        std::ifstream in(benchFile());
        size_t ticks = 0, inputs = 0;
        if (!(in >> r.startX >> r.startY >> r.dt >> ticks >> inputs)) {
            return false;
        }
        for (size_t i = 0; i < inputs; ++i) {
            int t, d;
            if (!(in >> t >> d)) {
                return false;
            }
            r.inputs.push_back({ t, d != 0 });
        }
        for (size_t i = 0; i < ticks; ++i) {
            float x, y;
            if (!(in >> x >> y)) {
                return false;
            }
            r.track.push_back({ x, y });
        }
        return !r.track.empty();
    }

    void benchStart(PlayLayer* layer, PlayerObject* p1) {
        benchLive = {};
        benchLive.startX = p1->getPosition().x;
        benchLive.startY = p1->getPosition().y;
        benchLive.dt = buf.tickLength;
        bool fromCheckpoint = layer->m_isPracticeMode && layer->m_checkpointArray && layer->m_checkpointArray->count() > 0;
        benchRecording = !memoryKey.empty() && !fromCheckpoint;
    }

    void benchInput(bool down) {
        if (benchRecording) {
            benchLive.inputs.push_back({ static_cast<int>(benchLive.track.size()), down });
        }
    }

    void benchStep(PlayerObject* p1) {
        if (benchRecording) {
            benchLive.track.push_back(p1->getPosition());
            if (benchLive.track.size() > 200000) {
                benchRecording = false;
            }
        }
    }

    void benchFinish() {
        if (!benchRecording) {
            return;
        }
        benchRecording = false;
        if (benchLive.track.size() < 240 || benchLive.track.size() <= benchSavedTicks()) {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(benchFile().parent_path(), ec);
        std::ofstream out(benchFile(), std::ios::trunc);
        out << benchLive.startX << ' ' << benchLive.startY << ' ' << benchLive.dt << ' ' << benchLive.track.size() << ' '
            << benchLive.inputs.size() << '\n';
        for (auto const& [t, d] : benchLive.inputs) {
            out << t << ' ' << (d ? 1 : 0) << '\n';
        }
        for (auto const& p : benchLive.track) {
            out << p.x << ' ' << p.y << '\n';
        }
    }

    void benchCheck(PlayLayer* layer, PlayerObject* p1) {
        BenchRun r;
        if (!benchLoad(r)) {
            say("run check: no recorded run for this level yet (play one attempt from the start first)");
            return;
        }
        if (std::abs(r.startX - p1->getPosition().x) > 0.5f || std::abs(r.startY - p1->getPosition().y) > 0.5f) {
            say("run check: this attempt doesn't start where the recording did (start from the beginning, no checkpoint)");
            benchPending = true;
            return;
        }
        buf.simulating = true;
        buf.ghostDied = false;
        buf.ghostHitPortal = false;
        buf.haveRealRect = false;
        buf.harmless.clear();
        buf.orbClick = false;
        buf.retapAt = -1;
        copyState(buf.ghost1, p1);
        buf.held1 = false;
        buf.auto1 = false;
        buf.autoAt1 = -1;
        size_t next = 0;
        int firstOff = -1;
        float maxErr = 0.f;
        int t = 0;
        char const* end = "end of the recording";
        auto clockStart = std::chrono::steady_clock::now();
        for (; t < static_cast<int>(r.track.size()); ++t) {
            while (next < r.inputs.size() && r.inputs[next].first <= t) {
                pressGhost(buf.ghost1, r.inputs[next].second);
                ++next;
            }
            buf.simTick = t;
            advanceGhost(buf.ghost1, r.dt);
            releaseAutoClicks();
            if (!buf.ghostDied) {
                checkGhost(layer, buf.ghost1, r.dt);
            }
            if (buf.ghostDied) {
                end = "the ghost died";
                break;
            }
            if (buf.ghostHitPortal) {
                end = "a mode portal (predictions stop there)";
                break;
            }
            auto g = buf.ghost1->getPosition();
            float err = std::hypot(g.x - r.track[t].x, g.y - r.track[t].y);
            maxErr = std::max(maxErr, err);
            if (err > 3.f && firstOff < 0) {
                firstOff = t;
            }
        }
        restoreObjects();
        buf.ghost1->stopAllActions();
        buf.simulating = false;
        buf.orbClick = buf.assumeOrbs;
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - clockStart).count();
        int matched = firstOff < 0 ? t : firstOff;
        lastBench = fmt::format("{}/{} steps ({:.0f}%) match, stopped at {}", matched, r.track.size(),
            100.0 * matched / r.track.size(), end);
        say("run check: {} | max error {:.1f}u{} | {:.0f} ms", lastBench, maxErr,
            std::abs(r.dt - buf.tickLength) > 1e-4f ? " | recorded at another frame rate" : "", ms);
        std::error_code ec;
        std::filesystem::create_directories(Mod::get()->getSaveDir(), ec);
        std::ofstream(Mod::get()->getSaveDir() / "run-check-results.txt", std::ios::app)
            << Mod::get()->getVersion().toVString() << ' ' << memoryKey << ": " << lastBench << '\n';
    }

    void livePath(PlayLayer* layer, PlayerObject* p1) {
        if (!buf.showPath || buf.perfMode || !buf.pathNode || !buf.ghost1 || buf.simulating || p1->m_isDead || layer->m_isPlatformer) {
            return;
        }
        if (tickCount < buf.pathHoldUntil || !buf.queue.empty() || !buf.queueP2.empty() || tickCount % st(12) != 0) {
            return;
        }
        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        bool dual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        bool wave = p1->m_isDart || (p2 && p2->m_isDart);
        bool ship = p1->m_isShip || (p2 && p2->m_isShip);
        bool flyer = isFlying(p1) || (p2 && isFlying(p2));
        bool mini = (p1->m_isDart && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);
        int horizon = wave ? fullWaveHorizon(mini) : st(flyer ? 48 : std::clamp(buf.groundHorizon, 48, 240));
        bool down = !buf.appliedInput;
        int releaseAfter = down && !wave && !ship ? holdEstimate() : -1;

        buf.pathNode->clear();
        bool idleDied = false;
        buf.skipPress = true;
        auto idle = recordRun(layer, p1, p2, dual, 0, down, horizon, -1, &idleDied);
        buf.skipPress = false;
        drawPath(idle, 0, { 0.45f, 0.7f, 1.f, 0.6f }, idleDied);
        bool clickDied = false;
        auto click = recordRun(layer, p1, p2, dual, 0, down, horizon, releaseAfter, &clickDied);
        drawPath(click, 0, { 1.f, 0.85f, 0.3f, 0.85f }, clickDied);
    }

    void queueInput(std::deque<QueuedInput>& q, int ticks, bool down) {
        q.push_back({ ticks, down });
    }

    struct Stats {
        int saves = 0;
        int earlySum = 0;
        int attempts = 0;
        std::map<int, int> savesAt;
    };
    Stats attemptStats;
    Stats sessionStats;
    std::string statsKey;

    void noteSave(PlayLayer* layer, int ticks) {
        int t = static_cast<int>(std::lround(ticks / stepScale()));
        int pct = std::clamp(static_cast<int>(layer->getCurrentPercent()), 0, 100);
        for (auto* s : { &attemptStats, &sessionStats }) {
            ++s->saves;
            s->earlySum += t;
            ++s->savesAt[pct];
        }
    }

    std::string attemptSummary() {
        auto const& a = attemptStats;
        return fmt::format("{} {} | avg {:.1f}t ({:.2f}f) early", a.saves, buf.train ? (a.saves == 1 ? "early click" : "early clicks")
            : (a.saves == 1 ? "save" : "saves"), static_cast<double>(a.earlySum) / a.saves, a.earlySum * TICK / a.saves);
    }

    void endAttempt() {
        if (attemptStats.saves > 0) {
            ++sessionStats.attempts;
            say("attempt: {}", attemptSummary());
        }
        attemptStats = {};
    }

    bool trainInstead(PlayLayer* layer, int ticks, bool down, char const* what) {
        if (!buf.train) {
            return false;
        }
        noteSave(layer, ticks);
        say("TRAIN {} {}: {}t ({:g}f) early", what, down ? "press" : "release", ticks, ticks * TICK);
        return true;
    }

    constexpr char const* PROFILE_KEYS[] = { "robust-saves", "orb-align", "orb-timing", "level-memory",
        "more-time-death-spots", "ship-buffer", "two-player-mode", "ufo-smart", "ball-smart", "swing-smart", "cbf-sync",
        "predict-moving-objects", "training-mode" };

    struct Globals {
        bool have = false;
        std::vector<bool> values;
    } globals;

    std::string profileKey() {
        return memoryKey.empty() ? std::string() : "profile-" + memoryKey;
    }

    void saveProfile() {
        if (!buf.perLevel || profileKey().empty() || !globals.have) {
            return;
        }
        auto v = matjson::Value::object();
        for (auto* key : PROFILE_KEYS) {
            if (Mod::get()->hasSetting(key)) {
                v[key] = Mod::get()->getSettingValue<bool>(key);
            }
        }
        Mod::get()->setSavedValue(profileKey(), v);
    }

    void restoreGlobals() {
        if (!globals.have) {
            return;
        }
        auto* mod = Mod::get();
        for (size_t i = 0; i < std::size(PROFILE_KEYS); ++i) {
            if (mod->hasSetting(PROFILE_KEYS[i])) {
                mod->setSettingValue<bool>(PROFILE_KEYS[i], globals.values[i]);
            }
        }
        globals = {};
        mod->setSavedValue("profile-globals", matjson::Value(nullptr));
        readSettings();
        applyStage();
    }

    void loadProfile() {
        if (!buf.perLevel || profileKey().empty()) {
            return;
        }
        auto* mod = Mod::get();
        globals.have = true;
        globals.values.clear();
        auto backup = matjson::Value::object();
        for (auto* key : PROFILE_KEYS) {
            bool value = mod->hasSetting(key) ? mod->getSettingValue<bool>(key) : true;
            globals.values.push_back(value);
            backup[key] = value;
        }
        mod->setSavedValue("profile-globals", backup);
        if (!mod->hasSavedValue(profileKey())) {
            return;
        }
        auto v = mod->getSavedValue<matjson::Value>(profileKey());
        if (!v.isObject()) {
            return;
        }
        for (auto* key : PROFILE_KEYS) {
            if (v.contains(key) && mod->hasSetting(key)) {
                mod->setSettingValue<bool>(key, v[key].asBool().unwrapOr(mod->getSettingValue<bool>(key)));
            }
        }
        readSettings();
        applyStage();
        say("level settings loaded: {}", stageText());
    }

    void recoverGlobals() {
        auto* mod = Mod::get();
        if (!mod->hasSavedValue("profile-globals")) {
            return;
        }
        auto v = mod->getSavedValue<matjson::Value>("profile-globals");
        if (!v.isObject()) {
            return;
        }
        for (auto* key : PROFILE_KEYS) {
            if (v.contains(key) && mod->hasSetting(key)) {
                mod->setSettingValue<bool>(key, v[key].asBool().unwrapOr(mod->getSettingValue<bool>(key)));
            }
        }
        mod->setSavedValue("profile-globals", matjson::Value(nullptr));
    }

    void refreshHeatmap() {}

    std::string statsText() {
        auto const& s = sessionStats;
        std::string text;
        if (s.saves == 0) {
            text += "No early clicks yet this session.\n";
        }
        else {
            text += fmt::format("<cp>{}</c> {} in <cp>{}</c> attempts, on average <cp>{:.1f}t</c> ({:.2f}f) early.\n",
                s.saves, buf.train ? "early clicks" : "saves", s.attempts, static_cast<double>(s.earlySum) / s.saves, s.earlySum * TICK / s.saves);
            std::vector<std::pair<int, int>> spots(s.savesAt.begin(), s.savesAt.end());
            std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            text += "Most early clicks at:";
            for (size_t i = 0; i < std::min<size_t>(3, spots.size()); ++i) {
                text += fmt::format(" <cy>{}%</c> ({}x)", spots[i].first, spots[i].second);
            }
            text += "\n";
        }
        auto* layer = GameManager::get()->m_playLayer;
        if (layer && layer->m_levelLength > 0.f) {
            std::map<int, uint32_t> deathsAt;
            for (auto const& [slice, spot] : memory) {
                if (spot.deaths > 0) {
                    deathsAt[std::clamp(static_cast<int>((slice + 0.5f) * SLICE / layer->m_levelLength * 100.f), 0, 100)] += spot.deaths;
                }
            }
            std::vector<std::pair<int, uint32_t>> spots(deathsAt.begin(), deathsAt.end());
            std::sort(spots.begin(), spots.end(), [](auto const& a, auto const& b) { return a.second > b.second; });
            if (!spots.empty()) {
                text += "Most deaths at:";
                for (size_t i = 0; i < std::min<size_t>(3, spots.size()); ++i) {
                    text += fmt::format(" <cr>{}%</c> ({}x)", spots[i].first, spots[i].second);
                }
                text += "\n";
            }
        }
        if (!lastBench.empty()) {
            text += "Run check: " + lastBench + "\n";
        }
        return text;
    }

    bool intercept(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        clickStart = std::chrono::steady_clock::now();
        clickBudget();
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) {
            return false;
        }

        auto* level = layer->m_level;
        bool twoPlayer = level && level->m_twoPlayerMode;
        bool inDual = layer->m_gameState.m_isDualMode && layer->m_player2;

        // In 2-player mode outside of dual sections, both P1 and P2 buttons steer Player 1.
        // Inside dual sections, !isPlayer1 steers Player 2 independently.
        bool controlP2 = twoPlayer && !isPlayer1 && inDual;
        if (twoPlayer && !buf.twoPlayerSupport && controlP2) {
            return false;
        }

        // In 2-player levels, P1 and P2 have separate buttons, so never simulate P1's input on P2 (or vice versa).
        bool dual = !twoPlayer && inDual;

        buf.isPlayer1 = !controlP2;
        auto& q = controlP2 ? buf.queueP2 : buf.queue;
        auto& curShift = controlP2 ? buf.shiftP2 : buf.shift;
        auto& curReleaseShift = controlP2 ? buf.releaseShiftP2 : buf.releaseShift;
        auto& curRealInput = controlP2 ? buf.realInputP2 : buf.realInput;
        auto& curAppliedInput = controlP2 ? buf.appliedInputP2 : buf.appliedInput;
        auto& curSinceInput = controlP2 ? buf.sinceInputP2 : buf.sinceInput;
        auto& curRapid = controlP2 ? buf.rapidP2 : buf.rapid;
        auto& curAutoReleased = controlP2 ? buf.autoReleasedP2 : buf.autoReleased;
        auto& curAutoDash = controlP2 ? buf.autoDashP2 : buf.autoDash;

        curRealInput = down;
        if (curAutoReleased) {
            curAutoReleased = false;
            curAutoDash = false;
            if (!down) {
                return true;
            }
        }
        if (!buf.enabled || (twoPlayer && !buf.twoPlayerSupport)) {
            curAppliedInput = down;
            return false;
        }
        if (buf.practiceOnly && !layer->m_isPracticeMode) {
            curAppliedInput = down;
            return false;
        }
        if (layer->m_isPlatformer) {
            curAppliedInput = down;
            return false;
        }

        auto* p1 = controlP2 ? layer->m_player2 : self->m_player1;
        auto* p2 = dual ? layer->m_player2 : nullptr;

        float midTick = 0.f;
        bool inUpd = controlP2 ? buf.inRealUpdateP2 : buf.inRealUpdate;
        float tStartX = controlP2 ? buf.tickStartXP2 : buf.tickStartX;
        float tDx = controlP2 ? buf.tickDxP2 : buf.tickDx;
        if (buf.cbfSync && inUpd && p1 && std::abs(tDx) > 0.01f) {
            midTick = std::clamp((p1->getPosition().x - tStartX) / tDx, 0.f, 0.999f);
        }
        buf.firstStep = 1.f - midTick;
        struct FirstStepReset {
            ~FirstStepReset() {
                buf.firstStep = 1.f;
            }
        } firstStepReset;
        int early = midTick > 0.f ? 1 : 0;

        bool anyShip = p1 && (p1->m_isShip || (p2 && p2->m_isShip));
        if (!p1 || p1->m_isDead || (anyShip && !buf.shipBuffer)) {
            q.clear();
            curShift = 0;
            curReleaseShift = 0;
            curAppliedInput = down;
            return false;
        }

        if (down && q.empty()) {
            curReleaseShift = 0;
        }

        bool p1Wave = p1->m_isDart;
        bool anyWave = p1Wave || (p2 && p2->m_isDart);
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        int spamGap = st(anyShip && !anyWave ? std::clamp(buf.shipSpamGap, 2, 40) : std::clamp(buf.spamGap, 4, 40));
        if (curSinceInput < spamGap) {
            ++curRapid;
        }
        else {
            curRapid = 0;
        }
        curSinceInput = 0;
        bool anyUfo = p1->m_isBird || (p2 && p2->m_isBird);
        bool anyBall = p1->m_isBall || (p2 && p2->m_isBall);
        bool anySwing = p1->m_isSwing || (p2 && p2->m_isSwing);
        if ((anyWave || anyShip || (anyUfo && buf.ufoSmart) || (anyBall && buf.ballSmart) || (anySwing && buf.swingSmart)) &&
            curRapid >= SPAM_STREAK) {
            if (buf.explain) {
                say("pass {}: spam ({} fast inputs in a row, gap < {}t)", down ? "press" : "release", curRapid, spamGap);
            }
            if (buf.showPath && buf.pathNode) {
                buf.pathNode->clear();
                buf.pathNode->drawDot(p1->getPosition(), 4.f, { 0.6f, 0.6f, 0.65f, 0.9f });
            }
            if (!q.empty()) {
                for (size_t i = 0; i < q.size(); ++i) {
                    q[i].ticksLeft = static_cast<int>(i);
                }
                if (q.back().down != down) {
                    queueInput(q, static_cast<int>(q.size()), down);
                }
                curShift = 0;
                return true;
            }
            curAppliedInput = down;
            return false;
        }

        if (!q.empty() && anyWave && buf.waveV46) {
            auto first = q.front();
            q.erase(q.begin() + 1, q.end());
            if (down != first.down) {
                queueInput(q, first.ticksLeft + 1, down);
            }
            return true;
        }

        if (!q.empty()) {
            int after = q.back().ticksLeft;
            queueInput(q, buf.lowLatency && down ? after : std::max(curShift - early, after), down);
            return true;
        }

        // Releases only matter for wave and robot.
        if (!p1Wave) {
            bool robot = p1->m_isRobot;
            bool wave2 = false;
            if (p2 && !robot) {
                wave2 = p2->m_isDart;
                if (!wave2) {
                    robot = p2->m_isRobot;
                }
            }
            bool dashing = p1->m_isDashing || (p2 && p2->m_isDashing);
            if (!wave2 && !down && !robot && !dashing && !anyShip) {
                if (curReleaseShift > 0) {
                    curShift = curReleaseShift;
                    curReleaseShift = 0;
                    queueInput(q, curShift, false);
                    return true;
                }
                curAppliedInput = false;
                return false;
            }
        }

        bool anyFlyer = isFlying(p1) || (p2 && isFlying(p2));
        int horizon = st(anyFlyer ? 48 : std::clamp(buf.groundHorizon, 48, 240));
        if (anyWave) {
            horizon = fullWaveHorizon(mini);
        }
        int maxDelay = st(anyWave ? buf.ticksWave : (anyShip ? std::clamp(buf.shipMaxDelay, 0, 16) : buf.ticksAll));
        if (buf.lowLatency) {
            maxDelay = std::min(maxDelay, st(4));
        }
        if (maxDelay <= 0) {
            if (buf.explain) {
                say("pass {}: max delay is 0 for this mode", down ? "press" : "release");
            }
            curAppliedInput = down;
            return false;
        }

        bool anyRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        int releaseAfter = -1;
        if (down && !anyWave && !anyShip) {
            releaseAfter = anyRobot ? robotHold.pick(-1, 50) : holdEstimate();
        }

        bool grounded = p1->m_isOnGround && (!p2 || p2->m_isOnGround);
        if (buf.explain && down && grounded && !buf.orbAlignGround && !anyWave && !anyShip && buf.orbAlign && buf.smart) {
            SimEnd probeEnd;
            buf.skipPress = true;
            buf.orbClick = false;
            int window = st(std::clamp(buf.orbWindow, 1, 64));
            simulate(layer, p1, p2, dual, 0, true, window + 2, -1, window + 2, &probeEnd);
            buf.skipPress = false;
            buf.orbClick = buf.assumeOrbs;
            if (buf.firstOrbTick >= 0 && buf.firstOrbTick <= window) {
                say("orb: you'd touch an orb at t{}, but this press is from the ground ('also from the ground' is off)",
                    buf.firstOrbTick);
            }
        }
        if (down && !anyWave && !anyShip && buf.orbAlign && buf.smart && (!grounded || buf.orbAlignGround)) {
            int orbLanded = 0;
            int k = memorySaysSkip(sliceOf(p1)) ? 0 : alignOrb(layer, p1, p2, dual, horizon, releaseAfter, memoryDelay(sliceOf(p1)), &orbLanded);
            if (k > 0 && trainInstead(layer, k, down, "orb")) {
                decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
                curAppliedInput = down;
                return false;
            }
            if (k > 0) {
                say("ORB {} +{}t ({:g}f) -> {}t{}", buf.orbWasTiming ? "timing" : "press", k, k * TICK, orbLanded,
                    controlP2 ? " [P2]" : "");
                noteSave(layer, k);
                decided(layer, p1, p2, dual, k, down, horizon, releaseAfter);
                curShift = k;
                queueInput(q, k - early, down);
                return true;
            }
        }

        buf.sawMover = false;
        buf.trackMovers = buf.avoidMovers || buf.predictMovers;
        SimEnd end;
        bool cubeStyleWave = anyWave && !buf.waveV46;
        bool robotPress = down && anyRobot && !anyWave && buf.robotSmart;
        bool anySpider = p1->m_isSpider || (p2 && p2->m_isSpider);
        bool spiderPress = down && anySpider && !anyWave && !robotPress && buf.spiderSmart;
        bool ufoPress = down && anyUfo && !anyWave && !anyShip && buf.ufoSmart;
        bool swingPress = down && anySwing && !anyWave && !anyShip && !ufoPress && buf.swingSmart;
        ufoPress = ufoPress || swingPress;
        bool shipInput = anyShip && !anyWave;
        buf.captureKiller = true;
        buf.baseTouchedDown = false;
        buf.killerSeen = false;
        buf.baseEscape = -1;
        int base = cubeStyleWave ? waveScore(layer, p1, p2, dual, 0, down, horizon, horizon, &end)
                 : robotPress    ? robotScore(layer, p1, p2, dual, 0, horizon, horizon, &end)
                 : spiderPress   ? spiderScore(layer, p1, p2, dual, 0, horizon, releaseAfter, horizon, &end)
                 : ufoPress      ? ufoScore(layer, p1, p2, dual, 0, horizon, releaseAfter, horizon, &end)
                 : shipInput     ? shipScore(layer, p1, p2, dual, 0, down, horizon, horizon, &end)
                                 : simulateOrbs(layer, p1, p2, dual, 0, down, horizon, releaseAfter, horizon, &end);
        buf.captureKiller = false;
        buf.trackMovers = false;

        if (buf.killerSeen && (base <= st(3) || (buf.deathLog && base < horizon))) {
            auto real = p1->getObjectRect();
            auto const& g = buf.ghostDeathRect;
            if (buf.killerType >= 0) {
                auto const& k = buf.killerRect;
                say("ghost killed at t{} by obj {} (type {}{}{}) at dx {:.1f} dy {:.1f} size {:.1f}x{:.1f}", buf.killerTick, buf.killerId,
                    buf.killerType, buf.killerNoTouch ? ", no-touch" : "", buf.killerDisabled ? ", disabled" : "",
                    k.getMidX() - real.getMidX(), k.getMidY() - real.getMidY(), k.size.width, k.size.height);
            }
            else {
                say("ghost killed at t{} by no object (crushed / out of bounds / slope)", buf.killerTick);
            }
            say("  hitbox ghost {:.1f}x{:.1f} vs you {:.1f}x{:.1f}, ghost moved dx {:.1f} dy {:.1f}{}", g.size.width, g.size.height,
                real.size.width, real.size.height, g.getMidX() - real.getMidX(), g.getMidY() - real.getMidY(),
                buf.predictMovers ? " (moving objects predicted)" : "");
            auto desc = [](PlayerObject* p) {
                if (!p) {
                    return std::string("none");
                }
                char const* mode = p->m_isDart ? "wave" : p->m_isShip ? "ship" : p->m_isBird ? "ufo" : p->m_isBall ? "ball"
                    : p->m_isRobot ? "robot" : p->m_isSpider ? "spider" : p->m_isSwing ? "swing" : "cube";
                return fmt::format("{}{}{}", mode, p->m_controlsDisabled ? " controls-off" : "", p->isVisible() ? "" : " hidden");
            };
            say("  players: P1 {} | P2 {} | dual {} | predicted {}", desc(layer->m_player1), desc(layer->m_player2),
                layer->m_gameState.m_isDualMode, p1 == layer->m_player1 ? "P1" : "P2");
        }

        std::string tickTag = midTick > 0.f ? fmt::format(" @{:.2f}", midTick) : std::string();
        if (controlP2) {
            tickTag += " [P2]";
        }
        if (base >= horizon) {
            bool reactMode = cubeStyleWave || shipInput || ufoPress || spiderPress;
            if (buf.moreTime && buf.learn && reactMode && buf.baseEscape >= 0 && spotAround(sliceOf(p1)).deaths >= 2) {
                int crash0 = buf.baseCrash;
                int bestK = 0;
                int bestCrash = crash0;
                int hold = (cubeStyleWave || shipInput) ? -1 : releaseAfter;
                int moreMax = (cubeStyleWave || shipInput) ? std::min(maxDelay, st(4)) : maxDelay;
                for (int k = 1; k <= moreMax; ++k) {
                    SimEnd e;
                    int ret = simulateOrbs(layer, p1, p2, dual, k, down, horizon, hold, horizon, &e);
                    if (e == SimEnd::StopSearch) {
                        break;
                    }
                    if (ret >= horizon) {
                        bestK = k;
                        bestCrash = -1;
                        break;
                    }
                    int crashAt = k + ret;
                    if (crashAt >= crash0 + st(8) && crashAt > bestCrash) {
                        bestK = k;
                        bestCrash = crashAt;
                    }
                }
                if (bestK > 0 && !buf.train) {
                    noteSave(layer, bestK);
                    say("MORE TIME {}{} +{}t: crash t{} -> {} (died here {}x)", down ? "press" : "release", tickTag, bestK, crash0,
                        bestCrash < 0 ? std::string("none") : fmt::format("t{}", bestCrash), spotAround(sliceOf(p1)).deaths);
                    decided(layer, p1, p2, dual, bestK, down, horizon, releaseAfter);
                    curShift = bestK;
                    queueInput(q, bestK - early, down);
                    return true;
                }
            }
            if (buf.explain) {
                if (buf.baseEscape >= 0) {
                    say("pass {}{}: safe only if you react by t{} (else crash at t{})", down ? "press" : "release", tickTag,
                        buf.baseEscape, buf.baseCrash);
                }
                else {
                    say("pass {}{}: safe now (survives the {}t look-ahead)", down ? "press" : "release", tickTag, horizon);
                }
            }
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            curAppliedInput = down;
            return false;
        }

        bool groundPress = down && !anyFlyer && !anyRobot && !anySpider && !anyWave;
        if (groundPress && buf.lateCrash > 0 && base >= st(buf.lateCrash) && buf.baseTouchedDown) {
            if (buf.explain) {
                say("pass press: crash only at {}t, after the jump would land (late crash rule)", base);
            }
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            curAppliedInput = down;
            return false;
        }

        int slice = sliceOf(p1);
        if (memorySaysSkip(slice)) {
            say("memory: saves here failed before, input left alone");
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            pending.skipped = true;
            curAppliedInput = down;
            return false;
        }

        int landed = 0;
        int saved = 0;
        bool fromMemory = false;
        crashFixed = false;
        auto scoreAt = [&](int k, int target, SimEnd* e) {
            return cubeStyleWave ? waveScore(layer, p1, p2, dual, k, down, horizon, target, e)
                 : robotPress    ? robotScore(layer, p1, p2, dual, k, horizon, target, e)
                 : spiderPress   ? spiderScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e)
                 : ufoPress      ? ufoScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e)
                 : shipInput     ? shipScore(layer, p1, p2, dual, k, down, horizon, target, e)
                                 : simulateOrbs(layer, p1, p2, dual, k, down, horizon, releaseAfter, target, e);
        };

        int needed = (groundPress || robotPress || ufoPress) ? horizon : std::min(horizon, base + st(12));
        int learned = memoryDelay(slice);
        if (learned > 0 && learned <= maxDelay && !(anyWave && buf.waveV46)) {
            int target = needed;
            SimEnd e;
            int ret = scoreAt(learned, target, &e);
            if (ret >= target) {
                saved = learned;
                landed = ret;
                fromMemory = true;
            }
        }
        if (saved <= 0) {
            saved = cubeStyleWave ? searchWave(layer, p1, p2, dual, down, horizon, maxDelay, base, &landed)
                  : robotPress    ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return robotScore(layer, p1, p2, dual, k, horizon, target, e);
                    }, true)
                  : spiderPress   ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return spiderScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e);
                    })
                  : shipInput     ? searchSmallest(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return shipScore(layer, p1, p2, dual, k, down, horizon, target, e);
                    })
                  : ufoPress      ? searchTwoPass(horizon, maxDelay, base, &landed, [&](int k, int target, SimEnd* e) {
                        return ufoScore(layer, p1, p2, dual, k, horizon, releaseAfter, target, e);
                    }, true)
                                  : searchDelay(layer, p1, p2, dual, down, anyWave, horizon, maxDelay, releaseAfter, base, &landed,
                                        groundPress);
        }
        if (saved <= 0) {
            say("no save: {}{} dies in {}t (max {}t, best +{}t -> {}t of {}){}", down ? "press" : "release", tickTag, base, maxDelay,
                dbgBestK, dbgBestRet, horizon, !crashFixed ? "" : base < st(30) ? " - too late: waiting only makes it crash sooner" : " - same crash at any timing (next obstacle)");
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            curAppliedInput = down;
            return false;
        }

        bool robustNote = false;
        if (buf.robustSaves && !(anyWave && buf.waveV46)) {
            int target = needed;
            float shift = std::max(0.3f, 0.5f * (311.58f / 240.f) * (std::abs(p1->m_playerSpeed) / 0.9f));
            auto robust = [&](int k) {
                for (float off : { shift, -shift }) {
                    buf.ghostShiftX = off;
                    SimEnd e;
                    int ret = scoreAt(k, target, &e);
                    buf.ghostShiftX = 0.f;
                    if (ret < target) {
                        return false;
                    }
                }
                return true;
            };
            if (clickMs() < buf.budgetMs && !robust(saved)) {
                for (int k = saved + 1; k <= std::min(maxDelay, saved + st(3)); ++k) {
                    SimEnd e;
                    int ret = scoreAt(k, target, &e);
                    if (ret >= target && robust(k)) {
                        saved = k;
                        landed = ret;
                        robustNote = true;
                        break;
                    }
                }
            }
        }

        if (buf.avoidMovers) {
            buf.trackMovers = true;
            simulateOrbs(layer, p1, p2, dual, saved, down, horizon, releaseAfter, horizon, &end);
            buf.trackMovers = false;
        }
        if (buf.avoidMovers && buf.sawMover) {
            say("skip +{}t save: moving objects ahead", saved);
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            curAppliedInput = down;
            return false;
        }

        if (trainInstead(layer, saved, down, swingPress ? "swing" : "")) {
            decided(layer, p1, p2, dual, 0, down, horizon, releaseAfter);
            curAppliedInput = down;
            return false;
        }
        if (robotPress) {
            releaseAfter = robotUsedHold;
        }
        noteSave(layer, saved);
        say("SAVED {}{} +{}t ({:g}f)  {}->{}  [max {}]{}", down ? "press" : "release", tickTag, saved, saved * TICK, base, landed, maxDelay,
            robustNote ? std::string("  (robust)")
            : fromMemory ? std::string("  (memory)")
            : cubeStyleWave && marginUsed ? std::string("  +margin")
            : robotPress ? fmt::format("  hold {}t", robotUsedHold) : std::string());
        decided(layer, p1, p2, dual, saved, down, horizon, releaseAfter);
        curShift = saved;
        queueInput(q, saved - early, down);
        return true;
    }

    void cycleStage(int dir = 1) {
        buf.stage = (buf.stage + dir + STAGE_COUNT) % STAGE_COUNT;
        Mod::get()->setSavedValue("stage", buf.stage);
        resetPending();
        auto* layer = GameManager::get()->m_playLayer;
        if (layer) {
            if (!buf.autoReleased && buf.appliedInput != buf.realInput) {
                sendButton(layer, buf.realInput, true);
            }
            if (!buf.autoReleasedP2 && buf.appliedInputP2 != buf.realInputP2) {
                sendButton(layer, buf.realInputP2, false);
            }
        }
        applyStage();
        readSettings();
        saveProfile();
        say("F6 -> {}", stageText());
        refreshAutoTag();
        refreshWatermark();
        refreshPanel();
    }

    void tryAutoRelease(PlayLayer* layer, PlayerObject* p, bool isP1) {
        auto& curAutoReleased = isP1 ? buf.autoReleased : buf.autoReleasedP2;
        auto& curAutoDash = isP1 ? buf.autoDash : buf.autoDashP2;
        auto& curQueue = isP1 ? buf.queue : buf.queueP2;
        bool curRealInput = isP1 ? buf.realInput : buf.realInputP2;
        bool curAppliedInput = isP1 ? buf.appliedInput : buf.appliedInputP2;

        if ((!buf.autoRelease && !buf.robotAutoRelease && !buf.dashAutoRelease) || curAutoReleased || layer->m_isPlatformer) {
            return;
        }
        if (!layer->m_isPracticeMode && !buf.autoNormalRuns) {
            return;
        }
        if (!curQueue.empty() || !curRealInput || !curAppliedInput || !buf.ghost1) {
            return;
        }
        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        if (twoPlayer && !buf.twoPlayerSupport) {
            return;
        }
        bool dual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        bool dash = p->m_isDashing || (p2 && p2->m_isDashing);
        bool wave = !dash && (p->m_isDart || (p2 && p2->m_isDart));
        bool robot = !dash && !wave && (p->m_isRobot || (p2 && p2->m_isRobot));
        if (p->m_isDead || !((dash && buf.dashAutoRelease) || (wave && buf.autoRelease) || (robot && buf.robotAutoRelease))) {
            return;
        }
        bool mini = (p->m_isDart && p->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        int window = st(std::clamp(buf.autoWindow, 2, 24));
        buf.sawMover = false;
        buf.skipPress = true;
        buf.orbClick = buf.assumeOrbs;
        SimEnd end;
        int hold = simulate(layer, p, p2, dual, 0, true, window, -1, window, &end);
        buf.skipPress = false;
        if (hold >= window || buf.ghostHitPortal) {
            return;
        }

        int horizon = wave ? fullWaveHorizon(mini) : st(std::clamp(buf.groundHorizon, 48, 240));
        if (dash && (p->m_isDart || p->m_isBird || p->m_isSwing)) {
            horizon = st(48);
        }
        buf.trackMovers = buf.avoidMovers;
        int release = simulateOrbs(layer, p, p2, dual, 0, false, horizon, -1, horizon, &end);
        buf.trackMovers = false;
        if (release < horizon && release < hold + st(10)) {
            return;
        }
        if (buf.avoidMovers && buf.sawMover) {
            return;
        }
        curAutoReleased = true;
        curAutoDash = dash;
        sendButton(layer, false, isP1);
        say("AUTO release{}{}: holding dies in {}t, letting go lives {}t", dash ? " (dash)" : (robot ? " (robot)" : ""),
            isP1 ? "" : " [P2]", hold, release);
    }

    void deliverPendingFor(PlayLayer* layer, PlayerObject* player, bool isP1) {
        auto& q = isP1 ? buf.queue : buf.queueP2;
        auto& curShift = isP1 ? buf.shift : buf.shiftP2;
        auto& curReleaseShift = isP1 ? buf.releaseShift : buf.releaseShiftP2;
        auto& curRealInput = isP1 ? buf.realInput : buf.realInputP2;
        auto& curAppliedInput = isP1 ? buf.appliedInput : buf.appliedInputP2;
        auto& curSinceInput = isP1 ? buf.sinceInput : buf.sinceInputP2;
        auto& curAutoReleased = isP1 ? buf.autoReleased : buf.autoReleasedP2;
        auto& curAutoDash = isP1 ? buf.autoDash : buf.autoDashP2;

        if (curSinceInput <= 998) {
            ++curSinceInput;
        }

        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        bool dual = !twoPlayer && layer->m_gameState.m_isDualMode && layer->m_player2;
        if (!buf.shipBuffer && (player->m_isShip || (dual && layer->m_player2->m_isShip))) {
            q.clear();
            curShift = 0;
            curReleaseShift = 0;
            curAutoReleased = false;
            if (curAppliedInput != curRealInput) {
                sendButton(layer, curRealInput, isP1);
            }
            return;
        }

        bool autoMode = player->m_isDart || player->m_isRobot || (dual && (layer->m_player2->m_isDart || layer->m_player2->m_isRobot));
        if (curAutoReleased && !curAutoDash && !autoMode) {
            curAutoReleased = false;
        }

        if (q.empty()) {
            if (curAppliedInput != curRealInput && !curAutoReleased) {
                sendButton(layer, curRealInput, isP1);
            }
            tryAutoRelease(layer, player, isP1);
            return;
        }

        while (!q.empty() && q.front().ticksLeft <= 0) {
            bool down = q.front().down;
            q.pop_front();
            curReleaseShift = down ? curShift : 0;
            if (down != curAppliedInput) {
                sendButton(layer, down, isP1);
            }
        }
        for (auto& item : q) {
            --item.ticksLeft;
        }
        if (q.empty()) {
            curShift = 0;
        }
    }

    void deliverPending(PlayLayer* layer, PlayerObject* player) {
        if ((buf.avoidMovers || buf.predictMovers) && !watched.empty()) {
            updateWatched();
        }
        deliverPendingFor(layer, player, true);
        bool twoPlayer = layer->m_level && layer->m_level->m_twoPlayerMode;
        if (twoPlayer && buf.twoPlayerSupport && layer->m_gameState.m_isDualMode && layer->m_player2) {
            deliverPendingFor(layer, layer->m_player2, false);
        }
        sweepGhostLayer();
    }
}

// v2.44 in-game menu: F7 in a level. Stage, the main switches,
// status, and shortcuts to all settings / copy log / forget level.
class BufferMenu : public geode::Popup {
    struct Option {
        char const* key;    // mod setting, or nullptr for the runtime path view
        char const* label;
    };
    static constexpr Option OPTIONS[] = {
        { "low-latency", "Low latency" },
        { "robust-saves", "Robust saves" },
        { "orb-align", "Orb alignment" },
        { "orb-timing", "Orb timing" },
        { "level-memory", "Level memory" },
        { "more-time-death-spots", "More time (death spots)" },
        { "ship-buffer", "Ship buffering" },
        { "two-player-mode", "2-Player mode" },
        { "ufo-smart", "Smart UFO" },
        { "ball-smart", "Smart ball" },
        { "swing-smart", "Smart swing" },
        { "cbf-sync", "CBF sync" },
        { "predict-moving-objects", "Predict moving objects" },
        { nullptr, "Path view (F10)" },
        { "training-mode", "Training (hints only)" },
        { "death-heatmap", "Death heatmap" },
        { "save-ding", "Ding on save" },
        { "performance-mode", "Performance mode" },
        { "explain-inputs", "Explain inputs (debug)" },
    };

    struct Theme {
        ccColor3B main, soft, accent, bg;
        char const* button;
    };

public:
    static Theme currentTheme() {
        auto name = Mod::get()->getSettingValue<std::string>("menu-theme");
        if (name == "Blue") {
            return { { 120, 180, 255 }, { 205, 228, 255 }, { 170, 210, 255 }, { 170, 205, 255 }, "GJ_button_06.png" };
        }
        if (name == "Green") {
            return { { 120, 230, 150 }, { 210, 255, 220 }, { 180, 240, 200 }, { 170, 235, 190 }, "GJ_button_01.png" };
        }
        if (name == "Dark") {
            return { { 200, 160, 255 }, { 225, 225, 235 }, { 170, 170, 190 }, { 90, 85, 110 }, "GJ_button_04.png" };
        }
        return { { 255, 150, 205 }, { 255, 205, 230 }, { 215, 190, 255 }, { 255, 195, 235 }, "GJ_button_02.png" };
    }

private:
    Theme m_t = currentTheme();

    CCLabelBMFont* m_stage = nullptr;
    CCLabelBMFont* m_status = nullptr;
    std::vector<CCLabelBMFont*> m_labels;

    static CCSprite* heart(float scale, ccColor3B color) {
        auto* sprite = CCSprite::create("heart.png"_spr);
        sprite->setScale(scale);
        sprite->setColor(color);
        return sprite;
    }

    static void pulse(CCNode* node, float scale) {
        node->runAction(CCRepeatForever::create(CCSequence::create(CCEaseSineInOut::create(CCScaleTo::create(0.45f, scale * 1.18f)),
            CCEaseSineInOut::create(CCScaleTo::create(0.45f, scale)), nullptr)));
    }

    void addFloaters() {
        for (int i = 0; i < 9; ++i) {
            bool sparkle = i % 3 == 2;
            auto* sprite = sparkle ? CCSprite::create("sparkle.png"_spr) : heart(0.3f + 0.05f * (i % 3), i % 2 ? m_t.main : m_t.accent);
            if (sparkle) {
                sprite->setScale(0.35f);
            }
            sprite->setOpacity(0);
            sprite->setPosition({ 20.f + 340.f * ((i * 37) % 100) / 100.f, 20.f });
            sprite->setTag(50 + 10 * (i % 4));
            sprite->runAction(CCSequence::create(CCDelayTime::create(0.7f * i),
                CCCallFuncN::create(this, callfuncN_selector(BufferMenu::loopFloater)), nullptr));
            m_mainLayer->addChild(sprite, -1);
        }
    }

    void loopFloater(CCNode* node) {
        float rise = node->getTag() / 10.f;
        node->runAction(CCRepeatForever::create(CCSequence::create(
            CCSpawn::create(CCMoveBy::create(rise, { 0.f, 270.f }),
                CCSequence::create(CCFadeTo::create(rise * 0.3f, 110), CCDelayTime::create(rise * 0.4f), CCFadeTo::create(rise * 0.3f, 0),
                    nullptr),
                nullptr),
            CCPlace::create(node->getPosition()), nullptr)));
    }

    void colorLabels() {
        for (size_t i = 0; i < m_labels.size(); ++i) {
            m_labels[i]->setColor(optionValue(OPTIONS[i]) ? m_t.soft : ccColor3B { 170, 160, 185 });
        }
    }

    static bool optionValue(Option const& o) {
        if (!o.key) {
            return buf.showPath;
        }
        if (std::string_view(o.key) == "two-player-mode" && !Mod::get()->hasSetting("two-player-mode")) {
            return buf.twoPlayerSupport;
        }
        return Mod::get()->getSettingValue<bool>(o.key);
    }

    void refresh() {
        m_stage->setString(stageText().c_str());
        m_stage->limitLabelWidth(200.f, 0.5f, 0.2f);
        float dt = buf.tickLength > 0.001f ? buf.tickLength : TICK;
        m_status->setString(fmt::format("physics {:.0f}/s  |  CBF {}  |  memory {} spots", 60.f / dt,
            Loader::get()->isModLoaded("syzzi.click_between_frames") ? "on" : "off", memorySpots()).c_str());
        colorLabels();
    }

    void onToggle(CCObject* sender) {
        auto const& o = OPTIONS[sender->getTag()];
        bool value = !optionValue(o);
        if (o.key) {
            if (std::string_view(o.key) == "two-player-mode" && !Mod::get()->hasSetting("two-player-mode")) {
                buf.twoPlayerSupport = value;
            } else {
                Mod::get()->setSettingValue<bool>(o.key, value);
                readSettings();
            }
            saveProfile();
            refreshHeatmap();
            refreshWatermark();
        }
        else {
            buf.showPath = value;
            if (buf.pathNode) {
                buf.pathNode->clear();
            }
        }
        say("{} {}", o.label, value ? "on" : "off");
        refresh();
    }

    void onStage(CCObject* sender) {
        cycleStage(sender->getTag());
        refresh();
    }

    void onAllSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }

    void onCopyLog(CCObject*) {
        exportConsole();
    }

    void onPreset(CCObject* sender) {
        auto* mod = Mod::get();
        int which = sender->getTag();
        int stage = which == 0 ? 2 : which == 1 ? 3 : 5;
        mod->setSettingValue<bool>("robust-saves", true);
        mod->setSettingValue<bool>("wave-safety-margin", which != 2);
        mod->setSettingValue<bool>("more-time-death-spots", which != 0);
        mod->setSettingValue<bool>("low-latency", false);
        buf.stage = stage;
        mod->setSavedValue("stage", buf.stage);
        resetPending();
        applyStage();
        readSettings();
        refreshWatermark();
        refreshPanel();
        say("preset: {} -> {}", which == 0 ? "Safe" : which == 1 ? "Balanced" : "Aggressive", stageText());
        refresh();
    }

    void onRunCheck(CCObject*) {
        benchPending = true;
        say("run check: runs at the start of your next attempt from the beginning (restart the level)");
        FLAlertLayer::create("Run check",
            "Restart the level from the beginning: your longest recorded run is replayed on the hidden player and the "
            "result shows in the console (F9) and in Stats.",
            "OK")->show();
    }

    void onStats(CCObject*) {
        FLAlertLayer::create("Practice stats", statsText(), "OK")->show();
    }

    void onForget(CCObject*) {
        memory.clear();
        pending.active = false;
        memoryDirty = true;
        saveMemory();
        refreshHeatmap();
        say("memory for this level cleared");
        refresh();
    }

    CCMenuItemSpriteExtra* textButton(char const* text, SEL_MenuHandler handler, float scale = 0.6f) {
        auto* sprite = ButtonSprite::create(text, "bigFont.fnt", m_t.button, 0.8f);
        sprite->setScale(scale);
        return CCMenuItemSpriteExtra::create(sprite, this, handler);
    }

protected:
    bool init() {
        if (!Popup::init(380.f, 412.f, "GJ_square04.png")) {
            return false;
        }
        m_bgSprite->setColor(m_t.bg);
        this->setTitle("Click Before Frames");
        if (m_title) {
            m_title->setColor(m_t.soft);
        }
        addFloaters();

        for (float side : { -1.f, 1.f }) {
            auto* h = heart(0.32f, m_t.main);
            h->setRotation(side * 15.f);
            m_mainLayer->addChildAtPosition(h, Anchor::Top, { side * 118.f, -20.f });
            pulse(h, 0.32f);
            auto* sp = CCSprite::create("sparkle.png"_spr);
            sp->setScale(0.25f);
            sp->setColor(m_t.soft);
            m_mainLayer->addChildAtPosition(sp, Anchor::Top, { side * 134.f, -10.f });
            sp->runAction(CCRepeatForever::create(CCRotateBy::create(3.f, side * 360.f)));
        }

        auto* gm = GameManager::get();
        auto* icon = SimplePlayer::create(gm->getPlayerFrame());
        icon->updatePlayerFrame(gm->getPlayerFrame(), IconType::Cube);
        icon->setColor(gm->colorForIdx(gm->getPlayerColor()));
        icon->setSecondColor(gm->colorForIdx(gm->getPlayerColor2()));
        icon->setScale(0.7f);
        m_mainLayer->addChildAtPosition(icon, Anchor::TopLeft, { 34.f, -30.f });
        icon->runAction(CCRepeatForever::create(CCSequence::create(CCEaseSineInOut::create(CCMoveBy::create(0.8f, { 0.f, 3.f })),
            CCEaseSineInOut::create(CCMoveBy::create(0.8f, { 0.f, -3.f })), nullptr)));
        auto* iconHeart = heart(0.2f, m_t.main);
        m_mainLayer->addChildAtPosition(iconHeart, Anchor::TopLeft, { 52.f, -16.f });
        pulse(iconHeart, 0.2f);

        auto* prev = CCMenuItemSpriteExtra::create(CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"), this,
            menu_selector(BufferMenu::onStage));
        prev->setTag(-1);
        prev->setScale(0.6f);
        prev->m_baseScale = 0.6f;
        m_buttonMenu->addChildAtPosition(prev, Anchor::Top, { -130.f, -52.f });
        auto* nextSprite = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        nextSprite->setFlipX(true);
        auto* next = CCMenuItemSpriteExtra::create(nextSprite, this, menu_selector(BufferMenu::onStage));
        next->setTag(1);
        next->setScale(0.6f);
        next->m_baseScale = 0.6f;
        m_buttonMenu->addChildAtPosition(next, Anchor::Top, { 130.f, -52.f });
        m_stage = CCLabelBMFont::create("", "bigFont.fnt");
        m_mainLayer->addChildAtPosition(m_stage, Anchor::Top, { 0.f, -52.f });

        constexpr int ROWS = 10;
        for (int i = 0; i < static_cast<int>(std::size(OPTIONS)); ++i) {
            int col = i / ROWS;
            int row = i % ROWS;
            CCPoint at { col == 0 ? -165.f : 15.f, -86.f - row * 23.f };
            auto* toggler = CCMenuItemToggler::createWithStandardSprites(this, menu_selector(BufferMenu::onToggle), 0.55f);
            toggler->setTag(i);
            toggler->toggle(optionValue(OPTIONS[i]));
            m_buttonMenu->addChildAtPosition(toggler, Anchor::Top, at);
            auto* label = CCLabelBMFont::create(OPTIONS[i].label, "bigFont.fnt");
            label->setAnchorPoint({ 0.f, 0.5f });
            label->limitLabelWidth(140.f, 0.38f, 0.2f);
            m_mainLayer->addChildAtPosition(label, Anchor::Top, at + CCPoint { 16.f, 0.f });
            m_labels.push_back(label);
        }

        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setScale(0.55f);
        m_status->setColor(m_t.accent);
        m_mainLayer->addChildAtPosition(m_status, Anchor::Bottom, { 0.f, 56.f });
        m_buttonMenu->addChildAtPosition(textButton("Settings", menu_selector(BufferMenu::onAllSettings), 0.55f), Anchor::Bottom,
            { -132.f, 26.f });
        m_buttonMenu->addChildAtPosition(textButton("Stats", menu_selector(BufferMenu::onStats), 0.55f), Anchor::Bottom, { -45.f, 26.f });
        m_buttonMenu->addChildAtPosition(textButton("Copy log", menu_selector(BufferMenu::onCopyLog), 0.55f), Anchor::Bottom,
            { 37.f, 26.f });
        m_buttonMenu->addChildAtPosition(textButton("Forget", menu_selector(BufferMenu::onForget), 0.55f), Anchor::Bottom,
            { 124.f, 26.f });

        char const* presets[] = { "Safe", "Balanced", "Aggressive" };
        for (int i = 0; i < 3; ++i) {
            auto* button = textButton(presets[i], menu_selector(BufferMenu::onPreset), 0.5f);
            button->setTag(i);
            m_buttonMenu->addChildAtPosition(button, Anchor::Bottom, { -132.f + 82.f * i, 82.f });
        }
        m_buttonMenu->addChildAtPosition(textButton("Run check", menu_selector(BufferMenu::onRunCheck), 0.5f), Anchor::Bottom,
            { 124.f, 82.f });

        refresh();
        return true;
    }

public:
    void show() override {
        Popup::show();
        m_mainLayer->stopAllActions();
        m_mainLayer->setScale(0.1f);
        m_mainLayer->runAction(CCEaseElasticOut::create(CCScaleTo::create(0.5f, MENU_SCALE), 0.6f));
    }

    static constexpr float MENU_SCALE = 0.78f;

    static BufferMenu* create() {
        auto* ret = new BufferMenu();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    static void open() {
        if (auto* menu = create()) {
            menu->show();
        }
    }
};

class $modify(BufEndLevelLayer, EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();
        if (!buf.usedThisAttempt) {
            return;
        }
        benchFinish();
        endAttempt();
    }
};

$on_mod(Loaded) {
    buf.console = Mod::get()->getSettingValue<bool>("debug-console");
    buf.showPath = Mod::get()->getSettingValue<bool>("show-path");
    recoverGlobals();
    buf.stage = std::clamp(static_cast<int>(Mod::get()->getSavedValue<int64_t>("stage", 0)), 0, STAGE_COUNT - 1);
    if (!Mod::get()->getSavedValue<bool>("per-level-v262", false)) {
        Mod::get()->setSettingValue<bool>("per-level-settings", false);
        Mod::get()->setSavedValue("per-level-v262", true);
    }
    if (!Mod::get()->getSavedValue<bool>("low-latency-v260", false)) {
        Mod::get()->setSettingValue<bool>("low-latency", false);
        Mod::get()->setSavedValue("low-latency-v260", true);
    }
    readSettings();
    applyStage();
    if (buf.console) {
        AllocConsole();
        FILE* out = nullptr;
        freopen_s(&out, "CONOUT$", "w", stdout);
        SetConsoleTitleA("Click Before Frames - Live Console");
    }
    if (Loader::get()->isModLoaded("syzzi.click_between_frames")) {
        say("Click Between Frames detected: mid-tick sync {}", buf.cbfSync ? "on" : "off");
    }
    if (Loader::get()->isModLoaded("square3ang.cbfextrapolate")) {
        say("CBF Extrapolate detected: visual only, predictions unaffected");
    }
    say("v2.76 loaded. F6 cycles: 0.5f -> 1f -> 2f -> 4f -> 6f -> 8f -> custom -> off");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (buf.simulating && isGhost(player)) {
            buf.ghostDied = true;
            if (buf.captureKiller && !buf.killerSeen) {
                buf.killerSeen = true;
                buf.killerTick = buf.simTick;
                buf.ghostDeathRect = player->getObjectRect();
                if (object) {
                    buf.killerId = object->m_objectID;
                    buf.killerType = static_cast<int>(object->m_objectType);
                    buf.killerNoTouch = object->m_isNoTouch;
                    buf.killerDisabled = object->m_isDisabled;
                    buf.killerRect = object->getObjectRect();
                }
                else {
                    buf.killerId = 0;
                    buf.killerType = -1;
                }
            }
            return;
        }
        bool wasDead = player && player->m_isDead;
        PlayLayer::destroyPlayer(player, object);
        if (!player || wasDead || !player->m_isDead) {
            return;
        }
        resetPending();
        buf.appliedInput = false;
        buf.appliedInputP2 = false;
        buf.autoReleased = false;
        buf.autoReleasedP2 = false;
        buf.rapid = 0;
        buf.rapidP2 = 0;
        forgetPress();
        memoryDeath();
        benchFinish();
        refreshHeatmap();
        endAttempt();
        finishDrift(" (died)");
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        dropGhosts();
        readSettings();
        applyStage();
        exportLines.clear();
        logClock = 0;
        loadMemory(level);
        if (statsKey != memoryKey) {
            statsKey = memoryKey;
            sessionStats = {};
        }
        attemptStats = {};
        benchNeedStart = true;
        benchRecording = false;
        loadProfile();
        buf.ghost1 = makeGhost(this);
        buf.ghost2 = makeGhost(this);
        buf.ghostLayerBase = buf.ghostLayer->getChildrenCount();

        constexpr float W = PANEL_W;
        constexpr float H = PANEL_H;
        auto win = CCDirector::get()->getWinSize();
        buf.panel = CCNode::create();
        buf.panel->setPosition({ 8.f, win.height - H - 8.f });
        buf.panel->setZOrder(1000);

        auto* stencil = CCLayerColor::create({ 255, 255, 255, 255 }, W, H);
        auto* clip = CCClippingNode::create(stencil);
        if (auto* art = CCSprite::create("game_bg_01_001.png")) {
            auto size = art->getContentSize();
            art->setScale(std::max(W / size.width, H / size.height));
            art->setPosition({ W / 2.f, H / 2.f });
            art->setColor({ 40, 120, 255 });
            art->setOpacity(150);
            clip->addChild(art);
        }
        auto* shade = CCLayerGradient::create({ 0, 0, 0, 50 }, { 0, 0, 0, 140 }, { 0.f, -1.f });
        shade->setContentSize({ W, H });
        clip->addChild(shade);
        auto* header = CCLayerColor::create({ 0, 0, 0, 80 }, W, 28.f);
        header->setPosition({ 0.f, H - 28.f });
        clip->addChild(header);
        auto* rule = CCLayerColor::create({ 255, 255, 255, 45 }, W - 16.f, 1.f);
        rule->setPosition({ 8.f, H - 46.f });
        clip->addChild(rule);
        buf.panel->addChild(clip);

        if (auto* frame = CCScale9Sprite::create("GJ_square07.png")) {
            frame->setContentSize({ W, H });
            frame->setPosition({ W / 2.f, H / 2.f });
            frame->setOpacity(190);
            buf.panel->addChild(frame);
        }

        auto* title = CCLabelBMFont::create("Click Before Frames", "goldFont.fnt");
        title->setScale(0.55f);
        title->setAnchorPoint({ 0.f, 0.5f });
        title->setPosition({ 10.f, H - 14.f });
        buf.panel->addChild(title);

        auto* version = CCLabelBMFont::create(Mod::get()->getVersion().toVString().c_str(), "chatFont.fnt");
        version->setScale(0.45f);
        version->setOpacity(150);
        version->setAnchorPoint({ 1.f, 0.5f });
        version->setPosition({ W - 10.f, H - 14.f });
        buf.panel->addChild(version);

        buf.panelTitle = CCLabelBMFont::create("", "bigFont.fnt");
        buf.panelTitle->setScale(0.4f);
        buf.panelTitle->setAnchorPoint({ 0.f, 0.5f });
        buf.panelTitle->setPosition({ 10.f, H - 37.f });
        buf.panel->addChild(buf.panelTitle);

        buf.panelLines.clear();
        for (size_t i = 0; i < PANEL_LINES; ++i) {
            auto* line = CCLabelBMFont::create("", "chatFont.fnt");
            line->setScale(0.55f);
            line->setAnchorPoint({ 0.f, 0.5f });
            line->setPosition({ 10.f, H - 57.f - i * 13.f });
            buf.panel->addChild(line);
            buf.panelLines.push_back(line);
        }

        auto* hint = CCLabelBMFont::create("F6 stage  F7 menu  F8 copy log  F9 console  F10 path  F11 forget", "chatFont.fnt");
        hint->setScale(0.45f);
        hint->setColor({ 160, 190, 255 });
        hint->setAnchorPoint({ 0.5f, 0.5f });
        hint->setPosition({ W / 2.f, 9.f });
        buf.panel->addChild(hint);

        this->addChild(buf.panel);
        refreshPanel();

        buf.watermark = nullptr;
        buf.autoTag = nullptr;
        buf.hintLabel = nullptr;
        buf.heatNode = nullptr;
        buf.usedThisAttempt = false;

        buf.pathNode = CCDrawNode::create();
        (m_aboveShaderObjectLayer ? m_aboveShaderObjectLayer : m_objectLayer)->addChild(buf.pathNode, 9999);

        buf.realInput = false;
        buf.appliedInput = false;
        buf.realInputP2 = false;
        buf.appliedInputP2 = false;
        buf.isPlayer1 = true;
        buf.sinceInput = 999;
        buf.sinceInputP2 = 999;
        buf.rapid = 0;
        buf.rapidP2 = 0;
        forgetPress();
        return true;
    }

    void onQuit() {
        benchFinish();
        benchRecording = false;
        saveMemory();
        saveProfile();
        restoreGlobals();
        buf.hintLabel = nullptr;
        buf.heatNode = nullptr;
        memoryKey.clear();
        dropGhosts();
        PlayLayer::onQuit();
    }

    void resetLevel() {
        benchFinish();
        benchNeedStart = true;
        endAttempt();
        buf.usedThisAttempt = false;
        resetPending();
        forgetPress();
        refreshAutoTag();
        if (buf.pathNode) {
            buf.pathNode->clear();
        }
        buf.autoReleased = false;
        buf.autoReleasedP2 = false;
        drift.active = false;
        PlayLayer::resetLevel();
    }

    void togglePracticeMode(bool practiceMode) {
        PlayLayer::togglePracticeMode(practiceMode);
        refreshWatermark();
        refreshAutoTag();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (buf.simulating) {
            return;
        }
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
        if (buf.simulating) {
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
        if (buf.simulating) {
            return;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(BufGameObject, GameObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GameObject::playShineEffect", HOOK_PRIORITY);
    }

    void playShineEffect() {
        if (buf.simulating) {
            return;
        }
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
        if (layer && this == layer->m_player1) {
            ++tickCount;
            ++logClock;
            if (buf.enabled) {
                buf.usedThisAttempt = true;
            }
            if (buf.panelDirty) {
                buf.panelDirty = false;
                refreshPanel();
            }
            if (benchNeedStart) {
                benchNeedStart = false;
                if (benchPending) {
                    benchPending = false;
                    benchCheck(layer, this);
                }
                benchStart(layer, this);
            }
            driftStep(this);
            memoryTick();
            if (buf.enabled) {
                deliverPending(layer, this);
                livePath(layer, this);
            }
        }

        bool isP1 = layer && this == layer->m_player1;
        bool isP2 = layer && this == layer->m_player2;
        if (isP1) {
            buf.tickStartX = getPosition().x;
            buf.inRealUpdate = true;
        } else if (isP2) {
            buf.tickStartXP2 = getPosition().x;
            buf.inRealUpdateP2 = true;
        }
        PlayerObject::update(dt);
        if (isP1) {
            benchStep(this);
            buf.inRealUpdate = false;
            float dx = getPosition().x - buf.tickStartX;
            if (std::abs(dx) > 0.01f) {
                buf.tickDx = dx;
            }
            if (dt > 0.f) {
                float avg = buf.tickLength;
                buf.tickLength = (avg > 0.f && std::abs(dt - avg) < avg * 0.3f) ? avg * 0.9f + dt * 0.1f : dt;
            }
        } else if (isP2) {
            buf.inRealUpdateP2 = false;
            float dx = getPosition().x - buf.tickStartXP2;
            if (std::abs(dx) > 0.01f) {
                buf.tickDxP2 = dx;
            }
        }
    }

    void incrementJumps() {
        if (buf.simulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    bool ghostEffect() {
        return buf.simulating && isGhost(this);
    }

    void spawnCircle() {
        if (!ghostEffect()) PlayerObject::spawnCircle();
    }

    void spawnDualCircle() {
        if (!ghostEffect()) PlayerObject::spawnDualCircle();
    }

    void spawnPortalCircle(ccColor3B color, float startRadius) {
        if (!ghostEffect()) PlayerObject::spawnPortalCircle(color, startRadius);
    }

    void spawnScaleCircle() {
        if (!ghostEffect()) PlayerObject::spawnScaleCircle();
    }

    void playBumpEffect(int objectType, GameObject* object) {
        if (!ghostEffect()) PlayerObject::playBumpEffect(objectType, object);
    }

    void playSpawnEffect() {
        if (!ghostEffect()) PlayerObject::playSpawnEffect();
    }

    void createFadeOutDartStreak() {
        if (!ghostEffect()) PlayerObject::createFadeOutDartStreak();
    }

    void fadeOutStreak2(float duration) {
        if (!ghostEffect()) PlayerObject::fadeOutStreak2(duration);
    }

    void playDynamicSpiderRun() {
        if (!ghostEffect()) PlayerObject::playDynamicSpiderRun();
    }

    void animatePlatformerJump(float scale) {
        if (!ghostEffect()) PlayerObject::animatePlatformerJump(scale);
    }

    void runBallRotation(float speed) {
        if (!ghostEffect()) PlayerObject::runBallRotation(speed);
    }

    void runBallRotation2() {
        if (!ghostEffect()) PlayerObject::runBallRotation2();
    }

    void runNormalRotation(bool notNormalMode, float speed) {
        if (!ghostEffect()) PlayerObject::runNormalRotation(notNormalMode, speed);
    }

    void toggleGhostEffect(GhostType type) {
        if (!ghostEffect()) PlayerObject::toggleGhostEffect(type);
    }

    void activateStreak() {
        if (!ghostEffect()) PlayerObject::activateStreak();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (buf.simulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(this)) {
                return;
            }
            if (buf.orbsUsed.contains(ring)) {
                return;
            }
            snapshotObject(ring);
            double vy = m_yVelocity;
            bool upsideDown = m_isUpsideDown;
            bool dashing = m_isDashing;
            PlayerObject::ringJump(ring, skipCheck);
            if (m_yVelocity != vy || m_isUpsideDown != upsideDown || m_isDashing != dashing) {
                buf.orbsUsed.insert(ring);
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
            buf.orbsUsed.insert(ring);
            if (buf.orbUsedTick < 0) {
                buf.orbUsedTick = buf.simTick;
            }
        }
        PlayerObject::startDashing(ring);
    }
};

class $modify(BufKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat && GameManager::get()->m_playLayer) {
            int k = static_cast<int>(key);
            auto is = [&](int i) { return buf.keys[i] != 0 && k == buf.keys[i]; };
            if (is(0)) {
                cycleStage();
            }
            else if (is(1)) {
                auto* layer = GameManager::get()->m_playLayer;
                if (layer && !layer->m_isPaused) {
                    layer->pauseGame(true);
                }
                BufferMenu::open();
            }
            else if (is(3)) {
                buf.panelOpen = !buf.panelOpen;
                refreshPanel();
            }
            else if (is(2)) {
                exportConsole();
            }
            else if (is(5)) {
                memory.clear();
                pending.active = false;
                memoryDirty = true;
                saveMemory();
                say("memory for this level cleared");
            }
            else if (is(4)) {
                buf.showPath = !buf.showPath;
                if (buf.pathNode) {
                    buf.pathNode->clear();
                }
                say("path view {}", buf.showPath ? "on" : "off");
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
            if (!buf.smart || !isGhost(player) || !object) {
                return false;
            }
            if (typeinfo_cast<RingObject*>(object)) {
                if (buf.orbsUsed.contains(object)) {
                    return false;
                }
                snapshotObject(object);
                return true;
            }
            if (buf.simUsed.contains(object)) {
                return false;
            }
            snapshotObject(object);
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

        uint64_t keep = buf.smart ? (COLLIDING_TYPES | INTERACT_TYPES) : COLLIDING_TYPES;
        bool haveRect = false;
        CCRect ghostRect;
        auto& list = *objects;
        for (int i = 0; i < objectCount; ++i) {
            auto* object = list[i];
            if (!object) {
                continue;
            }
            auto type = static_cast<unsigned>(object->m_objectType);
            uint64_t bit = type < 64 ? (1ull << type) : 0;
            if (keep & bit) {
                if (!buf.simDisabled.empty() && buf.simDisabled.contains(object)) {
                    continue;
                }
                if (buf.haveRealRect && (bit & HAZARD_TYPES)) {
                    if (!buf.harmless.empty() && buf.harmless.contains(object)) {
                        continue;
                    }
                    if (buf.simTick <= 1) {
                        auto const& r = object->getObjectRect();
                        if (r.intersectsRect(buf.realRect1) || r.intersectsRect(buf.realRect2)) {
                            buf.harmless.insert(object);
                            continue;
                        }
                    }
                }
                relevant.push_back(object);
                if (buf.trackMovers && (COLLIDING_TYPES & bit)) {
                    watchObject(object);
                }
                continue;
            }
            if (!buf.smart) {
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
                }
                else if (size) {
                    bool mini = object->m_objectType == GameObjectType::MiniSizePortal;
                    if ((player->m_vehicleSize < 1.f) != mini) {
                        player->togglePlayerScale(mini, true);
                    }
                }
                else {
                    player->m_playerSpeed = speed;
                }
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!buf.simulating && button == 1 && !buf.injecting) {
            bool twoPlayer = m_level && m_level->m_twoPlayerMode;
            auto* targetPlayer = (twoPlayer && !isPlayer1 && m_gameState.m_isDualMode && m_player2) ? m_player2 : m_player1;
            if (down || (targetPlayer && (targetPlayer->m_isDart || targetPlayer->m_isRobot))) {
                finishDrift("");
            }
            recordRealInput(down, targetPlayer);
        }
        if (!buf.simulating && button == 1 && !buf.injecting) {
            buf.simRuns = 0;
            buf.simTicks = 0;
            auto start = std::chrono::steady_clock::now();
            bool held = intercept(this, down, isPlayer1);
            sweepGhostLayer();
            if (buf.perfLog && buf.simRuns > 0) {
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
                say("perf: {:.2f} ms, {} runs, {} ghost ticks ({:.1f} us/tick)", ms, buf.simRuns, buf.simTicks,
                    ms * 1000.0 / static_cast<double>(std::max(1L, buf.simTicks)));
            }
            if (held) {
                return;
            }
        }
        if (!buf.simulating && button == 1) {
            benchInput(down);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (buf.simulating) {
            if (!buf.smart || !isGhost(player)) {
                return;
            }
            snapshotObject(ring);
            buf.touchedOrb = true;
            if (buf.firstOrbTick < 0) {
                buf.firstOrbTick = buf.simTick;
            }
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);

        if (buf.simulating && buf.orbClick) {
            bool first = player == buf.ghost1;
            bool& held = first ? buf.held1 : buf.held2;
            bool& autoClick = first ? buf.auto1 : buf.auto2;
            int& autoAt = first ? buf.autoAt1 : buf.autoAt2;
            if (buf.orbClickDelay > 0) {
                if (!held && autoAt == -1) {
                    autoAt = buf.simTick + buf.orbClickDelay;
                }
            }
            else if (!held && !autoClick) {
                player->pushButton(PlayerButton::Jump);
                autoClick = true;
            }
        }
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (buf.simulating) {
            if (buf.smart && isGhost(player)) {
                player->flipGravity(flip, true);
            }
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void toggleGroupTriggered(int group, bool activate, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (buf.simulating) {
            if (!activate) {
                if (auto* objects = this->getGroup(group)) {
                    for (auto* object : CCArrayExt<GameObject*>(objects)) {
                        buf.simDisabled.insert(object);
                    }
                }
            }
            return;
        }
        GJBaseGameLayer::toggleGroupTriggered(group, activate, remapKeys, triggerID, controlID);
    }

    void spawnGroup(int group, bool ordered, double delay, gd::vector<int> const& remapKeys, int triggerID, int controlID) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::spawnGroup(group, ordered, delay, remapKeys, triggerID, controlID);
    }

    void toggleGroup(int id, bool activate) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::toggleGroup(id, activate);
    }

    void pickupItem(EffectGameObject* object) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::pickupItem(object);
    }

    void updateCounters(int itemId, int value) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::updateCounters(itemId, value);
    }

    void activateItemEditTrigger(ItemTriggerGameObject* object) {
        if (buf.simulating) {
            return;
        }
        GJBaseGameLayer::activateItemEditTrigger(object);
    }

    void activateItemCompareTrigger(ItemTriggerGameObject* object, gd::vector<int> const& remapKeys) {
        if (buf.simulating) {
            return;
        }
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

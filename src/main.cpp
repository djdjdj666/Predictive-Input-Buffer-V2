// Predictive Input Buffer v3 (Ultimate Hybrid)
// Merged v2 Smart Prediction with v4.6 Wave Math & Spam Guard + Experimental Ship Support.

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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace {
    constexpr int HOOK_PRIORITY = -1000000;
    constexpr float TICK = 0.25f;
    constexpr size_t PANEL_LINES = 8;
    constexpr int SPAM_INTERVAL = 8; // Spam guard threshold

    constexpr uint64_t typeBit(GameObjectType type) {
        return 1ull << static_cast<int>(type);
    }

    constexpr uint64_t COLLIDING_TYPES = typeBit(GameObjectType::Solid) | typeBit(GameObjectType::Hazard) | typeBit(GameObjectType::Slope) |
                                         typeBit(GameObjectType::CollisionObject) | typeBit(GameObjectType::AnimatedHazard);

    constexpr uint64_t INTERACT_TYPES =
        typeBit(GameObjectType::YellowJumpPad) | typeBit(GameObjectType::PinkJumpPad) | typeBit(GameObjectType::RedJumpPad) |
        typeBit(GameObjectType::GravityPad) | typeBit(GameObjectType::SpiderPad) | typeBit(GameObjectType::YellowJumpRing) |
        typeBit(GameObjectType::PinkJumpRing) | typeBit(GameObjectType::RedJumpRing) | typeBit(GameObjectType::GravityRing) |
        typeBit(GameObjectType::GreenRing) | typeBit(GameObjectType::DropRing) | typeBit(GameObjectType::CustomRing) |
        typeBit(GameObjectType::DashRing) | typeBit(GameObjectType::GravityDashRing) | typeBit(GameObjectType::SpiderOrb) |
        typeBit(GameObjectType::InverseGravityPortal) | typeBit(GameObjectType::NormalGravityPortal) |
        typeBit(GameObjectType::GravityTogglePortal);

    constexpr uint64_t STOP_TYPES =
        typeBit(GameObjectType::ShipPortal) | typeBit(GameObjectType::CubePortal) | typeBit(GameObjectType::BallPortal) |
        typeBit(GameObjectType::UfoPortal) | typeBit(GameObjectType::WavePortal) | typeBit(GameObjectType::RobotPortal) |
        typeBit(GameObjectType::SpiderPortal) | typeBit(GameObjectType::SwingPortal) | typeBit(GameObjectType::DualPortal) |
        typeBit(GameObjectType::SoloPortal) | typeBit(GameObjectType::RegularSizePortal) | typeBit(GameObjectType::MiniSizePortal) |
        typeBit(GameObjectType::InverseMirrorPortal) | typeBit(GameObjectType::NormalMirrorPortal) |
        typeBit(GameObjectType::TeleportPortal) | typeBit(GameObjectType::TeleportOrb);

    bool hasType(uint64_t mask, GameObject* object) {
        auto type = static_cast<unsigned>(object->m_objectType);
        return type < 64 && ((mask >> type) & 1);
    }

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

        CCNode* panel = nullptr;
        CCLabelBMFont* panelTitle = nullptr;
        CCLabelBMFont* panelLog = nullptr;
        bool panelOpen = false; 
        std::deque<std::string> logLines;

        bool simulating = false;
        bool injecting = false;
        bool ghostDied = false;
        bool ghostHitPortal = false;
        bool realInput = false;
        bool appliedInput = false;
        bool isPlayer1 = true;

        bool enabled = true;
        int stage = 0;
        int ticksAll = 2;
        int ticksWave = 2;

        std::deque<QueuedInput> queue;
        int shift = 0; 

        float tickLength = TICK;
        int sinceInput = 999;
        int rapid = 0;

        bool smart = true;
        bool console = false;
        double budgetMs = 6.0;

        std::unordered_set<GameObject*> simUsed;
        std::vector<ObjSnapshot> snapshots;
    } buf;

    std::string stageText() {
        if (!buf.enabled) return "Buffer: OFF";
        return fmt::format("Buffer{}: {:g}f / wave {:g}f", STAGES[buf.stage].custom ? " (custom)" : "", buf.ticksAll * TICK, buf.ticksWave * TICK);
    }

    void refreshPanel() {
        if (!buf.panel) return;
        buf.panel->setVisible(buf.panelOpen);
        if (!buf.panelOpen) return;
        buf.panelTitle->setString(stageText().c_str());
        std::string text = "F6: change stage    K: hide\n";
        for (auto const& line : buf.logLines) {
            text += line + '\n';
        }
        buf.panelLog->setString(text.c_str());
    }

    template <class... Args>
    void say(fmt::format_string<Args...> f, Args&&... args) {
        auto msg = fmt::format(f, std::forward<Args>(args)...);
        buf.logLines.push_back(msg);
        while (buf.logLines.size() > PANEL_LINES) buf.logLines.pop_front();
        if (buf.console) std::printf("[InputBuffer v3] %s\n", msg.c_str());
        if (buf.panelOpen) refreshPanel();
    }

    bool isGhost(PlayerObject* p) { return p && (p == buf.ghost1 || p == buf.ghost2); }

    void resetPending() { buf.queue.clear(); buf.shift = 0; }

    void readSettings() {
        auto* mod = Mod::get();
        buf.smart = mod->getSettingValue<bool>("smart-prediction");
        buf.budgetMs = mod->getSettingValue<double>("sim-budget-ms");
    }

    void applyStage() {
        auto const& s = STAGES[buf.stage];
        buf.enabled = s.enabled;
        if (s.custom) {
            buf.ticksAll = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-ticks"));
            buf.ticksWave = static_cast<int>(Mod::get()->getSettingValue<int64_t>("custom-wave-ticks"));
        } else {
            buf.ticksAll = s.ticksAll;
            buf.ticksWave = s.ticksWave;
        }
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
        buf.panel = nullptr; buf.panelTitle = nullptr; buf.panelLog = nullptr;
        buf.simulating = false; buf.injecting = false; buf.rapid = 0;
        resetPending();
    }

    PlayerObject* makeGhost(PlayLayer* layer) {
        if (!buf.ghostLayer) { buf.ghostLayer = CCLayer::create(); buf.ghostLayer->retain(); }
        auto* ghost = PlayerObject::create(1, 1, layer, buf.ghostLayer, true);
        ghost->retain(); ghost->setPosition(CCPoint(0.f, 105.f)); ghost->setVisible(false);
        buf.ghostLayer->addChild(ghost);
        return ghost;
    }

    void sendButton(PlayLayer* layer, bool down) {
        buf.injecting = true; buf.appliedInput = down;
        layer->handleButton(down, 1, buf.isPlayer1);
        buf.injecting = false;
    }

    void pressGhost(PlayerObject* ghost, bool down) {
        if (down) ghost->pushButton(PlayerButton::Jump); else ghost->releaseButton(PlayerButton::Jump);
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->setVisible(false); ghost->resetTouchedRings(true);
        ghost->copyAttributes(real); ghost->setScale(real->getScale()); ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width; ghost->m_height = real->m_height; ghost->m_gravityMod = real->m_gravityMod;
        ghost->m_isOnGround = real->m_isOnGround; ghost->m_isUpsideDown = real->m_isUpsideDown;
        ghost->m_vehicleSize = real->m_vehicleSize; ghost->m_playerSpeed = real->m_playerSpeed;
        ghost->m_isDashing = real->m_isDashing; ghost->m_lastActivatedPortal = real->m_lastActivatedPortal;
        ghost->m_holdingButtons = real->m_holdingButtons; ghost->m_jumpBuffered = real->m_jumpBuffered;
        ghost->m_wasJumpBuffered = real->m_wasJumpBuffered; ghost->m_stateJumpBuffered = real->m_stateJumpBuffered;
        ghost->m_stateRingJump = real->m_stateRingJump; ghost->m_stateRingJump2 = real->m_stateRingJump2;
        ghost->m_wasRobotJump = real->m_wasRobotJump; ghost->m_touchedRing = real->m_touchedRing;
        ghost->m_touchedPad = real->m_touchedPad; ghost->m_touchedGravityPortal = real->m_touchedGravityPortal;
    }

    constexpr int HOLD_SAMPLES = 8;
    int holdSamples[HOLD_SAMPLES] = {};
    int holdCount = 0, holdIndex = 0, tickCount = 0, pressTick = -1;

    void recordRealInput(bool down) {
        if (down) { pressTick = tickCount; return; }
        if (pressTick < 0) return;
        int held = std::clamp(tickCount - pressTick, 1, 240); pressTick = -1;
        holdSamples[holdIndex] = held; holdIndex = (holdIndex + 1) % HOLD_SAMPLES;
        holdCount = std::min(holdCount + 1, HOLD_SAMPLES);
    }

    int holdEstimate() {
        if (holdCount == 0) return 12;
        int tmp[HOLD_SAMPLES]; std::copy(holdSamples, holdSamples + holdCount, tmp);
        std::nth_element(tmp, tmp + holdCount / 2, tmp + holdCount);
        return tmp[holdCount / 2];
    }

    void snapshotObject(GameObject* object) {
        if (!object || buf.simUsed.contains(object)) return;
        buf.simUsed.insert(object);
        ObjSnapshot s { object, object->m_isActivated, nullptr, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect; s.activated = effect->m_activated; s.byP1 = effect->m_activatedByPlayer1; s.byP2 = effect->m_activatedByPlayer2;
        }
        buf.snapshots.push_back(s);
    }

    void restoreObjects() {
        for (auto const& s : buf.snapshots) {
            s.object->m_isActivated = s.isActivated;
            if (s.effect) { s.effect->m_activated = s.activated; s.effect->m_activatedByPlayer1 = s.byP1; s.effect->m_activatedByPlayer2 = s.byP2; }
        }
        buf.snapshots.clear(); buf.simUsed.clear();
    }

    void advanceGhost(PlayerObject* ghost, float dt) { ghost->update(dt); ghost->updateRotation(dt); }
    void checkGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->m_collisionLogTop->removeAllObjects(); ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects(); ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
    }

    // Ship added to isFlying for proper simulation (v3)
    bool isFlying(PlayerObject* player) { return player->m_isDart || player->m_isBird || player->m_isSwing || player->m_isShip; }

    enum class SimEnd { Ok, StopSearch };

    int simulate(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, int delay, bool down, int horizon, int releaseAfter, int stopAt, SimEnd* end) {
        *end = SimEnd::Ok;
        if (!buf.ghost1) return horizon;
        buf.simulating = true; buf.ghostDied = false; buf.ghostHitPortal = false;
        bool second = dual && buf.ghost2 && p2;
        copyState(buf.ghost1, p1); if (second) copyState(buf.ghost2, p2);

        float dt = buf.tickLength; if (!(dt > 0.001f && dt < 2.0f)) dt = TICK;
        bool flying = isFlying(p1) || (dual && p2 && isFlying(p2));
        int total = delay + horizon, survived = 0, airTicks = 0, groundTicks = 0, tick = 0;
        bool died = false, safe = false;

        for (; tick < total; ++tick) {
            if (tick == delay) { pressGhost(buf.ghost1, down); if (second) pressGhost(buf.ghost2, down); }
            if (releaseAfter >= 0 && tick == delay + releaseAfter) { pressGhost(buf.ghost1, false); if (second) pressGhost(buf.ghost2, false); }
            advanceGhost(buf.ghost1, dt); if (second) advanceGhost(buf.ghost2, dt);
            if (buf.ghostDied) { died = true; break; }
            checkGhost(layer, buf.ghost1, dt); if (!buf.ghostDied && second) checkGhost(layer, buf.ghost2, dt);
            if (buf.ghostDied) { died = true; break; }
            if (buf.ghostHitPortal) { safe = tick >= delay; break; }
            if (tick < delay) continue;
            ++survived; if (survived >= stopAt) break;
            if (flying) continue;

            bool grounded = buf.ghost1->m_isOnGround && (!second || buf.ghost2->m_isOnGround);
            if (!grounded) { ++airTicks; groundTicks = 0; continue; }
            if (airTicks < 6) continue;
            if (groundTicks > 2) { safe = true; break; }
            ++groundTicks;
        }
        if ((died || buf.ghostHitPortal) && tick < delay) *end = SimEnd::StopSearch;
        restoreObjects(); buf.simulating = false;
        return safe ? horizon : survived;
    }

    int searchDelay(PlayLayer* layer, PlayerObject* p1, PlayerObject* p2, bool dual, bool down, bool anyWave, bool flying, int horizon, int maxDelay, int releaseAfter, int base, int* landed) {
        using clock = std::chrono::steady_clock;
        auto start = clock::now();
        auto overBudget = [&] { return std::chrono::duration<double, std::milli>(clock::now() - start).count() > buf.budgetMs; };

        int target = anyWave ? base + 10 : std::min(horizon, base + 12);
        int bestTicks = base;

        for (int k = 1; k <= maxDelay; ++k) {
            SimEnd end;
            int ret = simulate(layer, p1, p2, dual, k, down, horizon, releaseAfter, std::min(target, horizon), &end);
            if (end == SimEnd::StopSearch) return 0;
            
            bool good = false;
            // v3: Advanced Wave Math from 4.6
            if (anyWave) {
                int minClearedGain = (k * 2) + 10;
                good = (ret >= horizon && (ret - base) >= (k * 2) + 6) || (ret >= base + minClearedGain && ret > bestTicks + 2);
                if (ret > bestTicks) bestTicks = ret;
            } 
            // v3: Ship Support Safety Net
            else if (flying) {
                good = (ret >= target) && (ret - base >= 10);
            } 
            // v2: Classic Ground Physics
            else {
                good = ret >= target;
            }

            if (good) { *landed = ret; return k; }
            if (overBudget()) { say("prediction budget hit at +{} ticks", k); return 0; }
        }
        return 0;
    }

    void queueInput(int ticks, bool down) { buf.queue.push_back({ ticks, down }); }

    bool intercept(GJBaseGameLayer* self, bool down, bool isPlayer1) {
        auto* layer = GameManager::get()->m_playLayer;
        if (!layer) return false;

        bool dual = layer->m_gameState.m_isDualMode && layer->m_player2;
        auto* level = layer->m_level;
        if (level && !isPlayer1 && level->m_twoPlayerMode) return false;

        buf.isPlayer1 = isPlayer1; buf.realInput = down;
        if (!buf.enabled) { buf.appliedInput = down; return false; }

        auto* p1 = self->m_player1;
        auto* p2 = dual ? layer->m_player2 : nullptr;
        
        // v3: Ship is NO LONGER blocked here!
        if (!p1 || p1->m_isDead) {
            resetPending(); buf.appliedInput = down; return false;
        }

        if (!buf.queue.empty()) {
            int after = buf.queue.back().ticksLeft;
            queueInput(std::max(buf.shift, after), down);
            buf.sinceInput = 0; return true;
        }

        bool p1Wave = p1->m_isDart;
        bool anyWave = p1Wave || (p2 && p2->m_isDart);
        bool flying = isFlying(p1) || (dual && p2 && isFlying(p2));
        bool mini = (p1Wave && p1->m_vehicleSize < 1.f) || (p2 && p2->m_isDart && p2->m_vehicleSize < 1.f);

        // v3: Integrated Spam Guard for Wave/Ship
        if (buf.sinceInput < SPAM_INTERVAL) {
            buf.rapid++;
        } else {
            buf.rapid = 0;
        }
        buf.sinceInput = 0;

        if (buf.rapid >= 2 && (anyWave || flying)) {
            buf.appliedInput = down; return false;
        }

        if (!p1Wave) {
            bool robot = p1->m_isRobot;
            bool wave2 = false;
            if (p2 && !robot) { wave2 = p2->m_isDart; if (!wave2) robot = p2->m_isRobot; }
            if (!wave2 && !down && !robot) { buf.appliedInput = false; return false; }
        }

        int horizon = mini ? 28 : (anyWave ? 38 : 48);
        int maxDelay = anyWave ? buf.ticksWave : buf.ticksAll;
        if (maxDelay <= 0) { buf.appliedInput = down; return false; }

        bool anyRobot = p1->m_isRobot || (p2 && p2->m_isRobot);
        int releaseAfter = (down && !anyWave && !anyRobot) ? holdEstimate() : -1;

        SimEnd end;
        int base = simulate(layer, p1, p2, dual, 0, down, horizon, releaseAfter, horizon, &end);
        if (base >= horizon) { buf.appliedInput = down; return false; }

        int landed = 0;
        int saved = searchDelay(layer, p1, p2, dual, down, anyWave, flying, horizon, maxDelay, releaseAfter, base, &landed);
        if (saved <= 0) { buf.appliedInput = down; return false; }

        say("SAVED {} +{}t ({:g}f)  {}->{}  [max {}]", down ? "press" : "release", saved, saved * TICK, base, landed, maxDelay);
        buf.shift = saved;
        queueInput(saved, down);
        return true;
    }

    void cycleStage() {
        buf.stage = (buf.stage + 1) % STAGE_COUNT;
        resetPending(); applyStage(); readSettings();
        say("F6 -> {}", stageText()); refreshPanel();
    }

    void deliverPending(PlayLayer* layer, PlayerObject* player) {
        if (buf.sinceInput <= 998) ++buf.sinceInput;
        
        if (buf.queue.empty()) {
            if (buf.appliedInput != buf.realInput) sendButton(layer, buf.realInput);
            return;
        }

        while (!buf.queue.empty() && buf.queue.front().ticksLeft <= 0) {
            bool down = buf.queue.front().down; buf.queue.pop_front();
            if (down != buf.appliedInput) sendButton(layer, down);
        }
        for (auto& q : buf.queue) --q.ticksLeft;
        if (buf.queue.empty()) buf.shift = 0;
    }
}

$on_mod(Loaded) {
    buf.console = Mod::get()->getSettingValue<bool>("debug-console");
    readSettings(); applyStage();
    if (buf.console) {
        AllocConsole(); FILE* out = nullptr; freopen_s(&out, "CONOUT$", "w", stdout);
        SetConsoleTitleA("Predictive Input Buffer v3");
    }
    say("v3 Loaded (Hybrid v2+v4.6). F6 cycles stages. K opens Panel.");
}

class $modify(BufPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayLayer::playEndAnimationToPos", HOOK_PRIORITY);
    }
    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (buf.simulating && isGhost(player)) { buf.ghostDied = true; return; }
        resetPending(); buf.appliedInput = false; buf.rapid = 0;
        PlayLayer::destroyPlayer(player, object);
    }
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        dropGhosts(); readSettings(); applyStage();
        buf.ghost1 = makeGhost(this); buf.ghost2 = makeGhost(this);
        auto win = CCDirector::get()->getWinSize();
        buf.panel = CCNode::create(); buf.panel->setPosition({ 5.f, win.height - 130.f - 5.f }); buf.panel->setZOrder(1000);
        buf.panel->addChild(CCLayerColor::create({ 0, 0, 0, 160 }, 250.f, 130.f));
        buf.panelTitle = CCLabelBMFont::create("", "bigFont.fnt"); buf.panelTitle->setScale(0.35f); buf.panelTitle->setAnchorPoint({ 0.f, 1.f }); buf.panelTitle->setPosition({ 6.f, 130.f - 5.f }); buf.panel->addChild(buf.panelTitle);
        buf.panelLog = CCLabelBMFont::create("", "chatFont.fnt"); buf.panelLog->setScale(0.5f); buf.panelLog->setAnchorPoint({ 0.f, 1.f }); buf.panelLog->setPosition({ 6.f, 130.f - 22.f }); buf.panel->addChild(buf.panelLog);
        this->addChild(buf.panel); refreshPanel();
        buf.realInput = false; buf.appliedInput = false; buf.isPlayer1 = true; buf.sinceInput = 999; buf.rapid = 0;
        return true;
    }
    void onQuit() { dropGhosts(); PlayLayer::onQuit(); }
    void resetLevel() { resetPending(); PlayLayer::resetLevel(); }
    void playEndAnimationToPos(CCPoint position) { if (!buf.simulating) PlayLayer::playEndAnimationToPos(position); }
};

class $modify(BufEditorLayer, LevelEditorLayer) { bool init(GJGameLevel* level, bool noUI) { bool ok = LevelEditorLayer::init(level, noUI); dropGhosts(); return ok; } };
class $modify(BufHardStreak, HardStreak) { void addPoint(CCPoint point) { if (!buf.simulating) HardStreak::addPoint(point); } };
class $modify(BufEffectGameObject, EffectGameObject) { void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) { if (!buf.simulating) EffectGameObject::triggerObject(layer, uniqueID, remapKeys); } };
class $modify(BufGameObject, GameObject) { void playShineEffect() { if (!buf.simulating) GameObject::playShineEffect(); } };

class $modify(BufPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::incrementJumps", HOOK_PRIORITY);
        (void)self.setHookPriority("PlayerObject::ringJump", HOOK_PRIORITY);
    }
    void update(float dt) {
        if (buf.simulating) { PlayerObject::update(dt); return; }
        auto* layer = GameManager::get()->m_playLayer;
        if (layer && this == layer->m_player1) { ++tickCount; if (buf.enabled) deliverPending(layer, this); }
        PlayerObject::update(dt);
        if (dt > 0.f) buf.tickLength = dt;
    }
    void incrementJumps() { if (!buf.simulating) PlayerObject::incrementJumps(); }
    void playSpiderDashEffect(CCPoint from, CCPoint to) { if (!buf.simulating) PlayerObject::playSpiderDashEffect(from, to); }
    void ringJump(RingObject* ring, bool skipCheck) { if (buf.simulating) { if (buf.smart && isGhost(this)) snapshotObject(ring); return; } PlayerObject::ringJump(ring, skipCheck); }
};

class $modify(BufKeyboard, CCKeyboardDispatcher) {
    bool dispatchKeyboardMSG(enumKeyCodes key, bool isKeyDown, bool isKeyRepeat, double timestamp) {
        if (isKeyDown && !isKeyRepeat && GameManager::get()->m_playLayer) {
            if (key == KEY_F6) cycleStage(); else if (key == KEY_K) { buf.panelOpen = !buf.panelOpen; refreshPanel(); }
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
        if (buf.simulating) { if (!buf.smart || !isGhost(player) || !object || buf.simUsed.contains(object)) return false; snapshotObject(object); return true; }
        return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
    }
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!buf.simulating) { GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt); return; }
        static gd::vector<GameObject*> relevant; relevant.clear(); if (static_cast<int>(relevant.capacity()) < objectCount) relevant.reserve(objectCount);
        uint64_t keep = buf.smart ? (COLLIDING_TYPES | INTERACT_TYPES) : COLLIDING_TYPES; bool haveRect = false; CCRect ghostRect; auto& list = *objects;
        for (int i = 0; i < objectCount; ++i) {
            auto* object = list[i]; if (!object) continue;
            auto type = static_cast<unsigned>(object->m_objectType); uint64_t bit = type < 64 ? (1ull << type) : 0;
            if (keep & bit) { relevant.push_back(object); continue; }
            if (!buf.smart) continue;
            bool stop = (STOP_TYPES & bit) != 0; float speed = stop ? 0.f : speedPortalValue(object->m_objectID);
            if (!stop && speed <= 0.f) continue;
            if (!haveRect) { ghostRect = player->getObjectRect(); haveRect = true; }
            if (ghostRect.intersectsRect(object->getObjectRect())) { if (stop) buf.ghostHitPortal = true; else player->m_playerSpeed = speed; }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &relevant, static_cast<int>(relevant.size()), dt);
    }
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!buf.simulating && button == 1 && !buf.injecting) recordRealInput(down);
        if (!buf.simulating && button == 1 && !buf.injecting && intercept(this, down, isPlayer1)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
    void playerTouchedRing(PlayerObject* player, RingObject* ring) { if (buf.simulating) { if (buf.smart && isGhost(player)) snapshotObject(ring); return; } GJBaseGameLayer::playerTouchedRing(player, ring); }
    void flipGravity(PlayerObject* player, bool flip, bool noEffects) { if (buf.simulating) { if (buf.smart && isGhost(player)) player->flipGravity(flip, true); return; } GJBaseGameLayer::flipGravity(player, flip, noEffects); }
    void gameEventTriggered(GJGameEvent event, int material, int playerID) { if (!buf.simulating) GJBaseGameLayer::gameEventTriggered(event, material, playerID); }
    void playerWillSwitchMode(PlayerObject* player, GameObject* object) { if (buf.simulating) { buf.ghostHitPortal = true; return; } GJBaseGameLayer::playerWillSwitchMode(player, object); }
};

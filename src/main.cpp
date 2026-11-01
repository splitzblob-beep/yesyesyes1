// Click Before Frames
//
// A from-scratch recreation of the "Click Before Frames" idea by vVexis.
//
//  - Cube, ball, robot, spider: a click made in the air (not on the ground, not on an
//    orb) is simulated ahead with a hidden copy of your icon. If clicking now would
//    kill you but clicking a few ticks later wouldn't, the click waits until then.
//    Clicks on the ground or on orbs are never delayed.
//  - Ship, UFO, swing, wave: taps shorter than a minimum length are stretched to it.
//  - Floating, draggable V button on every screen opens the menu: mode
//    (OFF / LOW / MED / HIGH), which gamemodes it works in, live stats, settings.
//    The pause menu has a V button that opens the settings.
//
// 1 tick = 1/240 s = a quarter of a frame at 60 fps.
// Dual mode and 2-player levels are left alone. Not for leaderboard or list runs.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCDrawNode.hpp>
#include <Geode/modify/EffectGameObject.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GameObject.hpp>
#include <Geode/modify/HardStreak.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/ui/OverlayManager.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_set>
#include <vector>

using namespace geode::prelude;

namespace cbf {

    // ------------------------------------------------------------------
    // Settings and saved choices
    // ------------------------------------------------------------------

    enum Mode { MODE_OFF = 0, MODE_LOW = 1, MODE_MED = 2, MODE_HIGH = 3 };

    // Saved choices made in the V menu (not regular settings).
    constexpr char const* KEY_MODE = "mode";
    constexpr char const* KEY_CUBE = "en-cube";  // cube / ball / robot / spider
    constexpr char const* KEY_FLY = "en-fly";    // ship / ufo / swing
    constexpr char const* KEY_WAVE = "en-wave";

    int intSetting(char const* key) {
        return static_cast<int>(Mod::get()->getSettingValue<int64_t>(key));
    }

    int mode() {
        return std::clamp(Mod::get()->getSavedValue<int>(KEY_MODE, MODE_MED), 0, 3);
    }

    bool groupOn(char const* key) {
        return Mod::get()->getSavedValue<bool>(key, true);
    }

    int ticksForMode(int m) {
        switch (m) {
            case MODE_LOW: return intSetting("low-ticks");
            case MODE_MED: return intSetting("med-ticks");
            case MODE_HIGH: return intSetting("high-ticks");
            default: return 0;
        }
    }

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------

    struct Stats {
        int clicks = 0;     // presses seen in cube / ball / robot / spider
        int air = 0;        // ...of those, made in the air (the only ones it checks)
        int safe = 0;       // checked and fine as they were
        int wouldDie = 0;   // checked and would have killed you
        int delayed = 0;    // held back to a safe moment
        int stretched = 0;  // short ship / ufo / swing / wave taps lengthened
        int lastDelay = 0;
    };

    struct Pending {
        int wait;
        bool down;
        bool p1;
    };

    struct Snapshot {
        GameObject* object;
        bool isActivated;
        EffectGameObject* effect;
        bool activated;
        bool byP1;
        bool byP2;
    };

    struct State {
        Stats stats;

        // input pipeline
        std::deque<Pending> queue;
        int shift = 0;       // while a click is held back, later inputs wait this long too
        int releaseWait = 0; // ticks until a stretched release is sent
        bool injecting = false;
        int tick = 0;
        int pressTick = -1;
        int hold = 12;       // your usual tap length (smoothed), used by the prediction
        float tickDt = 0.25f;

        // prediction
        PlayerObject* ghost = nullptr;  // owned by the PlayLayer
        bool simulating = false;
        bool ghostDied = false;
        bool ghostHitStop = false;
        std::unordered_set<GameObject*> touched;
        std::vector<Snapshot> snapshots;

        bool warnedConflict = false;
    };

    State g;

    void clearPending() {
        g.queue.clear();
        g.shift = 0;
        g.releaseWait = 0;
        g.pressTick = -1;
    }

    PlayLayer* playLayer() {
        return GameManager::get()->m_playLayer;
    }

    // ------------------------------------------------------------------
    // Which objects the ghost may touch while predicting
    // ------------------------------------------------------------------

    constexpr uint64_t bit(GameObjectType type) {
        return 1ull << static_cast<unsigned>(type);
    }

    uint64_t typeBit(GameObject* object) {
        auto type = static_cast<unsigned>(object->m_objectType);
        return type < 64 ? (1ull << type) : 0;
    }

    constexpr uint64_t SOLID_MASK = bit(GameObjectType::Solid) | bit(GameObjectType::Hazard) |
        bit(GameObjectType::AnimatedHazard) | bit(GameObjectType::Slope);

    constexpr uint64_t ORB_MASK = bit(GameObjectType::YellowJumpPad) | bit(GameObjectType::PinkJumpPad) |
        bit(GameObjectType::RedJumpPad) | bit(GameObjectType::GravityPad) | bit(GameObjectType::SpiderPad) |
        bit(GameObjectType::YellowJumpRing) | bit(GameObjectType::PinkJumpRing) | bit(GameObjectType::RedJumpRing) |
        bit(GameObjectType::GravityRing) | bit(GameObjectType::GreenRing) | bit(GameObjectType::DropRing) |
        bit(GameObjectType::CustomRing) | bit(GameObjectType::DashRing) | bit(GameObjectType::GravityDashRing) |
        bit(GameObjectType::SpiderOrb) | bit(GameObjectType::InverseGravityPortal) |
        bit(GameObjectType::NormalGravityPortal) | bit(GameObjectType::GravityTogglePortal);

    // Too much changes at these to predict past them; reaching one counts as safe.
    constexpr uint64_t STOP_MASK = bit(GameObjectType::ShipPortal) | bit(GameObjectType::CubePortal) |
        bit(GameObjectType::BallPortal) | bit(GameObjectType::UfoPortal) | bit(GameObjectType::WavePortal) |
        bit(GameObjectType::RobotPortal) | bit(GameObjectType::SpiderPortal) | bit(GameObjectType::SwingPortal) |
        bit(GameObjectType::DualPortal) | bit(GameObjectType::SoloPortal) | bit(GameObjectType::RegularSizePortal) |
        bit(GameObjectType::MiniSizePortal) | bit(GameObjectType::InverseMirrorPortal) |
        bit(GameObjectType::NormalMirrorPortal) | bit(GameObjectType::TeleportPortal) |
        bit(GameObjectType::TeleportOrb);

    float speedPortalSpeed(int objectID) {
        switch (objectID) {
            case 200: return 0.7f;
            case 201: return 0.9f;
            case 202: return 1.1f;
            case 203: return 1.3f;
            case 1334: return 1.6f;
            default: return 0.f;
        }
    }

    bool isGhost(PlayerObject* player) {
        return player && player == g.ghost;
    }

    // Anything the ghost changes in the level is put back after each prediction.
    void snapshot(GameObject* object) {
        if (!object || g.touched.contains(object)) {
            return;
        }
        g.touched.insert(object);
        Snapshot s { object, object->m_isActivated, nullptr, false, false, false };
        if (auto* effect = typeinfo_cast<EffectGameObject*>(object)) {
            s.effect = effect;
            s.activated = effect->m_activated;
            s.byP1 = effect->m_activatedByPlayer1;
            s.byP2 = effect->m_activatedByPlayer2;
        }
        g.snapshots.push_back(s);
    }

    void restoreSandbox() {
        for (auto const& s : g.snapshots) {
            s.object->m_isActivated = s.isActivated;
            if (s.effect) {
                s.effect->m_activated = s.activated;
                s.effect->m_activatedByPlayer1 = s.byP1;
                s.effect->m_activatedByPlayer2 = s.byP2;
            }
        }
        g.snapshots.clear();
        g.touched.clear();
    }

    // ------------------------------------------------------------------
    // Prediction
    // ------------------------------------------------------------------

    PlayerObject* makeGhost(PlayLayer* layer, CCLayer* sandbox) {
        bool asP1 = Mod::get()->getSettingValue<bool>("ghost-as-p1");
        auto* ghost = PlayerObject::create(1, 1, layer, sandbox, asP1);
        if (!ghost) {
            return nullptr;
        }
        ghost->m_isSecondPlayer = !asP1;
        ghost->setVisible(false);
        sandbox->addChild(ghost);
        return ghost;
    }

    void copyState(PlayerObject* ghost, PlayerObject* real) {
        ghost->resetTouchedRings(true);
        ghost->copyAttributes(real);
        ghost->setVisible(false);
        ghost->setPosition(real->getPosition());
        ghost->setRotation(real->getRotation());
        ghost->setScale(real->getScale());
        ghost->setContentSize(real->getContentSize());
        ghost->m_width = real->m_width;
        ghost->m_height = real->m_height;
        ghost->m_yVelocity = real->m_yVelocity;
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
    }

    void stepGhost(PlayLayer* layer, PlayerObject* ghost, float dt) {
        ghost->update(dt);
        ghost->updateRotation(dt);
        if (g.ghostDied) {
            return;
        }
        ghost->m_collisionLogTop->removeAllObjects();
        ghost->m_collisionLogBottom->removeAllObjects();
        ghost->m_collisionLogLeft->removeAllObjects();
        ghost->m_collisionLogRight->removeAllObjects();
        layer->checkCollisions(ghost, dt, false);
        // A hazard sets "player died" even though the ghost's death is blocked.
        if (layer->m_playerDied) {
            g.ghostDied = true;
            layer->m_playerDied = false;
        }
    }

    // Upward speed, whichever way gravity currently points.
    float upSpeed(PlayerObject* p) {
        return static_cast<float>(p->m_isUpsideDown ? -p->m_yVelocity : p->m_yVelocity);
    }

    struct SimResult {
        bool safe = false;
        bool doomed = false;  // died before the click even happened: waiting longer can't help
    };

    // Taps `delay` ticks from now (held for your usual tap length) and checks whether
    // the ghost gets through. "Through" means: it survives the whole prediction length,
    // or the click actually made it jump / flip and it then landed cleanly, or it reached
    // a portal the prediction can't see past.
    SimResult simulate(PlayLayer* layer, PlayerObject* real, int delay, int horizon) {
        SimResult res;
        auto* ghost = g.ghost;
        if (!ghost) {
            res.safe = true;
            return res;
        }
        copyState(ghost, real);

        bool savedDied = layer->m_playerDied;
        bool savedDebug = layer->m_isDebugDrawEnabled;
        layer->m_playerDied = false;

        g.simulating = true;
        g.ghostDied = false;
        g.ghostHitStop = false;

        float dt = (g.tickDt > 0.001f && g.tickDt < 2.f) ? g.tickDt : 0.25f;
        int total = delay + horizon;
        int t = 0;
        int survived = 0;
        bool jumped = false;
        int airAfterJump = 0;
        int groundAfterJump = 0;
        float prevUp = upSpeed(ghost);
        bool prevGround = ghost->m_isOnGround;
        bool prevFlip = ghost->m_isUpsideDown;

        for (; t < total; ++t) {
            if (t == delay) {
                ghost->pushButton(PlayerButton::Jump);
            }
            if (t == delay + g.hold) {
                ghost->releaseButton(PlayerButton::Jump);
            }

            stepGhost(layer, ghost, dt);
            if (g.ghostDied) {
                break;
            }
            if (g.ghostHitStop) {
                res.safe = t >= delay;
                break;
            }

            bool ground = ghost->m_isOnGround;
            float up = upSpeed(ghost);
            bool flip = ghost->m_isUpsideDown;

            if (t >= delay) {
                ++survived;
                if (!jumped && (up > prevUp + 3.f || flip != prevFlip || (prevGround && !ground && up > 0.f))) {
                    jumped = true;
                }
                if (jumped) {
                    if (!ground) {
                        ++airAfterJump;
                        groundAfterJump = 0;
                    }
                    else if (airAfterJump >= 4 && ++groundAfterJump >= 3) {
                        res.safe = true;
                        break;
                    }
                }
            }
            prevUp = up;
            prevGround = ground;
            prevFlip = flip;
        }

        if (!g.ghostDied && !g.ghostHitStop && survived >= horizon) {
            res.safe = true;
        }
        if ((g.ghostDied || g.ghostHitStop) && t < delay) {
            res.doomed = true;
            res.safe = false;
        }

        restoreSandbox();
        layer->m_playerDied = savedDied;
        layer->m_isDebugDrawEnabled = savedDebug;
        g.simulating = false;
        g.ghostDied = false;
        g.ghostHitStop = false;
        return res;
    }

    // Smallest delay that gets you through, or 0 to click now.
    int findDelay(PlayLayer* layer, PlayerObject* real, int maxDelay) {
        int horizon = intSetting("horizon-ticks");
        double budget = Mod::get()->getSettingValue<double>("sim-budget-ms");
        auto start = std::chrono::steady_clock::now();

        auto now = simulate(layer, real, 0, horizon);
        if (now.safe) {
            ++g.stats.safe;
            return 0;
        }
        ++g.stats.wouldDie;

        for (int d = 1; d <= maxDelay; ++d) {
            if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() > budget) {
                return 0;
            }
            auto later = simulate(layer, real, d, horizon);
            if (later.doomed) {
                return 0;
            }
            if (later.safe) {
                ++g.stats.delayed;
                g.stats.lastDelay = d;
                return d;
            }
        }
        return 0;
    }

    // Your click can already do something right now (on the ground or on an orb).
    bool canActNow(PlayerObject* p) {
        if (p->m_isOnGround) {
            return true;
        }
        auto* rings = p->m_touchingRings;
        return rings && rings->count() > 0;
    }

    // ------------------------------------------------------------------
    // Input pipeline
    // ------------------------------------------------------------------

    void send(GJBaseGameLayer* layer, bool down, bool p1) {
        g.injecting = true;
        layer->handleButton(down, 1, p1);
        g.injecting = false;
    }

    // Once per physics tick, before player 1 moves.
    void onTick(PlayLayer* layer) {
        ++g.tick;

        while (!g.queue.empty() && g.queue.front().wait <= 0) {
            auto next = g.queue.front();
            g.queue.pop_front();
            send(layer, next.down, next.p1);
        }
        for (auto& pending : g.queue) {
            --pending.wait;
        }
        if (g.queue.empty()) {
            g.shift = 0;
        }

        if (g.releaseWait > 0 && --g.releaseWait == 0) {
            send(layer, false, true);
        }
    }

    // Returns true if the input was taken over by the mod.
    bool intercept(GJBaseGameLayer* self, PlayLayer* layer, bool down, bool isPlayer1) {
        // How long you hold each click (player 1).
        int heldNow = -1;
        if (isPlayer1) {
            if (down) {
                g.pressTick = g.tick;
            }
            else if (g.pressTick >= 0) {
                heldNow = g.tick - g.pressTick;
                g.pressTick = -1;
            }
        }

        // A click is being held back: everything after it waits the same amount, so
        // your rhythm (tap length, follow-up clicks) comes out exactly as you played it.
        if (!g.queue.empty()) {
            g.queue.push_back({ g.shift, down, isPlayer1 });
            return true;
        }

        auto* p = self->m_player1;
        bool twoPlayer = self->m_level && self->m_level->m_twoPlayerMode;
        if (!isPlayer1 || !p || p->m_isDead || p->m_isPlatformer || self->m_gameState.m_isDualMode || twoPlayer) {
            return false;
        }

        // Pressing again while a stretched release is still waiting: let go first.
        if (down && g.releaseWait > 0) {
            g.releaseWait = 0;
            send(self, false, true);
        }

        bool wave = p->m_isDart;
        bool fly = p->m_isShip || p->m_isBird || p->m_isSwing;

        // Ship / UFO / swing / wave: stretch very short taps to the minimum length.
        if (wave || fly) {
            int minTicks = intSetting(wave ? "wave-min-ticks" : "fly-min-ticks");
            bool on = groupOn(wave ? KEY_WAVE : KEY_FLY);
            if (on && minTicks > 0 && !down && heldNow >= 0 && heldNow < minTicks) {
                g.releaseWait = minTicks - heldNow;
                ++g.stats.stretched;
                return true;
            }
            return false;
        }

        // Cube / ball / robot / spider: hold early air clicks until they're safe.
        if (heldNow > 0) {
            g.hold = (g.hold * 3 + std::clamp(heldNow, 1, 120)) / 4;
        }
        if (!down || !groupOn(KEY_CUBE)) {
            return false;
        }
        ++g.stats.clicks;
        if (canActNow(p)) {
            return false;
        }
        ++g.stats.air;

        int delay = findDelay(layer, p, ticksForMode(mode()));
        if (delay <= 0) {
            return false;
        }
        g.shift = delay;
        g.queue.push_back({ delay, true, true });
        return true;
    }
}

// ----------------------------------------------------------------------
// V menu
// ----------------------------------------------------------------------

void openVMenu();

// Floating V button. Lives in Geode's overlay, so it shows on top of every screen.
// Tap it to open the menu, drag it to move it (the spot is remembered).
class CBFVButton : public CCLayer {
    CCPoint m_touchStart;
    CCPoint m_posStart;
    bool m_moved = false;

public:
    static CBFVButton* create() {
        auto* ret = new CBFVButton();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) {
            return false;
        }
        auto* letter = CCLabelBMFont::create("V", "bigFont.fnt");
        auto* circle = CircleButtonSprite::create(letter, CircleBaseColor::Blue, CircleBaseSize::Small);
        auto size = circle->getContentSize();
        this->setContentSize(size);
        this->ignoreAnchorPointForPosition(false);
        this->setAnchorPoint({ 0.5f, 0.5f });
        circle->setPosition(size / 2);
        this->addChild(circle);
        this->setTouchEnabled(true);
        this->scheduleUpdate();
        this->loadPlacement();
        return true;
    }

    void keepOnScreen(CCPoint& pos) {
        auto win = CCDirector::get()->getWinSize();
        auto half = this->getContentSize() * this->getScale() / 2;
        pos.x = std::clamp(pos.x, half.width, win.width - half.width);
        pos.y = std::clamp(pos.y, half.height, win.height - half.height);
    }

    void loadPlacement() {
        auto win = CCDirector::get()->getWinSize();
        auto* mod = Mod::get();
        this->setScale(static_cast<float>(mod->getSettingValue<double>("v-size")));
        CCPoint pos { mod->getSavedValue<float>("v-x", win.width - 34.f), mod->getSavedValue<float>("v-y", win.height - 60.f) };
        keepOnScreen(pos);
        this->setPosition(pos);
    }

    // Follows the size / visibility settings live.
    void update(float) override {
        auto* mod = Mod::get();
        float size = static_cast<float>(mod->getSettingValue<double>("v-size"));
        if (std::abs(this->getScale() - size) > 0.001f) {
            this->setScale(size);
            auto pos = this->getPosition();
            keepOnScreen(pos);
            this->setPosition(pos);
        }
        this->setVisible(mod->getSettingValue<bool>("show-v"));
    }

    bool isOnButton(CCTouch* touch) {
        auto local = this->convertToNodeSpace(touch->getLocation());
        auto center = this->getContentSize() / 2;
        float radius = center.width + 6.f;
        float dx = local.x - center.width;
        float dy = local.y - center.height;
        return dx * dx + dy * dy <= radius * radius;
    }

    void registerWithTouchDispatcher() override {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -200, true);
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (!this->isVisible() || !this->getParent() || !isOnButton(touch)) {
            return false;
        }
        m_touchStart = this->getParent()->convertToNodeSpace(touch->getLocation());
        m_posStart = this->getPosition();
        m_moved = false;
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!this->getParent()) {
            return;
        }
        auto offset = this->getParent()->convertToNodeSpace(touch->getLocation()) - m_touchStart;
        // Small wiggles still count as a tap.
        if (!m_moved && offset.x * offset.x + offset.y * offset.y < 100.f) {
            return;
        }
        m_moved = true;
        auto pos = m_posStart + offset;
        keepOnScreen(pos);
        this->setPosition(pos);
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        if (m_moved) {
            Mod::get()->setSavedValue<float>("v-x", this->getPositionX());
            Mod::get()->setSavedValue<float>("v-y", this->getPositionY());
        }
        else {
            openVMenu();
        }
        m_moved = false;
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_moved = false;
    }
};

// The menu the V button opens: mode, active gamemodes, live stats, settings.
class CBFMenu : public CCLayer {
    std::array<CCSprite*, 4> m_modeChecks {};
    std::array<CCSprite*, 3> m_groupChecks {};
    CCLabelBMFont* m_statsTop = nullptr;
    CCLabelBMFont* m_statsBottom = nullptr;
    float m_sinceRefresh = 1.f;

    static constexpr std::array<char const*, 3> GROUP_KEYS { cbf::KEY_CUBE, cbf::KEY_FLY, cbf::KEY_WAVE };

public:
    static CBFMenu* create() {
        auto* ret = new CBFMenu();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    // A checkbox whose "on" sprite can be shown or hidden.
    CCNode* checkbox(CCSprite*& onSprite) {
        constexpr float scale = 0.7f;
        auto* off = CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png");
        onSprite = CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png");
        auto size = off->getContentSize() * scale;
        auto* holder = CCNode::create();
        holder->setContentSize(size);
        for (auto* sprite : { off, onSprite }) {
            sprite->setScale(scale);
            sprite->setPosition(size / 2);
            holder->addChild(sprite);
        }
        return holder;
    }

    CCLabelBMFont* text(std::string const& str, CCPoint pos, float scale, ccColor3B color) {
        auto* label = CCLabelBMFont::create(str.c_str(), "bigFont.fnt");
        label->setScale(scale);
        label->setAnchorPoint({ 0.f, 0.5f });
        label->setPosition(pos);
        label->setColor(color);
        this->addChild(label);
        return label;
    }

    bool init() override {
        if (!CCLayer::init()) {
            return false;
        }
        auto win = CCDirector::get()->getWinSize();
        CCPoint c = win / 2;
        ccColor3B heading { 140, 165, 190 };
        ccColor3B white { 255, 255, 255 };

        this->addChild(CCLayerColor::create({ 0, 0, 0, 140 }));

        auto* panel = CCScale9Sprite::create("GJ_square01.png");
        panel->setContentSize({ 300.f, 270.f });
        panel->setPosition(c);
        this->addChild(panel);

        auto* title = CCLabelBMFont::create("Click Before Frames", "goldFont.fnt");
        title->setScale(0.6f);
        title->setPosition(c + CCPoint { 0.f, 118.f });
        this->addChild(title);

        auto* menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        this->addChild(menu);

        // Mode
        text("MODE  (max delay in ticks)", c + CCPoint { -125.f, 96.f }, 0.33f, heading);
        constexpr std::array<char const*, 4> names { "OFF", "LOW", "MED", "HIGH" };
        for (int i = 0; i < 4; ++i) {
            CCPoint pos = c + CCPoint { -108.f + (i % 2) * 125.f, 72.f - (i / 2) * 28.f };
            auto* button = CCMenuItemSpriteExtra::create(checkbox(m_modeChecks[i]), this, menu_selector(CBFMenu::onMode));
            button->setTag(i);
            button->setPosition(pos);
            menu->addChild(button);
            auto label = i == 0 ? std::string("OFF") : fmt::format("{}  {}", names[i], cbf::ticksForMode(i));
            text(label, pos + CCPoint { 18.f, 0.f }, 0.45f, white);
        }

        // Active in
        text("ACTIVE IN", c + CCPoint { -125.f, 20.f }, 0.33f, heading);
        constexpr std::array<char const*, 3> groups {
            "Cube / Ball / Robot / Spider",
            "Ship / UFO / Swing  (min tap)",
            "Wave  (min tap)",
        };
        for (int i = 0; i < 3; ++i) {
            CCPoint pos = c + CCPoint { -108.f, -2.f - i * 26.f };
            auto* button = CCMenuItemSpriteExtra::create(checkbox(m_groupChecks[i]), this, menu_selector(CBFMenu::onGroup));
            button->setTag(i);
            button->setPosition(pos);
            menu->addChild(button);
            text(groups[i], pos + CCPoint { 18.f, 0.f }, 0.38f, white);
        }

        // Stats
        text("STATS", c + CCPoint { -125.f, -76.f }, 0.33f, heading);
        m_statsTop = text("", c + CCPoint { -125.f, -91.f }, 0.3f, white);
        m_statsBottom = text("", c + CCPoint { -125.f, -103.f }, 0.3f, white);

        // Buttons
        auto addButton = [&](char const* label, CCPoint pos, SEL_MenuHandler handler) {
            auto* sprite = ButtonSprite::create(label);
            sprite->setScale(0.6f);
            auto* button = CCMenuItemSpriteExtra::create(sprite, this, handler);
            button->setPosition(pos);
            menu->addChild(button);
        };
        addButton("Settings", c + CCPoint { -60.f, -121.f }, menu_selector(CBFMenu::onSettings));
        addButton("Reset stats", c + CCPoint { 60.f, -121.f }, menu_selector(CBFMenu::onResetStats));

        auto* close = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png"), this, menu_selector(CBFMenu::onClose));
        close->setPosition(c + CCPoint { -146.f, 131.f });
        menu->addChild(close);

        this->refreshChecks();
        this->refreshStats();
        this->setTouchEnabled(true);
        this->setKeypadEnabled(true);
        this->scheduleUpdate();
        this->setID("menu"_spr);
        return true;
    }

    void refreshChecks() {
        int current = cbf::mode();
        for (int i = 0; i < 4; ++i) {
            m_modeChecks[i]->setVisible(i == current);
        }
        for (int i = 0; i < 3; ++i) {
            m_groupChecks[i]->setVisible(cbf::groupOn(GROUP_KEYS[i]));
        }
    }

    void refreshStats() {
        auto const& s = cbf::g.stats;
        m_statsTop->setString(
            fmt::format("clicks {}  air {}  safe {}  would-die {}", s.clicks, s.air, s.safe, s.wouldDie).c_str());
        m_statsBottom->setString(fmt::format(
            "delayed {}  stretched {}  last {}t  dt {:.3f}", s.delayed, s.stretched, s.lastDelay, cbf::g.tickDt)
                                     .c_str());
    }

    void update(float dt) override {
        m_sinceRefresh += dt;
        if (m_sinceRefresh >= 0.2f) {
            m_sinceRefresh = 0.f;
            refreshStats();
        }
    }

    void onMode(CCObject* sender) {
        Mod::get()->setSavedValue<int>(cbf::KEY_MODE, static_cast<CCNode*>(sender)->getTag());
        cbf::clearPending();
        refreshChecks();
    }

    void onGroup(CCObject* sender) {
        auto const* key = GROUP_KEYS[static_cast<CCNode*>(sender)->getTag()];
        Mod::get()->setSavedValue<bool>(key, !cbf::groupOn(key));
        cbf::clearPending();
        refreshChecks();
    }

    void onResetStats(CCObject*) {
        cbf::g.stats = {};
        refreshStats();
    }

    void onSettings(CCObject*) {
        openSettingsPopup(Mod::get());
        this->removeFromParentAndCleanup(true);
    }

    void onClose(CCObject*) {
        this->removeFromParentAndCleanup(true);
    }

    // Blocks touches to whatever is behind the menu.
    void registerWithTouchDispatcher() override {
        CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }
    bool ccTouchBegan(CCTouch*, CCEvent*) override {
        return true;
    }
    void ccTouchMoved(CCTouch*, CCEvent*) override {}
    void ccTouchEnded(CCTouch*, CCEvent*) override {}
    void ccTouchCancelled(CCTouch*, CCEvent*) override {}
    void keyBackClicked() override {
        onClose(nullptr);
    }
};

void openVMenu() {
    auto* scene = CCDirector::get()->getRunningScene();
    if (!scene || scene->getChildByID("menu"_spr)) {
        return;
    }
    if (auto* menu = CBFMenu::create()) {
        scene->addChild(menu, 10000);
        handleTouchPriority(menu);
    }
}

namespace {
    CBFVButton* g_vButton = nullptr;
}

// ----------------------------------------------------------------------
// Hooks
// ----------------------------------------------------------------------

// Create the V button once, on the main menu; it then stays on every screen.
class $modify(CBFMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }
        if (!g_vButton) {
            g_vButton = CBFVButton::create();
            if (g_vButton) {
                OverlayManager::get()->addChild(g_vButton, 99999);
            }
        }
        return true;
    }
};

class $modify(CBFPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto* letter = CCLabelBMFont::create("V", "bigFont.fnt");
        auto* sprite = CircleButtonSprite::create(letter, CircleBaseColor::Blue, CircleBaseSize::Small);
        auto* button = CCMenuItemSpriteExtra::create(sprite, this, menu_selector(CBFPauseLayer::onCBFSettings));
        button->setID("settings-button"_spr);

        if (auto* menu = typeinfo_cast<CCMenu*>(this->getChildByID("right-button-menu"))) {
            menu->addChild(button);
            menu->updateLayout();
        }
        else {
            auto win = CCDirector::get()->getWinSize();
            auto* own = CCMenu::create();
            own->setPosition({ win.width - 34.f, 34.f });
            button->setPosition({ 0.f, 0.f });
            own->addChild(button);
            this->addChild(own);
        }
    }

    void onCBFSettings(CCObject*) {
        openSettingsPopup(Mod::get());
    }
};

class $modify(CBFPlayLayer, PlayLayer) {
    struct Fields {
        Ref<CCLayer> sandbox;
        Ref<PlayerObject> ghost;
    };

    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::First);
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }
        cbf::clearPending();
        auto* sandbox = CCLayer::create();
        m_fields->sandbox = sandbox;
        m_fields->ghost = cbf::makeGhost(this, sandbox);
        cbf::g.ghost = m_fields->ghost.data();

        // Two mods changing click timing at once fight each other.
        if (!cbf::g.warnedConflict && Loader::get()->isModLoaded("splitzblob.predictive-input-buffer")) {
            cbf::g.warnedConflict = true;
            Notification::create("Click Before Frames: disable Predictive Input Buffer, they clash",
                NotificationIcon::Warning)
                ->show();
        }
        return true;
    }

    void onQuit() {
        cbf::clearPending();
        cbf::g.ghost = nullptr;
        m_fields->ghost = nullptr;
        m_fields->sandbox = nullptr;
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (cbf::g.simulating) {
            cbf::g.ghostDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void resetLevel() {
        cbf::clearPending();
        PlayLayer::resetLevel();
    }

    void levelComplete() {
        if (cbf::g.simulating) {
            return;
        }
        PlayLayer::levelComplete();
    }

    void playEndAnimationToPos(CCPoint position) {
        if (cbf::g.simulating) {
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }
};

class $modify(CBFGameLayer, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::VeryEarly);
        (void)self.setHookPriority("GJBaseGameLayer::collisionCheckObjects", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::canBeActivatedByPlayer", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::playerTouchedRing", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::flipGravity", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::gameEventTriggered", Priority::First);
        (void)self.setHookPriority("GJBaseGameLayer::playerWillSwitchMode", Priority::First);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto* layer = cbf::playLayer();
        bool ours = !cbf::g.injecting && !cbf::g.simulating && button == 1 && layer &&
            static_cast<GJBaseGameLayer*>(layer) == this && cbf::mode() != cbf::MODE_OFF;
        if (ours && cbf::intercept(this, layer, down, isPlayer1)) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // While predicting, the ghost only sees what can kill it, orbs/pads, and portals.
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects, int objectCount, float dt) {
        if (!cbf::g.simulating || !objects) {
            GJBaseGameLayer::collisionCheckObjects(player, objects, objectCount, dt);
            return;
        }
        static gd::vector<GameObject*> kept;
        kept.clear();

        constexpr uint64_t keep = cbf::SOLID_MASK | cbf::ORB_MASK;
        bool haveRect = false;
        CCRect ghostRect;
        int count = std::min(objectCount, static_cast<int>(objects->size()));
        for (int i = 0; i < count; ++i) {
            auto* object = (*objects)[i];
            if (!object) {
                continue;
            }
            uint64_t type = cbf::typeBit(object);
            if (keep & type) {
                kept.push_back(object);
                continue;
            }
            bool stop = (cbf::STOP_MASK & type) != 0;
            float speed = stop ? 0.f : cbf::speedPortalSpeed(object->m_objectID);
            if (!stop && speed <= 0.f) {
                continue;
            }
            if (!haveRect) {
                ghostRect = player->getObjectRect();
                haveRect = true;
            }
            if (ghostRect.intersectsRect(object->getObjectRect())) {
                if (stop) {
                    cbf::g.ghostHitStop = true;
                }
                else {
                    player->m_playerSpeed = speed;
                }
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &kept, static_cast<int>(kept.size()), dt);
    }

    // The ghost may use each orb/pad once per prediction; the real ones are untouched.
    bool canBeActivatedByPlayer(PlayerObject* player, EffectGameObject* object) {
        if (!cbf::g.simulating) {
            return GJBaseGameLayer::canBeActivatedByPlayer(player, object);
        }
        if (!cbf::isGhost(player) || !object || cbf::g.touched.contains(object)) {
            return false;
        }
        if (object->m_activatedByPlayer1 || object->m_activatedByPlayer2) {
            return false;
        }
        cbf::snapshot(object);
        return true;
    }

    void playerTouchedRing(PlayerObject* player, RingObject* ring) {
        if (cbf::g.simulating) {
            if (!cbf::isGhost(player)) {
                return;
            }
            cbf::snapshot(ring);
        }
        GJBaseGameLayer::playerTouchedRing(player, ring);
    }

    void flipGravity(PlayerObject* player, bool flip, bool noEffects) {
        if (cbf::g.simulating) {
            if (cbf::isGhost(player)) {
                player->flipGravity(flip, true);
            }
            return;
        }
        GJBaseGameLayer::flipGravity(player, flip, noEffects);
    }

    void gameEventTriggered(GJGameEvent event, int material, int playerID) {
        if (cbf::g.simulating) {
            return;
        }
        GJBaseGameLayer::gameEventTriggered(event, material, playerID);
    }

    void playerWillSwitchMode(PlayerObject* player, GameObject* object) {
        if (cbf::g.simulating) {
            cbf::g.ghostHitStop = true;
            return;
        }
        GJBaseGameLayer::playerWillSwitchMode(player, object);
    }

    void updateDebugDraw() {
        if (cbf::g.simulating) {
            return;
        }
        GJBaseGameLayer::updateDebugDraw();
    }
};

class $modify(CBFPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::ringJump", Priority::First);
        (void)self.setHookPriority("PlayerObject::incrementJumps", Priority::First);
        (void)self.setHookPriority("PlayerObject::playSpiderDashEffect", Priority::First);
    }

    void update(float dt) {
        if (!cbf::g.simulating) {
            auto* layer = cbf::playLayer();
            if (layer && this == layer->m_player1) {
                if (dt > 0.f) {
                    cbf::g.tickDt = dt;
                }
                cbf::onTick(layer);
            }
        }
        PlayerObject::update(dt);
    }

    void ringJump(RingObject* ring, bool skipCheck) {
        if (cbf::g.simulating) {
            if (!cbf::isGhost(this)) {
                return;
            }
            cbf::snapshot(ring);
        }
        PlayerObject::ringJump(ring, skipCheck);
    }

    void incrementJumps() {
        if (cbf::g.simulating) {
            return;
        }
        PlayerObject::incrementJumps();
    }

    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (cbf::g.simulating) {
            return;
        }
        PlayerObject::playSpiderDashEffect(from, to);
    }
};

class $modify(CBFEffectGameObject, EffectGameObject) {
    void triggerObject(GJBaseGameLayer* layer, int uniqueID, gd::vector<int> const* remapKeys) {
        if (cbf::g.simulating) {
            return;
        }
        EffectGameObject::triggerObject(layer, uniqueID, remapKeys);
    }
};

class $modify(CBFGameObject, GameObject) {
    void playShineEffect() {
        if (cbf::g.simulating) {
            return;
        }
        GameObject::playShineEffect();
    }
};

class $modify(CBFHardStreak, HardStreak) {
    void addPoint(CCPoint point) {
        if (cbf::g.simulating) {
            return;
        }
        HardStreak::addPoint(point);
    }
};

// Nothing gets drawn mid-prediction, so hitbox views never flash.
class $modify(CBFDrawNode, CCDrawNode) {
    bool drawPolygon(CCPoint* verts, unsigned int count, ccColor4F const& fill, float borderWidth,
        ccColor4F const& borderColor, BorderAlignment alignment) {
        if (cbf::g.simulating) {
            return false;
        }
        return CCDrawNode::drawPolygon(verts, count, fill, borderWidth, borderColor, alignment);
    }

    bool drawRect(CCRect const& rect, ccColor4F const& fill, float borderWidth, ccColor4F const& borderColor,
        BorderAlignment alignment) {
        if (cbf::g.simulating) {
            return false;
        }
        return CCDrawNode::drawRect(rect, fill, borderWidth, borderColor, alignment);
    }

    bool drawRect(CCPoint const& origin, CCPoint const& destination, ccColor4F const& fill, float borderWidth,
        ccColor4F const& borderColor, BorderAlignment alignment) {
        if (cbf::g.simulating) {
            return false;
        }
        return CCDrawNode::drawRect(origin, destination, fill, borderWidth, borderColor, alignment);
    }

    bool drawSegment(CCPoint const& from, CCPoint const& to, float radius, ccColor4F const& color) {
        if (cbf::g.simulating) {
            return false;
        }
        return CCDrawNode::drawSegment(from, to, radius, color);
    }

    bool drawLines(CCPoint* verts, unsigned int count, float width, ccColor4F const& color) {
        if (cbf::g.simulating) {
            return false;
        }
        return CCDrawNode::drawLines(verts, count, width, color);
    }

    bool drawCircle(CCPoint const& position, float radius, ccColor4F const& color, float borderWidth,
        ccColor4F const& borderColor, unsigned int segments) {
        if (cbf::g.simulating) {
            return false;
        }
        return CCDrawNode::drawCircle(position, radius, color, borderWidth, borderColor, segments);
    }

    bool drawDot(CCPoint const& position, float radius, ccColor4F const& color) {
        if (cbf::g.simulating) {
            return false;
        }
        return CCDrawNode::drawDot(position, radius, color);
    }
};

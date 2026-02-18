#include "scriptPCH.h"
#include "Utilities/EventMap.h"

#include <map>
#include <vector>
#include <algorithm>
#include <sstream>

enum
{
    NPC_HEAL_DUMMY   = 60002,
    NPC_DAMAGE_DUMMY = 60003
};

enum
{
    GOSSIP_ACTION_BOSS_START  = GOSSIP_ACTION_INFO_DEF + 1,
    GOSSIP_ACTION_BOSS_RESET  = GOSSIP_ACTION_INFO_DEF + 2,
    GOSSIP_ACTION_BOSS_REPORT = GOSSIP_ACTION_INFO_DEF + 3
};

namespace TrainingDummy
{
    static const uint32 kResetAfterIdleMs      = 12000;  // nach 12s ohne Aktion Report+Reset
    static const uint32 kMaxTopEntries         = 5;

    static const uint32 kHealDummyTickMs       = 2000;   // alle 2s clamp
    static const uint32 kHealDummyTargetHpPct  = 35;     // Ziel-HP in %
    static const uint32 kMinHp                 = 1;      // nie sterben

    enum Events
    {
        EVENT_NONE = 0,
        EVENT_HEALDUMMY_CLAMPHP,
        EVENT_BOSS_COUNTDOWN_TICK
    };

    static Player* ResolveOwnerPlayer(Unit* u)
    {
        if (!u)
            return nullptr;

        if (u->IsPlayer())
            return static_cast<Player*>(u);

        if (Player* p = u->GetCharmerOrOwnerPlayerOrPlayerItself())
            return p;

        return nullptr;
    }

    static std::string FormatFloat2(float v)
    {
        std::ostringstream ss;
        ss.setf(std::ios::fixed);
        ss.precision(2);
        ss << v;
        return ss.str();
    }

    struct GuidValue
    {
        ObjectGuid guid;
        uint64 value;

        GuidValue() : guid(ObjectGuid()), value(0) {}
        GuidValue(ObjectGuid g, uint64 v) : guid(g), value(v) {}
    };

    static void BuildTopList(const std::map<ObjectGuid, uint64>& m, std::vector<GuidValue>& out)
    {
        out.clear();
        out.reserve(m.size());

        for (const auto& it : m)
            out.push_back(GuidValue(it.first, it.second));

        std::sort(out.begin(), out.end(), [](const GuidValue& a, const GuidValue& b)
        {
            return a.value > b.value;
        });

        if (out.size() > kMaxTopEntries)
            out.resize(kMaxTopEntries);
    }

    // ---- Stabiler “Dummy bleibt stehen & dreht nicht” Block ----
    static void ForceIdleNoMoveNoThreat(Creature* c)
    {
        if (!c)
            return;

        c->StopMoving();
        c->CombatStop(true);
        c->DeleteThreatList();
        c->ClearInCombat();

        if (c->GetMotionMaster())
        {
            c->GetMotionMaster()->Clear(false);
            c->GetMotionMaster()->MoveIdle();
        }

        c->SetReactState(REACT_PASSIVE);

        // Target “löschen” (damit er nicht anfängt zu verfolgen/targeten)
        c->SetUInt64Value(UNIT_FIELD_TARGET, 0);
    }
}

// ============================================================
// Damage Dummy AI (ScriptName: npc_damage_dummy)
// ============================================================
struct npc_damage_dummyAI : public ScriptedAI
{
    npc_damage_dummyAI(Creature* c) : ScriptedAI(c),
        mActive(false),
        mElapsedMs(0),
        mIdleMs(0),
        mHomeOri(0.0f)
    {
        Reset();
    }

    EventMap mEvents;

    bool   mActive;
    uint32 mElapsedMs;
    uint32 mIdleMs;

    float  mHomeOri;

    std::map<ObjectGuid, uint64> mDamageByPlayer;

    void Reset() override
    {
        mEvents.Reset();

        mActive    = false;
        mElapsedMs = 0;
        mIdleMs    = 0;

        mDamageByPlayer.clear();

        me->SetHealth(me->GetMaxHealth());

        // Home-Orientierung merken (damit er nicht “mitdreht”)
        mHomeOri = me->GetOrientation();

        TrainingDummy::ForceIdleNoMoveNoThreat(me);

        // Damage Dummy ist immer attackbar -> KEIN NonAttackable Flag hier setzen.
        // (sonst kannst du ihn nicht schlagen)
    }

    void StartIfNeeded()
    {
        if (mActive)
            return;

        mActive = true;
        mElapsedMs = 0;
        mIdleMs = 0;
    }

    void ReportAndReset()
    {
        if (!mActive)
        {
            Reset();
            return;
        }

        const float seconds = std::max(1.0f, float(mElapsedMs) / 1000.0f);

        uint64 total = 0;
        for (const auto& it : mDamageByPlayer)
            total += it.second;

        const float dps = float(total) / seconds;

        std::vector<TrainingDummy::GuidValue> top;
        TrainingDummy::BuildTopList(mDamageByPlayer, top);

        {
            std::ostringstream ss;
            ss << "Damage Dummy: " << uint32(seconds) << "s, Total " << total
               << ", DPS " << TrainingDummy::FormatFloat2(dps);

            std::string msg = ss.str();
            me->MonsterTextEmote(msg.c_str(), nullptr);
        }

        for (size_t i = 0; i < top.size(); ++i)
        {
            Player* p = me->GetMap() ? me->GetMap()->GetPlayer(top[i].guid) : nullptr;
            std::string name = p ? p->GetName() : std::string("Unbekannt");

            float pdps = float(top[i].value) / seconds;

            std::ostringstream ss;
            ss << (i + 1) << ". " << name << ": " << top[i].value
               << " (" << TrainingDummy::FormatFloat2(pdps) << " DPS)";

            std::string line = ss.str();
            me->MonsterTextEmote(line.c_str(), nullptr);
        }

        Reset();
    }

    void DamageTaken(Unit* doneBy, uint32& damage) override
    {
        // nie sterben lassen
        if (me->GetHealth() <= TrainingDummy::kMinHp)
        {
            damage = 0;
            return;
        }

        if (damage >= me->GetHealth())
            damage = me->GetHealth() - TrainingDummy::kMinHp;

        if (damage == 0)
            return;

        Player* owner = TrainingDummy::ResolveOwnerPlayer(doneBy);
        if (!owner)
            return;

        StartIfNeeded();
        mIdleMs = 0;

        mDamageByPlayer[owner->GetObjectGuid()] += uint64(damage);

        // wichtig: keine Threat/Verfolgung aufbauen
        TrainingDummy::ForceIdleNoMoveNoThreat(me);
    }

    void UpdateAI(uint32 diff) override
    {
        // IMMER stehen + nicht drehen
        TrainingDummy::ForceIdleNoMoveNoThreat(me);
        me->SetFacingTo(mHomeOri);

        if (!mActive)
            return;

        mElapsedMs += diff;
        mIdleMs    += diff;

        if (mIdleMs >= TrainingDummy::kResetAfterIdleMs)
            ReportAndReset();
    }
};

static CreatureAI* GetAI_npc_damage_dummy(Creature* pCreature)
{
    return new npc_damage_dummyAI(pCreature);
}

// ============================================================
// Heal Dummy AI (ScriptName: npc_heal_dummy)
// ============================================================
struct npc_heal_dummyAI : public ScriptedAI
{
    npc_heal_dummyAI(Creature* c) : ScriptedAI(c),
        mActive(false),
        mElapsedMs(0),
        mIdleMs(0),
        mHomeOri(0.0f)
    {
        Reset();
    }

    EventMap mEvents;

    bool   mActive;
    uint32 mElapsedMs;
    uint32 mIdleMs;

    float  mHomeOri;

    std::map<ObjectGuid, uint64> mHealByPlayer;

    void ClampToTargetHP()
    {
        const uint32 maxHp = me->GetMaxHealth();
        if (maxHp == 0)
            return;

        uint32 target = (maxHp * TrainingDummy::kHealDummyTargetHpPct) / 100;
        if (target < TrainingDummy::kMinHp)
            target = TrainingDummy::kMinHp;

        // zu hoch -> runter, zu tief -> lassen
        if (me->GetHealth() > target)
            me->SetHealth(target);

        if (me->GetHealth() < TrainingDummy::kMinHp)
            me->SetHealth(TrainingDummy::kMinHp);
    }

    void Reset() override
    {
        mEvents.Reset();

        mActive    = false;
        mElapsedMs = 0;
        mIdleMs    = 0;

        mHealByPlayer.clear();

        mHomeOri = me->GetOrientation();

        ClampToTargetHP();

        TrainingDummy::ForceIdleNoMoveNoThreat(me);

        mEvents.ScheduleEvent(TrainingDummy::EVENT_HEALDUMMY_CLAMPHP, TrainingDummy::kHealDummyTickMs);
    }

    void StartIfNeeded()
    {
        if (mActive)
            return;

        mActive = true;
        mElapsedMs = 0;
        mIdleMs = 0;
    }

    void ReportAndReset()
    {
        if (!mActive)
        {
            Reset();
            return;
        }

        const float seconds = std::max(1.0f, float(mElapsedMs) / 1000.0f);

        uint64 total = 0;
        for (const auto& it : mHealByPlayer)
            total += it.second;

        const float hps = float(total) / seconds;

        std::vector<TrainingDummy::GuidValue> top;
        TrainingDummy::BuildTopList(mHealByPlayer, top);

        {
            std::ostringstream ss;
            ss << "Heal Dummy: " << uint32(seconds) << "s, Total " << total
               << ", HPS " << TrainingDummy::FormatFloat2(hps);

            std::string msg = ss.str();
            me->MonsterTextEmote(msg.c_str(), nullptr);
        }

        for (size_t i = 0; i < top.size(); ++i)
        {
            Player* p = me->GetMap() ? me->GetMap()->GetPlayer(top[i].guid) : nullptr;
            std::string name = p ? p->GetName() : std::string("Unbekannt");

            float phps = float(top[i].value) / seconds;

            std::ostringstream ss;
            ss << (i + 1) << ". " << name << ": " << top[i].value
               << " (" << TrainingDummy::FormatFloat2(phps) << " HPS)";

            std::string line = ss.str();
            me->MonsterTextEmote(line.c_str(), nullptr);
        }

        Reset();
    }

    // wird bei dir so gecallt: AI()->HealedBy(pUnit, addhealth) (uint32 by value)
    void HealedBy(Unit* healer, uint32 heal)
    {
        if (heal == 0)
            return;

        Player* owner = TrainingDummy::ResolveOwnerPlayer(healer);
        if (!owner)
            return;

        StartIfNeeded();
        mIdleMs = 0;

        mHealByPlayer[owner->GetObjectGuid()] += uint64(heal);

        TrainingDummy::ForceIdleNoMoveNoThreat(me);
    }

    void DamageTaken(Unit* /*doneBy*/, uint32& damage) override
    {
        // Heal dummy soll nicht sterben und nicht “kämpfen”
        if (me->GetHealth() <= TrainingDummy::kMinHp)
        {
            damage = 0;
            return;
        }

        if (damage >= me->GetHealth())
            damage = me->GetHealth() - TrainingDummy::kMinHp;

        TrainingDummy::ForceIdleNoMoveNoThreat(me);
    }

    void UpdateAI(uint32 diff) override
    {
        // IMMER stehen + nicht drehen
        TrainingDummy::ForceIdleNoMoveNoThreat(me);
        me->SetFacingTo(mHomeOri);

        mEvents.Update(diff);
        while (uint32 ev = mEvents.ExecuteEvent())
        {
            if (ev == TrainingDummy::EVENT_HEALDUMMY_CLAMPHP)
            {
                ClampToTargetHP();
                mEvents.ScheduleEvent(TrainingDummy::EVENT_HEALDUMMY_CLAMPHP, TrainingDummy::kHealDummyTickMs);
            }
        }

        if (!mActive)
            return;

        mElapsedMs += diff;
        mIdleMs    += diff;

        if (mIdleMs >= TrainingDummy::kResetAfterIdleMs)
            ReportAndReset();
    }
};

static CreatureAI* GetAI_npc_heal_dummy(Creature* pCreature)
{
    return new npc_heal_dummyAI(pCreature);
}

// ============================================================
// Boss Dummy AI (ScriptName: npc_boss_dummy) + Gossip
// ============================================================
struct npc_boss_dummyAI : public ScriptedAI
{
    npc_boss_dummyAI(Creature* c) : ScriptedAI(c),
        mCountingDown(false),
        mActive(false),
        mCountdownLeft(0),
        mElapsedMs(0),
        mIdleMs(0),
        mHomeOri(0.0f)
    {
        Reset();
    }

    EventMap mEvents;

    bool   mCountingDown;
    bool   mActive;
    uint32 mCountdownLeft;

    uint32 mElapsedMs;
    uint32 mIdleMs;

    float  mHomeOri;

    std::map<ObjectGuid, uint64> mDamageByPlayer;

    void SetBossIdleState()
    {
        // Gossip ON (npc_flags=1), nicht attackbar, aber anklickbar
        me->SetUInt32Value(UNIT_NPC_FLAGS, 1);

        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE_2);
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_ATTACKABLE_1);

        TrainingDummy::ForceIdleNoMoveNoThreat(me);
        me->SetFacingTo(mHomeOri);
    }

    void SetBossCombatState()
    {
        // Gossip OFF (npc_flags=0), attackbar
        me->SetUInt32Value(UNIT_NPC_FLAGS, 0);

        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE_2);
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_ATTACKABLE_1);
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);

        // trotzdem nicht bewegen/targeten
        TrainingDummy::ForceIdleNoMoveNoThreat(me);
        me->SetFacingTo(mHomeOri);
    }

    void Reset() override
    {
        mEvents.Reset();

        mCountingDown  = false;
        mActive        = false;
        mCountdownLeft = 0;

        mElapsedMs     = 0;
        mIdleMs        = 0;

        mDamageByPlayer.clear();

        me->SetHealth(me->GetMaxHealth());

        mHomeOri = me->GetOrientation();

        SetBossIdleState();
    }

    void StartCountdown(uint32 seconds)
    {
        Reset();

        mCountingDown  = true;
        mCountdownLeft = (seconds > 0 ? seconds : 1);

        {
            std::ostringstream ss;
            ss << "Boss Dummy: Pull in " << mCountdownLeft << " Sekunden.";
            std::string msg = ss.str();
            me->MonsterTextEmote(msg.c_str(), nullptr);
        }

        mEvents.ScheduleEvent(TrainingDummy::EVENT_BOSS_COUNTDOWN_TICK, 1000);
    }

    void BeginCombatTracking()
    {
        mCountingDown = false;
        mActive = true;

        mElapsedMs = 0;
        mIdleMs    = 0;

        SetBossCombatState();
        me->MonsterTextEmote("Boss Dummy: GO!", nullptr);
    }

    void Report()
    {
        const float seconds = std::max(1.0f, float(mElapsedMs) / 1000.0f);

        uint64 total = 0;
        for (const auto& it : mDamageByPlayer)
            total += it.second;

        const float dps = float(total) / seconds;

        std::vector<TrainingDummy::GuidValue> top;
        TrainingDummy::BuildTopList(mDamageByPlayer, top);

        {
            std::ostringstream ss;
            ss << "Boss Dummy: " << uint32(seconds) << "s, Total " << total
               << ", DPS " << TrainingDummy::FormatFloat2(dps);

            std::string msg = ss.str();
            me->MonsterTextEmote(msg.c_str(), nullptr);
        }

        for (size_t i = 0; i < top.size(); ++i)
        {
            Player* p = me->GetMap() ? me->GetMap()->GetPlayer(top[i].guid) : nullptr;
            std::string name = p ? p->GetName() : std::string("Unbekannt");

            float pdps = float(top[i].value) / seconds;

            std::ostringstream ss;
            ss << (i + 1) << ". " << name << ": " << top[i].value
               << " (" << TrainingDummy::FormatFloat2(pdps) << " DPS)";

            std::string line = ss.str();
            me->MonsterTextEmote(line.c_str(), nullptr);
        }
    }

    void FinishAndReset()
    {
        if (mActive)
            Report();

        Reset();
    }

    void DamageTaken(Unit* doneBy, uint32& damage) override
    {
        // Countdown/Idle: komplett “unhittable” (kein Combat-Gedöns)
        if (!mActive)
        {
            damage = 0;
            return;
        }

        // nie sterben
        if (me->GetHealth() <= TrainingDummy::kMinHp)
        {
            damage = 0;
            return;
        }

        if (damage >= me->GetHealth())
            damage = me->GetHealth() - TrainingDummy::kMinHp;

        if (damage == 0)
            return;

        Player* owner = TrainingDummy::ResolveOwnerPlayer(doneBy);
        if (!owner)
            return;

        mIdleMs = 0;
        mDamageByPlayer[owner->GetObjectGuid()] += uint64(damage);

        // kein Threat / kein Drehen / kein Move
        TrainingDummy::ForceIdleNoMoveNoThreat(me);
        me->SetFacingTo(mHomeOri);
    }

    void UpdateAI(uint32 diff) override
    {
        // IMMER fix (auch im Countdown)
        if (!mActive)
        {
            SetBossIdleState();
        }
        else
        {
            SetBossCombatState();
        }

        mEvents.Update(diff);
        while (uint32 ev = mEvents.ExecuteEvent())
        {
            if (ev == TrainingDummy::EVENT_BOSS_COUNTDOWN_TICK)
            {
                if (!mCountingDown)
                    continue;

                if (mCountdownLeft > 1)
                {
                    --mCountdownLeft;

                    std::ostringstream ss;
                    ss << "Boss Dummy: Pull in " << mCountdownLeft << " Sekunden.";
                    std::string msg = ss.str();
                    me->MonsterTextEmote(msg.c_str(), nullptr);

                    mEvents.ScheduleEvent(TrainingDummy::EVENT_BOSS_COUNTDOWN_TICK, 1000);
                }
                else
                {
                    BeginCombatTracking();
                }
            }
        }

        if (!mActive)
            return;

        mElapsedMs += diff;
        mIdleMs    += diff;

        if (mIdleMs >= TrainingDummy::kResetAfterIdleMs)
            FinishAndReset();
    }
};

static CreatureAI* GetAI_npc_boss_dummy(Creature* pCreature)
{
    return new npc_boss_dummyAI(pCreature);
}

static bool GossipHello_npc_boss_dummy(Player* pPlayer, Creature* pCreature)
{
    if (!pPlayer || !pCreature)
        return true;

    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Start (10s Pull)", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_START);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Report (jetzt)",   GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_REPORT);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Reset",           GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_RESET);

    pPlayer->SEND_GOSSIP_MENU(DEFAULT_GOSSIP_MESSAGE, pCreature->GetObjectGuid());
    return true;
}

static bool GossipSelect_npc_boss_dummy(Player* pPlayer, Creature* pCreature, uint32 /*sender*/, uint32 action)
{
    if (!pPlayer || !pCreature)
        return true;

    pPlayer->CLOSE_GOSSIP_MENU();

    npc_boss_dummyAI* ai = dynamic_cast<npc_boss_dummyAI*>(pCreature->AI());
    if (!ai)
        return true;

    switch (action)
    {
        case GOSSIP_ACTION_BOSS_START:
            ai->StartCountdown(10);
            break;

        case GOSSIP_ACTION_BOSS_REPORT:
            ai->Report();
            break;

        case GOSSIP_ACTION_BOSS_RESET:
            ai->Reset();
            pCreature->MonsterTextEmote("Boss Dummy: Reset.", nullptr);
            break;

        default:
            break;
    }

    return true;
}

// ============================================================
// Registration (ScriptDev2 / vMaNGOS Style)
// ============================================================
void AddSC_npc_training_dummies()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "npc_damage_dummy";
    newscript->GetAI = &GetAI_npc_damage_dummy;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "npc_heal_dummy";
    newscript->GetAI = &GetAI_npc_heal_dummy;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "npc_boss_dummy";
    newscript->GetAI = &GetAI_npc_boss_dummy;
    newscript->pGossipHello = &GossipHello_npc_boss_dummy;
    newscript->pGossipSelect = &GossipSelect_npc_boss_dummy;
    newscript->RegisterSelf();
}

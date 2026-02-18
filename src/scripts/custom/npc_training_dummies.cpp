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

namespace TrainingDummy
{
    static const uint32 kResetAfterIdleMs      = 12000;
    static const uint32 kAnnounceEveryMs       = 0;
    static const uint32 kMaxTopEntries         = 5;

    static const uint32 kHealDummyTickMs       = 2000;
    static const uint32 kHealDummyTargetHpPct  = 35;
    static const uint32 kHealDummyMinHp        = 1;

    static const uint32 kDamageDummyMinHp      = 1;

    enum Events
    {
        EVENT_NONE = 0,
        EVENT_REPORT_TICK,
        EVENT_HEALDUMMY_CLAMPHP
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
}

// ============================================================
// Damage Dummy AI (Entry 60003)
// ============================================================
struct npc_damage_dummyAI : public ScriptedAI
{
    npc_damage_dummyAI(Creature* c) : ScriptedAI(c)
    {
        Reset();
    }

    EventMap mEvents;

    bool   mActive;
    uint32 mElapsedMs;
    uint32 mIdleMs;

    std::map<ObjectGuid, uint64> mDamageByPlayer;

    void Reset() override
    {
        mEvents.Reset();
        mActive    = false;
        mElapsedMs = 0;
        mIdleMs    = 0;
        mDamageByPlayer.clear();

        me->SetHealth(me->GetMaxHealth());
    }

    void StartIfNeeded()
    {
        if (mActive)
            return;

        mActive = true;
        mElapsedMs = 0;
        mIdleMs = 0;

        if (TrainingDummy::kAnnounceEveryMs > 0)
            mEvents.ScheduleEvent(TrainingDummy::EVENT_REPORT_TICK, TrainingDummy::kAnnounceEveryMs);
    }

    void FinishAndReset()
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
            ss << "Damage Dummy Report: Dauer " << uint32(seconds) << "s, Total "
               << total << ", DPS " << TrainingDummy::FormatFloat2(dps);

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
        if (me->GetHealth() <= TrainingDummy::kDamageDummyMinHp)
        {
            damage = 0;
            return;
        }

        if (damage >= me->GetHealth())
            damage = me->GetHealth() - TrainingDummy::kDamageDummyMinHp;

        if (damage == 0)
            return;

        Player* owner = TrainingDummy::ResolveOwnerPlayer(doneBy);
        if (!owner)
            return;

        StartIfNeeded();
        mIdleMs = 0;

        mDamageByPlayer[owner->GetObjectGuid()] += uint64(damage);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!mActive)
            return;

        mElapsedMs += diff;
        mIdleMs    += diff;

        mEvents.Update(diff);
        while (uint32 ev = mEvents.ExecuteEvent())
        {
            switch (ev)
            {
                case TrainingDummy::EVENT_REPORT_TICK:
                    if (TrainingDummy::kAnnounceEveryMs > 0)
                        mEvents.ScheduleEvent(TrainingDummy::EVENT_REPORT_TICK, TrainingDummy::kAnnounceEveryMs);
                    break;
                default:
                    break;
            }
        }

        if (mIdleMs >= TrainingDummy::kResetAfterIdleMs)
            FinishAndReset();
    }
};

static CreatureAI* GetAI_npc_damage_dummy(Creature* pCreature)
{
    return new npc_damage_dummyAI(pCreature);
}

// ============================================================
// Heal Dummy AI (Entry 60002)
// ============================================================
struct npc_heal_dummyAI : public ScriptedAI
{
    npc_heal_dummyAI(Creature* c) : ScriptedAI(c)
    {
        Reset();
    }

    EventMap mEvents;

    bool   mActive;
    uint32 mElapsedMs;
    uint32 mIdleMs;

    std::map<ObjectGuid, uint64> mHealByPlayer;

    void ClampToTargetHP()
    {
        const uint32 maxHp = me->GetMaxHealth();
        if (maxHp == 0)
            return;

        uint32 target = (maxHp * TrainingDummy::kHealDummyTargetHpPct) / 100;
        if (target < TrainingDummy::kHealDummyMinHp)
            target = TrainingDummy::kHealDummyMinHp;

        if (me->GetHealth() > target)
            me->SetHealth(target);

        if (me->GetHealth() < TrainingDummy::kHealDummyMinHp)
            me->SetHealth(TrainingDummy::kHealDummyMinHp);
    }

    void Reset() override
    {
        mEvents.Reset();
        mActive    = false;
        mElapsedMs = 0;
        mIdleMs    = 0;
        mHealByPlayer.clear();

        ClampToTargetHP();
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

    void FinishAndReset()
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
            ss << "Heal Dummy Report: Dauer " << uint32(seconds) << "s, Total "
               << total << ", HPS " << TrainingDummy::FormatFloat2(hps);

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

    // WICHTIG: bei dir wird so gecallt: AI()->HealedBy(pUnit, addhealth) (uint32 BY VALUE)
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
    }

    void DamageTaken(Unit* /*doneBy*/, uint32& damage) override
    {
        if (me->GetHealth() <= TrainingDummy::kHealDummyMinHp)
        {
            damage = 0;
            return;
        }

        if (damage >= me->GetHealth())
            damage = me->GetHealth() - TrainingDummy::kHealDummyMinHp;
    }

    void UpdateAI(uint32 diff) override
    {
        mEvents.Update(diff);
        while (uint32 ev = mEvents.ExecuteEvent())
        {
            switch (ev)
            {
                case TrainingDummy::EVENT_HEALDUMMY_CLAMPHP:
                    ClampToTargetHP();
                    mEvents.ScheduleEvent(TrainingDummy::EVENT_HEALDUMMY_CLAMPHP, TrainingDummy::kHealDummyTickMs);
                    break;
                default:
                    break;
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

static CreatureAI* GetAI_npc_heal_dummy(Creature* pCreature)
{
    return new npc_heal_dummyAI(pCreature);
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
}

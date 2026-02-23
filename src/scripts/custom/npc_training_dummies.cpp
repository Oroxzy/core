#include "scriptPCH.h"
#include "Utilities/EventMap.h"

#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <ctime>

enum
{
    NPC_HEAL_DUMMY   = 60002,
    NPC_DAMAGE_DUMMY = 60003
};

enum
{
    GOSSIP_ACTION_BOSS_START_1M  = GOSSIP_ACTION_INFO_DEF + 1,
    GOSSIP_ACTION_BOSS_START_3M  = GOSSIP_ACTION_INFO_DEF + 2,
    GOSSIP_ACTION_BOSS_START_5M  = GOSSIP_ACTION_INFO_DEF + 3,
    GOSSIP_ACTION_BOSS_START_10M = GOSSIP_ACTION_INFO_DEF + 4,
    GOSSIP_ACTION_BOSS_RESET     = GOSSIP_ACTION_INFO_DEF + 5,
    GOSSIP_ACTION_BOSS_REPORT    = GOSSIP_ACTION_INFO_DEF + 6
};

namespace TrainingDummy
{
    // ------------------------------------------------------------
    // Zentrale Konstanten
    // ------------------------------------------------------------
    static const uint32 kResetAfterIdleMs   = 10000;   // nach X ms ohne Aktivitaet -> Report + Reset   // nach X ms ohne Aktivitaet -> Report + Reset
    static const uint32 kMinFightMs         = 2500;    // Puffer: verhindert "sofort reset" bei kurzem Antippen
    static const uint32 kAnnounceEveryMs    = 0;       // 0 = keine Zwischen-Ansagen
    static const uint32 kMaxTopEntries      = 5;

    static const uint32 kHealDummyTickMs      = 2000;
    static const uint32 kHealDummyTargetHpPct = 35;
    static const uint32 kHealDummyMinHp       = 1;

    static const uint32 kDamageDummyMinHp     = 1;

    static const uint32 kSoundCountdownTick = 116;
    static const uint32 kSoundCountdownGo   = 8232; // <- setz hier deine Wunsch-SoundId

    // DU wolltest 12 Sekunden:
    static const uint32 kKickPlayerAfterIdleMs = 10000;

    // Sweep-Takt wie im alten Script (Combat/Attacker Cleanup nicht jede Sekunde)
    static const uint32 kKickSweepIntervalMs  = 15000;

    // Rote Combat-Aura (wie dein alter Dummy). Falls deine SpellId anders ist: hier aendern.
    static const uint32 kCombatAuraSpellId = 31309;

    enum Events
    {
        EVENT_NONE = 0,
        EVENT_REPORT_TICK,
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

    // NUR einfrieren / nicht drehen (KEIN CombatStop / KEIN Threat-Reset!)
    static void FreezeInPlace(Creature* me, float homeOri)
    {
        if (!me)
            return;

        me->StopMoving();

        if (me->GetMotionMaster())
        {
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveIdle();
        }

        me->SetFacingTo(homeOri);
    }

    // Combat stabil halten (sonst dropt Combat gern bei passivem Dummy)
    static void EnsureCombat(Creature* me, Unit* attackerOrHealer)
    {
        if (!me || !attackerOrHealer)
            return;

        me->SetInCombatWith(attackerOrHealer);
        attackerOrHealer->SetInCombatWith(me);

        // Mini-Threat reicht, damit Combat nicht sofort weg ist
        me->AddThreat(attackerOrHealer, 1.0f);
    }

    static void SetCombatAura(Creature* me, bool active)
    {
        if (!me)
            return;

        if (kCombatAuraSpellId == 0)
            return;

        if (active)
        {
            if (!me->HasAura(kCombatAuraSpellId))
                me->CastSpell(me, kCombatAuraSpellId, true);
        }
        else
        {
            if (me->HasAura(kCombatAuraSpellId))
                me->RemoveAurasDueToSpell(kCombatAuraSpellId);
        }
    }

    static void RemoveGossip(Creature* me)
    {
        if (!me)
            return;

        // npcflag live wegnehmen (DB npcflag bleibt trotzdem)
        me->SetUInt32Value(UNIT_NPC_FLAGS, 0);
    }

    static void RestoreGossip(Creature* me, uint32 npcFlags)
    {
        if (!me)
            return;

        me->SetUInt32Value(UNIT_NPC_FLAGS, npcFlags);
    }
    // ------------------------------------------------------------
    // "Sickes Programm": Debuffs/Buffs wie im alten Attack-Dummy
    // ------------------------------------------------------------
    static void DummyApplyDebuffAura(Creature* me, uint32 spellId)
    {
        if (!me || spellId == 0)
            return;

        if (SpellAuraHolder* pHolder = me->AddAura(spellId))
        {
            switch (spellId)
            {
                case 16928: // Armor Shatter
                    pHolder->SetStackAmount(3);
                    break;
                case 11597: // Sunder Armor
                case 12579: // Winter's Chill
                case 15258: // Shadow Vulnerability (Priest)
                case 22959: // Improved Scorch
                    pHolder->SetStackAmount(5);
                    break;
                default:
                    break;
            }

            pHolder->UpdateAuraDuration();
            pHolder->SetPermanent(true);
            pHolder->SetCasterGuid(me->GetGUID() + urand(1, 999999));
        }
    }

    static void ApplyImprovedDebuffAura(Unit* unit, Creature* creature, uint32 spellId)
    {
        if (!unit || !creature || spellId == 0)
            return;

        enum Talents
        {
            IMPROVED_HUNTERS_MARK_RANK5          = 19425,
            IMPROVED_SEAL_OF_THE_CRUSADER_RANK3  = 20337,
        };

        const uint32 talentspells[2] =
        {
            IMPROVED_HUNTERS_MARK_RANK5,
            IMPROVED_SEAL_OF_THE_CRUSADER_RANK3
        };

        unit->RemoveAurasDueToSpell(spellId);

        // temporär "beste Ränge" als Aura geben (wie alt), dann Debuff setzen, danach wieder weg
        if (unit->IsPlayer())
        {
            Player* p = unit->ToPlayer();
            for (uint32 i = 0; i < 2; ++i)
            {
                if (!p->HasSpell(talentspells[i]) && !p->HasAura(talentspells[i]))
                    p->AddAura(talentspells[i]);
            }
        }

        creature->AddAura(spellId, ADD_AURA_PERMANENT, unit);

        if (unit->IsPlayer())
        {
            Player* p = unit->ToPlayer();
            for (uint32 i = 0; i < 2; ++i)
            {
                if (!p->HasSpell(talentspells[i]) && p->HasAura(talentspells[i]))
                    p->RemoveAurasDueToSpell(talentspells[i]);
            }
        }
    }

    static void ApplyBuffsAndDebuffs(Creature* me)
    {
        if (!me)
            return;

        if (!me->IsInCombat())
            return;

        // wie alt: Debuffs nur auf Elite + Boss-Dummies
        if (!me->IsElite())
            return;

        switch (me->GetEntry())
        {
            case 60003: // caster mob
                DummyApplyDebuffAura(me, 21992); // Thunderfury
                DummyApplyDebuffAura(me, 12579); // Winter's Chill
                DummyApplyDebuffAura(me, 17937); // Curse of Shadow
                DummyApplyDebuffAura(me, 17800); // Shadow Vulnerability (Warlock)
                DummyApplyDebuffAura(me, 15258); // Shadow Vulnerability (Priest)
                DummyApplyDebuffAura(me, 11722); // Curse of the Elements
                DummyApplyDebuffAura(me, 22959); // Improved Scorch
                DummyApplyDebuffAura(me, 23605); // Spell Vulnerability (Nightfall)
                break;

            case 60002: // melee mob
                DummyApplyDebuffAura(me, 21992); // Thunderfury
                DummyApplyDebuffAura(me, 11374); // Gift of Arthas
                DummyApplyDebuffAura(me, 9907);  // Faerie Fire
                DummyApplyDebuffAura(me, 11597); // Sunder Armor
                DummyApplyDebuffAura(me, 16928); // Armor Shatter
                DummyApplyDebuffAura(me, 11717); // Curse of Recklessness
                DummyApplyDebuffAura(me, 23577); // Expose Weakness
                break;

            default:
                break;
        }
    }

    static void ApplyBossDebuffs(Creature* me)
    {
        // Zweck: Boss-Dummy bekommt die "vollen" Debuffs wie im alten Attack-Dummy.
        // Hinweis: Permanent Auren -> einmal setzen reicht, wir sweepten trotzdem periodisch falls Aura-Limits greifen.
        if (!me)
            return;

        if (!me->IsInCombat())
            return;

        if (!me->IsElite())
            return;

        // Union aus melee + caster (alte Datei hatte teilweise doppelte Eintraege)
        DummyApplyDebuffAura(me, 21992); // Thunderfury
        DummyApplyDebuffAura(me, 11374); // Gift of Arthas
        DummyApplyDebuffAura(me, 9907);  // Faerie Fire
        DummyApplyDebuffAura(me, 11597); // Sunder Armor
        DummyApplyDebuffAura(me, 16928); // Armor Shatter
        DummyApplyDebuffAura(me, 11717); // Curse of Recklessness
        DummyApplyDebuffAura(me, 23577); // Expose Weakness

        DummyApplyDebuffAura(me, 12579); // Winter's Chill
        DummyApplyDebuffAura(me, 17937); // Curse of Shadow
        DummyApplyDebuffAura(me, 17800); // Shadow Vulnerability (Warlock)
        DummyApplyDebuffAura(me, 15258); // Shadow Vulnerability (Priest)
        DummyApplyDebuffAura(me, 11722); // Curse of the Elements
        DummyApplyDebuffAura(me, 22959); // Improved Scorch
        DummyApplyDebuffAura(me, 23605); // Spell Vulnerability (Nightfall)
    }


}

// ============================================================
// Damage Dummy AI (ScriptName: npc_damage_dummy)
// ============================================================
struct npc_damage_dummyAI : public ScriptedAI
{
    npc_damage_dummyAI(Creature* c) : ScriptedAI(c),
        mActive(false), mElapsedMs(0), mIdleMs(0), mHomeOri(0.0f),
        mKickSweepMs(0), mDebuffSweepMs(0)
    {
        Reset();
    }

    EventMap mEvents;

    bool   mActive;
    uint32 mElapsedMs;
    uint32 mIdleMs;

    float  mHomeOri;

    uint32 mKickSweepMs;

    uint32 mDebuffSweepMs;

    std::map<ObjectGuid, uint64>  mDamageByPlayer;
    std::map<ObjectGuid, time_t>  mLastActivityTs; // letzte Aktivitaet als epoch-seconds

    void Reset() override
    {
        mEvents.Reset();

        mActive      = false;
        mElapsedMs   = 0;
        mIdleMs      = 0;
        mKickSweepMs = 0;
        mDebuffSweepMs = 0;

        mDamageByPlayer.clear();
        mLastActivityTs.clear();

        if (me)
        {
            mHomeOri = me->GetOrientation();
            me->SetHealth(me->GetMaxHealth());

            me->SetReactState(REACT_PASSIVE);
            SetCombatMovement(false);

            // Reset darf Combat/Threat aufraeumen
            me->CombatStop(true);
            me->DeleteThreatList();
            me->AttackStop();
            me->SetTargetGuid(ObjectGuid());
            me->ClearInCombat();

            TrainingDummy::SetCombatAura(me, false);
            TrainingDummy::FreezeInPlace(me, mHomeOri);
        }
    }

    void StartIfNeeded()
    {
        if (mActive)
            return;

        mActive    = true;
        mElapsedMs = 0;
        mIdleMs    = 0;

        TrainingDummy::SetCombatAura(me, true);

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
        TrainingDummy::FreezeInPlace(me, mHomeOri);

        // Nie sterben
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

        TrainingDummy::EnsureCombat(me, doneBy);

        // Aktivitaet aktualisieren (epoch)
        mLastActivityTs[owner->GetObjectGuid()] = std::time(nullptr);

        mIdleMs = 0;
        mDamageByPlayer[owner->GetObjectGuid()] += uint64(damage);
    }

    void KickInactivePlayers()
    {
        if (!me)
            return;

        if (mLastActivityTs.empty())
            return;

        const time_t now = std::time(nullptr);

        for (auto it = mLastActivityTs.begin(); it != mLastActivityTs.end(); )
        {
            Unit* u = me->GetMap() ? me->GetMap()->GetUnit(it->first) : nullptr;

            if (!u || !u->IsInWorld())
            {
                it = mLastActivityTs.erase(it);
                continue;
            }

            if ((now - it->second) * 1000 >= TrainingDummy::kKickPlayerAfterIdleMs)
            {
                if (u->IsPlayer())
                {
                    Player* p = u->ToPlayer();

                    // WIE IM ALTEN SCRIPT: sonst bleibt Player haengen
                    p->CombatStopWithPets(true);
                    p->CombatStop(true);

                    // Dummy-seitig Attacker/Threat sauber entfernen
                    me->_removeAttacker(u);
                }

                it = mLastActivityTs.erase(it);
                continue;
            }

            ++it;
        }

        // Wenn keiner mehr dran ist: Dummy wirklich raus aus Combat
        if (mLastActivityTs.empty())
        {
            me->DeleteThreatList();
            me->AttackStop();
            me->SetTargetGuid(ObjectGuid());
            me->ClearInCombat();

            TrainingDummy::SetCombatAura(me, false);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        TrainingDummy::FreezeInPlace(me, mHomeOri);

        // Zeit laeuft immer
        mElapsedMs += diff;

        // Sweep wie im alten Script (15s)
        if (mKickSweepMs <= diff)
        {
            KickInactivePlayers();
            mKickSweepMs = TrainingDummy::kKickSweepIntervalMs;
        }
        else
            mKickSweepMs -= diff;

        if (!mActive)
            return;

        mIdleMs += diff;

        if (mIdleMs >= TrainingDummy::kResetAfterIdleMs && mElapsedMs >= TrainingDummy::kMinFightMs)
            FinishAndReset();
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
        mActive(false), mElapsedMs(0), mIdleMs(0), mHomeOri(0.0f),
        mKickSweepMs(0)
    {
        Reset();
    }

    EventMap mEvents;

    bool   mActive;
    uint32 mElapsedMs;
    uint32 mIdleMs;

    float  mHomeOri;

    uint32 mKickSweepMs;

    std::map<ObjectGuid, uint64>  mHealByPlayer;
    std::map<ObjectGuid, time_t>  mLastActivityTs;

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

        mActive      = false;
        mElapsedMs   = 0;
        mIdleMs      = 0;
        mKickSweepMs = 0;

        mHealByPlayer.clear();
        mLastActivityTs.clear();

        if (!me)
            return;

        mHomeOri = me->GetOrientation();

        me->SetReactState(REACT_PASSIVE);
        SetCombatMovement(false);

        me->CombatStop(true);
        me->DeleteThreatList();
        me->AttackStop();
        me->SetTargetGuid(ObjectGuid());
        me->ClearInCombat();

        TrainingDummy::SetCombatAura(me, false);

        ClampToTargetHP();
        mEvents.ScheduleEvent(TrainingDummy::EVENT_HEALDUMMY_CLAMPHP, TrainingDummy::kHealDummyTickMs);

        TrainingDummy::FreezeInPlace(me, mHomeOri);
    }

    void StartIfNeeded()
    {
        if (mActive)
            return;

        mActive    = true;
        mElapsedMs = 0;
        mIdleMs    = 0;

        TrainingDummy::SetCombatAura(me, true);
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

    void HealedBy(Unit* healer, uint32 heal)
    {
        TrainingDummy::FreezeInPlace(me, mHomeOri);

        if (heal == 0)
            return;

        Player* owner = TrainingDummy::ResolveOwnerPlayer(healer);
        if (!owner)
            return;

        StartIfNeeded();

        TrainingDummy::EnsureCombat(me, healer);

        mLastActivityTs[owner->GetObjectGuid()] = std::time(nullptr);

        mIdleMs = 0;
        mHealByPlayer[owner->GetObjectGuid()] += uint64(heal);
    }

    void DamageTaken(Unit* /*doneBy*/, uint32& damage) override
    {
        TrainingDummy::FreezeInPlace(me, mHomeOri);

        if (me->GetHealth() <= TrainingDummy::kHealDummyMinHp)
        {
            damage = 0;
            return;
        }

        if (damage >= me->GetHealth())
            damage = me->GetHealth() - TrainingDummy::kHealDummyMinHp;
    }

    void KickInactivePlayers()
    {
        if (!me)
            return;

        if (mLastActivityTs.empty())
            return;

        const time_t now = std::time(nullptr);

        for (auto it = mLastActivityTs.begin(); it != mLastActivityTs.end(); )
        {
            Unit* u = me->GetMap() ? me->GetMap()->GetUnit(it->first) : nullptr;

            if (!u || !u->IsInWorld())
            {
                it = mLastActivityTs.erase(it);
                continue;
            }

            if ((now - it->second) * 1000 >= TrainingDummy::kKickPlayerAfterIdleMs)
            {
                if (u->IsPlayer())
                {
                    Player* p = u->ToPlayer();
                    p->CombatStopWithPets(true);
                    p->CombatStop(true);
                    me->_removeAttacker(u);
                }

                it = mLastActivityTs.erase(it);
                continue;
            }

            ++it;
        }

        if (mLastActivityTs.empty())
        {
            me->DeleteThreatList();
            me->AttackStop();
            me->SetTargetGuid(ObjectGuid());
            me->ClearInCombat();
            TrainingDummy::SetCombatAura(me, false);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        TrainingDummy::FreezeInPlace(me, mHomeOri);

        mEvents.Update(diff);
        while (uint32 ev = mEvents.ExecuteEvent())
        {
            if (ev == TrainingDummy::EVENT_HEALDUMMY_CLAMPHP)
            {
                ClampToTargetHP();
                mEvents.ScheduleEvent(TrainingDummy::EVENT_HEALDUMMY_CLAMPHP, TrainingDummy::kHealDummyTickMs);
            }
        }

        mElapsedMs += diff;

        if (mKickSweepMs <= diff)
        {
            KickInactivePlayers();
            mKickSweepMs = TrainingDummy::kKickSweepIntervalMs;
        }
        else
            mKickSweepMs -= diff;

        // Idle -> Fight Ende (wie Damage Dummy)
        if (mLastActivityTs.empty())
            mIdleMs += diff;
        else
            mIdleMs = 0;

        if (mIdleMs >= TrainingDummy::kResetAfterIdleMs && mElapsedMs >= TrainingDummy::kMinFightMs)
            FinishAndReset();
    }
};

static CreatureAI* GetAI_npc_heal_dummy(Creature* pCreature)
{
    return new npc_heal_dummyAI(pCreature);
}
// Boss Dummy AI (ScriptName: npc_boss_dummy) + Gossip
// ============================================================
struct npc_boss_dummyAI : public ScriptedAI
{
    npc_boss_dummyAI(Creature* c) : ScriptedAI(c),
        mCountingDown(false), mActive(false), mCountdownLeft(0),
        mFightDurationMs(0), mFightStarted(false), mFightElapsedMs(0),
        mElapsedMs(0), mIdleMs(0), mHomeOri(0.0f), mNpcFlagsOriginal(1),
        mKickSweepMs(0), mDebuffSweepMs(0)
    {
        Reset();
    }

    EventMap mEvents;

    bool   mCountingDown;
    bool   mActive;
    uint32 mCountdownLeft;

    uint32 mFightDurationMs;   // Auswahl via Gossip (1/3/5/10 Min)
    bool   mFightStarted;      // startet erst beim ersten Hit/Aggro
    uint32 mFightElapsedMs;    // laeuft nur wenn mFightStarted

    uint32 mElapsedMs;         // weiterhin fuer Report/Legacy (falls gebraucht)
    uint32 mIdleMs;

    float  mHomeOri;

    uint32 mNpcFlagsOriginal;

    uint32 mKickSweepMs;
    uint32 mDebuffSweepMs;

    std::map<ObjectGuid, uint64>  mDamageByPlayer;
    std::map<ObjectGuid, time_t>  mLastActivityTs;

    void Reset() override
    {
        mEvents.Reset();

        mCountingDown  = false;
        mActive        = false;
        mCountdownLeft = 0;

        mFightDurationMs = 0;
        mFightStarted    = false;
        mFightElapsedMs  = 0;

        mElapsedMs     = 0;
        mIdleMs        = 0;
        mKickSweepMs   = 0;
        mDebuffSweepMs = 0;

        mDamageByPlayer.clear();
        mLastActivityTs.clear();

        if (!me)
            return;

        mHomeOri = me->GetOrientation();

        mNpcFlagsOriginal = me->GetUInt32Value(UNIT_NPC_FLAGS);

        me->SetHealth(me->GetMaxHealth());

        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE_2);
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_ATTACKABLE_1);
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);

        TrainingDummy::RestoreGossip(me, mNpcFlagsOriginal);

        me->SetReactState(REACT_PASSIVE);
        SetCombatMovement(false);

        me->CombatStop(true);
        me->DeleteThreatList();
        me->AttackStop();
        me->SetTargetGuid(ObjectGuid());
        me->ClearInCombat();

        TrainingDummy::SetCombatAura(me, false);
        TrainingDummy::FreezeInPlace(me, mHomeOri);
    }

    void StartCountdown(uint32 seconds, uint32 fightDurationMs)
    {
        Reset();

        if (!me)
            return;

        mFightDurationMs = fightDurationMs;

        mCountingDown  = true;
        mCountdownLeft = (seconds > 0 ? seconds : 1);

        TrainingDummy::RemoveGossip(me);

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
        if (!me)
            return;

        mCountingDown = false;
        mActive       = true;

        // WICHTIG: Timer startet NICHT hier, sondern beim ersten Hit/Aggro
        mFightStarted   = false;
        mFightElapsedMs = 0;

        mElapsedMs = 0;
        mIdleMs    = 0;

        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE_2);
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_ATTACKABLE_1);
        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);

        me->SetReactState(REACT_PASSIVE);
        SetCombatMovement(false);

        TrainingDummy::FreezeInPlace(me, mHomeOri);
        TrainingDummy::SetCombatAura(me, true);

        me->PlayDirectSound(TrainingDummy::kSoundCountdownGo, 0);
        me->MonsterTextEmote("Boss Dummy: GO!", nullptr);
    }

    void Report()
    {
        if (!me)
            return;

        const float seconds = std::max(1.0f, float(mFightStarted ? mFightElapsedMs : mElapsedMs) / 1000.0f);

        uint64 total = 0;
        for (const auto& it : mDamageByPlayer)
            total += it.second;

        const float dps = float(total) / seconds;

        std::vector<TrainingDummy::GuidValue> top;
        TrainingDummy::BuildTopList(mDamageByPlayer, top);

        {
            std::ostringstream ss;
            ss << "Boss Dummy Report: Dauer " << uint32(seconds) << "s, Total "
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
    }

    void ForceEndFightAndReset()
    {
        if (!me)
            return;

        // Report sofort (wie alt)
        if (mActive && mFightStarted)
            Report();

        // alle Teilnehmer sofort aus Combat (wie alt)
        for (auto it = mLastActivityTs.begin(); it != mLastActivityTs.end(); ++it)
        {
            Unit* u = me->GetMap() ? me->GetMap()->GetUnit(it->first) : nullptr;
            if (!u || !u->IsInWorld())
                continue;

            if (u->IsPlayer())
            {
                Player* p = u->ToPlayer();
                p->CombatStopWithPets(true);
                p->CombatStop(true);
            }

            me->_removeAttacker(u);
        }

        // --- Dummy-seitig hart aufraeumen ---
        me->AttackStop();
        me->CombatStop(true);
        me->DeleteThreatList();
        me->SetTargetGuid(ObjectGuid());
        me->ClearInCombat();

        // WICHTIG: alle Debuffs/Buffs komplett entfernen (sonst bleiben Boss-Debuffs haengen)
        me->RemoveAllAuras();

        // Falls dein Core das hat (manche Branches): auch AuraStates resetten
        me->ClearAllReactives(); // optional, nur falls vorhanden

        TrainingDummy::SetCombatAura(me, false);
        TrainingDummy::RestoreGossip(me, mNpcFlagsOriginal);

        // kompletter Reset (setzt HP etc)
        Reset();
    }

    void FinishAndReset()
    {
        if (mActive)
            Report();

        if (me)
            TrainingDummy::RestoreGossip(me, mNpcFlagsOriginal);

        Reset();
    }

    void DamageTaken(Unit* doneBy, uint32& damage) override
    {
        TrainingDummy::FreezeInPlace(me, mHomeOri);

        if (!mActive)
        {
            damage = 0;
            return;
        }

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

        // Start Fight-Timer beim ersten echten Hit/Aggro
        if (!mFightStarted)
        {
            mFightStarted   = true;
            mFightElapsedMs = 0;
            mElapsedMs      = 0;
            mIdleMs         = 0;

            // Debuffs sofort setzen (wie alt)
            TrainingDummy::ApplyBossDebuffs(me);

            // erste Message mit Dauer
            if (mFightDurationMs > 0)
            {
                std::ostringstream ss;
                ss << "Boss Dummy: Fight gestartet (" << (mFightDurationMs / 60000) << " Min).";
                std::string msg = ss.str();
                me->MonsterTextEmote(msg.c_str(), nullptr);
            }
        }

        TrainingDummy::EnsureCombat(me, doneBy);
        mLastActivityTs[owner->GetObjectGuid()] = std::time(nullptr);

        mIdleMs = 0;
        mDamageByPlayer[owner->GetObjectGuid()] += uint64(damage);
    }

    void KickInactivePlayers()
    {
        if (!me)
            return;

        if (mLastActivityTs.empty())
            return;

        const time_t now = std::time(nullptr);

        for (auto it = mLastActivityTs.begin(); it != mLastActivityTs.end(); )
        {
            Unit* u = me->GetMap() ? me->GetMap()->GetUnit(it->first) : nullptr;

            if (!u || !u->IsInWorld())
            {
                it = mLastActivityTs.erase(it);
                continue;
            }

            if ((now - it->second) * 1000 >= TrainingDummy::kKickPlayerAfterIdleMs)
            {
                if (u->IsPlayer())
                {
                    Player* p = u->ToPlayer();
                    p->CombatStopWithPets(true);
                    p->CombatStop(true);
                    me->_removeAttacker(u);
                }

                it = mLastActivityTs.erase(it);
                continue;
            }

            ++it;
        }

        if (mLastActivityTs.empty())
        {
            me->DeleteThreatList();
            me->AttackStop();
            me->SetTargetGuid(ObjectGuid());
            me->ClearInCombat();
            TrainingDummy::SetCombatAura(me, false);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        TrainingDummy::FreezeInPlace(me, mHomeOri);

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

                    me->PlayDirectSound(TrainingDummy::kSoundCountdownTick, 0);

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

        // Idle/Legacy
        mElapsedMs += diff;
        mIdleMs    += diff;

        // Fight-Timer: laeuft nur ab erstem Hit
        if (mFightStarted)
        {
            mFightElapsedMs += diff;

            // Debuffs periodisch nachsetzen (falls Aura-Limits / Dispel / etc.)
            if (mDebuffSweepMs <= diff)
            {
                TrainingDummy::ApplyBossDebuffs(me);
                mDebuffSweepMs = 5000;
            }
            else
                mDebuffSweepMs -= diff;

            // Harte Laufzeit
            if (mFightDurationMs > 0 && mFightElapsedMs >= mFightDurationMs)
            {
                ForceEndFightAndReset();
                return;
            }
        }

        if (mKickSweepMs <= diff)
        {
            KickInactivePlayers();
            mKickSweepMs = TrainingDummy::kKickSweepIntervalMs;
        }
        else
            mKickSweepMs -= diff;

        // Safety: wenn keiner mehr aktiv ist -> normaler Reset (wie vorher)
        if (mIdleMs >= TrainingDummy::kResetAfterIdleMs && mElapsedMs >= TrainingDummy::kMinFightMs)
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

    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Start 1 Minute (10s Pull)",   GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_START_1M);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Start 3 Minuten (10s Pull)",  GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_START_3M);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Start 5 Minuten (10s Pull)",  GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_START_5M);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Start 10 Minuten (10s Pull)", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_START_10M);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Report (jetzt)",              GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_REPORT);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Reset",                       GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_RESET);

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
        case GOSSIP_ACTION_BOSS_START_1M:
            ai->StartCountdown(10, 1 * 60 * 1000);
            break;

        case GOSSIP_ACTION_BOSS_START_3M:
            ai->StartCountdown(10, 3 * 60 * 1000);
            break;

        case GOSSIP_ACTION_BOSS_START_5M:
            ai->StartCountdown(10, 5 * 60 * 1000);
            break;

        case GOSSIP_ACTION_BOSS_START_10M:
            ai->StartCountdown(10, 10 * 60 * 1000);
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

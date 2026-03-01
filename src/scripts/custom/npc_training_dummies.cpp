#include "scriptPCH.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Utilities/EventMap.h"

#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <ctime>
#include <cstdarg>
#include <cstdio>
#include <cctype>
// ------------------------------------------------------------
// Debug-Helper: schreibt eine Chat-Zeile nur an den auswaehlenden Spieler
static void SendBossDummyDebug(Player* pPlayer, Creature* pCreature, const char* fmt, ...)
{
    if (!pPlayer || !pCreature || !fmt)
        return;

    char buffer[512];
    buffer[0] = '\0';

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    buffer[sizeof(buffer) - 1] = '\0';
    pCreature->MonsterWhisper(buffer, pPlayer, true);
}


enum
{
    NPC_HEAL_DUMMY   = 60002,
    NPC_DAMAGE_DUMMY = 60003
};

// ------------------------------------------------------------
// Debug (World Console)
// ------------------------------------------------------------
static void BossDummyLog(const char* /*fmt*/, ...)
{
    // In diesem Branch kein generisches Script-Log verfügbar (und wir wollen keine Compile-Probleme).
    // Debug-Ausgaben erfolgen gezielt per Chat an den Spieler (siehe SendBossDummyDebug).
}

enum
{
    // Raid/Boss Auswahl (Boss Dummy Gossip)
    GOSSIP_ACTION_RAID_BASE      = GOSSIP_ACTION_INFO_DEF + 10,   // + index
    GOSSIP_ACTION_BOSS_BASE      = GOSSIP_ACTION_INFO_DEF + 1000, // + index (global)
    GOSSIP_ACTION_BACK_TO_RAIDS  = GOSSIP_ACTION_INFO_DEF + 9000,
    GOSSIP_ACTION_BACK_TO_BOSSES = GOSSIP_ACTION_INFO_DEF + 9001,

    // Timer Auswahl
    GOSSIP_ACTION_BOSS_START_1M  = GOSSIP_ACTION_INFO_DEF + 1,
    GOSSIP_ACTION_BOSS_START_3M  = GOSSIP_ACTION_INFO_DEF + 2,
    GOSSIP_ACTION_BOSS_START_5M  = GOSSIP_ACTION_INFO_DEF + 3,
    GOSSIP_ACTION_BOSS_START_10M = GOSSIP_ACTION_INFO_DEF + 4,

    GOSSIP_ACTION_BOSS_RESET     = GOSSIP_ACTION_INFO_DEF + 5,
    GOSSIP_ACTION_BOSS_REPORT    = GOSSIP_ACTION_INFO_DEF + 6
};


// ------------------------------------------------------------
// Boss-Dummy: Raid/Boss Auswahl (hardcoded Liste)
// Du kannst hier ganz einfach Boss-Entries ergaenzen/korrekturen machen.
// ------------------------------------------------------------
struct RaidBossDef
{
    const char* raidName;
    std::vector<uint32> bossEntries;
};

static const RaidBossDef kRaidBossMenus[] =
{
    // Naxxramas (Beispiele)
    { "Naxxramas", { 16028 /*Patchwerk*/, 15956 /*Anub'Rekhan*/, 16060 /*Gothik the Harvester*/, 15990 /*Kel'Thuzad (DB abhaengig)*/ } },

    // Molten Core (Beispiele)
    { "Molten Core", { 12118 /*Lucifron*/, 11982 /*Magmadar*/, 12259 /*Gehennas*/, 12056 /*Baron Geddon*/, 11502 /*Ragnaros (DB abhaengig)*/ } },

    // Blackwing Lair (Beispiele)
    { "Blackwing Lair", { 12435 /*Razorgore*/, 13020 /*Vaelastrasz*/, 11981 /*Nefarian (DB abhaengig)*/ } },

    // Ahn'Qiraj 40 (Beispiele)
    { "Ahn'Qiraj (40)", { 15263 /*The Prophet Skeram*/, 15511 /*Lord Kri*/, 15727 /*C'Thun (DB abhaengig)*/ } },

    // Onyxia
    { "Onyxia", { 10184 /*Onyxia*/ } },

    // Zul'Gurub (Beispiele)
    { "Zul'Gurub", { 14517 /*High Priestess Jeklik*/, 14510 /*High Priestess Mar'li*/, 14834 /*Hakkar*/ } }
};

static std::map<ObjectGuid, uint32> gSelectedRaidIdxByPlayer;
static std::map<ObjectGuid, uint32> gSelectedBossEntryByPlayer;

static const CreatureInfo* ResolveCreatureTemplate(uint32 entry)
{
    if (!entry)
        return nullptr;
    return sObjectMgr.GetCreatureTemplate(entry);
}
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

        // kompletter Aura-Reset (auch Debuffs)
        me->RemoveAllAuras();

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

        // kompletter Aura-Reset (auch Debuffs)
        me->RemoveAllAuras();

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

        // kompletter Aura-Reset (auch Debuffs)
        me->RemoveAllAuras();

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

        // kompletter Aura-Reset (auch Debuffs)
        me->RemoveAllAuras();
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
        mFightDurationMs(0), mSelectedBossEntry(0), mAppliedMechanicMask(0), mAppliedSchoolMask(0), mFightStarted(false), mFightElapsedMs(0),
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
    uint32 mSelectedBossEntry; // gewaehlter Boss (creature_template entry)
    uint32 mAppliedMechanicMask;
    uint32 mAppliedSchoolMask;
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
        // mSelectedBossEntry bleibt bestehen (Menue-Auswahl bleibt erhalten)
        mAppliedMechanicMask = 0;
        mAppliedSchoolMask   = 0;
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

        // kompletter Aura-Reset (auch Debuffs)
        me->RemoveAllAuras();

        TrainingDummy::SetCombatAura(me, false);
        TrainingDummy::FreezeInPlace(me, mHomeOri);
    }

void ClearAndApplyImmunityMasks(uint32 mechanicMask, uint32 schoolMask)
{
    Creature* me = m_creature;
    if (!me)
        return;
    // Entferne alte (falls wir welche gesetzt hatten)
    if (mAppliedMechanicMask)
    {
        for (uint32 bit = 0; bit < 32; ++bit)
        {
            const uint32 m = (1u << bit);
            if (mAppliedMechanicMask & m)
                me->ApplySpellImmune(0, IMMUNITY_MECHANIC, bit, false);
        }
    }

    if (mAppliedSchoolMask)
        me->ApplySpellImmune(0, IMMUNITY_SCHOOL, mAppliedSchoolMask, false);

    // Setze neue
    if (mechanicMask)
    {
        for (uint32 bit = 0; bit < 32; ++bit)
        {
            const uint32 m = (1u << bit);
            if (mechanicMask & m)
                me->ApplySpellImmune(0, IMMUNITY_MECHANIC, bit, true);
        }
    }

    if (schoolMask)
        me->ApplySpellImmune(0, IMMUNITY_SCHOOL, schoolMask, true);

    mAppliedMechanicMask = mechanicMask;
    mAppliedSchoolMask   = schoolMask;
}


// ------------------------------------------------------------
// creature_classlevelstats Schema Detection (DB ist je nach Core/DB Dump unterschiedlich)
// Wir duerfen KEINE Query mit nicht existierenden Spalten absetzen, weil vMaNGOS dann assert-crasht.
// ------------------------------------------------------------
enum ClassLevelStatsLayout
{
    CLS_LAYOUT_UNKNOWN = 0,
    CLS_LAYOUT_BASEHP012 = 1,   // basehp0/basehp1/basehp2/basemana/(basearmor)
    CLS_LAYOUT_BASEHP = 2       // basehp/basemana/(basearmor)
};

static ClassLevelStatsLayout& GetClsLayout()
{
    static ClassLevelStatsLayout sLayout = CLS_LAYOUT_UNKNOWN;
    return sLayout;
}

static bool& GetClsHasBaseArmor()
{
    static bool sHasBaseArmor = false;
    return sHasBaseArmor;
}



static std::string& GetClsColHp0() { static std::string s; return s; }
static std::string& GetClsColHp1() { static std::string s; return s; }
static std::string& GetClsColHp2() { static std::string s; return s; }
static std::string& GetClsColHp()  { static std::string s; return s; }
static std::string& GetClsColMana(){ static std::string s; return s; }
static std::string& GetClsColArmor(){ static std::string s; return s; }
static std::string& GetClsColClass(){ static std::string s; return s; }static void DetectClassLevelStatsLayout()
{
    if (GetClsLayout() != CLS_LAYOUT_UNKNOWN)
        return;

    GetClsLayout() = CLS_LAYOUT_BASEHP012; // default Annahme, wird unten ggf. korrigiert
    GetClsHasBaseArmor() = false;

    // SHOW COLUMNS ist in MySQL immer gueltig und crasht nicht
    auto cRes = WorldDatabase.Query("SHOW COLUMNS FROM creature_classlevelstats");
    if (!cRes)
    {
        // Wenn Tabelle fehlt oder Query fehlschlaegt, bleiben wir bei default,
        // aber ApplyBossTemplateToDummy wird dann sauber false liefern.
        GetClsLayout() = CLS_LAYOUT_UNKNOWN;
        return;
    }

    bool hasBasehp0 = false;
    bool hasBasehp1 = false;
    bool hasBasehp2 = false;
    bool hasBasehp  = false;
    bool hasBaseHealth = false;
    std::string baseHealthCol;
    bool hasBasemana = false;
    bool hasBasearmor = false;
    bool hasClassCol = false;
    bool hasUnitClassCol = false;
    // Resolved column names (shared statics)
    std::string& sColHp0 = GetClsColHp0();
    std::string& sColHp1 = GetClsColHp1();
    std::string& sColHp2 = GetClsColHp2();
    std::string& sColHp  = GetClsColHp();
    std::string& sColMana = GetClsColMana();
    std::string& sColArmor = GetClsColArmor();
    std::string& sColClass = GetClsColClass();

    sColHp0.clear();
    sColHp1.clear();
    sColHp2.clear();
    sColHp.clear();
    sColMana.clear();
    sColArmor.clear();
    sColClass.clear();

    do
    {
        Field* f = cRes->Fetch(); // Field: Field, Type, Null, Key, Default, Extra
        if (!f)
            break;

        std::string colRaw = f[0].GetString();
        std::string col = colRaw;
        std::transform(col.begin(), col.end(), col.begin(), ::tolower);

        // HP layouts (support common variants)
        if (col == "basehp0" || col == "hp0" || col == "basehp_0")
        {
            hasBasehp0 = true;
            sColHp0 = colRaw;
        }
        else if (col == "basehp1" || col == "hp1" || col == "basehp_1")
        {
            hasBasehp1 = true;
            sColHp1 = colRaw;
        }
        else if (col == "basehp2" || col == "hp2" || col == "basehp_2")
        {
            hasBasehp2 = true;
            sColHp2 = colRaw;
        }
        else if (col == "basehp" || col == "hp" || col == "health" || col == "base_health" || col == "basehealth")
        {
            hasBasehp = true;
            sColHp = colRaw;
            if (col == "base_health" || col == "basehealth")
            {
                hasBaseHealth = true;
                baseHealthCol = colRaw;
            }
        }

        if (col == "base_mana" || col == "basemana" || col == "mana" || col == "basemana0")
        {
            hasBasemana = true;
            sColMana = colRaw;
        }
        if (col == "basearmor" || col == "armor")
        {
            hasBasearmor = true;
            sColArmor = colRaw;
        }

        if (col == "class")
        {
            hasClassCol = true;
            sColClass = colRaw;
        }
        else if (col == "unit_class" || col == "unitclass")
        {
            hasUnitClassCol = true;
            if (sColClass.empty())
                sColClass = colRaw;
        }

    } while (cRes->NextRow());


    // Prefer base_health over health if available
    if (hasBaseHealth && !baseHealthCol.empty())
        sColHp = baseHealthCol;

    if (hasBasehp0 && hasBasehp1 && hasBasehp2)
        GetClsLayout() = CLS_LAYOUT_BASEHP012;
    else if (hasBasehp)
        GetClsLayout() = CLS_LAYOUT_BASEHP;
    else
        GetClsLayout() = CLS_LAYOUT_UNKNOWN;

    GetClsHasBaseArmor() = hasBasearmor;

    // reasonable fallbacks
    if (GetClsLayout() == CLS_LAYOUT_BASEHP012)
    {
        if (sColHp0.empty()) sColHp0 = "basehp0";
        if (sColHp1.empty()) sColHp1 = "basehp1";
        if (sColHp2.empty()) sColHp2 = "basehp2";
    }
    if (GetClsLayout() == CLS_LAYOUT_BASEHP)
    {
        if (sColHp.empty()) sColHp = "basehp";
    }
    if (!hasBasemana)
        sColMana = "basemana";
    if (!hasClassCol && !hasUnitClassCol)
        sColClass = "class";

    BossDummyLog("Detected creature_classlevelstats layout=%u hp0='%s' hp1='%s' hp2='%s' hp='%s' mana='%s' armor='%s' classcol='%s'",
        uint32(GetClsLayout()),
        sColHp0.c_str(), sColHp1.c_str(), sColHp2.c_str(), sColHp.c_str(), sColMana.c_str(),
        (GetClsHasBaseArmor() ? sColArmor.c_str() : ""), sColClass.c_str());
}

static bool QueryClassLevelBaseStats(uint32 level, uint32 unitClass, uint32& outHp0, uint32& outHp1, uint32& outHp2, uint32& outMana, uint32& outArmor)
{
    DetectClassLevelStatsLayout();
    outHp0 = outHp1 = outHp2 = 0;
    outMana = 0;
    outArmor = 0;

    if (GetClsLayout() == CLS_LAYOUT_UNKNOWN)
        return false;
    // Resolved column names (shared statics)
    std::string& sColHp0 = GetClsColHp0();
    std::string& sColHp1 = GetClsColHp1();
    std::string& sColHp2 = GetClsColHp2();
    std::string& sColHp  = GetClsColHp();
    std::string& sColMana = GetClsColMana();
    std::string& sColArmor = GetClsColArmor();
    std::string& sColClass = GetClsColClass();
    if (sColMana.empty())  sColMana  = "basemana";
    if (sColClass.empty()) sColClass = "class";

    std::ostringstream q;
    if (GetClsLayout() == CLS_LAYOUT_BASEHP012)
    {
        if (sColHp0.empty()) sColHp0 = "basehp0";
        if (sColHp1.empty()) sColHp1 = "basehp1";
        if (sColHp2.empty()) sColHp2 = "basehp2";

        q << "SELECT " << sColHp0 << "," << sColHp1 << "," << sColHp2 << "," << sColMana;
        if (GetClsHasBaseArmor())
        {
            if (sColArmor.empty()) sColArmor = "basearmor";
            q << "," << sColArmor;
        }
        q << " FROM creature_classlevelstats WHERE level=" << level << " AND " << sColClass << "=" << unitClass;

        auto sRes = WorldDatabase.Query(q.str().c_str());
        if (!sRes)
            return false;

        Field* sf = sRes->Fetch();
        if (!sf)
            return false;

        outHp0   = sf[0].GetUInt32();
        outHp1   = sf[1].GetUInt32();
        outHp2   = sf[2].GetUInt32();
        outMana  = sf[3].GetUInt32();
        outArmor = (GetClsHasBaseArmor() ? sf[4].GetUInt32() : 0);

        BossDummyLog("ClassLevelStats(level=%u class=%u) hp0=%u hp1=%u hp2=%u mana=%u armor=%u",
            level, unitClass, outHp0, outHp1, outHp2, outMana, outArmor);
        return true;
    }

    // CLS_LAYOUT_BASEHP
    if (sColHp.empty()) sColHp = "basehp";
    q << "SELECT " << sColHp << "," << sColMana;
    if (GetClsHasBaseArmor())
    {
        if (sColArmor.empty()) sColArmor = "basearmor";
        q << "," << sColArmor;
    }
    q << " FROM creature_classlevelstats WHERE level=" << level << " AND " << sColClass << "=" << unitClass;

    auto sRes = WorldDatabase.Query(q.str().c_str());
    if (!sRes)
        return false;

    Field* sf = sRes->Fetch();
    if (!sf)
        return false;

    const uint32 basehp = sf[0].GetUInt32();
    outHp0 = outHp1 = outHp2 = basehp;
    outMana  = sf[1].GetUInt32();
    outArmor = (GetClsHasBaseArmor() ? sf[2].GetUInt32() : 0);

    BossDummyLog("ClassLevelStats(level=%u class=%u) hp=%u mana=%u armor=%u",
        level, unitClass, basehp, outMana, outArmor);
    return true;
}

static bool GetClassLevelBaseStats(uint32 level, uint32 unitClass, uint32& outHp0, uint32& outHp1, uint32& outHp2, uint32& outMana, uint32& outArmor)
{
    return QueryClassLevelBaseStats(level, unitClass, outHp0, outHp1, outHp2, outMana, outArmor);
}


static bool ApplyBossTemplateToDummy(Creature* pDummy, uint32 bossEntry, Player* pPlayerForChat)
{
    if (!pDummy || bossEntry == 0)
        return false;

    const CreatureInfo* ci = sObjectMgr.GetCreatureTemplate(bossEntry);
    if (!ci)
    {
        if (pPlayerForChat)
            SendBossDummyDebug(pPlayerForChat, pDummy, "BossDummy: Template %u nicht gefunden.", bossEntry);
        return false;
    }

    // Level: wir nehmen level_max falls gesetzt, sonst level_min
    const uint32 level = (ci->level_max > 0 ? ci->level_max : ci->level_min);
    if (level > 0)
        pDummy->SetLevel(level);

    const uint32 unitClass = (ci->unit_class > 0 ? ci->unit_class : 1);

    // UnitClass setzen (Bytes_0[1] = Klasse) - nur damit Werte / Power-Type konsistenter sind
    pDummy->SetByteValue(UNIT_FIELD_BYTES_0, 1, uint8(unitClass));

    // ------------------------------------------------------------
    // Basewerte aus creature_classlevelstats (DB) holen (layout-sicher)
    // ------------------------------------------------------------
    uint32 hp0 = 0, hp1 = 0, hp2 = 0;
    uint32 manaBase = 0;
    uint32 armorBase = 0;

    if (!GetClassLevelBaseStats(level, unitClass, hp0, hp1, hp2, manaBase, armorBase))
    {
        // Fallback: aktuelle Dummy-Werte (besser als Crash / 0)
        hp0 = hp1 = hp2 = std::max<uint32>(1u, pDummy->GetMaxHealth());
        manaBase = pDummy->GetMaxPower(POWER_MANA);
        armorBase = pDummy->GetResistance(SPELL_SCHOOL_NORMAL);

        if (pPlayerForChat)
            SendBossDummyDebug(pPlayerForChat, pDummy, "BossDummy: WARN - creature_classlevelstats nicht verfuegbar -> Fallback Dummy-Basiswerte.");
    }

    // HP Auswahl je nach Rank (0=normal, 1=elite, 2=rare elite, 3=worldboss)
    uint32 hpBase = hp0;
    if (ci->rank == 3)
        hpBase = hp2;
    else if (ci->rank == 1 || ci->rank == 2)
        hpBase = hp1;

    if (hpBase < 1)
        hpBase = std::max<uint32>(1u, pDummy->GetMaxHealth());

    // ------------------------------------------------------------
    // Multipliers aus Template anwenden
    // ------------------------------------------------------------
    uint32 hpFinal = uint32(std::max(1.0f, float(hpBase) * std::max(0.01f, ci->health_multiplier)));
    uint32 manaFinal = uint32(std::max(0.0f, float(manaBase) * std::max(0.0f, ci->mana_multiplier)));
    int32  armorFinal = int32(std::max(0.0f, float(armorBase) * std::max(0.0f, ci->armor_multiplier)));

    pDummy->SetMaxHealth(hpFinal);
    pDummy->SetHealth(hpFinal);

    pDummy->SetMaxPower(POWER_MANA, manaFinal);
    pDummy->SetPower(POWER_MANA, manaFinal);

    // Armor / Resist: Unit::SetResistance ist protected -> direkt ins Feld schreiben
    pDummy->SetUInt32Value(UNIT_FIELD_RESISTANCES + SPELL_SCHOOL_NORMAL, uint32(std::max(0, armorFinal)));

    // Elementarresistenzen aus Template
    pDummy->SetUInt32Value(UNIT_FIELD_RESISTANCES + SPELL_SCHOOL_HOLY,   ci->holy_res);
    pDummy->SetUInt32Value(UNIT_FIELD_RESISTANCES + SPELL_SCHOOL_FIRE,   ci->fire_res);
    pDummy->SetUInt32Value(UNIT_FIELD_RESISTANCES + SPELL_SCHOOL_NATURE, ci->nature_res);
    pDummy->SetUInt32Value(UNIT_FIELD_RESISTANCES + SPELL_SCHOOL_FROST,  ci->frost_res);
    pDummy->SetUInt32Value(UNIT_FIELD_RESISTANCES + SPELL_SCHOOL_SHADOW, ci->shadow_res);
    pDummy->SetUInt32Value(UNIT_FIELD_RESISTANCES + SPELL_SCHOOL_ARCANE, ci->arcane_res);

    // Immunitaeten aus Template (branch-sicher via ApplySpellImmune)
    if (ci->mechanic_immune_mask)
    {
        for (uint32 m = 0; m < 32; ++m)
            if (ci->mechanic_immune_mask & (1u << m))
                pDummy->ApplySpellImmune(0, IMMUNITY_MECHANIC, m, true);
    }
    if (ci->school_immune_mask)
    {
        for (uint32 sc = 0; sc < 7; ++sc)
            if (ci->school_immune_mask & (1u << sc))
                pDummy->ApplySpellImmune(0, IMMUNITY_SCHOOL, sc, true);
    }

    // Debug-Ausgabe
    if (pPlayerForChat)
    {
                const uint32 rbHp = pDummy->GetHealth();
        const uint32 rbHpMax = pDummy->GetMaxHealth();
        const uint32 rbMana = pDummy->GetPower(POWER_MANA);
        const uint32 rbManaMax = pDummy->GetMaxPower(POWER_MANA);
        const uint32 rbArmor = pDummy->GetResistance(SPELL_SCHOOL_NORMAL);

        SendBossDummyDebug(pPlayerForChat, pDummy,
            "BossDummy: Boss=%u (%s) -> Level=%u, HP=%u, Mana=%u, Armor=%u, Res(H/F/N/Fr/S/A)=%u/%u/%u/%u/%u/%u | Readback HP=%u/%u Mana=%u/%u Armor=%u",
            bossEntry, ci->name.c_str(), level, hpFinal, manaFinal,
            rbArmor,
            pDummy->GetResistance(SPELL_SCHOOL_HOLY),
            pDummy->GetResistance(SPELL_SCHOOL_FIRE),
            pDummy->GetResistance(SPELL_SCHOOL_NATURE),
            pDummy->GetResistance(SPELL_SCHOOL_FROST),
            pDummy->GetResistance(SPELL_SCHOOL_SHADOW),
            pDummy->GetResistance(SPELL_SCHOOL_ARCANE),
            rbHp, rbHpMax, rbMana, rbManaMax, rbArmor);
}

    return true;
}

void StartCountdown(uint32 seconds, uint32 fightDurationMs, uint32 bossEntry)
{
    Reset();

    if (!me)
        return;

    mFightDurationMs = fightDurationMs;

    // Boss-Stats sofort setzen (damit HP/Resis ab Fight korrekt sind)
    if (bossEntry)
        ApplyBossTemplateToDummy(me, bossEntry, nullptr);

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

        // kompletter Aura-Reset (auch Debuffs)
        me->RemoveAllAuras();
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

static void BuildRaidMenu(Player* pPlayer, Creature* pCreature)
{
    if (!pPlayer || !pCreature)
        return;

    pPlayer->PlayerTalkClass->ClearMenus();

    for (uint32 i = 0; i < (sizeof(kRaidBossMenus) / sizeof(kRaidBossMenus[0])); ++i)
        pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, kRaidBossMenus[i].raidName, GOSSIP_SENDER_MAIN, GOSSIP_ACTION_RAID_BASE + i);

    // optional quick tools
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Report (jetzt)", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_REPORT);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Reset",          GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_RESET);

    pPlayer->SEND_GOSSIP_MENU(DEFAULT_GOSSIP_MESSAGE, pCreature->GetObjectGuid());
}

static void BuildBossMenu(Player* pPlayer, Creature* pCreature, uint32 raidIdx)
{
    if (!pPlayer || !pCreature)
        return;

    pPlayer->PlayerTalkClass->ClearMenus();

    if (raidIdx >= (sizeof(kRaidBossMenus) / sizeof(kRaidBossMenus[0])))
    {
        BuildRaidMenu(pPlayer, pCreature);
        return;
    }

    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "< Zurueck (Raids)", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BACK_TO_RAIDS);

    const RaidBossDef& raid = kRaidBossMenus[raidIdx];
    for (uint32 i = 0; i < raid.bossEntries.size(); ++i)
    {
        const uint32 entry = raid.bossEntries[i];
        const CreatureInfo* ci = ResolveCreatureTemplate(entry);

        std::ostringstream ss;
        ss << (ci ? ci->name : std::string("Boss Entry ")) << entry;

        // globale Boss-Liste: wir encoden raidIdx und local idx in action index
        // action = GOSSIP_ACTION_BOSS_BASE + (raidIdx * 100) + i
        { std::string label = ss.str(); pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, label.c_str(), GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_BASE + (raidIdx * 100) + i); }
    }

    pPlayer->SEND_GOSSIP_MENU(DEFAULT_GOSSIP_MESSAGE, pCreature->GetObjectGuid());
}

static void BuildTimerMenu(Player* pPlayer, Creature* pCreature)
{
    if (!pPlayer || !pCreature)
        return;

    pPlayer->PlayerTalkClass->ClearMenus();

    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "< Zurueck (Bosses)", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BACK_TO_BOSSES);

    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Start 1 Minute (10s Pull)",   GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_START_1M);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Start 3 Minuten (10s Pull)",  GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_START_3M);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Start 5 Minuten (10s Pull)",  GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_START_5M);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Start 10 Minuten (10s Pull)", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_START_10M);

    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Report (jetzt)",              GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_REPORT);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Reset",                       GOSSIP_SENDER_MAIN, GOSSIP_ACTION_BOSS_RESET);

    pPlayer->SEND_GOSSIP_MENU(DEFAULT_GOSSIP_MESSAGE, pCreature->GetObjectGuid());
}

static bool GossipHello_npc_boss_dummy(Player* pPlayer, Creature* pCreature)
{
    if (!pPlayer || !pCreature)
        return true;

    BuildRaidMenu(pPlayer, pCreature);
    return true;
}

static bool GossipSelect_npc_boss_dummy(Player* pPlayer, Creature* pCreature, uint32 /*sender*/, uint32 action)
{
    if (!pPlayer || !pCreature)
        return true;

    npc_boss_dummyAI* ai = dynamic_cast<npc_boss_dummyAI*>(pCreature->AI());
    if (!ai)
        return true;

    const ObjectGuid pguid = pPlayer->GetObjectGuid();

    // Navigation / Auswahl
    if (action >= GOSSIP_ACTION_RAID_BASE && action < GOSSIP_ACTION_RAID_BASE + (sizeof(kRaidBossMenus) / sizeof(kRaidBossMenus[0])))
    {
        const uint32 raidIdx = action - GOSSIP_ACTION_RAID_BASE;
        gSelectedRaidIdxByPlayer[pguid] = raidIdx;
        BuildBossMenu(pPlayer, pCreature, raidIdx);
        return true;
    }

    if (action >= GOSSIP_ACTION_BOSS_BASE && action < GOSSIP_ACTION_BOSS_BASE + 10000)
    {
        const uint32 packed = action - GOSSIP_ACTION_BOSS_BASE;
        const uint32 raidIdx = packed / 100;
        const uint32 localIdx = packed % 100;

        if (raidIdx < (sizeof(kRaidBossMenus) / sizeof(kRaidBossMenus[0])))
        {
            const RaidBossDef& raid = kRaidBossMenus[raidIdx];
            if (localIdx < raid.bossEntries.size())
            {
                const uint32 bossEntry = raid.bossEntries[localIdx];
                gSelectedBossEntryByPlayer[pguid] = bossEntry;
                gSelectedRaidIdxByPlayer[pguid] = raidIdx;

                // Anzeige direkt: Stats jetzt schon setzen (damit HP sofort sichtbar ist)
                npc_boss_dummyAI::ApplyBossTemplateToDummy(pCreature, bossEntry, pPlayer);

                BuildTimerMenu(pPlayer, pCreature);
                return true;
            }
        }

        BuildRaidMenu(pPlayer, pCreature);
        return true;
    }

    if (action == GOSSIP_ACTION_BACK_TO_RAIDS)
    {
        BuildRaidMenu(pPlayer, pCreature);
        return true;
    }

    if (action == GOSSIP_ACTION_BACK_TO_BOSSES)
    {
        uint32 raidIdx = 0;
        auto it = gSelectedRaidIdxByPlayer.find(pguid);
        if (it != gSelectedRaidIdxByPlayer.end())
            raidIdx = it->second;

        BuildBossMenu(pPlayer, pCreature, raidIdx);
        return true;
    }

    // Timer starten -> Gossip schliessen
    pPlayer->CLOSE_GOSSIP_MENU();

    const uint32 bossEntry = (gSelectedBossEntryByPlayer.count(pguid) ? gSelectedBossEntryByPlayer[pguid] : 0);
    if (!bossEntry && (action == GOSSIP_ACTION_BOSS_START_1M || action == GOSSIP_ACTION_BOSS_START_3M || action == GOSSIP_ACTION_BOSS_START_5M || action == GOSSIP_ACTION_BOSS_START_10M))
    {
        pCreature->MonsterTextEmote("Boss Dummy: Bitte zuerst Raid -> Boss auswaehlen.", nullptr);
        return true;
    }

    switch (action)
    {
        case GOSSIP_ACTION_BOSS_START_1M:
            ai->StartCountdown(10, 1 * 60 * 1000, bossEntry);
            break;

        case GOSSIP_ACTION_BOSS_START_3M:
            ai->StartCountdown(10, 3 * 60 * 1000, bossEntry);
            break;

        case GOSSIP_ACTION_BOSS_START_5M:
            ai->StartCountdown(10, 5 * 60 * 1000, bossEntry);
            break;

        case GOSSIP_ACTION_BOSS_START_10M:
            ai->StartCountdown(10, 10 * 60 * 1000, bossEntry);
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
} // namespace TrainingDummy

// ============================================================
// Registration (ScriptDev2 / vMaNGOS Style)
// ============================================================
void AddSC_npc_training_dummies()
{
    Script* newscript;

    newscript = new Script;
    newscript->Name = "npc_damage_dummy";
    newscript->GetAI = &TrainingDummy::GetAI_npc_damage_dummy;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "npc_heal_dummy";
    newscript->GetAI = &TrainingDummy::GetAI_npc_heal_dummy;
    newscript->RegisterSelf();

    newscript = new Script;
    newscript->Name = "npc_boss_dummy";
    newscript->GetAI = &TrainingDummy::GetAI_npc_boss_dummy;
    newscript->pGossipHello = &TrainingDummy::GossipHello_npc_boss_dummy;
    newscript->pGossipSelect = &TrainingDummy::GossipSelect_npc_boss_dummy;
    newscript->RegisterSelf();
}

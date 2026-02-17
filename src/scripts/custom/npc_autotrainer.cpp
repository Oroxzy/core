#include "scriptPCH.h"
#include "ObjectMgr.h"
#include <vector>
#include <string>

enum
{
    GOSSIP_ACTION_LEARN_CURRENT     = GOSSIP_ACTION_INFO_DEF + 1,
    GOSSIP_ACTION_LEVEL_PLUS_10     = GOSSIP_ACTION_INFO_DEF + 2,
    GOSSIP_ACTION_LEVEL_NEXT_TEN    = GOSSIP_ACTION_INFO_DEF + 3,
    GOSSIP_ACTION_CANCEL            = GOSSIP_ACTION_INFO_DEF + 4
};

struct AutoTrainerSource
{
    uint32 trainerId;
    uint32 trainerEntry;

    AutoTrainerSource() : trainerId(0), trainerEntry(0) {}
};

static bool gClassSourcesResolved[12] = { false };
static std::vector<AutoTrainerSource> gClassSources[12];

static bool gWeaponSourcesResolved = false;
static std::vector<AutoTrainerSource> gWeaponSources;

static uint32 GetMaxPlayerLevel_Cached()
{
    return 60;
}

static uint16 GetMaxSkillForPlayerLevel(Player* pPlayer)
{
    if (!pPlayer)
        return 1;

    uint32 lvl = pPlayer->GetLevel();
    if (lvl < 1)
        lvl = 1;

    uint32 v = lvl * 5;
    if (v > 300)
        v = 300;

    return uint16(v);
}

static bool StrContainsI(std::string const& haystack, char const* needle)
{
    if (!needle || !*needle)
        return true;

    std::string n(needle);

    auto toLowerAscii = [](unsigned char c) -> unsigned char
    {
        if (c >= 'A' && c <= 'Z')
            return (unsigned char)(c - 'A' + 'a');
        return c;
    };

    std::string h = haystack;
    for (size_t i = 0; i < h.size(); ++i) h[i] = (char)toLowerAscii((unsigned char)h[i]);
    for (size_t i = 0; i < n.size(); ++i) n[i] = (char)toLowerAscii((unsigned char)n[i]);

    return (h.find(n) != std::string::npos);
}

static bool StrContainsI(char const* haystack, char const* needle)
{
    if (!needle || !*needle)
        return true;

    if (!haystack || !*haystack)
        return false;

    return StrContainsI(std::string(haystack), needle);
}

static void SetSkillToMaxIfKnown(Player* pPlayer, uint32 skillId, uint16 maxValue)
{
    if (!pPlayer || !skillId)
        return;

    if (pPlayer->GetSkillValue(skillId) <= 0)
        return;

    pPlayer->SetSkill(skillId, maxValue, maxValue);
}

static void MaxOutWeaponDefenseAndRiding(Player* pPlayer)
{
    if (!pPlayer)
        return;

    uint16 maxV = GetMaxSkillForPlayerLevel(pPlayer);

    SetSkillToMaxIfKnown(pPlayer, 43,  maxV);
    SetSkillToMaxIfKnown(pPlayer, 44,  maxV);
    SetSkillToMaxIfKnown(pPlayer, 45,  maxV);
    SetSkillToMaxIfKnown(pPlayer, 46,  maxV);
    SetSkillToMaxIfKnown(pPlayer, 54,  maxV);
    SetSkillToMaxIfKnown(pPlayer, 55,  maxV);
    SetSkillToMaxIfKnown(pPlayer, 136, maxV);
    SetSkillToMaxIfKnown(pPlayer, 160, maxV);
    SetSkillToMaxIfKnown(pPlayer, 162, maxV);
    SetSkillToMaxIfKnown(pPlayer, 173, maxV);
    SetSkillToMaxIfKnown(pPlayer, 176, maxV);
    SetSkillToMaxIfKnown(pPlayer, 226, maxV);
    SetSkillToMaxIfKnown(pPlayer, 228, maxV);
    SetSkillToMaxIfKnown(pPlayer, 229, maxV);
    SetSkillToMaxIfKnown(pPlayer, 172, maxV);
    SetSkillToMaxIfKnown(pPlayer, 473, maxV);

    SetSkillToMaxIfKnown(pPlayer, 95,  maxV);
    SetSkillToMaxIfKnown(pPlayer, 762, maxV);
}

static void ResolveTrainerSourcesForClass(uint8 playerClass)
{
    // Zweck: "normale" Klassenlehrer sammeln (einmalig pro Klasse) - OHNE DB
    // WICHTIG:
    // - Pet Trainer NICHT hier reinnehmen: deren "grün" ist pet-/familie-/level-abhängig und macht Autotraining inkonsistent.
    // - Demon Trainer ebenfalls NICHT: bei dir laufen Demon-Abilities über Grimoires (Items/Teach-Spells), nicht über TrainerSpellData.

    if (playerClass >= 12)
        return;

    if (gClassSourcesResolved[playerClass])
        return;

    gClassSourcesResolved[playerClass] = true;
    gClassSources[playerClass].clear();

    CreatureInfoMap const& m = sObjectMgr.GetCreatureInfoMap();

    std::vector<uint32> seenTrainerIds;
    std::vector<uint32> seenEntries;

    for (CreatureInfoMap::const_iterator it = m.begin(); it != m.end(); ++it)
    {
        CreatureInfo const* cInfo = it->second.get();
        if (!cInfo)
            continue;

        if ((cInfo->npc_flags & UNIT_NPC_FLAG_TRAINER) == 0)
            continue;

        // Pet/Demon Trainer explizit skippen
        if (StrContainsI(cInfo->subname, "Pet Trainer"))
            continue;

        if (StrContainsI(cInfo->subname, "Demon Trainer"))
            continue;

        // nur "normale" Trainer
        if (cInfo->trainer_type != 0)
            continue;

        if (cInfo->trainer_class != uint32(playerClass))
            continue;

        bool ok = false;

        switch (playerClass)
        {
            case CLASS_MAGE:
                ok = StrContainsI(cInfo->subname, "Mage Trainer") || StrContainsI(cInfo->subname, "Portal Trainer");
                break;

            case CLASS_HUNTER:
                ok = StrContainsI(cInfo->subname, "Hunter Trainer");
                break;

            case CLASS_WARLOCK:
                ok = StrContainsI(cInfo->subname, "Warlock Trainer");
                break;

            case CLASS_WARRIOR: ok = StrContainsI(cInfo->subname, "Warrior Trainer"); break;
            case CLASS_PALADIN: ok = StrContainsI(cInfo->subname, "Paladin Trainer"); break;
            case CLASS_ROGUE:   ok = StrContainsI(cInfo->subname, "Rogue Trainer");   break;
            case CLASS_PRIEST:  ok = StrContainsI(cInfo->subname, "Priest Trainer");  break;
            case CLASS_SHAMAN:  ok = StrContainsI(cInfo->subname, "Shaman Trainer");  break;
            case CLASS_DRUID:   ok = StrContainsI(cInfo->subname, "Druid Trainer");   break;

            default:
                ok = StrContainsI(cInfo->subname, "Trainer");
                break;
        }

        if (!ok)
            continue;

        uint32 entry = cInfo->entry;
        uint32 tid   = cInfo->trainer_id;

        bool duplicate = false;

        if (tid)
        {
            for (size_t i = 0; i < seenTrainerIds.size(); ++i)
            {
                if (seenTrainerIds[i] == tid)
                {
                    duplicate = true;
                    break;
                }
            }
        }
        else
        {
            for (size_t i = 0; i < seenEntries.size(); ++i)
            {
                if (seenEntries[i] == entry)
                {
                    duplicate = true;
                    break;
                }
            }
        }

        if (duplicate)
            continue;

        AutoTrainerSource src;
        src.trainerEntry = entry;
        src.trainerId    = tid;
        gClassSources[playerClass].push_back(src);

        if (tid)
            seenTrainerIds.push_back(tid);
        else
            seenEntries.push_back(entry);
    }
}

static void ResolveWeaponTrainerSources()
{
    if (gWeaponSourcesResolved)
        return;

    gWeaponSourcesResolved = true;
    gWeaponSources.clear();

    CreatureInfoMap const& m = sObjectMgr.GetCreatureInfoMap();

    std::vector<uint32> seenTrainerIds;
    std::vector<uint32> seenEntries;

    for (CreatureInfoMap::const_iterator it = m.begin(); it != m.end(); ++it)
    {
        CreatureInfo const* cInfo = it->second.get();
        if (!cInfo)
            continue;

        if ((cInfo->npc_flags & UNIT_NPC_FLAG_TRAINER) == 0)
            continue;

        if (!StrContainsI(cInfo->subname, "Weapon Master") && !StrContainsI(cInfo->name, "Weapon Master"))
            continue;

        uint32 entry = cInfo->entry;
        uint32 tid   = cInfo->trainer_id;

        bool duplicate = false;

        if (tid)
        {
            for (size_t i = 0; i < seenTrainerIds.size(); ++i)
            {
                if (seenTrainerIds[i] == tid)
                {
                    duplicate = true;
                    break;
                }
            }
        }
        else
        {
            for (size_t i = 0; i < seenEntries.size(); ++i)
            {
                if (seenEntries[i] == entry)
                {
                    duplicate = true;
                    break;
                }
            }
        }

        if (duplicate)
            continue;

        AutoTrainerSource src;
        src.trainerEntry = entry;
        src.trainerId    = tid;
        gWeaponSources.push_back(src);

        if (tid)
            seenTrainerIds.push_back(tid);
        else
            seenEntries.push_back(entry);
    }
}

static TrainerSpellData const* GetTrainerSpells_EntryFirst_FallbackTemplate(AutoTrainerSource const& src)
{
    if (src.trainerEntry)
    {
        TrainerSpellData const* byEntry = sObjectMgr.GetNpcTrainerSpells(src.trainerEntry);
        if (byEntry)
            return byEntry;
    }

    if (src.trainerId)
        return sObjectMgr.GetNpcTrainerTemplateSpells(src.trainerId);

    return nullptr;
}

static bool gSpellChainNextBuilt = false;
static std::vector<uint32> gNextInChain;

static void BuildSpellChainNextCache()
{
    if (gSpellChainNextBuilt)
        return;

    gSpellChainNextBuilt = true;

    uint32 maxId = sSpellMgr.GetMaxSpellId();
    gNextInChain.clear();
    gNextInChain.resize(maxId + 1, 0);

    for (uint32 id = 1; id <= maxId; ++id)
    {
        uint32 prev = sSpellMgr.GetPrevSpellInChain(id);
        if (prev > 0 && prev <= maxId)
            gNextInChain[prev] = id;
    }
}

static uint32 GetNextSpellInChain_Cached(uint32 spellId)
{
    if (!spellId)
        return 0;

    BuildSpellChainNextCache();

    if (spellId >= gNextInChain.size())
        return 0;

    return gNextInChain[spellId];
}

static bool IsPureLearnContainerSpell(uint32 spellId)
{
    SpellEntry const* proto = sSpellMgr.GetSpellEntry(spellId);
    if (!proto)
        return false;

    bool hasLearnEffect = false;
    bool hasOtherEffect = false;

    for (uint8 i = 0; i < 3; ++i)
    {
        if (proto->Effect[i] == SPELL_EFFECT_LEARN_SPELL)
            hasLearnEffect = true;
        else if (proto->Effect[i] != 0)
            hasOtherEffect = true;
    }

    return (hasLearnEffect && !hasOtherEffect);
}

static uint32 LearnHigherRanksFromSpellChains(Player* pPlayer)
{
    if (!pPlayer)
        return 0;

    uint32 learned = 0;
    uint32 maxId = sSpellMgr.GetMaxSpellId();
    if (maxId < 2)
        return 0;

    for (uint32 knownId = 1; knownId <= maxId; ++knownId)
    {
        if (!pPlayer->HasSpell(knownId))
            continue;

        uint32 nextId = GetNextSpellInChain_Cached(knownId);
        while (nextId && !pPlayer->HasSpell(nextId))
        {
            if (!pPlayer->IsSpellFitByClassAndRace(nextId))
                break;

            SpellEntry const* nextProto = sSpellMgr.GetSpellEntry(nextId);
            if (!nextProto)
                break;

            if (nextProto->spellLevel > 0 && pPlayer->GetLevel() < uint32(nextProto->spellLevel))
                break;

            if ((nextProto->Attributes & SPELL_ATTR_PASSIVE) != 0)
                break;

            if (IsPureLearnContainerSpell(nextId))
                break;

            pPlayer->LearnSpell(nextId, false);
            ++learned;

            knownId = nextId;
            nextId = GetNextSpellInChain_Cached(nextId);
        }
    }

    return learned;
}

static uint32 GetMinLevelForSpecialSpell(uint32 spellId)
{
    switch (spellId)
    {
        case 688:   return 1;
        case 697:   return 10;
        case 712:   return 20;
        case 691:   return 30;
        case 1122:  return 50;
        case 5784:  return 40;
        case 23161: return 60;
        case 18540: return 60;

        case 7328:  return 12;
        case 13819: return 40;
        case 23214: return 60;
        case 19752: return 20;

        case 71:    return 10;
        case 2458:  return 30;

        case 5487:  return 10;
        case 1066:  return 16;
        case 5215:  return 20;
        case 768:   return 20;
        case 783:   return 30;
        case 40120: return 40;
        case 33943: return 60;

        case 1515:  return 10;
        case 5149:  return 10;
        case 2641:  return 10;
        case 883:   return 10;
        case 982:   return 10;
        case 6991:  return 10;
        case 19883: return 30;

        case 8071:  return 4;
        case 3599:  return 4;
        case 5394:  return 20;
        case 2484:  return 12;
        case 3738:  return 6;

        case 6346:  return 20;
        case 9035:  return 1;
        case 18137: return 20;

        case 3561:  return 20;
        case 3562:  return 20;
        case 3563:  return 20;
        case 3565:  return 20;
        case 3566:  return 20;
        case 3567:  return 20;
        case 10059: return 40;
        case 11416: return 40;
        case 11417: return 40;
        case 11418: return 40;
        case 11419: return 40;
        case 11420: return 40;

        case 1784:  return 1;
        case 1804:  return 1;
        case 51724: return 1;

        default:
            return 1;
    }
}

static bool LearnDirectSpellIfMissing(Player* pPlayer, uint32 spellId)
{
    if (!pPlayer || !spellId)
        return false;

    if (pPlayer->HasSpell(spellId))
        return false;

    if (IsPureLearnContainerSpell(spellId))
        return false;

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(spellId);
    if (!proto)
        return false;

    uint32 minLvl = GetMinLevelForSpecialSpell(spellId);
    if (pPlayer->GetLevel() < minLvl)
        return false;

    if (proto->spellLevel > 0 && pPlayer->GetLevel() < uint32(proto->spellLevel))
        return false;

    pPlayer->LearnSpell(spellId, false);
    return true;
}

static bool LearnQuestSpellIfAllowed(Player* pPlayer, uint32 spellId)
{
    if (!pPlayer || !spellId)
        return false;

    if (!pPlayer->IsSpellFitByClassAndRace(spellId))
        return false;

    return LearnDirectSpellIfMissing(pPlayer, spellId);
}

static uint32 LearnQuestSpecialSpellsForClass(Player* pPlayer)
{
    if (!pPlayer)
        return 0;

    uint8 cls = pPlayer->GetByteValue(UNIT_FIELD_BYTES_0, 1);
    uint32 learned = 0;

    switch (cls)
    {
        case CLASS_HUNTER:
            if (LearnQuestSpellIfAllowed(pPlayer, 2641))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 883))   ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 1515))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 5149))  ++learned; // Beast Training (Spieler)
            if (LearnQuestSpellIfAllowed(pPlayer, 6991))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 982))   ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 19801)) ++learned;
            break;

        case CLASS_ROGUE:
            if (LearnQuestSpellIfAllowed(pPlayer, 2842))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 25347)) ++learned;
            break;

        case CLASS_WARRIOR:
            if (LearnQuestSpellIfAllowed(pPlayer, 2457))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 71))    ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 7386))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 355))   ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 2458))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 20252)) ++learned;
            break;

        case CLASS_WARLOCK:
            if (LearnQuestSpellIfAllowed(pPlayer, 688))   ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 697))   ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 712))   ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 691))   ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 1122))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 5784))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 23161)) ++learned;
            break;

        case CLASS_MAGE:
            if (LearnQuestSpellIfAllowed(pPlayer, 28272)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 28271)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 28270)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 23028)) ++learned;
            break;

        case CLASS_SHAMAN:
            if (LearnQuestSpellIfAllowed(pPlayer, 3599)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 5394)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 8071)) ++learned;
            break;

        case CLASS_PRIEST:
            if (LearnQuestSpellIfAllowed(pPlayer, 2944))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 9035))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 2652))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 10797)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 2651))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 18137)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 6346))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 13896)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 13908)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 27683)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 21564)) ++learned;
            break;

        case CLASS_DRUID:
            if (LearnQuestSpellIfAllowed(pPlayer, 8946))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 5487))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 6795))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 6807))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 1066))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 18960)) ++learned;
            break;

        case CLASS_PALADIN:
            if (LearnQuestSpellIfAllowed(pPlayer, 7328))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 13819)) ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 23214)) ++learned;
            break;

        default:
            break;
    }

    return learned;
}

static bool CastTriggeredSpellOnPlayer(Player* pPlayer, Creature* pCreatureCaster, uint32 spellId)
{
    // Zweck: Teach-/Item-Spell sauber "ausführen" (auch wenn es ein reiner Learn-Container ist)
    if (!pPlayer || !spellId)
        return false;

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(spellId);
    if (!proto)
        return false;

    if (!pPlayer->IsSpellFitByClassAndRace(spellId))
        return false;

    if (proto->spellLevel > 0 && pPlayer->GetLevel() < uint32(proto->spellLevel))
        return false;

    pPlayer->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pPlayer->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pPlayer->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    const bool kTriggered = true;
    Unit* caster = pCreatureCaster ? (Unit*)pCreatureCaster : (Unit*)pPlayer;

    Spell* spell = new Spell(caster, proto, kTriggered);

    SpellCastTargets targets;
    targets.setUnitTarget(pPlayer);

    SpellCastResult cast_result = spell->prepare(std::move(targets));

    if (cast_result != SPELL_CAST_OK)
    {
        delete spell;
        return false;
    }

    return true;
}

static uint32 LearnWarlockGrimoireSpells(Player* pPlayer, Creature* pCreatureCaster)
{
    // Zweck: Warlock-Grimoire Teach-Spells (Vendor/Items) automatisch "anwenden"
    if (!pPlayer)
        return 0;

    uint8 cls = pPlayer->GetByteValue(UNIT_FIELD_BYTES_0, 1);
    if (cls != CLASS_WARLOCK)
        return 0;

    static const uint32 kGrimoireTeachSpells[] =
    {
        20270,
        20312,20313,20314,20315,20316,20317,20318,20319,20320,20321,20322,20323,20324,
        20326,20327,20329,
        20377,20378,20379,20380,20381,20382,20383,20384,20385,20386,20387,20388,20389,20390,
        20391,20392,20393,20394,20395,20396,20397,20398,20399,20400,20401,20402,20403,20404,
        20405,20406,20407,20408,
        20426,20427,20428,20429,20430,20431,20432,20433,20434,20435
    };

    uint32 learned = 0;

    for (size_t i = 0; i < (sizeof(kGrimoireTeachSpells) / sizeof(kGrimoireTeachSpells[0])); ++i)
    {
        uint32 sid = kGrimoireTeachSpells[i];

        // Falls der Spell beim Player bereits als "known" markiert ist, skippen (spart Casts)
        if (pPlayer->HasSpell(sid))
            continue;

        if (CastTriggeredSpellOnPlayer(pPlayer, pCreatureCaster, sid))
            ++learned;
    }

    return learned;
}

static bool CastTrainerTeachSpell(Player* pPlayer, Creature* pCreatureCaster, TrainerSpell const* trainerSpell)
{
    if (!pPlayer || !trainerSpell)
        return false;

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(trainerSpell->spell);
    if (!proto)
        return false;

    pPlayer->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pPlayer->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pPlayer->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    const bool kTriggered = true;
    Unit* caster = pCreatureCaster ? (Unit*)pCreatureCaster : (Unit*)pPlayer;

    Spell* spell = new Spell(caster, proto, kTriggered);

    SpellCastTargets targets;
    targets.setUnitTarget(pPlayer);

    SpellCastResult cast_result = spell->prepare(std::move(targets));

    if (cast_result != SPELL_CAST_OK)
    {
        delete spell;
        return false;
    }

    return true;
}

static uint32 LearnFromTrainerSpellData_OnePass(Player* pPlayer, Creature* pCreatureCaster, TrainerSpellData const* pData)
{
    if (!pPlayer || !pData)
        return 0;

    uint32 learnedCount = 0;

    for (TrainerSpellMap::const_iterator itr = pData->spellList.begin(); itr != pData->spellList.end(); ++itr)
    {
        TrainerSpell const* tSpell = &itr->second;

        TrainerSpellState state = pPlayer->GetTrainerSpellState(tSpell);
        if (state != TRAINER_SPELL_GREEN)
            continue;

        if (CastTrainerTeachSpell(pPlayer, pCreatureCaster, tSpell))
            ++learnedCount;
    }

    return learnedCount;
}

static uint32 LearnAllAvailableInLoop(Player* pPlayer, Creature* pCreatureCaster)
{
    if (!pPlayer)
        return 0;

    uint8 cls = pPlayer->GetByteValue(UNIT_FIELD_BYTES_0, 1);

    if (cls > 0 && cls < 12)
        ResolveTrainerSourcesForClass(cls);

    ResolveWeaponTrainerSources();

    const uint32 kMaxPasses = 50;
    uint32 totalLearned = 0;

    for (uint32 pass = 0; pass < kMaxPasses; ++pass)
    {
        uint32 learnedThisPass = 0;

        learnedThisPass += LearnQuestSpecialSpellsForClass(pPlayer);

        // Warlock Grimoires (Teach-Spells) pro Pass prüfen (nach Level-Up kann Neues verfügbar sein)
        learnedThisPass += LearnWarlockGrimoireSpells(pPlayer, pCreatureCaster);

        if (cls > 0 && cls < 12)
        {
            for (size_t i = 0; i < gClassSources[cls].size(); ++i)
            {
                TrainerSpellData const* classSpells = GetTrainerSpells_EntryFirst_FallbackTemplate(gClassSources[cls][i]);
                if (classSpells)
                    learnedThisPass += LearnFromTrainerSpellData_OnePass(pPlayer, pCreatureCaster, classSpells);
            }
        }

        for (size_t i = 0; i < gWeaponSources.size(); ++i)
        {
            TrainerSpellData const* weaponSpells = GetTrainerSpells_EntryFirst_FallbackTemplate(gWeaponSources[i]);
            if (weaponSpells)
                learnedThisPass += LearnFromTrainerSpellData_OnePass(pPlayer, pCreatureCaster, weaponSpells);
        }

        learnedThisPass += LearnHigherRanksFromSpellChains(pPlayer);

        totalLearned += learnedThisPass;

        if (learnedThisPass == 0)
            break;
    }

    MaxOutWeaponDefenseAndRiding(pPlayer);
    return totalLearned;
}

static uint32 LevelToAndLearn(Player* pPlayer, Creature* pCreatureCaster, uint32 targetLevel)
{
    if (!pPlayer)
        return 0;

    uint32 maxLvl = GetMaxPlayerLevel_Cached();
    if (targetLevel < 1)
        targetLevel = 1;
    if (targetLevel > maxLvl)
        targetLevel = maxLvl;

    uint32 cur = pPlayer->GetLevel();

    if (targetLevel > cur)
    {
        pPlayer->GiveLevel(targetLevel);
        pPlayer->SetUInt32Value(PLAYER_XP, 0);
    }

    return LearnAllAvailableInLoop(pPlayer, pCreatureCaster);
}

static uint32 LearnCurrentLevel(Player* pPlayer, Creature* pCreatureCaster)
{
    return LearnAllAvailableInLoop(pPlayer, pCreatureCaster);
}

static uint32 LevelPlusTenAndLearn(Player* pPlayer, Creature* pCreatureCaster)
{
    if (!pPlayer)
        return 0;

    return LevelToAndLearn(pPlayer, pCreatureCaster, pPlayer->GetLevel() + 10);
}

static uint32 LevelToNextTenAndLearn(Player* pPlayer, Creature* pCreatureCaster)
{
    if (!pPlayer)
        return 0;

    uint32 lvl = pPlayer->GetLevel();
    uint32 nextTen = ((lvl + 9) / 10) * 10;
    if (nextTen <= lvl)
        nextTen = lvl + 10;

    return LevelToAndLearn(pPlayer, pCreatureCaster, nextTen);
}

bool GossipHello_npc_autotrainer(Player* pPlayer, Creature* pCreature)
{
    if (!pPlayer || !pCreature)
        return true;

    pPlayer->PlayerTalkClass->ClearMenus();

    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Alles lernen (aktuelles Level)",            GOSSIP_SENDER_MAIN, GOSSIP_ACTION_LEARN_CURRENT);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "+10 Level und alles lernen",                GOSSIP_SENDER_MAIN, GOSSIP_ACTION_LEVEL_PLUS_10);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Bis naechstes 10er-Level und alles lernen", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_LEVEL_NEXT_TEN);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT,    "Abbrechen",                                 GOSSIP_SENDER_MAIN, GOSSIP_ACTION_CANCEL);

    pPlayer->SEND_GOSSIP_MENU(DEFAULT_GOSSIP_MESSAGE, pCreature->GetObjectGuid());
    return true;
}

bool GossipSelect_npc_autotrainer(Player* pPlayer, Creature* pCreature, uint32 sender, uint32 action)
{
    if (!pPlayer || !pCreature)
        return true;

    if (sender != GOSSIP_SENDER_MAIN)
        return true;

    pPlayer->PlayerTalkClass->ClearMenus();

    if (action == GOSSIP_ACTION_CANCEL)
    {
        pPlayer->CLOSE_GOSSIP_MENU();
        return true;
    }

    uint32 learned = 0;

    switch (action)
    {
        case GOSSIP_ACTION_LEARN_CURRENT:
            learned = LearnCurrentLevel(pPlayer, pCreature);
            break;

        case GOSSIP_ACTION_LEVEL_PLUS_10:
            learned = LevelPlusTenAndLearn(pPlayer, pCreature);
            break;

        case GOSSIP_ACTION_LEVEL_NEXT_TEN:
            learned = LevelToNextTenAndLearn(pPlayer, pCreature);
            break;

        default:
            pPlayer->CLOSE_GOSSIP_MENU();
            return true;
    }

    if (learned == 0)
        pPlayer->GetSession()->SendNotification("Im Moment gibt es nichts Neues zu lernen.");
    else
        pPlayer->GetSession()->SendNotification("Gelernt: %u Zauber/Faehigkeiten.", learned);

    pPlayer->CLOSE_GOSSIP_MENU();
    return true;
}

void AddSC_npc_autotrainer()
{
    Script* pNewScript = new Script;
    pNewScript->Name = "npc_autotrainer";
    pNewScript->pGossipHello  = &GossipHello_npc_autotrainer;
    pNewScript->pGossipSelect = &GossipSelect_npc_autotrainer;
    pNewScript->RegisterSelf();
}

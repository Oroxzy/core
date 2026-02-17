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
    bool   isPetTrainer; // true nur fuer Pet Trainer (Hunter)

    AutoTrainerSource() : trainerId(0), trainerEntry(0), isPetTrainer(false) {}
};

static bool gClassSourcesResolved[12] = { false };
static std::vector<AutoTrainerSource> gClassSources[12];

static bool gWeaponSourcesResolved = false;
static std::vector<AutoTrainerSource> gWeaponSources;

static uint32 GetMaxPlayerLevel_Cached()
{
    // Zweck: Vanilla Max-Level
    return 60;
}

static uint16 GetMaxSkillForPlayerLevel(Player* pPlayer)
{
    // Zweck: Skill-Max nach Level (lvl*5, max 300)
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
    // Zweck: ASCII case-insensitive contains
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
    // Zweck: Wrapper fuer const char*
    if (!needle || !*needle)
        return true;

    if (!haystack || !*haystack)
        return false;

    return StrContainsI(std::string(haystack), needle);
}

static void SetSkillToMaxIfKnown(Player* pPlayer, uint32 skillId, uint16 maxValue)
{
    // Zweck: Skill nur maxen wenn vorhanden
    if (!pPlayer || !skillId)
        return;

    if (pPlayer->GetSkillValue(skillId) <= 0)
        return;

    pPlayer->SetSkill(skillId, maxValue, maxValue);
}

static void MaxOutWeaponDefenseAndRiding(Player* pPlayer)
{
    // Zweck: Weapon/Defense/Riding Skills auf Level-Max
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
    // Zweck: Trainerquellen fuer Klasse sammeln (inkl. Pet Trainer fuer Hunter)
    // WICHTIG:
    // - Pet Trainer (Hunter) ist erlaubt und wird markiert (isPetTrainer = true)
    // - Demon Trainer (Warlock) NICHT ueber TrainerSpellData lernen (Grimoires hardcoded) => wird hier NICHT als Source aufgenommen

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

        const bool isPetTrainer   = StrContainsI(cInfo->subname, "Pet Trainer");
        const bool isDemonTrainer = StrContainsI(cInfo->subname, "Demon Trainer");

        // Demon Trainer wird bewusst nicht als Trainer-Source verwendet (Grimoires hardcoded)
        if (isDemonTrainer)
            continue;

        // Normal: nur "normale" Trainer
        // Ausnahme: Pet Trainer (Hunter) darf auch abweichen
        if (!isPetTrainer)
        {
            if (cInfo->trainer_type != 0)
                continue;

            if (cInfo->trainer_class != uint32(playerClass))
                continue;
        }
        else
        {
            if (playerClass != CLASS_HUNTER)
                continue;
        }

        bool ok = false;

        switch (playerClass)
        {
            case CLASS_MAGE:
                ok = StrContainsI(cInfo->subname, "Mage Trainer") || StrContainsI(cInfo->subname, "Portal Trainer");
                break;

            case CLASS_HUNTER:
                ok = StrContainsI(cInfo->subname, "Hunter Trainer") || isPetTrainer;
                break;

            case CLASS_WARLOCK:
                ok = StrContainsI(cInfo->subname, "Warlock Trainer"); // Demon Trainer ist oben schon raus
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
        src.isPetTrainer = isPetTrainer;

        gClassSources[playerClass].push_back(src);

        if (tid)
            seenTrainerIds.push_back(tid);
        else
            seenEntries.push_back(entry);
    }
}

static void ResolveWeaponTrainerSources()
{
    // Zweck: Weapon Master Quellen einmalig sammeln
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
        src.isPetTrainer = false;

        gWeaponSources.push_back(src);

        if (tid)
            seenTrainerIds.push_back(tid);
        else
            seenEntries.push_back(entry);
    }
}

static TrainerSpellData const* GetTrainerSpells_EntryFirst_FallbackTemplate(AutoTrainerSource const& src)
{
    // Zweck: TrainerSpellData holen (Entry bevorzugt, sonst Template)
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
    // Zweck: Next-in-chain Cache bauen (einmalig)
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
    // Zweck: Detect "pure learn container" spells (nur LearnSpell effects)
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
    // Zweck: Hoehere Ranks aus Spell Chains lernen
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
    // Zweck: Mindestlevel fuer gewisse Spezial-/Quest-Spells
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
    // Zweck: direkte Spezialspells lernen (Quest etc.)
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
    // Zweck: Spezialspell nur wenn Class/Race passt
    if (!pPlayer || !spellId)
        return false;

    if (!pPlayer->IsSpellFitByClassAndRace(spellId))
        return false;

    return LearnDirectSpellIfMissing(pPlayer, spellId);
}

static uint32 LearnQuestSpecialSpellsForClass(Player* pPlayer)
{
    // Zweck: Whitelist Quest-/Spezialspells pro Klasse
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
            if (LearnQuestSpellIfAllowed(pPlayer, 5149))  ++learned;
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

static bool CastTrainerTeachSpellToUnit(Player* pPlayer, Creature* pCreatureCaster, TrainerSpell const* trainerSpell, Unit* target)
{
    // Zweck: TrainerSpell wie Trainer "ausfuehren" (triggered cast auf target)
    if (!pPlayer || !trainerSpell || !target)
        return false;

    // Crash-Schutz: target muss im World-Kontext gueltig sein
    if (!target->IsInWorld())
        return false;

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(trainerSpell->spell);
    if (!proto)
        return false;

    pPlayer->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pPlayer->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pPlayer->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    const bool kTriggered = true;
	Unit* caster = (Unit*)pPlayer;

    Spell* spell = new Spell(caster, proto, kTriggered);

	SpellCastTargets targets;
	targets.setUnitTarget(target);
	
	SpellCastResult cast_result = spell->prepare(std::move(targets));

    if (cast_result != SPELL_CAST_OK)
    {
        delete spell;
        return false;
    }

    return true;
}

static uint32 LearnFromTrainerSpellData_OnePass(Player* pPlayer, Creature* pCreatureCaster, TrainerSpellData const* pData, Unit* target)
{
    // Zweck: Ein Pass ueber TrainerSpellData, nur GREEN Spells
    if (!pPlayer || !pData || !target)
        return 0;

    uint32 learnedCount = 0;

    for (TrainerSpellMap::const_iterator itr = pData->spellList.begin(); itr != pData->spellList.end(); ++itr)
    {
        TrainerSpell const* tSpell = &itr->second;

        TrainerSpellState state = pPlayer->GetTrainerSpellState(tSpell);
        if (state != TRAINER_SPELL_GREEN)
            continue;

        if (CastTrainerTeachSpellToUnit(pPlayer, pCreatureCaster, tSpell, target))
            ++learnedCount;
    }

    return learnedCount;
}

static bool CastTriggeredSpellOnPlayer(Player* pPlayer, Creature* pCreatureCaster, uint32 spellId)
{
    // Zweck: Teach-/Item-Spell (Grimoire) auf Player casten (triggered)
    if (!pPlayer || !spellId)
        return false;

    if (!pPlayer->IsInWorld())
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
	Unit* caster = (Unit*)pPlayer;

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

static bool CastTriggeredSpellToUnit(Player* pPlayer, Creature* /*pCreatureCaster*/, uint32 spellId, Unit* target)
{
    // Zweck: Teach-/Item-Spell (Grimoire) triggered auf Ziel (Pet) casten
    // Fix: Caster IMMER Player (NPC kann despawnen => Use-after-free Crash)
    // Fix: Target explizit setzen (Pet)

    if (!pPlayer || !spellId || !target)
        return false;

    if (!pPlayer->IsInWorld())
        return false;

    if (!target->IsInWorld())
        return false;
	
    if (!pPlayer->IsAlive())
        return false;

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(spellId);
    if (!proto)
        return false;

    if (proto->spellLevel > 0 && pPlayer->GetLevel() < uint32(proto->spellLevel))
        return false;

    pPlayer->InterruptSpellsWithChannelFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pPlayer->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_INTERACTING_CANCELS);
    pPlayer->RemoveSpellsCausingAura(SPELL_AURA_MOUNTED);

    const bool kTriggered = true;
    Unit* caster = (Unit*)pPlayer;

    Spell* spell = new Spell(caster, proto, kTriggered);

    SpellCastTargets targets;
    targets.setUnitTarget(target);

    SpellCastResult cast_result = spell->prepare(std::move(targets));

    if (cast_result != SPELL_CAST_OK)
    {
        delete spell;
        return false;
    }

    return true;
}

static uint32 LearnWarlockGrimoireSpells(Player* pPlayer, Creature* /*pCreatureCaster*/)
{
    // Zweck: Warlock-Grimoires HARD-CODED korrekt anwenden:
    // - Pet wird je nach vorhandenem Summon-Spell beschworen (Imp/Voidwalker/Succubus/Felhunter)
    // - NUR die zu diesem Pet gehoerenden Learned-Spells werden gelernt (gemäss CSV)
    // - Lernen erfolgt DIREKT via pet->LearnSpell()
    //
    // Fix: verhindert, dass jedes Pet jeden Spell lernt (Felhunter/Imp/etc strikt getrennt)

    if (!pPlayer)
        return 0;

    uint8 cls = pPlayer->GetByteValue(UNIT_FIELD_BYTES_0, 1);
    if (cls != CLASS_WARLOCK)
        return 0;

    // Summon-Spells fuer Warlock-Pets (Classic)
    static const uint32 kSummonImp       = 688;
    static const uint32 kSummonVoidwalker= 697;
    static const uint32 kSummonSuccubus  = 712;
    static const uint32 kSummonFelhunter = 691;

    struct GrimoirePetSpell
    {
        uint32 summonSpell;   // welches Pet (ueber UNIT_CREATED_BY_SPELL)
        uint32 teachSpell;    // Grimoire-Teach-Spell (nur fuer Doku/Debug)
        uint32 learnedSpell;  // echter Pet-Spell der gelernt werden soll
    };

    // HARD-CODE Mapping gemäss deinem CSV (TeachSpell -> LearnedSpell), strikt pro Pet
    static const GrimoirePetSpell kMap[] =
    {
        // ---------------- IMP ----------------
        { kSummonImp, 20270,  7799  }, // Firebolt (Rank 2)
        { kSummonImp, 20312,  7800  }, // Firebolt (Rank 3)
        { kSummonImp, 20313,  7801  }, // Firebolt (Rank 4)
        { kSummonImp, 20314,  7802  }, // Firebolt (Rank 5)
        { kSummonImp, 20315,  11762 }, // Firebolt (Rank 6)
        { kSummonImp, 20316,  11763 }, // Firebolt (Rank 7)
        { kSummonImp, 20329,  4511  }, // Phase Shift

        { kSummonImp, 20397,  6307  }, // Blood Pact (Rank 1)
        { kSummonImp, 20318,  7804  }, // Blood Pact (Rank 2)
        { kSummonImp, 20319,  7805  }, // Blood Pact (Rank 3)
        { kSummonImp, 20320,  11766 }, // Blood Pact (Rank 4)
        { kSummonImp, 20321,  11767 }, // Blood Pact (Rank 5)

        { kSummonImp, 20322,  2947  }, // Fire Shield (Rank 1)
        { kSummonImp, 20323,  8316  }, // Fire Shield (Rank 2)
        { kSummonImp, 20324,  8317  }, // Fire Shield (Rank 3)
        { kSummonImp, 20326,  11770 }, // Fire Shield (Rank 4)
        { kSummonImp, 20327,  11771 }, // Fire Shield (Rank 5)

        // ---------------- VOIDWALKER ----------------
        { kSummonVoidwalker, 20317,  7809  }, // Torment (Rank 2)
        { kSummonVoidwalker, 20377,  7810  }, // Torment (Rank 3)
        { kSummonVoidwalker, 20378,  7811  }, // Torment (Rank 4)
        { kSummonVoidwalker, 20379,  11774 }, // Torment (Rank 5)
        { kSummonVoidwalker, 20380,  11775 }, // Torment (Rank 6)

        { kSummonVoidwalker, 20381,  7812  }, // Sacrifice (Rank 1)
        { kSummonVoidwalker, 20382,  19438 }, // Sacrifice (Rank 2)
        { kSummonVoidwalker, 20383,  19440 }, // Sacrifice (Rank 3)
        { kSummonVoidwalker, 20384,  19441 }, // Sacrifice (Rank 4)
        { kSummonVoidwalker, 20385,  19442 }, // Sacrifice (Rank 5)
        { kSummonVoidwalker, 20386,  19443 }, // Sacrifice (Rank 6)

        { kSummonVoidwalker, 20387,  17767 }, // Consume Shadows (Rank 1)
        { kSummonVoidwalker, 20388,  17850 }, // Consume Shadows (Rank 2)
        { kSummonVoidwalker, 20389,  17851 }, // Consume Shadows (Rank 3)
        { kSummonVoidwalker, 20390,  17852 }, // Consume Shadows (Rank 4)
        { kSummonVoidwalker, 20391,  17853 }, // Consume Shadows (Rank 5)
        { kSummonVoidwalker, 20392,  17854 }, // Consume Shadows (Rank 6)

        { kSummonVoidwalker, 20393,  17735 }, // Suffering (Rank 1)
        { kSummonVoidwalker, 20394,  17750 }, // Suffering (Rank 2)
        { kSummonVoidwalker, 20395,  17751 }, // Suffering (Rank 3)
        { kSummonVoidwalker, 20396,  17752 }, // Suffering (Rank 4)

        // ---------------- SUCCUBUS ----------------
        { kSummonSuccubus, 20398,  7815  }, // Lash of Pain (Rank 2)
        { kSummonSuccubus, 20399,  7816  }, // Lash of Pain (Rank 3)
        { kSummonSuccubus, 20400,  11778 }, // Lash of Pain (Rank 4)
        { kSummonSuccubus, 20401,  11779 }, // Lash of Pain (Rank 5)
        { kSummonSuccubus, 20402,  11780 }, // Lash of Pain (Rank 6)

        { kSummonSuccubus, 20403,  6360  }, // Soothing Kiss (Rank 1)
        { kSummonSuccubus, 20404,  7813  }, // Soothing Kiss (Rank 2)
        { kSummonSuccubus, 20405,  11784 }, // Soothing Kiss (Rank 3)
        { kSummonSuccubus, 20406,  11785 }, // Soothing Kiss (Rank 4)

        { kSummonSuccubus, 20407,  6358  }, // Seduction
        { kSummonSuccubus, 20408,  7870  }, // Lesser Invisibility

        // ---------------- FELHUNTER ----------------
        { kSummonFelhunter, 20426,  19731 }, // Devour Magic (Rank 2)
        { kSummonFelhunter, 20427,  19734 }, // Devour Magic (Rank 3)
        { kSummonFelhunter, 20428,  19736 }, // Devour Magic (Rank 4)

        { kSummonFelhunter, 20429,  19478 }, // Tainted Blood (Rank 1)
        { kSummonFelhunter, 20430,  19655 }, // Tainted Blood (Rank 2)
        { kSummonFelhunter, 20431,  19656 }, // Tainted Blood (Rank 3)
        { kSummonFelhunter, 20432,  19660 }, // Tainted Blood (Rank 4)

        { kSummonFelhunter, 20433,  19244 }, // Spell Lock (Rank 1)
        { kSummonFelhunter, 20434,  19647 }, // Spell Lock (Rank 2)

        { kSummonFelhunter, 20435,  19480 }  // Paranoia
    };

    // Ursprungs-Pet merken (damit wir am Schluss wieder herstellen koennen)
    uint32 originalSummonSpell = 0;
    {
        Pet* curPet = pPlayer->GetPet();
        if (curPet)
            originalSummonSpell = curPet->GetUInt32Value(UNIT_CREATED_BY_SPELL);
    }

    static const uint32 kSummonSpells[] =
    {
        kSummonImp,
        kSummonVoidwalker,
        kSummonSuccubus,
        kSummonFelhunter
    };

    uint32 learned = 0;

    for (size_t s = 0; s < (sizeof(kSummonSpells) / sizeof(kSummonSpells[0])); ++s)
    {
        uint32 summonId = kSummonSpells[s];

        // Nur Pets "durchprobieren", die der Warlock wirklich kennt
        if (!pPlayer->HasSpell(summonId))
            continue;

        SpellEntry const* summonProto = sSpellMgr.GetSpellEntry(summonId);
        if (!summonProto)
            continue;

        if (summonProto->spellLevel > 0 && pPlayer->GetLevel() < uint32(summonProto->spellLevel))
            continue;

        // Pet beschwoeren (triggered)
        pPlayer->CastSpell(pPlayer, summonId, true);

        Pet* pet = pPlayer->GetPet();
        if (!pet)
            continue;

        if (!pet->IsInWorld())
            continue;

        // Sicherstellen, dass das gerade beschworene Pet wirklich zu diesem Summon gehoert
        if (pet->GetUInt32Value(UNIT_CREATED_BY_SPELL) != summonId)
            continue;

        // NUR die zu diesem Pet gehoerenden Spells lernen
        for (size_t i = 0; i < (sizeof(kMap) / sizeof(kMap[0])); ++i)
        {
            if (kMap[i].summonSpell != summonId)
                continue;

            uint32 learnedId = kMap[i].learnedSpell;
            if (!learnedId)
                continue;

            if (pet->HasSpell(learnedId))
                continue;

            SpellEntry const* learnedProto = sSpellMgr.GetSpellEntry(learnedId);
            if (!learnedProto)
                continue;

            // Level-Check am Pet-Spell
            if (learnedProto->spellLevel > 0 && pPlayer->GetLevel() < uint32(learnedProto->spellLevel))
                continue;

            pet->LearnSpell(learnedId);
            ++learned;
        }
    }

    // Urspruengliches Pet wiederherstellen (wenn eines aktiv war)
    if (originalSummonSpell && pPlayer->HasSpell(originalSummonSpell))
        pPlayer->CastSpell(pPlayer, originalSummonSpell, true);

    return learned;
}

static uint32 LearnAllAvailableInLoop(Player* pPlayer, Creature* pCreatureCaster)
{
    // Zweck: Mehrere Paesse, bis nichts mehr zu lernen ist
    if (!pPlayer)
        return 0;

    uint8 cls = pPlayer->GetByteValue(UNIT_FIELD_BYTES_0, 1);

    if (cls > 0 && cls < 12)
        ResolveTrainerSourcesForClass(cls);

    ResolveWeaponTrainerSources();

    const uint32 kMaxPasses = 50;
    uint32 totalLearned = 0;

	// Warlock Grimoires nur einmal pro "All learn" (sonst 50x Summons)
	totalLearned += LearnWarlockGrimoireSpells(pPlayer, pCreatureCaster);
		
    for (uint32 pass = 0; pass < kMaxPasses; ++pass)
    {
        uint32 learnedThisPass = 0;

        learnedThisPass += LearnQuestSpecialSpellsForClass(pPlayer);

        if (cls > 0 && cls < 12)
        {
            for (size_t i = 0; i < gClassSources[cls].size(); ++i)
            {
                AutoTrainerSource const& src = gClassSources[cls][i];

                TrainerSpellData const* classSpells = GetTrainerSpells_EntryFirst_FallbackTemplate(src);
                if (!classSpells)
                    continue;

                // WICHTIG: Pet Trainer Spells beim Hunter sollen IMMER gelernt werden, auch ohne/egal welches Pet.
                // Darum: target ist immer Player (nicht Pet).
                Unit* target = (Unit*)pPlayer;

                learnedThisPass += LearnFromTrainerSpellData_OnePass(pPlayer, pCreatureCaster, classSpells, target);
            }
        }

        for (size_t i = 0; i < gWeaponSources.size(); ++i)
        {
            TrainerSpellData const* weaponSpells = GetTrainerSpells_EntryFirst_FallbackTemplate(gWeaponSources[i]);
            if (weaponSpells)
                learnedThisPass += LearnFromTrainerSpellData_OnePass(pPlayer, pCreatureCaster, weaponSpells, (Unit*)pPlayer);
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
    // Zweck: Level setzen + lernen
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
    // Zweck: Gossip-Menue anzeigen
    if (!pPlayer || !pCreature)
        return true;

    pPlayer->PlayerTalkClass->ClearMenus();

    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Alles lernen (aktuelles Level)",             GOSSIP_SENDER_MAIN, GOSSIP_ACTION_LEARN_CURRENT);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "+10 Level und alles lernen",                 GOSSIP_SENDER_MAIN, GOSSIP_ACTION_LEVEL_PLUS_10);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Bis naechstes 10er-Level und alles lernen",  GOSSIP_SENDER_MAIN, GOSSIP_ACTION_LEVEL_NEXT_TEN);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT,    "Abbrechen",                                  GOSSIP_SENDER_MAIN, GOSSIP_ACTION_CANCEL);

    pPlayer->SEND_GOSSIP_MENU(DEFAULT_GOSSIP_MESSAGE, pCreature->GetObjectGuid());
    return true;
}

bool GossipSelect_npc_autotrainer(Player* pPlayer, Creature* pCreature, uint32 sender, uint32 action)
{
    // Zweck: Auswahl aus dem Gossip-Menue ausfuehren
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

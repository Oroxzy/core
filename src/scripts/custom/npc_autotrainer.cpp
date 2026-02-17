#include "scriptPCH.h"
#include "ObjectMgr.h"
#include <vector>
#include <string>

/*
    npc_autotrainer.cpp

    Zweck:
    - Gossip NPC, der dem Spieler automatisch alle aktuell verfuegbaren Trainer-Spells beibringt (nur "gruene" = TRAINER_SPELL_GREEN).
    - Optional: Level +10 oder bis naechstes 10er-Level und dann lernen.
    - Zusaetzlich: gewisse Quest-/Spezial-Spells (klassenspezifisch) und Warlock-Grimoires.

    Wichtiger Stabilitaets-Fix (Crash):
    - Trainer-/Teach-Spells werden NICHT mehr ueber das Spell-Cast-System (new Spell + prepare) ausgefuehrt,
      sondern wie bei echten Trainern DIREKT gelernt (Player::LearnSpell).
    - Warlock-Grimoire Teach-Spells (Learn-Container) werden ebenfalls NICHT gecastet, sondern der "gelernte"
      Spell wird aus dem Container extrahiert und direkt gelernt.

    Hintergrund:
    - Massencasts in Update-Loops koennen in MaNGOS/vMaNGOS zu Pointer-Update-Crashes fuehren (Spell::UpdatePointers / Object::GetUInt64Value).
*/

enum
{
    GOSSIP_ACTION_LEARN_CURRENT     = GOSSIP_ACTION_INFO_DEF + 1,
    GOSSIP_ACTION_LEVEL_PLUS_10     = GOSSIP_ACTION_INFO_DEF + 2,
    GOSSIP_ACTION_LEVEL_NEXT_TEN    = GOSSIP_ACTION_INFO_DEF + 3,
    GOSSIP_ACTION_CANCEL            = GOSSIP_ACTION_INFO_DEF + 4
};

struct AutoTrainerSource
{
    uint32 trainerId;       // Template-ID (trainer_id)
    uint32 trainerEntry;    // Creature entry

    AutoTrainerSource() : trainerId(0), trainerEntry(0) {}
};

static bool gClassSourcesResolved[12] = { false };
static std::vector<AutoTrainerSource> gClassSources[12];

static bool gWeaponSourcesResolved = false;
static std::vector<AutoTrainerSource> gWeaponSources;

/*
    Zweck: Max-Level Cache.
    Hinweis: Hier fix 60 (Vanilla). Wenn du Core-Config willst, musst du es aus Config/World ziehen.
*/
static uint32 GetMaxPlayerLevel_Cached()
{
    return 60;
}

/*
    Zweck: Skill-Maxwert passend zum Playerlevel (Level*5, max 300).
*/
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

/*
    Zweck: Case-insensitive "contains" fuer ASCII (ohne Locale).
*/
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

/*
    Zweck: Skill auf max setzen, aber nur wenn Skill beim Spieler existiert.
*/
static void SetSkillToMaxIfKnown(Player* pPlayer, uint32 skillId, uint16 maxValue)
{
    if (!pPlayer || !skillId)
        return;

    if (pPlayer->GetSkillValue(skillId) <= 0)
        return;

    pPlayer->SetSkill(skillId, maxValue, maxValue);
}

/*
    Zweck: Weapon, Defense und Riding Skills passend zum Level maxen.
    Hinweis: IDs sind Classic-Skill-IDs (Weapon/Defense/Riding).
*/
static void MaxOutWeaponDefenseAndRiding(Player* pPlayer)
{
    if (!pPlayer)
        return;

    uint16 maxV = GetMaxSkillForPlayerLevel(pPlayer);

    // Weapon Skills / Defense
    SetSkillToMaxIfKnown(pPlayer, 43,  maxV);  // Swords
    SetSkillToMaxIfKnown(pPlayer, 44,  maxV);  // Axes
    SetSkillToMaxIfKnown(pPlayer, 45,  maxV);  // Bows
    SetSkillToMaxIfKnown(pPlayer, 46,  maxV);  // Guns
    SetSkillToMaxIfKnown(pPlayer, 54,  maxV);  // Maces
    SetSkillToMaxIfKnown(pPlayer, 55,  maxV);  // Two-Handed Swords
    SetSkillToMaxIfKnown(pPlayer, 136, maxV);  // Staves
    SetSkillToMaxIfKnown(pPlayer, 160, maxV);  // Two-Handed Maces
    SetSkillToMaxIfKnown(pPlayer, 162, maxV);  // Unarmed
    SetSkillToMaxIfKnown(pPlayer, 173, maxV);  // Daggers
    SetSkillToMaxIfKnown(pPlayer, 176, maxV);  // Thrown
    SetSkillToMaxIfKnown(pPlayer, 226, maxV);  // Crossbows
    SetSkillToMaxIfKnown(pPlayer, 228, maxV);  // Wands
    SetSkillToMaxIfKnown(pPlayer, 229, maxV);  // Polearms
    SetSkillToMaxIfKnown(pPlayer, 172, maxV);  // Two-Handed Axes
    SetSkillToMaxIfKnown(pPlayer, 473, maxV);  // Fist Weapons

    // Riding
    SetSkillToMaxIfKnown(pPlayer, 95,  maxV);  // Defense? (je nach DB/Core) - bleibt wie bei dir
    SetSkillToMaxIfKnown(pPlayer, 762, maxV);  // Riding
}

/*
    Zweck: Trainer-Quellen fuer eine Klasse einmalig sammeln (ohne DB).
    WICHTIG:
    - Pet Trainer NICHT aufnehmen (pet-/familie-/level-abhaengig, macht Autotraining inkonsistent).
    - Demon Trainer NICHT aufnehmen (bei dir ueber Grimoires/Teach-Spells).
*/
static void ResolveTrainerSourcesForClass(uint8 playerClass)
{
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

        // nur "normale" Trainer (trainer_type == 0)
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

/*
    Zweck: Weapon Master Quellen einmalig sammeln.
*/
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

/*
    Zweck: TrainerSpellData holen (erst entry, dann template fallback).
*/
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

/*
    ========= Spell-Chain Cache =========
    Zweck: Naechsten Rank in Chain finden, ohne teuer pro Spell nachzuschlagen.
*/
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

/*
    Zweck: True, wenn der Spell NUR ein LearnSpell-Effect ist und sonst nichts (reiner Teach-Container).
*/
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

/*
    NEU (Crash-Fix):
    Zweck: Aus einem reinen Learn-Container (SPELL_EFFECT_LEARN_SPELL) den "eigentlich zu lernenden" Spell extrahieren.
    Rueckgabe:
    - 0 wenn nicht moeglich
    - sonst SpellId, die wirklich gelernt werden soll
*/
static uint32 ExtractLearnedSpellFromPureContainer(uint32 teachSpellId)
{
    SpellEntry const* proto = sSpellMgr.GetSpellEntry(teachSpellId);
    if (!proto)
        return 0;

    // Nur fuer "pure" Container sauber/zuverlaessig
    if (!IsPureLearnContainerSpell(teachSpellId))
        return 0;

    for (uint8 i = 0; i < 3; ++i)
    {
        if (proto->Effect[i] == SPELL_EFFECT_LEARN_SPELL)
        {
            // In MaNGOS/vMaNGOS steht der gelernte Spell in EffectTriggerSpell[i]
            uint32 learnedId = uint32(proto->EffectTriggerSpell[i]);
            if (learnedId != 0)
                return learnedId;
        }
    }

    return 0;
}

/*
    Zweck: Hoehere Ranks automatisch aus Spellchains lernen (nur wenn Class/Race/Level passt).
    Hinweis: Passive, reine Learn-Container etc. werden bewusst nicht automatisch gelernt.
*/
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

            // Passive nicht automatisch hochziehen (du hattest das so drin, bleibt so)
            if ((nextProto->Attributes & SPELL_ATTR_PASSIVE) != 0)
                break;

            // Teach-Container nicht in Spellbook lernen
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

/*
    Zweck: Mindestlevel fuer bestimmte "Spezial-/Quest-/Teach" Spells.
    Hinweis: Das ist eine harte Whitelist-Gate-Logik (wie bei dir).
*/
static uint32 GetMinLevelForSpecialSpell(uint32 spellId)
{
    switch (spellId)
    {
        // Warlock Summons / Mounts etc.
        case 688:   return 1;   // Summon Imp
        case 697:   return 10;  // Summon Voidwalker
        case 712:   return 20;  // Summon Succubus
        case 691:   return 30;  // Summon Felhunter
        case 1122:  return 50;  // Inferno
        case 5784:  return 40;  // Felsteed
        case 23161: return 60;  // Dreadsteed (Teach chain, je nach DB)
        case 18540: return 60;  // Ritual of Doom? (bleibt wie bei dir)

        // Paladin Mounts etc.
        case 7328:  return 12;
        case 13819: return 40;
        case 23214: return 60;
        case 19752: return 20;

        // Warrior Stances
        case 71:    return 10;
        case 2458:  return 30;

        // Druid Forms
        case 5487:  return 10;
        case 1066:  return 16;
        case 5215:  return 20;
        case 768:   return 20;
        case 783:   return 30;
        case 40120: return 40;
        case 33943: return 60;

        // Hunter basics / pet
        case 1515:  return 10;
        case 5149:  return 10;
        case 2641:  return 10;
        case 883:   return 10;
        case 982:   return 10;
        case 6991:  return 10;
        case 19883: return 30;

        // Shaman Totems etc.
        case 8071:  return 4;
        case 3599:  return 4;
        case 5394:  return 20;
        case 2484:  return 12;
        case 3738:  return 6;

        // Priest racials/quests etc.
        case 6346:  return 20;
        case 9035:  return 1;
        case 18137: return 20;

        // Mage Teleports/Portals
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

        // Rogue
        case 1784:  return 1;
        case 1804:  return 1;
        case 51724: return 1;

        default:
            return 1;
    }
}

/*
    Zweck: Direkter Learn fuer "Spezial" Spells, aber mit Safety:
    - Nicht lernen, wenn schon bekannt
    - Pure Learn-Container NICHT ins Spellbook
    - Level/SpellLevel Gate
*/
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

/*
    Zweck: Quest-/Spezial Spell nur wenn Class/Race passt.
*/
static bool LearnQuestSpellIfAllowed(Player* pPlayer, uint32 spellId)
{
    if (!pPlayer || !spellId)
        return false;

    if (!pPlayer->IsSpellFitByClassAndRace(spellId))
        return false;

    return LearnDirectSpellIfMissing(pPlayer, spellId);
}

/*
    Zweck: Klassenspezifische Quest-/Spezial-Spells lernen (Whitelist).
    Hinweis: Diese Liste bleibt inhaltlich wie bei dir; Gate/FitByClassAndRace verhindert falsches Lernen.
*/
static uint32 LearnQuestSpecialSpellsForClass(Player* pPlayer)
{
    if (!pPlayer)
        return 0;

    uint8 cls = pPlayer->GetByteValue(UNIT_FIELD_BYTES_0, 1);
    uint32 learned = 0;

    switch (cls)
    {
        case CLASS_HUNTER:
            if (LearnQuestSpellIfAllowed(pPlayer, 2641))  ++learned; // Dismiss Pet
            if (LearnQuestSpellIfAllowed(pPlayer, 883))   ++learned; // Call Pet
            if (LearnQuestSpellIfAllowed(pPlayer, 1515))  ++learned; // Tame Beast
            if (LearnQuestSpellIfAllowed(pPlayer, 5149))  ++learned; // Beast Training (Spieler)
            if (LearnQuestSpellIfAllowed(pPlayer, 6991))  ++learned; // Feed Pet
            if (LearnQuestSpellIfAllowed(pPlayer, 982))   ++learned; // Revive Pet
            if (LearnQuestSpellIfAllowed(pPlayer, 19801)) ++learned; // bleibt wie bei dir (FitByClassAndRace gate)
            break;

        case CLASS_ROGUE:
            if (LearnQuestSpellIfAllowed(pPlayer, 2842))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 25347)) ++learned;
            break;

        case CLASS_WARRIOR:
            if (LearnQuestSpellIfAllowed(pPlayer, 2457))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 71))    ++learned; // Defensive Stance
            if (LearnQuestSpellIfAllowed(pPlayer, 7386))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 355))   ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 2458))  ++learned; // Berserker Stance
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
            if (LearnQuestSpellIfAllowed(pPlayer, 5487))  ++learned; // Bear Form
            if (LearnQuestSpellIfAllowed(pPlayer, 6795))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 6807))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 1066))  ++learned; // Aquatic Form
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

/*
    ALT-Funktion (war Crash-relevant, bleibt drin aber wird nicht mehr fuer Trainer/Grimoire verwendet):
    Zweck: Teach-/Item-Spell "casten".
    Hinweis: Wir lassen das als Fallback stehen, aber in diesem Script wird es NICHT mehr benutzt, um Crashes zu vermeiden.
*/
static bool CastTriggeredSpellOnPlayer(Player* pPlayer, Creature* pCreatureCaster, uint32 spellId)
{
    if (!pPlayer || !spellId)
        return false;

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(spellId);
    if (!proto)
        return false;

    if (!pPlayer->IsSpellFitByClassAndRace(spellId))
        return false;

    if (proto->spellLevel > 0 && pPlayer->GetLevel() < uint32(proto->spellLevel))
        return false;

    // Crash-Schutz: Diese Funktion sollte hier NICHT mehr massenhaft in Schleifen laufen.
    // Wir geben bewusst false zurueck, damit der Aufrufer (in diesem Script) nicht castet.
    (void)pCreatureCaster;
    return false;
}

/*
    NEU (Crash-Fix):
    Zweck: Warlock-Grimoire Teach-Spells sicher anwenden, ohne Spell-System zu casten.
    Logik:
    - TeachSpell ist i.d.R. ein reiner Learn-Container -> wir extrahieren den tatsaechlich zu lernenden Spell und lernen direkt.
*/
static uint32 LearnWarlockGrimoireSpells(Player* pPlayer, Creature* pCreatureCaster)
{
    (void)pCreatureCaster;

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
        uint32 teachId = kGrimoireTeachSpells[i];

        // Teach-Spell selbst soll NICHT als "known" im Spellbook landen.
        // Wir lernen stattdessen den eigentlichen Spell aus dem Container.
        uint32 learnedId = ExtractLearnedSpellFromPureContainer(teachId);
        if (learnedId == 0)
            continue;

        if (!pPlayer->IsSpellFitByClassAndRace(learnedId))
            continue;

        if (LearnDirectSpellIfMissing(pPlayer, learnedId))
            ++learned;
    }

    return learned;
}

/*
    ALT-Funktion (war Crash-relevant):
    Zweck: TrainerSpell "casten".
    Fix: Wir lernen TrainerSpells DIREKT (wie Trainer) und casten NICHT.
*/
static bool CastTrainerTeachSpell(Player* pPlayer, Creature* pCreatureCaster, TrainerSpell const* trainerSpell)
{
    (void)pCreatureCaster;

    if (!pPlayer || !trainerSpell)
        return false;

    uint32 sid = trainerSpell->spell;
    if (!sid)
        return false;

    // Trainer-Spell ist der Spell, der gelernt werden soll (kein Teach-Container).
    if (!pPlayer->IsSpellFitByClassAndRace(sid))
        return false;

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(sid);
    if (!proto)
        return false;

    // Level Gate (zusaetzlich zur GREEN-Logik, bleibt defensiv)
    if (proto->spellLevel > 0 && pPlayer->GetLevel() < uint32(proto->spellLevel))
        return false;

    if (pPlayer->HasSpell(sid))
        return false;

    // Direkt lernen = stabil und entspricht Trainer-Flow
    pPlayer->LearnSpell(sid, false);
    return true;
}

/*
    Zweck: Ein Pass ueber TrainerSpellData -> nur GREEN lernen.
*/
static uint32 LearnFromTrainerSpellData_OnePass(Player* pPlayer, Creature* pCreatureCaster, TrainerSpellData const* pData)
{
    if (!pPlayer || !pData)
        return 0;

    uint32 learnedCount = 0;

    for (TrainerSpellMap::const_iterator itr = pData->spellList.begin(); itr != pData->spellList.end(); ++itr)
    {
        TrainerSpell const* tSpell = &itr->second;

        // Nur die Spells, die beim Trainer "grün" waeren
        TrainerSpellState state = pPlayer->GetTrainerSpellState(tSpell);
        if (state != TRAINER_SPELL_GREEN)
            continue;

        if (CastTrainerTeachSpell(pPlayer, pCreatureCaster, tSpell))
            ++learnedCount;
    }

    return learnedCount;
}

/*
    Zweck: Alles verfuegbare Lernen in mehreren Paessen:
    - Quest/Spezial pro Pass
    - Warlock Grimoires pro Pass
    - Klassen-Trainer pro Pass
    - Weapon Master pro Pass
    - Danach Spellchains fuer hoehere Ranks
*/
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

        // Quest-/Spezial-Spells (Whitelist + Levelgate + FitByClassAndRace)
        learnedThisPass += LearnQuestSpecialSpellsForClass(pPlayer);

        // Warlock Grimoires (Teach-Container -> extract + direct learn)
        learnedThisPass += LearnWarlockGrimoireSpells(pPlayer, pCreatureCaster);

        // Klassen-Trainer: nur GREEN
        if (cls > 0 && cls < 12)
        {
            for (size_t i = 0; i < gClassSources[cls].size(); ++i)
            {
                TrainerSpellData const* classSpells = GetTrainerSpells_EntryFirst_FallbackTemplate(gClassSources[cls][i]);
                if (classSpells)
                    learnedThisPass += LearnFromTrainerSpellData_OnePass(pPlayer, pCreatureCaster, classSpells);
            }
        }

        // Weapon Master: nur GREEN
        for (size_t i = 0; i < gWeaponSources.size(); ++i)
        {
            TrainerSpellData const* weaponSpells = GetTrainerSpells_EntryFirst_FallbackTemplate(gWeaponSources[i]);
            if (weaponSpells)
                learnedThisPass += LearnFromTrainerSpellData_OnePass(pPlayer, pCreatureCaster, weaponSpells);
        }

        // Hoehere Ranks aus Spellchains (defensiv)
        learnedThisPass += LearnHigherRanksFromSpellChains(pPlayer);

        totalLearned += learnedThisPass;

        // Wenn in diesem Pass nichts mehr neu kam -> fertig
        if (learnedThisPass == 0)
            break;
    }

    // Skills passend zum Level maxen
    MaxOutWeaponDefenseAndRiding(pPlayer);

    return totalLearned;
}

/*
    Zweck: Level setzen (bis max) und danach lernen.
*/
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

/*
    GossipHello:
    - Menue aufbauen
*/
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

/*
    GossipSelect:
    - Aktion ausfuehren
    - Lernen/Leveln
*/
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

/*
    Script-Registration
*/
void AddSC_npc_autotrainer()
{
    Script* pNewScript = new Script;
    pNewScript->Name = "npc_autotrainer";
    pNewScript->pGossipHello  = &GossipHello_npc_autotrainer;
    pNewScript->pGossipSelect = &GossipSelect_npc_autotrainer;
    pNewScript->RegisterSelf();
}

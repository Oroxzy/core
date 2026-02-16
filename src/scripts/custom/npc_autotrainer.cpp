#include "scriptPCH.h"
#include "ObjectMgr.h"
#include "World.h"
#include <vector>
#include <string>
#include <algorithm>

enum
{
    GOSSIP_ACTION_LEARN_ALL = GOSSIP_ACTION_INFO_DEF + 1,
    GOSSIP_ACTION_CANCEL    = GOSSIP_ACTION_INFO_DEF + 2
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

// ------------------------------------------------------------
// PATCH-FIELD Anpassung (JE NACH vMaNGOS BRANCH)
// ------------------------------------------------------------
// In deinem SpellEntry heisst das Feld evtl. NICHT "patch".
// -> Öffne: core/src/game/Spells/SpellEntry.h
// -> suche nach "patch" und nimm den Member-Namen.
// Beispiel: patch / spellPatch / requiredPatch / introducedInPatch / etc.
#define SPELL_PATCH_FIELD patch

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

    // Weapon Skills (SkillLine IDs Classic)
    SetSkillToMaxIfKnown(pPlayer, 43,  maxV); // Swords
    SetSkillToMaxIfKnown(pPlayer, 44,  maxV); // Axes
    SetSkillToMaxIfKnown(pPlayer, 45,  maxV); // Bows
    SetSkillToMaxIfKnown(pPlayer, 46,  maxV); // Guns
    SetSkillToMaxIfKnown(pPlayer, 54,  maxV); // Maces
    SetSkillToMaxIfKnown(pPlayer, 55,  maxV); // Two-Handed Swords
    SetSkillToMaxIfKnown(pPlayer, 136, maxV); // Staves
    SetSkillToMaxIfKnown(pPlayer, 160, maxV); // Two-Handed Maces
    SetSkillToMaxIfKnown(pPlayer, 162, maxV); // Unarmed
    SetSkillToMaxIfKnown(pPlayer, 173, maxV); // Daggers
    SetSkillToMaxIfKnown(pPlayer, 176, maxV); // Thrown
    SetSkillToMaxIfKnown(pPlayer, 226, maxV); // Crossbows
    SetSkillToMaxIfKnown(pPlayer, 228, maxV); // Wands
    SetSkillToMaxIfKnown(pPlayer, 229, maxV); // Polearms
    SetSkillToMaxIfKnown(pPlayer, 172, maxV); // Two-Handed Axes
    SetSkillToMaxIfKnown(pPlayer, 473, maxV); // Fist Weapons

    // Defense
    SetSkillToMaxIfKnown(pPlayer, 95, maxV);

    // Riding (762)
    SetSkillToMaxIfKnown(pPlayer, 762, maxV);
}

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
                ok = StrContainsI(cInfo->subname, "Hunter Trainer") || StrContainsI(cInfo->subname, "Pet Trainer");
                break;

            case CLASS_WARLOCK:
                ok = StrContainsI(cInfo->subname, "Warlock Trainer") || StrContainsI(cInfo->subname, "Demon Trainer");
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

// ------------------------------------------------------------
// Level + Patch Gate fuer DIRECT Learn (Quest/Book/Special)
// ------------------------------------------------------------

static uint32 GetMinLevelOverrideForSpecialSpell(uint32 spellId)
{
    // Zweck: Nur dort overriden, wo DBC-SpellLevel nicht passt (Mount/Pet/Quest etc.)
    switch (spellId)
    {
        // Warlock Pets / Mounts
        case 688:   return 1;
        case 697:   return 10;
        case 712:   return 20;
        case 691:   return 30;
        case 1122:  return 50;
        case 5784:  return 40;
        case 23161: return 60;

        // Paladin Mounts / Redemption
        case 7328:  return 12;
        case 13819: return 40;
        case 23214: return 60;

        // Warrior Stances
        case 71:    return 10;
        case 2458:  return 30;

        // Hunter Core
        case 1515:  return 10;
        case 5149:  return 10;
        case 2641:  return 10;
        case 883:   return 10;
        case 982:   return 10;
        case 6991:  return 10;

        default:
            return 0; // kein Override
    }
}

static uint32 GetMinLevelFromSpellEntry(SpellEntry const* proto)
{
    if (!proto)
        return 1;

    uint32 lvl = (uint32)proto->spellLevel;
    if (lvl < 1)
        lvl = 1;

    return lvl;
}

static uint32 GetCurrentWoWPatch()
{
    // In deinem Core ist GetWoWPatch() offenbar eine FREIE Funktion (siehe World::GetPatchName()).
    // Darum NICHT: sWorld.GetWoWPatch()
    return (uint32)GetWoWPatch();
}

static bool IsSpellAllowedByPatch(SpellEntry const* proto)
{
    if (!proto)
        return false;

    uint32 currentPatch = GetCurrentWoWPatch();

    // Wenn Compile-Error: proto->SPELL_PATCH_FIELD existiert nicht -> Macro oben anpassen!
    uint32 spellPatch = (uint32)proto->SPELL_PATCH_FIELD;

    return (spellPatch <= currentPatch);
}

static bool LearnDirectSpellIfMissing(Player* pPlayer, uint32 spellId)
{
    // Zweck: Quest-/Spezial-/Book-Spell direkt als End-Spell lernen, aber mit Level+Patch Gate
    if (!pPlayer || !spellId)
        return false;

    if (pPlayer->HasSpell(spellId))
        return false;

    SpellEntry const* proto = sSpellMgr.GetSpellEntry(spellId);
    if (!proto)
        return false;

    if (!IsSpellAllowedByPatch(proto))
        return false;

    uint32 minLvlFromDBC   = GetMinLevelFromSpellEntry(proto);
    uint32 minLvlOverride  = GetMinLevelOverrideForSpecialSpell(spellId);
    uint32 minLvl          = std::max(minLvlFromDBC, minLvlOverride);

    if (pPlayer->GetLevel() < minLvl)
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
            if (LearnQuestSpellIfAllowed(pPlayer, 5149))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 6991))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 982))   ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 19801)) ++learned; // Book/Quest
            break;

        case CLASS_ROGUE:
            if (LearnQuestSpellIfAllowed(pPlayer, 2842))  ++learned;
            if (LearnQuestSpellIfAllowed(pPlayer, 25347)) ++learned; // Book
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
            if (LearnQuestSpellIfAllowed(pPlayer, 27683)) ++learned; // Book
            if (LearnQuestSpellIfAllowed(pPlayer, 21564)) ++learned; // Book
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

// ------------------------------------------------------------
// Trainer "Teach" Cast (CRASH FIX)
// ------------------------------------------------------------

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

    // Bei OK NICHT löschen -> Spell-System übernimmt Ownership/Lifecycle
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

        // Quest-/Spezial-/Book-Spells (mit Level+Patch Gate)
        learnedThisPass += LearnQuestSpecialSpellsForClass(pPlayer);

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

        totalLearned += learnedThisPass;

        if (learnedThisPass == 0)
            break;
    }

    MaxOutWeaponDefenseAndRiding(pPlayer);
    return totalLearned;
}

bool GossipHello_npc_autotrainer(Player* pPlayer, Creature* pCreature)
{
    if (!pPlayer || !pCreature)
        return true;

    pPlayer->PlayerTalkClass->ClearMenus();

    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Alles lernen (gratis)", GOSSIP_SENDER_MAIN, GOSSIP_ACTION_LEARN_ALL);
    pPlayer->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT,    "Abbrechen",            GOSSIP_SENDER_MAIN, GOSSIP_ACTION_CANCEL);

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

    if (action != GOSSIP_ACTION_LEARN_ALL)
        return true;

    uint32 learned = LearnAllAvailableInLoop(pPlayer, pCreature);

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

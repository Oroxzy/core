#include "AutoLearnCore.h"

// ---------------------------------------------------------------------------
// vMaNGOS API-Kompatibilitaet (Branch-agnostisch)
// ---------------------------------------------------------------------------
#ifndef TEMPLATE_NPC_ENABLE_PETS
    // Falls dein Branch Pet/Player APIs anders benennt, setze das vorerst auf 0,
    // damit Gear/Talents/Cache sauber bauen. Danach passen wir Pet-Teil gezielt an.
    #define TEMPLATE_NPC_ENABLE_PETS 0
#endif

static inline uint8 TemplateNpc_GetPlayerClassId(Player* player)
{
    if (!player)
        return 0;
    // Branch-agnostisch: Class steckt in UNIT_FIELD_BYTES_0 Byte 1
    return player->GetByteValue(UNIT_FIELD_BYTES_0, 1);
}

static inline uint32 TemplateNpc_GetPlayerLevel(Player* player)
{
    if (!player)
        return 0;
    return player->GetLevel();
}


#include "ScriptMgr.h"
#include "Chat.h"
#include "GameEventMgr.h"
#include "TemplateNPC.h"
#include "AccountMgr.h"
#include "Player.h"
#include "Spell.h"
#include "ObjectMgr.h"
#include "Database/DatabaseEnv.h"
#include "Bag.h"
#include "Opcodes.h"



#ifndef SMSG_PET_UNLEARN_CONFIRM
// Core-Unterschiede: falls dieses Opcode nicht existiert, wird der Packet-Teil kompiliert aber nicht genutzt.
#define SMSG_PET_UNLEARN_CONFIRM 0
#endif

#ifndef MAX_BAG_SIZE
// Fallback: wenn MAX_BAG_SIZE im Core nicht vorhanden ist, nutze 36 (Standard-Taschen-Slots). 
// Wird nur in seltenen Randfaellen genutzt; bessere Logik verwendet Bag::GetBagSize().
#define MAX_BAG_SIZE 36
#endif

// ------------------------------------------------------------
// Helper: Klasse als String-Key (wie bisher in der DB gespeichert)
// ------------------------------------------------------------
static const char* GetClassKeyString(Player* player)
{
    if (!player)
        return "UNKNOWN";

    switch (TemplateNpc_GetPlayerClassId(player))
    {
        case CLASS_WARRIOR: return "WARRIOR";
        case CLASS_PALADIN: return "PALADIN";
        case CLASS_HUNTER: return "HUNTER";
        case CLASS_ROGUE: return "ROGUE";
        case CLASS_PRIEST: return "PRIEST";
        case CLASS_SHAMAN: return "SHAMAN";
        case CLASS_MAGE: return "MAGE";
        case CLASS_WARLOCK: return "WARLOCK";
        case CLASS_DRUID: return "DRUID";
        default: break;
    }

    return "UNKNOWN";
}


#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <memory>
#include <utility>


//std::string spec;


// ------------------------------------------------------------
// TemplateNPC Cache (RAM) – reduziert Runtime-DB Queries im Gossip
// Lädt Template-Daten einmalig beim Script-Init (Worldstart) und kann bei Bedarf neu geladen werden.
// ------------------------------------------------------------
namespace TemplateNpcCache
{
    struct GearItem
    {
        uint32 entry = 0;
        uint32 enchant = 0;
    };

    struct TalentEntry
    {
        uint32 talentId = 0;
        uint8  rank = 0;
    };

    struct TemplateData
    {
        uint32 tempId = 0;
        std::string classKey;          // wie in DB gespeichert (z.B. "WARRIOR")
        std::string gossipText;
        uint32 talentTabId = 0;
        uint8  patch = 0;

        std::unordered_map<uint8, GearItem> gearBySlot;
        std::vector<TalentEntry> talents;
    };

    // classKey -> (tempId -> data)
    static std::unordered_map<std::string, std::unordered_map<uint32, TemplateData> > sTemplatesByClass;
    // classKey -> talentTabId -> list of tempIds (sortiert)
    static std::unordered_map<std::string, std::unordered_map<uint32, std::vector<uint32> > > sTempIdsByClassAndTab;
    // classKey -> list of talentTabId (sortiert)
    static std::unordered_map<std::string, std::vector<uint32> > sTabsByClass;

    static bool sLoaded = false;

    static void Clear()
    {
        sTemplatesByClass.clear();
        sTempIdsByClassAndTab.clear();
        sTabsByClass.clear();
        sLoaded = false;
    }

    static void BuildIndexes()
    {
        sTempIdsByClassAndTab.clear();
        sTabsByClass.clear();

        for (auto& itC : sTemplatesByClass)
        {
            const std::string& classKey = itC.first;
            auto& byTemp = itC.second;

            std::unordered_map<uint32, std::vector<uint32> > byTab;
            std::unordered_set<uint32> tabSet;

            for (auto& itT : byTemp)
            {
                const TemplateData& td = itT.second;
                byTab[td.talentTabId].push_back(td.tempId);
                tabSet.insert(td.talentTabId);
            }

            // sort tempIds per tab
            for (auto& itTab : byTab)
                std::sort(itTab.second.begin(), itTab.second.end());

            // build sorted tabs
            std::vector<uint32> tabs;
            tabs.reserve(tabSet.size());
            for (uint32 v : tabSet) tabs.push_back(v);
            std::sort(tabs.begin(), tabs.end());

            sTempIdsByClassAndTab[classKey] = std::move(byTab);
            sTabsByClass[classKey] = std::move(tabs);
        }
    }

    static void LoadFromDB()
    {
        Clear();

        // 1) Gear/Templates
        std::unique_ptr<QueryResult> resGear(CharacterDatabase.PQuery(
            "SELECT temp_id, class, gossip_text, item_slot, item_entry, item_enchant, talent_tab_id, patch "
            "FROM template_npc_gear"));

        if (resGear)
        {
            do
            {
                Field* f = resGear->Fetch();
                uint32 tempId = f[0].GetUInt32();
                std::string classKey = f[1].GetString();
                std::string gossipText = f[2].GetString();
                uint8 slot = f[3].GetUInt8();
                uint32 itemEntry = f[4].GetUInt32();
                uint32 itemEnchant = f[5].GetUInt32();
                uint32 tabId = f[6].GetUInt32();
                uint8 patch = f[7].GetUInt8();

                TemplateData& td = sTemplatesByClass[classKey][tempId];
                td.tempId = tempId;
                td.classKey = classKey;
                td.gossipText = gossipText;
                td.talentTabId = tabId;
                td.patch = patch;

                GearItem gi;
                gi.entry = itemEntry;
                gi.enchant = itemEnchant;
                td.gearBySlot[slot] = gi;

            } while (resGear->NextRow());
        }

        // 2) Talents
        std::unique_ptr<QueryResult> resTal(CharacterDatabase.PQuery(
            "SELECT temp_id, class, talent_id, rank "
            "FROM template_npc_talents"));

        if (resTal)
        {
            do
            {
                Field* f = resTal->Fetch();
                uint32 tempId = f[0].GetUInt32();
                std::string classKey = f[1].GetString();
                uint32 talentId = f[2].GetUInt32();
                uint8 rank = f[3].GetUInt8();

                auto itC = sTemplatesByClass.find(classKey);
                if (itC == sTemplatesByClass.end())
                    continue;

                auto itT = itC->second.find(tempId);
                if (itT == itC->second.end())
                    continue;

                TalentEntry te;
                te.talentId = talentId;
                te.rank = rank;
                itT->second.talents.push_back(te);

            } while (resTal->NextRow());
        }

        // optional: sort talents for deterministic order
        for (auto& itC : sTemplatesByClass)
        {
            for (auto& itT : itC.second)
            {
                auto& v = itT.second.talents;
                std::sort(v.begin(), v.end(), [](const TalentEntry& a, const TalentEntry& b)
                {
                    if (a.talentId != b.talentId) return a.talentId < b.talentId;
                    return a.rank < b.rank;
                });
            }
        }

        BuildIndexes();
        sLoaded = true;
    }

    static void EnsureLoaded()
    {
        if (!sLoaded)
            LoadFromDB();
    }

    static const TemplateData* GetTemplate(Player* player, uint32 tempId)
    {
        if (!player)
            return nullptr;

        EnsureLoaded();

        std::string classKey = GetClassKeyString(player);
        auto itC = sTemplatesByClass.find(classKey);
        if (itC == sTemplatesByClass.end())
            return nullptr;

        auto itT = itC->second.find(tempId);
        if (itT == itC->second.end())
            return nullptr;

        return &itT->second;
    }

    static bool HasAnyForClass(Player* player)
    {
        if (!player) return false;
        EnsureLoaded();
        std::string classKey = GetClassKeyString(player);
        auto itC = sTemplatesByClass.find(classKey);
        return (itC != sTemplatesByClass.end() && !itC->second.empty());
    }

    static const std::vector<uint32>* GetTabsForClass(Player* player)
    {
        if (!player) return nullptr;
        EnsureLoaded();
        std::string classKey = GetClassKeyString(player);
        auto it = sTabsByClass.find(classKey);
        if (it == sTabsByClass.end())
            return nullptr;
        return &it->second;
    }

    static const std::vector<uint32>* GetTempIdsForTab(Player* player, uint32 tabId)
    {
        if (!player) return nullptr;
        EnsureLoaded();
        std::string classKey = GetClassKeyString(player);
        auto itC = sTempIdsByClassAndTab.find(classKey);
        if (itC == sTempIdsByClassAndTab.end())
            return nullptr;

        auto itT = itC->second.find(tabId);
        if (itT == itC->second.end())
            return nullptr;

        return &itT->second;
    }

    static bool GetGearItem(Player* player, uint32 tempId, uint8 slot, uint32& outEntry, uint32& outEnchant)
    {
        outEntry = 0;
        outEnchant = 0;

        const TemplateData* td = GetTemplate(player, tempId);
        if (!td) return false;

        auto it = td->gearBySlot.find(slot);
        if (it == td->gearBySlot.end())
            return false;

        outEntry = it->second.entry;
        outEnchant = it->second.enchant;
        return true;
    }

    static const std::string* GetGossipText(Player* player, uint32 tempId)
    {
        const TemplateData* td = GetTemplate(player, tempId);
        if (!td) return nullptr;
        return &td->gossipText;
    }

    static uint8 GetPatch(Player* player, uint32 tempId)
    {
        const TemplateData* td = GetTemplate(player, tempId);
        if (!td) return 0;
        return td->patch;
    }

    static const std::vector<TalentEntry>* GetTalents(Player* player, uint32 tempId)
    {
        const TemplateData* td = GetTemplate(player, tempId);
        if (!td) return nullptr;
        return &td->talents;
    }
} // namespace TemplateNpcCache




// generic defines

#define GOSSIP_SENDER_TEMP_DELETE           10000001
#define GOSSIP_SENDER_TEMP_ADD              10000002
#define GOSSIP_SENDER_TEMP_EQUIP            10000003
#define GOSSIP_SENDER_PET_DELETE            10000004
#define GOSSIP_SENDER_PET_ADD               10000005
#define GOSSIP_SENDER_PET_SELECT            10000006
#define GOSSIP_SENDER_TEMP_CONFIRM          10000007
#define GOSSIP_SENDER_TEMP_SAVE_PREMADENAME 10000008
#define GOSSIP_SENDER_TEMP_SPECS            10000009

// spell defines

#define COOL_VISUAL_SPELL       17451
#define COOL_VISUAL_SPELL_2     17321
#define COOL_VISUAL_SPELL_3     19473

// item defines

#define BOTTOMLESS_BAG                  14156
#define ANCIENT_SINEW_WRAPPED_LAMINA    18714
#define DOOMSHOT                        12654
#define CORE_FELCLOTH_BAG               21342
#define SOUL_SHARD                      6265
#define EARTH_TOTEM                     5175
#define AIR_TOTEM                       5178
#define WATER_TOTEM                     5177
#define FIRE_TOTEM                      5176

// npc defines

#define MAGE_NPC        600003
#define HUNTER_NPC      600001
#define ROGUE_NPC       600006
#define WARRIOR_NPC     600011
#define WARLOCK_NPC     600012
#define PALADIN_NPC     600010
#define PRIEST_NPC      600015
#define SHAMAN_NPC      600014
#define DRUID_NPC       600016

// warlock pet enties

#define WARLOCK_PET_IMP         416
#define WARLOCK_PET_SUCCUBUS    1863
#define WARLOCK_PET_VOIDWALKER  1860
#define WARLOCK_PET_FELHUNTER   417

// warlock pet spells (only max ranks)

#define WARLOCK_PETSPELL_IMP_FIREBOLT_RANK7     11763
#define WARLOCK_PETSPELL_IMP_FIRE_SHIELD_RANK5  11771
#define WARLOCK_PETSPELL_IMP_BLOOD_PACT_RANK5   11767
#define WARLOCK_PETSPELL_IMP_PHASE_SHIFT        4511

#define WARLOCK_PETSPELL_SUCCUBUS_SEDUCTION             6358
#define WARLOCK_PETSPELL_SUCCUBUS_LESSER_INVISIBILITY   7870
#define WARLOCK_PETSPELL_SUCCUBUS_LASH_OF_PAIN_RANK6    11780
#define WARLOCK_PETSPELL_SUCCUBUS_SOOTHING_KISS         11785

#define WARLOCK_PETSPELL_VOIDWALKER_CONSUME_SHADOWS_RANK6   17854
#define WARLOCK_PETSPELL_VOIDWALKER_SACRIFICE_RANK6         19443
#define WARLOCK_PETSPELL_VOIDWALKER_TORMENT_RANK6           11775
#define WARLOCK_PETSPELL_VOIDWALKER_SUFFERING_RANK4         17752

#define WARLOCK_PETSPELL_FELHUNTER_TAINTED_BLOOD_RANK4  19660
#define WARLOCK_PETSPELL_FELHUNTER_DEVOUR_MAGIC_RANK4   19736
#define WARLOCK_PETSPELL_FELHUNTER_SPELL_LOCK_RANK2     19647
#define WARLOCK_PETSPELL_FELHUNTER_PARANOIA             19480

// gossip defines

#define GET_LVL_60          GOSSIP_ACTION_INFO_DEF+1
#define MAKE_PET_HAPPY      GOSSIP_ACTION_INFO_DEF+2
#define SHOW_PETS           GOSSIP_ACTION_INFO_DEF+3
#define SAVE_PET            GOSSIP_ACTION_INFO_DEF+4
#define DELETE_PET          GOSSIP_ACTION_INFO_DEF+5
#define RESET_TALENTS       GOSSIP_ACTION_INFO_DEF+6
#define RESET_COOLDOWNS_AND_CHARGES     GOSSIP_ACTION_INFO_DEF+7
#define DELETE_GEAR         GOSSIP_ACTION_INFO_DEF+8
#define ALL_BUFFS           GOSSIP_ACTION_INFO_DEF+9
#define SHOW_TEMP           GOSSIP_ACTION_INFO_DEF+10
#define SAVE_TEMP           GOSSIP_ACTION_INFO_DEF+11
#define DELETE_TEMP         GOSSIP_ACTION_INFO_DEF+12
#define TEACH_WARLOCK_PET   GOSSIP_ACTION_INFO_DEF+13
#define SHOW_ILVL           GOSSIP_ACTION_INFO_DEF+14
#define FIX_DB              GOSSIP_ACTION_INFO_DEF+15
#define SHOW_SPECS          GOSSIP_ACTION_INFO_DEF+16
#define SELECT_SPEC_BASE 900000
#define UPGRADE_TALENTS          GOSSIP_ACTION_INFO_DEF+17
#define LEVEL_LEARN_CURRENT     GOSSIP_ACTION_INFO_DEF+90
#define LEVEL_LEARN_PLUS_10     GOSSIP_ACTION_INFO_DEF+91
#define LEVEL_LEARN_NEXT_TEN    GOSSIP_ACTION_INFO_DEF+92
#define LEVEL_LEARN_TO_60       GOSSIP_ACTION_INFO_DEF+93
#define LEVEL_LEARN_CANCEL      GOSSIP_ACTION_INFO_DEF+94

#define SHOW_PROF_MENU          GOSSIP_ACTION_INFO_DEF+120
#define PROF_LEARN_FIRST_AID    GOSSIP_ACTION_INFO_DEF+121
#define PROF_LEARN_COOKING      GOSSIP_ACTION_INFO_DEF+122
#define PROF_LEARN_FISHING      GOSSIP_ACTION_INFO_DEF+123
#define PROF_LEARN_ALCHEMY      GOSSIP_ACTION_INFO_DEF+124
#define PROF_LEARN_BLACKSMITH   GOSSIP_ACTION_INFO_DEF+125
#define PROF_LEARN_ENCHANTING   GOSSIP_ACTION_INFO_DEF+126
#define PROF_LEARN_ENGINEERING  GOSSIP_ACTION_INFO_DEF+127
#define PROF_LEARN_HERBALISM    GOSSIP_ACTION_INFO_DEF+128
#define PROF_LEARN_LEATHERWORK  GOSSIP_ACTION_INFO_DEF+129
#define PROF_LEARN_MINING       GOSSIP_ACTION_INFO_DEF+130
#define PROF_LEARN_SKINNING     GOSSIP_ACTION_INFO_DEF+131
#define PROF_LEARN_TAILORING    GOSSIP_ACTION_INFO_DEF+132
#define PROF_MENU_BACK          GOSSIP_ACTION_INFO_DEF+133
enum TalentTabNames
{
    WarriorProtection = 163,
    WarriorFury = 164,
    WarriorArms = 161,
    WarlockDemonology = 303,
    WarlockDestruction = 301,
    WarlockAffliction = 302,
    ShamanRestoration = 262,
    ShamanEnhancement = 263,
    ShamanElementalCombat = 261,
    RogueSubtlety = 183,
    RogueCombat = 181,
    RogueAssassination = 182,
    PriestShadow = 203,
    PriestHoly = 202,
    PriestDiscipline = 201,
    PaladinProtection = 383,
    PaladinHoly = 382,
    PaladinRetribution = 381,
    MageFrost = 61,
    MageFire = 41,
    MageArcane = 81,
    HunterSurvival = 362,
    HunterMarksmanship = 363,
    HunterBeastMastery = 361,
    DruidRestoration = 282,
    DruidFeralCombat = 281,
    DruidBalance = 283,
};


// ------------------------------------------------------------
// Helper: Klassen-Name fuer Anzeige (nicht DB-Key)
// ------------------------------------------------------------
static const char* GetClassDisplayName(Player* player)
{
    if (!player)
        return "Unknown";

    switch (TemplateNpc_GetPlayerClassId(player))
    {
        case CLASS_WARRIOR: return "Warrior";
        case CLASS_PALADIN: return "Paladin";
        case CLASS_HUNTER: return "Hunter";
        case CLASS_ROGUE: return "Rogue";
        case CLASS_PRIEST: return "Priest";
        case CLASS_SHAMAN: return "Shaman";
        case CLASS_MAGE: return "Mage";
        case CLASS_WARLOCK: return "Warlock";
        case CLASS_DRUID: return "Druid";
        default: break;
    }

    return "Unknown";
}


std::string GetPetFamily(uint32 Entry)
{
    switch (Entry)
    {
    case 1: return "Wolf"; break;
    case 2: return "Cat"; break;
    case 3: return "Spider"; break;
    case 4: return "Bear"; break;
    case 5: return "Boar"; break;
    case 6: return "Crocolisk"; break;
    case 7: return "Carrion Bird"; break;
    case 8: return "Crab"; break;
    case 9: return "Gorilla"; break;
    case 11: return "Raptor"; break;
    case 12: return "Tallstrider"; break;
    case 15: return "Felhunter"; break;
    case 16: return "Voidwalker"; break;
    case 17: return "Succubus"; break;
    case 19: return "Doomguard"; break;
    case 20: return "Scorpid"; break;
    case 21: return "Turtle"; break;
    case 23: return "Imp"; break;
    case 24: return "Bat"; break;
    case 25: return "Hyena"; break;
    case 26: return "Owl"; break;
    case 27: return "Wind Serpent"; break;
    case 28: return "Remote Control"; break;
    }
    return "";
}

std::string GetMySpellFamilyName(Player* player)
{
    switch (TemplateNpc_GetPlayerClassId(player))
    {
    case CLASS_PRIEST:
        return "SPELLFAMILY_PRIEST";
        break;
    case CLASS_PALADIN:
        return "SPELLFAMILY_PALADIN";
        break;
    case CLASS_WARRIOR:
        return "SPELLFAMILY_WARRIOR";
        break;
    case CLASS_MAGE:
        return "SPELLFAMILY_MAGE";
        break;
    case CLASS_WARLOCK:
        return "SPELLFAMILY_WARLOCK";
        break;
    case CLASS_SHAMAN:
        return "SPELLFAMILY_SHAMAN";
        break;
    case CLASS_DRUID:
        return "SPELLFAMILY_DRUID";
        break;
    case CLASS_HUNTER:
        return "SPELLFAMILY_HUNTER";
        break;
    case CLASS_ROGUE:
        return "SPELLFAMILY_ROGUE";
        break;
    default:
        break;
    }
    return ""; // Fix warning, this should never happen
}

UINT16 AverageItemLevel(Player* player)
{
    float ilevel = 0;
    uint32 counter = 0;

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (i == EQUIPMENT_SLOT_TABARD || i == EQUIPMENT_SLOT_BODY)
            continue;

        Item* equippedItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i);

        if (equippedItem)
        {
            uint32 itemId = equippedItem->GetEntry();
            ItemPrototype const* item_proto = sObjectMgr.GetItemPrototype(itemId);
            if (item_proto->ItemLevel)
            {
                ilevel = ilevel + item_proto->ItemLevel;
                ++counter;
            }
        }
    }

    ilevel = ilevel / counter; //calculate average itemlevel.
    double ilvl = floor(ilevel * 100.0 + .5) / 100.0;

    if (ilvl)
        return ilvl;
    else
        return 0;

    /* was for testing.

    std::ostringstream ss;
    ss << "Your average itemlevel is " << ilvl << ".";
    ChatHandler(player->GetSession()).PSendSysMessage(ss.str().c_str());
    player->GetSession()->SendAreaTriggerMessage(ss.str().c_str());
    */
}

std::string DefineMageSpec(Player* player)
{
    enum MageSpells
    {
        IGNITE_RANK1 = 11119,
        IGNITE_RANK2 = 11120,
        IGNITE_RANK3 = 12846,
        IGNITE_RANK4 = 12847,
        IGNITE_RANK5 = 12848,
        COMBUSTION = 11129,
        ICE_BARRIER_RANK1 = 11426,
        ICE_BARRIER_RANK2 = 13031,
        ICE_BARRIER_RANK3 = 13032,
        ICE_BARRIER_RANK4 = 13033,
        WINTERS_CHILL_RANK1 = 11180,
        WINTERS_CHILL_RANK2 = 28592,
        WINTERS_CHILL_RANK3 = 28593,
        WINTERS_CHILL_RANK4 = 28594,
        WINTERS_CHILL_RANK5 = 28595,
        ARCANE_POWER = 12042,
        PYROBLAST = 18809,
        COLD_SNAP = 12472,
        ICE_BLOCK = 11958,
        FROST_CHANNELING_RANK1 = 11160,
        FROST_CHANNELING_RANK2 = 12518,
        FROST_CHANNELING_RANK3 = 12519,
    };

    std::ostringstream GetSpells;

    if (player->HasSpell(ARCANE_POWER))
    {
        if (player->HasSpell(PYROBLAST))
            GetSpells << "(Arcane Power/Pyroblast) ";
        if (player->HasSpell(FROST_CHANNELING_RANK1) || player->HasSpell(FROST_CHANNELING_RANK2) || player->HasSpell(FROST_CHANNELING_RANK3))
            GetSpells << "(Arcane Power/Frost) ";
    };

    if (player->HasSpell(PYROBLAST) && !player->HasSpell(ARCANE_POWER))
    {
        if (player->HasSpell(ICE_BLOCK))
            GetSpells << "(Pyroblast/Ice Block) ";
    };

    if (player->HasSpell(IGNITE_RANK1) || player->HasSpell(IGNITE_RANK2) || player->HasSpell(IGNITE_RANK3) || player->HasSpell(IGNITE_RANK4) || player->HasSpell(IGNITE_RANK5))
    {
        if (player->HasSpell(COMBUSTION))
            GetSpells << "(Combustion/Ignite) ";
    };

    if (player->HasSpell(ICE_BARRIER_RANK4))
    {
        if (player->HasSpell(WINTERS_CHILL_RANK5))
            GetSpells << "(Ice Barrier/Winters Chill) ";
        if (player->HasSpell(COLD_SNAP))
            GetSpells << "(Ice Barrier/Cold Snap) ";
    };

    if (GetSpells)
        return GetSpells.str().c_str();
    else
        return "";
}

std::string DefineWarlockSpec(Player* player)
{
    enum WarlockSpells
    {
        SHADOW_MASTERY_RANK1 = 18271,
        SHADOW_MASTERY_RANK2 = 18272,
        SHADOW_MASTERY_RANK3 = 18273,
        SHADOW_MASTERY_RANK4 = 18274,
        SHADOW_MASTERY_RANK5 = 18275,
        RUIN = 17959,
        DEMONIC_SACRIFICE = 18788,
        CONFLAGRATE_RANK1 = 17962,
        CONFLAGRATE_RANK2 = 18930,
        CONFLAGRATE_RANK3 = 18931,
        CONFLAGRATE_RANK4 = 18932,
        SOUL_LINK = 19028,
    };

    std::ostringstream GetSpells;

    if (player->HasSpell(SOUL_LINK))
        GetSpells << "(Soul Link) ";

    if (player->HasSpell(RUIN))
    {
        if (player->HasSpell(SHADOW_MASTERY_RANK1) || player->HasSpell(SHADOW_MASTERY_RANK2) || player->HasSpell(SHADOW_MASTERY_RANK3) || player->HasSpell(SHADOW_MASTERY_RANK4) || player->HasSpell(SHADOW_MASTERY_RANK5))
            GetSpells << "(SM/Ruin) ";
        else if (player->HasSpell(DEMONIC_SACRIFICE))
            GetSpells << "(DS/Ruin) ";
        else if (player->HasSpell(CONFLAGRATE_RANK1) || player->HasSpell(CONFLAGRATE_RANK2) || player->HasSpell(CONFLAGRATE_RANK3) || player->HasSpell(CONFLAGRATE_RANK4))
            GetSpells << "(Ruin/Conflag) ";
    };

    if (GetSpells)
        return GetSpells.str().c_str();
    else
        return "";
}

std::string DefineHunterSpec(Player* player)
{
    enum HunterSpells
    {
        TRUESHOT_AURA_RANK1 = 19506,
        TRUESHOT_AURA_RANK2 = 20905,
        TRUESHOT_AURA_RANK3 = 20906,
        BESTIAL_WRATH = 19574,
        LIGHTNING_REFLEXES_RANK1 = 19168,
        LIGHTNING_REFLEXES_RANK2 = 19180,
        LIGHTNING_REFLEXES_RANK3 = 19181,
        LIGHTNING_REFLEXES_RANK4 = 24296,
        LIGHTNING_REFLEXES_RANK5 = 24297,
        AIMED_SHOT_RANK1 = 19434,
    };

    std::ostringstream GetSpells;

    if (player->HasSpell(LIGHTNING_REFLEXES_RANK1) || player->HasSpell(LIGHTNING_REFLEXES_RANK2) || player->HasSpell(LIGHTNING_REFLEXES_RANK3) || player->HasSpell(LIGHTNING_REFLEXES_RANK4) || player->HasSpell(LIGHTNING_REFLEXES_RANK5))
        GetSpells << "(Lightning Reflexes) ";
    else if (player->HasSpell(BESTIAL_WRATH))
        GetSpells << "(Bestial Wrath) ";
    else if (player->HasSpell(TRUESHOT_AURA_RANK1) || player->HasSpell(TRUESHOT_AURA_RANK2) || player->HasSpell(TRUESHOT_AURA_RANK3))
        GetSpells << "(Trueshot) ";

    if (GetSpells)
        return GetSpells.str().c_str();
    else
        return "";
}

std::string DefineRogueSpec(Player* player)
{
    enum HunterSpells
    {
        LETHALITY_RANK1 = 14128,
        LETHALITY_RANK2 = 14132,
        LETHALITY_RANK3 = 14135,
        LETHALITY_RANK4 = 14136,
        LETHALITY_RANK5 = 14137,
        IMPROVED_BACKSTAB_RANK1 = 13733,
        IMPROVED_BACKSTAB_RANK2 = 13865,
        IMPROVED_BACKSTAB_RANK3 = 13866,
        OPPORTUNITY_RANK1 = 14057,
        OPPORTUNITY_RANK2 = 14072,
        OPPORTUNITY_RANK3 = 14073,
        OPPORTUNITY_RANK4 = 14074,
        OPPORTUNITY_RANK5 = 14075,
        DAGGER_SPECIALIZATION_RANK1 = 13706,
        DAGGER_SPECIALIZATION_RANK2 = 13804,
        DAGGER_SPECIALIZATION_RANK3 = 13805,
        DAGGER_SPECIALIZATION_RANK4 = 13806,
        DAGGER_SPECIALIZATION_RANK5 = 13807,
        SWORD_SPECIALIZATION_RANK1 = 13960,
        SWORD_SPECIALIZATION_RANK2 = 13961,
        SWORD_SPECIALIZATION_RANK3 = 13962,
        SWORD_SPECIALIZATION_RANK4 = 13963,
        SWORD_SPECIALIZATION_RANK5 = 13964,
        SEAL_FATE_RANK1 = 14186,
        SEAL_FATE_RANK2 = 14190,
        SEAL_FATE_RANK3 = 14193,
        SEAL_FATE_RANK4 = 14194,
        SEAL_FATE_RANK5 = 14195,
        COLD_BLOOD = 14177,
        PREPARATION = 14185,
    };

    std::ostringstream GetSpells;

    if (player->HasSpell(DAGGER_SPECIALIZATION_RANK1) || player->HasSpell(DAGGER_SPECIALIZATION_RANK2) || player->HasSpell(DAGGER_SPECIALIZATION_RANK3) || player->HasSpell(DAGGER_SPECIALIZATION_RANK4) || player->HasSpell(DAGGER_SPECIALIZATION_RANK5))
        if (player->HasSpell(IMPROVED_BACKSTAB_RANK1) || player->HasSpell(IMPROVED_BACKSTAB_RANK2) || player->HasSpell(IMPROVED_BACKSTAB_RANK3) && player->HasSpell(OPPORTUNITY_RANK1) || player->HasSpell(OPPORTUNITY_RANK2) || player->HasSpell(OPPORTUNITY_RANK3) || player->HasSpell(OPPORTUNITY_RANK4) || player->HasSpell(OPPORTUNITY_RANK5) && player->HasSpell(LETHALITY_RANK1) || player->HasSpell(LETHALITY_RANK2) || player->HasSpell(LETHALITY_RANK3) || player->HasSpell(LETHALITY_RANK4) || player->HasSpell(LETHALITY_RANK5))
        {
            GetSpells << "(Dagger/Backstab) ";
        }
    if (player->HasSpell(SWORD_SPECIALIZATION_RANK1) || player->HasSpell(SWORD_SPECIALIZATION_RANK2) || player->HasSpell(SWORD_SPECIALIZATION_RANK3) || player->HasSpell(SWORD_SPECIALIZATION_RANK4) || player->HasSpell(SWORD_SPECIALIZATION_RANK5))
        GetSpells << "(Sword) ";
    if (player->HasSpell(SEAL_FATE_RANK1) || player->HasSpell(SEAL_FATE_RANK2) || player->HasSpell(SEAL_FATE_RANK3) || player->HasSpell(SEAL_FATE_RANK4) || player->HasSpell(SEAL_FATE_RANK5))
    {
        if ((player->HasSpell(IMPROVED_BACKSTAB_RANK1) || player->HasSpell(IMPROVED_BACKSTAB_RANK2) || player->HasSpell(IMPROVED_BACKSTAB_RANK3)) && (player->HasSpell(OPPORTUNITY_RANK1) || player->HasSpell(OPPORTUNITY_RANK2) || player->HasSpell(OPPORTUNITY_RANK3) || player->HasSpell(OPPORTUNITY_RANK4) || player->HasSpell(OPPORTUNITY_RANK5)) && (player->HasSpell(LETHALITY_RANK1) || player->HasSpell(LETHALITY_RANK2) || player->HasSpell(LETHALITY_RANK3) || player->HasSpell(LETHALITY_RANK4) || player->HasSpell(LETHALITY_RANK5)))
            GetSpells << "(Dagger/Seal Fate) ";
        else if (!player->HasSpell(IMPROVED_BACKSTAB_RANK1) && !player->HasSpell(IMPROVED_BACKSTAB_RANK2) && !player->HasSpell(IMPROVED_BACKSTAB_RANK3) && !player->HasSpell(OPPORTUNITY_RANK1) && !player->HasSpell(OPPORTUNITY_RANK2) && !player->HasSpell(OPPORTUNITY_RANK3) && !player->HasSpell(OPPORTUNITY_RANK4) && !player->HasSpell(OPPORTUNITY_RANK5))
            GetSpells << "(Sword/Seal Fate) ";
    }
    else if (player->HasSpell(COLD_BLOOD) && player->HasSpell(PREPARATION))
        GetSpells << "(Cold Blood/Preparation) ";

    if (GetSpells)
        return GetSpells.str().c_str();
    else
        return "";
}

std::string DefineMainHand(Player* player)
{
    Item* MainHandItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    std::ostringstream MainweaponSub;

    if (MainHandItem)
    {
        uint32 itemId = MainHandItem->GetEntry();
        ItemPrototype const* item_proto = sObjectMgr.GetItemPrototype(itemId);

        switch (item_proto->InventoryType)
        {
        case INVTYPE_2HWEAPON:
            if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2)
                MainweaponSub << ("2H Axe");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2)
                MainweaponSub << ("2H Mace");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2)
                MainweaponSub << ("2H Sword");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_POLEARM)
                MainweaponSub << ("Polearm");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_STAFF)
                MainweaponSub << ("Staff");
            break;
        case INVTYPE_WEAPON:
            if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE)
                MainweaponSub << ("Axe");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE)
                MainweaponSub << ("Mace");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD)
                MainweaponSub << ("Sword");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER)
                MainweaponSub << ("Dagger");
            break;
        }
    }
    if (MainweaponSub)
        return MainweaponSub.str().c_str();
    else
        return "";
}

std::string DefineOffHand(Player* player)
{
    Item* OffHandItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    std::ostringstream OffweaponSub;

    if (OffHandItem)
    {
        uint32 itemId = OffHandItem->GetEntry();
        ItemPrototype const* item_proto = sObjectMgr.GetItemPrototype(itemId);

        switch (item_proto->InventoryType)
        {
        case INVTYPE_SHIELD:
            OffweaponSub << ("Shield");
            break;
        case INVTYPE_WEAPON:
            if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_AXE)
                OffweaponSub << ("Axe");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_MACE)
                OffweaponSub << ("Mace");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD)
                OffweaponSub << ("Sword");
            else if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER)
                OffweaponSub << ("Dagger");
            break;
        }
    }
    if (OffweaponSub)
        return OffweaponSub.str().c_str();
    else
        return "";
}

std::string GetWeaponsString(Player* player)
{
    std::ostringstream GetWeapons;
    if (DefineMainHand(player) != "" && DefineOffHand(player) == "")
        GetWeapons << "(" << DefineMainHand(player) << ")";
    if (DefineMainHand(player) != "" && DefineOffHand(player) != "")
        GetWeapons << "(" << DefineMainHand(player) << "/" << DefineOffHand(player) << ")";

    if (GetWeapons)
        return GetWeapons.str().c_str();
    else
        return "";
}

std::string TemplateExportNameString(Player* player)
{
    uint32 curtalent_spent = 0;
    uint32 WarriorProtectionPoints = 0;
    uint32 WarriorFuryPoints = 0;
    uint32 WarriorArmsPoints = 0;
    uint32 WarlockDemonologyPoints = 0;
    uint32 WarlockDestructionPoints = 0;
    uint32 WarlockAfflictionPoints = 0;
    uint32 ShamanRestorationPoints = 0;
    uint32 ShamanEnhancementPoints = 0;
    uint32 ShamanElementalCombatPoints = 0;
    uint32 RogueSubtletyPoints = 0;
    uint32 RogueCombatPoints = 0;
    uint32 RogueAssassinationPoints = 0;
    uint32 PriestShadowPoints = 0;
    uint32 PriestHolyPoints = 0;
    uint32 PriestDisciplinePoints = 0;
    uint32 PaladinProtectionPoints = 0;
    uint32 PaladinHolyPoints = 0;
    uint32 PaladinRetributionPoints = 0;
    uint32 MageFrostPoints = 0;
    uint32 MageFirePoints = 0;
    uint32 MageArcanePoints = 0;
    uint32 HunterSurvivalPoints = 0;
    uint32 HunterMarksmanshipPoints = 0;
    uint32 HunterBeastMasteryPoints = 0;
    uint32 DruidRestorationPoints = 0;
    uint32 DruidFeralCombatPoints = 0;
    uint32 DruidBalancePoints = 0;

    for (uint32 i = 1; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const *talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo) continue;

        TalentTabEntry const *talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
        if (!talentTabInfo)
            continue;

        if (talentTabInfo->ClassMask != (TemplateNpc_GetPlayerClassId(player) ? (1u << (TemplateNpc_GetPlayerClassId(player)-1)) : 0u))
            continue;

        uint32 spentPoints = 0;
        for (int j = 0; j < MAX_TALENT_RANK; ++j)
        {
            if (talentInfo->RankID[j] != 0)
            {
                switch (talentInfo->TalentTab)
                {
                    //  WARRIOR

                case WarriorProtection:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarriorProtection)
                    {
                        spentPoints += j + 1;
                        WarriorProtectionPoints += spentPoints;
                    }
                    break;
                case WarriorFury:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarriorFury)
                    {
                        spentPoints += j + 1;
                        WarriorFuryPoints += spentPoints;
                    }
                    break;
                case WarriorArms:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarriorArms)
                    {
                        spentPoints += j + 1;
                        WarriorArmsPoints += spentPoints;
                    }
                    break;

                    //  WARLOCK

                case WarlockDemonology:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarlockDemonology)
                    {
                        spentPoints += j + 1;
                        WarlockDemonologyPoints += spentPoints;
                    }
                    break;
                case WarlockDestruction:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarlockDestruction)
                    {
                        spentPoints += j + 1;
                        WarlockDestructionPoints += spentPoints;
                    }
                    break;
                case WarlockAffliction:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarlockAffliction)
                    {
                        spentPoints += j + 1;
                        WarlockAfflictionPoints += spentPoints;
                    }
                    break;

                    //  SHAMAN

                case ShamanElementalCombat:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == ShamanElementalCombat)
                    {
                        spentPoints += j + 1;
                        ShamanElementalCombatPoints += spentPoints;
                    }
                    break;
                case ShamanEnhancement:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == ShamanEnhancement)
                    {
                        spentPoints += j + 1;
                        ShamanEnhancementPoints += spentPoints;
                    }
                    break;
                case ShamanRestoration:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == ShamanRestoration)
                    {
                        spentPoints += j + 1;
                        ShamanRestorationPoints += spentPoints;
                    }
                    break;

                    //  ROGUE

                case RogueAssassination:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == RogueAssassination)
                    {
                        spentPoints += j + 1;
                        RogueAssassinationPoints += spentPoints;
                    }
                    break;
                case RogueCombat:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == RogueCombat)
                    {
                        spentPoints += j + 1;
                        RogueCombatPoints += spentPoints;
                    }
                    break;
                case RogueSubtlety:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == RogueSubtlety)
                    {
                        spentPoints += j + 1;
                        RogueSubtletyPoints += spentPoints;
                    }
                    break;

                    //  PRIEST

                case PriestDiscipline:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PriestDiscipline)
                    {
                        spentPoints += j + 1;
                        PriestDisciplinePoints += spentPoints;
                    }
                    break;
                case PriestHoly:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PriestHoly)
                    {
                        spentPoints += j + 1;
                        PriestHolyPoints += spentPoints;
                    }
                    break;
                case PriestShadow:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PriestShadow)
                    {
                        spentPoints += j + 1;
                        PriestShadowPoints += spentPoints;
                    }
                    break;

                    //  PALADIN

                case PaladinHoly:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PaladinHoly)
                    {
                        spentPoints += j + 1;
                        PaladinHolyPoints += spentPoints;
                    }
                    break;
                case PaladinProtection:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PaladinProtection)
                    {
                        spentPoints += j + 1;
                        PaladinProtectionPoints += spentPoints;
                    }
                    break;
                case PaladinRetribution:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PaladinRetribution)
                    {
                        spentPoints += j + 1;
                        PaladinRetributionPoints += spentPoints;
                    }
                    break;

                    //  MAGE

                case MageArcane:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == MageArcane)
                    {
                        spentPoints += j + 1;
                        MageArcanePoints += spentPoints;
                    }
                    break;
                case MageFire:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == MageFire)
                    {
                        spentPoints += j + 1;
                        MageFirePoints += spentPoints;
                    }
                    break;
                case MageFrost:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == MageFrost)
                    {
                        spentPoints += j + 1;
                        MageFrostPoints += spentPoints;
                    }
                    break;

                    //  HUNTER

                case HunterBeastMastery:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == HunterBeastMastery)
                    {
                        spentPoints += j + 1;
                        HunterBeastMasteryPoints += spentPoints;
                    }
                    break;
                case HunterMarksmanship:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == HunterMarksmanship)
                    {
                        spentPoints += j + 1;
                        HunterMarksmanshipPoints += spentPoints;
                    }
                    break;
                case HunterSurvival:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == HunterSurvival)
                    {
                        spentPoints += j + 1;
                        HunterSurvivalPoints += spentPoints;
                    }
                    break;

                    //  DRUID

                case DruidBalance:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == DruidBalance)
                    {
                        spentPoints += j + 1;
                        DruidBalancePoints += spentPoints;
                    }
                    break;
                case DruidFeralCombat:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == DruidFeralCombat)
                    {
                        spentPoints += j + 1;
                        DruidFeralCombatPoints += spentPoints;
                    }
                    break;
                case DruidRestoration:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == DruidRestoration)
                    {
                        spentPoints += j + 1;
                        DruidRestorationPoints += spentPoints;
                    }
                    break;
                }
            }
        }
        /* for debugging
        std::ostringstream ss;
        ss << "talentpoints spent " << talentInfo->TalentTab << " / " << curtalent_spent << " / " << spentPoints << ".";
        ChatHandler(player->GetSession()).PSendSysMessage(ss.str().c_str());
        */
    }

    std::ostringstream PointsStream;
    DefineMainHand(player);
    //WARRIOR
    std::ostringstream GetWeapons;
    if (DefineMainHand(player) != "" && DefineOffHand(player) == "")
        GetWeapons << "(" << DefineMainHand(player) << ")";
    if (DefineMainHand(player) != "" && DefineOffHand(player) != "")
        GetWeapons << "(" << DefineMainHand(player) << "/" << DefineOffHand(player);

    if (WarriorProtectionPoints > WarriorFuryPoints && WarriorProtectionPoints > WarriorArmsPoints)
    {
        if (GetWeaponsString(player) != "")
            PointsStream << "Protection " << GetWeaponsString(player) << " " << WarriorArmsPoints << "/" << WarriorFuryPoints << "/" << WarriorProtectionPoints;
        else
            PointsStream << "Protection " << WarriorArmsPoints << "/" << WarriorFuryPoints << "/" << WarriorProtectionPoints;
    }
    else if (WarriorFuryPoints > WarriorProtectionPoints && WarriorFuryPoints > WarriorArmsPoints)
    {
        if (GetWeaponsString(player) != "")
            PointsStream << "Fury " << GetWeaponsString(player) << " " << WarriorArmsPoints << "/" << WarriorFuryPoints << "/" << WarriorProtectionPoints;
        else
            PointsStream << "Fury " << WarriorArmsPoints << "/" << WarriorFuryPoints << "/" << WarriorProtectionPoints;
    }
    else if (WarriorArmsPoints > WarriorFuryPoints && WarriorArmsPoints > WarriorProtectionPoints)
    {
        if (GetWeaponsString(player) != "")
            PointsStream << "Arms " << GetWeaponsString(player) << " " << WarriorArmsPoints << "/" << WarriorFuryPoints << "/" << WarriorProtectionPoints;
        else
            PointsStream << "Arms " << WarriorArmsPoints << "/" << WarriorFuryPoints << "/" << WarriorProtectionPoints;
    }

    //WARLOCK
    if (WarlockAfflictionPoints > WarlockDemonologyPoints && WarlockAfflictionPoints > WarlockDestructionPoints)
    {
        PointsStream << "Affliction " << DefineWarlockSpec(player) << WarlockAfflictionPoints << "/" << WarlockDemonologyPoints << "/" << WarlockDestructionPoints;
    }
    else if (WarlockDemonologyPoints > WarlockAfflictionPoints && WarlockDemonologyPoints > WarlockDestructionPoints)
    {
        PointsStream << "Demonology" << DefineWarlockSpec(player) << WarlockAfflictionPoints << "/" << WarlockDemonologyPoints << "/" << WarlockDestructionPoints;
    }
    else if (WarlockDestructionPoints > WarlockDemonologyPoints && WarlockDestructionPoints > WarlockAfflictionPoints)
    {
        PointsStream << "Destruction " << DefineWarlockSpec(player) << WarlockAfflictionPoints << "/" << WarlockDemonologyPoints << "/" << WarlockDestructionPoints;
    }

    //SHAMAN
    if (ShamanElementalCombatPoints > ShamanEnhancementPoints && ShamanElementalCombatPoints > ShamanRestorationPoints)
    {
        PointsStream << "Elemental " << ShamanElementalCombatPoints << "/" << ShamanEnhancementPoints << "/" << ShamanRestorationPoints;
    }
    else if (ShamanEnhancementPoints > ShamanElementalCombatPoints && ShamanEnhancementPoints > ShamanRestorationPoints)
    {
        PointsStream << "Enhancement " << ShamanElementalCombatPoints << "/" << ShamanEnhancementPoints << "/" << ShamanRestorationPoints;
    }
    else if (ShamanRestorationPoints > ShamanEnhancementPoints && ShamanRestorationPoints > ShamanElementalCombatPoints)
    {
        PointsStream << "Restoration " << ShamanElementalCombatPoints << "/" << ShamanEnhancementPoints << "/" << ShamanRestorationPoints;
    }

    //ROGUE
    if (RogueAssassinationPoints > RogueCombatPoints && RogueAssassinationPoints > RogueSubtletyPoints)
    {
        PointsStream << "Assassination " << DefineRogueSpec(player) << RogueAssassinationPoints << "/" << RogueCombatPoints << "/" << RogueSubtletyPoints;
    }
    else if (RogueCombatPoints > RogueAssassinationPoints && RogueCombatPoints > RogueSubtletyPoints)
    {
        PointsStream << "Combat " << DefineRogueSpec(player) << RogueAssassinationPoints << "/" << RogueCombatPoints << "/" << RogueSubtletyPoints;
    }
    else if (RogueSubtletyPoints > RogueCombatPoints && RogueSubtletyPoints > RogueAssassinationPoints)
    {
        PointsStream << "Subtlety " << DefineRogueSpec(player) << RogueAssassinationPoints << "/" << RogueCombatPoints << "/" << RogueSubtletyPoints;
    }

    //PRIEST
    if (PriestDisciplinePoints > PriestHolyPoints && PriestDisciplinePoints > PriestShadowPoints)
    {
        PointsStream << "Discipline " << PriestDisciplinePoints << "/" << PriestHolyPoints << "/" << PriestShadowPoints;
    }
    else if (PriestHolyPoints > PriestDisciplinePoints && PriestHolyPoints > PriestShadowPoints)
    {
        PointsStream << "Holy " << PriestDisciplinePoints << "/" << PriestHolyPoints << "/" << PriestShadowPoints;
    }
    else if (PriestShadowPoints > PriestDisciplinePoints && PriestShadowPoints > PriestHolyPoints)
    {
        PointsStream << "Shadow " << PriestDisciplinePoints << "/" << PriestHolyPoints << "/" << PriestShadowPoints;
    }

    //PALADIN
    if (PaladinHolyPoints > PaladinProtectionPoints && PaladinHolyPoints > PaladinRetributionPoints)
    {
        PointsStream << "Holy " << PaladinHolyPoints << "/" << PaladinProtectionPoints << "/" << PaladinRetributionPoints;
    }
    else if (PaladinProtectionPoints > PaladinHolyPoints && PaladinProtectionPoints > PaladinRetributionPoints)
    {
        PointsStream << "Protection " << PaladinHolyPoints << "/" << PaladinProtectionPoints << "/" << PaladinRetributionPoints;
    }
    else if (PaladinRetributionPoints > PaladinHolyPoints && PaladinRetributionPoints > PaladinProtectionPoints)
    {
        PointsStream << "Retribution " << PaladinHolyPoints << "/" << PaladinProtectionPoints << "/" << PaladinRetributionPoints;
    }

    //MAGE
    if (MageArcanePoints > MageFirePoints && MageArcanePoints > MageFrostPoints)
    {
        PointsStream << "Arcane " << DefineMageSpec(player) << MageArcanePoints << "/" << MageFirePoints << "/" << MageFrostPoints;
    }
    else if (MageFirePoints > MageArcanePoints && MageFirePoints > MageFrostPoints)
    {
        PointsStream << "Fire " << DefineMageSpec(player) << MageArcanePoints << "/" << MageFirePoints << "/" << MageFrostPoints;
    }
    else if (MageFrostPoints > MageArcanePoints && MageFrostPoints > MageFirePoints)
    {
        PointsStream << "Frost " << DefineMageSpec(player) << MageArcanePoints << "/" << MageFirePoints << "/" << MageFrostPoints;
    }

    //HUNTER
    if (HunterBeastMasteryPoints > HunterMarksmanshipPoints && HunterBeastMasteryPoints > HunterSurvivalPoints)
    {
        PointsStream << "Beast Mastery " << DefineHunterSpec(player) << HunterBeastMasteryPoints << "/" << HunterMarksmanshipPoints << "/" << HunterSurvivalPoints;
    }
    else if (HunterMarksmanshipPoints > HunterBeastMasteryPoints && HunterMarksmanshipPoints > HunterSurvivalPoints)
    {
        PointsStream << "Marksmanship " << DefineHunterSpec(player) << HunterBeastMasteryPoints << "/" << HunterMarksmanshipPoints << "/" << HunterSurvivalPoints;
    }
    else if (HunterSurvivalPoints > HunterBeastMasteryPoints && HunterSurvivalPoints > HunterMarksmanshipPoints)
    {
        PointsStream << "Survival " << DefineHunterSpec(player) << HunterBeastMasteryPoints << "/" << HunterMarksmanshipPoints << "/" << HunterSurvivalPoints;
    }

    //DRUID
    if (DruidBalancePoints > DruidFeralCombatPoints && DruidBalancePoints > DruidRestorationPoints)
    {
        PointsStream << "Balance " << DruidBalancePoints << "/" << DruidFeralCombatPoints << "/" << DruidRestorationPoints;
    }
    else if (DruidFeralCombatPoints > DruidBalancePoints && DruidFeralCombatPoints > DruidRestorationPoints)
    {
        PointsStream << "Feral Combat " << DruidBalancePoints << "/" << DruidFeralCombatPoints << "/" << DruidRestorationPoints;
    }
    else if (DruidRestorationPoints > DruidBalancePoints && DruidRestorationPoints > DruidFeralCombatPoints)
    {
        PointsStream << "Restoration " << DruidBalancePoints << "/" << DruidFeralCombatPoints << "/" << DruidRestorationPoints;
    }

    uint32 ilvl = AverageItemLevel(player);

        std::ostringstream exportStream;
        exportStream << (PointsStream.str().c_str()) << ", iLvL " << ilvl;
        return (exportStream.str().c_str());

}

std::string GetSpecNameByTalentPoints(Player* player)
{
    uint32 curtalent_spent = 0;
    uint32 WarriorProtectionPoints = 0;
    uint32 WarriorFuryPoints = 0;
    uint32 WarriorArmsPoints = 0;
    uint32 WarlockDemonologyPoints = 0;
    uint32 WarlockDestructionPoints = 0;
    uint32 WarlockAfflictionPoints = 0;
    uint32 ShamanRestorationPoints = 0;
    uint32 ShamanEnhancementPoints = 0;
    uint32 ShamanElementalCombatPoints = 0;
    uint32 RogueSubtletyPoints = 0;
    uint32 RogueCombatPoints = 0;
    uint32 RogueAssassinationPoints = 0;
    uint32 PriestShadowPoints = 0;
    uint32 PriestHolyPoints = 0;
    uint32 PriestDisciplinePoints = 0;
    uint32 PaladinProtectionPoints = 0;
    uint32 PaladinHolyPoints = 0;
    uint32 PaladinRetributionPoints = 0;
    uint32 MageFrostPoints = 0;
    uint32 MageFirePoints = 0;
    uint32 MageArcanePoints = 0;
    uint32 HunterSurvivalPoints = 0;
    uint32 HunterMarksmanshipPoints = 0;
    uint32 HunterBeastMasteryPoints = 0;
    uint32 DruidRestorationPoints = 0;
    uint32 DruidFeralCombatPoints = 0;
    uint32 DruidBalancePoints = 0;

    for (uint32 i = 1; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const *talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo) continue;

        TalentTabEntry const *talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
        if (!talentTabInfo)
            continue;

        if (talentTabInfo->ClassMask != (TemplateNpc_GetPlayerClassId(player) ? (1u << (TemplateNpc_GetPlayerClassId(player)-1)) : 0u))
            continue;

        uint32 spentPoints = 0;
        for (int j = 0; j < MAX_TALENT_RANK; ++j)
        {
            if (talentInfo->RankID[j] != 0)
            {
                switch (talentInfo->TalentTab)
                {
                    //  WARRIOR

                case WarriorProtection:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarriorProtection)
                    {
                        spentPoints += j + 1;
                        WarriorProtectionPoints += spentPoints;
                    }
                    break;
                case WarriorFury:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarriorFury)
                    {
                        spentPoints += j + 1;
                        WarriorFuryPoints += spentPoints;
                    }
                    break;
                case WarriorArms:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarriorArms)
                    {
                        spentPoints += j + 1;
                        WarriorArmsPoints += spentPoints;
                    }
                    break;

                    //  WARLOCK

                case WarlockDemonology:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarlockDemonology)
                    {
                        spentPoints += j + 1;
                        WarlockDemonologyPoints += spentPoints;
                    }
                    break;
                case WarlockDestruction:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarlockDestruction)
                    {
                        spentPoints += j + 1;
                        WarlockDestructionPoints += spentPoints;
                    }
                    break;
                case WarlockAffliction:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarlockAffliction)
                    {
                        spentPoints += j + 1;
                        WarlockAfflictionPoints += spentPoints;
                    }
                    break;

                    //  SHAMAN

                case ShamanElementalCombat:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == ShamanElementalCombat)
                    {
                        spentPoints += j + 1;
                        ShamanElementalCombatPoints += spentPoints;
                    }
                    break;
                case ShamanEnhancement:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == ShamanEnhancement)
                    {
                        spentPoints += j + 1;
                        ShamanEnhancementPoints += spentPoints;
                    }
                    break;
                case ShamanRestoration:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == ShamanRestoration)
                    {
                        spentPoints += j + 1;
                        ShamanRestorationPoints += spentPoints;
                    }
                    break;

                    //  ROGUE

                case RogueAssassination:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == RogueAssassination)
                    {
                        spentPoints += j + 1;
                        RogueAssassinationPoints += spentPoints;
                    }
                    break;
                case RogueCombat:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == RogueCombat)
                    {
                        spentPoints += j + 1;
                        RogueCombatPoints += spentPoints;
                    }
                    break;
                case RogueSubtlety:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == RogueSubtlety)
                    {
                        spentPoints += j + 1;
                        RogueSubtletyPoints += spentPoints;
                    }
                    break;

                    //  PRIEST

                case PriestDiscipline:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PriestDiscipline)
                    {
                        spentPoints += j + 1;
                        PriestDisciplinePoints += spentPoints;
                    }
                    break;
                case PriestHoly:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PriestHoly)
                    {
                        spentPoints += j + 1;
                        PriestHolyPoints += spentPoints;
                    }
                    break;
                case PriestShadow:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PriestShadow)
                    {
                        spentPoints += j + 1;
                        PriestShadowPoints += spentPoints;
                    }
                    break;

                    //  PALADIN

                case PaladinHoly:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PaladinHoly)
                    {
                        spentPoints += j + 1;
                        PaladinHolyPoints += spentPoints;
                    }
                    break;
                case PaladinProtection:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PaladinProtection)
                    {
                        spentPoints += j + 1;
                        PaladinProtectionPoints += spentPoints;
                    }
                    break;
                case PaladinRetribution:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PaladinRetribution)
                    {
                        spentPoints += j + 1;
                        PaladinRetributionPoints += spentPoints;
                    }
                    break;

                    //  MAGE

                case MageArcane:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == MageArcane)
                    {
                        spentPoints += j + 1;
                        MageArcanePoints += spentPoints;
                    }
                    break;
                case MageFire:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == MageFire)
                    {
                        spentPoints += j + 1;
                        MageFirePoints += spentPoints;
                    }
                    break;
                case MageFrost:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == MageFrost)
                    {
                        spentPoints += j + 1;
                        MageFrostPoints += spentPoints;
                    }
                    break;

                    //  HUNTER

                case HunterBeastMastery:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == HunterBeastMastery)
                    {
                        spentPoints += j + 1;
                        HunterBeastMasteryPoints += spentPoints;
                    }
                    break;
                case HunterMarksmanship:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == HunterMarksmanship)
                    {
                        spentPoints += j + 1;
                        HunterMarksmanshipPoints += spentPoints;
                    }
                    break;
                case HunterSurvival:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == HunterSurvival)
                    {
                        spentPoints += j + 1;
                        HunterSurvivalPoints += spentPoints;
                    }
                    break;

                    //  DRUID

                case DruidBalance:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == DruidBalance)
                    {
                        spentPoints += j + 1;
                        DruidBalancePoints += spentPoints;
                    }
                    break;
                case DruidFeralCombat:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == DruidFeralCombat)
                    {
                        spentPoints += j + 1;
                        DruidFeralCombatPoints += spentPoints;
                    }
                    break;
                case DruidRestoration:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == DruidRestoration)
                    {
                        spentPoints += j + 1;
                        DruidRestorationPoints += spentPoints;
                    }
                    break;
                }
            }
        }

        /* for debugging
        std::ostringstream ss;
        ss << "talentpoints spent " << talentInfo->TalentTab << " / " << curtalent_spent << " / " << spentPoints << ".";
        ChatHandler(player->GetSession()).PSendSysMessage(ss.str().c_str());
        */
    }

    std::string Specification = "Undefined";

    //WARRIOR
    if (WarriorProtectionPoints > WarriorFuryPoints && WarriorProtectionPoints > WarriorArmsPoints)
    {
        Specification = "Protection";
    }
    else if (WarriorFuryPoints > WarriorProtectionPoints && WarriorFuryPoints > WarriorArmsPoints)
    {
        Specification = "Fury";
    }
    else if (WarriorArmsPoints > WarriorFuryPoints && WarriorArmsPoints > WarriorProtectionPoints)
    {
        Specification = "Arms";
    }

    //WARLOCK
    if (WarlockAfflictionPoints > WarlockDemonologyPoints && WarlockAfflictionPoints > WarlockDestructionPoints)
    {
        Specification = "Affliction";
    }
    else if (WarlockDemonologyPoints > WarlockAfflictionPoints && WarlockDemonologyPoints > WarlockDestructionPoints)
    {
        Specification = "Demonology";
    }
    else if (WarlockDestructionPoints > WarlockDemonologyPoints && WarlockDestructionPoints > WarlockAfflictionPoints)
    {
        Specification = "Destruction";
    }

    //SHAMAN
    if (ShamanElementalCombatPoints > ShamanEnhancementPoints && ShamanElementalCombatPoints > ShamanRestorationPoints)
    {
        Specification = "Elemental";
    }
    else if (ShamanEnhancementPoints > ShamanElementalCombatPoints && ShamanEnhancementPoints > ShamanRestorationPoints)
    {
        Specification = "Enhancement";
    }
    else if (ShamanRestorationPoints > ShamanEnhancementPoints && ShamanRestorationPoints > ShamanElementalCombatPoints)
    {
        Specification = "Restoration";
    }

    //ROGUE
    if (RogueAssassinationPoints > RogueCombatPoints && RogueAssassinationPoints > RogueSubtletyPoints)
    {
        Specification = "Assassination";
    }
    else if (RogueCombatPoints > RogueAssassinationPoints && RogueCombatPoints > RogueSubtletyPoints)
    {
        Specification = "Combat";
    }
    else if (RogueSubtletyPoints > RogueCombatPoints && RogueSubtletyPoints > RogueAssassinationPoints)
    {
        Specification = "Subtlety";
    }

    //PRIEST
    if (PriestDisciplinePoints > PriestHolyPoints && PriestDisciplinePoints > PriestShadowPoints)
    {
        Specification = "Discipline";
    }
    else if (PriestHolyPoints > PriestDisciplinePoints && PriestHolyPoints > PriestShadowPoints)
    {
        Specification = "Holy";
    }
    else if (PriestShadowPoints > PriestDisciplinePoints && PriestShadowPoints > PriestHolyPoints)
    {
        Specification = "Shadow";
    }

    //PALADIN
    if (PaladinHolyPoints > PaladinProtectionPoints && PaladinHolyPoints > PaladinRetributionPoints)
    {
        Specification = "Holy";
    }
    else if (PaladinProtectionPoints > PaladinHolyPoints && PaladinProtectionPoints > PaladinRetributionPoints)
    {
        Specification = "Protection";
    }
    else if (PaladinRetributionPoints > PaladinHolyPoints && PaladinRetributionPoints > PaladinProtectionPoints)
    {
        Specification = "Retribution";
    }

    //MAGE
    if (MageArcanePoints > MageFirePoints && MageArcanePoints > MageFrostPoints)
    {
        Specification = "Arcane";
    }
    else if (MageFirePoints > MageArcanePoints && MageFirePoints > MageFrostPoints)
    {
        Specification = "Fire";
    }
    else if (MageFrostPoints > MageArcanePoints && MageFrostPoints > MageFirePoints)
    {
        Specification = "Frost";
    }

    //HUNTER
    if (HunterBeastMasteryPoints > HunterMarksmanshipPoints && HunterBeastMasteryPoints > HunterSurvivalPoints)
    {
        Specification = "Beast Mastery";
    }
    else if (HunterMarksmanshipPoints > HunterBeastMasteryPoints && HunterMarksmanshipPoints > HunterSurvivalPoints)
    {
        Specification = "Marksmanship";
    }
    else if (HunterSurvivalPoints > HunterBeastMasteryPoints && HunterSurvivalPoints > HunterMarksmanshipPoints)
    {
        Specification = "Survival";
    }

    //DRUID
    if (DruidBalancePoints > DruidFeralCombatPoints && DruidBalancePoints > DruidRestorationPoints)
    {
        Specification = "Balance";
    }
    else if (DruidFeralCombatPoints > DruidBalancePoints && DruidFeralCombatPoints > DruidRestorationPoints)
    {
        Specification = "Feral Combat";
    }
    else if (DruidRestorationPoints > DruidBalancePoints && DruidRestorationPoints > DruidFeralCombatPoints)
    {
        Specification = "Restoration";
    }

    return Specification;
}

uint16 GetSpecIDByTalentPoints(Player* player)
{
    uint32 curtalent_spent = 0;
    uint32 WarriorProtectionPoints = 0;
    uint32 WarriorFuryPoints = 0;
    uint32 WarriorArmsPoints = 0;
    uint32 WarlockDemonologyPoints = 0;
    uint32 WarlockDestructionPoints = 0;
    uint32 WarlockAfflictionPoints = 0;
    uint32 ShamanRestorationPoints = 0;
    uint32 ShamanEnhancementPoints = 0;
    uint32 ShamanElementalCombatPoints = 0;
    uint32 RogueSubtletyPoints = 0;
    uint32 RogueCombatPoints = 0;
    uint32 RogueAssassinationPoints = 0;
    uint32 PriestShadowPoints = 0;
    uint32 PriestHolyPoints = 0;
    uint32 PriestDisciplinePoints = 0;
    uint32 PaladinProtectionPoints = 0;
    uint32 PaladinHolyPoints = 0;
    uint32 PaladinRetributionPoints = 0;
    uint32 MageFrostPoints = 0;
    uint32 MageFirePoints = 0;
    uint32 MageArcanePoints = 0;
    uint32 HunterSurvivalPoints = 0;
    uint32 HunterMarksmanshipPoints = 0;
    uint32 HunterBeastMasteryPoints = 0;
    uint32 DruidRestorationPoints = 0;
    uint32 DruidFeralCombatPoints = 0;
    uint32 DruidBalancePoints = 0;

    for (uint32 i = 1; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const *talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo) continue;

        TalentTabEntry const *talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
        if (!talentTabInfo)
            continue;

        if (talentTabInfo->ClassMask != (TemplateNpc_GetPlayerClassId(player) ? (1u << (TemplateNpc_GetPlayerClassId(player)-1)) : 0u))
            continue;

        uint32 spentPoints = 0;
        for (int j = 0; j < MAX_TALENT_RANK; ++j)
        {
            if (talentInfo->RankID[j] != 0)
            {
                switch (talentInfo->TalentTab)
                {
                    //  WARRIOR

                case WarriorProtection:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarriorProtection)
                    {
                        spentPoints += j + 1;
                        WarriorProtectionPoints += spentPoints;
                    }
                    break;
                case WarriorFury:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarriorFury)
                    {
                        spentPoints += j + 1;
                        WarriorFuryPoints += spentPoints;
                    }
                    break;
                case WarriorArms:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarriorArms)
                    {
                        spentPoints += j + 1;
                        WarriorArmsPoints += spentPoints;
                    }
                    break;

                    //  WARLOCK

                case WarlockDemonology:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarlockDemonology)
                    {
                        spentPoints += j + 1;
                        WarlockDemonologyPoints += spentPoints;
                    }
                    break;
                case WarlockDestruction:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarlockDestruction)
                    {
                        spentPoints += j + 1;
                        WarlockDestructionPoints += spentPoints;
                    }
                    break;
                case WarlockAffliction:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == WarlockAffliction)
                    {
                        spentPoints += j + 1;
                        WarlockAfflictionPoints += spentPoints;
                    }
                    break;

                    //  SHAMAN

                case ShamanElementalCombat:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == ShamanElementalCombat)
                    {
                        spentPoints += j + 1;
                        ShamanElementalCombatPoints += spentPoints;
                    }
                    break;
                case ShamanEnhancement:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == ShamanEnhancement)
                    {
                        spentPoints += j + 1;
                        ShamanEnhancementPoints += spentPoints;
                    }
                    break;
                case ShamanRestoration:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == ShamanRestoration)
                    {
                        spentPoints += j + 1;
                        ShamanRestorationPoints += spentPoints;
                    }
                    break;

                    //  ROGUE

                case RogueAssassination:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == RogueAssassination)
                    {
                        spentPoints += j + 1;
                        RogueAssassinationPoints += spentPoints;
                    }
                    break;
                case RogueCombat:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == RogueCombat)
                    {
                        spentPoints += j + 1;
                        RogueCombatPoints += spentPoints;
                    }
                    break;
                case RogueSubtlety:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == RogueSubtlety)
                    {
                        spentPoints += j + 1;
                        RogueSubtletyPoints += spentPoints;
                    }
                    break;

                    //  PRIEST

                case PriestDiscipline:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PriestDiscipline)
                    {
                        spentPoints += j + 1;
                        PriestDisciplinePoints += spentPoints;
                    }
                    break;
                case PriestHoly:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PriestHoly)
                    {
                        spentPoints += j + 1;
                        PriestHolyPoints += spentPoints;
                    }
                    break;
                case PriestShadow:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PriestShadow)
                    {
                        spentPoints += j + 1;
                        PriestShadowPoints += spentPoints;
                    }
                    break;

                    //  PALADIN

                case PaladinHoly:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PaladinHoly)
                    {
                        spentPoints += j + 1;
                        PaladinHolyPoints += spentPoints;
                    }
                    break;
                case PaladinProtection:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PaladinProtection)
                    {
                        spentPoints += j + 1;
                        PaladinProtectionPoints += spentPoints;
                    }
                    break;
                case PaladinRetribution:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == PaladinRetribution)
                    {
                        spentPoints += j + 1;
                        PaladinRetributionPoints += spentPoints;
                    }
                    break;

                    //  MAGE

                case MageArcane:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == MageArcane)
                    {
                        spentPoints += j + 1;
                        MageArcanePoints += spentPoints;
                    }
                    break;
                case MageFire:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == MageFire)
                    {
                        spentPoints += j + 1;
                        MageFirePoints += spentPoints;
                    }
                    break;
                case MageFrost:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == MageFrost)
                    {
                        spentPoints += j + 1;
                        MageFrostPoints += spentPoints;
                    }
                    break;

                    //  HUNTER

                case HunterBeastMastery:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == HunterBeastMastery)
                    {
                        spentPoints += j + 1;
                        HunterBeastMasteryPoints += spentPoints;
                    }
                    break;
                case HunterMarksmanship:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == HunterMarksmanship)
                    {
                        spentPoints += j + 1;
                        HunterMarksmanshipPoints += spentPoints;
                    }
                    break;
                case HunterSurvival:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == HunterSurvival)
                    {
                        spentPoints += j + 1;
                        HunterSurvivalPoints += spentPoints;
                    }
                    break;

                    //  DRUID

                case DruidBalance:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == DruidBalance)
                    {
                        spentPoints += j + 1;
                        DruidBalancePoints += spentPoints;
                    }
                    break;
                case DruidFeralCombat:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == DruidFeralCombat)
                    {
                        spentPoints += j + 1;
                        DruidFeralCombatPoints += spentPoints;
                    }
                    break;
                case DruidRestoration:
                    if (player->HasSpell(talentInfo->RankID[j]) && talentInfo->TalentTab == DruidRestoration)
                    {
                        spentPoints += j + 1;
                        DruidRestorationPoints += spentPoints;
                    }
                    break;
                }
            }
        }
        /* for debugging
        std::ostringstream ss;
        ss << "talentpoints spent " << talentInfo->TalentTab << " / " << curtalent_spent << " / " << spentPoints << ".";
        ChatHandler(player->GetSession()).PSendSysMessage(ss.str().c_str());
        */
    }

    //WARRIOR
    if (WarriorProtectionPoints > WarriorFuryPoints && WarriorProtectionPoints > WarriorArmsPoints)
    {
        return 163;
    }
    else if (WarriorFuryPoints > WarriorProtectionPoints && WarriorFuryPoints > WarriorArmsPoints)
    {
        return 164;
    }
    else if (WarriorArmsPoints > WarriorFuryPoints && WarriorArmsPoints > WarriorProtectionPoints)
    {
        return 161;
    }

    //WARLOCK
    if (WarlockAfflictionPoints > WarlockDemonologyPoints && WarlockAfflictionPoints > WarlockDestructionPoints)
    {
        return 302;
    }
    else if (WarlockDemonologyPoints > WarlockAfflictionPoints && WarlockDemonologyPoints > WarlockDestructionPoints)
    {
        return 303;
    }
    else if (WarlockDestructionPoints > WarlockDemonologyPoints && WarlockDestructionPoints > WarlockAfflictionPoints)
    {
        return 301;
    }

    //SHAMAN
    if (ShamanElementalCombatPoints > ShamanEnhancementPoints && ShamanElementalCombatPoints > ShamanRestorationPoints)
    {
        return 261;
    }
    else if (ShamanEnhancementPoints > ShamanElementalCombatPoints && ShamanEnhancementPoints > ShamanRestorationPoints)
    {
        return 263;
    }
    else if (ShamanRestorationPoints > ShamanEnhancementPoints && ShamanRestorationPoints > ShamanElementalCombatPoints)
    {
        return 262;
    }

    //ROGUE
    if (RogueAssassinationPoints > RogueCombatPoints && RogueAssassinationPoints > RogueSubtletyPoints)
    {
        return 182;
    }
    else if (RogueCombatPoints > RogueAssassinationPoints && RogueCombatPoints > RogueSubtletyPoints)
    {
        return 181;
    }
    else if (RogueSubtletyPoints > RogueCombatPoints && RogueSubtletyPoints > RogueAssassinationPoints)
    {
        return 183;
    }

    //PRIEST
    if (PriestDisciplinePoints > PriestHolyPoints && PriestDisciplinePoints > PriestShadowPoints)
    {
        return 201;
    }
    else if (PriestHolyPoints > PriestDisciplinePoints && PriestHolyPoints > PriestShadowPoints)
    {
        return 202;
    }
    else if (PriestShadowPoints > PriestDisciplinePoints && PriestShadowPoints > PriestHolyPoints)
    {
        return 203;
    }

    //PALADIN
    if (PaladinHolyPoints > PaladinProtectionPoints && PaladinHolyPoints > PaladinRetributionPoints)
    {
        return 382;
    }
    else if (PaladinProtectionPoints > PaladinHolyPoints && PaladinProtectionPoints > PaladinRetributionPoints)
    {
        return 383;
    }
    else if (PaladinRetributionPoints > PaladinHolyPoints && PaladinRetributionPoints > PaladinProtectionPoints)
    {
        return 381;
    }

    //MAGE
    if (MageArcanePoints > MageFirePoints && MageArcanePoints > MageFrostPoints)
    {
        return 81;
    }
    else if (MageFirePoints > MageArcanePoints && MageFirePoints > MageFrostPoints)
    {
        return 41;
    }
    else if (MageFrostPoints > MageArcanePoints && MageFrostPoints > MageFirePoints)
    {
        return 61;
    }

    //HUNTER
    if (HunterBeastMasteryPoints > HunterMarksmanshipPoints && HunterBeastMasteryPoints > HunterSurvivalPoints)
    {
        return 361;
    }
    else if (HunterMarksmanshipPoints > HunterBeastMasteryPoints && HunterMarksmanshipPoints > HunterSurvivalPoints)
    {
        return 363;
    }
    else if (HunterSurvivalPoints > HunterBeastMasteryPoints && HunterSurvivalPoints > HunterMarksmanshipPoints)
    {
        return 362;
    }

    //DRUID
    if (DruidBalancePoints > DruidFeralCombatPoints && DruidBalancePoints > DruidRestorationPoints)
    {
        return 283;
    }
    else if (DruidFeralCombatPoints > DruidBalancePoints && DruidFeralCombatPoints > DruidRestorationPoints)
    {
        return 281;
    }
    else if (DruidRestorationPoints > DruidBalancePoints && DruidRestorationPoints > DruidFeralCombatPoints)
    {
        return 282;
    }
    
    return 0;
}

enum QuestSpellsProficiencies
{
    HUNTER_DISMISS_PET = 2641,
    HUNTER_CALL_PET = 883,
    HUNTER_TAME_BEAST = 1515,
    HUNTER_BEAST_TRAINING = 5149,
    HUNTER_FEED_PET = 6991,
    HUNTER_REVIVE_PET = 982,
    HUNTER_TRANQUILIZING_SHOT = 19801,

    ROGUE_POISONS = 2842,
    ROGUE_DEADLY_POISON_V = 25347,

    WARRIOR_DEFENSIVE_STANCE = 71,
    WARRIOR_SUNDER_ARMOR = 7386,
    WARRIOR_TAUNT = 355,
    WARRIOR_BERSERKER_STANCE = 2458,
    WARRIOR_INTERCEPT = 20252,

    WARLOCK_SUMMON_VOIDWALKER = 697,
    WARLOCK_SUMMON_SUCCUBUS = 712,
    WARLOCK_SUMMON_IMP = 688,
    WARLOCK_SUMMON_FELHUNTER = 691,
    WARLOCK_INFERNO = 1122,
    WARLOCK_SUMMON_FELSTEED = 5784,
    WARLOCK_SUMMON_DREADSTEED = 23161,

    MAGE_POLYMORPH_PIG = 28272,
    MAGE_POLYMORPH_TURTLE = 28271,
    MAGE_POLYMORPH_COW = 28270,
    MAGE_ARCANE_BRILLIANCE = 23028,

    SHAMAN_SEARING_TOTEM = 3599,
    SHAMAN_HEALING_STREAM_TOTEM = 5394,
    SHAMAN_STONESKIN_TOTEM = 8071,

    PRIEST_DEVOURING_PLAGUE = 2944,
    PRIEST_HEX_OF_WEAKNESS = 9035,
    PRIEST_TOUCH_OF_WEAKNESS = 2652,
    PRIEST_STARSHARDS = 10797,
    PRIEST_ELUNES_GRACE = 2651,
    PRIEST_SHADOWGUARD = 18137,
    PRIEST_FEAR_WARD = 6346,
    PRIEST_FEEDBACK = 13896,
    PRIEST_DESPERATE_PRAYER = 13908,
    PRIEST_PRAYER_OF_SHADOW_PROTECTION = 27683,
    PRIEST_PRAYER_OF_FORTITUDE = 21564,

    DRUID_CURE_POISON = 8946,
    DRUID_AQUATIC_FORM = 1066,
    DRUID_BEAR_FORM = 5487,
    DRUID_GROWL = 6795,
    DRUID_MAUL = 6807,
};

void ApplyBonus(Player* player, Item* item, EnchantmentSlot slot, uint32 bonusEntry, uint32 duration, uint32 charges)
{
    if (!item)
        return;

    if (!bonusEntry || bonusEntry == 0)
        return;

    player->ApplyEnchantment(item, slot, false);
    item->SetEnchantment(slot, bonusEntry, duration, charges);
    player->ApplyEnchantment(item, slot, true);
}

void LearnQuestSpells(Player* player)
{
    QuestSpellsProficiencies questspells[] = {
        HUNTER_DISMISS_PET, HUNTER_CALL_PET, HUNTER_TAME_BEAST, HUNTER_BEAST_TRAINING, HUNTER_FEED_PET, HUNTER_REVIVE_PET, HUNTER_TRANQUILIZING_SHOT,
        ROGUE_POISONS, ROGUE_DEADLY_POISON_V,
        WARRIOR_DEFENSIVE_STANCE, WARRIOR_SUNDER_ARMOR, WARRIOR_TAUNT, WARRIOR_BERSERKER_STANCE, WARRIOR_INTERCEPT,
        WARLOCK_SUMMON_VOIDWALKER, WARLOCK_SUMMON_SUCCUBUS, WARLOCK_SUMMON_IMP, WARLOCK_SUMMON_FELHUNTER, WARLOCK_INFERNO, WARLOCK_SUMMON_FELSTEED, WARLOCK_SUMMON_DREADSTEED,
        MAGE_POLYMORPH_PIG, MAGE_POLYMORPH_TURTLE, MAGE_POLYMORPH_COW, MAGE_ARCANE_BRILLIANCE,
        SHAMAN_SEARING_TOTEM, SHAMAN_HEALING_STREAM_TOTEM, SHAMAN_STONESKIN_TOTEM,
        PRIEST_DEVOURING_PLAGUE, PRIEST_HEX_OF_WEAKNESS, PRIEST_TOUCH_OF_WEAKNESS, PRIEST_STARSHARDS, PRIEST_ELUNES_GRACE, PRIEST_SHADOWGUARD, PRIEST_FEAR_WARD, PRIEST_FEEDBACK, PRIEST_DESPERATE_PRAYER, PRIEST_PRAYER_OF_SHADOW_PROTECTION, PRIEST_PRAYER_OF_FORTITUDE,
        DRUID_CURE_POISON, DRUID_AQUATIC_FORM, DRUID_BEAR_FORM, DRUID_GROWL, DRUID_MAUL,
    };

    uint32 size = 44;

    for (uint32 i = 0; i < size; ++i) {

        if (!player->IsSpellFitByClassAndRace(questspells[i]))
            continue;

        if (player->HasSpell(questspells[i]))
            continue;

        player->LearnSpell(questspells[i], false);
    }
}

void ApplyBuffAura_OLD(Unit* unit, uint32 spellId)
{
    if (!spellId)
        return;

    SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
    if (!spellInfo)
        return;

    enum Talents
    {
        IMPROVED_BATTLE_SHOUT_RANK1 = 12318,
        IMPROVED_BATTLE_SHOUT_RANK2 = 12857,
        IMPROVED_BATTLE_SHOUT_RANK3 = 12858,
        IMPROVED_BATTLE_SHOUT_RANK4 = 12860,
        IMPROVED_BATTLE_SHOUT_RANK5 = 12861,
        RESTORATIVE_TOTEMS_RANK1 = 16187,
        RESTORATIVE_TOTEMS_RANK2 = 16205,
        RESTORATIVE_TOTEMS_RANK3 = 16206,
        RESTORATIVE_TOTEMS_RANK4 = 16207,
        RESTORATIVE_TOTEMS_RANK5 = 16208,
        ENHANCING_TOTEMS_RANK1 = 16259,
        ENHANCING_TOTEMS_RANK2 = 16295,
        IMPROVED_WEAPON_TOTEMS_RANK1 = 29192,
        IMPROVED_WEAPON_TOTEMS_RANK2 = 29193,
        IMPROVED_BLESSING_OF_MIGHT_RANK1 = 20042,
        IMPROVED_BLESSING_OF_MIGHT_RANK2 = 20045,
        IMPROVED_BLESSING_OF_MIGHT_RANK3 = 20046,
        IMPROVED_BLESSING_OF_MIGHT_RANK4 = 20047,
        IMPROVED_BLESSING_OF_MIGHT_RANK5 = 20048,
        IMPROVED_BLESSING_OF_WISDOM_RANK1 = 20244,
        IMPROVED_BLESSING_OF_WISDOM_RANK2 = 20245,
        IMPROVED_MARK_OF_THE_WILD_RANK1 = 17050,
        IMPROVED_MARK_OF_THE_WILD_RANK2 = 17051,
        IMPROVED_MARK_OF_THE_WILD_RANK3 = 17053,
        IMPROVED_MARK_OF_THE_WILD_RANK4 = 17054,
        IMPROVED_MARK_OF_THE_WILD_RANK5 = 17055,
        IMPROVED_DEVOTION_AURA_RANK1 = 20138,
        IMPROVED_DEVOTION_AURA_RANK2 = 20139,
        IMPROVED_DEVOTION_AURA_RANK3 = 20140,
        IMPROVED_DEVOTION_AURA_RANK4 = 20141,
        IMPROVED_DEVOTION_AURA_RANK5 = 20142,
    };

    enum Talents talentpells[] = {
        IMPROVED_BATTLE_SHOUT_RANK1,
        IMPROVED_BATTLE_SHOUT_RANK2,
        IMPROVED_BATTLE_SHOUT_RANK3,
        IMPROVED_BATTLE_SHOUT_RANK4,
        IMPROVED_BATTLE_SHOUT_RANK5,
        RESTORATIVE_TOTEMS_RANK1,
        RESTORATIVE_TOTEMS_RANK2,
        RESTORATIVE_TOTEMS_RANK3,
        RESTORATIVE_TOTEMS_RANK4,
        RESTORATIVE_TOTEMS_RANK5,
        ENHANCING_TOTEMS_RANK1,
        ENHANCING_TOTEMS_RANK2,
        IMPROVED_BLESSING_OF_MIGHT_RANK1,
        IMPROVED_BLESSING_OF_MIGHT_RANK2,
        IMPROVED_BLESSING_OF_MIGHT_RANK3,
        IMPROVED_BLESSING_OF_MIGHT_RANK4,
        IMPROVED_BLESSING_OF_MIGHT_RANK5,
        IMPROVED_BLESSING_OF_WISDOM_RANK1,
        IMPROVED_BLESSING_OF_WISDOM_RANK2,
        IMPROVED_MARK_OF_THE_WILD_RANK1,
        IMPROVED_MARK_OF_THE_WILD_RANK2,
        IMPROVED_MARK_OF_THE_WILD_RANK3,
        IMPROVED_MARK_OF_THE_WILD_RANK4,
        IMPROVED_MARK_OF_THE_WILD_RANK5,
        IMPROVED_DEVOTION_AURA_RANK1,
        IMPROVED_DEVOTION_AURA_RANK2,
        IMPROVED_DEVOTION_AURA_RANK3,
        IMPROVED_DEVOTION_AURA_RANK4,
        IMPROVED_DEVOTION_AURA_RANK5,
    };

    uint32 TalentRankBefore = 0;

    //Save talentrank if we already have it.
    for (uint32 i = 0; i < 29; ++i) {

        if (!unit->IsPlayer())
            continue;

        if (!unit->ToPlayer()->IsSpellFitByClassAndRace(talentpells[i]))
            continue;

        if (!unit->ToPlayer()->HasActiveSpell(talentpells[i]))
            continue;

        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(talentpells[i]);
        if (!spellInfo)
            continue;

        TalentRankBefore = talentpells[i];
    }

    unit->RemoveAurasDueToSpell(spellId);

    //learn all high ranks for buffing.
    for (uint32 i = 0; i < 29; ++i) {

        if (!unit->IsPlayer())
            continue;

        if (!unit->ToPlayer()->HasSpell(talentpells[i]))
            unit->ToPlayer()->LearnSpellHighRank(talentpells[i]);
    }

    //buff.
    if (SpellAuraHolder* pHolder = unit->AddAura(spellId))
    {
        pHolder->SetAuraMaxDuration(7200000);
        pHolder->SetAuraDuration(7200000);
        pHolder->UpdateAuraDuration();
        pHolder->SetPassive(false);
        pHolder->SetPermanent(false);
        if (spellId == 25898)
            pHolder->SetCasterGuid(unit->GetGUID() + 1);
        if (spellId == 25916)
            pHolder->SetCasterGuid(unit->GetGUID() + 2);
        if (spellId == 25918)
            pHolder->SetCasterGuid(unit->GetGUID() + 3);
    }

    //remove all talents again.
    for (uint32 i = 0; i < 29; ++i) {

        if (!unit->IsPlayer())
            continue;

        if (unit->ToPlayer()->HasSpell(talentpells[i]))
            unit->ToPlayer()->RemoveSpell(talentpells[i], false, false);
    }

    //learn old talent again.
    if (TalentRankBefore > 0 && unit->IsPlayer())
        unit->ToPlayer()->LearnSpell(TalentRankBefore, false);

    /*
        std::ostringstream ss;
        ss << pwho->GetName();
        ss << spellId;
        pwho->MonsterYell(ss.str().c_str());
        */
}

void ApplyBuffAura(Unit* unit, uint32 spellId)
{
    if (!spellId)
        return;

    SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
    if (!spellInfo)
        return;

    enum Talents
    {
        IMPROVED_BATTLE_SHOUT_RANK1 = 12318,
        IMPROVED_BATTLE_SHOUT_RANK2 = 12857,
        IMPROVED_BATTLE_SHOUT_RANK3 = 12858,
        IMPROVED_BATTLE_SHOUT_RANK4 = 12860,
        IMPROVED_BATTLE_SHOUT_RANK5 = 12861,
        RESTORATIVE_TOTEMS_RANK1 = 16187,
        RESTORATIVE_TOTEMS_RANK2 = 16205,
        RESTORATIVE_TOTEMS_RANK3 = 16206,
        RESTORATIVE_TOTEMS_RANK4 = 16207,
        RESTORATIVE_TOTEMS_RANK5 = 16208,
        ENHANCING_TOTEMS_RANK1 = 16259,
        ENHANCING_TOTEMS_RANK2 = 16295,
        IMPROVED_WEAPON_TOTEMS_RANK1 = 29192,
        IMPROVED_WEAPON_TOTEMS_RANK2 = 29193,
        IMPROVED_BLESSING_OF_MIGHT_RANK1 = 20042,
        IMPROVED_BLESSING_OF_MIGHT_RANK2 = 20045,
        IMPROVED_BLESSING_OF_MIGHT_RANK3 = 20046,
        IMPROVED_BLESSING_OF_MIGHT_RANK4 = 20047,
        IMPROVED_BLESSING_OF_MIGHT_RANK5 = 20048,
        IMPROVED_BLESSING_OF_WISDOM_RANK1 = 20244,
        IMPROVED_BLESSING_OF_WISDOM_RANK2 = 20245,
        IMPROVED_MARK_OF_THE_WILD_RANK1 = 17050,
        IMPROVED_MARK_OF_THE_WILD_RANK2 = 17051,
        IMPROVED_MARK_OF_THE_WILD_RANK3 = 17053,
        IMPROVED_MARK_OF_THE_WILD_RANK4 = 17054,
        IMPROVED_MARK_OF_THE_WILD_RANK5 = 17055,
        IMPROVED_DEVOTION_AURA_RANK1 = 20138,
        IMPROVED_DEVOTION_AURA_RANK2 = 20139,
        IMPROVED_DEVOTION_AURA_RANK3 = 20140,
        IMPROVED_DEVOTION_AURA_RANK4 = 20141,
        IMPROVED_DEVOTION_AURA_RANK5 = 20142,
    };

    enum Talents talentpells[] = {
        IMPROVED_BATTLE_SHOUT_RANK5,
        RESTORATIVE_TOTEMS_RANK5,
        ENHANCING_TOTEMS_RANK2,
        IMPROVED_BLESSING_OF_MIGHT_RANK5,
        IMPROVED_BLESSING_OF_WISDOM_RANK2,
        IMPROVED_MARK_OF_THE_WILD_RANK5,
        IMPROVED_DEVOTION_AURA_RANK5,
    };

    unit->RemoveAurasDueToSpell(spellId);

    //learn all high ranks for buffing.
    for (uint32 i = 0; i < 7; ++i) {

        if (!unit)
            return;

        if (!unit->IsPlayer())
            continue;

        if (!unit->ToPlayer()->HasSpell(talentpells[i]) && !unit->ToPlayer()->HasAura(talentpells[i]))
            unit->ToPlayer()->AddAura(talentpells[i]);
    }

    //buff.
    if (SpellAuraHolder* pHolder = unit->AddAura(spellId))
    {
        pHolder->SetAuraMaxDuration(7200000);
        pHolder->SetAuraDuration(7200000);
        pHolder->UpdateAuraDuration();
        pHolder->SetPassive(false);
        pHolder->SetPermanent(false);
        if (spellId == 25898)
            pHolder->SetCasterGuid(unit->GetGUID() + 1);
        if (spellId == 25916)
            pHolder->SetCasterGuid(unit->GetGUID() + 2);
        if (spellId == 25918)
            pHolder->SetCasterGuid(unit->GetGUID() + 3);
    }

    //remove all talents again.
    for (uint32 i = 0; i < 7; ++i) {

        if (!unit)
            return;

        if (!unit->IsPlayer())
            continue;

        if (!unit->ToPlayer()->HasSpell(talentpells[i]) && unit->ToPlayer()->HasAura(talentpells[i]))
            unit->ToPlayer()->RemoveAurasDueToSpell(talentpells[i]);
    }
}

void FullBuffed(Player* player)
{
    enum WorldBuffs
    {
        ECHOES_OF_LORDAERON_ALLIANCE = 1386, //Increases melee, ranged and spell damage dealt to Undead by 5%.
        ECHOES_OF_LORDAERON_HORDE = 29520, //Increases melee, ranged and spell damage dealt to Undead by 5%.
        WARCHIEFS_BLESSING = 16609, //Increases hitpoints by 300. 15% haste to melee attacks. 10 mana regen every 5 seconds.
        RALLYING_CRY_OF_THE_DRAGONSLAYER = 22888, //Increases critical chance of spells by 10%, melee and ranged by 5% and grants 140 attack power. 120 minute duration.
        SPIRIT_OF_ZANDALAR = 24425, //Increases movement speed by 10 % and all stats by 15 % for 2 hours.
        SONGFLOWER_SERENADE = 15366, //Increases chance for a melee, ranged, or spell critical by 5% and all attributes by 15 for 1 hr.
        SLIPKIKS_SAVVY_DIRE_MAUL = 22820, //Chance for a critical hit with a spell increased by 3%.
        FENGUS_FEROCITY_DIRE_MAUL = 22817, //Attack power increased by 200.
        MOLDARS_MOXIE_DIRE_MAUL = 22818, //Overall Stamina increased by 15%.
        SAYGES_DARK_FORTUNE_OF_DAMAGE = 23768, //Increases damage by 10%.
        SAYGES_DARK_FORTUNE_OF_STRENGTH = 23735, //Increases Strength by 10%.
        SAYGES_DARK_FORTUNE_OF_AGILITY = 23736, //Increases Agility by 10%.
        SAYGES_DARK_FORTUNE_OF_SPIRIT = 23738, //Increases damage by 10%.
        SAYGES_DARK_FORTUNE_OF_STAMINA = 23737, //Increases stamina by 10%.
        SAYGES_DARK_FORTUNE_OF_INTELLIGENCE = 23766, //Increases intellect by 10%.
        TRACES_OF_SILITHYST = 29534, //Melee, ranged and spell damage dealt increased by 5%.
        SOUL_REVIVAL = 28681, //During the invasion you need to kill regular undead to turn the Necrotic Crystal into a Damaged Necrotic Crystal. This takes roughly 200 kills of regular undead spawns to do. When the crystal is destroyed a localised buff is cast on all players within 65 yards of the crystal. This Heals you to full, Restores your mana to full, and gives you this + 10 % damage buff for 30 minutes.
    };

    enum ClassBuffs
    {
        ARCANE_BRILLIANCE = 23028, //Infuses the target's party with brilliance, increasing their Intellect by 31 for 1 hr.
        PRAYER_OF_FORTITUDE = 21564, //Power infuses the target's party, increasing their Stamina by 54 for 1 hr.
        PRAYER_OF_SPIRIT = 27681, //Power infuses the target's party, increasing their Spirit by 40 for 1 hr.
        MOONKIN_AURA = 24907, //Increases spell critical chance by 3%.
        TRUESHOT_AURA = 20906, //Increases the attack power of party members within 45 yards by 100. Lasts 30 min.
        LEADER_OF_THE_PACK = 24932, //While in Cat, Bear or Dire Bear Form, the Leader of the Pack increases ranged and melee critical chance of all party members within 45 yards by 3%.
        GREATER_BLESSING_OF_KINGS = 25898, //increasing total stats by 10% for 15 min.
        GREATER_BLESSING_OF_MIGHT = 25916, //increasing melee attack power by 185 for 15 min
        GREATER_BLESSING_OF_WISDOM = 25918, //Gives all members of the raid or group that share the same class with the target the Greater Blessing of Kings, increasing total stats by 10% for 15 min. Players may only have one Blessing on them per Paladin at any one time.
        MARK_OF_THE_WILD = 9885, //Increases the friendly target's armor by 285, all attributes by 12 and all resistances by 20 for 30 min.
        BATTLE_SHOUT = 25289, //The warrior shouts, increasing the melee attack power of all party members within 20 yards by 232. Lasts 2 min.
        GRACE_OF_AIR = 25360,
        STRENGTH_OF_EARTH = 25362,
        MANA_SPRING = 10494,
        BLOOD_PACT = 11767,
        DEVOTION_AURA = 10293, //Gives 735 additional armor to party members within 30 yards. Players may only have one Aura on them per Paladin at any one time.
    };

    enum WeapenBuffs
    {
        CONSECRATED_WEAPON = 2684, //While applied to target weapon it increases attack power against undead by 100. Lasts for 1 hour.
        ELEMENTAL_SHARPENING_STONE = 2506, //Increase critical chance on a melee weapon by 2% for 30 minutes.
        IRON_COUNTERWEIGHT = 34, //Attaches a counterweight to a two-handed sword, mace, axe or polearm making it 3% faster.
        DENSE_WEIGHTSTONE = 1703, //Increase the damage of a blunt weapon by 8 for 30 minutes.
        BRILLIANT_MANA_OIL = 2629, //While applied to target weapon it restores 12 mana to the caster every 5 seconds and increases the effect of healing spells by up to 25. Lasts for 30 minutes.
        BRILLIANT_WIZARD_OIL = 2628, //While applied to target weapon it restores 12 mana to the caster every 5 seconds and increases the effect of healing spells by up to 25. Lasts for 30 minutes.
    };

    enum Flask
    {
        HOLY_MIGHTSTONE = 24833, //Shatters the Holy Mightstone, granting 300 Attack Power and increasing Holy damage from spells and effects by up to 400 when fighting undead. Lasts 10 min.
        CEREBRAL_CORTEX_COMPOUND = 10690, //Increases Intellect by 25 when consumed.
        HEADMASTERS_CHARGE = 18264, //Gives 20 additional intellect to party members within 30 yards.
        BLESSING_OF_BLACKFATHOM = 8733, //5 Intellect, 5 Spirit, 15 Frost Damage.
        GROUND_SCORPOK_ASSAY = 10669, //Increases Agility by 25 when consumed. Effect lasts for 60 minutes.
        FURY_OF_THE_BOGLING = 5665, //Increases physical damage by 1 for 10 min.
        MANA_RUBY = 10058, //Restores 1000 to 1201 mana. (cooldown 2 min)
        GREATER_DREAMLESS_SLEEP_POTION = 24360, //Puts the imbiber in a dreamless sleep for 12 sec. During that time the imbiber heals 2100 health and 2100 mana. (cooldown 2 min)
        MAJOR_TROLLS_BLOOD_POTION = 24361, //Regenerate 20 health every 5 sec for 1 hr.
        FLASK_OF_CHROMATIC_RESISTANCE = 17629, //Increases your resistance to all schools of magic by 25 for 2 hr. You can only have the effect of one flask at a time. This effect persists through death and stacks with all other resistance spells and items.
        FLASK_OF_PETRIFICATION = 17624, //You turn to stone, protecting you from all physical attacks and spells for 1 min, but during that time you cannot attack, move or cast spells. You can only have the effect of one flask at a time.
        FLASK_OF_THE_TITANS = 17626, //Increases the player's maximum health by 1200 for 2 hr. You can only have the effect of one flask at a time. This effect persists through death.
        FLASK_OF_DISTILLED_WISDOM = 17627, //Increases the player's maximum mana by 2000 for 2 hr. You can only have the effect of one flask at a time. This effect persists through death.
        FLASK_OF_SUPREME_POWER = 17628, //Increases damage done by magical spells and effects by up to 150 for 2 hr. You can only have the effect of one flask at a time. This effect persists through death.
        MAJOR_MANA_POTION = 17531, //Restores 1350 to 2251 mana. (cooldown 2 min)
        GREATER_SHADOW_PROTECTION_POTION = 17548, //Absorbs 1950 to 3251 shadow damage. Lasts 1 hr. (cooldown 2 min)
        GREATER_FIRE_PROTECTION_POTION = 17543, //Absorbs 1950 to 3251 fire damage. Lasts 1 hr. (cooldown 2 min)
        GREATER_ARCANE_PROTECTION_POTION = 17549, //Absorbs 1950 to 3251 arcane damage. Lasts 1 hr. (cooldown 2 min)
        GREATER_FROST_PROTECTION_POTION = 17544, //Absorbs 1950 to 3251 frost damage. Lasts 1 hr. (cooldown 2 min)
        POTION_DE_PROTECTION_CONTRE_LE_SACR_SUPRIEURE = 17545,
        GREATER_ARCANE_ELIXIR = 17539, //Increases spell damage by up to 35 for 1 hr.
        MANA_CITRINE = 10057, //Restores 775 to 926 mana. (cooldown 2 min)
        GREATER_NATURE_PROTECTION_POTION = 17546, //Absorbs 1950 to 3251 nature damage. Lasts 1 hr. (cooldown 2 min)
        LIVING_ACTION_POTION = 24364, //Makes you immune to Stun and Movement Impairing effects for the next 5 sec. Also removes existing Stun and Movement Impairing effects. (cooldown 2 min)
        PURIFICATION_POTION = 17550, //Attempts to remove one Curse, one Disease and one Poison from the Imbiber. (cooldown 2 min)
        GREATER_STONESHIELD_POTION = 17540, //Increases armor by 2000 for 2 min. (cooldown 2 min)
        ELIXIR_OF_THE_MONGOOSE = 17538, //Increases Agility by 25 and chance to get a critical hit by 2% for 1 hr.
        MIGHTY_RAGE_POTION = 17528, //Increases Rage by 45 to 346 and increases Strength by 60 for 20 sec. (cooldown 2 min)
        RAW_WHITESCALE_SALMON = 1129, //Restores 1392 health over 30 sec. Must remain seated while eating.
        ELIXIR_OF_BRUTE_FORCE = 17537, //Increases Strength and Stamina by 18 for 1 hr.
        LIMITED_INVULNERABILITY_POTION = 3169, //Imbiber is immune to physical attacks for the next 6 sec. (cooldown 2 min)
        ELIXIR_OF_THE_SAGES = 17535, //Increases Intellect and Spirit by 18 for 1 hr.
        ELIXIR_OF_SUPERIOR_DEFENSE = 11348, //Increases armor by 450 for 1 hr.
        ELIXIR_OF_SHADOW_POWER = 11474, //Increases spell shadow damage by up to 40 for 30 min.
        MAGEBLOOD_POTION = 24363, //Regenerate 12 mana per 5 sec for 1 hr. (cooldown 10 sec)
        ELIXIR_OF_DEMONSLAYING = 11406, //Increases attack power by 265 against demons. Lasts 5 min. 
        ELIXIR_OF_GREATER_FIREPOWER = 26276, //Increases spell fire damage by up to 40 for 30 min.
        ELIXIR_OF_DETECT_DEMON = 11407, //Shows the location of all nearby demons on the minimap for 1 hr.
        ELIXIR_OF_GREATER_AGILITY = 11334, //Increases Agility by 25 for 1 hr.
        ELIXIR_OF_GIANTS = 11405, //Increases your Strength by 25 for 1 hr.
        ARCANE_ELIXIR = 11390, //Increases spell damage by up to 20 for 30 min.
        ELIXIR_OF_GREATER_INTELLECT = 11396, //Increases Intellect by 25 for 1 hr.
        ELIXIR_OF_DETECT_UNDEAD = 11389, //Shows the location of all nearby undead on the minimap for 1 hr.
        NOGGENFOGGER_ELIXIR = 16589, //Drink Me.
        POACHED_SUNSCALE_SALMON = 18232, //Restores 874.8 health over 27 sec. Must remain seated while eating. Also restores 6 health every 5 seconds for 10 min.
        ELIXIR_OF_GREATER_WATER_BREATHING = 22807, //Allows the Imbiber to breathe water for 1 hr.
        MAGIC_RESISTANCE_POTION = 11364, //Increases your resistance to all schools of magic by 50 for 3 min. (cooldown 2 min)
        CATSEYE_ELIXIR = 12608, //Slightly increases your stealth detection for 10 min.
        ELIXIR_OF_GREATER_DEFENSE = 11349, //Increases armor by 250 for 1 hr.
        ELIXIR_OF_DETECT_LESSER_INVISIBILITY = 6512, //Grants detect lesser invisibility for 10 min.
        FROST_PROTECTION_POTION = 7239, //Absorbs 1350 to 2251 frost damage. Lasts 1 hr. (cooldown 2 min)
        NATURE_PROTECTION_POTION = 7254, //Absorbs 1350 to 2251 nature damage. Lasts 1 hr. (cooldown 2 min)
        ELIXIR_OF_FROST_POWER = 21920, //Increases spell frost damage by up to 15 for 30 min.
        ELIXIR_OF_AGILITY = 11328, //Increases Agility by 15 for 1 hr.
        MIGHTY_TROLLS_BLOOD_POTION = 3223, //Regenerate 12 health every 5 sec for 1 hr.
        ELIXIR_OF_FORTITUDE = 3593, //Increases the player's maximum health by 120 for 1 hr.
        FIRE_PROTECTION_POTION = 7233, //Absorbs 975 to 1626 fire damage. Lasts 1 hr. (cooldown 2 min)
        STRONG_TROLLS_BLOOD_POTION = 3222, //Regenerate 6 health every 5 sec for 1 hr.
        HOLY_PROTECTION_POTION = 7245, //Absorbs 300 to 501 holy damage. Lasts 1 hr. (cooldown 2 min)
        BLOODKELP_ELIXIR_OF_DODGING = 27653, //Increases chance to dodge by 3% for 30 min.
        BLOODKELP_ELIXIR_OF_RESISTANCE = 27652, //Increases all magical resistances by 15 for 30 min.
        RUMSEY_RUM_BLACK_LABEL = 25804, //Increases Stamina by 15 for 15 min and gets you drunk to boot!
        ELIXIR_OF_WATER_WALKING = 8827, //Lets you walk on water for 30 min.
        NIGHTFIN_SOUP = 18194, //Restores 874.8 health over 27 sec. Must remain seated while eating. Also restores 162 Mana every 5 seconds for 10 min.
        WINTERFALL_FIREWATER = 17038, //Increases your melee attack power by 35 and size for 20 min.
        JUJU_MIGHT = 16329, //Increases attack power by 40 for 10 min.
        JUJU_POWER = 16323, //Increases the target's Strength by 30 for 30 min.
        SMOKED_DESERT_DUMPLINGS = 24799, //Restores 2148 health over 30 sec. Must remain seated while eating. If you spend at least 10 seconds eating you will become well fed and gain 20 Strength for 15 min.
        BLESSED_SUNFRUIT = 18125, //Increases Strength by 10 for 10 min.
        GRILLED_SQUID = 18192, //Increases Agility by 10 for 10 min.
        BOBBING_APPLE = 24870, //Restores 2% of your health per second for 24 sec. Must remain seated while eating. If you spend at least 10 seconds eating you will become well fed and gain Stamina and Spirit for 15 min.
        ROIDS = 10667, //Increases Strength by 25 when consumed. Effect lasts for 60 minutes.
        RUNN_TUM_TUBER_SURPRISE = 22730, //Increases Intellect by 10 for 10 min.
    };

    enum Crystals
    {
        CRYSTAL_WARD = 15233, //Increases the target's Armor by 200 for 30 min.
        CRYSTAL_FORCE = 15231, //Increases the target's Spirit by 30 for 30 min.
        CRYSTAL_SPIRE = 15279, //A crystal shield surrounds the friendly target, doing 12 damage to anyone who hits him. Lasts 10 min.
    };

    player->RemoveAllAurasOnDeath();
    player->RemoveAurasDueToSpell(15007);

    std::string ClassSpec = GetSpecNameByTalentPoints(player);

    if (player->GetTeam() == HORDE)
        ApplyBuffAura(player, ECHOES_OF_LORDAERON_HORDE);
    if (player->GetTeam() == ALLIANCE)
        ApplyBuffAura(player, ECHOES_OF_LORDAERON_ALLIANCE);

    switch (TemplateNpc_GetPlayerClassId(player))
    {
    case CLASS_WARRIOR:
        if (player->GetTeam() == HORDE)
        {
            ApplyBuffAura(player, WARCHIEFS_BLESSING);
            ApplyBuffAura(player, GRACE_OF_AIR);
            ApplyBuffAura(player, STRENGTH_OF_EARTH);
        }
        ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
        ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
        ApplyBuffAura(player, SONGFLOWER_SERENADE);
        ApplyBuffAura(player, FENGUS_FEROCITY_DIRE_MAUL);
        ApplyBuffAura(player, TRACES_OF_SILITHYST);
        ApplyBuffAura(player, SOUL_REVIVAL);
        ApplyBuffAura(player, LEADER_OF_THE_PACK);
        ApplyBuffAura(player, MARK_OF_THE_WILD);
        ApplyBuffAura(player, TRUESHOT_AURA);
        if (player->GetTeam() == ALLIANCE)
        {
            ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
            ApplyBuffAura(player, GREATER_BLESSING_OF_MIGHT);
        }
        ApplyBuffAura(player, BATTLE_SHOUT);
        break;
    case CLASS_PRIEST:
        if (player->GetTeam() == HORDE)
        {
            ApplyBuffAura(player, WARCHIEFS_BLESSING);
            ApplyBuffAura(player, MANA_SPRING);
        }
        ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
        ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
        ApplyBuffAura(player, SONGFLOWER_SERENADE);
        ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
        ApplyBuffAura(player, TRACES_OF_SILITHYST);
        ApplyBuffAura(player, SOUL_REVIVAL);
        ApplyBuffAura(player, PRAYER_OF_SPIRIT);
        ApplyBuffAura(player, MOONKIN_AURA);
        ApplyBuffAura(player, ARCANE_BRILLIANCE);
        ApplyBuffAura(player, MARK_OF_THE_WILD);
        if (player->GetTeam() == ALLIANCE)
        {
            ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
            ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
        }
        break;
    case CLASS_PALADIN:
        if (ClassSpec == "Protection")
        {
            if (player->GetTeam() == HORDE)
                ApplyBuffAura(player, WARCHIEFS_BLESSING);
            ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(player, SONGFLOWER_SERENADE);
            ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
            ApplyBuffAura(player, FENGUS_FEROCITY_DIRE_MAUL);
            ApplyBuffAura(player, TRACES_OF_SILITHYST);
            ApplyBuffAura(player, SOUL_REVIVAL);
            ApplyBuffAura(player, LEADER_OF_THE_PACK);
            ApplyBuffAura(player, PRAYER_OF_SPIRIT);
            ApplyBuffAura(player, MOONKIN_AURA);
            ApplyBuffAura(player, ARCANE_BRILLIANCE);
            ApplyBuffAura(player, MARK_OF_THE_WILD);
            ApplyBuffAura(player, TRUESHOT_AURA);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(player, GREATER_BLESSING_OF_MIGHT);
                ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
            }
            ApplyBuffAura(player, BATTLE_SHOUT);
        }
        if (ClassSpec == "Retribution")
        {
            if (player->GetTeam() == HORDE)
                ApplyBuffAura(player, WARCHIEFS_BLESSING);
            ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(player, SONGFLOWER_SERENADE);
            ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
            ApplyBuffAura(player, FENGUS_FEROCITY_DIRE_MAUL);
            ApplyBuffAura(player, TRACES_OF_SILITHYST);
            ApplyBuffAura(player, SOUL_REVIVAL);
            ApplyBuffAura(player, LEADER_OF_THE_PACK);
            ApplyBuffAura(player, PRAYER_OF_SPIRIT);
            ApplyBuffAura(player, MOONKIN_AURA);
            ApplyBuffAura(player, ARCANE_BRILLIANCE);
            ApplyBuffAura(player, MARK_OF_THE_WILD);
            ApplyBuffAura(player, TRUESHOT_AURA);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(player, GREATER_BLESSING_OF_MIGHT);
                ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
            }
            ApplyBuffAura(player, BATTLE_SHOUT);
        }
        if (ClassSpec == "Holy")
        {
            if (player->GetTeam() == HORDE)
                ApplyBuffAura(player, WARCHIEFS_BLESSING);
            ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(player, SONGFLOWER_SERENADE);
            ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
            ApplyBuffAura(player, TRACES_OF_SILITHYST);
            ApplyBuffAura(player, SOUL_REVIVAL);
            ApplyBuffAura(player, PRAYER_OF_SPIRIT);
            ApplyBuffAura(player, MOONKIN_AURA);
            ApplyBuffAura(player, ARCANE_BRILLIANCE);
            ApplyBuffAura(player, MARK_OF_THE_WILD);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
            }
        }
        break;
    case CLASS_ROGUE:
        if (player->GetTeam() == HORDE)
        {
            ApplyBuffAura(player, WARCHIEFS_BLESSING);
            ApplyBuffAura(player, GRACE_OF_AIR);
            ApplyBuffAura(player, STRENGTH_OF_EARTH);
        }
        ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
        ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
        ApplyBuffAura(player, SONGFLOWER_SERENADE);
        ApplyBuffAura(player, FENGUS_FEROCITY_DIRE_MAUL);
        ApplyBuffAura(player, TRACES_OF_SILITHYST);
        ApplyBuffAura(player, SOUL_REVIVAL);
        ApplyBuffAura(player, LEADER_OF_THE_PACK);
        ApplyBuffAura(player, MARK_OF_THE_WILD);
        ApplyBuffAura(player, TRUESHOT_AURA);
        if (player->GetTeam() == ALLIANCE)
        {
            ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
            ApplyBuffAura(player, GREATER_BLESSING_OF_MIGHT);
        }
        ApplyBuffAura(player, BATTLE_SHOUT);
        break;
    case CLASS_MAGE:
        if (player->GetTeam() == HORDE)
        {
            ApplyBuffAura(player, WARCHIEFS_BLESSING);
            ApplyBuffAura(player, MANA_SPRING);
        }
        ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
        ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
        ApplyBuffAura(player, SONGFLOWER_SERENADE);
        ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
        ApplyBuffAura(player, TRACES_OF_SILITHYST);
        ApplyBuffAura(player, SOUL_REVIVAL);
        ApplyBuffAura(player, PRAYER_OF_SPIRIT);
        ApplyBuffAura(player, MOONKIN_AURA);
        ApplyBuffAura(player, ARCANE_BRILLIANCE);
        ApplyBuffAura(player, MARK_OF_THE_WILD);
        if (player->GetTeam() == ALLIANCE)
        {
            ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
            ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
        }
        break;
    case CLASS_SHAMAN:
        if (ClassSpec == "Elemental")
        {
            if (player->GetTeam() == HORDE)
            {
                ApplyBuffAura(player, WARCHIEFS_BLESSING);
                ApplyBuffAura(player, MANA_SPRING);
            }
            ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(player, SONGFLOWER_SERENADE);
            ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
            ApplyBuffAura(player, TRACES_OF_SILITHYST);
            ApplyBuffAura(player, SOUL_REVIVAL);
            ApplyBuffAura(player, PRAYER_OF_SPIRIT);
            ApplyBuffAura(player, MOONKIN_AURA);
            ApplyBuffAura(player, ARCANE_BRILLIANCE);
            ApplyBuffAura(player, MARK_OF_THE_WILD);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
            }
        }
        if (ClassSpec == "Enhancement")
        {
            if (player->GetTeam() == HORDE)
            {
                ApplyBuffAura(player, WARCHIEFS_BLESSING);
                ApplyBuffAura(player, MANA_SPRING);
                ApplyBuffAura(player, STRENGTH_OF_EARTH);
                ApplyBuffAura(player, GRACE_OF_AIR);
            }
            ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(player, SONGFLOWER_SERENADE);
            ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
            ApplyBuffAura(player, FENGUS_FEROCITY_DIRE_MAUL);
            ApplyBuffAura(player, TRACES_OF_SILITHYST);
            ApplyBuffAura(player, SOUL_REVIVAL);
            ApplyBuffAura(player, LEADER_OF_THE_PACK);
            //ApplyBuffAura(player, PRAYER_OF_SPIRIT);
            ApplyBuffAura(player, MOONKIN_AURA);
            //ApplyBuffAura(player, ARCANE_BRILLIANCE);
            //ApplyBuffAura(player, MARK_OF_THE_WILD);
            ApplyBuffAura(player, TRUESHOT_AURA);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(player, GREATER_BLESSING_OF_MIGHT);
                ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
            }
            ApplyBuffAura(player, BATTLE_SHOUT);
        }
        if (ClassSpec == "Restoration")
        {
            if (player->GetTeam() == HORDE)
            {
                ApplyBuffAura(player, WARCHIEFS_BLESSING);
                ApplyBuffAura(player, MANA_SPRING);
            }
            ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(player, SONGFLOWER_SERENADE);
            ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
            ApplyBuffAura(player, TRACES_OF_SILITHYST);
            ApplyBuffAura(player, SOUL_REVIVAL);
            ApplyBuffAura(player, PRAYER_OF_SPIRIT);
            ApplyBuffAura(player, MOONKIN_AURA);
            ApplyBuffAura(player, ARCANE_BRILLIANCE);
            ApplyBuffAura(player, MARK_OF_THE_WILD);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
            }
        }
        break;
    case CLASS_HUNTER:
        if (player->GetTeam() == HORDE)
        {
            ApplyBuffAura(player, WARCHIEFS_BLESSING);
            ApplyBuffAura(player, MANA_SPRING);
            ApplyBuffAura(player, STRENGTH_OF_EARTH);
            ApplyBuffAura(player, GRACE_OF_AIR);
        }
        ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
        ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
        ApplyBuffAura(player, SONGFLOWER_SERENADE);
        ApplyBuffAura(player, FENGUS_FEROCITY_DIRE_MAUL);
        ApplyBuffAura(player, TRACES_OF_SILITHYST);
        ApplyBuffAura(player, SOUL_REVIVAL);
        ApplyBuffAura(player, LEADER_OF_THE_PACK);
        ApplyBuffAura(player, MARK_OF_THE_WILD);
        ApplyBuffAura(player, TRUESHOT_AURA);
        ApplyBuffAura(player, PRAYER_OF_SPIRIT);
        ApplyBuffAura(player, ARCANE_BRILLIANCE);
        ApplyBuffAura(player, BATTLE_SHOUT);

        if (player->GetTeam() == ALLIANCE)
        {
            ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
            ApplyBuffAura(player, GREATER_BLESSING_OF_MIGHT);
            ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
        }
        if (Pet* pet = player->GetPet())
        {
            if (!pet)
                return;

            if (!pet->IsControlled())
                return;

            if (pet->GetPetType() != HUNTER_PET)
                return;

            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(pet, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(pet, GREATER_BLESSING_OF_MIGHT);
            }
            if (player->GetTeam() == HORDE)
            {
                ApplyBuffAura(pet, WARCHIEFS_BLESSING);
                ApplyBuffAura(pet, STRENGTH_OF_EARTH);
                ApplyBuffAura(pet, GRACE_OF_AIR);
            }
            ApplyBuffAura(pet, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(pet, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(pet, FENGUS_FEROCITY_DIRE_MAUL);
            ApplyBuffAura(pet, TRACES_OF_SILITHYST);
            ApplyBuffAura(pet, SOUL_REVIVAL);
            ApplyBuffAura(pet, LEADER_OF_THE_PACK);
            ApplyBuffAura(pet, MARK_OF_THE_WILD);
            ApplyBuffAura(pet, TRUESHOT_AURA);
            ApplyBuffAura(pet, BATTLE_SHOUT);
        }
        break;
    case CLASS_DRUID:
        if (ClassSpec == "Balance")
        {
            if (player->GetTeam() == HORDE)
            {
                ApplyBuffAura(player, WARCHIEFS_BLESSING);
                ApplyBuffAura(player, MANA_SPRING);
            }
            ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(player, SONGFLOWER_SERENADE);
            ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
            ApplyBuffAura(player, TRACES_OF_SILITHYST);
            ApplyBuffAura(player, SOUL_REVIVAL);
            ApplyBuffAura(player, PRAYER_OF_SPIRIT);
            ApplyBuffAura(player, MOONKIN_AURA);
            ApplyBuffAura(player, ARCANE_BRILLIANCE);
            ApplyBuffAura(player, MARK_OF_THE_WILD);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
            }
        }
        if (ClassSpec == "Feral Combat")
        {
            if (player->GetTeam() == HORDE)
            {
                ApplyBuffAura(player, WARCHIEFS_BLESSING);
                ApplyBuffAura(player, MANA_SPRING);
                ApplyBuffAura(player, STRENGTH_OF_EARTH);
                ApplyBuffAura(player, GRACE_OF_AIR);
            }
            ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(player, SONGFLOWER_SERENADE);
            ApplyBuffAura(player, FENGUS_FEROCITY_DIRE_MAUL);
            ApplyBuffAura(player, TRACES_OF_SILITHYST);
            ApplyBuffAura(player, SOUL_REVIVAL);
            ApplyBuffAura(player, LEADER_OF_THE_PACK);
            ApplyBuffAura(player, MARK_OF_THE_WILD);
            ApplyBuffAura(player, TRUESHOT_AURA);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(player, GREATER_BLESSING_OF_MIGHT);
            }
            ApplyBuffAura(player, BATTLE_SHOUT);
        }
        if (ClassSpec == "Restoration")
        {
            if (player->GetTeam() == HORDE)
            {
                ApplyBuffAura(player, WARCHIEFS_BLESSING);
                ApplyBuffAura(player, MANA_SPRING);
            }
            ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
            ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
            ApplyBuffAura(player, SONGFLOWER_SERENADE);
            ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
            ApplyBuffAura(player, TRACES_OF_SILITHYST);
            ApplyBuffAura(player, SOUL_REVIVAL);
            ApplyBuffAura(player, PRAYER_OF_SPIRIT);
            ApplyBuffAura(player, MOONKIN_AURA);
            ApplyBuffAura(player, ARCANE_BRILLIANCE);
            ApplyBuffAura(player, MARK_OF_THE_WILD);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
                ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
            }
        }
        break;
    case CLASS_WARLOCK:
        if (player->GetTeam() == HORDE)
        {
            ApplyBuffAura(player, WARCHIEFS_BLESSING);
            ApplyBuffAura(player, MANA_SPRING);
        }
        ApplyBuffAura(player, RALLYING_CRY_OF_THE_DRAGONSLAYER);
        ApplyBuffAura(player, SPIRIT_OF_ZANDALAR);
        ApplyBuffAura(player, SONGFLOWER_SERENADE);
        ApplyBuffAura(player, SLIPKIKS_SAVVY_DIRE_MAUL);
        ApplyBuffAura(player, TRACES_OF_SILITHYST);
        ApplyBuffAura(player, SOUL_REVIVAL);
        ApplyBuffAura(player, PRAYER_OF_SPIRIT);
        ApplyBuffAura(player, MOONKIN_AURA);
        ApplyBuffAura(player, ARCANE_BRILLIANCE);
        ApplyBuffAura(player, MARK_OF_THE_WILD);
        if (player->GetTeam() == ALLIANCE)
        {
            ApplyBuffAura(player, GREATER_BLESSING_OF_KINGS);
            ApplyBuffAura(player, GREATER_BLESSING_OF_WISDOM);
        }
        break;
    }

    switch (TemplateNpc_GetPlayerClassId(player))
    {
    case CLASS_WARRIOR:
        if (ClassSpec == "Protection")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_STAMINA);
            ApplyBuffAura(player, MOLDARS_MOXIE_DIRE_MAUL);
            ApplyBuffAura(player, PRAYER_OF_FORTITUDE);
            ApplyBuffAura(player, FLASK_OF_THE_TITANS);
            ApplyBuffAura(player, ELIXIR_OF_FORTITUDE);
            ApplyBuffAura(player, ELIXIR_OF_THE_MONGOOSE);
            ApplyBuffAura(player, ELIXIR_OF_SUPERIOR_DEFENSE);
            ApplyBuffAura(player, ELIXIR_OF_BRUTE_FORCE);
            ApplyBuffAura(player, BLOOD_PACT);
            ApplyBuffAura(player, GREATER_STONESHIELD_POTION);
            ApplyBuffAura(player, CRYSTAL_WARD);
            ApplyBuffAura(player, ROIDS);
            ApplyBuffAura(player, BLOODKELP_ELIXIR_OF_DODGING);
            ApplyBuffAura(player, RUMSEY_RUM_BLACK_LABEL);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, DEVOTION_AURA);
            }
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, DENSE_WEIGHTSTONE, 1800000, 0);
        }
        else if (ClassSpec == "Arms")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_AGILITY);
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_STRENGTH);
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
            ApplyBuffAura(player, FURY_OF_THE_BOGLING);
            //Agility 
            ApplyBuffAura(player, ELIXIR_OF_GREATER_AGILITY);
            ApplyBuffAura(player, ELIXIR_OF_THE_MONGOOSE);
            //Attack Power 
            ApplyBuffAura(player, WINTERFALL_FIREWATER);
            ApplyBuffAura(player, JUJU_MIGHT);
            //Strength
            ApplyBuffAura(player, ELIXIR_OF_BRUTE_FORCE);
            ApplyBuffAura(player, ELIXIR_OF_GIANTS);
            ApplyBuffAura(player, JUJU_POWER);
            //Blasted Lands buffs
            ApplyBuffAura(player, GROUND_SCORPOK_ASSAY);
            ApplyBuffAura(player, ROIDS);
            //Food
            ApplyBuffAura(player, GRILLED_SQUID);
            ApplyBuffAura(player, BLESSED_SUNFRUIT);
            ApplyBuffAura(player, SMOKED_DESERT_DUMPLINGS);
            if (player->GetTeam() == ALLIANCE)
                ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, ELEMENTAL_SHARPENING_STONE, 1800000, 0);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND), TEMP_ENCHANTMENT_SLOT, DENSE_WEIGHTSTONE, 1800000, 0);
        }
        else if (ClassSpec == "Fury")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_AGILITY);
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_STRENGTH);
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
            ApplyBuffAura(player, FURY_OF_THE_BOGLING);
            //Agility 
            ApplyBuffAura(player, ELIXIR_OF_GREATER_AGILITY);
            ApplyBuffAura(player, ELIXIR_OF_THE_MONGOOSE);
            //Attack Power 
            ApplyBuffAura(player, WINTERFALL_FIREWATER);
            ApplyBuffAura(player, JUJU_MIGHT);
            //Strength
            ApplyBuffAura(player, ELIXIR_OF_BRUTE_FORCE);
            ApplyBuffAura(player, ELIXIR_OF_GIANTS);
            ApplyBuffAura(player, JUJU_POWER);
            //Blasted Lands buffs
            ApplyBuffAura(player, GROUND_SCORPOK_ASSAY);
            ApplyBuffAura(player, ROIDS);
            //Food
            ApplyBuffAura(player, GRILLED_SQUID);
            ApplyBuffAura(player, BLESSED_SUNFRUIT);
            ApplyBuffAura(player, SMOKED_DESERT_DUMPLINGS);
            if (player->GetTeam() == ALLIANCE)
                ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, DENSE_WEIGHTSTONE, 600000, 0);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND), TEMP_ENCHANTMENT_SLOT, ELEMENTAL_SHARPENING_STONE, 600000, 0);
        }
        break;
    case CLASS_PRIEST:
        if (ClassSpec == "Shadow")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
            ApplyBuffAura(player, FLASK_OF_SUPREME_POWER);
            ApplyBuffAura(player, GREATER_ARCANE_ELIXIR);
            ApplyBuffAura(player, ELIXIR_OF_SHADOW_POWER);
            ApplyBuffAura(player, MAGEBLOOD_POTION);
            ApplyBuffAura(player, CRYSTAL_FORCE);
            ApplyBuffAura(player, NIGHTFIN_SOUP);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_MANA_OIL, 1800000, 0);
        }
        else if (ClassSpec == "Disciplin")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_INTELLIGENCE);
            ApplyBuffAura(player, FLASK_OF_DISTILLED_WISDOM);
            ApplyBuffAura(player, ELIXIR_OF_THE_SAGES);
            ApplyBuffAura(player, MAGEBLOOD_POTION);
            ApplyBuffAura(player, NIGHTFIN_SOUP);
            ApplyBuffAura(player, CRYSTAL_FORCE);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_MANA_OIL, 1800000, 0);
        }
        else if (ClassSpec == "Holy")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_INTELLIGENCE);
            ApplyBuffAura(player, FLASK_OF_DISTILLED_WISDOM);
            ApplyBuffAura(player, ELIXIR_OF_THE_SAGES);
            ApplyBuffAura(player, MAGEBLOOD_POTION);
            ApplyBuffAura(player, NIGHTFIN_SOUP);
            ApplyBuffAura(player, CRYSTAL_FORCE);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_MANA_OIL, 1800000, 0);
        }
        break;
    case CLASS_PALADIN:
        if (ClassSpec == "Protection")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_STAMINA);
            ApplyBuffAura(player, MOLDARS_MOXIE_DIRE_MAUL);
            ApplyBuffAura(player, PRAYER_OF_FORTITUDE);
            ApplyBuffAura(player, FLASK_OF_THE_TITANS);
            ApplyBuffAura(player, ELIXIR_OF_FORTITUDE);
            ApplyBuffAura(player, ELIXIR_OF_THE_MONGOOSE);
            ApplyBuffAura(player, ELIXIR_OF_SUPERIOR_DEFENSE);
            ApplyBuffAura(player, ELIXIR_OF_BRUTE_FORCE);
            ApplyBuffAura(player, BLOOD_PACT);
            ApplyBuffAura(player, GREATER_STONESHIELD_POTION);
            ApplyBuffAura(player, CRYSTAL_WARD);
            ApplyBuffAura(player, ROIDS);
            ApplyBuffAura(player, BLOODKELP_ELIXIR_OF_DODGING);
            ApplyBuffAura(player, RUMSEY_RUM_BLACK_LABEL);
            ApplyBuffAura(player, DEVOTION_AURA);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, DENSE_WEIGHTSTONE, 1800000, 0);
        }
        if (ClassSpec == "Retribution")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);

            ApplyBuffAura(player, ELIXIR_OF_THE_MONGOOSE);
            ApplyBuffAura(player, ELIXIR_OF_GIANTS);
            ApplyBuffAura(player, ELIXIR_OF_BRUTE_FORCE);
            ApplyBuffAura(player, JUJU_MIGHT);
            ApplyBuffAura(player, ROIDS);
            ApplyBuffAura(player, WINTERFALL_FIREWATER);
            ApplyBuffAura(player, SMOKED_DESERT_DUMPLINGS);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, ELEMENTAL_SHARPENING_STONE, 1800000, 0);
        }
        if (ClassSpec == "Holy")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_INTELLIGENCE);
            ApplyBuffAura(player, FLASK_OF_DISTILLED_WISDOM);
            ApplyBuffAura(player, ELIXIR_OF_THE_SAGES);
            ApplyBuffAura(player, MAGEBLOOD_POTION);
            ApplyBuffAura(player, NIGHTFIN_SOUP);
            ApplyBuffAura(player, CRYSTAL_FORCE);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_MANA_OIL, 1800000, 0);
        }
        break;
    case CLASS_ROGUE:
        ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_AGILITY);
        ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
        ApplyBuffAura(player, FURY_OF_THE_BOGLING);

        //Agility 
        ApplyBuffAura(player, ELIXIR_OF_GREATER_AGILITY);
        ApplyBuffAura(player, ELIXIR_OF_THE_MONGOOSE);
        //Attack Power 
        ApplyBuffAura(player, WINTERFALL_FIREWATER);
        ApplyBuffAura(player, JUJU_MIGHT);
        //Strength
        ApplyBuffAura(player, ELIXIR_OF_BRUTE_FORCE);
        ApplyBuffAura(player, ELIXIR_OF_GIANTS);
        ApplyBuffAura(player, JUJU_POWER);
        //Blasted Lands buffs
        ApplyBuffAura(player, ROIDS);
        ApplyBuffAura(player, GROUND_SCORPOK_ASSAY);
        //Food
        ApplyBuffAura(player, BLESSED_SUNFRUIT);
        ApplyBuffAura(player, SMOKED_DESERT_DUMPLINGS);
        ApplyBuffAura(player, GRILLED_SQUID);
        if (player->GetTeam() == HORDE)
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND), TEMP_ENCHANTMENT_SLOT, CONSECRATED_WEAPON, 1800000, 0);
        break;
    case CLASS_MAGE:
        ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
        ApplyBuffAura(player, FLASK_OF_SUPREME_POWER);
        ApplyBuffAura(player, GREATER_ARCANE_ELIXIR);
        ApplyBuffAura(player, MAGEBLOOD_POTION);
        ApplyBuffAura(player, CRYSTAL_FORCE);
        ApplyBuffAura(player, HEADMASTERS_CHARGE);
        ApplyBuffAura(player, BLESSING_OF_BLACKFATHOM);
        ApplyBuffAura(player, CEREBRAL_CORTEX_COMPOUND);
        ApplyBuffAura(player, RUNN_TUM_TUBER_SURPRISE);
        if (ClassSpec == "Arcane")
            ApplyBuffAura(player, ELIXIR_OF_GREATER_FIREPOWER); //there is no arcane spell power+ pot, so we use fire...
        if (ClassSpec == "Frost")
            ApplyBuffAura(player, ELIXIR_OF_FROST_POWER);
        if (ClassSpec == "Fire")
            ApplyBuffAura(player, ELIXIR_OF_GREATER_FIREPOWER);
        ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_WIZARD_OIL, 1800000, 0);
        break;
    case CLASS_SHAMAN:
        if (ClassSpec == "Elemental")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
            ApplyBuffAura(player, FLASK_OF_SUPREME_POWER);
            ApplyBuffAura(player, GREATER_ARCANE_ELIXIR);
            ApplyBuffAura(player, MAGEBLOOD_POTION);
            ApplyBuffAura(player, CRYSTAL_FORCE);
            ApplyBuffAura(player, HEADMASTERS_CHARGE);
            ApplyBuffAura(player, BLESSING_OF_BLACKFATHOM);
            ApplyBuffAura(player, CEREBRAL_CORTEX_COMPOUND);
            ApplyBuffAura(player, RUNN_TUM_TUBER_SURPRISE);
            ApplyBuffAura(player, ELIXIR_OF_GREATER_FIREPOWER);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_WIZARD_OIL, 1800000, 0);
        }
        if (ClassSpec == "Enhancement")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_AGILITY);
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
            ApplyBuffAura(player, FLASK_OF_DISTILLED_WISDOM);
            ApplyBuffAura(player, FURY_OF_THE_BOGLING);
            //ApplyBuffAura(player, MAGEBLOOD_POTION);
            //Agility 
            ApplyBuffAura(player, ELIXIR_OF_GREATER_AGILITY);
            ApplyBuffAura(player, ELIXIR_OF_THE_MONGOOSE);
            //Attack Power 
            ApplyBuffAura(player, WINTERFALL_FIREWATER);
            ApplyBuffAura(player, JUJU_MIGHT);
            //Blasted Lands buffs
            ApplyBuffAura(player, ROIDS);
            ApplyBuffAura(player, GROUND_SCORPOK_ASSAY);
            //Strength
            ApplyBuffAura(player, ELIXIR_OF_BRUTE_FORCE);
            ApplyBuffAura(player, ELIXIR_OF_GIANTS);
            ApplyBuffAura(player, JUJU_POWER);
            //Food
            ApplyBuffAura(player, SMOKED_DESERT_DUMPLINGS);
            break;
        }
        if (ClassSpec == "Restoration")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_INTELLIGENCE);
            ApplyBuffAura(player, FLASK_OF_DISTILLED_WISDOM);
            ApplyBuffAura(player, ELIXIR_OF_THE_SAGES);
            ApplyBuffAura(player, MAGEBLOOD_POTION);
            ApplyBuffAura(player, NIGHTFIN_SOUP);
            ApplyBuffAura(player, CRYSTAL_FORCE);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_MANA_OIL, 1800000, 0);
        }
        break;
    case CLASS_HUNTER:
        ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_AGILITY);
        ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
        ApplyBuffAura(player, FLASK_OF_DISTILLED_WISDOM);
        //ApplyBuffAura(player, FURY_OF_THE_BOGLING);
        ApplyBuffAura(player, MAGEBLOOD_POTION);
        //Agility 
        ApplyBuffAura(player, ELIXIR_OF_GREATER_AGILITY);
        ApplyBuffAura(player, ELIXIR_OF_THE_MONGOOSE);
        //Attack Power 
        ApplyBuffAura(player, WINTERFALL_FIREWATER);
        ApplyBuffAura(player, JUJU_MIGHT);
        //Blasted Lands buffs
        ApplyBuffAura(player, ROIDS);
        ApplyBuffAura(player, GROUND_SCORPOK_ASSAY);
        //Strength
        ApplyBuffAura(player, ELIXIR_OF_BRUTE_FORCE);
        ApplyBuffAura(player, ELIXIR_OF_GIANTS);
        ApplyBuffAura(player, JUJU_POWER);
        //Food
        ApplyBuffAura(player, BLESSED_SUNFRUIT);
        ApplyBuffAura(player, SMOKED_DESERT_DUMPLINGS);
        ApplyBuffAura(player, GRILLED_SQUID);
        ApplyBuffAura(player, 24383);
        if (player->GetTeam() == ALLIANCE)
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, CONSECRATED_WEAPON, 1800000, 0);
        ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND), TEMP_ENCHANTMENT_SLOT, CONSECRATED_WEAPON, 1800000, 0);
        break;
    case CLASS_DRUID:
        if (ClassSpec == "Balance")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
            ApplyBuffAura(player, FLASK_OF_SUPREME_POWER);
            ApplyBuffAura(player, GREATER_ARCANE_ELIXIR);
            ApplyBuffAura(player, ELIXIR_OF_SHADOW_POWER);
            ApplyBuffAura(player, MAGEBLOOD_POTION);
            ApplyBuffAura(player, CRYSTAL_FORCE);
            ApplyBuffAura(player, NIGHTFIN_SOUP);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_MANA_OIL, 1800000, 0);
        }
        if (ClassSpec == "Feral Combat")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_AGILITY);
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
            ApplyBuffAura(player, FLASK_OF_DISTILLED_WISDOM);
            ApplyBuffAura(player, FURY_OF_THE_BOGLING);
            ApplyBuffAura(player, MAGEBLOOD_POTION);
            //Agility 
            ApplyBuffAura(player, ELIXIR_OF_GREATER_AGILITY);
            ApplyBuffAura(player, ELIXIR_OF_THE_MONGOOSE);
            //Attack Power 
            ApplyBuffAura(player, WINTERFALL_FIREWATER);
            ApplyBuffAura(player, JUJU_MIGHT);
            //Blasted Lands buffs
            ApplyBuffAura(player, ROIDS);
            ApplyBuffAura(player, GROUND_SCORPOK_ASSAY);
            //Strength
            ApplyBuffAura(player, ELIXIR_OF_BRUTE_FORCE);
            ApplyBuffAura(player, ELIXIR_OF_GIANTS);
            ApplyBuffAura(player, JUJU_POWER);
            //Food
            ApplyBuffAura(player, BLESSED_SUNFRUIT);
            ApplyBuffAura(player, SMOKED_DESERT_DUMPLINGS);
            ApplyBuffAura(player, GRILLED_SQUID);
            if (player->GetTeam() == ALLIANCE)
            {
                ApplyBuffAura(player, DEVOTION_AURA);
            }
            //ApplyBuffAura(player, 24383);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, DENSE_WEIGHTSTONE, 1800000, 0);
        }
        if (ClassSpec == "Restoration")
        {
            ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_INTELLIGENCE);
            ApplyBuffAura(player, FLASK_OF_DISTILLED_WISDOM);
            ApplyBuffAura(player, ELIXIR_OF_THE_SAGES);
            ApplyBuffAura(player, MAGEBLOOD_POTION);
            ApplyBuffAura(player, NIGHTFIN_SOUP);
            ApplyBuffAura(player, CRYSTAL_FORCE);
            ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_MANA_OIL, 1800000, 0);
        }
        break;
    case CLASS_WARLOCK:
        ApplyBuffAura(player, SAYGES_DARK_FORTUNE_OF_DAMAGE);
        ApplyBuffAura(player, FLASK_OF_SUPREME_POWER);
        ApplyBuffAura(player, GREATER_ARCANE_ELIXIR);
        ApplyBuffAura(player, ELIXIR_OF_SHADOW_POWER);
        ApplyBuffAura(player, MAGEBLOOD_POTION);
        ApplyBuffAura(player, CRYSTAL_FORCE);
        ApplyBuffAura(player, HEADMASTERS_CHARGE);
        ApplyBuffAura(player, BLESSING_OF_BLACKFATHOM);
        ApplyBuffAura(player, RUNN_TUM_TUBER_SURPRISE);
        ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND), TEMP_ENCHANTMENT_SLOT, BRILLIANT_WIZARD_OIL, 1800000, 0);
        break;
    default:
        break;
    }
}

uint32 GetTemplateID()
{
    auto TalentIdResult = CharacterDatabase.PQuery("SELECT MAX(temp_id) FROM template_npc_talents");
    auto GearIdResult = CharacterDatabase.PQuery("SELECT MAX(temp_id) FROM template_npc_gear");

    uint32 TalentId;
    uint32 GearId;

    if (TalentIdResult)
    {
        Field* fields = TalentIdResult->Fetch();
        TalentId = fields[0].GetInt32();
    }
    if (GearIdResult)
    {
        Field* fields = GearIdResult->Fetch();
        GearId = fields[0].GetInt32();
    }
    if (GearId = TalentId)
        return GearId + 1;
    else
        return 50;
}

void perform_npc_vendor_template()
{
    //auto result = WorldDatabase.PQuery("SELECT item FROM npc_vendor_template WHERE entry='15127'");
    auto result = WorldDatabase.PQuery("SELECT item FROM npc_vendor WHERE entry='15127'");

    //12799 15127

    if (result)
    {
        std::string alliance_id_name;

        do
        {
            Field *fields = result->Fetch();

            uint32 entry = fields[0].GetUInt32();

            if (entry)
            {
                if (ItemPrototype const* alliance_id_proto = sObjectMgr.GetItemPrototype(entry))
                    alliance_id_name = alliance_id_proto->Name1;

                auto founditem = WorldDatabase.PQuery("SELECT alliance_id FROM player_factionchange_items WHERE alliance_id='%u'", entry);

                if (!founditem)
                    WorldDatabase.PExecute("INSERT INTO player_factionchange_items (`alliance_id`, `horde_id`, `comment`) VALUES ('%u', '%u', '%s');", entry, 0, "todo");
            }

        } while (result->NextRow());
    }
}

void perform_player_factionchange_items()
{
    auto result = WorldDatabase.PQuery("SELECT alliance_id, horde_id, comment FROM player_factionchange_items");

    if (result)
    {
        std::string alliance_id_name;
        std::string horde_id_name;

        do
        {
            Field *fields = result->Fetch();

            uint32 alliance_id = fields[0].GetUInt32();
            uint32 horde_id = fields[1].GetUInt32();

            if (alliance_id && horde_id)
            {
                if (ItemPrototype const* alliance_id_proto = sObjectMgr.GetItemPrototype(alliance_id))
                    alliance_id_name = alliance_id_proto->Name1;

                if (ItemPrototype const* horde_id_proto = sObjectMgr.GetItemPrototype(horde_id))
                    horde_id_name = horde_id_proto->Name1;

                std::replace(alliance_id_name.begin(), alliance_id_name.end(), '\'', '\''); // replace quotes or Server sql will crash.
                std::replace(horde_id_name.begin(), horde_id_name.end(), '\'', '"');

                std::ostringstream QueryText;

                QueryText << alliance_id_name << " / " << horde_id_name;

                WorldDatabase.PExecute("UPDATE player_factionchange_items SET comment = '%s' WHERE alliance_id='%u'", QueryText.str().c_str(), alliance_id);
            }

        } while (result->NextRow());
    }
}

void ExtractGearTemplateToDB(Player* player, std::string& gossipTempText)
{
    uint32 TempID = GetTemplateID();
    uint32 patch = 0;

    // todo get specid

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (i == EQUIPMENT_SLOT_TABARD || i == EQUIPMENT_SLOT_BODY)
            continue;

        Item* equippedItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i);

        if (equippedItem)
        {
            uint32 itemId = equippedItem->GetEntry();

            auto getpatch = WorldDatabase.PQuery("SELECT patch FROM item_template "
                "WHERE entry = '%u'", itemId);

            if (getpatch)
            {
                Field* fields = getpatch->Fetch();
                uint32 newpatch = fields[0].GetInt32();
                if (patch < newpatch)
                    patch = newpatch;
            }

            CharacterDatabase.PExecute("INSERT INTO template_npc_gear (`temp_id`, `class`, `gossip_text`, `item_slot`, `item_entry`, `item_enchant`, `talent_tab_id`, `patch`) VALUES ('%u', '%s', '%s', '%u', '%u', '%u', '%u', '%u');"
                , TempID, GetClassKeyString(player), gossipTempText.c_str(), equippedItem->GetSlot(), equippedItem->GetEntry(), equippedItem->GetEnchantmentId(PERM_ENCHANTMENT_SLOT), GetSpecIDByTalentPoints(player), patch);
        }

        CharacterDatabase.PExecute("UPDATE template_npc_gear SET patch = '%u' WHERE temp_id='%u'", patch, TempID);
    }
}

bool IsIgnoredSpell(uint32 spellID)
{
    std::vector<uint32> ignoreSpells;

    uint32 temp[] = {
        877, //Elemental Fury
        1868 //zzOLDHoly Flurry
    };

    ignoreSpells = std::vector<uint32>(temp, temp + sizeof(temp) / sizeof(temp[0]));

    for (std::vector<uint32>::const_iterator itr = ignoreSpells.begin(); itr != ignoreSpells.end(); ++itr)
        if (spellID == (*itr))
            return true;
    return false;
}

void LearnSkillRecipes(Player *player, uint32 skill_id)
{
    uint32 classmask = (TemplateNpc_GetPlayerClassId(player) ? (1u << (TemplateNpc_GetPlayerClassId(player)-1)) : 0u);

    for (uint32 j = 0; j < sObjectMgr.GetMaxSkillLineAbilityId(); ++j)
    {
        SkillLineAbilityEntry const *skillLine = sObjectMgr.GetSkillLineAbility(j);
        if (!skillLine)
            continue;

        // wrong skill
        if (skillLine->skillId != skill_id)
            continue;

        // not high rank
        if (skillLine->forward_spellid)
            continue;

        // skip racial skills
        if (skillLine->racemask != 0)
            continue;

        // skip wrong class skills
        if (skillLine->classmask && (skillLine->classmask & classmask) == 0)
            continue;

        SpellEntry const* spellEntry = sSpellMgr.GetSpellEntry(skillLine->spellId);
        if (!spellEntry || !SpellMgr::IsSpellValid(spellEntry, player, false))
            continue;

        player->LearnSpell(skillLine->spellId, false);
    }
}

bool LearnAllRecipesProfession(Player *pPlayer, SkillType skill)
{
    ChatHandler handler(pPlayer->GetSession());
    char* skill_name;

    SkillLineEntry const *SkillInfo = sSkillLineStore.LookupEntry(skill);
    skill_name = SkillInfo->name[sWorld.GetDefaultDbcLocale()];

    if (!SkillInfo)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "Profession NPC: received non-valid skill ID");
        return false;
    }

    pPlayer->SetSkill(SkillInfo->id, 300, 300);
    LearnSkillRecipes(pPlayer, SkillInfo->id);
    pPlayer->GetSession()->SendNotification("All recipes for %s learned", skill_name);
    return true;
}

void LearnProfession(Player *pPlayer, Creature *pCreature, SkillType skill)
{
    if (pPlayer->GetFreePrimaryProfessionPoints() == 0 && !(skill == SKILL_COOKING || skill == SKILL_FIRST_AID))
    {
        pPlayer->GetSession()->SendNotification("You already know two primary professions.");
    }
    else
    {
        if (!LearnAllRecipesProfession(pPlayer, skill))
            pPlayer->GetSession()->SendNotification("Internal error.");
    }
}

void LearnWarlockPetSpells(Player* player)
{
    if (TemplateNpc_GetPlayerClassId(player) != CLASS_WARLOCK)
        return;

    if (Pet* pet = player->GetPet())
    {
        if (pet->GetPetType() != SUMMON_PET)
            return;

        uint32 petentry = pet->GetEntry();

        if (!petentry)
            return;

        switch (petentry)
        {
        case WARLOCK_PET_IMP:
            pet->LearnSpell(WARLOCK_PETSPELL_IMP_PHASE_SHIFT);
            pet->LearnSpell(WARLOCK_PETSPELL_IMP_BLOOD_PACT_RANK5);
            pet->LearnSpell(WARLOCK_PETSPELL_IMP_FIRE_SHIELD_RANK5);
            pet->LearnSpell(WARLOCK_PETSPELL_IMP_FIREBOLT_RANK7);
            pet->CastSpell(pet, COOL_VISUAL_SPELL, true);
            pet->CastSpell(pet, COOL_VISUAL_SPELL_3, true);
            break;
        case WARLOCK_PET_SUCCUBUS:
            pet->LearnSpell(WARLOCK_PETSPELL_SUCCUBUS_SEDUCTION);
            pet->LearnSpell(WARLOCK_PETSPELL_SUCCUBUS_SOOTHING_KISS);
            pet->LearnSpell(WARLOCK_PETSPELL_SUCCUBUS_LESSER_INVISIBILITY);
            pet->LearnSpell(WARLOCK_PETSPELL_SUCCUBUS_LASH_OF_PAIN_RANK6);
            pet->CastSpell(pet, COOL_VISUAL_SPELL, true);
            pet->CastSpell(pet, COOL_VISUAL_SPELL_3, true);
            break;
        case WARLOCK_PET_VOIDWALKER:
            pet->LearnSpell(WARLOCK_PETSPELL_VOIDWALKER_SUFFERING_RANK4);
            pet->LearnSpell(WARLOCK_PETSPELL_VOIDWALKER_TORMENT_RANK6);
            pet->LearnSpell(WARLOCK_PETSPELL_VOIDWALKER_SACRIFICE_RANK6);
            pet->LearnSpell(WARLOCK_PETSPELL_VOIDWALKER_CONSUME_SHADOWS_RANK6);
            pet->CastSpell(pet, COOL_VISUAL_SPELL, true);
            pet->CastSpell(pet, COOL_VISUAL_SPELL_3, true);
            break;
        case WARLOCK_PET_FELHUNTER:
            pet->LearnSpell(WARLOCK_PETSPELL_FELHUNTER_PARANOIA);
            pet->LearnSpell(WARLOCK_PETSPELL_FELHUNTER_SPELL_LOCK_RANK2);
            pet->LearnSpell(WARLOCK_PETSPELL_FELHUNTER_DEVOUR_MAGIC_RANK4);
            pet->LearnSpell(WARLOCK_PETSPELL_FELHUNTER_TAINTED_BLOOD_RANK4);
            pet->CastSpell(pet, COOL_VISUAL_SPELL, true);
            pet->CastSpell(pet, COOL_VISUAL_SPELL_3, true);
            break;
        default:
            break;
        }
    }
}
/* unused
void LearnAllHunterPetSpells(Player* player)
{
    //if (!TrainerID)
        //return;

    CreatureInfo const* creature = sObjectMgr.GetCreatureTemplate(10088);

    if (!creature)
        return;

    if (TrainerSpellData const* cSpells = sObjectMgr.GetNpcTrainerSpells(10088))

    for (TrainerSpellMap::const_iterator itr = cSpells->spellList.begin(); itr != cSpells->spellList.end(); ++itr)
    {
        TrainerSpell const* tSpell = &itr->second;

        uint32 triggerSpell = sSpellMgr.GetSpellEntry(tSpell->spell)->EffectTriggerSpell[0];

        if (!player->IsSpellFitByClassAndRace(tSpell->spell))
            continue;

        SkillLineAbilityMapBounds bounds = sSpellMgr.GetSkillLineAbilityMapBounds(tSpell->spell);

        TrainerSpellState state = player->GetTrainerSpellState(tSpell);

        if (state != TRAINER_SPELL_GREEN)
            continue;

        player->CastSpell(player, tSpell->spell, true);
    }
}
*/

void PlayerLearnAllHunterPetSpellsDB(Player* player)
{
    if (TemplateNpc_GetPlayerClassId(player) != CLASS_HUNTER)
        return;

    if (Pet* pet = player->GetPet())
    {
        if (!pet)
            return;

        if (!pet->IsControlled())
            return;

        if (pet->GetPetType() != HUNTER_PET)
            return;
    }

    auto select = WorldDatabase.PQuery("SELECT ID FROM pet_spell_template;");

    if (!select) {
        return;
    }
    else
    {
        do
        {
            Field* fields = select->Fetch();
            uint32 spellId = fields[0].GetUInt32();

            player->LearnSpell(spellId, false);

        } while (select->NextRow());
    }
}

void SaveHunterPetSpellsToDB(Player* player)
{
    if (TemplateNpc_GetPlayerClassId(player) != CLASS_HUNTER)
        return;

    if (Pet* pet = player->GetPet())
    {
        if (TemplateNpc_GetPlayerClassId(player) != CLASS_HUNTER)
            return;

        if (!pet)
            return;

        if (!pet->IsControlled())
            return;
    }

    Pet* pet = player->GetPet();

    uint32 petentry = pet->GetEntry();

    CreatureInfo const *cInfo = sObjectMgr.GetCreatureTemplate(petentry);

    if (!cInfo) return;

    std::string petname = cInfo->name;

    static SqlStatementID delSpell;
    static SqlStatementID insSpell;
    SqlStatement stmtdel = CharacterDatabase.CreateStatement(delSpell, "DELETE FROM template_pet_spell WHERE name = ? AND spell = ?");
    SqlStatement stmtins = CharacterDatabase.CreateStatement(insSpell, "INSERT INTO template_pet_spell (name,spell) VALUES (?, ?)");

    for (PetSpellMap::iterator itr = pet->m_petSpells.begin(), next = pet->m_petSpells.begin(); itr != pet->m_petSpells.end(); itr = next)
    {
        ++next;

        // prevent saving family passives to DB
        if (itr->second.type == PETSPELL_FAMILY)
            continue;

        if (itr->second.state == PETSPELL_REMOVED)
            continue;

        stmtdel.PExecute(petname.c_str(), itr->first);
        stmtins.PExecute(petname.c_str(), itr->first);
    }
}

void ExportHunterPetToDB(Player* player)
{
    if (Pet* pet = player->GetPet())
    {
        if (TemplateNpc_GetPlayerClassId(player) != CLASS_HUNTER)
            return;

        if (!pet)
            return;

        if (!pet->IsControlled())
            return;

        if (pet->GetPetType() != HUNTER_PET)
            return;
    }

    static SqlStatementID insPet;
    static SqlStatementID delPet;
    SqlStatement PetsDEL = CharacterDatabase.CreateStatement(delPet, "DELETE FROM template_pets WHERE name = ? and entry = ?");
    SqlStatement PetsINS = CharacterDatabase.CreateStatement(insPet, "INSERT INTO template_pets (name, entry, PetFamily, AttackSpeed) VALUES (?, ?, ?, ?)");

    uint32 petentry = player->GetPet()->GetEntry();

    CreatureInfo const *cInfo = sObjectMgr.GetCreatureTemplate(petentry);

    if (!cInfo) return;

    std::string petname = cInfo->name;
    uint32 petfamily = cInfo->pet_family;
    uint32 attackspeed = cInfo->base_attack_time;
    PetsDEL.PExecute(petname.c_str(), petentry);
    PetsINS.PExecute(petname.c_str(), petentry, petfamily, attackspeed);
}

void LearnPetSpellsFromDB(Player* player)
{
    if (Pet* pet = player->GetPet())
    {
        if (TemplateNpc_GetPlayerClassId(player) != CLASS_HUNTER)
            return;

        if (!pet)
            return;

        if (!pet->IsControlled())
            return;

        if (pet->GetPetType() != HUNTER_PET)
            return;
    }

    uint32 petentry = player->GetPet()->GetEntry();

    CreatureInfo const *cInfo = sObjectMgr.GetCreatureTemplate(petentry);

    if (!cInfo) return;

    std::string petname = cInfo->name;

	auto select = CharacterDatabase.PQuery(
		"SELECT spell FROM template_pet_spell WHERE entry = '%u' AND active = 1;",
		petentry);
	
    if (select)
    {
        do
        {
            Field* fields = select->Fetch();
            uint32 spellId = fields[0].GetUInt32();

            if (spellId)
                player->GetPet()->LearnSpell(spellId);

        } while (select->NextRow());
    }

    //player->GetPet()->SetTP(0);
    player->GetPet()->SavePetToDB(PET_SAVE_AS_CURRENT);
}

void CreateHunterPet(Player *player, Creature * m_creature, uint32 entry)
{
    if (TemplateNpc_GetPlayerClassId(player) != CLASS_HUNTER)
        return;

    if (Pet* pet = player->GetPet())
    {
        if (pet)
        {
            //player->CastSpell(player, 883, true); // Call Pet. If you don't do this and your pet is dismissed the Server will crash. Need to find a fix.
            player->GetSession()->SendAreaTriggerMessage("You already have a Pet.");
            return;
        }

        if (pet->GetPetType() == HUNTER_PET)
        {
            pet->Unsummon(PET_SAVE_AS_DELETED, player);
            //player->GetSession()->SendAreaTriggerMessage("You already have a Pet.");
            //return;
        }
    }

    if (Creature *creatureTarget = m_creature->SummonCreature(entry, player->GetPositionX(), player->GetPositionY() + 2, player->GetPositionZ(), player->GetOrientation(), TEMPSUMMON_CORPSE_DESPAWN))
    {

        if (!creatureTarget) return;

        // cast Tame Beast.
        Pet* pet = new Pet(HUNTER_PET);

        // Nostalrius: defensive as default behaviour
        pet->SetReactState(REACT_DEFENSIVE);

        if (!pet->CreateBaseAtCreature(creatureTarget))
        {
            delete pet;
            pet = nullptr;
            return;
        }

        pet->SetOwnerGuid(player->GetObjectGuid());
        pet->SetFactionTemplateId(player->GetFactionTemplateId());
        pet->SetCreatorGuid(player->GetObjectGuid());
        //pet->SetUInt32Value(UNIT_CREATED_BY_SPELL, m_spellInfo->Id);

        if (player->IsPvP())
            pet->SetPvP(true);

        if (!pet->InitStatsForLevel(creatureTarget->GetLevel()))
        {
            sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "Pet::InitStatsForLevel() failed for creature (Entry: %u)!", creatureTarget->GetEntry());
            return;
        }

        pet->GetCharmInfo()->SetPetNumber(sObjectMgr.GeneratePetNumber(), true);
        // this enables pet details window (Shift+P)
        pet->AIM_Initialize();
        pet->InitPetCreateSpells();
        pet->SetHealth(pet->GetMaxHealth());

        // "kill" original creature
        creatureTarget->ForcedDespawn();

        // add to world
        pet->GetMap()->Add((Creature*)pet);

        // caster have pet now
        player->SetPet(pet);

        pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(POWER_HAPPINESS));

        pet->SetLoyaltyLevel(BEST_FRIEND);
        pet->InitStatsForLevel(TemplateNpc_GetPlayerLevel(player));
        pet->GetMaxLoyaltyPoints(pet->GetLevel());
        LearnPetSpellsFromDB(player);
        pet->SetTP(0);
        pet->UpdateAllStats();


        player->PetSpellInitialize();

        pet->SavePetToDB(PET_SAVE_AS_CURRENT);
    }

}

void LearnAllTrainerSpellsDB(Player* player, uint32 TrainerID)
{
    if (!TrainerID)
        return;

    auto select = WorldDatabase.PQuery("SELECT spell FROM npc_trainer WHERE entry = '%u' "
        "ORDER BY reqlevel ASC;", TrainerID);

    if (select)
    {
        do
        {
            Field* fields = select->Fetch();
            uint32 spellId = fields[0].GetUInt32();

            // exist, already checked at loading
            SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
            SpellEntry const* TriggerSpell = sSpellMgr.GetSpellEntry(spell->EffectTriggerSpell[0]);

            // check race/class requirement
            if (!player->IsSpellFitByClassAndRace(TriggerSpell->Id))
                continue;

            if (!SpellMgr::IsSpellValid(TriggerSpell, player, false))
                continue;

            if (SpellChainNode const* spell_chain = sSpellMgr.GetSpellChainNode(TriggerSpell->Id))
            {
                // check prev.rank requirement
                if (spell_chain->prev && !player->HasSpell(spell_chain->prev))
                    continue;

                // check additional spell requirement
                if (spell_chain->req && !player->HasSpell(spell_chain->req))
                    continue;
            }

            if (IsIgnoredSpell(TriggerSpell->Id))
                continue;

            // if we do not this SQL check it learns some weird spells too.. i don't know why!
            auto qspell = WorldDatabase.PQuery("SELECT spell FROM npc_trainer WHERE spell = '%u' AND entry = '%u';", spellId, TrainerID);

            if (qspell && !player->HasSpell(TriggerSpell->Id))
                player->LearnSpell(TriggerSpell->Id, false);
            //player->LearnSpellHighRank(TriggerSpell->Id);

        } while (select->NextRow());
    }
}

void LearnAllSpells(Player* player, Creature* m_creature)
{
    uint32 TrainerID = 0;

    enum TrainerIDS
    {
        RIDING_TRAINER = 4752,              //Kildar <Riding Trainer>
        WEAPON_MASTER_IRONFORGE = 11865,    //Buliwyf Stonehand <Weapon Master>
        WEAPON_MASTER_DARNASSUS = 11866,    //Ilyenia Moonfire <Weapon Master>
        WEAPON_MASTER_STORMWIND = 11867,    //Woo Ping <Weapon Master>
        WARRIOR_TRAINER = 914,              //Ander Germaine <Warrior Trainer>
        PRIEST_TRAINER = 3046,              //Father Cobb <Priest Trainer>
        PALADIN_TRAINER = 928,              //Lord Grayson Shadowbreaker <Paladin Trainer>
        ROGUE_TRAINER = 13283,              //Lord Tony Romano <Rogue Trainer>
        MAGE_TRAINER = 3047,                //Archmage Shymm <Mage Trainer>
        SHAMAN_TRAINER = 3032,              //Beram Skychaser <Shaman Trainer>
        HUNTER_TRAINER = 5115,              //Daera Brightspear <Hunter Trainer>
        DRUID_TRAINER = 9465,               //Golhine the Hooded <Druid Trainer>
        WARLOCK_TRAINER = 5173,             //Alexander Calder <Warlock Trainer>
        PET_TRAINER = 10088,                //Xao'tsu <Pet Trainer>
        PORTAL_TRAINER_TB = 5957,           //Birgitte Cranston <Portal Trainer>
        PORTAL_TRAINER_DA = 4165,           //Elissa Dumas <Portal Trainer>
        PORTAL_TRAINER_SW = 2485,           //Larimaine Purdue <Portal Trainer>
        PORTAL_TRAINER_UC = 2492,           //Lexington Mortaim <Portal Trainer>
        PORTAL_TRAINER_IF = 2489,           //Milstaff Stormeye <Portal Trainer>
        PORTAL_TRAINER_OG = 5958            //Thuul <Portal Trainer>
    };

    switch (TemplateNpc_GetPlayerClassId(player))
    {
    case CLASS_WARRIOR:
        TrainerID = WARRIOR_TRAINER;
        break;
    case CLASS_PRIEST:
        TrainerID = PRIEST_TRAINER;
        break;
    case CLASS_PALADIN:
        TrainerID = PALADIN_TRAINER;
        break;
    case CLASS_ROGUE:
        TrainerID = ROGUE_TRAINER;
        break;
    case CLASS_MAGE:
        TrainerID = MAGE_TRAINER;
        LearnAllTrainerSpellsDB(player, PORTAL_TRAINER_TB);
        LearnAllTrainerSpellsDB(player, PORTAL_TRAINER_DA);
        LearnAllTrainerSpellsDB(player, PORTAL_TRAINER_SW);
        LearnAllTrainerSpellsDB(player, PORTAL_TRAINER_UC);
        LearnAllTrainerSpellsDB(player, PORTAL_TRAINER_IF);
        LearnAllTrainerSpellsDB(player, PORTAL_TRAINER_OG);
        break;
    case CLASS_SHAMAN:
        TrainerID = SHAMAN_TRAINER;
        break;
    case CLASS_HUNTER:
        TrainerID = HUNTER_TRAINER;
        break;
    case CLASS_DRUID:
        TrainerID = DRUID_TRAINER;
        break;
    case CLASS_WARLOCK:
        TrainerID = WARLOCK_TRAINER;
        break;
    default:
        break;
    }

    LearnAllTrainerSpellsDB(player, TrainerID);
    LearnAllTrainerSpellsDB(player, WEAPON_MASTER_IRONFORGE);
    LearnAllTrainerSpellsDB(player, WEAPON_MASTER_DARNASSUS);
    LearnAllTrainerSpellsDB(player, WEAPON_MASTER_STORMWIND);
    //LearnAllTrainerSpellsDB(player, RIDING_TRAINER);
}

void LearnTalentsFromDB(Player* player, uint32 temp_id)
{
    if (!player)
        return;

    const std::vector<TemplateNpcCache::TalentEntry>* talents = TemplateNpcCache::GetTalents(player, temp_id);
    if (!talents)
        return;

    for (const auto& te : *talents)
        player->LearnSpell(te.talentId, false, true);
}



// Cause of the AQ ClassBooks etc.
void UpgradePlayerSpellsToMax(Player* player)
{
    ChrClassesEntry const* clsEntry = sChrClassesStore.LookupEntry(TemplateNpc_GetPlayerClassId(player));
    if (!clsEntry)
        return;
    uint32 family = clsEntry->spellfamily;

    PlayerSpellMap m_spells = player->GetSpellMap();

    for (PlayerSpellMap::iterator itr = m_spells.begin(); itr != m_spells.end(); ++itr)
    {
        if (itr->second.state == PLAYERSPELL_REMOVED || itr->second.disabled)
            continue;

        SpellEntry const* spellEntry = sSpellMgr.GetSpellEntry(itr->first);
        if (!spellEntry || !SpellMgr::IsSpellValid(spellEntry, player, false))
            continue;

        if (!player->HasSpell(spellEntry->Id))
            continue;

        if (spellEntry->spellLevel == 0)
            continue;

        if (!player->IsSpellFitByClassAndRace(spellEntry->Id))
            continue;

        // skip other spell families
        if (spellEntry->SpellFamilyName != family)
            continue;

        if (SpellChainNode const* spell_chain = sSpellMgr.GetSpellChainNode(spellEntry->Id))
        {
            // check prev.rank requirement
            if (spell_chain->prev && !player->HasSpell(spell_chain->prev))
                continue;

            // check additional spell requirement
            if (spell_chain->req && !player->HasSpell(spell_chain->req))
                continue;
        }

        player->LearnSpellHighRank(itr->first);
    }
}

void DeleteEquippedGear(Player* player)
{
    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; i++)
    {
        if (i == EQUIPMENT_SLOT_TABARD || i == EQUIPMENT_SLOT_BODY)
            continue;

        if (Item* haveItemEquipped = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (haveItemEquipped)
            {
                player->DestroyItem(INVENTORY_SLOT_BAG_0, i, true);
            }
        }
    }
}

void DeleteBagsAndContent(Player* player)
{
    for (uint8 i = PLAYER_SLOT_START; i < PLAYER_SLOT_END; ++i)
    {
        if (Item* pItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (pItem)
            {
                if (pItem->IsBag())
                {
                    if (pItem->IsEquipped())
                    {
                        for (int e = 0; e < MAX_BAG_SIZE; ++e)
                            player->DestroyItem(INVENTORY_SLOT_BAG_0, e, true);
                    }
                    else if (!((Bag*)pItem)->IsEmpty())
                        player->DestroyItem(INVENTORY_SLOT_BAG_0, i, true);
                }
            }
        }
    }
}

void AddBags(Player* player)
{
    bool hasQuiver = false;
    bool hasFelcloth = false;
    bool hasTotems = false;

    for (int bagslot = INVENTORY_SLOT_BAG_START; bagslot < INVENTORY_SLOT_BAG_END; ++bagslot)
    {
        if (TemplateNpc_GetPlayerClassId(player) == CLASS_SHAMAN && !hasTotems)
        {
            player->AddItem(FIRE_TOTEM, 1);
            player->AddItem(WATER_TOTEM, 1);
            player->AddItem(AIR_TOTEM, 1);
            player->AddItem(EARTH_TOTEM, 1);
            hasTotems = true;
        }
        if (TemplateNpc_GetPlayerClassId(player) == CLASS_HUNTER)
        {
            if (!hasQuiver)
            {
                player->EquipNewItem(bagslot, ANCIENT_SINEW_WRAPPED_LAMINA, true);
                player->AddItem(DOOMSHOT, 3600);
                player->SetAmmo(DOOMSHOT);
                hasQuiver = true;
            }
            else
                player->EquipNewItem(bagslot, BOTTOMLESS_BAG, true);
        }
        else if (TemplateNpc_GetPlayerClassId(player) == CLASS_WARLOCK)
        {
            if (!hasFelcloth)
            {
                player->EquipNewItem(bagslot, CORE_FELCLOTH_BAG, true);
                player->AddItem(SOUL_SHARD, 28);
                hasFelcloth = true;
            }
            else
                player->EquipNewItem(bagslot, BOTTOMLESS_BAG, true);
        }
        else
            player->EquipNewItem(bagslot, BOTTOMLESS_BAG, true);
    }
}

void EquipItemsFromDB(Player* player, uint32 temp_id)
{
    if (!temp_id)
    {
        ChatHandler(player).PSendSysMessage("blob");
        return;
    }

    for (uint8 equipmentSlot = EQUIPMENT_SLOT_START; equipmentSlot < EQUIPMENT_SLOT_END; ++equipmentSlot)
    {
        if (equipmentSlot == EQUIPMENT_SLOT_TABARD || equipmentSlot == EQUIPMENT_SLOT_BODY)
            continue;

        uint32 itemEntry = 0;
        uint32 enchant = 0;

        if (!TemplateNpcCache::GetGearItem(player, temp_id, equipmentSlot, itemEntry, enchant))
            continue;

        ItemPrototype const* item_proto = sObjectMgr.GetItemPrototype(itemEntry);

        if (!item_proto)
            continue;

        // Check if we need some Reputation for the Item and set it to Exalted.
        if (item_proto->RequiredReputationFaction && item_proto->RequiredReputationRank > 0)
        {
            if (ReputationRank(item_proto->RequiredReputationRank) > player->GetReputationRank(item_proto->RequiredReputationFaction))
                player->GetReputationMgr().ModifyReputation(sObjectMgr.GetFactionEntry(item_proto->RequiredReputationFaction), 85000);
        }

        // Looking for if the item has another entry for the opposite Team in player_factionchange_items.
        std::unique_ptr<QueryResult> player_factionchange_items(WorldDatabase.PQuery("SELECT alliance_id, horde_id "
            "FROM player_factionchange_items WHERE alliance_id = '%u' OR horde_id = '%u';", itemEntry, itemEntry));

        if (player_factionchange_items)
        {
            Field* fields = player_factionchange_items->Fetch();
            uint32 alliance_id = fields[0].GetUInt32();
            uint32 horde_id = fields[1].GetUInt32();

            if (player->GetTeam() == ALLIANCE)
                player->EquipNewItem(equipmentSlot, alliance_id, true); // Found alliance item.
            if (player->GetTeam() == HORDE)
                player->EquipNewItem(equipmentSlot, horde_id, true);    // Found horde item.
        }
        else
            player->EquipNewItem(equipmentSlot, itemEntry, true); // No items found in player_factionchange_items equip itemEntry.

        // Apply Enchants
        ApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, equipmentSlot), PERM_ENCHANTMENT_SLOT, enchant, 0, 0);

        /* Get itemIcon in Chat: not working in Classic WoW?

        ItemPrototype const* item_proto = sObjectMgr.GetItemPrototype(itemEntry);
        uint32 itemID = item_proto->DisplayInfoID;
        std::string itemName = item_proto->Name1;

        if (itemID)
        {
            auto result = WorldDatabase.PQuery("SELECT ID, icon "
            "FROM item_display_info WHERE ID = '%u';", itemID);

            Field* fields = result->Fetch();
            std::string IcnStr = fields[1].GetString();

            std::ostringstream ChatText;
            ChatText << "|TInterface\\Icons\\" << IcnStr << ".blp:20|t" << itemName << " equipped.";

            ChatHandler(player).PSendSysMessage(ChatText.str().c_str());
            player->GetSession()->SendNotification(ChatText.str().c_str());
        }
        */
    }
}

void ApplyTemplateToPlayer(Player* player, Creature* creature, uint32 temp_id) // Merge
{
    player->ResetTalents(true);
    LearnTalentsFromDB(player, temp_id);
    EquipItemsFromDB(player, temp_id);
    player->UpdateSkillsToMaxSkillsForLevel();
    player->RemoveAllCooldowns();
    LearnAllSpells(player, creature);


    //UpgradePlayerSpellsToMax(player); do not use this here or it will upgrade some talents you don't learned.
    //player->SaveToDB();

    creature->CastSpell(player, COOL_VISUAL_SPELL, true);
    player->CastSpell(player, COOL_VISUAL_SPELL_3, true);

    //player->GetSession()->SendAreaTriggerMessage("Successfuly equipped %s %s template!", temp_id, GetClassKeyString(player));
}

// Export Talent Spell ID's (not Talent ID's) to Database!
void ExportCharacterTalentsToDB(Player* player, std::string& gossipTempText)
{
    static SqlStatementID insTalents;
    uint32 TempID = GetTemplateID();

    SqlStatement stmtIns = CharacterDatabase.CreateStatement(insTalents, "INSERT INTO template_npc_talents (temp_id, class, talent_id, rank) VALUES (?, ?, ?, ?)");

    if (player->GetFreeTalentPoints() > 0)
    {
        player->GetSession()->SendAreaTriggerMessage("You have unspend talent points. Please spend all your talent points.");
        return;
    }

    for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
    {
        TalentEntry const *talentInfo = sTalentStore.LookupEntry(i);
        if (!talentInfo)
            continue;

        TalentTabEntry const *talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
        if (!talentTabInfo)
            continue;

        if (((TemplateNpc_GetPlayerClassId(player) ? (1u << (TemplateNpc_GetPlayerClassId(player)-1)) : 0u) & talentTabInfo->ClassMask) == 0)
            continue;

        uint32 spellid = 0;

        for (int rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        {
            PlayerSpellMap const& uSpells = player->GetSpellMap();
            for (PlayerSpellMap::const_iterator itr = uSpells.begin(); itr != uSpells.end(); ++itr)
            {
                if (itr->second.state == PLAYERSPELL_REMOVED || itr->second.disabled)
                    continue;

                uint32 rank2 = sSpellMgr.GetSpellRank(itr->first);

                if (itr->first == talentInfo->RankID[rank] || sSpellMgr.IsSpellLearnToSpell(talentInfo->RankID[rank], itr->first))
                    stmtIns.PExecute(TempID, GetClassKeyString(player), talentInfo->RankID[rank], rank + 1);
            }
        }
    }
}

bool GossipHello_TemplateNPC(Player* player, Creature* creature)
{
    if (player->IsInCombat())
    {
        player->GetSession()->SendNotification("You are in combat!");
        return true;
    }

    player->PlayerTalkClass->ClearMenus();

    if (player->GetHealth() != player->GetMaxHealth())
    {
        creature->CastSpell(player, 24171, true);
        player->ModifyHealth(player->GetMaxHealth());
    }

    switch (TemplateNpc_GetPlayerClassId(player))
    {
    case CLASS_PRIEST:

    case CLASS_PALADIN:

    case CLASS_MAGE:

    case CLASS_WARLOCK:

    case CLASS_SHAMAN:

    case CLASS_DRUID:

    case CLASS_HUNTER:
        if (player->GetPower(POWER_MANA) != player->GetMaxPower(POWER_MANA))
            player->ModifyPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    default:
        break;
    }

    if (TemplateNpc_GetPlayerLevel(player) < 60)
    {
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "I am weak... Do something!", GOSSIP_SENDER_MAIN, GET_LVL_60);
    }
    else
    {
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_VENDOR, "What can I buy from you?", GOSSIP_SENDER_MAIN, GOSSIP_OPTION_VENDOR);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Reset my Talents.", GOSSIP_SENDER_MAIN, RESET_TALENTS);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Upgrade all my Talents Ranks.", GOSSIP_SENDER_MAIN, UPGRADE_TALENTS);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Reset my Cooldowns & Spell Charges.", GOSSIP_SENDER_MAIN, RESET_COOLDOWNS_AND_CHARGES);
        //player->ADD_GOSSIP_ITEM_EXTENDED(GOSSIP_ICON_CHAT,        "Delete my Equipped Gear.", GOSSIP_SENDER_MAIN, DELETE_GEAR, "Are you sure?", false);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Buff me please.", GOSSIP_SENDER_MAIN, ALL_BUFFS);

        if (TemplateNpc_GetPlayerClassId(player) == CLASS_HUNTER)
        {
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, "Choose a Hunter Pet", GOSSIP_SENDER_MAIN, SHOW_PETS);
        
            if (player->GetPet() && player->GetPet()->IsControlled() && player->GetPet()->GetPetType() == HUNTER_PET)
            {
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Make my pet happy", GOSSIP_SENDER_MAIN, MAKE_PET_HAPPY);
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Save my current pet", GOSSIP_SENDER_MAIN, SAVE_PET);
            }
        }
        
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Professions & Recipes", GOSSIP_SENDER_MAIN, SHOW_PROF_MENU);
        bool hasTemplates = TemplateNpcCache::HasAnyForClass(player);

        if (hasTemplates)
        {
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Do you have Specifications and Gear Templates?", GOSSIP_SENDER_MAIN, SHOW_SPECS);
        }
        // Only admin can add and delete templates.
        if (player->GetSession()->GetSecurity() >= SEC_ADMINISTRATOR)
        {
            //player->ADD_GOSSIP_ITEM_EXTENDED(GOSSIP_ICON_INTERACT_1, "<ADMIN> Save my current Gear & Talents.", GOSSIP_SENDER_MAIN, SAVE_TEMP, "Save as...", true);
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "<ADMIN> Save my current Gear & Talents.", GOSSIP_SENDER_MAIN, SAVE_TEMP);
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "<ADMIN> Delete a Template.", GOSSIP_SENDER_MAIN, DELETE_TEMP);
            //player->ADD_GOSSIP_ITEM               (GOSSIP_ICON_INTERACT_1,    "<ADMIN> player_factionchange_items.",      GOSSIP_SENDER_MAIN, FIX_DB);

        }
    }
    // Entry-unabhaengig: TemplateNPC zeigt Menue gemaess Spielerklasse (nicht per Creature-Entry).
    player->SEND_GOSSIP_MENU(player->GetGossipTextId(creature), creature->GetObjectGuid());
    return true;
}


// Forward declaration (needed for SELECT_SPEC_BASE routing)
static bool GossipSpecs_Template(Player* player, Creature* creature, uint32 uiAction);

bool GossipStart_TemplateNPC(Player* player, Creature* creature, uint32 uiAction)
{
    // Spec-Select Routing: Action-Range (SELECT_SPEC_BASE + TabId) auf GossipSpecs_Template umleiten
    if (uiAction >= SELECT_SPEC_BASE)
    {
        uint32 tabId = uiAction - SELECT_SPEC_BASE;
        return GossipSpecs_Template(player, creature, tabId);
    }

    if (uiAction == GOSSIP_OPTION_STABLEPET)
    {
        player->GetSession()->SendStablePet(creature->GetGUID());
    }
    else if (uiAction == GOSSIP_OPTION_VENDOR)
    {
        player->GetSession()->SendListInventory(creature->GetGUID());
    }
    else if (uiAction == GOSSIP_OPTION_UNLEARNPETSKILLS)
    {
        Pet* pet = player->GetPet();

        if (pet)
        {
            WorldPacket data(SMSG_PET_UNLEARN_CONFIRM /*guarded*/, (8));
            data << ObjectGuid(pet->GetObjectGuid());
            player->GetSession()->SendPacket(&data);
        }
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == GET_LVL_60)
    {
        // Level/Lernen Submenu (AutoTrainer-Logik, GREEN-only)
        player->PlayerTalkClass->ClearMenus();

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Alles lernen (aktuelles Level)",            GOSSIP_SENDER_MAIN, LEVEL_LEARN_CURRENT);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "+10 Level und alles lernen",                GOSSIP_SENDER_MAIN, LEVEL_LEARN_PLUS_10);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Bis naechstes 10er-Level und alles lernen", GOSSIP_SENDER_MAIN, LEVEL_LEARN_NEXT_TEN);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Level 60 setzen und alles lernen",          GOSSIP_SENDER_MAIN, LEVEL_LEARN_TO_60);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT,    "Abbrechen",                                 GOSSIP_SENDER_MAIN, LEVEL_LEARN_CANCEL);

        player->SEND_GOSSIP_MENU(DEFAULT_GOSSIP_MESSAGE, creature->GetObjectGuid());
    }
    else if (uiAction == LEVEL_LEARN_CANCEL)
    {
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == LEVEL_LEARN_CURRENT)
    {
        uint32 learned = AutoLearnCore::LearnAllAtCurrentLevel(player, creature);
        (void)learned;
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == LEVEL_LEARN_PLUS_10)
    {
        uint32 learned = AutoLearnCore::LevelPlusTenAndLearnAll(player, creature);
        (void)learned;
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == LEVEL_LEARN_NEXT_TEN)
    {
        uint32 learned = AutoLearnCore::LevelToNextTenAndLearnAll(player, creature);
        (void)learned;
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == LEVEL_LEARN_TO_60)
    {
        uint32 learned = AutoLearnCore::LevelToAndLearnAll(player, creature, 60);
        (void)learned;
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == SHOW_PROF_MENU)
    {
        player->PlayerTalkClass->ClearMenus();

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "First Aid (learn + all recipes)",   GOSSIP_SENDER_MAIN, PROF_LEARN_FIRST_AID);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Cooking (learn + all recipes)",    GOSSIP_SENDER_MAIN, PROF_LEARN_COOKING);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Fishing (learn + all recipes)",    GOSSIP_SENDER_MAIN, PROF_LEARN_FISHING);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Alchemy (learn + all recipes)",        GOSSIP_SENDER_MAIN, PROF_LEARN_ALCHEMY);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Blacksmithing (learn + all recipes)", GOSSIP_SENDER_MAIN, PROF_LEARN_BLACKSMITH);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Enchanting (learn + all recipes)",    GOSSIP_SENDER_MAIN, PROF_LEARN_ENCHANTING);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Engineering (learn + all recipes)",   GOSSIP_SENDER_MAIN, PROF_LEARN_ENGINEERING);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Herbalism (learn + all recipes)",     GOSSIP_SENDER_MAIN, PROF_LEARN_HERBALISM);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Leatherworking (learn + all recipes)",GOSSIP_SENDER_MAIN, PROF_LEARN_LEATHERWORK);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Mining (learn + all recipes)",        GOSSIP_SENDER_MAIN, PROF_LEARN_MINING);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Skinning (learn + all recipes)",      GOSSIP_SENDER_MAIN, PROF_LEARN_SKINNING);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Tailoring (learn + all recipes)",     GOSSIP_SENDER_MAIN, PROF_LEARN_TAILORING);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, PROF_MENU_BACK);
        player->SEND_GOSSIP_MENU(DEFAULT_GOSSIP_MESSAGE, creature->GetObjectGuid());
    }
    else if (uiAction == PROF_MENU_BACK)
    {
        // Zurueck ins Hauptmenu
        return GossipHello_TemplateNPC(player, creature);
    }
    else if (uiAction == PROF_LEARN_FIRST_AID)
    {
        LearnProfession(player, creature, SKILL_FIRST_AID);
        LearnAllRecipesProfession(player, SKILL_FIRST_AID);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_COOKING)
    {
        LearnProfession(player, creature, SKILL_COOKING);
        LearnAllRecipesProfession(player, SKILL_COOKING);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_FISHING)
    {
        LearnProfession(player, creature, SKILL_FISHING);
        LearnAllRecipesProfession(player, SKILL_FISHING);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_ALCHEMY)
    {
        LearnProfession(player, creature, SKILL_ALCHEMY);
        LearnAllRecipesProfession(player, SKILL_ALCHEMY);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_BLACKSMITH)
    {
        LearnProfession(player, creature, SKILL_BLACKSMITHING);
        LearnAllRecipesProfession(player, SKILL_BLACKSMITHING);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_ENCHANTING)
    {
        LearnProfession(player, creature, SKILL_ENCHANTING);
        LearnAllRecipesProfession(player, SKILL_ENCHANTING);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_ENGINEERING)
    {
        LearnProfession(player, creature, SKILL_ENGINEERING);
        LearnAllRecipesProfession(player, SKILL_ENGINEERING);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_HERBALISM)
    {
        LearnProfession(player, creature, SKILL_HERBALISM);
        LearnAllRecipesProfession(player, SKILL_HERBALISM);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_LEATHERWORK)
    {
        LearnProfession(player, creature, SKILL_LEATHERWORKING);
        LearnAllRecipesProfession(player, SKILL_LEATHERWORKING);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_MINING)
    {
        LearnProfession(player, creature, SKILL_MINING);
        LearnAllRecipesProfession(player, SKILL_MINING);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_SKINNING)
    {
        LearnProfession(player, creature, SKILL_SKINNING);
        LearnAllRecipesProfession(player, SKILL_SKINNING);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == PROF_LEARN_TAILORING)
    {
        LearnProfession(player, creature, SKILL_TAILORING);
        LearnAllRecipesProfession(player, SKILL_TAILORING);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == SAVE_PET)
    {
        ExportHunterPetToDB(player);
        SaveHunterPetSpellsToDB(player);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == 800000008)
    {
        LearnPetSpellsFromDB(player);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == MAKE_PET_HAPPY)
    {
        if (TemplateNpc_GetPlayerClassId(player) == CLASS_HUNTER && player->GetPet()) {

            player->GetPet()->SetPower(POWER_HAPPINESS, 1048000);
            player->GetPet()->SetLoyaltyLevel(BEST_FRIEND);
            player->GetPet()->InitStatsForLevel(TemplateNpc_GetPlayerLevel(player));
            player->GetPet()->UpdateAllStats();
        }
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == TEACH_WARLOCK_PET)
    {
        LearnWarlockPetSpells(player);

        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == RESET_TALENTS)
    {
        player->ResetTalents(true);
        //player->UpdateFreeTalentPoints(false);
        creature->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->CastSpell(player, COOL_VISUAL_SPELL_3, true);

        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == UPGRADE_TALENTS)
    {
        LearnAllSpells(player, creature);
        creature->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->CastSpell(player, COOL_VISUAL_SPELL_3, true);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == RESET_COOLDOWNS_AND_CHARGES)
    {
        for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; i++)
        {
            if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (const ItemPrototype* pProto = item->GetProto())
                {
                    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
                    {
                        if (pProto->Spells[i].SpellId)
                        {
                            if (pProto->Spells[i].SpellCharges)
                            {
                                int32 charges = pProto->Spells[i].SpellCharges;
                                item->SetSpellCharges(i, charges);
                            }
                        }
                    }
                }
            }
        }
        player->RemoveAllCooldowns();
        creature->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->CastSpell(player, COOL_VISUAL_SPELL_3, true);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == ALL_BUFFS)
    {
        FullBuffed(player);
        creature->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->CastSpell(player, COOL_VISUAL_SPELL_3, true);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == FIX_DB)
    {
        perform_npc_vendor_template();
        //perform_player_factionchange_items();
        creature->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->CastSpell(player, COOL_VISUAL_SPELL_3, true);
        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == DELETE_GEAR)
    {
        for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; i++)
        {
            if (i == EQUIPMENT_SLOT_TABARD || i == EQUIPMENT_SLOT_BODY)
                continue;

            if (Item* haveItemEquipped = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
            {
                if (haveItemEquipped)
                {
                    player->DestroyItem(INVENTORY_SLOT_BAG_0, i, true);
                }
            }
        }
        player->SaveToDB();

        creature->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->CastSpell(player, COOL_VISUAL_SPELL_3, true);

        player->CLOSE_GOSSIP_MENU();
    }
    else if (uiAction == SHOW_SPECS)
    {
        const std::vector<uint32>* tabs = TemplateNpcCache::GetTabsForClass(player);
        const char* SpecText = "";

        if (tabs)
        {
            for (uint32 SpecID : *tabs)
            {
                // Patch-Filter: pro Spec die kleinste Patch-Anforderung aller Templates in diesem Tab verwenden
                uint32 patchMin = 0;
                const std::vector<uint32>* tempIds = TemplateNpcCache::GetTempIdsForTab(player, SpecID);
                if (tempIds && !tempIds->empty())
                {
                    patchMin = 255;
                    for (uint32 tid : *tempIds)
                    {
                        uint32 p = TemplateNpcCache::GetPatch(player, tid);
                        if (p < patchMin) patchMin = p;
                    }
                    if (patchMin == 255) patchMin = 0;
                }

                if (sWorld.GetWowPatch() >= patchMin)
                {
                    switch (SpecID)
                    {
                    case WarriorProtection:
                        SpecText = "Protection";
                        break;
                    case WarriorFury:
                        SpecText = "Fury";
                        break;
                    case WarriorArms:
                        SpecText = "Arms";
                        break;
                    case WarlockDemonology:
                        SpecText = "Demonology";
                        break;
                    case WarlockDestruction:
                        SpecText = "Destruction";
                        break;
                    case WarlockAffliction:
                        SpecText = "Affliction";
                        break;
                    case ShamanRestoration:
                        SpecText = "Restoration";
                        break;
                    case ShamanEnhancement:
                        SpecText = "Enhancement";
                        break;
                    case ShamanElementalCombat:
                        SpecText = "Elemental";
                        break;
                    case RogueSubtlety:
                        SpecText = "Subtlety";
                        break;
                    case RogueCombat:
                        SpecText = "Combat";
                        break;
                    case RogueAssassination:
                        SpecText = "Assassination";
                        break;
                    case PriestShadow:
                        SpecText = "Shadow";
                        break;
                    case PriestHoly:
                        SpecText = "Holy";
                        break;
                    case PriestDiscipline:
                        SpecText = "Discipline";
                        break;
                    case PaladinProtection:
                        SpecText = "Protection";
                        break;
                    case PaladinHoly:
                        SpecText = "Holy";
                        break;
                    case PaladinRetribution:
                        SpecText = "Retribution";
                        break;
                    case MageFrost:
                        SpecText = "Frost";
                        break;
                    case MageFire:
                        SpecText = "Fire";
                        break;
                    case MageArcane:
                        SpecText = "Arcane";
                        break;
                    case HunterSurvival:
                        SpecText = "Survival";
                        break;
                    case HunterMarksmanship:
                        SpecText = "Marksmanship";
                        break;
                    case HunterBeastMastery:
                        SpecText = "Beast Mastery";
                        break;
                    case DruidRestoration:
                        SpecText = "Restoration";
                        break;
                    case DruidFeralCombat:
                        SpecText = "Feral Combat";
                        break;
                    case DruidBalance:
                        SpecText = "Balance";
                        break;
                    }
                    std::ostringstream ss;
                    ss << "Choose " << SpecText;

                    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, ss.str().c_str(), GOSSIP_SENDER_MAIN, SELECT_SPEC_BASE + SpecID);
                }
            }
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, 0);
        player->SEND_GOSSIP_MENU(1, creature->GetObjectGuid());
    }
    
    else if (uiAction == DELETE_TEMP)
    {
        const std::vector<uint32>* tabs = TemplateNpcCache::GetTabsForClass(player);

        if (tabs)
        {
            std::unordered_set<uint32> seen;
            for (uint32 tabId : *tabs)
            {
                const std::vector<uint32>* tempIds = TemplateNpcCache::GetTempIdsForTab(player, tabId);
                if (!tempIds) continue;

                for (uint32 temp_id : *tempIds)
                {
                    if (seen.count(temp_id))
                        continue;
                    seen.insert(temp_id);

                    const std::string* gt = TemplateNpcCache::GetGossipText(player, temp_id);
                    const char* SpecText = gt ? gt->c_str() : "Template";

                    std::ostringstream ss;
                    ss << "Delete " << SpecText;

                    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, ss.str().c_str(), GOSSIP_SENDER_TEMP_DELETE, temp_id);
                }
            }
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, 0);
        player->SEND_GOSSIP_MENU(1, creature->GetObjectGuid());
    }

    else if (uiAction == SAVE_TEMP)
    {

        if (player->GetFreeTalentPoints() > 0)
        {
            player->GetSession()->SendAreaTriggerMessage("You have unspend talent points. Please spend all your talent points.");
            return false;
        }

        std::string name = TemplateExportNameString(player).c_str();

        if (name.length() > 50)
        {
            player->GetSession()->SendNotification("Name too long.");
            player->CLOSE_GOSSIP_MENU();
            return false;
        }
        else
        {
            CharacterDatabase.escape_string(name);
            ExtractGearTemplateToDB(player, name);
            ExportCharacterTalentsToDB(player, name);
            creature->CastSpell(player, COOL_VISUAL_SPELL, true);
            player->CastSpell(player, COOL_VISUAL_SPELL_3, true);
        }
        player->CLOSE_GOSSIP_MENU();

        return true;
    }
    else if (uiAction == SHOW_PETS) // List Hunter Pets
    {

        auto result = CharacterDatabase.PQuery("SELECT * FROM template_pets ORDER BY AttackSpeed ASC;");

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                const char* petname = fields[0].GetString();
                uint32 petentry = fields[1].GetUInt32();
                uint32 petfamily = fields[2].GetUInt32();
                float attackspeed = fields[3].GetFloat();
                //attackspeed = (float)((int)(attackspeed / 1000.f)) / 1.f;
                attackspeed = (0.0010 * attackspeed);

                std::ostringstream ss;
                ss << GetPetFamily(petfamily) << " -> " << petname << " -> " << attackspeed << " Attack Speed";

                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, ss.str().c_str(), GOSSIP_SENDER_PET_SELECT, petentry);

            } while (result->NextRow());
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, 0);
        player->SEND_GOSSIP_MENU(1, creature->GetObjectGuid());
    }

    else if (uiAction == 0)

        GossipHello_TemplateNPC(player, creature);

    return true;
}

bool GossipSelect_HunterPet(Player* player, Creature* creature, uint32 uiAction)
{
    CreateHunterPet(player, creature, uiAction);
    LearnPetSpellsFromDB(player);
    player->CLOSE_GOSSIP_MENU();
    return true;
}

bool GossipConfirm_Template(Player* player, Creature* creature, uint32 uiAction)
{
    player->PlayerTalkClass->ClearMenus();
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Ok let's do it!", GOSSIP_SENDER_TEMP_EQUIP, uiAction);
    player->SEND_GOSSIP_MENU(600007, creature->GetObjectGuid());

    return true;
}

bool GossipSpecs_Template(Player* player, Creature* creature, uint32 uiAction)
{
    if (!player || !creature)
        return true;

    const std::vector<uint32>* tempIds = TemplateNpcCache::GetTempIdsForTab(player, uiAction);
    if (tempIds)
    {
        std::vector<std::pair<uint32, uint32> > ordered;
        ordered.reserve(tempIds->size());

        for (uint32 tid : *tempIds)
            ordered.push_back(std::make_pair(uint32(TemplateNpcCache::GetPatch(player, tid)), tid));

        std::sort(ordered.begin(), ordered.end(), [](const std::pair<uint32, uint32>& a, const std::pair<uint32, uint32>& b)
        {
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });

        for (const auto& it : ordered)
        {
            uint32 Patch = it.first;
            uint32 temp_id = it.second;

            if (sWorld.GetWowPatch() < Patch)
                continue;

            const std::string* gt = TemplateNpcCache::GetGossipText(player, temp_id);
            const char* SpecText = gt ? gt->c_str() : "Template";

            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_BATTLE, SpecText, GOSSIP_SENDER_TEMP_CONFIRM, temp_id);
        }
    }

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, SHOW_SPECS);
    player->SEND_GOSSIP_MENU(1, creature->GetObjectGuid());

    return true;
}

bool GossipEquip_Template(Player* player, Creature* creature, uint32 uiAction)
{
    if (!player || !creature)
        return true;

    const TemplateNpcCache::TemplateData* td = TemplateNpcCache::GetTemplate(player, uiAction);
    if (td)
    {
        DeleteEquippedGear(player);
        ApplyTemplateToPlayer(player, creature, uiAction);
        player->CLOSE_GOSSIP_MENU();
    }

    return true;
}

bool GossipDelete_Template(Player* player, Creature* creature, uint32 uiAction)
{
    CharacterDatabase.PExecute("DELETE FROM template_npc_gear WHERE temp_id = '%u'", uiAction);
    CharacterDatabase.PExecute("DELETE FROM template_npc_talents WHERE temp_id = '%u'", uiAction);

    // Cache aktualisieren
    TemplateNpcCache::LoadFromDB();

    player->GetSession()->SendAreaTriggerMessage("Successfuly deleted");

    creature->CastSpell(player, COOL_VISUAL_SPELL, true);
    player->CastSpell(player, COOL_VISUAL_SPELL_3, true);

    player->CLOSE_GOSSIP_MENU();

    return true;
}

bool GossipSave_Template(Player* player, Creature* creature, uint32 sender, uint32 action, const char* code)
{
    std::string name = code;
    static const char* allowedcharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz _-/().,1234567890";
    if (!name.length() || name.find_first_not_of(allowedcharacters) != std::string::npos)
    {
        player->GetSession()->SendNotification("invalid template name");
        player->CLOSE_GOSSIP_MENU();
        return false;
    }
    else
    {
        player->SaveToDB();
        ExtractGearTemplateToDB(player, name);
        ExportCharacterTalentsToDB(player, name);
        creature->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->CastSpell(player, COOL_VISUAL_SPELL_3, true);
    }
    player->CLOSE_GOSSIP_MENU();

    return true;
}

bool GossipSelect_TemplateNPC(Player* player, Creature* creature, uint32 uiSender, uint32 uiAction)
{
    switch (uiSender)
    {
    case GOSSIP_SENDER_MAIN:
        GossipStart_TemplateNPC(player, creature, uiAction);
        break;
    case GOSSIP_SENDER_TEMP_DELETE:
        GossipDelete_Template(player, creature, uiAction);
        break;
    case GOSSIP_SENDER_TEMP_CONFIRM:
        GossipConfirm_Template(player, creature, uiAction);
        break;
    case GOSSIP_SENDER_TEMP_EQUIP:
        GossipEquip_Template(player, creature, uiAction);
        break;
    case GOSSIP_SENDER_TEMP_SPECS:
        GossipSpecs_Template(player, creature, uiAction);
        break;
    case GOSSIP_SENDER_PET_SELECT:
        GossipSelect_HunterPet(player, creature, uiAction);
        break;
    }
    return true;
}

struct TemplateNPCAI : public ScriptedAI
{
    TemplateNPCAI(Creature* pCreature) : ScriptedAI(pCreature)
    {
        Reset();
        me->SetFly(true);
        me->SetLevitate(true);
    }

    void Reset()
    {
        if (me->GetEntry() == 600003)
            me->SetLevitate(true);
            me->SetFly(true);
    }
};

CreatureAI* GetAITemplateNPC(Creature* pCreature)
{
    return new TemplateNPCAI(pCreature);
}

void AddSC_TemplateNPC()
{
    // TemplateNPC Cache beim Script-Init laden (Worldstart)
    TemplateNpcCache::LoadFromDB();

Script* pNewScript;
    pNewScript = new Script;
    pNewScript->Name = "TemplateNPC";
    pNewScript->GetAI = &GetAITemplateNPC;
    pNewScript->pGossipHello = &GossipHello_TemplateNPC;
    pNewScript->pGossipSelect = &GossipSelect_TemplateNPC;
    pNewScript->pGossipSelectWithCode = &GossipSave_Template;
    pNewScript->RegisterSelf();
}

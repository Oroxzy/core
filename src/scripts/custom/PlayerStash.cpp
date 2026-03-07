
#include "scriptPCH.h"
#include "BattleGroundAV.h"
#include "BattleGroundWS.h"

#include "Totem.h"
#include "WorldPacket.h"
#include "Log.h"
#include "Group.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "DBCStores.h"
#include "CreatureAI.h"
#include "InstanceData.h"

#include <ctime>

#define GOSSIP_SENDER_GEAR_DELETE       10000001
#define GOSSIP_SENDER_TALENTS_DELETE    10000002
#define GOSSIP_SENDER_GEAR_CONFIRM      10000003
#define GOSSIP_SENDER_TALENT_CONFIRM    10000004
#define GOSSIP_SENDER_GEAR_USE          10000007
#define GOSSIP_SENDER_TALENT_USE        10000008

// item defines
#define BOTTOMLESS_BAG      14156
// sound defines
#define ChestOpen           1277
#define ChestClose          1278
#define SpellFizzleHoly     1430
#define Click               116
// generic defines
#define SHOWBANK            GOSSIP_ACTION_INFO_DEF+1
#define SHOWMAIL            GOSSIP_ACTION_INFO_DEF+2
#define SAVE_TALENTS        GOSSIP_ACTION_INFO_DEF+3
#define SAVE_GEAR           GOSSIP_ACTION_INFO_DEF+4
#define SHOW_TALENTS        GOSSIP_ACTION_INFO_DEF+5
#define SHOW_GEAR           GOSSIP_ACTION_INFO_DEF+6
#define DELETE_GEAR         GOSSIP_ACTION_INFO_DEF+7
#define DELETE_TALENTS      GOSSIP_ACTION_INFO_DEF+8
#define DELETE_GEAR_OK      GOSSIP_ACTION_INFO_DEF+9
#define DELETE_TALENTS_OK   GOSSIP_ACTION_INFO_DEF+10
#define APPLY_GEAR          GOSSIP_ACTION_INFO_DEF+11
#define APPLY_TALENTS       GOSSIP_ACTION_INFO_DEF+12
#define BINDHOME       GOSSIP_ACTION_INFO_DEF+13
// spell defines
#define COOL_VISUAL_SPELL   25100

std::string StashGetClassString(Player* player)
{
    switch (player->getClass())
    {
    case CLASS_PRIEST:
        return "Priest";
        break;
    case CLASS_PALADIN:
        return "Paladin";
        break;
    case CLASS_WARRIOR:
        return "Warrior";
        break;
    case CLASS_MAGE:
        return "Mage";
        break;
    case CLASS_WARLOCK:
        return "Warlock";
        break;
    case CLASS_SHAMAN:
        return "Shaman";
        break;
    case CLASS_DRUID:
        return "Druid";
        break;
    case CLASS_HUNTER:
        return "Hunter";
        break;
    case CLASS_ROGUE:
        return "Rogue";
        break;
    default:
        break;
    }
    return ""; // Fix warning, this should never happen
}


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

std::string TalentsExportNameString(Player* player)
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

        if (talentTabInfo->ClassMask != player->getClassMask())
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

    //WARRIOR
    if (WarriorProtectionPoints > WarriorFuryPoints && WarriorProtectionPoints > WarriorArmsPoints)
    {
        PointsStream << "(Protection " << WarriorArmsPoints << "/" << WarriorFuryPoints << "/" << WarriorProtectionPoints << ")";
    }
    else if (WarriorFuryPoints > WarriorProtectionPoints && WarriorFuryPoints > WarriorArmsPoints)
    {
        PointsStream << "(Fury " << WarriorArmsPoints << "/" << WarriorFuryPoints << "/" << WarriorProtectionPoints << ")";
    }
    else if (WarriorArmsPoints > WarriorFuryPoints && WarriorArmsPoints > WarriorProtectionPoints)
    {
        PointsStream << "(Arms " << WarriorArmsPoints << "/" << WarriorFuryPoints << "/" << WarriorProtectionPoints << ")";
    }

    //WARLOCK
    if (WarlockAfflictionPoints > WarlockDemonologyPoints && WarlockAfflictionPoints > WarlockDestructionPoints)
    {
        PointsStream << "(Affliction " << WarlockAfflictionPoints << "/" << WarlockDemonologyPoints << "/" << WarlockDestructionPoints << ")";
    }
    else if (WarlockDemonologyPoints > WarlockAfflictionPoints && WarlockDemonologyPoints > WarlockDestructionPoints)
    {
        PointsStream << "(Demonology" << WarlockAfflictionPoints << "/" << WarlockDemonologyPoints << "/" << WarlockDestructionPoints << ")";
    }
    else if (WarlockDestructionPoints > WarlockDemonologyPoints && WarlockDestructionPoints > WarlockAfflictionPoints)
    {
        PointsStream << "(Destruction " << WarlockAfflictionPoints << "/" << WarlockDemonologyPoints << "/" << WarlockDestructionPoints << ")";
    }

    //SHAMAN
    if (ShamanElementalCombatPoints > ShamanEnhancementPoints && ShamanElementalCombatPoints > ShamanRestorationPoints)
    {
        PointsStream << "(Elemental " << ShamanElementalCombatPoints << "/" << ShamanEnhancementPoints << "/" << ShamanRestorationPoints << ")";
    }
    else if (ShamanEnhancementPoints > ShamanElementalCombatPoints && ShamanEnhancementPoints > ShamanRestorationPoints)
    {
        PointsStream << "(Enhancement " << ShamanElementalCombatPoints << "/" << ShamanEnhancementPoints << "/" << ShamanRestorationPoints << ")";
    }
    else if (ShamanRestorationPoints > ShamanEnhancementPoints && ShamanRestorationPoints > ShamanElementalCombatPoints)
    {
        PointsStream << "(Restoration " << ShamanElementalCombatPoints << "/" << ShamanEnhancementPoints << "/" << ShamanRestorationPoints << ")";
    }

    //ROGUE
    if (RogueAssassinationPoints > RogueCombatPoints && RogueAssassinationPoints > RogueSubtletyPoints)
    {
        PointsStream << "(Assassination " << RogueAssassinationPoints << "/" << RogueCombatPoints << "/" << RogueSubtletyPoints << ")";
    }
    else if (RogueCombatPoints > RogueAssassinationPoints && RogueCombatPoints > RogueSubtletyPoints)
    {
        PointsStream << "(Combat " << RogueAssassinationPoints << "/" << RogueCombatPoints << "/" << RogueSubtletyPoints << ")";
    }
    else if (RogueSubtletyPoints > RogueCombatPoints && RogueSubtletyPoints > RogueAssassinationPoints)
    {
        PointsStream << "(Subtlety " << RogueAssassinationPoints << "/" << RogueCombatPoints << "/" << RogueSubtletyPoints << ")";
    }

    //PRIEST
    if (PriestDisciplinePoints > PriestHolyPoints && PriestDisciplinePoints > PriestShadowPoints)
    {
        PointsStream << "(Discipline " << PriestDisciplinePoints << "/" << PriestHolyPoints << "/" << PriestShadowPoints << ")";
    }
    else if (PriestHolyPoints > PriestDisciplinePoints && PriestHolyPoints > PriestShadowPoints)
    {
        PointsStream << "(Holy " << PriestDisciplinePoints << "/" << PriestHolyPoints << "/" << PriestShadowPoints << ")";
    }
    else if (PriestShadowPoints > PriestDisciplinePoints && PriestShadowPoints > PriestHolyPoints)
    {
        PointsStream << "(Shadow " << PriestDisciplinePoints << "/" << PriestHolyPoints << "/" << PriestShadowPoints << ")";
    }

    //PALADIN
    if (PaladinHolyPoints > PaladinProtectionPoints && PaladinHolyPoints > PaladinRetributionPoints)
    {
        PointsStream << "(Holy " << PaladinHolyPoints << "/" << PaladinProtectionPoints << "/" << PaladinRetributionPoints << ")";
    }
    else if (PaladinProtectionPoints > PaladinHolyPoints && PaladinProtectionPoints > PaladinRetributionPoints)
    {
        PointsStream << "(Protection " << PaladinHolyPoints << "/" << PaladinProtectionPoints << "/" << PaladinRetributionPoints << ")";
    }
    else if (PaladinRetributionPoints > PaladinHolyPoints && PaladinRetributionPoints > PaladinProtectionPoints)
    {
        PointsStream << "(Retribution " << PaladinHolyPoints << "/" << PaladinProtectionPoints << "/" << PaladinRetributionPoints << ")";
    }

    //MAGE
    if (MageArcanePoints > MageFirePoints && MageArcanePoints > MageFrostPoints)
    {
        PointsStream << "(Arcane " << MageArcanePoints << "/" << MageFirePoints << "/" << MageFrostPoints << ")";
    }
    else if (MageFirePoints > MageArcanePoints && MageFirePoints > MageFrostPoints)
    {
        PointsStream << "(Fire " << MageArcanePoints << "/" << MageFirePoints << "/" << MageFrostPoints << ")";
    }
    else if (MageFrostPoints > MageArcanePoints && MageFrostPoints > MageFirePoints)
    {
        PointsStream << "(Frost " << MageArcanePoints << "/" << MageFirePoints << "/" << MageFrostPoints << ")";
    }

    //HUNTER
    if (HunterBeastMasteryPoints > HunterMarksmanshipPoints && HunterBeastMasteryPoints > HunterSurvivalPoints)
    {
        PointsStream << "(Beast Mastery " << HunterBeastMasteryPoints << "/" << HunterMarksmanshipPoints << "/" << HunterSurvivalPoints << ")";
    }
    else if (HunterMarksmanshipPoints > HunterBeastMasteryPoints && HunterMarksmanshipPoints > HunterSurvivalPoints)
    {
        PointsStream << "(Marksmanship " << HunterBeastMasteryPoints << "/" << HunterMarksmanshipPoints << "/" << HunterSurvivalPoints << ")";
    }
    else if (HunterSurvivalPoints > HunterBeastMasteryPoints && HunterSurvivalPoints > HunterMarksmanshipPoints)
    {
        PointsStream << "(Survival " << HunterBeastMasteryPoints << "/" << HunterMarksmanshipPoints << "/" << HunterSurvivalPoints << ")";
    }

    //DRUID
    if (DruidBalancePoints > DruidFeralCombatPoints && DruidBalancePoints > DruidRestorationPoints)
    {
        PointsStream << "(Balance " << DruidBalancePoints << "/" << DruidFeralCombatPoints << "/" << DruidRestorationPoints << ")";
    }
    else if (DruidFeralCombatPoints > DruidBalancePoints && DruidFeralCombatPoints > DruidRestorationPoints)
    {
        PointsStream << "(Feral Combat " << DruidBalancePoints << "/" << DruidFeralCombatPoints << "/" << DruidRestorationPoints << ")";
    }
    else if (DruidRestorationPoints > DruidBalancePoints && DruidRestorationPoints > DruidFeralCombatPoints)
    {
        PointsStream << "(Restoration " << DruidBalancePoints << "/" << DruidFeralCombatPoints << "/" << DruidRestorationPoints << ")";
    }

    return (PointsStream.str().c_str());
}

uint32 GetTalentID()
{
    QueryResult* TalentIdResult = CharacterDatabase.PQuery("SELECT MAX(temp_id) FROM player_stash_talents");

    uint32 TalentId;

    if (TalentIdResult)
    {
        Field* fields = TalentIdResult->Fetch();
        TalentId = fields[0].GetInt32();
		delete TalentIdResult;
    }
    if (TalentId)
        return TalentId + 1;
    else
        return 50;
}

uint32 GetGearID()
{
    QueryResult* GearIdResult = CharacterDatabase.PQuery("SELECT MAX(temp_id) FROM player_stash_gear");

    uint32 GearId;

    if (GearIdResult)
    {
        Field* fields = GearIdResult->Fetch();
        GearId = fields[0].GetInt32();
		delete GearIdResult;
    }
    if (GearId)
        return GearId + 1;
    else
        return 50;
}

void AddBankBags(Player* player)
{
    for (int i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
    {
        if (Bag* pBag = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            if (!pBag)
                player->EquipNewItem(i, BOTTOMLESS_BAG, true);
        }
        else
            player->EquipNewItem(i, BOTTOMLESS_BAG, true);
    }
}

void StashApplyBonus(Player* player, Item* item, EnchantmentSlot slot, uint32 bonusEntry, uint32 duration, uint32 charges)
{
    if (!item)
        return;

    if (!bonusEntry || bonusEntry == 0)
        return;

    player->ApplyEnchantment(item, slot, false);
    item->SetEnchantment(slot, bonusEntry, duration, charges);
    player->ApplyEnchantment(item, slot, true);
}

void ExtractGearToDB(Player* player, std::string& gossipTempText)
{
    uint32 TempID = GetGearID();
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

            QueryResult *getpatch = WorldDatabase.PQuery("SELECT patch FROM item_template "
                "WHERE entry = '%u'", itemId);

            if (getpatch)
            {
                Field* fields = getpatch->Fetch();
                uint32 newpatch = fields[0].GetInt32();
                if (patch < newpatch)
                    patch = newpatch;

				delete getpatch;
            }

            CharacterDatabase.PExecute("INSERT INTO player_stash_gear (`char_guid`, `temp_id`, `gossip_text`, `item_slot`, `item_entry`, `item_enchant`, `patch`) VALUES ('%u', '%u', '%s', '%u', '%u', '%u', '%u');"
                , player->GetGUID(), TempID, gossipTempText.c_str(), equippedItem->GetSlot(), equippedItem->GetEntry(), equippedItem->GetEnchantmentId(PERM_ENCHANTMENT_SLOT), patch);
        }

        CharacterDatabase.PExecute("UPDATE player_stash_gear SET patch = '%u' WHERE temp_id='%u'", patch, TempID);
    }
    player->PlayDirectSound(Click, player);
}

void StashLearnTalentsFromDB(Player* player, uint32 temp_id)
{
    QueryResult *select = CharacterDatabase.PQuery("SELECT talent_id FROM player_stash_talents WHERE char_guid = '%u' AND "
        "temp_id = '%u';", player->GetGUID(), temp_id);

    if (select)
    {
        do
        {
            Field* fields = select->Fetch();
            uint32 talentID = fields[0].GetUInt32();

            player->LearnSpell(talentID, false, true);
        } while (select->NextRow());
		delete select;
    }
}

void StashDeleteEquippedGear(Player* player)
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

uint32 CountStoredGear(Player* player)
{
    uint32 inv_count = 0;
    QueryResult *result = CharacterDatabase.PQuery("SELECT char_guid FROM player_stash_gear WHERE char_guid='%u' GROUP BY temp_id", player->GetGUID());
    if (result)
    {
        do {
            inv_count++;
        } while (result->NextRow());
        delete result;
    }
    return inv_count;
}

uint32 CountStoredTalents(Player* player)
{
    uint32 inv_count = 0;
    QueryResult *result = CharacterDatabase.PQuery("SELECT char_guid FROM player_stash_talents WHERE char_guid='%u' GROUP BY temp_id", player->GetGUID());
    if (result)
    {
        do {
            inv_count++;
        } while (result->NextRow());
        delete result;
    }
    return inv_count;
}

void StashEquipItemsFromDB(Player* player, uint32 temp_id)
{
    if (!temp_id)
    {
        ChatHandler(player).PSendSysMessage("no temp id.");
        return;
    }

    for (uint8 equipmentSlot = EQUIPMENT_SLOT_START; equipmentSlot < EQUIPMENT_SLOT_END; ++equipmentSlot)
    {
        if (equipmentSlot == EQUIPMENT_SLOT_TABARD || equipmentSlot == EQUIPMENT_SLOT_BODY)
            continue;

        std::unique_ptr<QueryResult> player_stash_gear(CharacterDatabase.PQuery("SELECT item_entry, item_enchant "
            "FROM player_stash_gear WHERE char_guid = '%u' AND temp_id = '%u' AND item_slot = '%u';", player->GetGUID(), temp_id, equipmentSlot));

        if (!player_stash_gear)
            continue;

        Field* fields = player_stash_gear->Fetch();
        uint32 itemEntry = fields[0].GetUInt32();
        uint32 enchant = fields[1].GetUInt32();

        ItemPrototype const* item_proto = ObjectMgr::GetItemPrototype(itemEntry);

        if (!item_proto)
            continue;

        // Check if we need some Reputation for the Item and set it to Exalted.
        if (item_proto->RequiredReputationFaction && item_proto->RequiredReputationRank > 0)
        {
            if (ReputationRank(item_proto->RequiredReputationRank) > player->GetReputationRank(item_proto->RequiredReputationFaction))
                player->GetReputationMgr().ModifyReputation(sObjectMgr.GetFactionEntry(item_proto->RequiredReputationFaction), 85000);
        }

        player->EquipNewItem(equipmentSlot, itemEntry, true); // No items found in player_factionchange_items equip itemEntry.

        // Apply Enchants
        StashApplyBonus(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, equipmentSlot), PERM_ENCHANTMENT_SLOT, enchant, 0, 0);
    }
}

void ExtractTalentsToDB(Player* player, std::string gossipTempText)
{
    static SqlStatementID insTalents;
    uint32 TempID = GetTalentID();

    SqlStatement stmtIns = CharacterDatabase.CreateStatement(insTalents, "INSERT INTO player_stash_talents (char_guid, temp_id, gossip_text, talent_id, rank) VALUES (?, ?, ?, ?, ?)");

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

        if ((player->getClassMask() & talentTabInfo->ClassMask) == 0)
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
                    stmtIns.PExecute(player->GetGUID(), TempID, gossipTempText.c_str(), talentInfo->RankID[rank], rank + 1);
            }
        }
    }
    player->PlayDirectSound(Click, player);
}

bool GossipHello_player_stash(Player* player, GameObject* gameobject)
{
    if (player->isInCombat())
    {
        player->GetSession()->SendNotification("You are in combat!");
        return false;
    }

    player->PlayerTalkClass->ClearMenus();
    gameobject->PlayDirectSound(ChestOpen, player);

    uint32 gearsets = 0;
    uint32 specs = 0;
    std::string talentstext = "Save as...";
    std::string gearstext = "Save as...";

    gearsets = CountStoredGear(player);
    specs = CountStoredTalents(player);

    std::ostringstream ss_specs;
    ss_specs << "Stored Specifications (" << specs << ").";

    std::ostringstream ss_gearsets;
    ss_gearsets << "Stored Equipement Sets (" << gearsets << ").";

	player->ADD_GOSSIP_ITEM(GOSSIP_ICON_VENDOR, "Storage.", GOSSIP_SENDER_MAIN, SHOWBANK);
	player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Create a new UterusOne Teleporter.", GOSSIP_SENDER_MAIN, BINDHOME);
	//player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Mail.", GOSSIP_SENDER_MAIN, SHOWMAIL);
    if (gearsets > 0)
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TABARD, ss_gearsets.str().c_str(), GOSSIP_SENDER_MAIN, SHOW_GEAR);
    if (specs > 0)
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, ss_specs.str().c_str(), GOSSIP_SENDER_MAIN, SHOW_TALENTS);
    if (gearsets < 6)
        player->ADD_GOSSIP_ITEM_EXTENDED(GOSSIP_ICON_INTERACT_1, "Store equipped Items & Enchants.", GOSSIP_SENDER_MAIN, SAVE_GEAR, gearstext, true);
    if (specs < 6)
        player->ADD_GOSSIP_ITEM_EXTENDED(GOSSIP_ICON_INTERACT_1, "Store current Specification.", GOSSIP_SENDER_MAIN, SAVE_TALENTS, talentstext, true);

    player->SEND_GOSSIP_MENU(600020, gameobject->GetObjectGuid());

    return true;
}

bool Gossipstart_player_stash(Player* player, GameObject* gameobject, uint32 uiSender, uint32 uiAction)
{
    if (uiAction == SHOWBANK)
    {
        player->GetSession()->SendShowBank(player->GetGUID());
        AddBankBags(player);
    }
	if (uiAction == BINDHOME)
	{
		player->AddItem(90678, 1);
		player->CLOSE_GOSSIP_MENU();
	}
    else if (uiAction == SHOW_GEAR)
    {
        QueryResult *result = CharacterDatabase.PQuery("SELECT gossip_text, temp_id, patch "
            "FROM player_stash_gear WHERE char_guid = '%u' GROUP BY temp_id", player->GetGUID());

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                const char* SpecText = fields[0].GetString();
                uint32 temp_id = fields[1].GetUInt32();
                uint32 patch = fields[2].GetUInt32();
                std::ostringstream ss;
                ss << "Equip " << SpecText << ".";

                if (sWorld.GetWowPatch() >= patch)
                {
                    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ss.str().c_str(), GOSSIP_SENDER_GEAR_CONFIRM, temp_id);
                }
            } while (result->NextRow());
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Delete an Equipement Set.", GOSSIP_SENDER_MAIN, DELETE_GEAR);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, 0);
        player->SEND_GOSSIP_MENU(1, gameobject->GetObjectGuid());
    }
    else if (uiAction == SHOW_TALENTS)
    {
        QueryResult *result = CharacterDatabase.PQuery("SELECT gossip_text, temp_id "
            "FROM player_stash_talents WHERE char_guid = '%u' GROUP BY temp_id", player->GetGUID());

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                const char* SpecText = fields[0].GetString();
                uint32 temp_id = fields[1].GetUInt32();
                std::ostringstream ss;
                ss << "Use " << SpecText << ".";

                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ss.str().c_str(), GOSSIP_SENDER_TALENT_CONFIRM, temp_id);

            } while (result->NextRow());
			delete result;
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Delete a Specification.", GOSSIP_SENDER_MAIN, DELETE_TALENTS);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, 0);
        player->SEND_GOSSIP_MENU(1, gameobject->GetObjectGuid());
    }
    else if (uiAction == DELETE_TALENTS)
    {
        QueryResult *result = CharacterDatabase.PQuery("SELECT gossip_text, temp_id "
            "FROM player_stash_talents WHERE char_guid = '%u' GROUP BY temp_id", player->GetGUID());

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                const char* SpecText = fields[0].GetString();
                uint32 temp_id = fields[1].GetUInt32();
                std::ostringstream ss;
                ss << "Delete " << SpecText << ".";

                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, ss.str().c_str(), GOSSIP_SENDER_TALENTS_DELETE, temp_id);

            } while (result->NextRow());
			delete result;
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, 0);
        player->SEND_GOSSIP_MENU(1, gameobject->GetObjectGuid());
    }
    else if (uiAction == DELETE_GEAR)
    {
        QueryResult *result = CharacterDatabase.PQuery("SELECT gossip_text, temp_id, patch "
            "FROM player_stash_gear WHERE char_guid = '%u' GROUP BY temp_id", player->GetGUID());

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                const char* SpecText = fields[0].GetString();
                uint32 temp_id = fields[1].GetUInt32();
                std::ostringstream ss;
                ss << "Delete " << SpecText << ".";

                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, ss.str().c_str(), GOSSIP_SENDER_GEAR_DELETE, temp_id);

            } while (result->NextRow());
			delete result;
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, 0);
        player->SEND_GOSSIP_MENU(1, gameobject->GetObjectGuid());
    }
    else if (uiAction == 0)
        GossipHello_player_stash(player, gameobject);

    return true;
}

bool GossipDelete_Talents(Player* player, GameObject* gameobject, uint32 uiAction)
{
    bool res = CharacterDatabase.PQuery("DELETE FROM player_stash_talents WHERE temp_id = '%u' AND char_guid = '%u'", uiAction, player->GetGUID());
    if (res)
        player->GetSession()->SendAreaTriggerMessage("Successfuly deleted");

    Gossipstart_player_stash(player, gameobject, GOSSIP_SENDER_MAIN, DELETE_TALENTS);
    player->PlayDirectSound(Click, player);
    return true;
}

bool GossipDelete_Gear(Player* player, GameObject* gameobject, uint32 uiAction)
{
    bool res = CharacterDatabase.PQuery("DELETE FROM player_stash_gear WHERE temp_id = '%u' AND char_guid = '%u'", uiAction, player->GetGUID());
    if (res)
        player->GetSession()->SendAreaTriggerMessage("Successfuly deleted");

    Gossipstart_player_stash(player, gameobject, GOSSIP_SENDER_MAIN, DELETE_GEAR);
    player->PlayDirectSound(Click, player);
    return true;
}

bool GossipEquip_Gear(Player* player, GameObject* gameobject, uint32 uiAction)
{
    QueryResult* result = CharacterDatabase.PQuery("SELECT char_guid FROM player_stash_gear WHERE temp_id = '%u'", uiAction);

    if (result)
    {
        StashDeleteEquippedGear(player);
        StashEquipItemsFromDB(player, uiAction);
        GossipHello_player_stash(player, gameobject);
        player->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->PlayDirectSound(SpellFizzleHoly, 0);
		delete result;
    }
    return true;
}

bool GossipUse_Talents(Player* player, GameObject* gameobject, uint32 uiAction)
{
    QueryResult* result = CharacterDatabase.PQuery("SELECT char_guid FROM player_stash_talents WHERE temp_id = '%u'", uiAction);

    if (result)
    {
        player->ResetTalents(true);
        StashLearnTalentsFromDB(player, uiAction);
        GossipHello_player_stash(player, gameobject);
        player->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->PlayDirectSound(SpellFizzleHoly, 0);
		delete result;
    }
    return true;
}

bool GossipConfirm_Gear(Player* player, GameObject* gameobject, uint32 uiAction)
{
    player->PlayerTalkClass->ClearMenus();
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Yes.", GOSSIP_SENDER_GEAR_USE, uiAction);
    player->SEND_GOSSIP_MENU(600007, gameobject->GetObjectGuid());

    return true;
}

bool GossipConfirm_Talents(Player* player, GameObject* gameobject, uint32 uiAction)
{
    player->PlayerTalkClass->ClearMenus();
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Yes.", GOSSIP_SENDER_TALENT_USE, uiAction);
    player->SEND_GOSSIP_MENU(600021, gameobject->GetObjectGuid());

    return true;
}

bool GossipSelect_player_stash(Player* player, GameObject* gameobject, uint32 uiSender, uint32 uiAction)
{
    switch (uiSender)
    {
    case GOSSIP_SENDER_MAIN:
        Gossipstart_player_stash(player, gameobject, uiSender, uiAction);
        break;
    case GOSSIP_SENDER_GEAR_DELETE:
        GossipDelete_Gear(player, gameobject, uiAction);
        break;
    case GOSSIP_SENDER_TALENTS_DELETE:
        GossipDelete_Talents(player, gameobject, uiAction);
        break;
    case GOSSIP_SENDER_GEAR_CONFIRM:
        GossipConfirm_Gear(player, gameobject, uiAction);
        break;
    case GOSSIP_SENDER_TALENT_CONFIRM:
        GossipConfirm_Talents(player, gameobject, uiAction);
        break;
    case GOSSIP_SENDER_GEAR_USE:
        GossipEquip_Gear(player, gameobject, uiAction);
        break;
    case GOSSIP_SENDER_TALENT_USE:
        GossipUse_Talents(player, gameobject, uiAction);
        break;
    }
    return true;
}

bool GossipSelectCode_player_stash(Player* player, GameObject* gameobject, uint32 sender, uint32 action, const char* code)
{
    std::string name = code;

    if (name.length() > 30)
    {
        player->GetSession()->SendNotification("Name is too long. Max Name length: 30");
        player->CLOSE_GOSSIP_MENU();
        return false;
    }
    else
    {
        CharacterDatabase.escape_string(name);

        if (sender == GOSSIP_SENDER_MAIN)
        {
            switch (action)
            {
            case SAVE_GEAR:
                ExtractGearToDB(player, name);
                break;
            case SAVE_TALENTS:
                if (player->GetFreeTalentPoints() > 0)
                {
                    player->GetSession()->SendAreaTriggerMessage("You have unspend talent points. Please spend all your talent points.");
                    return false;
                }
                std::ostringstream ss;
                ss << name << " " << TalentsExportNameString(player);
                ExtractTalentsToDB(player, ss.str().c_str());
                break;
            }
            player->CLOSE_GOSSIP_MENU();
        }
        return true;
    }
}

void AddSC_player_stash()
{
    Script* newscript;
    newscript = new Script;
    newscript->Name = "player_stash";
    newscript->pGOGossipHello = &GossipHello_player_stash;
    newscript->pGOGossipSelect = &GossipSelect_player_stash;
    newscript->pGOGossipSelectWithCode = &GossipSelectCode_player_stash;
    newscript->RegisterSelf();
}

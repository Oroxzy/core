
#include "scriptPCH.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "DBCStores.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

#define GOSSIP_SENDER_GEAR_DELETE       10000001
#define GOSSIP_SENDER_TALENTS_DELETE    10000002
#define GOSSIP_SENDER_GEAR_CONFIRM      10000003
#define GOSSIP_SENDER_TALENT_CONFIRM    10000004
#define GOSSIP_SENDER_GEAR_USE          10000007
#define GOSSIP_SENDER_TALENT_USE        10000008

#define BOTTOMLESS_BAG                  14156

#define ChestOpen                       1277
#define ChestClose                      1278
#define SpellFizzleHoly                 1430
#define Click                           116

#define SHOWBANK                        (GOSSIP_ACTION_INFO_DEF + 1)
#define SHOWMAIL                        (GOSSIP_ACTION_INFO_DEF + 2)
#define SAVE_TALENTS                    (GOSSIP_ACTION_INFO_DEF + 3)
#define SAVE_GEAR                       (GOSSIP_ACTION_INFO_DEF + 4)
#define SHOW_TALENTS                    (GOSSIP_ACTION_INFO_DEF + 5)
#define SHOW_GEAR                       (GOSSIP_ACTION_INFO_DEF + 6)
#define DELETE_GEAR                     (GOSSIP_ACTION_INFO_DEF + 7)
#define DELETE_TALENTS                  (GOSSIP_ACTION_INFO_DEF + 8)
#define DELETE_GEAR_OK                  (GOSSIP_ACTION_INFO_DEF + 9)
#define DELETE_TALENTS_OK               (GOSSIP_ACTION_INFO_DEF + 10)
#define APPLY_GEAR                      (GOSSIP_ACTION_INFO_DEF + 11)
#define APPLY_TALENTS                   (GOSSIP_ACTION_INFO_DEF + 12)
#define BINDHOME                        (GOSSIP_ACTION_INFO_DEF + 13)

#define COOL_VISUAL_SPELL               25100
#define MAX_STASH_SETS                  6
#define MIN_TEMP_ID                     50
#define STASH_TELEPORTER_ITEM           90678

namespace PlayerStash
{
    enum TalentTabNames
    {
        WarriorProtection      = 163,
        WarriorFury            = 164,
        WarriorArms            = 161,
        WarlockDemonology      = 303,
        WarlockDestruction     = 301,
        WarlockAffliction      = 302,
        ShamanRestoration      = 262,
        ShamanEnhancement      = 263,
        ShamanElementalCombat  = 261,
        RogueSubtlety          = 183,
        RogueCombat            = 181,
        RogueAssassination     = 182,
        PriestShadow           = 203,
        PriestHoly             = 202,
        PriestDiscipline       = 201,
        PaladinProtection      = 383,
        PaladinHoly            = 382,
        PaladinRetribution     = 381,
        MageFrost              = 61,
        MageFire               = 41,
        MageArcane             = 81,
        HunterSurvival         = 362,
        HunterMarksmanship     = 363,
        HunterBeastMastery     = 361,
        DruidRestoration       = 282,
        DruidFeralCombat       = 281,
        DruidBalance           = 283
    };

    struct GearRow
    {
        uint8 slot;
        uint32 itemEntry;
        uint32 enchantId;

        GearRow() : slot(0), itemEntry(0), enchantId(0) {}
        GearRow(uint8 pSlot, uint32 pItemEntry, uint32 pEnchantId)
            : slot(pSlot), itemEntry(pItemEntry), enchantId(pEnchantId) {}
    };

    static bool IsSupportedEquipmentSlot(uint8 slot)
    {
        if (slot < EQUIPMENT_SLOT_START || slot >= EQUIPMENT_SLOT_END)
            return false;

        return slot != EQUIPMENT_SLOT_BODY && slot != EQUIPMENT_SLOT_TABARD;
    }

    static void PlaySuccessFeedback(Player* player)
    {
        if (!player)
            return;

        player->CastSpell(player, COOL_VISUAL_SPELL, true);
        player->PlayDirectSound(SpellFizzleHoly, 0);
    }

    static uint32 GetNextTempId(const char* tableName)
    {
        uint32 tempId = 0;
        QueryResult* result = CharacterDatabase.PQuery("SELECT MAX(temp_id) FROM %s", tableName);
        if (result)
        {
            Field* fields = result->Fetch();
            if (fields)
                tempId = fields[0].GetUInt32();
            delete result;
        }

        return tempId ? (tempId + 1) : MIN_TEMP_ID;
    }

    static uint32 GetTalentId()
    {
        return GetNextTempId("player_stash_talents");
    }

    static uint32 GetGearId()
    {
        return GetNextTempId("player_stash_gear");
    }

    static uint32 CountDistinctSets(Player* player, const char* tableName)
    {
        if (!player)
            return 0;

        uint32 count = 0;
        QueryResult* result = CharacterDatabase.PQuery("SELECT COUNT(DISTINCT temp_id) FROM %s WHERE char_guid='%u'", tableName, player->GetGUID());
        if (result)
        {
            Field* fields = result->Fetch();
            if (fields)
                count = fields[0].GetUInt32();
            delete result;
        }

        return count;
    }

    static uint32 CountStoredGear(Player* player)
    {
        return CountDistinctSets(player, "player_stash_gear");
    }

    static uint32 CountStoredTalents(Player* player)
    {
        return CountDistinctSets(player, "player_stash_talents");
    }

    static bool HasStoredGear(Player* player, uint32 tempId)
    {
        if (!player || !tempId)
            return false;

        QueryResult* result = CharacterDatabase.PQuery("SELECT 1 FROM player_stash_gear WHERE temp_id='%u' AND char_guid='%u' LIMIT 1", tempId, player->GetGUID());
        if (!result)
            return false;

        delete result;
        return true;
    }

    static bool HasStoredTalents(Player* player, uint32 tempId)
    {
        if (!player || !tempId)
            return false;

        QueryResult* result = CharacterDatabase.PQuery("SELECT 1 FROM player_stash_talents WHERE temp_id='%u' AND char_guid='%u' LIMIT 1", tempId, player->GetGUID());
        if (!result)
            return false;

        delete result;
        return true;
    }

    static uint32 GetItemPatch(uint32 itemEntry)
    {
        uint32 patch = 0;
        QueryResult* result = WorldDatabase.PQuery("SELECT patch FROM item_template WHERE entry='%u'", itemEntry);
        if (result)
        {
            Field* fields = result->Fetch();
            if (fields)
                patch = fields[0].GetUInt32();
            delete result;
        }
        return patch;
    }

    static bool PlayerHasUnspentTalentPoints(Player* player)
    {
        return player && player->GetFreeTalentPoints() > 0;
    }

    static void AddBankBags(Player* player)
    {
        if (!player)
            return;

        for (uint8 slot = BANK_SLOT_BAG_START; slot < BANK_SLOT_BAG_END; ++slot)
        {
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                player->EquipNewItem(slot, BOTTOMLESS_BAG, true);
        }
    }

    static void ApplyEnchantment(Player* player, Item* item, EnchantmentSlot slot, uint32 enchantEntry, uint32 duration, uint32 charges)
    {
        if (!player || !item || !enchantEntry)
            return;

        player->ApplyEnchantment(item, slot, false);
        item->SetEnchantment(slot, enchantEntry, duration, charges);
        player->ApplyEnchantment(item, slot, true);
    }

    static void DeleteEquippedGear(Player* player)
    {
        if (!player)
            return;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (!IsSupportedEquipmentSlot(slot))
                continue;

            if (player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
        }
    }

    static bool LoadGearRows(Player* player, uint32 tempId, std::vector<GearRow>& rows)
    {
        rows.clear();

        if (!player || !tempId)
            return false;

        QueryResult* result = CharacterDatabase.PQuery(
            "SELECT item_slot, item_entry, item_enchant FROM player_stash_gear WHERE char_guid='%u' AND temp_id='%u' ORDER BY item_slot ASC",
            player->GetGUID(), tempId);

        if (!result)
            return false;

        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                continue;

            uint8 slot = uint8(fields[0].GetUInt32());
            if (!IsSupportedEquipmentSlot(slot))
                continue;

            rows.push_back(GearRow(slot, fields[1].GetUInt32(), fields[2].GetUInt32()));
        }
        while (result->NextRow());

        delete result;
        return !rows.empty();
    }

    static void EquipItemsFromDB(Player* player, uint32 tempId)
    {
        if (!player || !tempId)
            return;

        std::vector<GearRow> rows;
        if (!LoadGearRows(player, tempId, rows))
            return;

        for (std::vector<GearRow>::const_iterator itr = rows.begin(); itr != rows.end(); ++itr)
        {
            ItemPrototype const* itemProto = sObjectMgr.GetItemPrototype(itr->itemEntry);
            if (!itemProto)
                continue;

            if (itemProto->RequiredReputationFaction && itemProto->RequiredReputationRank > 0)
            {
                if (ReputationRank(itemProto->RequiredReputationRank) > player->GetReputationRank(itemProto->RequiredReputationFaction))
                {
                    FactionEntry const* factionEntry = sObjectMgr.GetFactionEntry(itemProto->RequiredReputationFaction);
                    if (factionEntry)
                        player->GetReputationMgr().ModifyReputation(factionEntry, 85000);
                }
            }

            player->EquipNewItem(itr->slot, itr->itemEntry, true);
            ApplyEnchantment(player, player->GetItemByPos(INVENTORY_SLOT_BAG_0, itr->slot), PERM_ENCHANTMENT_SLOT, itr->enchantId, 0, 0);
        }
    }

    static void LearnTalentsFromDB(Player* player, uint32 tempId)
    {
        if (!player || !tempId)
            return;

        QueryResult* result = CharacterDatabase.PQuery(
            "SELECT talent_id FROM player_stash_talents WHERE char_guid='%u' AND temp_id='%u' ORDER BY rank ASC",
            player->GetGUID(), tempId);

        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                continue;

            uint32 talentId = fields[0].GetUInt32();
            if (talentId)
                player->LearnSpell(talentId, false, true);
        }
        while (result->NextRow());

        delete result;
    }

    static std::string TalentsExportNameString(Player* player)
    {
        if (!player)
            return "";

        std::map<uint32, uint32> pointsByTab;

        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
            if (!talentInfo)
                continue;

            TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
            if (!talentTabInfo)
                continue;

            if ((player->getClassMask() & talentTabInfo->ClassMask) == 0)
                continue;

            int32 highestRank = -1;
            for (int32 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                if (!talentInfo->RankID[rank])
                    continue;

                if (player->HasSpell(talentInfo->RankID[rank]))
                {
                    highestRank = rank;
                    break;
                }
            }

            if (highestRank >= 0)
                pointsByTab[talentInfo->TalentTab] += uint32(highestRank + 1);
        }

        uint32 treeA = 0;
        uint32 treeB = 0;
        uint32 treeC = 0;
        const char* dominantName = "";

        switch (player->getClass())
        {
        case CLASS_WARRIOR:
            treeA = pointsByTab[WarriorArms];
            treeB = pointsByTab[WarriorFury];
            treeC = pointsByTab[WarriorProtection];
            dominantName = (treeA >= treeB && treeA >= treeC) ? "Arms" : ((treeB >= treeA && treeB >= treeC) ? "Fury" : "Protection");
            break;
        case CLASS_WARLOCK:
            treeA = pointsByTab[WarlockAffliction];
            treeB = pointsByTab[WarlockDemonology];
            treeC = pointsByTab[WarlockDestruction];
            dominantName = (treeA >= treeB && treeA >= treeC) ? "Affliction" : ((treeB >= treeA && treeB >= treeC) ? "Demonology" : "Destruction");
            break;
        case CLASS_SHAMAN:
            treeA = pointsByTab[ShamanElementalCombat];
            treeB = pointsByTab[ShamanEnhancement];
            treeC = pointsByTab[ShamanRestoration];
            dominantName = (treeA >= treeB && treeA >= treeC) ? "Elemental" : ((treeB >= treeA && treeB >= treeC) ? "Enhancement" : "Restoration");
            break;
        case CLASS_ROGUE:
            treeA = pointsByTab[RogueAssassination];
            treeB = pointsByTab[RogueCombat];
            treeC = pointsByTab[RogueSubtlety];
            dominantName = (treeA >= treeB && treeA >= treeC) ? "Assassination" : ((treeB >= treeA && treeB >= treeC) ? "Combat" : "Subtlety");
            break;
        case CLASS_PRIEST:
            treeA = pointsByTab[PriestDiscipline];
            treeB = pointsByTab[PriestHoly];
            treeC = pointsByTab[PriestShadow];
            dominantName = (treeA >= treeB && treeA >= treeC) ? "Discipline" : ((treeB >= treeA && treeB >= treeC) ? "Holy" : "Shadow");
            break;
        case CLASS_PALADIN:
            treeA = pointsByTab[PaladinHoly];
            treeB = pointsByTab[PaladinProtection];
            treeC = pointsByTab[PaladinRetribution];
            dominantName = (treeA >= treeB && treeA >= treeC) ? "Holy" : ((treeB >= treeA && treeB >= treeC) ? "Protection" : "Retribution");
            break;
        case CLASS_MAGE:
            treeA = pointsByTab[MageArcane];
            treeB = pointsByTab[MageFire];
            treeC = pointsByTab[MageFrost];
            dominantName = (treeA >= treeB && treeA >= treeC) ? "Arcane" : ((treeB >= treeA && treeB >= treeC) ? "Fire" : "Frost");
            break;
        case CLASS_HUNTER:
            treeA = pointsByTab[HunterBeastMastery];
            treeB = pointsByTab[HunterMarksmanship];
            treeC = pointsByTab[HunterSurvival];
            dominantName = (treeA >= treeB && treeA >= treeC) ? "Beast Mastery" : ((treeB >= treeA && treeB >= treeC) ? "Marksmanship" : "Survival");
            break;
        case CLASS_DRUID:
            treeA = pointsByTab[DruidBalance];
            treeB = pointsByTab[DruidFeralCombat];
            treeC = pointsByTab[DruidRestoration];
            dominantName = (treeA >= treeB && treeA >= treeC) ? "Balance" : ((treeB >= treeA && treeB >= treeC) ? "Feral Combat" : "Restoration");
            break;
        default:
            return "";
        }

        std::ostringstream out;
        out << "(" << dominantName << " " << treeA << "/" << treeB << "/" << treeC << ")";
        return out.str();
    }

    static void ExtractGearToDB(Player* player, std::string const& gossipText)
    {
        if (!player)
            return;

        uint32 tempId = GetGearId();
        uint32 patch = 0;
        std::vector<GearRow> rows;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (!IsSupportedEquipmentSlot(slot))
                continue;

            Item* equippedItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!equippedItem)
                continue;

            uint32 itemEntry = equippedItem->GetEntry();
            uint32 itemPatch = GetItemPatch(itemEntry);
            if (itemPatch > patch)
                patch = itemPatch;

            rows.push_back(GearRow(slot, itemEntry, equippedItem->GetEnchantmentId(PERM_ENCHANTMENT_SLOT)));
        }

        if (rows.empty())
        {
            player->GetSession()->SendNotification("No equipped items found to store.");
            return;
        }

        for (std::vector<GearRow>::const_iterator itr = rows.begin(); itr != rows.end(); ++itr)
        {
            CharacterDatabase.PExecute(
                "INSERT INTO player_stash_gear (char_guid, temp_id, gossip_text, item_slot, item_entry, item_enchant, patch) VALUES ('%u', '%u', '%s', '%u', '%u', '%u', '%u')",
                player->GetGUID(), tempId, gossipText.c_str(), itr->slot, itr->itemEntry, itr->enchantId, patch);
        }

        player->PlayDirectSound(Click, player);
        player->GetSession()->SendAreaTriggerMessage("Equipment set stored.");
    }

    static void ExtractTalentsToDB(Player* player, std::string const& gossipText)
    {
        if (!player)
            return;

        if (PlayerHasUnspentTalentPoints(player))
        {
            player->GetSession()->SendAreaTriggerMessage("You have unspent talent points. Please spend all your talent points.");
            return;
        }

        uint32 tempId = GetTalentId();
        static SqlStatementID insTalents;
        SqlStatement stmtIns = CharacterDatabase.CreateStatement(insTalents, "INSERT INTO player_stash_talents (char_guid, temp_id, gossip_text, talent_id, rank) VALUES (?, ?, ?, ?, ?)");

        uint32 savedCount = 0;
        for (uint32 i = 0; i < sTalentStore.GetNumRows(); ++i)
        {
            TalentEntry const* talentInfo = sTalentStore.LookupEntry(i);
            if (!talentInfo)
                continue;

            TalentTabEntry const* talentTabInfo = sTalentTabStore.LookupEntry(talentInfo->TalentTab);
            if (!talentTabInfo)
                continue;

            if ((player->getClassMask() & talentTabInfo->ClassMask) == 0)
                continue;

            for (int32 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                if (!talentInfo->RankID[rank])
                    continue;

                if (player->HasSpell(talentInfo->RankID[rank]))
                {
                    stmtIns.PExecute(player->GetGUID(), tempId, gossipText.c_str(), talentInfo->RankID[rank], rank + 1);
                    ++savedCount;
                    break;
                }
            }
        }

        if (!savedCount)
        {
            CharacterDatabase.PExecute("DELETE FROM player_stash_talents WHERE char_guid='%u' AND temp_id='%u'", player->GetGUID(), tempId);
            player->GetSession()->SendNotification("No talents found to store.");
            return;
        }

        player->PlayDirectSound(Click, player);
        player->GetSession()->SendAreaTriggerMessage("Specification stored.");
    }

    static void AddBackButton(Player* player)
    {
        if (!player)
            return;

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", GOSSIP_SENDER_MAIN, 0);
    }

    static void ShowMainMenu(Player* player, GameObject* gameobject)
    {
        if (!player || !gameobject)
            return;

        if (player->isInCombat())
        {
            player->GetSession()->SendNotification("You are in combat!");
            return;
        }

        player->PlayerTalkClass->ClearMenus();
        gameobject->PlayDirectSound(ChestOpen, player);

        const uint32 gearsets = CountStoredGear(player);
        const uint32 specs = CountStoredTalents(player);

        std::ostringstream ssSpecs;
        ssSpecs << "Stored Specifications (" << specs << ").";

        std::ostringstream ssGearsets;
        ssGearsets << "Stored Equipment Sets (" << gearsets << ").";

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_VENDOR, "Storage.", GOSSIP_SENDER_MAIN, SHOWBANK);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Create a new UterusOne Teleporter.", GOSSIP_SENDER_MAIN, BINDHOME);

        if (gearsets > 0)
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TABARD, ssGearsets.str().c_str(), GOSSIP_SENDER_MAIN, SHOW_GEAR);

        if (specs > 0)
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, ssSpecs.str().c_str(), GOSSIP_SENDER_MAIN, SHOW_TALENTS);

        if (gearsets < MAX_STASH_SETS)
            player->ADD_GOSSIP_ITEM_EXTENDED(GOSSIP_ICON_INTERACT_1, "Store equipped Items & Enchants.", GOSSIP_SENDER_MAIN, SAVE_GEAR, "Save as...", true);

        if (specs < MAX_STASH_SETS)
            player->ADD_GOSSIP_ITEM_EXTENDED(GOSSIP_ICON_INTERACT_1, "Store current Specification.", GOSSIP_SENDER_MAIN, SAVE_TALENTS, "Save as...", true);

        player->SEND_GOSSIP_MENU(600020, gameobject->GetObjectGuid());
    }

    static void ShowGearMenu(Player* player, GameObject* gameobject)
    {
        player->PlayerTalkClass->ClearMenus();

        QueryResult* result = CharacterDatabase.PQuery(
            "SELECT gossip_text, temp_id, patch FROM player_stash_gear WHERE char_guid='%u' GROUP BY temp_id ORDER BY temp_id ASC",
            player->GetGUID());

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                if (!fields)
                    continue;

                const char* specText = fields[0].GetString();
                uint32 tempId = fields[1].GetUInt32();
                uint32 patch = fields[2].GetUInt32();

                if (sWorld.GetWowPatch() >= patch)
                {
                    std::ostringstream ss;
                    ss << "Equip " << specText << ".";
                    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ss.str().c_str(), GOSSIP_SENDER_GEAR_CONFIRM, tempId);
                }
            }
            while (result->NextRow());

            delete result;
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Delete an Equipment Set.", GOSSIP_SENDER_MAIN, DELETE_GEAR);
        AddBackButton(player);
        player->SEND_GOSSIP_MENU(1, gameobject->GetObjectGuid());
    }

    static void ShowTalentMenu(Player* player, GameObject* gameobject)
    {
        player->PlayerTalkClass->ClearMenus();

        QueryResult* result = CharacterDatabase.PQuery(
            "SELECT gossip_text, temp_id FROM player_stash_talents WHERE char_guid='%u' GROUP BY temp_id ORDER BY temp_id ASC",
            player->GetGUID());

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                if (!fields)
                    continue;

                std::ostringstream ss;
                ss << "Use " << fields[0].GetString() << ".";
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, ss.str().c_str(), GOSSIP_SENDER_TALENT_CONFIRM, fields[1].GetUInt32());
            }
            while (result->NextRow());

            delete result;
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Delete a Specification.", GOSSIP_SENDER_MAIN, DELETE_TALENTS);
        AddBackButton(player);
        player->SEND_GOSSIP_MENU(1, gameobject->GetObjectGuid());
    }

    static void ShowDeleteTalentMenu(Player* player, GameObject* gameobject)
    {
        player->PlayerTalkClass->ClearMenus();

        QueryResult* result = CharacterDatabase.PQuery(
            "SELECT gossip_text, temp_id FROM player_stash_talents WHERE char_guid='%u' GROUP BY temp_id ORDER BY temp_id ASC",
            player->GetGUID());

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                if (!fields)
                    continue;

                std::ostringstream ss;
                ss << "Delete " << fields[0].GetString() << ".";
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, ss.str().c_str(), GOSSIP_SENDER_TALENTS_DELETE, fields[1].GetUInt32());
            }
            while (result->NextRow());

            delete result;
        }

        AddBackButton(player);
        player->SEND_GOSSIP_MENU(1, gameobject->GetObjectGuid());
    }

    static void ShowDeleteGearMenu(Player* player, GameObject* gameobject)
    {
        player->PlayerTalkClass->ClearMenus();

        QueryResult* result = CharacterDatabase.PQuery(
            "SELECT gossip_text, temp_id FROM player_stash_gear WHERE char_guid='%u' GROUP BY temp_id ORDER BY temp_id ASC",
            player->GetGUID());

        if (result)
        {
            do
            {
                Field* fields = result->Fetch();
                if (!fields)
                    continue;

                std::ostringstream ss;
                ss << "Delete " << fields[0].GetString() << ".";
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, ss.str().c_str(), GOSSIP_SENDER_GEAR_DELETE, fields[1].GetUInt32());
            }
            while (result->NextRow());

            delete result;
        }

        AddBackButton(player);
        player->SEND_GOSSIP_MENU(1, gameobject->GetObjectGuid());
    }

    static bool DeleteTalents(Player* player, GameObject* gameobject, uint32 tempId)
    {
        if (!player)
            return true;

        CharacterDatabase.PExecute("DELETE FROM player_stash_talents WHERE temp_id='%u' AND char_guid='%u'", tempId, player->GetGUID());
        player->GetSession()->SendAreaTriggerMessage("Successfully deleted.");
        player->PlayDirectSound(Click, player);
        ShowDeleteTalentMenu(player, gameobject);
        return true;
    }

    static bool DeleteGear(Player* player, GameObject* gameobject, uint32 tempId)
    {
        if (!player)
            return true;

        CharacterDatabase.PExecute("DELETE FROM player_stash_gear WHERE temp_id='%u' AND char_guid='%u'", tempId, player->GetGUID());
        player->GetSession()->SendAreaTriggerMessage("Successfully deleted.");
        player->PlayDirectSound(Click, player);
        ShowDeleteGearMenu(player, gameobject);
        return true;
    }

    static bool EquipGear(Player* player, GameObject* gameobject, uint32 tempId)
    {
        if (!player)
            return true;

        if (!HasStoredGear(player, tempId))
        {
            player->GetSession()->SendNotification("This equipment set does not exist.");
            ShowMainMenu(player, gameobject);
            return true;
        }

        DeleteEquippedGear(player);
        EquipItemsFromDB(player, tempId);
        PlaySuccessFeedback(player);
        ShowMainMenu(player, gameobject);
        return true;
    }

    static bool UseTalents(Player* player, GameObject* gameobject, uint32 tempId)
    {
        if (!player)
            return true;

        if (!HasStoredTalents(player, tempId))
        {
            player->GetSession()->SendNotification("This specification does not exist.");
            ShowMainMenu(player, gameobject);
            return true;
        }

        player->ResetTalents(true);
        LearnTalentsFromDB(player, tempId);
        PlaySuccessFeedback(player);
        ShowMainMenu(player, gameobject);
        return true;
    }

    static bool ConfirmGear(Player* player, GameObject* gameobject, uint32 tempId)
    {
        player->PlayerTalkClass->ClearMenus();
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Yes.", GOSSIP_SENDER_GEAR_USE, tempId);
        player->SEND_GOSSIP_MENU(600007, gameobject->GetObjectGuid());
        return true;
    }

    static bool ConfirmTalents(Player* player, GameObject* gameobject, uint32 tempId)
    {
        player->PlayerTalkClass->ClearMenus();
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Yes.", GOSSIP_SENDER_TALENT_USE, tempId);
        player->SEND_GOSSIP_MENU(600021, gameobject->GetObjectGuid());
        return true;
    }

    static bool HandleMainAction(Player* player, GameObject* gameobject, uint32 uiAction)
    {
        if (!player || !gameobject)
            return true;

        switch (uiAction)
        {
        case SHOWBANK:
            player->GetSession()->SendShowBank(player->GetGUID());
            AddBankBags(player);
            break;
        case BINDHOME:
            player->AddItem(STASH_TELEPORTER_ITEM, 1);
            player->CLOSE_GOSSIP_MENU();
            break;
        case SHOW_GEAR:
            ShowGearMenu(player, gameobject);
            break;
        case SHOW_TALENTS:
            ShowTalentMenu(player, gameobject);
            break;
        case DELETE_TALENTS:
            ShowDeleteTalentMenu(player, gameobject);
            break;
        case DELETE_GEAR:
            ShowDeleteGearMenu(player, gameobject);
            break;
        case 0:
            ShowMainMenu(player, gameobject);
            break;
        default:
            break;
        }

        return true;
    }
}

bool GossipHello_player_stash(Player* player, GameObject* gameobject)
{
    PlayerStash::ShowMainMenu(player, gameobject);
    return true;
}

bool Gossipstart_player_stash(Player* player, GameObject* gameobject, uint32 /*uiSender*/, uint32 uiAction)
{
    return PlayerStash::HandleMainAction(player, gameobject, uiAction);
}

bool GossipDelete_Talents(Player* player, GameObject* gameobject, uint32 uiAction)
{
    return PlayerStash::DeleteTalents(player, gameobject, uiAction);
}

bool GossipDelete_Gear(Player* player, GameObject* gameobject, uint32 uiAction)
{
    return PlayerStash::DeleteGear(player, gameobject, uiAction);
}

bool GossipEquip_Gear(Player* player, GameObject* gameobject, uint32 uiAction)
{
    return PlayerStash::EquipGear(player, gameobject, uiAction);
}

bool GossipUse_Talents(Player* player, GameObject* gameobject, uint32 uiAction)
{
    return PlayerStash::UseTalents(player, gameobject, uiAction);
}

bool GossipConfirm_Gear(Player* player, GameObject* gameobject, uint32 uiAction)
{
    return PlayerStash::ConfirmGear(player, gameobject, uiAction);
}

bool GossipConfirm_Talents(Player* player, GameObject* gameobject, uint32 uiAction)
{
    return PlayerStash::ConfirmTalents(player, gameobject, uiAction);
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
    default:
        break;
    }

    return true;
}

bool GossipSelectCode_player_stash(Player* player, GameObject* gameobject, uint32 sender, uint32 action, const char* code)
{
    if (!player)
        return false;

    std::string name = code ? code : "";
    if (name.empty())
    {
        player->GetSession()->SendNotification("Name is empty.");
        player->CLOSE_GOSSIP_MENU();
        return false;
    }

    if (name.length() > 30)
    {
        player->GetSession()->SendNotification("Name is too long. Max name length: 30.");
        player->CLOSE_GOSSIP_MENU();
        return false;
    }

    CharacterDatabase.escape_string(name);

    if (sender == GOSSIP_SENDER_MAIN)
    {
        switch (action)
        {
        case SAVE_GEAR:
            PlayerStash::ExtractGearToDB(player, name);
            break;
        case SAVE_TALENTS:
        {
            if (PlayerStash::PlayerHasUnspentTalentPoints(player))
            {
                player->GetSession()->SendAreaTriggerMessage("You have unspent talent points. Please spend all your talent points.");
                return false;
            }

            std::ostringstream ss;
            ss << name << " " << PlayerStash::TalentsExportNameString(player);
            PlayerStash::ExtractTalentsToDB(player, ss.str());
            break;
        }
        default:
            break;
        }
    }

    player->CLOSE_GOSSIP_MENU();
    return true;
}

void AddSC_player_stash()
{
    Script* newscript = new Script;
    newscript->Name = "player_stash";
    newscript->pGOGossipHello = &GossipHello_player_stash;
    newscript->pGOGossipSelect = &GossipSelect_player_stash;
    newscript->pGOGossipSelectWithCode = &GossipSelectCode_player_stash;
    newscript->RegisterSelf();
}

#include "scriptPCH.h"
#include "Player.h"
#include "Item.h"
#include "SpellMgr.h"
#include "Database/DatabaseEnv.h"

#include <map>
#include <vector>
#include <sstream>
#include <algorithm>

namespace
{
    static const uint32 kVisualSpell = 4319;

    static const uint32 ACTION_MAIN_MENU          = 1;
    static const uint32 ACTION_MAINHAND_MENU      = 10;
    static const uint32 ACTION_OFFHAND_MENU       = 11;
    static const uint32 ACTION_CONFIRM_REMOVE_ALL = 20;
    static const uint32 ACTION_DO_REMOVE_ALL      = 21;

    struct TempEnchantOption
    {
        uint32 itemEntry;
        uint32 spellId;
        uint32 requiredLevel;
        uint32 patch;
        std::string itemName;
        std::string spellName;
    };

    static bool gTempEnchantOptionsLoaded = false;
    static std::vector<TempEnchantOption> gTempEnchantOptions;

    bool IsWeaponEquippedInSlot(Player* player, uint8 slot)
    {
        if (!player)
            return false;

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->GetProto())
            return false;

        ItemPrototype const* proto = item->GetProto();
        if (proto->Class != ITEM_CLASS_WEAPON)
            return false;

        switch (proto->InventoryType)
        {
            case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND:
            case INVTYPE_WEAPONOFFHAND:
            case INVTYPE_2HWEAPON:
                return true;
            default:
                return false;
        }
    }

    std::string GetTempEnchantNameFromItem(Item* item)
    {
        if (!item)
            return std::string("No weapon");

        uint32 enchantId = item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
        if (!enchantId)
            return std::string("None");

        SpellItemEnchantmentEntry const* enchantEntry = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!enchantEntry)
        {
            std::ostringstream ss;
            ss << "Enchant ID " << enchantId;
            return ss.str();
        }

        for (int i = 0; i < 3; ++i)
        {
            if (enchantEntry->description[i] && enchantEntry->description[i][0] != '\0')
                return std::string(enchantEntry->description[i]);
        }

        std::ostringstream ss;
        ss << "Enchant ID " << enchantId;
        return ss.str();
    }

    const char* GetSlotName(uint8 slot)
    {
        switch (slot)
        {
            case EQUIPMENT_SLOT_MAINHAND: return "Main Hand";
            case EQUIPMENT_SLOT_OFFHAND:  return "Off Hand";
            default:                      return "Unknown Slot";
        }
    }

    void SendApplyMessage(Player* player, Item* item, uint8 slot, const std::string& oldName, const std::string& newName)
    {
        if (!player || !item || !item->GetProto())
            return;

        std::ostringstream ss;
        if (oldName != "None")
            ss << GetSlotName(slot) << ": " << item->GetProto()->Name1 << " replaced " << oldName << " with " << newName << ".";
        else
            ss << GetSlotName(slot) << ": " << item->GetProto()->Name1 << " was coated with " << newName << ".";

        player->GetSession()->SendNotification("%s", ss.str().c_str());
    }

    bool CanUseTempEnchantOption(Player* player, uint8 slot, TempEnchantOption const& option)
    {
        if (!player)
            return false;

        if (!IsWeaponEquippedInSlot(player, slot))
            return false;

        if (option.patch > sWorld.GetWowPatch())
            return false;

        ItemPrototype const* itemProto = sObjectMgr.GetItemPrototype(option.itemEntry);
        if (!itemProto)
            return false;

        if (player->CanUseItem(itemProto, false) != EQUIP_ERR_OK)
            return false;

        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(option.spellId);
        if (!spellInfo)
            return false;

        Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!weapon || !weapon->GetProto())
            return false;

        if (!weapon->IsFitToSpellRequirements(spellInfo))
            return false;

        return true;
    }

    void LoadTempEnchantOptions()
    {
        if (gTempEnchantOptionsLoaded)
            return;

        gTempEnchantOptionsLoaded = true;
        gTempEnchantOptions.clear();

        std::unique_ptr<QueryResult> result(WorldDatabase.Query(
            "SELECT `entry`, `patch`, `name`, "
            "`required_level`, "
            "`spellid_1`, `spelltrigger_1`, "
            "`spellid_2`, `spelltrigger_2`, "
            "`spellid_3`, `spelltrigger_3`, "
            "`spellid_4`, `spelltrigger_4`, "
            "`spellid_5`, `spelltrigger_5` "
            "FROM `item_template`"));

        if (!result)
            return;

        std::map<uint32, TempEnchantOption> bestBySpellId;

        do
        {
            Field* fields = result->Fetch();

            uint32 itemEntry      = fields[0].GetUInt32();
            uint32 patch          = fields[1].GetUInt32();
            std::string itemName  = fields[2].GetCppString();
            uint32 requiredLevel  = fields[3].GetUInt32();

            for (uint32 i = 0; i < 5; ++i)
            {
                uint32 spellId = fields[4 + (i * 2)].GetUInt32();
                uint32 trigger = fields[5 + (i * 2)].GetUInt32();

                if (!spellId)
                    continue;

                if (trigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;

                SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
                if (!spellInfo)
                    continue;

                bool isTempEnchant = false;
                uint32 enchantId = 0;

                for (int eff = 0; eff < 3; ++eff)
                {
                    if (spellInfo->Effect[eff] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                    {
                        isTempEnchant = true;
                        enchantId = spellInfo->EffectMiscValue[eff];
                        break;
                    }
                }

                if (!isTempEnchant || !enchantId)
                    continue;

                std::string spellName = spellInfo->SpellName[0];
                if (spellName.empty())
                    continue;
                
                if (spellName.find("zzOLD") != std::string::npos ||
                    spellName.find("(DND)") != std::string::npos ||
                    spellName.find("UNUSED") != std::string::npos ||
                    spellName.find("TEST") != std::string::npos)
                {
                    continue;
                }
                
                TempEnchantOption option;
                option.itemEntry      = itemEntry;
                option.spellId        = spellId;
                option.requiredLevel  = requiredLevel;
                option.patch          = patch;
                option.itemName       = itemName;
                option.spellName      = spellName;

                std::map<uint32, TempEnchantOption>::iterator itr = bestBySpellId.find(spellId);
                if (itr == bestBySpellId.end())
                {
                    bestBySpellId[spellId] = option;
                }
                else
                {
                    if (option.patch > itr->second.patch)
                        itr->second = option;
                    else if (option.patch == itr->second.patch && option.requiredLevel < itr->second.requiredLevel)
                        itr->second = option;
                }
            }
        }
        while (result->NextRow());

        for (std::map<uint32, TempEnchantOption>::const_iterator itr = bestBySpellId.begin(); itr != bestBySpellId.end(); ++itr)
            gTempEnchantOptions.push_back(itr->second);

        std::sort(gTempEnchantOptions.begin(), gTempEnchantOptions.end(),
            [](TempEnchantOption const& a, TempEnchantOption const& b) -> bool
            {
                if (a.requiredLevel != b.requiredLevel)
                    return a.requiredLevel < b.requiredLevel;

                if (a.itemName != b.itemName)
                    return a.itemName < b.itemName;

                return a.spellId < b.spellId;
            });
    }

    void ApplyTempEnchant(Player* player, GameObject* go, uint32 spellId, uint8 slot)
    {
        if (!player || !go)
            return;

        if (player->IsInCombat())
        {
            player->GetSession()->SendNotification("You cannot apply poisons or temporary weapon enchants while in combat.");
            return;
        }

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->GetProto())
            return;

        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
        if (!spellInfo)
        {
            player->GetSession()->SendNotification("Spell entry not found.");
            return;
        }

        if (!item->IsFitToSpellRequirements(spellInfo))
        {
            player->GetSession()->SendNotification("That temporary enchant cannot be applied to this weapon.");
            return;
        }

        uint32 enchantId = 0;
        uint32 duration = 0;

        for (int eff = 0; eff < 3; ++eff)
        {
            if (spellInfo->Effect[eff] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
            {
                enchantId = spellInfo->EffectMiscValue[eff];
                duration = (spellInfo->EffectBasePoints[eff] + 1) * IN_MILLISECONDS;
                break;
            }
        }

        if (!enchantId)
        {
            player->GetSession()->SendNotification("Temporary enchant data is missing on that spell.");
            return;
        }

        if (duration == 0)
            duration = 30 * MINUTE * IN_MILLISECONDS;

        std::string oldName = GetTempEnchantNameFromItem(item);
        std::string newName = spellInfo->SpellName[0].empty() ? std::string("Temporary Enchant") : std::string(spellInfo->SpellName[0]);

        player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, false);
        item->ClearEnchantment(TEMP_ENCHANTMENT_SLOT);
        item->SetEnchantment(TEMP_ENCHANTMENT_SLOT, enchantId, duration, 0, player->GetObjectGuid());
        player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, true);

        player->CastSpell(player, kVisualSpell, true);
        player->SaveInventoryAndGoldToDB();

        SendApplyMessage(player, item, slot, oldName, newName);
    }

    void RemoveAllTempEnchants(Player* player)
    {
        if (!player)
            return;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            if (!item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                continue;

            player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, false);
            item->ClearEnchantment(TEMP_ENCHANTMENT_SLOT);
            player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, true);
        }

        player->CastSpell(player, kVisualSpell, true);
        player->SaveInventoryAndGoldToDB();
        player->GetSession()->SendNotification("All temporary weapon enchants were removed.");
    }

    void ShowPoisonerMainMenu(Player* player, GameObject* go)
    {
        if (!player || !go)
            return;

        player->PlayerTalkClass->ClearMenus();

        if (IsWeaponEquippedInSlot(player, EQUIPMENT_SLOT_MAINHAND))
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            std::ostringstream ss;
            ss << "Main Hand [" << GetTempEnchantNameFromItem(item) << "]";
            uint8 icon = (item && item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT)) ? GOSSIP_ICON_INTERACT_1 : GOSSIP_ICON_CHAT;
            player->ADD_GOSSIP_ITEM(icon, ss.str().c_str(), ACTION_MAINHAND_MENU, ACTION_MAINHAND_MENU);
        }

        if (IsWeaponEquippedInSlot(player, EQUIPMENT_SLOT_OFFHAND))
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            std::ostringstream ss;
            ss << "Off Hand [" << GetTempEnchantNameFromItem(item) << "]";
            uint8 icon = (item && item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT)) ? GOSSIP_ICON_INTERACT_1 : GOSSIP_ICON_CHAT;
            player->ADD_GOSSIP_ITEM(icon, ss.str().c_str(), ACTION_OFFHAND_MENU, ACTION_OFFHAND_MENU);
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Remove all temporary weapon enchants", ACTION_CONFIRM_REMOVE_ALL, ACTION_CONFIRM_REMOVE_ALL);
        player->SEND_GOSSIP_MENU(600006, go->GetObjectGuid());
    }

    void ShowSlotMenu(Player* player, GameObject* go, uint8 slot)
    {
        if (!player || !go)
            return;

        if (!IsWeaponEquippedInSlot(player, slot))
        {
            player->GetSession()->SendNotification("No valid weapon is equipped in that slot.");
            ShowPoisonerMainMenu(player, go);
            return;
        }

        LoadTempEnchantOptions();

        player->PlayerTalkClass->ClearMenus();

        Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        std::ostringstream header;
        header << GetSlotName(slot) << ": " << (weapon && weapon->GetProto() ? weapon->GetProto()->Name1 : "Unknown");
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, header.str().c_str(), ACTION_MAIN_MENU, ACTION_MAIN_MENU);

        uint32 currentEnchantId = weapon ? weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) : 0;

        for (size_t i = 0; i < gTempEnchantOptions.size(); ++i)
        {
            TempEnchantOption const& option = gTempEnchantOptions[i];

            if (!CanUseTempEnchantOption(player, slot, option))
                continue;

            SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(option.spellId);
            if (!spellInfo)
                continue;

            uint32 optionEnchantId = 0;
            for (int eff = 0; eff < 3; ++eff)
            {
                if (spellInfo->Effect[eff] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                {
                    optionEnchantId = spellInfo->EffectMiscValue[eff];
                    break;
                }
            }

            std::ostringstream label;
            label << option.itemName << " (Lvl " << option.requiredLevel << ")";

            uint8 gossipIcon = (optionEnchantId && currentEnchantId == optionEnchantId) ? GOSSIP_ICON_INTERACT_1 : GOSSIP_ICON_CHAT;
            player->ADD_GOSSIP_ITEM(gossipIcon, label.str().c_str(), uint32(slot), option.spellId);
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", ACTION_MAIN_MENU, ACTION_MAIN_MENU);
        player->SEND_GOSSIP_MENU(600006, go->GetObjectGuid());
    }
}

bool GossipHello_npc_poisoneer(Player* player, GameObject* go)
{
    ShowPoisonerMainMenu(player, go);
    return true;
}

bool GossipSelect_npc_poisoneer(Player* player, GameObject* go, uint32 sender, uint32 action)
{
    if (!player || !go)
        return true;

    player->PlayerTalkClass->ClearMenus();

    switch (action)
    {
        case ACTION_MAIN_MENU:
            ShowPoisonerMainMenu(player, go);
            return true;
        case ACTION_MAINHAND_MENU:
            ShowSlotMenu(player, go, EQUIPMENT_SLOT_MAINHAND);
            return true;
        case ACTION_OFFHAND_MENU:
            ShowSlotMenu(player, go, EQUIPMENT_SLOT_OFFHAND);
            return true;
        case ACTION_CONFIRM_REMOVE_ALL:
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Yes, remove all temporary weapon enchants.", ACTION_DO_REMOVE_ALL, ACTION_DO_REMOVE_ALL);
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", ACTION_MAIN_MENU, ACTION_MAIN_MENU);
            player->SEND_GOSSIP_MENU(600007, go->GetObjectGuid());
            return true;
        case ACTION_DO_REMOVE_ALL:
            RemoveAllTempEnchants(player);
            ShowPoisonerMainMenu(player, go);
            return true;
        default:
            break;
    }

    if (sender == EQUIPMENT_SLOT_MAINHAND || sender == EQUIPMENT_SLOT_OFFHAND)
    {
        ApplyTempEnchant(player, go, action, uint8(sender));
        ShowSlotMenu(player, go, uint8(sender));
        return true;
    }

    ShowPoisonerMainMenu(player, go);
    return true;
}

void AddSC_npc_poisoneer()
{
    Script* newscript = new Script;
    newscript->Name = "npc_poisoneer";
    newscript->pGOGossipHello = &GossipHello_npc_poisoneer;
    newscript->pGOGossipSelect = &GossipSelect_npc_poisoneer;
    newscript->RegisterSelf();
}
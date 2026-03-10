#include "scriptPCH.h"
#include "Player.h"
#include "Item.h"
#include <sstream>
#include <vector>

namespace
{
    static const uint32 kVisualSpell = 4319;

    static const uint32 ACTION_MAIN_MENU            = 1;
    static const uint32 ACTION_MAINHAND_MENU        = 10;
    static const uint32 ACTION_OFFHAND_MENU         = 11;
    static const uint32 ACTION_CONFIRM_REMOVE_ALL   = 20;
    static const uint32 ACTION_DO_REMOVE_ALL        = 21;

    struct TempEnchantOption
    {
        uint32 spellId;
        const char* text;
        uint8 minPatch;
        bool rogueOnly;
    };

    // Spell IDs were selected from Classic-era temporary weapon enchant / poison spell entries.
    // They are split so the GO remains simple and robust, while still preserving the intended feature set.
    static TempEnchantOption const kGeneralOptions[] =
    {
        { 16138, "Dense Sharpening Stone",   0, false },
        { 16622, "Dense Weightstone",        0, false },
        { 22756, "Elemental Sharpening Stone", 7, false },
        { 25122, "Brilliant Wizard Oil",    7, false },
        { 25123, "Brilliant Mana Oil",      7, false }
    };

    static TempEnchantOption const kRogueOptions[] =
    {
        { 11343, "Instant Poison VI",       0, true  },
        { 25351, "Deadly Poison V",         0, true  },
        { 11202, "Crippling Poison II",     0, true  },
        { 11399, "Mind-numbing Poison III", 0, true  },
        { 13227, "Wound Poison IV",         0, true  }
    };

    bool IsRogue(Player* player)
    {
        if (!player)
            return false;

        return player->GetByteValue(UNIT_FIELD_BYTES_0, 1) == CLASS_ROGUE;
    }

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

    bool CanApplyToSlot(Player* player, uint8 slot, uint32 spellId)
    {
        if (!player || !spellId)
            return false;

        if (!IsWeaponEquippedInSlot(player, slot))
            return false;

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->GetProto())
            return false;

        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
        if (!spellInfo)
            return false;

        // For many temporary enchant spells the masks are already enough.
        // If the masks are empty, keep a conservative weapon-only check.
        if (spellInfo->EquippedItemSubClassMask != 0)
        {
            if (((1 << item->GetProto()->SubClass) & spellInfo->EquippedItemSubClassMask) == 0)
                return false;
        }

        if (spellInfo->EquippedItemInventoryTypeMask != 0)
        {
            if (((1 << item->GetProto()->InventoryType) & spellInfo->EquippedItemInventoryTypeMask) == 0)
                return false;
        }

        return true;
    }

    std::string GetTempEnchantNameFromSpell(uint32 spellId)
    {
        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
        if (!spellInfo)
            return std::string("Unknown temporary enchant");

        if (!spellInfo->SpellName[0].empty())
            return spellInfo->SpellName[0];

        return std::string("Unknown temporary enchant");
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

        // Some cores expose only arrays here; use the first non-empty description slot.
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

    void ApplyTempEnchant(Player* player, GameObject* go, uint32 spellId, uint8 slot)
    {
        if (!player || !go)
            return;

        if (player->IsInCombat())
        {
            player->GetSession()->SendNotification("You cannot apply poisons or temporary weapon enchants while in combat.");
            return;
        }

        if (!CanApplyToSlot(player, slot, spellId))
        {
            player->GetSession()->SendNotification("That temporary enchant cannot be applied to this weapon.");
            return;
        }

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            return;

        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
        if (!spellInfo)
        {
            player->GetSession()->SendNotification("Spell entry not found.");
            return;
        }

        uint32 enchantId = spellInfo->EffectMiscValue[0];
        if (!enchantId)
        {
            player->GetSession()->SendNotification("Temporary enchant data is missing on that spell.");
            return;
        }

        std::string oldName = GetTempEnchantNameFromItem(item);
        uint32 duration = (spellInfo->EffectBasePoints[0] + 1) * IN_MILLISECONDS;

        player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, false);
        item->ClearEnchantment(TEMP_ENCHANTMENT_SLOT);
        item->SetEnchantment(TEMP_ENCHANTMENT_SLOT, enchantId, duration, 0);
        player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, true);

        player->CastSpell(player, kVisualSpell, true);
        player->SaveInventoryAndGoldToDB();

        SendApplyMessage(player, item, slot, oldName, GetTempEnchantNameFromSpell(spellId));
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

    void AddSlotHeader(Player* player, uint8 slot)
    {
        if (!player)
            return;

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->GetProto())
            return;

        std::ostringstream header;
        header << GetSlotName(slot) << ": " << item->GetProto()->Name1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, header.str().c_str(), ACTION_MAIN_MENU, ACTION_MAIN_MENU);
    }

    void ShowPoisonerMainMenu(Player* player, GameObject* go)
    {
        if (!player || !go)
            return;

        player->PlayerTalkClass->ClearMenus();

        if (IsWeaponEquippedInSlot(player, EQUIPMENT_SLOT_MAINHAND))
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Main Hand", ACTION_MAINHAND_MENU, ACTION_MAINHAND_MENU);

        if (IsWeaponEquippedInSlot(player, EQUIPMENT_SLOT_OFFHAND))
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TRAINER, "Off Hand", ACTION_OFFHAND_MENU, ACTION_OFFHAND_MENU);

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

        player->PlayerTalkClass->ClearMenus();

        AddSlotHeader(player, slot);

        for (size_t i = 0; i < sizeof(kGeneralOptions) / sizeof(kGeneralOptions[0]); ++i)
        {
            TempEnchantOption const& option = kGeneralOptions[i];
            if (sWorld.GetWowPatch() < option.minPatch)
                continue;

            if (!CanApplyToSlot(player, slot, option.spellId))
                continue;

            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, option.text, slot, option.spellId);
        }

        if (IsRogue(player))
        {
            for (size_t i = 0; i < sizeof(kRogueOptions) / sizeof(kRogueOptions[0]); ++i)
            {
                TempEnchantOption const& option = kRogueOptions[i];
                if (!CanApplyToSlot(player, slot, option.spellId))
                    continue;

                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, option.text, slot, option.spellId);
            }
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

    // sender is the equipment slot here, action is the temporary enchant spell.
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

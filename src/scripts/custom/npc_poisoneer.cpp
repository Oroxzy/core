#include "scriptPCH.h"
#include "Player.h"
#include "Item.h"
#include <sstream>
#include <vector>
#include <map>

namespace
{
    static const uint32 kVisualSpell = 4319;

    static const uint32 ACTION_MAIN_MENU            = 1;
    static const uint32 ACTION_MAINHAND_MENU        = 10;
    static const uint32 ACTION_OFFHAND_MENU         = 11;
    static const uint32 ACTION_CONFIRM_REMOVE_ALL   = 20;
    static const uint32 ACTION_DO_REMOVE_ALL        = 21;

    enum TempEnchantFamily
    {
        FAMILY_NONE = 0,

        FAMILY_SHARPENING_STONE,
        FAMILY_WEIGHTSTONE,
        FAMILY_WIZARD_OIL,
        FAMILY_MANA_OIL,

        FAMILY_INSTANT_POISON,
        FAMILY_DEADLY_POISON,
        FAMILY_CRIPPLING_POISON,
        FAMILY_MIND_NUMBING_POISON,
        FAMILY_WOUND_POISON
    };

    struct TempEnchantOption
    {
        uint32 spellId;
        const char* text;
        uint8 minPatch;
        uint8 minLevel;
        bool rogueOnly;
        TempEnchantFamily family;
    };

    static TempEnchantOption const kGeneralOptions[] =
    {
        // --------------------------------
        // Sharpening Stones
        // --------------------------------
        { 2828,  "Rough Sharpening Stone",      0,  1, false, FAMILY_SHARPENING_STONE },
        { 2829,  "Coarse Sharpening Stone",     0,  5, false, FAMILY_SHARPENING_STONE },
        { 2830,  "Heavy Sharpening Stone",      0, 15, false, FAMILY_SHARPENING_STONE },
        { 9900,  "Solid Sharpening Stone",      0, 25, false, FAMILY_SHARPENING_STONE },
        { 16138, "Dense Sharpening Stone",      0, 35, false, FAMILY_SHARPENING_STONE },
        { 22756, "Elemental Sharpening Stone",  7, 50, false, FAMILY_SHARPENING_STONE },
    
        // --------------------------------
        // Weightstones
        // --------------------------------
        { 3112,  "Rough Weightstone",           0,  1, false, FAMILY_WEIGHTSTONE },
        { 3113,  "Coarse Weightstone",          0,  5, false, FAMILY_WEIGHTSTONE },
        { 3114,  "Heavy Weightstone",           0, 15, false, FAMILY_WEIGHTSTONE },
        { 9903,  "Solid Weightstone",           0, 25, false, FAMILY_WEIGHTSTONE },
        { 16622, "Dense Weightstone",           0, 35, false, FAMILY_WEIGHTSTONE },
    
        // --------------------------------
        // Wizard Oils
        // --------------------------------
        { 25117, "Minor Wizard Oil",            7,  5, false, FAMILY_WIZARD_OIL },
        { 25119, "Lesser Wizard Oil",           7, 30, false, FAMILY_WIZARD_OIL },
        { 25121, "Wizard Oil",                  7, 40, false, FAMILY_WIZARD_OIL },
        { 25122, "Brilliant Wizard Oil",        7, 45, false, FAMILY_WIZARD_OIL },
    
        // --------------------------------
        // Mana Oils
        // --------------------------------
        { 25118, "Minor Mana Oil",              7, 20, false, FAMILY_MANA_OIL },
        { 25120, "Lesser Mana Oil",             7, 40, false, FAMILY_MANA_OIL },
        { 25123, "Brilliant Mana Oil",          7, 45, false, FAMILY_MANA_OIL },
    
        // --------------------------------
        // Special Vanilla Oils
        // --------------------------------
        { 8017,  "Rockbiter Weapon",            0,  1, false, FAMILY_NONE }, // optional if you want class buffs
        { 8033,  "Frostbrand Weapon",           0, 20, false, FAMILY_NONE },
    
        // --------------------------------
        // Consumable Weapon Oils
        // --------------------------------
        { 16352, "Shadow Oil",                  0, 24, false, FAMILY_NONE },
        { 16355, "Frost Oil",                   0, 24, false, FAMILY_NONE }
    };

    static TempEnchantOption const kRogueOptions[] =
    {
        // --------------------------------
        // Instant Poison
        // --------------------------------
        { 8679,  "Instant Poison I",            0, 20, true,  FAMILY_INSTANT_POISON },
        { 8686,  "Instant Poison II",           0, 28, true,  FAMILY_INSTANT_POISON },
        { 8688,  "Instant Poison III",          0, 36, true,  FAMILY_INSTANT_POISON },
        { 11338, "Instant Poison IV",           0, 44, true,  FAMILY_INSTANT_POISON },
        { 11339, "Instant Poison V",            0, 52, true,  FAMILY_INSTANT_POISON },
        { 11340, "Instant Poison VI",           0, 60, true,  FAMILY_INSTANT_POISON },
    
        // --------------------------------
        // Deadly Poison
        // --------------------------------
        { 2823,  "Deadly Poison I",             0, 30, true,  FAMILY_DEADLY_POISON },
        { 2824,  "Deadly Poison II",            0, 38, true,  FAMILY_DEADLY_POISON },
        { 11355, "Deadly Poison III",           0, 46, true,  FAMILY_DEADLY_POISON },
        { 11356, "Deadly Poison IV",            0, 54, true,  FAMILY_DEADLY_POISON },
        { 25351, "Deadly Poison V",             0, 60, true,  FAMILY_DEADLY_POISON },
    
        // --------------------------------
        // Crippling Poison
        // --------------------------------
        { 3408,  "Crippling Poison I",          0, 20, true,  FAMILY_CRIPPLING_POISON },
        { 11202, "Crippling Poison II",         0, 50, true,  FAMILY_CRIPPLING_POISON },
    
        // --------------------------------
        // Mind Numbing Poison
        // --------------------------------
        { 5761,  "Mind-numbing Poison I",       0, 24, true,  FAMILY_MIND_NUMBING_POISON },
        { 8692,  "Mind-numbing Poison II",      0, 38, true,  FAMILY_MIND_NUMBING_POISON },
        { 11399, "Mind-numbing Poison III",     0, 52, true,  FAMILY_MIND_NUMBING_POISON },
    
        // --------------------------------
        // Wound Poison
        // --------------------------------
        { 13219, "Wound Poison I",              0, 32, true,  FAMILY_WOUND_POISON },
        { 13225, "Wound Poison II",             0, 40, true,  FAMILY_WOUND_POISON },
        { 13226, "Wound Poison III",            0, 48, true,  FAMILY_WOUND_POISON },
        { 13227, "Wound Poison IV",             0, 56, true,  FAMILY_WOUND_POISON }
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
        if (duration == 0)
            duration = 30 * MINUTE * IN_MILLISECONDS;

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

        std::ostringstream active;
        active << "[Active] " << GetTempEnchantNameFromItem(item);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, active.str().c_str(), ACTION_MAIN_MENU, ACTION_MAIN_MENU);
    }

    void AddBestOptionsForSlot(Player* player, uint8 slot, TempEnchantOption const* options, size_t count)
    {
        if (!player)
            return;

        std::map<uint32, TempEnchantOption const*> bestByFamily;

        for (size_t i = 0; i < count; ++i)
        {
            TempEnchantOption const& option = options[i];

            if (player->GetLevel() < option.minLevel)
                continue;

            if (sWorld.GetWowPatch() < option.minPatch)
                continue;

            if (!CanApplyToSlot(player, slot, option.spellId))
                continue;

            std::map<uint32, TempEnchantOption const*>::iterator it = bestByFamily.find(uint32(option.family));
            if (it == bestByFamily.end() || option.minLevel > it->second->minLevel)
                bestByFamily[uint32(option.family)] = &option;
        }

        for (std::map<uint32, TempEnchantOption const*>::const_iterator it = bestByFamily.begin(); it != bestByFamily.end(); ++it)
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, it->second->text, slot, it->second->spellId);
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
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, ss.str().c_str(), ACTION_MAINHAND_MENU, ACTION_MAINHAND_MENU);
        }

        if (IsWeaponEquippedInSlot(player, EQUIPMENT_SLOT_OFFHAND))
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            std::ostringstream ss;
            ss << "Off Hand [" << GetTempEnchantNameFromItem(item) << "]";
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, ss.str().c_str(), ACTION_OFFHAND_MENU, ACTION_OFFHAND_MENU);
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

        player->PlayerTalkClass->ClearMenus();

        AddSlotHeader(player, slot);
        AddBestOptionsForSlot(player, slot, kGeneralOptions, sizeof(kGeneralOptions) / sizeof(kGeneralOptions[0]));

        if (IsRogue(player))
            AddBestOptionsForSlot(player, slot, kRogueOptions, sizeof(kRogueOptions) / sizeof(kRogueOptions[0]));

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
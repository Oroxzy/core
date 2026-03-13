#include "scriptPCH.h"
#include "Player.h"
#include "Item.h"
#include "DBCStores.h"

#include <sstream>
#include <vector>
#include <set>
#include <string>
#include <algorithm>

namespace
{
    static const uint32 kVisualSpell = 4319;

    static const uint32 ACTION_PM_MAIN               = 1;
    static const uint32 ACTION_PM_OPEN_SLOT_BASE     = 1000;
    static const uint32 ACTION_PM_ITEM_PAGE_BASE     = 2000;
    static const uint32 ACTION_PM_MAIN_PAGE_BASE     = 3000;
    static const uint32 ACTION_PM_CLEAR_PROPERTY     = 4000;
    static const uint32 ACTION_PM_APPLY_BASE         = 500000;

    static const size_t kMainItemsPerPage            = 10;
    static const size_t kChoicesPerPage              = 10;
    static const size_t kMaxGossipLabelLength        = 120;

    bool IsEligibleRandomItem(Item* item)
    {
        if (!item || !item->GetProto())
            return false;

        ItemPrototype const* proto = item->GetProto();
        return proto->RandomProperty != 0;
    }

    uint32 GetTemplateRandomEntry(Item* item)
    {
        if (!item || !item->GetProto())
            return 0;

        ItemPrototype const* proto = item->GetProto();
        if (proto->RandomProperty > 0)
            return uint32(proto->RandomProperty);

        return 0;
    }

    std::string TruncateLabel(std::string const& text, size_t maxLen)
    {
        if (text.length() <= maxLen)
            return text;

        if (maxLen <= 3)
            return text.substr(0, maxLen);

        return text.substr(0, maxLen - 3) + "...";
    }

    std::string GetSlotNameText(uint8 slot)
    {
        switch (slot)
        {
            case EQUIPMENT_SLOT_HEAD:      return "Head";
            case EQUIPMENT_SLOT_NECK:      return "Neck";
            case EQUIPMENT_SLOT_SHOULDERS: return "Shoulders";
            case EQUIPMENT_SLOT_BODY:      return "Shirt";
            case EQUIPMENT_SLOT_CHEST:     return "Chest";
            case EQUIPMENT_SLOT_WAIST:     return "Waist";
            case EQUIPMENT_SLOT_LEGS:      return "Legs";
            case EQUIPMENT_SLOT_FEET:      return "Feet";
            case EQUIPMENT_SLOT_WRISTS:    return "Wrists";
            case EQUIPMENT_SLOT_HANDS:     return "Hands";
            case EQUIPMENT_SLOT_FINGER1:   return "Finger 1";
            case EQUIPMENT_SLOT_FINGER2:   return "Finger 2";
            case EQUIPMENT_SLOT_TRINKET1:  return "Trinket 1";
            case EQUIPMENT_SLOT_TRINKET2:  return "Trinket 2";
            case EQUIPMENT_SLOT_BACK:      return "Back";
            case EQUIPMENT_SLOT_MAINHAND:  return "Main Hand";
            case EQUIPMENT_SLOT_OFFHAND:   return "Off Hand";
            case EQUIPMENT_SLOT_RANGED:    return "Ranged";
            case EQUIPMENT_SLOT_TABARD:    return "Tabard";
            default:                       return "Slot";
        }
    }

    std::string GetLocalizedEnchantDescription(Player* player, SpellItemEnchantmentEntry const* enchantEntry)
    {
        if (!enchantEntry)
            return "";

        LocaleConstant loc = LOCALE_enUS;
        if (player && player->GetSession())
            loc = player->GetSession()->GetSessionDbcLocale();

        std::string text;
        if (loc < MAX_DBC_LOCALE && enchantEntry->description[loc] && enchantEntry->description[loc][0] != '\0')
            text = enchantEntry->description[loc];

        if (text.empty() && enchantEntry->description[0] && enchantEntry->description[0][0] != '\0')
            text = enchantEntry->description[0];

        return text;
    }

    std::string GetLocalizedPropertyName(Player* player, ItemRandomPropertiesEntry const* prop)
    {
        if (!prop)
            return "";

        LocaleConstant loc = LOCALE_enUS;
        if (player && player->GetSession())
            loc = player->GetSession()->GetSessionDbcLocale();

        std::string text;
        if (loc < MAX_DBC_LOCALE && prop->nameSuffix[loc] && prop->nameSuffix[loc][0] != '\0')
            text = prop->nameSuffix[loc];

        if (text.empty() && prop->internalName && prop->internalName[0] != '\0')
            text = prop->internalName;

        return text;
    }

    std::string JoinParts(std::vector<std::string> const& parts, std::string const& sep)
    {
        if (parts.empty())
            return "";

        std::ostringstream ss;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i > 0)
                ss << sep;
            ss << parts[i];
        }

        return ss.str();
    }

    std::string DescribeRandomPropertyId(Player* player, int32 propertyId)
    {
        if (propertyId == 0)
            return "None";

        if (propertyId < 0)
        {
            std::ostringstream ss;
            ss << "Suffix ID " << (-propertyId);
            return ss.str();
        }

        ItemRandomPropertiesEntry const* prop = sItemRandomPropertiesStore.LookupEntry(uint32(propertyId));
        if (!prop)
        {
            std::ostringstream ss;
            ss << "Property ID " << propertyId;
            return ss.str();
        }

        std::vector<std::string> parts;

        for (uint32 i = 0; i < 3; ++i)
        {
            if (!prop->enchant_id[i])
                continue;

            SpellItemEnchantmentEntry const* enchantEntry = sSpellItemEnchantmentStore.LookupEntry(prop->enchant_id[i]);
            if (!enchantEntry)
                continue;

            std::string desc = GetLocalizedEnchantDescription(player, enchantEntry);
            if (!desc.empty())
                parts.push_back(desc);
        }

        if (!parts.empty())
            return JoinParts(parts, " / ");

        std::string nameText = GetLocalizedPropertyName(player, prop);
        if (!nameText.empty())
            return nameText;

        std::ostringstream ss;
        ss << "Property ID " << propertyId;
        return ss.str();
    }

    std::string BuildPropertyChoiceLabel(Player* player, uint32 propertyId)
    {
        std::ostringstream ss;

        ItemRandomPropertiesEntry const* prop = sItemRandomPropertiesStore.LookupEntry(propertyId);
        std::string propName = GetLocalizedPropertyName(player, prop);
        std::string statText = DescribeRandomPropertyId(player, int32(propertyId));

        if (!propName.empty() && propName != statText)
            ss << propName << " | ";

        ss << statText << " [" << propertyId << "]";
        return ss.str();
    }

    std::string GetCurrentRandomPropertyText(Player* player, Item* item)
    {
        if (!item)
            return "None";

        int32 current = item->GetItemRandomPropertyId();
        return DescribeRandomPropertyId(player, current);
    }

    std::vector<uint32> LoadAvailableProperties(Item* item)
    {
        std::vector<uint32> out;
        if (!item)
            return out;

        uint32 entry = GetTemplateRandomEntry(item);
        if (!entry)
            return out;

        auto result = WorldDatabase.PQuery(
            "SELECT ench FROM item_enchantment_template WHERE ((%u >= patch_min) && (%u <= patch_max)) AND entry='%u' ORDER BY ench ASC",
            sWorld.GetWowPatch(),
            sWorld.GetWowPatch(),
            entry
        );
        if (!result)
            return out;

        std::set<uint32> seen;
        do
        {
            Field* fields = result->Fetch();
            if (!fields)
                continue;

            uint32 ench = fields[0].GetUInt32();
            if (!ench)
                continue;

            if (seen.insert(ench).second)
                out.push_back(ench);
        }
        while (result->NextRow());

        return out;
    }

    std::vector<uint8> GetEligibleSlots(Player* player)
    {
        std::vector<uint8> slots;
        if (!player)
            return slots;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (IsEligibleRandomItem(item))
                slots.push_back(slot);
        }

        return slots;
    }

    void ClearRandomProperty(Player* player, Item* item, uint8 /*slot*/)
    {
        if (!player || !item)
            return;

        for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < PROP_ENCHANTMENT_SLOT_0 + 3; ++i)
            player->ApplyEnchantment(item, EnchantmentSlot(i), false);

        item->SetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID, 0);
        item->SetUInt32Value(ITEM_FIELD_PROPERTY_SEED, 0);

        for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < PROP_ENCHANTMENT_SLOT_0 + 3; ++i)
            item->ClearEnchantment(EnchantmentSlot(i));

        item->SetState(ITEM_CHANGED, player);
        player->CastSpell(player, kVisualSpell, true);
        player->SaveInventoryAndGoldToDB();
    }

    void ApplyRandomProperty(Player* player, Item* item, uint8 /*slot*/, int32 propertyId)
    {
        if (!player || !item)
            return;

        for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < PROP_ENCHANTMENT_SLOT_0 + 3; ++i)
            player->ApplyEnchantment(item, EnchantmentSlot(i), false);

        if (propertyId > 0)
        {
            item->SetItemRandomProperties(propertyId);
        }
        else
        {
            item->SetInt32Value(ITEM_FIELD_RANDOM_PROPERTIES_ID, 0);
            item->SetUInt32Value(ITEM_FIELD_PROPERTY_SEED, 0);

            for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < PROP_ENCHANTMENT_SLOT_0 + 3; ++i)
                item->ClearEnchantment(EnchantmentSlot(i));
        }

        for (uint32 i = PROP_ENCHANTMENT_SLOT_0; i < PROP_ENCHANTMENT_SLOT_0 + 3; ++i)
            player->ApplyEnchantment(item, EnchantmentSlot(i), true);

        item->SetState(ITEM_CHANGED, player);
        player->CastSpell(player, kVisualSpell, true);
        player->SaveInventoryAndGoldToDB();
    }

    void ShowPropertyMainMenu(Player* player, GameObject* go, size_t page)
    {
        if (!player || !go)
            return;

        player->PlayerTalkClass->ClearMenus();

        std::vector<uint8> slots = GetEligibleSlots(player);
        if (slots.empty())
        {
            player->GetSession()->SendNotification("No equipped items with random property template found.");
            player->SEND_GOSSIP_MENU(600005, go->GetObjectGuid());
            return;
        }

        size_t start = page * kMainItemsPerPage;
        if (start >= slots.size())
            start = 0;

        size_t end = start + kMainItemsPerPage;
        if (end > slots.size())
            end = slots.size();

        for (size_t i = start; i < end; ++i)
        {
            uint8 slot = slots[i];
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item || !item->GetProto())
                continue;

            std::ostringstream label;
            label << GetSlotNameText(slot) << ": " << item->GetProto()->Name1 << " [" << GetCurrentRandomPropertyText(player, item) << "]";

            std::string finalLabel = TruncateLabel(label.str(), kMaxGossipLabelLength);
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, finalLabel.c_str(), 0, ACTION_PM_OPEN_SLOT_BASE + slot);
        }

        if (start > 0)
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Previous page", 0, ACTION_PM_MAIN_PAGE_BASE + uint32(page - 1));

        if (end < slots.size())
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "Next page ->", 0, ACTION_PM_MAIN_PAGE_BASE + uint32(page + 1));

        player->SEND_GOSSIP_MENU(600005, go->GetObjectGuid());
    }

    void ShowPropertyItemMenu(Player* player, GameObject* go, uint8 slot, size_t page)
    {
        if (!player || !go)
            return;

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!IsEligibleRandomItem(item))
        {
            player->GetSession()->SendNotification("This equipped item has no random property template.");
            ShowPropertyMainMenu(player, go, 0);
            return;
        }

        std::vector<uint32> choices = LoadAvailableProperties(item);
        if (choices.empty())
        {
            player->GetSession()->SendNotification("No random property choices were found for this item.");
            ShowPropertyMainMenu(player, go, 0);
            return;
        }

        size_t start = page * kChoicesPerPage;
        if (start >= choices.size())
            start = 0;

        size_t end = start + kChoicesPerPage;
        if (end > choices.size())
            end = choices.size();

        int32 currentPropertyId = item->GetItemRandomPropertyId();

        player->PlayerTalkClass->ClearMenus();

        {
            std::ostringstream info;
            info << item->GetProto()->Name1 << " (" << GetSlotNameText(slot) << ") - current: " << GetCurrentRandomPropertyText(player, item);
            player->GetSession()->SendNotification("%s", info.str().c_str());
        }

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Clear random property", slot, ACTION_PM_CLEAR_PROPERTY);

        for (size_t i = start; i < end; ++i)
        {
            uint32 propertyId = choices[i];
            std::string label = BuildPropertyChoiceLabel(player, propertyId);
            std::string finalLabel = TruncateLabel(label, kMaxGossipLabelLength);

            uint8 icon = (currentPropertyId == int32(propertyId)) ? GOSSIP_ICON_TAXI : GOSSIP_ICON_CHAT;
            player->ADD_GOSSIP_ITEM(icon, finalLabel.c_str(), slot, ACTION_PM_APPLY_BASE + propertyId);
        }

        if (start > 0)
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Previous page", slot, ACTION_PM_ITEM_PAGE_BASE + uint32(page - 1));

        if (end < choices.size())
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "Next page ->", slot, ACTION_PM_ITEM_PAGE_BASE + uint32(page + 1));

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", 0, ACTION_PM_MAIN);
        player->SEND_GOSSIP_MENU(600006, go->GetObjectGuid());
    }

    void SetPropertyOnSlot(Player* player, GameObject* go, uint8 slot, uint32 choiceId)
    {
        if (!player || !go)
            return;

        if (player->IsInCombat())
        {
            player->GetSession()->SendNotification("You cannot change random item properties while in combat.");
            return;
        }

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!IsEligibleRandomItem(item))
        {
            player->GetSession()->SendNotification("This equipped item is not eligible for random property changes.");
            ShowPropertyMainMenu(player, go, 0);
            return;
        }

        std::vector<uint32> choices = LoadAvailableProperties(item);
        bool found = false;
        for (size_t i = 0; i < choices.size(); ++i)
        {
            if (choices[i] == choiceId)
            {
                found = true;
                break;
            }
        }

        if (!found)
        {
            player->GetSession()->SendNotification("That random property is not valid for this item.");
            ShowPropertyItemMenu(player, go, slot, 0);
            return;
        }

        std::string oldText = GetCurrentRandomPropertyText(player, item);

        ApplyRandomProperty(player, item, slot, int32(choiceId));

        std::ostringstream ss;
        ss << item->GetProto()->Name1 << " on " << GetSlotNameText(slot) << " changed from " << oldText << " to " << DescribeRandomPropertyId(player, int32(choiceId)) << ".";
        player->GetSession()->SendNotification("%s", ss.str().c_str());
    }

    void ClearPropertyOnSlot(Player* player, GameObject* go, uint8 slot)
    {
        if (!player || !go)
            return;

        if (player->IsInCombat())
        {
            player->GetSession()->SendNotification("You cannot change random item properties while in combat.");
            return;
        }

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->GetProto())
        {
            ShowPropertyMainMenu(player, go, 0);
            return;
        }

        std::string oldText = GetCurrentRandomPropertyText(player, item);
        ClearRandomProperty(player, item, slot);

        std::ostringstream ss;
        ss << "Random property on " << item->GetProto()->Name1 << " (" << GetSlotNameText(slot) << ") was cleared. Previous value: " << oldText << ".";
        player->GetSession()->SendNotification("%s", ss.str().c_str());
    }
}

bool GossipHello_npc_property_manager(Player* player, GameObject* go)
{
    if (!player || !go)
        return false;

    if (player->IsInCombat())
    {
        player->GetSession()->SendNotification("You are in combat!");
        return true;
    }

    ShowPropertyMainMenu(player, go, 0);
    return true;
}

bool GossipSelect_npc_property_manager(Player* player, GameObject* go, uint32 sender, uint32 action)
{
    if (!player || !go)
        return true;

    player->PlayerTalkClass->ClearMenus();

    if (action == ACTION_PM_MAIN)
    {
        ShowPropertyMainMenu(player, go, 0);
        return true;
    }

    if (action >= ACTION_PM_MAIN_PAGE_BASE && action < ACTION_PM_CLEAR_PROPERTY)
    {
        uint32 page = action - ACTION_PM_MAIN_PAGE_BASE;
        ShowPropertyMainMenu(player, go, page);
        return true;
    }

    if (action >= ACTION_PM_OPEN_SLOT_BASE && action < ACTION_PM_ITEM_PAGE_BASE)
    {
        uint8 slot = uint8(action - ACTION_PM_OPEN_SLOT_BASE);
        ShowPropertyItemMenu(player, go, slot, 0);
        return true;
    }

    if (action == ACTION_PM_CLEAR_PROPERTY)
    {
        ClearPropertyOnSlot(player, go, uint8(sender));
        ShowPropertyItemMenu(player, go, uint8(sender), 0);
        return true;
    }

    if (action >= ACTION_PM_ITEM_PAGE_BASE && action < ACTION_PM_MAIN_PAGE_BASE)
    {
        uint32 page = action - ACTION_PM_ITEM_PAGE_BASE;
        ShowPropertyItemMenu(player, go, uint8(sender), page);
        return true;
    }

    if (action >= ACTION_PM_APPLY_BASE)
    {
        uint32 choiceId = action - ACTION_PM_APPLY_BASE;
        SetPropertyOnSlot(player, go, uint8(sender), choiceId);
        ShowPropertyItemMenu(player, go, uint8(sender), 0);
        return true;
    }

    ShowPropertyMainMenu(player, go, 0);
    return true;
}

void AddSC_npc_property_manager()
{
    Script* newscript = new Script;
    newscript->Name = "npc_property_manager";
    newscript->pGOGossipHello = &GossipHello_npc_property_manager;
    newscript->pGOGossipSelect = &GossipSelect_npc_property_manager;
    newscript->RegisterSelf();
}
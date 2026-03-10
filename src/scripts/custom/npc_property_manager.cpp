#include "scriptPCH.h"
#include "Player.h"
#include "Item.h"
#include <sstream>
#include <vector>
#include <set>

namespace
{
    static const uint32 kVisualSpell = 4319;

    static const uint32 ACTION_PM_MAIN              = 1;
    static const uint32 ACTION_PM_SELECT_ITEM       = 2;
    static const uint32 ACTION_PM_APPLY_BASE        = 500000;
    static const uint32 ACTION_PM_PAGE_BASE         = 800000;
    static const uint32 ACTION_PM_CLEAR_PROPERTY    = 900000;

    static const size_t kChoicesPerPage = 20;

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

    bool UsesRandomSuffix(Item* /*item*/)
    {
        return false;
    }

    std::string GetCurrentRandomPropertyText(Item* item)
    {
        if (!item)
            return std::string("None");

        int32 current = item->GetItemRandomPropertyId();
        if (current == 0)
            return std::string("None");

        std::ostringstream ss;
        if (current > 0)
            ss << "Property ID " << current;
        else
            ss << "Suffix ID " << -current;
        return ss.str();
    }

    std::vector<uint32> LoadAvailableProperties(Item* item)
    {
        std::vector<uint32> out;
        if (!item)
            return out;

        uint32 entry = GetTemplateRandomEntry(item);
        if (!entry)
            return out;

        auto result = WorldDatabase.PQuery("SELECT ench FROM item_enchantment_template WHERE entry='%u' ORDER BY chance DESC, ench ASC", entry);
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

    const char* GetSlotName(uint8 slot)
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

    void ShowPropertyMainMenu(Player* player, GameObject* go)
    {
        if (!player || !go)
            return;

        player->PlayerTalkClass->ClearMenus();

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!IsEligibleRandomItem(item))
                continue;

            std::ostringstream label;
            label << GetSlotName(slot) << ": " << item->GetProto()->Name1 << " (" << GetCurrentRandomPropertyText(item) << ")";
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, label.str().c_str(), slot, ACTION_PM_SELECT_ITEM);
        }

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
            ShowPropertyMainMenu(player, go);
            return;
        }

        std::vector<uint32> choices = LoadAvailableProperties(item);
        if (choices.empty())
        {
            player->GetSession()->SendNotification("No random property choices were found for this item in item_enchantment_template.");
            ShowPropertyMainMenu(player, go);
            return;
        }

        size_t start = page * kChoicesPerPage;
        if (start >= choices.size())
            start = 0;

        size_t end = start + kChoicesPerPage;
        if (end > choices.size())
            end = choices.size();

        player->PlayerTalkClass->ClearMenus();

        std::ostringstream header;
        header << item->GetProto()->Name1 << " - current: " << GetCurrentRandomPropertyText(item);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, header.str().c_str(), ACTION_PM_MAIN, ACTION_PM_MAIN);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Clear random property / suffix", slot, ACTION_PM_CLEAR_PROPERTY);

        for (size_t i = start; i < end; ++i)
        {
            std::ostringstream label;
            if (UsesRandomSuffix(item))
                label << "Apply suffix ID " << choices[i];
            else
                label << "Apply property ID " << choices[i];

            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, label.str().c_str(), slot, ACTION_PM_APPLY_BASE + choices[i]);
        }

        if (start > 0)
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Previous page", slot, ACTION_PM_PAGE_BASE + uint32(page - 1));

        if (end < choices.size())
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "Next page ->", slot, ACTION_PM_PAGE_BASE + uint32(page + 1));

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", ACTION_PM_MAIN, ACTION_PM_MAIN);
        player->SEND_GOSSIP_MENU(600006, go->GetObjectGuid());
    }

    void ApplyRandomProperty(Player* player, Item* item, uint8 slot, int32 propertyId)
    {
        if (!player || !item)
            return;

        item->SetItemRandomProperties(propertyId);
        item->SetState(ITEM_CHANGED, player);
        player->CastSpell(player, kVisualSpell, true);
        player->SaveInventoryAndGoldToDB();
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
            ShowPropertyMainMenu(player, go);
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

        int32 appliedId = UsesRandomSuffix(item) ? -int32(choiceId) : int32(choiceId);
        std::string oldText = GetCurrentRandomPropertyText(item);
        ApplyRandomProperty(player, item, slot, appliedId);

        std::ostringstream ss;
        ss << item->GetProto()->Name1 << " on " << GetSlotName(slot) << " changed from " << oldText << " to Property ID " << choiceId << ".";
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
        if (!item)
            return;

        std::string oldText = GetCurrentRandomPropertyText(item);
        ApplyRandomProperty(player, item, slot, 0);

        std::ostringstream ss;
        ss << "Random property on " << item->GetProto()->Name1 << " (" << GetSlotName(slot) << ") was cleared. Previous value: " << oldText << ".";
        player->GetSession()->SendNotification("%s", ss.str().c_str());
    }
}

bool GossipHello_npc_property_manager(Player* player, GameObject* go)
{
    ShowPropertyMainMenu(player, go);
    return true;
}

bool GossipSelect_npc_property_manager(Player* player, GameObject* go, uint32 sender, uint32 action)
{
    if (!player || !go)
        return true;

    player->PlayerTalkClass->ClearMenus();

    if (action == ACTION_PM_MAIN)
    {
        ShowPropertyMainMenu(player, go);
        return true;
    }

    if (action == ACTION_PM_SELECT_ITEM)
    {
        ShowPropertyItemMenu(player, go, uint8(sender), 0);
        return true;
    }

    if (action == ACTION_PM_CLEAR_PROPERTY)
    {
        ClearPropertyOnSlot(player, go, uint8(sender));
        ShowPropertyItemMenu(player, go, uint8(sender), 0);
        return true;
    }

    if (action >= ACTION_PM_PAGE_BASE && action < ACTION_PM_CLEAR_PROPERTY)
    {
        uint32 page = action - ACTION_PM_PAGE_BASE;
        ShowPropertyItemMenu(player, go, uint8(sender), page);
        return true;
    }

    if (action >= ACTION_PM_APPLY_BASE && action < ACTION_PM_PAGE_BASE)
    {
        uint32 choiceId = action - ACTION_PM_APPLY_BASE;
        SetPropertyOnSlot(player, go, uint8(sender), choiceId);
        ShowPropertyItemMenu(player, go, uint8(sender), 0);
        return true;
    }

    ShowPropertyMainMenu(player, go);
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

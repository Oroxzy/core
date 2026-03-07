/**
Script made by jeremymeile for uterusone.net
*/

#include "scriptPCH.h"
#include "Player.h"

// spell defines

#define COOL_VISUAL_SPELL_1     14867
#define COOL_VISUAL_SPELL_2     19473
#define REMOVE_ENCHANTS         99

void EnchantItem(Player* player, GameObject* gameobject, uint32 spellid, uint8 slot)
{
    Item* pItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!pItem)
    {
        player->GetSession()->SendNotification("Your item could not be enchanted, there is no item equipped in the specifified slot.");
        return;
    }
    SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellid);
    if (!spellInfo)
    {
        player->GetSession()->SendNotification("Invalid spellid.");
        return;
    }
    uint32 enchantid = spellInfo->EffectMiscValue[0];
    if (!enchantid)
    {
        player->GetSession()->SendNotification("Invalid spellid.");
        return;
    }

    if (!((1 << pItem->GetProto()->SubClass) & spellInfo->EquippedItemSubClassMask) &&
        !((1 << pItem->GetProto()->InventoryType) & spellInfo->EquippedItemInventoryTypeMask))
    {
        player->GetSession()->SendNotification("Your item could not be enchanted, wrong item type equipped.");

        return;
    }

    player->ApplyEnchantment(pItem, PERM_ENCHANTMENT_SLOT, false);
    pItem->SetEnchantment(PERM_ENCHANTMENT_SLOT, enchantid, 0, 0);
    player->ApplyEnchantment(pItem, PERM_ENCHANTMENT_SLOT, true);
    player->CastSpell(player, COOL_VISUAL_SPELL_1, true);
    player->CastSpell(player, COOL_VISUAL_SPELL_2, true);
    player->GetSession()->SendAreaTriggerMessage("Your item was enchanted successfully!");
}

void RemoveEnchantItem(Player* player, GameObject* gameobject)
{

    if (player->isInCombat())
    {
        player->GetSession()->SendNotification("You are in combat!");
        return;
    }

    player->PlayerTalkClass->ClearMenus();

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* equippedItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i);

        if (equippedItem)
        {
            player->ApplyEnchantment(equippedItem, PERM_ENCHANTMENT_SLOT, false);
            equippedItem->ClearEnchantment(PERM_ENCHANTMENT_SLOT);
            player->ApplyEnchantment(equippedItem, PERM_ENCHANTMENT_SLOT, true);
        }
    }
    player->CastSpell(player, COOL_VISUAL_SPELL_1, true);
    player->CastSpell(player, COOL_VISUAL_SPELL_2, true);
}

uint32 CheckEnchantID(Player* player, uint8 slot)
{
    Item* pItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);

    uint32 enchant_id = pItem->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
    if (enchant_id)
    {
        SpellItemEnchantmentEntry const* enchantEntry = sSpellItemEnchantmentStore.LookupEntry(enchant_id);
        uint32 enchant_id = enchantEntry->ID;

        return enchant_id;
    }
    else
        return 0;
}

bool GossipHello_EnchanterNPC(Player* player, GameObject* gameobject)
{
    if (player->isInCombat())
    {
        player->GetSession()->SendNotification("You are in combat!");
        return true;
    }

    player->PlayerTalkClass->ClearMenus();

    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        Item* equippedItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
        uint8 GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;

        if (equippedItem)
        {
            uint32 itemId = equippedItem->GetEntry();
            ItemPrototype const* item_proto = ObjectMgr::GetItemPrototype(itemId);

            if (equippedItem->GetEnchantmentId(PERM_ENCHANTMENT_SLOT))
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;

            switch (item_proto->InventoryType)
            {
            case INVTYPE_HEAD:
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_HEAD);
                break;
            case INVTYPE_SHOULDERS:
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_SHOULDERS);
                break;
            case INVTYPE_CLOAK:
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_CLOAK);
                break;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_CHEST);
                break;
            case INVTYPE_LEGS:
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_LEGS);
                break;
            case INVTYPE_FEET:
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_FEET);
                break;
            case INVTYPE_WRISTS:
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_WRISTS);
                break;
            case INVTYPE_HANDS:
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_HANDS);
                break;
            case INVTYPE_2HWEAPON:
            case INVTYPE_WEAPON:
            case INVTYPE_WEAPONMAINHAND:
            case INVTYPE_WEAPONOFFHAND:
            {
                if (i == EQUIPMENT_SLOT_MAINHAND)
                {
                    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_WEAPON);
                }

                if (player->CanDualWield() && i == EQUIPMENT_SLOT_OFFHAND)
                {
                    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_WEAPON + 200);
                }

                break;
            };
            case INVTYPE_SHIELD:
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_SHIELD);
                break;
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_RANGED:
                if (item_proto->SubClass == ITEM_SUBCLASS_WEAPON_CROSSBOW || item_proto->SubClass == ITEM_SUBCLASS_WEAPON_GUN || item_proto->SubClass == ITEM_SUBCLASS_WEAPON_BOW)
                {
                    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, item_proto->Name1, GOSSIP_SENDER_MAIN, INVTYPE_RANGEDRIGHT);
                }
                break;
            }
        }
    }

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "Remove all my Enchants.", GOSSIP_SENDER_MAIN, REMOVE_ENCHANTS);

    player->SEND_GOSSIP_MENU(600005, gameobject->GetObjectGuid());

    return true;
}

bool GossipSelect_EnchanterNPC(Player* player, GameObject* gameobject, uint32 uiSender, uint32 uiAction)
{
    player->PlayerTalkClass->ClearMenus();
    uint8 GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;

    if (player->isInCombat())
    {
        player->GetSession()->SendNotification("You are in combat!");
        return true;
    }
    else if (uiAction == INVTYPE_HEAD)
    {
        if (sWorld.GetWowPatch() >= 5)
        {
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) != 2591 && player->getClass() == CLASS_DRUID)   { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Intellect +10, Stamina +10, Healing Spells +24",   EQUIPMENT_SLOT_HEAD, 24168); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) != 2586 && player->getClass() == CLASS_HUNTER)  { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Ranged Attack Power +24, Stamina +10, Hit +1%",    EQUIPMENT_SLOT_HEAD, 24162); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) != 2588 && player->getClass() == CLASS_MAGE)    { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Healing and Spell Damage +18, Spell Hit +1%",      EQUIPMENT_SLOT_HEAD, 24164); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) != 2584 && player->getClass() == CLASS_PALADIN) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Defense +7, Stamina +10, Healing Spells +24",      EQUIPMENT_SLOT_HEAD, 24160); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) != 2590 && player->getClass() == CLASS_PRIEST)  { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Mana Regen +4, Stamina +10, Healing Spells +24",   EQUIPMENT_SLOT_HEAD, 24167); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) != 2585 && player->getClass() == CLASS_ROGUE)   { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Attack Power +28, Dodge +1%",                      EQUIPMENT_SLOT_HEAD, 24161); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) != 2587 && player->getClass() == CLASS_SHAMAN)  { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Healing and Spell Damage +13, Intellect +15",      EQUIPMENT_SLOT_HEAD, 24163); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) != 2589 && player->getClass() == CLASS_WARLOCK) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Healing and Spell Damage +18, Stamina +10",        EQUIPMENT_SLOT_HEAD, 24165); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) != 2583 && player->getClass() == CLASS_WARRIOR) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Defense +7, Stamina + 10, Block value +15",        EQUIPMENT_SLOT_HEAD, 24149); }

            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2591 && player->getClass() == CLASS_DRUID)   { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Intellect +10, Stamina +10, Healing Spells +24", EQUIPMENT_SLOT_HEAD, 24168); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2586 && player->getClass() == CLASS_HUNTER)  { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Ranged Attack Power +24, Stamina +10, Hit +1%",  EQUIPMENT_SLOT_HEAD, 24162); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2588 && player->getClass() == CLASS_MAGE)    { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Healing and Spell Damage +18, Spell Hit +1%",        EQUIPMENT_SLOT_HEAD, 24164); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2584 && player->getClass() == CLASS_PALADIN) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Defense +7, Stamina +10, Healing Spells +24",        EQUIPMENT_SLOT_HEAD, 24160); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2590 && player->getClass() == CLASS_PRIEST)  { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Mana Regen +4, Stamina +10, Healing Spells +24", EQUIPMENT_SLOT_HEAD, 24167); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2585 && player->getClass() == CLASS_ROGUE)   { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Attack Power +28, Dodge +1%",                        EQUIPMENT_SLOT_HEAD, 24161); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2587 && player->getClass() == CLASS_SHAMAN)  { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Healing and Spell Damage +13, Intellect +15",        EQUIPMENT_SLOT_HEAD, 24163); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2589 && player->getClass() == CLASS_WARLOCK) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Healing and Spell Damage +18, Stamina +10",      EQUIPMENT_SLOT_HEAD, 24165); }
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2583 && player->getClass() == CLASS_WARRIOR) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Defense +7, Stamina + 10, Block value +15",      EQUIPMENT_SLOT_HEAD, 24149); }
        }
        if (sWorld.GetWowPatch() >= 1)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2544)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET,        "Healing and Spell Damage +8", EQUIPMENT_SLOT_HEAD, 22844);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2545)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET,        "Dogde +1%", EQUIPMENT_SLOT_HEAD, 22846);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2543)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET,        "Attack Speed +1%", EQUIPMENT_SLOT_HEAD, 22840);
        }
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 1503)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "HP +100",         EQUIPMENT_SLOT_HEAD, 15389);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 1483)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Mana +150",       EQUIPMENT_SLOT_HEAD, 15340);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 1504)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Armor +125",      EQUIPMENT_SLOT_HEAD, 15391);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 1506)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Strength +8",     EQUIPMENT_SLOT_HEAD, 15397);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 1507)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Stamina +8",      EQUIPMENT_SLOT_HEAD, 15400);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 1508)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +8",      EQUIPMENT_SLOT_HEAD, 15402);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 1509)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Intellect +8",    EQUIPMENT_SLOT_HEAD, 15404);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 1510)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spirit +8",       EQUIPMENT_SLOT_HEAD, 15406);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "Next ->",            EQUIPMENT_SLOT_HEAD, INVTYPE_HEAD + 100);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",            EQUIPMENT_SLOT_HEAD, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());

    }
    else if (uiAction == INVTYPE_HEAD + 100)
    {
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 1505)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+20 Fire Resistance", EQUIPMENT_SLOT_HEAD, 15394);

        if (sWorld.GetWowPatch() >= 9)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2681)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+10 Nature Resistance", EQUIPMENT_SLOT_HEAD, 28161);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2682)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+10 Frost Resistance", EQUIPMENT_SLOT_HEAD, 28163);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HEAD) == 2683)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+10 Shadow Resistance", EQUIPMENT_SLOT_HEAD, 28165);
        }
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                EQUIPMENT_SLOT_HEAD, INVTYPE_HEAD);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());

    }
    else if (uiAction == INVTYPE_SHOULDERS)
    {
        if (sWorld.GetWowPatch() >= 9)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_SHOULDERS) == 2721)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spell Damage +15 and +1% Spell Critical Strike", EQUIPMENT_SLOT_SHOULDERS, 29467);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_SHOULDERS) == 2715)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Healing +31 and 5 mana per 5 sec.", EQUIPMENT_SLOT_SHOULDERS, 29475);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_SHOULDERS) == 2717)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Attack Power +26 and 1% Critical Strike", EQUIPMENT_SLOT_SHOULDERS, 29483);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_SHOULDERS) == 2716)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Stamina +16 and Armor +100", EQUIPMENT_SLOT_SHOULDERS, 29480);
        }
        if (sWorld.GetWowPatch() >= 5)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_SHOULDERS) == 2606)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Attack Power +30", EQUIPMENT_SLOT_SHOULDERS, 24422);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_SHOULDERS) == 2605)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Healing and Spell Damage +18", EQUIPMENT_SLOT_SHOULDERS, 24421);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_SHOULDERS) == 2604)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Healing Spells +33", EQUIPMENT_SLOT_SHOULDERS, 24420);
        }
        if (sWorld.GetWowPatch() >= 1)
        {
            if (CheckEnchantID(player, EQUIPMENT_SLOT_SHOULDERS) == 2488)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+5 All Resistance", EQUIPMENT_SLOT_SHOULDERS, 22599);
        }
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                                        EQUIPMENT_SLOT_SHOULDERS, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_CLOAK)
    {
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_BACK) == 1889)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Armor +70", EQUIPMENT_SLOT_BACK, 20015);

        if (sWorld.GetWowPatch() >= 7)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_BACK) == 2621)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Threat -2%", EQUIPMENT_SLOT_BACK, 25084);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_BACK) == 2620)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+15 Nature Resistance", EQUIPMENT_SLOT_BACK, 25082);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_BACK) == 2619)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+15 Fire Resistance", EQUIPMENT_SLOT_BACK, 25081);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_BACK) == 910)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Increased Stealth", EQUIPMENT_SLOT_BACK, 25083);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_BACK) == 2622)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Dodge +1%", EQUIPMENT_SLOT_BACK, 25086);
        }
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_BACK) == 804)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+10 Shadow Resistance", EQUIPMENT_SLOT_BACK, 13522);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_BACK) == 1888)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+5 All Resistance", EQUIPMENT_SLOT_BACK, 20014);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_BACK) == 849)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +3", EQUIPMENT_SLOT_BACK, 13882);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                EQUIPMENT_SLOT_BACK, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_CHEST)
    {
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_CHEST) == 1893)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Mana +100", EQUIPMENT_SLOT_CHEST, 20028);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_CHEST) == 1891)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "All Stats +4", EQUIPMENT_SLOT_CHEST, 20025);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_CHEST) == 1892)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Health +100", EQUIPMENT_SLOT_CHEST, 20026);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",        EQUIPMENT_SLOT_CHEST, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_WRISTS)
    {
        if (sWorld.GetWowPatch() >= 4)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_WRISTS) == 2566)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Healing Spells +24", EQUIPMENT_SLOT_WRISTS, 23802);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_WRISTS) == 2565)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Mana Regen 4 per 5 sec.", EQUIPMENT_SLOT_WRISTS, 23801);
        }
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_WRISTS) == 1886)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Stamina +9", EQUIPMENT_SLOT_WRISTS, 20011);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_WRISTS) == 1885)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Strength +9", EQUIPMENT_SLOT_WRISTS, 20010);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_WRISTS) == 1884)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spirit +9", EQUIPMENT_SLOT_WRISTS, 20009);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_WRISTS) == 1883)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Intellect +7", EQUIPMENT_SLOT_WRISTS, 20008);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_WRISTS) == 923)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Defense +3", EQUIPMENT_SLOT_WRISTS, 13931);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_WRISTS) == 247)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +1", EQUIPMENT_SLOT_WRISTS, 7779);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                    EQUIPMENT_SLOT_WRISTS, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_HANDS)
    {
        if (sWorld.GetWowPatch() >= 7)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 2564)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +15", EQUIPMENT_SLOT_HANDS, 25080);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 2613)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Threat +2%", EQUIPMENT_SLOT_HANDS, 25072);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 2617)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Healing Spells +30", EQUIPMENT_SLOT_HANDS, 25079);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 2615)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Frost Damage +20", EQUIPMENT_SLOT_HANDS, 25074);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 2616)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Fire Damage +20", EQUIPMENT_SLOT_HANDS, 25078);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 2614)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Shadow Damage +20", EQUIPMENT_SLOT_HANDS, 25073);
        }
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 931)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Attack Speed +1%", EQUIPMENT_SLOT_HANDS, 13948);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 927)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Strength +7", EQUIPMENT_SLOT_HANDS, 20013);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 1887)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +7", EQUIPMENT_SLOT_HANDS, 20012);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 930)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Minor Mount Speed Increase", EQUIPMENT_SLOT_HANDS, 13947);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 909)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Herbalism skill +5", EQUIPMENT_SLOT_HANDS, 13868);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 865)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Skinning skill +5", EQUIPMENT_SLOT_HANDS, 13698);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 846)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Fishing skill +2", EQUIPMENT_SLOT_HANDS, 13620);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_HANDS) == 906)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Mining skill +5", EQUIPMENT_SLOT_HANDS, 13841);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                    EQUIPMENT_SLOT_HANDS, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_LEGS)
    {
    if (sWorld.GetWowPatch() >= 5)
    {
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) != 2591 && player->getClass() == CLASS_DRUID) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Intellect +10, Stamina +10, Healing Spells +24", EQUIPMENT_SLOT_LEGS, 24168); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) != 2586 && player->getClass() == CLASS_HUNTER) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Ranged Attack Power +24, Stamina +10, Hit +1%", EQUIPMENT_SLOT_LEGS, 24162); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) != 2588 && player->getClass() == CLASS_MAGE) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Healing and Spell Damage +18, Spell Hit +1%", EQUIPMENT_SLOT_LEGS, 24164); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) != 2584 && player->getClass() == CLASS_PALADIN) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Defense +7, Stamina +10, Healing Spells +24", EQUIPMENT_SLOT_LEGS, 24160); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) != 2590 && player->getClass() == CLASS_PRIEST) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Mana Regen +4, Stamina +10, Healing Spells +24", EQUIPMENT_SLOT_LEGS, 24167); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) != 2585 && player->getClass() == CLASS_ROGUE) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Attack Power +28, Dodge +1%", EQUIPMENT_SLOT_LEGS, 24161); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) != 2587 && player->getClass() == CLASS_SHAMAN) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Healing and Spell Damage +13, Intellect +15", EQUIPMENT_SLOT_LEGS, 24163); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) != 2589 && player->getClass() == CLASS_WARLOCK) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Healing and Spell Damage +18, Stamina +10", EQUIPMENT_SLOT_LEGS, 24165); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) != 2583 && player->getClass() == CLASS_WARRIOR) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Defense +7, Stamina + 10, Block value +15", EQUIPMENT_SLOT_LEGS, 24149); }

        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2591 && player->getClass() == CLASS_DRUID) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Intellect +10, Stamina +10, Healing Spells +24", EQUIPMENT_SLOT_LEGS, 24168); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2586 && player->getClass() == CLASS_HUNTER) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Ranged Attack Power +24, Stamina +10, Hit +1%", EQUIPMENT_SLOT_LEGS, 24162); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2588 && player->getClass() == CLASS_MAGE) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Healing and Spell Damage +18, Spell Hit +1%", EQUIPMENT_SLOT_LEGS, 24164); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2584 && player->getClass() == CLASS_PALADIN) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Defense +7, Stamina +10, Healing Spells +24", EQUIPMENT_SLOT_LEGS, 24160); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2590 && player->getClass() == CLASS_PRIEST) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Mana Regen +4, Stamina +10, Healing Spells +24", EQUIPMENT_SLOT_LEGS, 24167); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2585 && player->getClass() == CLASS_ROGUE) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Attack Power +28, Dodge +1%", EQUIPMENT_SLOT_LEGS, 24161); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2587 && player->getClass() == CLASS_SHAMAN) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Healing and Spell Damage +13, Intellect +15", EQUIPMENT_SLOT_LEGS, 24163); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2589 && player->getClass() == CLASS_WARLOCK) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Healing and Spell Damage +18, Stamina +10", EQUIPMENT_SLOT_LEGS, 24165); }
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2583 && player->getClass() == CLASS_WARRIOR) { player->ADD_GOSSIP_ITEM(GOSSIP_ICON_INTERACT_1, "Defense +7, Stamina + 10, Block value +15", EQUIPMENT_SLOT_LEGS, 24149); }
    }
    if (sWorld.GetWowPatch() >= 1)
    {
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2544)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Healing and Spell Damage +8", EQUIPMENT_SLOT_LEGS, 22844);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2545)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Dogde +1%", EQUIPMENT_SLOT_LEGS, 22846);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2543)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Attack Speed +1%", EQUIPMENT_SLOT_LEGS, 22840);
    }
    GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
    if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 1503)
        GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "HP +100", EQUIPMENT_SLOT_LEGS, 15389);
    GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
    if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 1483)
        GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Mana +150", EQUIPMENT_SLOT_LEGS, 15340);
    GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
    if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 1504)
        GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Armor +125", EQUIPMENT_SLOT_LEGS, 15391);
    GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
    if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 1506)
        GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Strength +8", EQUIPMENT_SLOT_LEGS, 15397);
    GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
    if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 1507)
        GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Stamina +8", EQUIPMENT_SLOT_LEGS, 15400);
    GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
    if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 1508)
        GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +8", EQUIPMENT_SLOT_LEGS, 15402);
    GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
    if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 1509)
        GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Intellect +8", EQUIPMENT_SLOT_LEGS, 15404);
    GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
    if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 1510)
        GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spirit +8", EQUIPMENT_SLOT_LEGS, 15406);

    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "Next ->", EQUIPMENT_SLOT_LEGS, INVTYPE_LEGS + 100);
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", EQUIPMENT_SLOT_LEGS, 0);
    player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());

    }
    else if (uiAction == INVTYPE_LEGS + 100)
    {
    GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
    if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 1505)
        GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+20 Fire Resistance", EQUIPMENT_SLOT_LEGS, 15394);

    if (sWorld.GetWowPatch() >= 9)
    {
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2681)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+10 Nature Resistance", EQUIPMENT_SLOT_LEGS, 28161);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2682)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+10 Frost Resistance", EQUIPMENT_SLOT_LEGS, 28163);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_LEGS) == 2683)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+10 Shadow Resistance", EQUIPMENT_SLOT_LEGS, 28165);
    }
    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back", EQUIPMENT_SLOT_LEGS, INVTYPE_LEGS);
    player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());

    }
    else if (uiAction == INVTYPE_FEET)
    {
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_FEET) == 1887)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +7", EQUIPMENT_SLOT_FEET, 20023);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_FEET) == 851)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spirit +5", EQUIPMENT_SLOT_FEET, 20024);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_FEET) == 929)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Stamina +7", EQUIPMENT_SLOT_FEET, 20020);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_FEET) == 911)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Minor Speed Increase", EQUIPMENT_SLOT_FEET, 13890);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_FEET) == 464)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Mount Speed +4%", EQUIPMENT_SLOT_FEET, 9783);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                EQUIPMENT_SLOT_FEET, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_WEAPON)
    {
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 37)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Immune Disarm", EQUIPMENT_SLOT_MAINHAND, 7220);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 1900)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Crusader", EQUIPMENT_SLOT_MAINHAND, 20034);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 1898)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Lifestealing", EQUIPMENT_SLOT_MAINHAND, 20032);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 1899)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Unholy Weapon", EQUIPMENT_SLOT_MAINHAND, 20033);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 1894)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Icy Chill", EQUIPMENT_SLOT_MAINHAND, 20029);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 803)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Fiery Weapon", EQUIPMENT_SLOT_MAINHAND, 13898);

        if (sWorld.GetWowPatch() >= 4)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 2567)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spirit +20", EQUIPMENT_SLOT_MAINHAND, 23803);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 2568)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Intellect +22", EQUIPMENT_SLOT_MAINHAND, 23804);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 2563)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Strength  +15", EQUIPMENT_SLOT_MAINHAND, 23799);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 2564)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +15", EQUIPMENT_SLOT_MAINHAND, 23800);
        }
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "Next ->",                EQUIPMENT_SLOT_MAINHAND, INVTYPE_WEAPON + 100);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                EQUIPMENT_SLOT_MAINHAND, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_WEAPON + 100)
    {
        if (sWorld.GetWowPatch() >= 1)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 2505)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Healing Power +55", EQUIPMENT_SLOT_MAINHAND, 22750);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 2504)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spell Power +30", EQUIPMENT_SLOT_MAINHAND, 22749);
        }
        if (player->IsTwoHandUsed())
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 1904)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Intellect +9", EQUIPMENT_SLOT_MAINHAND, 20036);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 1896)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Weapon Damage +9", EQUIPMENT_SLOT_MAINHAND, 20030);

            if (sWorld.GetWowPatch() >= 8)
            {
                GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
                if (CheckEnchantID(player, EQUIPMENT_SLOT_MAINHAND) == 2646)
                    GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                    player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +25", EQUIPMENT_SLOT_MAINHAND, 27837);
            }
        }
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                EQUIPMENT_SLOT_MAINHAND, INVTYPE_WEAPON);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_WEAPON + 200)
    {
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 37)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Immune Disarm", EQUIPMENT_SLOT_OFFHAND, 7220);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 1900)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Crusader", EQUIPMENT_SLOT_OFFHAND, 20034);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 1898)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Lifestealing", EQUIPMENT_SLOT_OFFHAND, 20032);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 1899)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Unholy Weapon", EQUIPMENT_SLOT_OFFHAND, 20033);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 1894)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Icy Chill", EQUIPMENT_SLOT_OFFHAND, 20029);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 803)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Fiery Weapon", EQUIPMENT_SLOT_OFFHAND, 13898);

        if (sWorld.GetWowPatch() >= 4)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 2567)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spirit +20", EQUIPMENT_SLOT_OFFHAND, 23803);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 2568)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Intellect +22", EQUIPMENT_SLOT_OFFHAND, 23804);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 2563)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Strength  +15", EQUIPMENT_SLOT_OFFHAND, 23799);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 2564)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Agility +15", EQUIPMENT_SLOT_OFFHAND, 23800);
        }
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "Next ->",                EQUIPMENT_SLOT_OFFHAND, INVTYPE_WEAPON + 400);
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                EQUIPMENT_SLOT_OFFHAND, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
        }
        else if (uiAction == INVTYPE_WEAPON + 400)
        {
        if (sWorld.GetWowPatch() >= 1)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 2505)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Healing Power +55", EQUIPMENT_SLOT_OFFHAND, 22750);
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 2504)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
                player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spell Power +30", EQUIPMENT_SLOT_OFFHAND, 22749);
        }
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                EQUIPMENT_SLOT_OFFHAND, INVTYPE_WEAPON + 200);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_RANGEDRIGHT)
    {
        if (sWorld.GetWowPatch() >= 1)
        {
            GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
            if (CheckEnchantID(player, EQUIPMENT_SLOT_RANGED) == 2523)
                GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+3% Ranged Hit", EQUIPMENT_SLOT_RANGED, 22779);
        }

        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_RANGED) == 664)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Ranged Damage +7", EQUIPMENT_SLOT_RANGED, 12460);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                EQUIPMENT_SLOT_RANGED, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == INVTYPE_SHIELD)
    {
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 1704)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Thorium Spike", EQUIPMENT_SLOT_OFFHAND, 16623);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 929)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Stamina +7", EQUIPMENT_SLOT_OFFHAND, 20017);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 1890)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Spirit +9", EQUIPMENT_SLOT_OFFHAND, 20016);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 926)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "+8 Frost Resistance", EQUIPMENT_SLOT_OFFHAND, 13933);
        GOSSIP_ICON_SET = GOSSIP_ICON_CHAT;
        if (CheckEnchantID(player, EQUIPMENT_SLOT_OFFHAND) == 863)
            GOSSIP_ICON_SET = GOSSIP_ICON_INTERACT_1;
        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_SET, "Blocking +2%", EQUIPMENT_SLOT_OFFHAND, 13689);

        player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Back",                EQUIPMENT_SLOT_OFFHAND, 0);
        player->SEND_GOSSIP_MENU(600006, gameobject->GetObjectGuid());
    }
    else if (uiAction == REMOVE_ENCHANTS)
    {
        RemoveEnchantItem(player, gameobject);
        GossipHello_EnchanterNPC(player, gameobject);
    }
    else if (uiAction == 0)
        //player->CLOSE_GOSSIP_MENU();
        GossipHello_EnchanterNPC(player, gameobject);
    else
    {
        EnchantItem(player, gameobject, uiAction, uiSender);
        //player->CLOSE_GOSSIP_MENU();
        GossipHello_EnchanterNPC(player, gameobject);
    }
    return true;
}

void AddSC_npc_enchanter()
{
    Script *newscript;
    newscript = new Script;
    newscript->Name = "npc_enchanter";
    newscript->pGOGossipHello = &GossipHello_EnchanterNPC;
    newscript->pGOGossipSelect = &GossipSelect_EnchanterNPC;
    newscript->RegisterSelf();
}
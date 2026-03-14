#include "scriptPCH.h"
#include "Player.h"
#include "Item.h"
#include "SpellMgr.h"
#include "DBCStores.h"
#include "ObjectMgr.h"

#include <map>
#include <vector>
#include <sstream>
#include <algorithm>
#include <string>
#include <cctype>

namespace
{
    // ------------------------------------------------------------
    // Configuration
    // ------------------------------------------------------------

    // Visual feedback spell after apply/remove.
    static const uint32 kVisualSpell = 4319;

    // Maximum number of real enchant options shown per page.
    // Keep this low enough so the total gossip item count stays below 32.
    static const size_t kSlotOptionsPerPage = 10;

    // Maximum label length for cleaner gossip rendering.
    static const size_t kMaxGossipLabelLength = 100;

    // If true, source item usability is checked as well.
    // For a strict "player can really use this source item" behavior, keep true.
    // For a more GM/dev-oriented utility behavior, set false.
    static const bool kRequireSourceItemUsable = true;

    // ------------------------------------------------------------
    // Gossip actions
    // ------------------------------------------------------------

    static const uint32 ACTION_MAIN_MENU               = 1;
    static const uint32 ACTION_MAINHAND_MENU           = 10;
    static const uint32 ACTION_OFFHAND_MENU            = 11;
    static const uint32 ACTION_CONFIRM_REMOVE_ALL      = 20;
    static const uint32 ACTION_DO_REMOVE_ALL           = 21;
    static const uint32 ACTION_REMOVE_CURRENT          = 30;

    // sender = equipment slot
    // action = ACTION_SLOT_PAGE_BASE + page
    static const uint32 ACTION_SLOT_PAGE_BASE          = 1000;

    // sender = equipment slot
    // action = ACTION_APPLY_SPELL_BASE + spellId
    static const uint32 ACTION_APPLY_SPELL_BASE        = 100000;

    // ------------------------------------------------------------
    // Data structure for one available temp enchant source
    // ------------------------------------------------------------

    struct TempEnchantOption
    {
        uint32 itemEntry;
        uint32 spellId;
        uint32 requiredLevel;
        std::string itemName;
        std::string spellName;
    };

    static bool gTempEnchantOptionsLoaded = false;
    static std::vector<TempEnchantOption> gTempEnchantOptions;

    // ------------------------------------------------------------
    // Cache helpers
    // ------------------------------------------------------------

    void ResetTempEnchantCache()
    {
        gTempEnchantOptionsLoaded = false;
        gTempEnchantOptions.clear();
    }

    // ------------------------------------------------------------
    // String helpers
    // ------------------------------------------------------------

    std::string ToLowerCopy(std::string const& input)
    {
        std::string out = input;
        for (size_t i = 0; i < out.length(); ++i)
            out[i] = char(std::tolower(static_cast<unsigned char>(out[i])));
        return out;
    }

    bool ContainsInsensitive(std::string const& haystack, std::string const& needle)
    {
        return ToLowerCopy(haystack).find(ToLowerCopy(needle)) != std::string::npos;
    }

    std::string TruncateLabel(std::string const& text, size_t maxLen)
    {
        if (text.length() <= maxLen)
            return text;

        if (maxLen <= 3)
            return text.substr(0, maxLen);

        return text.substr(0, maxLen - 3) + "...";
    }

    // ------------------------------------------------------------
    // Basic slot / weapon helpers
    // ------------------------------------------------------------

    const char* GetSlotName(uint8 slot)
    {
        switch (slot)
        {
            case EQUIPMENT_SLOT_MAINHAND: return "Main Hand";
            case EQUIPMENT_SLOT_OFFHAND:  return "Off Hand";
            default:                      return "Unknown Slot";
        }
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

    bool HasAnyTempEnchant(Player* player)
    {
        if (!player)
            return false;

        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            if (item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
                return true;
        }

        return false;
    }

    uint32 GetTempEnchantIdFromSpell(uint32 spellId)
    {
        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
        if (!spellInfo)
            return 0;

        for (int i = 0; i < 3; ++i)
        {
            if (spellInfo->Effect[i] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                return spellInfo->EffectMiscValue[i];
        }

        return 0;
    }

    std::string GetTempEnchantNameFromItem(Item* item)
    {
        if (!item)
            return "No weapon";

        uint32 enchantId = item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
        if (!enchantId)
            return "None";

        SpellItemEnchantmentEntry const* enchant = sSpellItemEnchantmentStore.LookupEntry(enchantId);
        if (!enchant)
        {
            std::ostringstream ss;
            ss << "Enchant ID " << enchantId;
            return ss.str();
        }

        for (int i = 0; i < 3; ++i)
        {
            if (enchant->description[i] && enchant->description[i][0] != '\0')
                return enchant->description[i];
        }

        std::ostringstream ss;
        ss << "Enchant ID " << enchantId;
        return ss.str();
    }

    // ------------------------------------------------------------
    // Family grouping
    //
    // Goal:
    // - collapse multiple ranks into one family
    // - show only the best usable option for the player's level
    //
    // The final fallback groups by enchant ID if possible, which makes
    // the grouping more robust for custom names / weird data.
    // ------------------------------------------------------------

    std::string GetOptionFamilyKey(TempEnchantOption const& option)
    {
        std::string spell = ToLowerCopy(option.spellName);
        std::string item  = ToLowerCopy(option.itemName);

        if (spell.find("instant poison") != std::string::npos || item.find("instant poison") != std::string::npos)
            return "instant_poison";

        if (spell.find("deadly poison") != std::string::npos || item.find("deadly poison") != std::string::npos)
            return "deadly_poison";

        if (spell.find("wound poison") != std::string::npos || item.find("wound poison") != std::string::npos)
            return "wound_poison";

        if (spell.find("crippling poison") != std::string::npos || item.find("crippling poison") != std::string::npos)
            return "crippling_poison";

        if (spell.find("mind-numbing poison") != std::string::npos || item.find("mind-numbing poison") != std::string::npos || item.find("mind numbing poison") != std::string::npos)
            return "mind_numbing_poison";

        if (spell.find("scorpid poison") != std::string::npos || item.find("scorpid poison") != std::string::npos)
            return "scorpid_poison";

        if (spell.find("wizard oil") != std::string::npos || item.find("wizard oil") != std::string::npos)
            return "wizard_oil";

        if (spell.find("mana oil") != std::string::npos || item.find("mana oil") != std::string::npos)
            return "mana_oil";

        if (spell.find("sharpen") != std::string::npos || item.find("sharpening stone") != std::string::npos)
            return "sharpening_stone";

        if (spell.find("weightstone") != std::string::npos || item.find("weightstone") != std::string::npos)
            return "weightstone";

        if (spell.find("windfury") != std::string::npos || item.find("windfury") != std::string::npos)
            return "windfury_weapon";

        if (spell.find("rockbiter") != std::string::npos || item.find("rockbiter") != std::string::npos)
            return "rockbiter_weapon";

        if (spell.find("frostbrand") != std::string::npos || item.find("frostbrand") != std::string::npos)
            return "frostbrand_weapon";

        if (spell.find("flametongue") != std::string::npos || item.find("flametongue") != std::string::npos)
            return "flametongue_weapon";

        uint32 enchantId = GetTempEnchantIdFromSpell(option.spellId);
        if (enchantId)
        {
            std::ostringstream ss;
            ss << "enchant_" << enchantId;
            return ss.str();
        }

        return "spell_" + ToLowerCopy(option.spellName);
    }

    // ------------------------------------------------------------
    // Validation
    // ------------------------------------------------------------

    bool CanUseTempEnchantOption(Player* player, uint8 slot, TempEnchantOption const& option)
    {
        if (!player)
            return false;

        if (!IsWeaponEquippedInSlot(player, slot))
            return false;

        Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!weapon || !weapon->GetProto())
            return false;

        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(option.spellId);
        if (!spellInfo)
            return false;

        if (kRequireSourceItemUsable)
        {
            ItemPrototype const* sourceProto = sObjectMgr.GetItemPrototype(option.itemEntry);
            if (!sourceProto)
                return false;

            if (player->CanUseItem(sourceProto, false) != EQUIP_ERR_OK)
                return false;
        }

        if (!weapon->IsFitToSpellRequirements(spellInfo))
            return false;

        return true;
    }

    // ------------------------------------------------------------
    // Load all temp enchant options from the in-memory item store
    //
    // item_template is already loaded into ObjectMgr on startup, and
    // GetItemPrototypeMap() exposes the full in-memory map.
    // ------------------------------------------------------------

    void LoadTempEnchantOptions()
    {
        if (gTempEnchantOptionsLoaded)
            return;

        gTempEnchantOptionsLoaded = true;
        gTempEnchantOptions.clear();

        std::map<uint32, TempEnchantOption> bestBySpellId;
        ItemPrototypeMap const& itemMap = sObjectMgr.GetItemPrototypeMap();

        for (ItemPrototypeMap::const_iterator itr = itemMap.begin(); itr != itemMap.end(); ++itr)
        {
            ItemPrototype const* proto = &itr->second;
            if (!proto)
                continue;

            if (!proto->Name1 || proto->Name1[0] == '\0')
                continue;

            std::string itemName = proto->Name1;
            uint32 itemEntry = proto->ItemId;
            uint32 reqLevel = proto->RequiredLevel;

            for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
            {
                uint32 spellId = proto->Spells[i].SpellId;
                uint32 trigger = proto->Spells[i].SpellTrigger;

                if (!spellId)
                    continue;

                if (trigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;

                SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(spellId);
                if (!spellInfo)
                    continue;

                bool isTempEnchant = false;
                for (int eff = 0; eff < 3; ++eff)
                {
                    if (spellInfo->Effect[eff] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                    {
                        isTempEnchant = true;
                        break;
                    }
                }

                if (!isTempEnchant)
                    continue;

                std::string spellName = spellInfo->SpellName[0];
                if (spellName.empty())
                    continue;

                // Filter obvious junk / legacy / debug entries
                if (ContainsInsensitive(spellName, "zzold") ||
                    ContainsInsensitive(spellName, "(dnd)") ||
                    ContainsInsensitive(spellName, "unused") ||
                    ContainsInsensitive(spellName, "test"))
                {
                    continue;
                }

                TempEnchantOption opt;
                opt.itemEntry      = itemEntry;
                opt.spellId        = spellId;
                opt.requiredLevel  = reqLevel;
                opt.itemName       = itemName;
                opt.spellName      = spellName;

                std::map<uint32, TempEnchantOption>::iterator found = bestBySpellId.find(spellId);
                if (found == bestBySpellId.end())
                {
                    bestBySpellId[spellId] = opt;
                }
                else
                {
                    // Prefer the lower required level source item for the same spell.
                    if (opt.requiredLevel < found->second.requiredLevel)
                        found->second = opt;
                }
            }
        }

        for (std::map<uint32, TempEnchantOption>::const_iterator itr = bestBySpellId.begin(); itr != bestBySpellId.end(); ++itr)
            gTempEnchantOptions.push_back(itr->second);

        std::sort(gTempEnchantOptions.begin(), gTempEnchantOptions.end(),
            [](TempEnchantOption const& a, TempEnchantOption const& b) -> bool
            {
                if (a.requiredLevel != b.requiredLevel)
                    return a.requiredLevel < b.requiredLevel;

                if (a.spellName != b.spellName)
                    return a.spellName < b.spellName;

                return a.spellId < b.spellId;
            });
    }

    // ------------------------------------------------------------
    // Build the final list shown to the player:
    // - valid for slot/weapon
    // - valid for player level
    // - best option per family
    // ------------------------------------------------------------

    std::vector<TempEnchantOption> GetBestOptionsForPlayerLevel(Player* player, uint8 slot)
    {
        std::vector<TempEnchantOption> filtered;
        if (!player)
            return filtered;

        LoadTempEnchantOptions();

        uint32 playerLevel = player->GetLevel();
        std::map<std::string, TempEnchantOption> bestByFamily;

        for (size_t i = 0; i < gTempEnchantOptions.size(); ++i)
        {
            TempEnchantOption const& opt = gTempEnchantOptions[i];

            if (!CanUseTempEnchantOption(player, slot, opt))
                continue;

            if (opt.requiredLevel > playerLevel)
                continue;

            std::string family = GetOptionFamilyKey(opt);

            std::map<std::string, TempEnchantOption>::iterator found = bestByFamily.find(family);
            if (found == bestByFamily.end())
            {
                bestByFamily[family] = opt;
            }
            else
            {
                bool replace = false;

                // Prefer the highest requirement still usable for the player's level.
                if (opt.requiredLevel > found->second.requiredLevel)
                    replace = true;
                else if (opt.requiredLevel == found->second.requiredLevel && opt.spellName < found->second.spellName)
                    replace = true;

                if (replace)
                    found->second = opt;
            }
        }

        for (std::map<std::string, TempEnchantOption>::const_iterator itr = bestByFamily.begin(); itr != bestByFamily.end(); ++itr)
            filtered.push_back(itr->second);

        std::sort(filtered.begin(), filtered.end(),
            [](TempEnchantOption const& a, TempEnchantOption const& b) -> bool
            {
                if (a.requiredLevel != b.requiredLevel)
                    return a.requiredLevel < b.requiredLevel;

                if (a.spellName != b.spellName)
                    return a.spellName < b.spellName;

                return a.spellId < b.spellId;
            });

        return filtered;
    }

    // ------------------------------------------------------------
    // Apply one temp enchant
    // ------------------------------------------------------------

    void ApplyTempEnchant(Player* player, uint32 spellId, uint8 slot)
    {
        if (!player)
            return;

        if (player->IsInCombat())
        {
            player->GetSession()->SendNotification("You cannot apply temporary weapon enchants while in combat.");
            return;
        }

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || !item->GetProto())
        {
            player->GetSession()->SendNotification("No valid weapon was found in that slot.");
            return;
        }

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
            player->GetSession()->SendNotification("Temporary enchant data is missing.");
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

        item->SetState(ITEM_CHANGED, player);
        player->CastSpell(player, kVisualSpell, true);
        player->SaveInventoryAndGoldToDB();

        std::ostringstream ss;
        ss << GetSlotName(slot) << ": " << item->GetProto()->Name1
           << " changed from " << oldName << " to " << newName << ".";
        player->GetSession()->SendNotification("%s", ss.str().c_str());
    }

    // ------------------------------------------------------------
    // Remove temp enchant from one slot
    // ------------------------------------------------------------

    void RemoveTempEnchantFromSlot(Player* player, uint8 slot)
    {
        if (!player)
            return;

        if (player->IsInCombat())
        {
            player->GetSession()->SendNotification("You cannot remove temporary weapon enchants while in combat.");
            return;
        }

        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
        {
            player->GetSession()->SendNotification("No valid weapon was found in that slot.");
            return;
        }

        if (!item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
        {
            player->GetSession()->SendNotification("No temporary enchant is active on that slot.");
            return;
        }

        player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, false);
        item->ClearEnchantment(TEMP_ENCHANTMENT_SLOT);
        player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, true);

        item->SetState(ITEM_CHANGED, player);
        player->CastSpell(player, kVisualSpell, true);
        player->SaveInventoryAndGoldToDB();

        std::ostringstream ss;
        ss << "Removed temporary enchant from " << GetSlotName(slot) << ".";
        player->GetSession()->SendNotification("%s", ss.str().c_str());
    }

    // ------------------------------------------------------------
    // Remove temp enchants from all equipped items
    // ------------------------------------------------------------

    void RemoveAllTempEnchants(Player* player)
    {
        if (!player)
            return;

        if (player->IsInCombat())
        {
            player->GetSession()->SendNotification("You cannot remove temporary weapon enchants while in combat.");
            return;
        }

        bool removedAny = false;

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
            item->SetState(ITEM_CHANGED, player);

            removedAny = true;
        }

        if (!removedAny)
        {
            player->GetSession()->SendNotification("No temporary weapon enchants were active.");
            return;
        }

        player->CastSpell(player, kVisualSpell, true);
        player->SaveInventoryAndGoldToDB();
        player->GetSession()->SendNotification("All temporary weapon enchants were removed.");
    }

    // ------------------------------------------------------------
    // Main menu
    // ------------------------------------------------------------

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
            uint8 icon = (item && item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT)) ? GOSSIP_ICON_TAXI : GOSSIP_ICON_CHAT;
            player->ADD_GOSSIP_ITEM(icon, ss.str().c_str(), ACTION_MAINHAND_MENU, ACTION_MAINHAND_MENU);
        }

        if (IsWeaponEquippedInSlot(player, EQUIPMENT_SLOT_OFFHAND))
        {
            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            std::ostringstream ss;
            ss << "Off Hand [" << GetTempEnchantNameFromItem(item) << "]";
            uint8 icon = (item && item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT)) ? GOSSIP_ICON_TAXI : GOSSIP_ICON_CHAT;
            player->ADD_GOSSIP_ITEM(icon, ss.str().c_str(), ACTION_OFFHAND_MENU, ACTION_OFFHAND_MENU);
        }

        if (HasAnyTempEnchant(player))
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Remove all temporary weapon enchants", ACTION_CONFIRM_REMOVE_ALL, ACTION_CONFIRM_REMOVE_ALL);

        player->SEND_GOSSIP_MENU(600006, go->GetObjectGuid());
    }

    // ------------------------------------------------------------
    // Slot submenu with real pagination
    // ------------------------------------------------------------

    void ShowSlotMenu(Player* player, GameObject* go, uint8 slot, size_t page)
    {
        if (!player || !go)
            return;

        if (!IsWeaponEquippedInSlot(player, slot))
        {
            player->GetSession()->SendNotification("No valid weapon is equipped in that slot.");
            ShowPoisonerMainMenu(player, go);
            return;
        }

        Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        std::vector<TempEnchantOption> options = GetBestOptionsForPlayerLevel(player, slot);

        if (options.empty())
        {
            player->GetSession()->SendNotification("No suitable temporary enchant options were found for your current level and weapon.");
            ShowPoisonerMainMenu(player, go);
            return;
        }

        size_t totalOptions = options.size();
        size_t totalPages = (totalOptions + kSlotOptionsPerPage - 1) / kSlotOptionsPerPage;

        size_t safePage = page;
        if (safePage >= totalPages)
            safePage = 0;

        size_t start = safePage * kSlotOptionsPerPage;
        size_t end = start + kSlotOptionsPerPage;
        if (end > totalOptions)
            end = totalOptions;

        player->PlayerTalkClass->ClearMenus();

        {
            std::ostringstream ss;
            ss << GetSlotName(slot) << ": "
               << (weapon && weapon->GetProto() ? weapon->GetProto()->Name1 : "Unknown")
               << " - current: " << GetTempEnchantNameFromItem(weapon)
               << " - page " << (safePage + 1) << "/" << totalPages;
            player->GetSession()->SendNotification("%s", ss.str().c_str());
        }

        if (weapon && weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT))
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_CHAT, "Remove current temporary enchant", slot, ACTION_REMOVE_CURRENT);

        uint32 currentEnchantId = weapon ? weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) : 0;

        for (size_t i = start; i < end; ++i)
        {
            TempEnchantOption const& option = options[i];
            uint32 optionEnchantId = GetTempEnchantIdFromSpell(option.spellId);

            std::ostringstream label;
            label << option.spellName << " - " << option.itemName << " (Lvl " << option.requiredLevel << ")";

            std::string finalLabel = TruncateLabel(label.str(), kMaxGossipLabelLength);
            uint8 icon = (optionEnchantId && optionEnchantId == currentEnchantId) ? GOSSIP_ICON_TAXI : GOSSIP_ICON_CHAT;

            player->ADD_GOSSIP_ITEM(icon, finalLabel.c_str(), uint32(slot), ACTION_APPLY_SPELL_BASE + option.spellId);
        }

        if (safePage > 0)
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "<- Previous page", uint32(slot), ACTION_SLOT_PAGE_BASE + uint32(safePage - 1));

        if ((safePage + 1) < totalPages)
            player->ADD_GOSSIP_ITEM(GOSSIP_ICON_TALK, "Next page ->", uint32(slot), ACTION_SLOT_PAGE_BASE + uint32(safePage + 1));

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
            ShowSlotMenu(player, go, EQUIPMENT_SLOT_MAINHAND, 0);
            return true;

        case ACTION_OFFHAND_MENU:
            ShowSlotMenu(player, go, EQUIPMENT_SLOT_OFFHAND, 0);
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

        case ACTION_REMOVE_CURRENT:
            if (sender == EQUIPMENT_SLOT_MAINHAND || sender == EQUIPMENT_SLOT_OFFHAND)
            {
                RemoveTempEnchantFromSlot(player, uint8(sender));
                ShowSlotMenu(player, go, uint8(sender), 0);
                return true;
            }
            break;

        default:
            break;
    }

    if ((sender == EQUIPMENT_SLOT_MAINHAND || sender == EQUIPMENT_SLOT_OFFHAND) &&
        action >= ACTION_APPLY_SPELL_BASE)
    {
        uint32 spellId = action - ACTION_APPLY_SPELL_BASE;
        ApplyTempEnchant(player, spellId, uint8(sender));
        ShowSlotMenu(player, go, uint8(sender), 0);
        return true;
    }

    if ((sender == EQUIPMENT_SLOT_MAINHAND || sender == EQUIPMENT_SLOT_OFFHAND) &&
        action >= ACTION_SLOT_PAGE_BASE)
    {
        uint32 page = action - ACTION_SLOT_PAGE_BASE;
        ShowSlotMenu(player, go, uint8(sender), page);
        return true;
    }

    ShowPoisonerMainMenu(player, go);
    return true;
}

void AddSC_npc_poisoneer()
{
    ResetTempEnchantCache();

    Script* newscript = new Script;
    newscript->Name = "npc_poisoneer";
    newscript->pGOGossipHello = &GossipHello_npc_poisoneer;
    newscript->pGOGossipSelect = &GossipSelect_npc_poisoneer;
    newscript->RegisterSelf();
}
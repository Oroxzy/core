#include "PlayerAutoProgression.h"

#include "Creature.h"
#include "DBCStores.h"
#include "GameEventMgr.h"
#include "Item.h"
#include "LFGMgr.h"
#include "LootMgr.h"
#include "Map.h"
#include "SQLStorages.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PoolManager.h"
#include "QuestDef.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellMgr.h"
#include "World.h"
#ifdef MANGOS_DEBUG
#include "Timer.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct TrainerSource
{
    uint32 id = 0;
    bool isTemplate = false;

    bool operator==(TrainerSource const& other) const
    {
        return id == other.id && isTemplate == other.isTemplate;
    }
};

struct TrainerCache
{
    std::vector<TrainerSource> classes[12];
    std::vector<TrainerSource> weapons;
};
enum class ImprovementSource : uint8
{
    None,
    Enchanting,
    Scope,
    ShieldSpike,
    ArmorKit,
    Libram,
    Zandalar,
    ArgentDawn,
    Naxxramas
};

struct EnchantCandidate
{
    uint32 spellId = 0;
    uint32 enchantId = 0;
    uint32 sourceItemId = 0;
    ImprovementSource source = ImprovementSource::None;
    uint8 effectIndex = 0;
};

struct QuestRewardSource
{
    uint32 questId = 0;
    uint32 count = 0;
    bool choice = false;
};

enum ItemSourceMask : uint32
{
    ITEM_SOURCE_NONE = 0x00,
    ITEM_SOURCE_VENDOR = 0x01,
    ITEM_SOURCE_CRAFTED = 0x02,
    ITEM_SOURCE_QUEST = 0x04,
    ITEM_SOURCE_WORLD_LOOT = 0x08,
    ITEM_SOURCE_DUNGEON_LOOT = 0x10,
    ITEM_SOURCE_RAID_LOOT = 0x20,
    ITEM_SOURCE_OTHER = 0x40,
    ITEM_SOURCE_RESTRICTED = 0x80
};

struct AutoProgressionCache
{
    TrainerCache trainers;
    std::vector<EnchantCandidate> improvements;
    std::vector<uint32> equipmentItems;
    std::unordered_map<uint32, uint32> itemSources;
    std::unordered_map<uint32, std::vector<QuestRewardSource>> rewardQuests;
    std::set<uint32> activeQuestRewardItems;
    std::unordered_map<uint32, uint32> itemSetSizes;
};

struct AutoProgressionCacheState
{
    std::mutex mutex;
    std::condition_variable condition;
    uint32 activeUpdates = 0;
    uint32 activeReaders = 0;
    uint32 waitingUpdates = 0;
    std::thread::id updateThread;
    bool dirty = true;
    std::shared_ptr<AutoProgressionCache const> snapshot;
};

AutoProgressionCacheState& GetAutoProgressionCacheState()
{
    static AutoProgressionCacheState state;
    return state;
}

void BeginCacheUpdate()
{
    AutoProgressionCacheState& state = GetAutoProgressionCacheState();
    std::unique_lock<std::mutex> lock(state.mutex);
    std::thread::id const current = std::this_thread::get_id();
    if (state.activeUpdates && state.updateThread == current)
    {
        ++state.activeUpdates;
        state.dirty = true;
        return;
    }
    ++state.waitingUpdates;
    state.condition.wait(lock, [&state]()
    {
        return state.activeReaders == 0 && state.activeUpdates == 0;
    });
    --state.waitingUpdates;
    state.updateThread = current;
    ++state.activeUpdates;
    state.dirty = true;
}

void EndCacheUpdate()
{
    AutoProgressionCacheState& state = GetAutoProgressionCacheState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        MANGOS_ASSERT(state.activeUpdates > 0);
        MANGOS_ASSERT(state.updateThread == std::this_thread::get_id());
        --state.activeUpdates;
        if (!state.activeUpdates)
            state.updateThread = std::thread::id();
        state.dirty = true;
    }
    state.condition.notify_all();
}

void BeginCacheRead()
{
    AutoProgressionCacheState& state = GetAutoProgressionCacheState();
    std::unique_lock<std::mutex> lock(state.mutex);
    state.condition.wait(lock, [&state]()
    {
        return state.activeUpdates == 0 && state.waitingUpdates == 0;
    });
    ++state.activeReaders;
}

void EndCacheRead()
{
    AutoProgressionCacheState& state = GetAutoProgressionCacheState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        MANGOS_ASSERT(state.activeReaders > 0);
        --state.activeReaders;
    }
    state.condition.notify_all();
}
enum PendingAutoProgressionAction : uint8
{
    PENDING_AUTO_LEARN = 0x01,
    PENDING_AUTO_EQUIP = 0x02,
    PENDING_AUTO_ENCHANT = 0x04
};

ImprovementSource GetItemImprovementSource(uint32 spellId)
{
    switch (spellId)
    {
        case 2831: case 2832: case 2833: case 10344: case 19057: case 22725:
            return ImprovementSource::ArmorKit;
        case 3974: case 3975: case 3976: case 12459: case 12460: case 22779:
            return ImprovementSource::Scope;
        case 7216: case 9781: case 16623:
            return ImprovementSource::ShieldSpike;
        case 15340: case 15389: case 15391: case 15394: case 15397: case 15400:
        case 15402: case 15404: case 15406: case 22840: case 22844: case 22846:
            return ImprovementSource::Libram;
        case 24149: case 24160: case 24161: case 24162: case 24163: case 24164:
        case 24165: case 24167: case 24168: case 24420: case 24421: case 24422:
        case 28161:
            return ImprovementSource::Zandalar;
        case 22593: case 22594: case 22596: case 22597: case 22598: case 22599:
        case 28163: case 28165:
            return ImprovementSource::ArgentDawn;
        case 29467: case 29475: case 29480: case 29483:
            return ImprovementSource::Naxxramas;
        default:
            return ImprovementSource::None;
    }
}

bool ImprovementSourceEnabled(ImprovementSource source)
{
    switch (source)
    {
        case ImprovementSource::Enchanting:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_SOURCE_ENCHANTING);
        case ImprovementSource::Scope:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_SOURCE_SCOPES);
        case ImprovementSource::ShieldSpike:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_SOURCE_SHIELD_SPIKES);
        case ImprovementSource::ArmorKit:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_SOURCE_ARMOR_KITS);
        case ImprovementSource::Libram:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_SOURCE_LIBRAMS);
        case ImprovementSource::Zandalar:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_SOURCE_ZANDALAR);
        case ImprovementSource::ArgentDawn:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_SOURCE_ARGENT_DAWN);
        case ImprovementSource::Naxxramas:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_SOURCE_NAXXRAMAS);
        default:
            return false;
    }
}

uint32 ImprovementMinimumLevel(EnchantCandidate const& candidate)
{
    ItemPrototype const* source = candidate.sourceItemId ?
        sObjectMgr.GetItemPrototype(candidate.sourceItemId) : nullptr;
    uint32 level = source ? source->RequiredLevel : 0;
    if (source && source->SourceQuestLevel >= 0 &&
        (source->Bonding == BIND_WHEN_PICKED_UP ||
         source->Bonding == BIND_QUEST_ITEM ||
         source->Bonding == BIND_QUEST_ITEM1))
        level = std::max(level, uint32(source->SourceQuestLevel));

    if (candidate.sourceItemId >= 18329 && candidate.sourceItemId <= 18331)
        level = std::max(level, uint32(54));
    if (candidate.sourceItemId == 22636 || candidate.sourceItemId == 22638)
        level = std::max(level, uint32(60));

    switch (candidate.source)
    {
        case ImprovementSource::Libram: level = std::max(level, uint32(50)); break;
        case ImprovementSource::Zandalar: level = std::max(level, uint32(58)); break;
        case ImprovementSource::ArgentDawn: level = std::max(level, uint32(55)); break;
        case ImprovementSource::Naxxramas: level = std::max(level, uint32(60)); break;
        default: break;
    }
    return level;
}

bool HasAnyRewardedQuest(Player* player, uint32 const* questIds, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (player->GetQuestRewardStatus(questIds[i]))
            return true;
    return false;
}

bool ImprovementReputationAvailable(Player* player, uint32 sourceItemId)
{
    uint32 faction = 0;
    ReputationRank requiredRank = REP_NEUTRAL;
    switch (sourceItemId)
    {
        case 19782: case 19783: case 19784: case 19785: case 19786:
        case 19787: case 19788: case 19789: case 19790:
            faction = 270; requiredRank = REP_FRIENDLY; break;
        case 22635:
            faction = 270; requiredRank = REP_HONORED; break;
        case 20076: case 20077: case 20078:
            faction = 270; requiredRank = REP_EXALTED; break;
        case 18169: case 18170: case 18171: case 18172: case 18173:
        {
            uint32 const quests[] = { 5504, 5507, 5513 };
            if (!HasAnyRewardedQuest(player, quests, sizeof(quests) / sizeof(quests[0])))
                return false;
            faction = 529; requiredRank = REP_REVERED; break;
        }
        case 22636: case 22638:
            faction = 529; requiredRank = REP_HONORED; break;
        case 18182:
        {
            uint32 const quests[] = { 5517, 5521, 5524 };
            if (!HasAnyRewardedQuest(player, quests, sizeof(quests) / sizeof(quests[0])))
                return false;
            faction = 529; requiredRank = REP_EXALTED; break;
        }
        default:
            return true;
    }
    return player->GetReputationRank(faction) >= requiredRank;
}

bool ImprovementSourceAvailable(Player* player, EnchantCandidate const& candidate)
{
    ItemPrototype const* source = candidate.sourceItemId ?
        sObjectMgr.GetItemPrototype(candidate.sourceItemId) : nullptr;
    if (!candidate.sourceItemId)
        return true;
    if (!source)
        return false;
    if (player->GetLevel() < ImprovementMinimumLevel(candidate))
        return false;
    if (source->AllowableClass && !(source->AllowableClass & player->GetClassMask()))
        return false;
    if (source->AllowableRace && !(source->AllowableRace & player->GetRaceMask()))
        return false;
    if (!ImprovementReputationAvailable(player, candidate.sourceItemId))
        return false;
    if (source->RequiredReputationFaction &&
        uint32(player->GetReputationRank(source->RequiredReputationFaction)) < source->RequiredReputationRank)
        return false;
    return true;
}

void AddTaughtSpells(uint32 learningSpellId, std::set<uint32>& taughtSpells)
{
    SpellLearnSpellMapBounds const bounds =
        sSpellMgr.GetSpellLearnSpellMapBounds(learningSpellId);
    for (SpellLearnSpellMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
        if (itr->second.spell)
            taughtSpells.insert(itr->second.spell);

    SpellEntry const* learningSpell = sSpellMgr.GetSpellEntry(learningSpellId);
    if (!learningSpell)
        return;
    for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
        if (learningSpell->Effect[effect] == SPELL_EFFECT_LEARN_SPELL &&
            learningSpell->EffectTriggerSpell[effect])
            taughtSpells.insert(learningSpell->EffectTriggerSpell[effect]);
}

std::set<uint32> BuildAvailableEnchantingSpells()
{
    std::set<uint32> taughtSpells;
    std::set<TrainerSpellData const*> visitedTrainers;
    for (auto const& entry : sObjectMgr.GetCreatureInfoMap())
    {
        CreatureInfo const* info = entry.second.get();
        if (!info || !(info->npc_flags & UNIT_NPC_FLAG_TRAINER))
            continue;

        TrainerSpellData const* sources[2] =
        {
            sObjectMgr.GetNpcTrainerSpells(info->entry),
            info->trainer_id ? sObjectMgr.GetNpcTrainerTemplateSpells(info->trainer_id) : nullptr
        };
        for (TrainerSpellData const* source : sources)
        {
            if (!source || !visitedTrainers.insert(source).second)
                continue;
            for (auto const& trainerSpell : source->spellList)
                AddTaughtSpells(trainerSpell.second.spell, taughtSpells);
        }
    }

    for (auto const& entry : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const* formula = &entry.second;
        if (formula->RequiredSkill != SKILL_ENCHANTING ||
            formula->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE))
            continue;
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
            if (formula->Spells[slot].SpellId &&
                formula->Spells[slot].SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
                AddTaughtSpells(formula->Spells[slot].SpellId, taughtSpells);
    }

    SkillLineAbilityMapBounds const enchanting =
        sSpellMgr.GetSkillLineAbilityMapBoundsBySkillId(SKILL_ENCHANTING);
    for (SkillLineAbilityMap::const_iterator itr = enchanting.first; itr != enchanting.second; ++itr)
        if (itr->second &&
            itr->second->learnOnGetSkill == ABILITY_LEARNED_ON_GET_PROFESSION_SKILL)
            taughtSpells.insert(itr->second->spellId);
    return taughtSpells;
}


std::vector<EnchantCandidate> BuildImprovementCandidates()
{
    std::vector<EnchantCandidate> candidates;
    std::set<uint64> seen;
    auto addSpell = [&candidates, &seen](SpellEntry const* spell, ImprovementSource source,
        ItemPrototype const* sourceItem)
    {
        if (!spell || source == ImprovementSource::None || !(spell->Targets & TARGET_FLAG_ITEM))
            return;

        for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
        {
            if (spell->Effect[effect] != SPELL_EFFECT_ENCHANT_ITEM || spell->EffectMiscValue[effect] <= 0)
                continue;

            uint32 const enchantId = uint32(spell->EffectMiscValue[effect]);
            uint64 const key = (uint64(spell->Id) << 8) | effect;
            if (sSpellItemEnchantmentStore.LookupEntry(enchantId) && seen.insert(key).second)
                candidates.push_back({
                    spell->Id, enchantId, sourceItem ? sourceItem->ItemId : 0, source, effect });
        }
    };

    std::set<uint32> const availableEnchanting = BuildAvailableEnchantingSpells();
    SkillLineAbilityMapBounds const enchanting =
        sSpellMgr.GetSkillLineAbilityMapBoundsBySkillId(SKILL_ENCHANTING);
    for (SkillLineAbilityMap::const_iterator itr = enchanting.first; itr != enchanting.second; ++itr)
        if (itr->second && availableEnchanting.count(itr->second->spellId))
            addSpell(sSpellMgr.GetSpellEntry(itr->second->spellId), ImprovementSource::Enchanting, nullptr);

    for (auto const& entry : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const* sourceItem = &entry.second;
        if (sourceItem->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE))
            continue;

        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
        {
            if (!sourceItem->Spells[slot].SpellId ||
                sourceItem->Spells[slot].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;

            SpellEntry const* spell = sSpellMgr.GetSpellEntry(sourceItem->Spells[slot].SpellId);
            ImprovementSource const source = GetItemImprovementSource(sourceItem->Spells[slot].SpellId);
            addSpell(spell, source, sourceItem);
        }
    }
    return candidates;
}

bool ImprovementFits(Player* player, ItemPrototype const* item, uint8 equipmentSlot,
    EnchantCandidate const& candidate)
{
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(candidate.spellId);
    if (!item || !spell || candidate.effectIndex >= MAX_EFFECT_INDEX ||
        spell->Effect[candidate.effectIndex] != SPELL_EFFECT_ENCHANT_ITEM ||
        uint32(spell->EffectMiscValue[candidate.effectIndex]) != candidate.enchantId ||
        !sSpellItemEnchantmentStore.LookupEntry(candidate.enchantId) ||
        !(spell->Targets & TARGET_FLAG_ITEM) ||
        !ImprovementSourceEnabled(candidate.source) ||
        sObjectMgr.IsSpellDisabled(candidate.spellId) ||
        !ImprovementSourceAvailable(player, candidate))
        return false;
    if (item->ItemLevel < spell->baseLevel ||
        !Item::IsFitToSpellRequirements(spell, item->Class, item->SubClass,
            item->InventoryType))
        return false;
    if (spell->HasAttribute(SPELL_ATTR_HELD_ITEM_ONLY) &&
        equipmentSlot != EQUIPMENT_SLOT_MAINHAND)
        return false;
    return true;
}

bool ContainsI(std::string const& text, char const* needle)
{
    if (!needle)
        return false;
    std::string a(text), b(needle);
    std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return a.find(b) != std::string::npos;
}

void AddSource(std::vector<TrainerSource>& list, uint32 id, bool isTemplate)
{
    TrainerSource const source = { id, isTemplate };
    if (id && std::find(list.begin(), list.end(), source) == list.end())
        list.push_back(source);
}

void BuildTrainerCache(TrainerCache& cache,
    std::set<uint32> const& availableCreatureEntries)
{
    for (auto const& entry : sObjectMgr.GetCreatureInfoMap())
    {
        CreatureInfo const* info = entry.second.get();
        if (!info || !(info->npc_flags & UNIT_NPC_FLAG_TRAINER) ||
            !availableCreatureEntries.count(info->entry))
            continue;

        TrainerSpellData const* direct = sObjectMgr.GetNpcTrainerSpells(info->entry);
        TrainerSpellData const* inherited = info->trainer_id ?
            sObjectMgr.GetNpcTrainerTemplateSpells(info->trainer_id) : nullptr;
        bool const weaponMaster = ContainsI(info->subname, "Weapon Master");

        if (weaponMaster)
        {
            if (direct)
                AddSource(cache.weapons, info->entry, false);
            if (inherited)
                AddSource(cache.weapons, info->trainer_id, true);
        }
        else if (info->trainer_type == 0 && info->trainer_class > 0 && info->trainer_class < 12)
        {
            if (direct)
                AddSource(cache.classes[info->trainer_class], info->entry, false);
            if (inherited)
                AddSource(cache.classes[info->trainer_class], info->trainer_id, true);
        }
    }
}
uint32 LootSourceForMap(uint32 mapId)
{
    MapEntry const* map = sMapStorage.LookupEntry<MapEntry>(mapId);
    if (!map || !map->IsDungeon())
        return ITEM_SOURCE_WORLD_LOOT;
    return map->IsRaid() ? ITEM_SOURCE_RAID_LOOT : ITEM_SOURCE_DUNGEON_LOOT;
}

void MarkItems(std::unordered_map<uint32, uint32>& sources,
    std::set<uint32> const& itemIds, uint32 source)
{
    if (!source)
        return;
    for (uint32 itemId : itemIds)
        if (itemId)
            sources[itemId] |= source;
}

void MarkVendorItems(AutoProgressionCache& cache, VendorItemData const* items,
    bool vendorAvailable)
{
    if (!items)
        return;
    for (VendorItem const* item : items->m_items)
        if (item && item->item)
            cache.itemSources[item->item] |=
                vendorAvailable && !item->conditionId ?
                ITEM_SOURCE_VENDOR : ITEM_SOURCE_RESTRICTED;
}

struct CreatureMapSourceCollector
{
    std::unordered_map<uint32, uint32>& sources;
    std::set<uint32>& restricted;
    std::set<uint32>& available;
    std::unordered_map<uint32, int16> const& creatureEvents;
    std::unordered_map<uint32, int16> const& poolEvents;

    bool operator()(CreatureDataPair const& pair)
    {
        auto const directEvent = creatureEvents.find(pair.first);
        int16 eventId = directEvent == creatureEvents.end() ? 0 :
            directEvent->second;
        if (uint16 topPool = sPoolMgr.IsPartOfTopPool<Creature>(pair.first))
        {
            auto const poolEvent = poolEvents.find(topPool);
            if (poolEvent != poolEvents.end())
                eventId = poolEvent->second;
        }
        uint16 const absoluteEventId = uint16(
            eventId < 0 ? -int32(eventId) : int32(eventId));
        bool const eventInactive = eventId &&
            (!sGameEventMgr.IsEnabled(absoluteEventId) ||
             (eventId > 0 && !sGameEventMgr.IsActiveEvent(absoluteEventId)) ||
             (eventId < 0 && sGameEventMgr.IsActiveEvent(absoluteEventId)));
        bool const spawnUnavailable =
            (pair.second.spawn_flags & SPAWN_FLAG_DISABLED) || eventInactive;
        bool const lootUnavailable = spawnUnavailable ||
            sObjectMgr.IsMapLootDisabled(pair.second.position.mapId);
        uint32 const source = LootSourceForMap(pair.second.position.mapId);
        uint32 const replacementEntry = !spawnUnavailable ?
            (sGameEventMgr.GetCreatureUpdateDataForActiveEvent(pair.first) ?
                sGameEventMgr.GetCreatureUpdateDataForActiveEvent(pair.first)->entry_id : 0) : 0;
        for (uint32 originalEntry : pair.second.creature_id)
            if (originalEntry && replacementEntry &&
                originalEntry != replacementEntry)
                restricted.insert(originalEntry);

        auto recordEntry = [&](uint32 entry)
        {
            if (entry)
            {
                if (!spawnUnavailable)
                    available.insert(entry);
                if (lootUnavailable)
                    restricted.insert(entry);
                else
                    sources[entry] |= source;
            }
        };
        if (replacementEntry)
            recordEntry(replacementEntry);
        else
            for (uint32 entry : pair.second.creature_id)
                recordEntry(entry);
        return false;
    }
};

struct GameObjectMapSourceCollector
{
    std::unordered_map<uint32, uint32>& sources;
    std::set<uint32>& restricted;
    std::unordered_map<uint32, int16> const& gameObjectEvents;
    std::unordered_map<uint32, int16> const& poolEvents;

    bool operator()(GameObjectDataPair const& pair)
    {
        if (pair.second.id)
        {
            auto const directEvent = gameObjectEvents.find(pair.first);
            int16 eventId = directEvent == gameObjectEvents.end() ? 0 :
                directEvent->second;
            if (uint16 topPool = sPoolMgr.IsPartOfTopPool<GameObject>(pair.first))
            {
                auto const poolEvent = poolEvents.find(topPool);
                if (poolEvent != poolEvents.end())
                    eventId = poolEvent->second;
            }
            uint16 const absoluteEventId = uint16(
                eventId < 0 ? -int32(eventId) : int32(eventId));
            bool const eventInactive = eventId &&
                (!sGameEventMgr.IsEnabled(absoluteEventId) ||
                 (eventId > 0 &&
                  !sGameEventMgr.IsActiveEvent(absoluteEventId)) ||
                 (eventId < 0 &&
                  sGameEventMgr.IsActiveEvent(absoluteEventId)));
            if ((pair.second.spawn_flags & SPAWN_FLAG_DISABLED) ||
                sObjectMgr.IsMapLootDisabled(pair.second.position.mapId) ||
                eventInactive)
                restricted.insert(pair.second.id);
            else
                sources[pair.second.id] |= LootSourceForMap(pair.second.position.mapId);
        }
        return false;
    }
};

std::set<uint32> MarkLootSources(AutoProgressionCache& cache)
{
    std::unordered_map<uint32, uint32> creatureSources;
    std::set<uint32> restrictedCreatures;
    std::set<uint32> availableCreatures;
    CreatureMapSourceCollector creatureCollector =
        { creatureSources, restrictedCreatures, availableCreatures,
          sGameEventMgr.GetCreatureEventIds(),
          sGameEventMgr.GetPoolEventIds() };
    sObjectMgr.DoCreatureData(creatureCollector);

    for (auto const& entry : sObjectMgr.GetCreatureInfoMap())
    {
        CreatureInfo const* info = entry.second.get();
        if (!info)
            continue;
        auto const sourceItr = creatureSources.find(info->entry);
        uint32 const source = sourceItr != creatureSources.end() ?
            sourceItr->second : restrictedCreatures.count(info->entry) ?
            ITEM_SOURCE_NONE : ITEM_SOURCE_OTHER;
        bool const vendorAvailable =
            availableCreatures.count(info->entry) != 0;
        MarkVendorItems(cache, sObjectMgr.GetNpcVendorItemList(info->entry),
            vendorAvailable);
        if (info->vendor_id)
            MarkVendorItems(cache,
                sObjectMgr.GetNpcVendorTemplateItemList(info->vendor_id),
                vendorAvailable);
        std::set<uint32> items;
        std::set<uint32> allItems;
        if (info->loot_id)
        {
            LootTemplates_Creature.CollectItemIds(info->loot_id, items);
            LootTemplates_Creature.CollectItemIds(info->loot_id, allItems, true);
        }
        if (info->pickpocket_loot_id)
        {
            LootTemplates_Pickpocketing.CollectItemIds(info->pickpocket_loot_id, items);
            LootTemplates_Pickpocketing.CollectItemIds(
                info->pickpocket_loot_id, allItems, true);
        }
        if (info->skinning_loot_id)
        {
            LootTemplates_Skinning.CollectItemIds(info->skinning_loot_id, items);
            LootTemplates_Skinning.CollectItemIds(
                info->skinning_loot_id, allItems, true);
        }
        MarkItems(cache.itemSources, allItems, ITEM_SOURCE_RESTRICTED);
        MarkItems(cache.itemSources, items, source);
    }

    std::unordered_map<uint32, uint32> gameObjectSources;
    std::set<uint32> restrictedGameObjects;
    GameObjectMapSourceCollector gameObjectCollector =
        { gameObjectSources, restrictedGameObjects,
          sGameEventMgr.GetGameObjectEventIds(),
          sGameEventMgr.GetPoolEventIds() };
    sObjectMgr.DoGOData(gameObjectCollector);
    for (auto const& entry : sObjectMgr.GetGameObjectInfoMap())
    {
        GameObjectInfo const* info = entry.second.get();
        if (!info || !info->GetLootId())
            continue;
        std::set<uint32> items;
        std::set<uint32> allItems;
        LootTemplates_Gameobject.CollectItemIds(info->GetLootId(), items);
        LootTemplates_Gameobject.CollectItemIds(
            info->GetLootId(), allItems, true);
        auto const sourceItr = gameObjectSources.find(entry.first);
        uint32 const source = sourceItr != gameObjectSources.end() ?
            sourceItr->second : restrictedGameObjects.count(entry.first) ?
            ITEM_SOURCE_NONE : ITEM_SOURCE_OTHER;
        MarkItems(cache.itemSources, allItems, ITEM_SOURCE_RESTRICTED);
        MarkItems(cache.itemSources, items, source);
    }

    bool hasAvailableFishingArea = false;
    for (auto itr = sAreaStorage.begin<AreaEntry>();
         itr < sAreaStorage.end<AreaEntry>(); ++itr)
    {
        std::set<uint32> items;
        std::set<uint32> allItems;
        LootTemplates_Fishing.CollectItemIds(itr->Id, items);
        LootTemplates_Fishing.CollectItemIds(itr->Id, allItems, true);
        MarkItems(cache.itemSources, allItems, ITEM_SOURCE_RESTRICTED);
        if (!sObjectMgr.IsMapLootDisabled(itr->MapId))
        {
            MarkItems(cache.itemSources, items, LootSourceForMap(itr->MapId));
            hasAvailableFishingArea = true;
        }
    }
    std::set<uint32> fallbackFishingItems;
    std::set<uint32> allFallbackFishingItems;
    LootTemplates_Fishing.CollectItemIds(0, fallbackFishingItems);
    LootTemplates_Fishing.CollectItemIds(0, allFallbackFishingItems, true);
    MarkItems(cache.itemSources, allFallbackFishingItems,
        ITEM_SOURCE_RESTRICTED);
    if (hasAvailableFishingArea)
        MarkItems(cache.itemSources, fallbackFishingItems,
            ITEM_SOURCE_WORLD_LOOT);

    std::set<uint32> otherItems;
    std::set<uint32> allOtherItems;
    LootTemplates_Item.CollectAllItemIds(otherItems);
    LootTemplates_Mail.CollectAllItemIds(otherItems);
    LootTemplates_Disenchant.CollectAllItemIds(otherItems);
    LootTemplates_Item.CollectAllItemIds(allOtherItems, true);
    LootTemplates_Mail.CollectAllItemIds(allOtherItems, true);
    LootTemplates_Disenchant.CollectAllItemIds(allOtherItems, true);
    MarkItems(cache.itemSources, allOtherItems, ITEM_SOURCE_RESTRICTED);
    MarkItems(cache.itemSources, otherItems, ITEM_SOURCE_OTHER);
    return availableCreatures;
}

void BuildAutoProgressionCache(AutoProgressionCache& cache)
{
#ifdef MANGOS_DEBUG
    uint32 const debugStarted = WorldTimer::getMSTime();
#endif

    for (auto const& entry : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const& item = entry.second;
        if (item.Class == ITEM_CLASS_WEAPON || item.Class == ITEM_CLASS_ARMOR)
        {
            cache.equipmentItems.push_back(item.ItemId);
            if (item.ItemSet)
                ++cache.itemSetSizes[item.ItemSet];
        }
    }

    std::set<uint32> const availableCreatureEntries =
        MarkLootSources(cache);
    BuildTrainerCache(cache.trainers, availableCreatureEntries);
    cache.improvements = BuildImprovementCandidates();

    for (uint32 id = 0; id < sObjectMgr.GetMaxSkillLineAbilityId(); ++id)
    {
        SkillLineAbilityEntry const* ability = sObjectMgr.GetSkillLineAbility(id);
        if (!ability)
            continue;
        SkillLineEntry const* skill = sSkillLineStore.LookupEntry(ability->skillId);
        if (!skill || (skill->categoryId != SKILL_CATEGORY_PROFESSION &&
            skill->categoryId != SKILL_CATEGORY_SECONDARY))
            continue;
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(ability->spellId);
        if (!spell)
            continue;
        for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
            if (spell->Effect[effect] == SPELL_EFFECT_CREATE_ITEM &&
                spell->EffectItemType[effect])
                cache.itemSources[spell->EffectItemType[effect]] |=
                    sObjectMgr.IsSpellDisabled(ability->spellId) ?
                    ITEM_SOURCE_RESTRICTED : ITEM_SOURCE_CRAFTED;
    }

    for (auto const& entry : sObjectMgr.GetQuestTemplates())
    {
        Quest const* quest = entry.second.get();
        if (!quest || !sObjectMgr.IsQuestTemplateLoaded(entry.first))
            continue;
        auto addReward = [&](uint32 itemId, uint32 count, bool choice)
        {
            if (!itemId || !count)
                return;
            cache.itemSources[itemId] |= ITEM_SOURCE_QUEST;
            cache.rewardQuests[itemId].push_back(
                { entry.first, count, choice });
            if (quest->IsActive())
                cache.activeQuestRewardItems.insert(itemId);
        };
        for (uint8 index = 0; index < QUEST_REWARD_CHOICES_COUNT; ++index)
            addReward(quest->RewChoiceItemId[index],
                quest->RewChoiceItemCount[index], true);
        for (uint8 index = 0; index < QUEST_REWARDS_COUNT; ++index)
            addReward(quest->RewItemId[index], quest->RewItemCount[index], false);
    }

#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
    {
        uint32 trainerSources = uint32(cache.trainers.weapons.size());
        for (std::vector<TrainerSource> const& sources : cache.trainers.classes)
            trainerSources += uint32(sources.size());

        uint32 rewardLinks = 0;
        for (auto const& entry : cache.rewardQuests)
            rewardLinks += uint32(entry.second.size());

        uint32 restrictedMappings = 0;
        for (auto const& entry : cache.itemSources)
            if (entry.second & ITEM_SOURCE_RESTRICTED)
                ++restrictedMappings;

        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: cache rebuilt in %u ms; equipment=%u, sourceMappings=%u (restricted=%u), questRewardItems=%u (links=%u), trainerSources=%u, improvements=%u.",
            WorldTimer::getMSTimeDiffToNow(debugStarted),
            uint32(cache.equipmentItems.size()), uint32(cache.itemSources.size()),
            restrictedMappings, uint32(cache.rewardQuests.size()), rewardLinks,
            trainerSources, uint32(cache.improvements.size()));
    }
#endif
}

std::shared_ptr<AutoProgressionCache const> EnsureAutoProgressionCache()
{
    AutoProgressionCacheState& state = GetAutoProgressionCacheState();
    std::unique_lock<std::mutex> lock(state.mutex);
    state.condition.wait(lock, [&state]() { return state.activeUpdates == 0; });
    if (!state.dirty && state.snapshot)
        return state.snapshot;

    std::shared_ptr<AutoProgressionCache> cache =
        std::make_shared<AutoProgressionCache>();
    BuildAutoProgressionCache(*cache);
    state.snapshot = cache;
    state.dirty = false;
    return state.snapshot;
}


bool ItemSourceEnabled(uint32 source)
{
    switch (source)
    {
        case ITEM_SOURCE_VENDOR:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_VENDOR);
        case ITEM_SOURCE_CRAFTED:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_CRAFTED);
        case ITEM_SOURCE_QUEST:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_INCLUDE_QUEST_REWARDS);
        case ITEM_SOURCE_WORLD_LOOT:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_WORLD_LOOT);
        case ITEM_SOURCE_DUNGEON_LOOT:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_DUNGEON_LOOT);
        case ITEM_SOURCE_RAID_LOOT:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_RAID_LOOT);
        case ITEM_SOURCE_NONE:
        case ITEM_SOURCE_OTHER:
            return sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_OTHER);
        default:
            return false;
    }
}

uint32 EnabledItemSources(uint32 mask, bool includeQuest)
{
    uint32 enabled = 0;
    uint32 const sources[] =
    {
        ITEM_SOURCE_VENDOR, ITEM_SOURCE_CRAFTED, ITEM_SOURCE_WORLD_LOOT,
        ITEM_SOURCE_DUNGEON_LOOT, ITEM_SOURCE_RAID_LOOT, ITEM_SOURCE_OTHER
    };
    for (uint32 source : sources)
        if ((mask & source) && ItemSourceEnabled(source))
            enabled |= source;
    if (includeQuest && (mask & ITEM_SOURCE_QUEST) && ItemSourceEnabled(ITEM_SOURCE_QUEST))
        enabled |= ITEM_SOURCE_QUEST;
    if (!mask && ItemSourceEnabled(ITEM_SOURCE_NONE))
        enabled = ITEM_SOURCE_OTHER;
    return enabled;
}

bool CastTrainerSpell(Player* player, TrainerSpell const& trainer)
{
    SpellEntry const* info = sSpellMgr.GetSpellEntry(trainer.spell);
    if (!info)
        return false;

    Spell* spell = new Spell(player, info, true);
    SpellCastTargets targets;
    targets.setUnitTarget(player);
    SpellCastResult const result = spell->prepare(std::move(targets));
    if (result == SPELL_CAST_OK)
        spell->update(1);
    else
        delete spell;
    return result == SPELL_CAST_OK;
}

uint32 LearnSources(Player* player, std::vector<TrainerSource> const& sources)
{
    uint32 learned = 0;
    std::set<uint32> attempted;
    for (TrainerSource const& sourceId : sources)
    {
        TrainerSpellData const* source = sourceId.isTemplate ?
            sObjectMgr.GetNpcTrainerTemplateSpells(sourceId.id) :
            sObjectMgr.GetNpcTrainerSpells(sourceId.id);
        if (!source)
            continue;
        for (auto const& entry : source->spellList)
        {
            TrainerSpell const& trainer = entry.second;
            if (!attempted.insert(trainer.spell).second)
                continue;
            if (player->GetTrainerSpellState(&trainer) == TRAINER_SPELL_GREEN && CastTrainerSpell(player, trainer))
                ++learned;
        }
    }
    return learned;
}

void MaxCombatSkills(Player* player)
{
    uint16 const maximum = uint16(std::min<uint32>(player->GetLevel() * 5, 300));
    uint16 const skills[] =
    {
        SKILL_SWORDS, SKILL_AXES, SKILL_BOWS, SKILL_GUNS, SKILL_MACES, SKILL_2H_SWORDS,
        SKILL_STAVES, SKILL_2H_MACES, SKILL_UNARMED, SKILL_2H_AXES, SKILL_DAGGERS,
        SKILL_THROWN, SKILL_CROSSBOWS, SKILL_WANDS, SKILL_POLEARMS, SKILL_FIST_WEAPONS,
        SKILL_DEFENSE
    };
    for (uint16 skill : skills)
        if (player->GetSkillValue(skill))
            player->SetSkill(skill, maximum, maximum);
}

struct Weights
{
    float str = 0, agi = 0, sta = 0.25f, intl = 0, spi = 0;
    float armor = 0, blockValue = 0, weaponDps = 0, rangedDps = 0;
    float ap = 0, rap = 0, spell = 0, healing = 0, mp5 = 0;
    float meleeHit = 0, spellHit = 0, meleeCrit = 0, spellCrit = 0;
    float dodge = 0, parry = 0, block = 0, defense = 0, weaponSkill = 0;
    uint32 spellSchools = 0;
    bool tank = false, caster = false, healer = false;
    bool twoHand = false, dualWield = false, shield = false;
};

Weights StrengthMelee()
{
    Weights w;
    w.str = 2.0f; w.agi = 1.2f; w.sta = 0.55f; w.weaponDps = 13.0f; w.rangedDps = 0.25f;
    w.ap = 1.0f; w.meleeHit = 12.0f; w.meleeCrit = 10.0f; w.weaponSkill = 16.0f;
    return w;
}

Weights AgilityMelee()
{
    Weights w = StrengthMelee();
    w.str = 1.0f; w.agi = 2.4f; w.sta = 0.45f; w.dualWield = true;
    return w;
}

Weights Caster()
{
    Weights w;
    w.sta = 0.35f; w.intl = 1.0f; w.spi = 0.45f; w.rangedDps = 3.5f;
    w.spell = 1.0f; w.mp5 = 2.0f; w.spellHit = 11.0f; w.spellCrit = 8.0f; w.caster = true;
    w.spellSchools = SPELL_SCHOOL_MASK_MAGIC;
    return w;
}

Weights Healer()
{
    Weights w;
    w.sta = 0.35f; w.intl = 1.0f; w.spi = 0.8f; w.spell = 0.25f;
    w.healing = 1.0f; w.mp5 = 3.5f; w.spellCrit = 5.0f; w.caster = true; w.healer = true;
    w.spellSchools = SPELL_SCHOOL_MASK_MAGIC;
    return w;
}

Weights GetWeights(Player* player)
{
    uint32 const tree = LFGMgr::GetHighestTalentTree(player);
    Weights w;
    switch (player->GetClass())
    {
        case CLASS_WARRIOR:
            w = StrengthMelee();
            if (tree == 2)
            {
                w.sta = 2.2f; w.armor = 0.045f; w.blockValue = 0.18f;
                w.dodge = 12; w.parry = 13; w.block = 10; w.defense = 4;
                w.tank = true; w.shield = true;
            }
            else { w.twoHand = tree == 0; w.dualWield = tree == 1; }
            break;
        case CLASS_PALADIN:
            if (tree == 0) w = Healer();
            else if (tree == 1)
            {
                w = StrengthMelee(); w.intl = 0.65f; w.spell = 0.35f; w.sta = 2.0f;
                w.armor = 0.045f; w.blockValue = 0.18f; w.dodge = 11; w.parry = 12;
                w.block = 10; w.defense = 4; w.tank = true; w.caster = true; w.shield = true;
            }
            else { w = StrengthMelee(); w.intl = 0.35f; w.spell = 0.2f; w.twoHand = true; }
            w.spellSchools = SPELL_SCHOOL_MASK_HOLY;
            break;
        case CLASS_HUNTER:
            w = AgilityMelee(); w.str = 0.15f; w.agi = 2.7f; w.intl = 0.45f;
            w.weaponDps = 0.35f; w.rangedDps = 16; w.rap = 1; w.dualWield = false;
            break;
        case CLASS_ROGUE:
            w = AgilityMelee(); w.agi = 2.5f;
            break;
        case CLASS_PRIEST:
            w = tree == 2 ? Caster() : Healer();
            if (tree == 0) w.spi = 1.0f;
            w.spellSchools = tree == 2 ? SPELL_SCHOOL_MASK_SHADOW : SPELL_SCHOOL_MASK_HOLY;
            break;
        case CLASS_SHAMAN:
            if (tree == 0) w = Caster();
            else if (tree == 1)
            {
                w = StrengthMelee(); w.agi = 1.5f; w.intl = 0.45f; w.mp5 = 0.8f; w.twoHand = true;
            }
            else w = Healer();
            w.spellSchools = tree == 0 ? (SPELL_SCHOOL_MASK_NATURE | SPELL_SCHOOL_MASK_FIRE |
                SPELL_SCHOOL_MASK_FROST) : SPELL_SCHOOL_MASK_NATURE;
            break;
        case CLASS_MAGE:
            w = Caster(); if (tree == 1) w.spellCrit = 10;
            w.spellSchools = tree == 0 ? SPELL_SCHOOL_MASK_ARCANE :
                tree == 1 ? SPELL_SCHOOL_MASK_FIRE : SPELL_SCHOOL_MASK_FROST;
            break;
        case CLASS_WARLOCK:
            w = Caster(); w.sta = 0.8f; w.spi = 0.25f; w.spellCrit = tree == 2 ? 9.0f : 6.5f;
            w.spellSchools = tree == 2 ? (SPELL_SCHOOL_MASK_FIRE | SPELL_SCHOOL_MASK_SHADOW) :
                SPELL_SCHOOL_MASK_SHADOW;
            break;
        case CLASS_DRUID:
            if (tree == 0) w = Caster();
            else if (tree == 1)
            {
                w = AgilityMelee(); w.str = 2; w.agi = 2.5f; w.sta = 1.2f;
                w.weaponDps = 0; w.armor = 0.02f; w.dualWield = false;
            }
            else w = Healer();
            w.spellSchools = tree == 0 ? (SPELL_SCHOOL_MASK_NATURE | SPELL_SCHOOL_MASK_ARCANE) :
                tree == 2 ? SPELL_SCHOOL_MASK_NATURE : 0;
            break;
        default: break;
    }
    return w;
}

float PrimaryScale() { return sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_PRIMARY_STAT_SCALE); }
float SecondaryScale() { return sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_SECONDARY_STAT_SCALE); }
float SurvivalScale() { return sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_SURVIVAL_STAT_SCALE); }
float WeaponScale() { return sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_WEAPON_DPS_SCALE); }
float SpellScale() { return sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_SPELL_POWER_SCALE); }

float ScoreItemMod(Weights const& w, uint32 type, float value)
{
    if (value == 0)
        return 0;

    switch (type)
    {
        case ITEM_MOD_STRENGTH: return value * w.str * PrimaryScale();
        case ITEM_MOD_AGILITY: return value * w.agi * PrimaryScale();
        case ITEM_MOD_STAMINA: return value * w.sta * SurvivalScale();
        case ITEM_MOD_INTELLECT: return value * w.intl * PrimaryScale();
        case ITEM_MOD_SPIRIT: return value * w.spi * SecondaryScale();
        case ITEM_MOD_HEALTH: return value * 0.1f * w.sta * SurvivalScale();
        case ITEM_MOD_MANA: return value / 15.0f * w.intl * PrimaryScale();
        default: return 0;
    }
}

float ScoreResistance(Weights const& w, float value, uint32 schoolMask)
{
    if (value == 0)
        return 0;

    float score = 0;
    if (schoolMask & (1u << SPELL_SCHOOL_NORMAL))
        score += value * w.armor * SurvivalScale();

    uint32 magicSchools = 0;
    for (uint8 school = SPELL_SCHOOL_HOLY; school < MAX_SPELL_SCHOOL; ++school)
        if (schoolMask & (1u << school))
            ++magicSchools;
    score += value * magicSchools * (w.tank ? 0.18f : 0.08f) * SurvivalScale();
    return score;
}

float SpellSchoolScale(Weights const& w, uint32 schoolMask)
{
    uint32 const magic = schoolMask & SPELL_SCHOOL_MASK_MAGIC;
    if (!magic || magic == SPELL_SCHOOL_MASK_MAGIC)
        return 1.0f;
    return (magic & w.spellSchools) ? 1.0f : 0.0f;
}

bool IsWeaponSkill(uint32 skill)
{
    switch (skill)
    {
        case SKILL_SWORDS: case SKILL_AXES: case SKILL_BOWS: case SKILL_GUNS:
        case SKILL_MACES: case SKILL_2H_SWORDS: case SKILL_STAVES: case SKILL_2H_MACES:
        case SKILL_UNARMED: case SKILL_2H_AXES: case SKILL_DAGGERS: case SKILL_THROWN:
        case SKILL_CROSSBOWS: case SKILL_WANDS: case SKILL_POLEARMS: case SKILL_FIST_WEAPONS:
            return true;
        default:
            return false;
    }
}

bool EffectTargetsHostile(SpellEntry const* spell, uint8 effectIndex)
{
    uint32 const targetA = spell->EffectImplicitTargetA[effectIndex];
    uint32 const targetB = spell->EffectImplicitTargetB[effectIndex];
    if (Spells::IsFriendlyTarget(targetA) || Spells::IsFriendlyTarget(targetB))
        return false;
    if (!Spells::IsPositiveTarget(targetA, targetB))
        return true;
    return !spell->IsPositiveEffect(SpellEffectIndex(effectIndex));
}

float ScoreAura(Weights const& w, SpellEntry const* spell, float trigger,
    uint8 depth = 0)
{
    if (!spell || trigger <= 0)
        return 0;

    float score = 0;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (spell->Effect[i] != SPELL_EFFECT_APPLY_AURA)
            continue;

        float const rawValue = float(spell->CalculateSimpleValue(SpellEffectIndex(i)));
        float const value = std::fabs(rawValue);
        AuraType const aura = AuraType(spell->EffectApplyAuraName[i]);
        bool const positive = spell->IsPositiveEffect(SpellEffectIndex(i));
        bool const hostileTarget = EffectTargetsHostile(spell, i);
        float const scoredValue = hostileTarget ? -rawValue : rawValue;

        switch (aura)
        {
            case SPELL_AURA_MOD_ATTACKSPEED:
            case SPELL_AURA_MOD_MELEE_HASTE:
                if (positive && rawValue > 0)
                    score += value * std::max(w.meleeCrit * 0.7f, w.weaponDps * 0.5f) * SecondaryScale();
                else if (!positive && rawValue < 0)
                    score += (hostileTarget ? 1.0f : -1.0f) * value *
                        (0.20f + w.sta * 0.08f) * SurvivalScale();
                continue;
            case SPELL_AURA_MOD_DECREASE_SPEED:
                if (!positive && rawValue < 0)
                    score += (hostileTarget ? 1.0f : -1.0f) * value *
                        (0.10f + w.sta * 0.03f) * SurvivalScale();
                continue;
            case SPELL_AURA_MOD_RANGED_HASTE:
                if (positive && rawValue > 0)
                    score += value * w.rangedDps * 0.5f * SecondaryScale();
                else if (!positive && rawValue < 0)
                    score += (hostileTarget ? 1.0f : -1.0f) * value *
                        (0.12f + w.sta * 0.04f) * SurvivalScale();
                continue;
            case SPELL_AURA_MOD_INCREASE_SPEED:
                score += scoredValue * (positive ? 0.6f : 0.15f) * SecondaryScale();
                continue;
            case SPELL_AURA_MOD_THREAT:
                if ((w.tank && rawValue > 0) || (!w.tank && rawValue < 0))
                    score += value * (w.tank ? 4.0f : 2.0f) * SecondaryScale();
                continue;
            case SPELL_AURA_PROC_TRIGGER_SPELL:
                if (depth < 2 && spell->EffectTriggerSpell[i] &&
                    spell->EffectTriggerSpell[i] != spell->Id)
                    score += ScoreAura(w, sSpellMgr.GetSpellEntry(spell->EffectTriggerSpell[i]),
                        spell->procChance ? float(spell->procChance) / 100.0f : 0.20f,
                        depth + 1);
                continue;
            case SPELL_AURA_MOD_DAMAGE_DONE:
                if (!positive && rawValue < 0)
                {
                    score += (hostileTarget ? 1.0f : -1.0f) * value *
                        (0.12f + w.sta * 0.05f) * SurvivalScale();
                    continue;
                }
                break;
            default:
                break;
        }

        if (std::fabs(scoredValue) <= 0.001f)
            continue;

        switch (aura)
        {
            case SPELL_AURA_MOD_DAMAGE_DONE:
                score += scoredValue * w.spell *
                    SpellSchoolScale(w, uint32(spell->EffectMiscValue[i])) * SpellScale(); break;
            case SPELL_AURA_MOD_DAMAGE_DONE_CREATURE:
                score += scoredValue * (w.spell + w.weaponDps / 13.0f) * 0.10f * SecondaryScale(); break;
            case SPELL_AURA_MOD_HEALING_DONE: score += scoredValue * w.healing * SpellScale(); break;
            case SPELL_AURA_MOD_HEALING_DONE_PERCENT:
                score += scoredValue * w.healing * 4.0f * SpellScale(); break;
            case SPELL_AURA_MOD_ATTACK_POWER: score += scoredValue * w.ap * PrimaryScale(); break;
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER: score += scoredValue * w.rap * PrimaryScale(); break;
            case SPELL_AURA_MOD_HIT_CHANCE: score += scoredValue * w.meleeHit * SecondaryScale(); break;
            case SPELL_AURA_MOD_SPELL_HIT_CHANCE: score += scoredValue * w.spellHit * SecondaryScale(); break;
            case SPELL_AURA_MOD_CRIT_PERCENT: score += scoredValue * w.meleeCrit * SecondaryScale(); break;
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
                score += scoredValue * w.spellCrit * SecondaryScale(); break;
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL:
                score += scoredValue * w.spellCrit *
                    SpellSchoolScale(w, uint32(spell->EffectMiscValue[i])) * SecondaryScale(); break;
            case SPELL_AURA_MOD_DODGE_PERCENT: score += scoredValue * w.dodge * SurvivalScale(); break;
            case SPELL_AURA_MOD_PARRY_PERCENT: score += scoredValue * w.parry * SurvivalScale(); break;
            case SPELL_AURA_MOD_BLOCK_PERCENT: score += scoredValue * w.block * SurvivalScale(); break;
            case SPELL_AURA_MOD_SHIELD_BLOCKVALUE:
                score += scoredValue * w.blockValue * SurvivalScale(); break;
            case SPELL_AURA_MOD_SHIELD_BLOCKVALUE_PCT:
                score += scoredValue * w.block * 0.6f * SurvivalScale(); break;
            case SPELL_AURA_MOD_RESISTANCE:
            case SPELL_AURA_MOD_RESISTANCE_EXCLUSIVE:
                score += ScoreResistance(w, scoredValue, uint32(spell->EffectMiscValue[i])); break;
            case SPELL_AURA_MOD_POWER_REGEN:
            case SPELL_AURA_MOD_MANA_REGEN_INTERRUPT:
                score += scoredValue * w.mp5 * SecondaryScale(); break;
            case SPELL_AURA_MOD_INCREASE_HEALTH:
                score += ScoreItemMod(w, ITEM_MOD_HEALTH, scoredValue); break;
            case SPELL_AURA_MOD_INCREASE_ENERGY:
                if (spell->EffectMiscValue[i] == POWER_MANA)
                    score += ScoreItemMod(w, ITEM_MOD_MANA, scoredValue);
                break;
            case SPELL_AURA_SCHOOL_ABSORB:
                score += scoredValue * (0.30f + w.sta * 0.08f) * SurvivalScale(); break;
            case SPELL_AURA_MOD_SKILL:
            case SPELL_AURA_MOD_SKILL_TALENT:
                if (spell->EffectMiscValue[i] == SKILL_DEFENSE)
                    score += scoredValue * w.defense * SurvivalScale();
                else if (IsWeaponSkill(uint32(spell->EffectMiscValue[i])))
                    score += scoredValue * w.weaponSkill * SecondaryScale();
                break;
            case SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE:
                score += scoredValue * ((w.str + w.agi + w.intl) * PrimaryScale() +
                    w.sta * SurvivalScale() + w.spi * SecondaryScale()) * 0.6f; break;
            case SPELL_AURA_MOD_STAT:
                if (spell->EffectMiscValue[i] < 0)
                    score += scoredValue * ((w.str + w.agi + w.intl) * PrimaryScale() +
                        w.sta * SurvivalScale() + w.spi * SecondaryScale());
                else
                    score += ScoreItemMod(w, uint32(spell->EffectMiscValue[i] == STAT_STRENGTH ? ITEM_MOD_STRENGTH :
                        spell->EffectMiscValue[i] == STAT_AGILITY ? ITEM_MOD_AGILITY :
                        spell->EffectMiscValue[i] == STAT_STAMINA ? ITEM_MOD_STAMINA :
                        spell->EffectMiscValue[i] == STAT_INTELLECT ? ITEM_MOD_INTELLECT :
                        spell->EffectMiscValue[i] == STAT_SPIRIT ? ITEM_MOD_SPIRIT : MAX_ITEM_MOD), scoredValue);
                break;
            default: break;
        }
    }
    return score * trigger;
}

float WeaponDps(ItemPrototype const* item)
{
    if (!item->Delay)
        return 0;
    float damage = 0;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_DAMAGES; ++i)
        damage += (item->Damage[i].DamageMin + item->Damage[i].DamageMax) * 0.5f;
    return damage * 1000.0f / float(item->Delay);
}
float EnchantProcPpm(ItemPrototype const* item, SpellEntry const* procSpell, uint32 chance)
{
    float const configured = procSpell ? sSpellMgr.GetItemEnchantProcChance(procSpell->Id) : 0;
    if (configured > 0)
        return configured;

    uint32 const delay = item ? item->Delay : 0;
    if (chance && delay)
        return float(chance) * 600.0f / float(delay);
    return 1.0f;
}

float ScoreProcPayload(Weights const& w, ItemPrototype const* item, SpellEntry const* spell, float ppm, uint8 depth)
{
    if (!spell || ppm <= 0 || depth > 2)
        return 0;

    int32 const duration = spell->GetDuration();
    float const uptime = duration > 0 ?
        1.0f - std::exp(-ppm * float(duration) / 60000.0f) :
        std::min(1.0f, ppm / 60.0f);
    float score = ScoreAura(w, spell, uptime);

    float throughput = item && item->IsRangedWeapon() ? w.rangedDps : w.weaponDps;
    if (throughput <= 0)
        throughput = std::max(0.10f, w.spell * 0.35f);

    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        float const value = std::fabs(float(
            spell->CalculateSimpleValue(SpellEffectIndex(i))));
        float const perSecond = value * ppm / 60.0f;
        bool const hostileTarget = EffectTargetsHostile(spell, i);

        switch (spell->Effect[i])
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                score += (hostileTarget ? 1.0f : -1.0f) *
                    perSecond * throughput * WeaponScale();
                break;
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
            {
                float const averageWeaponDamage = item && item->Delay ?
                    WeaponDps(item) * float(item->Delay) / 1000.0f : 0;
                float const damagePerSecond =
                    value / 100.0f * averageWeaponDamage * ppm / 60.0f;
                score += (hostileTarget ? 1.0f : -1.0f) *
                    damagePerSecond * throughput * WeaponScale();
                break;
            }
            case SPELL_EFFECT_HEALTH_LEECH:
                score += (hostileTarget ? 1.0f : -1.0f) *
                    perSecond * throughput * WeaponScale();
                if (hostileTarget)
                    score += perSecond * (0.5f + w.sta * 0.1f) * SurvivalScale();
                break;
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
                score += (hostileTarget ? -1.0f : 1.0f) * perSecond *
                    (w.healer ? w.healing * 0.25f : 0.5f + w.sta * 0.1f) *
                    SurvivalScale();
                break;
            case SPELL_EFFECT_ENERGIZE:
                score += (hostileTarget ? -1.0f : 1.0f) *
                    perSecond * w.mp5 * 0.2f * SecondaryScale();
                break;
            case SPELL_EFFECT_TRIGGER_SPELL:
                if (spell->EffectTriggerSpell[i] && spell->EffectTriggerSpell[i] != spell->Id)
                    score += ScoreProcPayload(w, item,
                        sSpellMgr.GetSpellEntry(spell->EffectTriggerSpell[i]), ppm, depth + 1);
                break;
            default:
                break;
        }
    }
    return score * (spell->TargetCreatureType ? 0.20f : 1.0f);
}

float ScoreEnchantment(Weights const& w, ItemPrototype const* item,
    SpellItemEnchantmentEntry const* enchant)
{
    if (!item || !enchant)
        return 0;

    float score = 0.001f;
    for (uint8 i = 0; i < 3; ++i)
    {
        float const amount = float(enchant->amount[i]);
        switch (enchant->type[i])
        {
            case ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL:
            {
                SpellEntry const* procSpell = sSpellMgr.GetSpellEntry(enchant->spellid[i]);
                score += ScoreProcPayload(w, item, procSpell,
                    EnchantProcPpm(item, procSpell, enchant->amount[i]), 0);
                break;
            }
            case ITEM_ENCHANTMENT_TYPE_DAMAGE:
                if (item->Delay)
                    score += amount * 1000.0f / float(item->Delay) *
                        (item->IsRangedWeapon() ? w.rangedDps : w.weaponDps) * WeaponScale();
                break;
            case ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL:
                score += ScoreAura(w, sSpellMgr.GetSpellEntry(enchant->spellid[i]), 1.0f);
                break;
            case ITEM_ENCHANTMENT_TYPE_RESISTANCE:
                if (enchant->spellid[i] < MAX_SPELL_SCHOOL)
                    score += ScoreResistance(w, amount, 1u << enchant->spellid[i]);
                break;
            case ITEM_ENCHANTMENT_TYPE_STAT:
                score += ScoreItemMod(w, enchant->spellid[i], amount);
                break;
            default:
                break;
        }
    }
    return score;
}

float ItemUsePpm(_ItemSpell const& itemSpell, SpellEntry const* spell)
{
    uint32 const spellCooldown = itemSpell.SpellCooldown >= 0 ?
        uint32(itemSpell.SpellCooldown) : (spell ? spell->RecoveryTime : 0);
    uint32 const categoryCooldown = itemSpell.SpellCategoryCooldown >= 0 ?
        uint32(itemSpell.SpellCategoryCooldown) :
        (spell ? spell->CategoryRecoveryTime : 0);
    uint32 const cooldown = std::max(spellCooldown, categoryCooldown);
    if (!cooldown)
        return 0.5f;
    return std::min(6.0f, 60000.0f / float(cooldown));
}

float ScoreItem(Weights const& w, ItemPrototype const* item)
{
    float score = item->ItemLevel * 0.025f + item->Quality * 0.2f;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        score += ScoreItemMod(w, item->ItemStat[i].ItemStatType,
            float(item->ItemStat[i].ItemStatValue));
    score += item->Armor * w.armor * SurvivalScale();
    score += item->Block * w.blockValue * SurvivalScale();
    score += ScoreResistance(w, item->HolyRes, SPELL_SCHOOL_MASK_HOLY);
    score += ScoreResistance(w, item->FireRes, SPELL_SCHOOL_MASK_FIRE);
    score += ScoreResistance(w, item->NatureRes, SPELL_SCHOOL_MASK_NATURE);
    score += ScoreResistance(w, item->FrostRes, SPELL_SCHOOL_MASK_FROST);
    score += ScoreResistance(w, item->ShadowRes, SPELL_SCHOOL_MASK_SHADOW);
    score += ScoreResistance(w, item->ArcaneRes, SPELL_SCHOOL_MASK_ARCANE);

    float const dps = WeaponDps(item);
    score += dps * (item->IsRangedWeapon() ? w.rangedDps : w.weaponDps) * WeaponScale();

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (!item->Spells[i].SpellId)
            continue;
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(item->Spells[i].SpellId);
        switch (item->Spells[i].SpellTrigger)
        {
            case ITEM_SPELLTRIGGER_ON_EQUIP:
                score += ScoreAura(w, spell, 1.0f);
                break;
            case ITEM_SPELLTRIGGER_ON_USE:
                score += ScoreProcPayload(w, item, spell,
                    ItemUsePpm(item->Spells[i], spell), 0);
                break;
            case ITEM_SPELLTRIGGER_CHANCE_ON_HIT:
            {
                float ppm = item->Spells[i].SpellPPMRate;
                if (ppm <= 0 && spell && spell->procChance && item->Delay)
                    ppm = float(spell->procChance) * 600.0f / float(item->Delay);
                score += ScoreProcPayload(w, item, spell, ppm > 0 ? ppm : 1.0f, 0);
                break;
            }
            default:
                break;
        }
    }
    return score;
}

float ScoreSetPotential(Weights const& w, Player const* player,
    ItemPrototype const* item, AutoProgressionCache const& cache)
{
    if (!item || !item->ItemSet)
        return 0;
    ItemSetEntry const* set = sItemSetStore.LookupEntry(item->ItemSet);
    if (!set)
        return 0;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_6_1
    if (set->required_skill_id &&
        player->GetSkillValue(set->required_skill_id) < set->required_skill_value)
        return 0;
#endif
    float score = 0;
    auto const sizeItr = cache.itemSetSizes.find(item->ItemSet);
    uint32 const setSize = sizeItr == cache.itemSetSizes.end() ? 0 : sizeItr->second;
    if (!setSize)
        return 0;
    for (uint8 i = 0; i < 8; ++i)
        if (set->spells[i] && set->items_to_triggerspell[i])
            score += ScoreAura(w, sSpellMgr.GetSpellEntry(set->spells[i]), 1.0f) /
                float(setSize);
    return score * sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_SET_BONUS_SCALE);
}

float ScoreItemBaseForPlayer(Weights const& w, Player const* player,
    ItemPrototype const* item)
{
    float score = ScoreItem(w, item);
    if (item->Class == ITEM_CLASS_ARMOR &&
        item->GetProficiencySkill() == player->GetHighestKnownArmorProficiency() &&
        item->InventoryType != INVTYPE_CLOAK && item->InventoryType != INVTYPE_SHIELD &&
        item->InventoryType != INVTYPE_HOLDABLE)
        score *= 1.0f +
            sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_ARMOR_CLASS_BONUS_PERCENT) / 100.0f;
    return score;
}

float ScoreItemForPlayer(Weights const& w, Player const* player,
    ItemPrototype const* item, AutoProgressionCache const& cache)
{
    return ScoreItemBaseForPlayer(w, player, item) +
        ScoreSetPotential(w, player, item, cache);
}

float ScoreItemEnchantments(Weights const& w, Item const* item)
{
    if (!item)
        return 0;
    float score = 0;
    EnchantmentSlot const slots[] =
    {
        PERM_ENCHANTMENT_SLOT,
        PROP_ENCHANTMENT_SLOT_0,
        PROP_ENCHANTMENT_SLOT_1,
        PROP_ENCHANTMENT_SLOT_2,
        PROP_ENCHANTMENT_SLOT_3
    };
    for (EnchantmentSlot slot : slots)
    {
        uint32 const enchantId = item->GetEnchantmentId(slot);
        if (enchantId)
            score += ScoreEnchantment(w, item->GetProto(),
                sSpellItemEnchantmentStore.LookupEntry(enchantId));
    }
    return score;
}

float ScoreItemInstance(Weights const& w, Player const* player,
    Item const* item, AutoProgressionCache const& cache)
{
    if (!item)
        return 0;
    return ScoreItemForPlayer(w, player, item->GetProto(), cache) +
        ScoreItemEnchantments(w, item);
}

bool IsSupportedScoredAura(AuraType aura)
{
    switch (aura)
    {
        case SPELL_AURA_MOD_ATTACKSPEED:
        case SPELL_AURA_MOD_MELEE_HASTE:
        case SPELL_AURA_MOD_DECREASE_SPEED:
        case SPELL_AURA_MOD_RANGED_HASTE:
        case SPELL_AURA_MOD_INCREASE_SPEED:
        case SPELL_AURA_MOD_THREAT:
        case SPELL_AURA_PROC_TRIGGER_SPELL:
        case SPELL_AURA_MOD_DAMAGE_DONE:
        case SPELL_AURA_MOD_DAMAGE_DONE_CREATURE:
        case SPELL_AURA_MOD_HEALING_DONE:
        case SPELL_AURA_MOD_HEALING_DONE_PERCENT:
        case SPELL_AURA_MOD_ATTACK_POWER:
        case SPELL_AURA_MOD_RANGED_ATTACK_POWER:
        case SPELL_AURA_MOD_HIT_CHANCE:
        case SPELL_AURA_MOD_SPELL_HIT_CHANCE:
        case SPELL_AURA_MOD_CRIT_PERCENT:
        case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
        case SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL:
        case SPELL_AURA_MOD_DODGE_PERCENT:
        case SPELL_AURA_MOD_PARRY_PERCENT:
        case SPELL_AURA_MOD_BLOCK_PERCENT:
        case SPELL_AURA_MOD_SHIELD_BLOCKVALUE:
        case SPELL_AURA_MOD_SHIELD_BLOCKVALUE_PCT:
        case SPELL_AURA_MOD_RESISTANCE:
        case SPELL_AURA_MOD_RESISTANCE_EXCLUSIVE:
        case SPELL_AURA_MOD_POWER_REGEN:
        case SPELL_AURA_MOD_MANA_REGEN_INTERRUPT:
        case SPELL_AURA_MOD_INCREASE_HEALTH:
        case SPELL_AURA_MOD_INCREASE_ENERGY:
        case SPELL_AURA_SCHOOL_ABSORB:
        case SPELL_AURA_MOD_SKILL:
        case SPELL_AURA_MOD_SKILL_TALENT:
        case SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE:
        case SPELL_AURA_MOD_STAT:
            return true;
        default:
            return false;
    }
}

bool HasUnsupportedSetBonusEffects(SpellEntry const* spell)
{
    if (!spell)
        return true;
    bool hasEffect = false;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (!spell->Effect[i])
            continue;
        hasEffect = true;
        if (spell->Effect[i] != SPELL_EFFECT_APPLY_AURA ||
            !IsSupportedScoredAura(AuraType(spell->EffectApplyAuraName[i])))
            return true;
    }
    return !hasEffect;
}

float ScoreExactSetBonuses(Weights const& w, Player const* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& plan)
{
    std::unordered_map<uint32, uint32> counts;
    for (ItemPrototype const* item : plan)
        if (item && item->ItemSet)
            ++counts[item->ItemSet];

    float score = 0;
    for (auto const& count : counts)
    {
        ItemSetEntry const* set = sItemSetStore.LookupEntry(count.first);
        if (!set)
            continue;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_6_1
        if (set->required_skill_id &&
            player->GetSkillValue(set->required_skill_id) < set->required_skill_value)
            continue;
#endif
        for (uint8 i = 0; i < 8; ++i)
            if (set->spells[i] && set->items_to_triggerspell[i] <= count.second)
                score += ScoreAura(w, sSpellMgr.GetSpellEntry(set->spells[i]), 1.0f);
    }
    return score * sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_SET_BONUS_SCALE);
}

float ScoreEquipmentPlan(Weights const& w, Player const* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& plan)
{
    float score = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        ItemPrototype const* item = plan[slot];
        if (!item)
            continue;
        Item const* current =
            player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        float itemScore = ScoreItemBaseForPlayer(w, player, item);
        if (current && current->GetEntry() == item->ItemId)
            itemScore += ScoreItemEnchantments(w, current);
        if (slot == EQUIPMENT_SLOT_MAINHAND && w.twoHand)
            itemScore *= item->InventoryType == INVTYPE_2HWEAPON ? 1.20f : 0.90f;
        else if (slot == EQUIPMENT_SLOT_OFFHAND && item->Class == ITEM_CLASS_WEAPON)
            itemScore *= 0.70f;
        score += itemScore;
    }
    ItemPrototype const* offhand = plan[EQUIPMENT_SLOT_OFFHAND];
    if (w.dualWield && offhand && offhand->Class == ITEM_CLASS_WEAPON)
        score += 8.0f;
    return score + ScoreExactSetBonuses(w, player, plan);
}

void ProtectPlanAgainstSetBonusLoss(Player* player, Weights const& w,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> currentPlan = {};
    std::unordered_map<uint32, uint32> currentCounts;
    std::unordered_map<uint32, uint32> plannedCounts;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (current)
        {
            currentPlan[slot] = current->GetProto();
            if (currentPlan[slot]->ItemSet)
                ++currentCounts[currentPlan[slot]->ItemSet];
        }
        if (plan[slot] && plan[slot]->ItemSet)
            ++plannedCounts[plan[slot]->ItemSet];
    }

    bool const planScoreLower =
        ScoreEquipmentPlan(w, player, plan) + 0.001f <
        ScoreEquipmentPlan(w, player, currentPlan);
    std::set<uint32> protectedSets;
    for (auto const& count : currentCounts)
    {
        ItemSetEntry const* set = sItemSetStore.LookupEntry(count.first);
        if (!set)
            continue;
#if SUPPORTED_CLIENT_BUILD > CLIENT_BUILD_1_6_1
        if (set->required_skill_id &&
            player->GetSkillValue(set->required_skill_id) <
                set->required_skill_value)
            continue;
#endif
        uint32 const plannedCount = plannedCounts[count.first];
        for (uint8 i = 0; i < 8; ++i)
        {
            if (!set->spells[i] ||
                set->items_to_triggerspell[i] > count.second ||
                set->items_to_triggerspell[i] <= plannedCount)
                continue;
            SpellEntry const* spell =
                sSpellMgr.GetSpellEntry(set->spells[i]);
            float const bonusScore = ScoreAura(w, spell, 1.0f);
            bool const unsupported = HasUnsupportedSetBonusEffects(spell);
            if (unsupported || (planScoreLower && bonusScore > 0.001f))
            {
                protectedSets.insert(count.first);
#ifdef MANGOS_DEBUG
                if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
                    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
                        "PlayerAutoProgression[debug]: set-guard player=%s set=%u currentPieces=%u plannedPieces=%u spell=%u score=%.2f reason=%s.",
                        player->GetName(), count.first, count.second,
                        plannedCount, set->spells[i], bonusScore,
                        unsupported ? "unsupported" : "plan-score-loss");
#endif
                break;
            }
        }
    }

    if (protectedSets.empty())
        return;

    bool protectHands = false;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (currentPlan[slot] &&
            protectedSets.count(currentPlan[slot]->ItemSet))
        {
            plan[slot] = currentPlan[slot];
            protectHands = protectHands || slot == EQUIPMENT_SLOT_MAINHAND ||
                slot == EQUIPMENT_SLOT_OFFHAND;
        }
    }
    if (protectHands)
    {
        plan[EQUIPMENT_SLOT_MAINHAND] = currentPlan[EQUIPMENT_SLOT_MAINHAND];
        plan[EQUIPMENT_SLOT_OFFHAND] = currentPlan[EQUIPMENT_SLOT_OFFHAND];
    }
}

bool MeetsUpgradeThreshold(float currentScore, float candidateScore)
{
    if (candidateScore <= currentScore + 0.001f)
        return false;
    float const percent =
        sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_MIN_UPGRADE_PERCENT);
    if (percent <= 0 || currentScore <= 0)
        return true;
    return candidateScore >= currentScore * (1.0f + percent / 100.0f);
}

bool HasStatsOrArmor(ItemPrototype const* item)
{
    if (item->Armor > 0 || item->Block > 0 || WeaponDps(item) > 0 || item->ItemSet ||
        item->HolyRes != 0 || item->FireRes != 0 || item->NatureRes != 0 ||
        item->FrostRes != 0 || item->ShadowRes != 0 || item->ArcaneRes != 0)
        return true;

    for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        if (item->ItemStat[i].ItemStatValue != 0)
            return true;

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (!item->Spells[i].SpellId)
            continue;

        switch (item->Spells[i].SpellTrigger)
        {
            case ITEM_SPELLTRIGGER_ON_EQUIP:
            case ITEM_SPELLTRIGGER_ON_USE:
            case ITEM_SPELLTRIGGER_CHANCE_ON_HIT:
                return true;
            default:
                break;
        }
    }

    return false;
}

struct Candidate
{
    ItemPrototype const* item;
    float score;
};
bool Better(Candidate const& a, Candidate const& b)
{
    return std::fabs(a.score - b.score) > 0.001f ? a.score > b.score : a.item->ItemLevel > b.item->ItemLevel;
}
bool Unique(ItemPrototype const* item)
{
    return item && (item->MaxCount == 1 || (item->Flags & ITEM_FLAG_UNIQUE_EQUIPPED));
}
void RemoveDuplicateUniquePlannedItems(Player* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    std::unordered_map<uint32, uint8> firstSlots;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        ItemPrototype const* item = plan[slot];
        if (!Unique(item))
            continue;
        auto const inserted = firstSlots.emplace(item->ItemId, slot);
        if (inserted.second)
            continue;

        uint8 const firstSlot = inserted.first->second;
        Item* firstCurrent =
            player->GetItemByPos(INVENTORY_SLOT_BAG_0, firstSlot);
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        bool const firstKeepsCurrent = firstCurrent &&
            firstCurrent->GetEntry() == item->ItemId;
        bool const keepsCurrent = current && current->GetEntry() == item->ItemId;
        if (keepsCurrent && !firstKeepsCurrent)
        {
            plan[firstSlot] = firstCurrent ? firstCurrent->GetProto() : nullptr;
            inserted.first->second = slot;
        }
        else
            plan[slot] = current ? current->GetProto() : nullptr;
    }
}
bool SlotAllowed(Weights const& w, ItemPrototype const* item, uint8 slot)
{
    if (slot != EQUIPMENT_SLOT_OFFHAND)
        return true;
    if (w.shield)
        return item->InventoryType == INVTYPE_SHIELD;
    return w.caster || item->InventoryType != INVTYPE_HOLDABLE;
}
bool Compatible(Player* player, Weights const& w, ItemPrototype const* main, ItemPrototype const* off)
{
    if (!main)
        return false;
    if (main->InventoryType == INVTYPE_2HWEAPON)
        return !off;
    if (!off)
        return !w.shield;
    if (w.shield)
        return off->InventoryType == INVTYPE_SHIELD;
    return off->Class != ITEM_CLASS_WEAPON || player->CanDualWield();
}

bool MoveEquippedItemToBag(Player* player, uint8 slot)
{
    Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!item)
        return true;

    ItemPosCountVec destination;
    uint8 bagSlot = 0;
    if (player->CanStoreItem(NULL_BAG, NULL_SLOT, destination, item, bagSlot, false) != EQUIP_ERR_OK)
        return false;

    player->RemoveItem(INVENTORY_SLOT_BAG_0, slot, false);
    if (player->StoreItem(destination, item, true))
        return true;

    player->EquipItem((uint16(INVENTORY_SLOT_BAG_0) << 8) | slot, item, true);
    return false;
}

void RestoreStoredItem(Player* player, uint8 slot, Item* item)
{
    if (!item || player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        return;
    if (item->GetSlot() != NULL_SLOT)
        player->RemoveItem(item->GetBagSlot(), item->GetSlot(), false);
    player->EquipItem((uint16(INVENTORY_SLOT_BAG_0) << 8) | slot, item, true);
}

struct QuestRewardCatalog
{
    std::map<uint32, uint32> minimumLevelForPlayer;
    std::unordered_map<uint32, uint32> maximumItemCounts;
    std::set<uint32> fixedItems;
    std::unordered_map<uint32, std::vector<uint32>> choiceQuests;
    std::unordered_map<uint32, std::vector<uint32>> allChoiceQuests;
    std::unordered_map<uint64, uint32> choiceCounts;
};

uint64 QuestItemKey(uint32 questId, uint32 itemId)
{
    return (uint64(questId) << 32) | itemId;
}

bool PassesBasicAutoEquipRules(Player* player, ItemPrototype const* item)
{
    if (!item ||
        (item->Class != ITEM_CLASS_WEAPON && item->Class != ITEM_CLASS_ARMOR) ||
        !item->InventoryType || item->InventoryType == INVTYPE_BODY ||
        item->InventoryType == INVTYPE_TABARD ||
        item->InventoryType == INVTYPE_BAG ||
        item->InventoryType == INVTYPE_AMMO ||
        item->InventoryType == INVTYPE_QUIVER ||
        item->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE) || item->Duration ||
        item->RandomProperty ||
        item->Quality > sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_MAX_QUALITY) ||
        (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_REQUIRE_DISCOVERED) &&
         !item->Discovered) ||
        (sWorld.getConfig(
             CONFIG_BOOL_AUTO_EQUIP_IGNORE_ITEMS_WITHOUT_STATS_AND_ARMOR) &&
         !HasStatsOrArmor(item)))
        return false;

    uint32 const levelBonus =
        sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_MAX_ITEM_LEVEL_BONUS);
    if (item->RequiredLevel > player->GetLevel() ||
        (!item->RequiredLevel &&
         item->ItemLevel > player->GetLevel() + levelBonus) ||
        item->RequiredHonorRank || item->RequiredCityRank)
        return false;
    if (item->RequiredReputationFaction &&
        uint32(player->GetReputationRank(item->RequiredReputationFaction)) <
            item->RequiredReputationRank)
        return false;
    return player->CanUseItem(item, false) == EQUIP_ERR_OK;
}

uint32 SelectQuestChoiceReward(Player* player, Quest const* quest,
    uint32 questId, Weights const& weights, AutoProgressionCache const& cache)
{
    struct Choice
    {
        uint32 itemId;
        float score;
        uint32 itemLevel;
        uint32 quality;
    };
    std::vector<Choice> choices;
    for (uint32 itemId : quest->RewChoiceItemId)
    {
        ItemPrototype const* item = sObjectMgr.GetItemPrototype(itemId);
        if (!PassesBasicAutoEquipRules(player, item))
            continue;
        choices.push_back({ itemId,
            ScoreItemForPlayer(weights, player, item, cache),
            item->ItemLevel, item->Quality });
    }
    if (choices.empty())
        return 0;
    std::sort(choices.begin(), choices.end(), [](Choice const& left,
        Choice const& right)
    {
        if (std::fabs(left.score - right.score) > 0.001f)
            return left.score > right.score;
        if (left.itemLevel != right.itemLevel)
            return left.itemLevel > right.itemLevel;
        if (left.quality != right.quality)
            return left.quality > right.quality;
        return left.itemId < right.itemId;
    });
    Choice const best = choices.front();
    std::vector<uint32> equivalent;
    for (Choice const& choice : choices)
    {
        if (std::fabs(choice.score - best.score) > 0.001f ||
            choice.itemLevel != best.itemLevel ||
            choice.quality != best.quality)
            break;
        equivalent.push_back(choice.itemId);
    }
    uint64 const seed = uint64(player->GetGUIDLow()) * 0x9E3779B97F4A7C15ULL ^
        uint64(questId) * 0xBF58476D1CE4E5B9ULL;
    return equivalent[size_t(seed % equivalent.size())];
}

QuestRewardCatalog BuildQuestRewardCatalog(Player* player, Weights const& weights,
    AutoProgressionCache const& cache)
{
    QuestRewardCatalog catalog;
    for (auto const& entry : cache.rewardQuests)
    {
        for (QuestRewardSource const& reward : entry.second)
        {
            uint32 const questId = reward.questId;
            Quest const* quest = sObjectMgr.GetQuestTemplate(questId);
            if (!quest || !sObjectMgr.IsQuestTemplateLoaded(questId))
                continue;
            bool const rewarded = player->GetQuestRewardStatus(questId);
            QuestStatusData const* statusData =
                player->GetQuestStatusData(questId);
            uint32 const rewardedChoice =
                rewarded && statusData ? statusData->m_reward_choice : 0;
            if (reward.choice && rewarded &&
                rewardedChoice != entry.first)
                continue;
            if (!quest->IsActive() && !rewarded)
                continue;
            QuestStatus const status = player->GetQuestStatus(questId);
            bool const alreadyAccessible = rewarded ||
                status == QUEST_STATUS_INCOMPLETE ||
                status == QUEST_STATUS_COMPLETE;
            if (!alreadyAccessible && !player->CanTakeQuest(quest, false))
                continue;
            if (reward.choice)
            {
                std::vector<uint32>& allQuests =
                    catalog.allChoiceQuests[entry.first];
                if (std::find(allQuests.begin(), allQuests.end(), questId) ==
                    allQuests.end())
                    allQuests.push_back(questId);
                uint32 const selectedChoice = rewardedChoice ?
                    rewardedChoice :
                    SelectQuestChoiceReward(player, quest, questId,
                        weights, cache);
                if (selectedChoice != entry.first)
                    continue;
            }
            auto const itr = catalog.minimumLevelForPlayer.find(entry.first);
            if (itr == catalog.minimumLevelForPlayer.end() || quest->GetMinLevel() < itr->second)
                catalog.minimumLevelForPlayer[entry.first] = quest->GetMinLevel();
            catalog.maximumItemCounts[entry.first] += reward.count;
            if (reward.choice)
            {
                std::vector<uint32>& quests = catalog.choiceQuests[entry.first];
                if (std::find(quests.begin(), quests.end(), questId) == quests.end())
                    quests.push_back(questId);
                catalog.choiceCounts[QuestItemKey(questId, entry.first)] +=
                    reward.count;
            }
            else
                catalog.fixedItems.insert(entry.first);
        }
    }
    return catalog;
}

bool IsBoEQuestRewardAvailableWithoutQuest(ItemPrototype const* item,
    uint32 sourceMask, AutoProgressionCache const& cache)
{
    return item && item->Bonding == BIND_WHEN_EQUIPPED &&
        (sourceMask & ITEM_SOURCE_QUEST) &&
        cache.activeQuestRewardItems.count(item->ItemId) != 0 &&
        sWorld.getConfig(
            CONFIG_BOOL_AUTO_EQUIP_ALLOW_BOE_QUEST_REWARDS_WITHOUT_QUEST);
}

bool ItemEligibleForAutoEquip(Player* player, ItemPrototype const* item,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards)
{
    if (!PassesBasicAutoEquipRules(player, item))
        return false;

    uint32 sourceMask = 0;
    auto const sourceItr = cache.itemSources.find(item->ItemId);
    if (sourceItr != cache.itemSources.end())
        sourceMask = sourceItr->second;
    uint32 const enabledNonQuest = EnabledItemSources(sourceMask, false);
    bool questAvailable = false;
    if (sourceMask & ITEM_SOURCE_QUEST)
    {
        auto const questLevel = questRewards.minimumLevelForPlayer.find(item->ItemId);
        questAvailable = IsBoEQuestRewardAvailableWithoutQuest(item,
                sourceMask, cache) ||
            (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_INCLUDE_QUEST_REWARDS) &&
             questLevel != questRewards.minimumLevelForPlayer.end() &&
             questLevel->second <= player->GetLevel());
    }
    if (!enabledNonQuest && !questAvailable)
        return false;

    return true;
}

bool RequiresQuestChoice(ItemPrototype const* item,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards)
{
    if (!item || questRewards.fixedItems.count(item->ItemId))
        return false;
    auto const sourceItr = cache.itemSources.find(item->ItemId);
    uint32 const sourceMask = sourceItr == cache.itemSources.end() ?
        ITEM_SOURCE_NONE : sourceItr->second;
    if (EnabledItemSources(sourceMask, false) ||
        IsBoEQuestRewardAvailableWithoutQuest(item, sourceMask, cache))
        return false;
    return questRewards.allChoiceQuests.count(item->ItemId) != 0;
}

bool RequiresQuestOnly(ItemPrototype const* item,
    AutoProgressionCache const& cache)
{
    if (!item)
        return false;
    auto const sourceItr = cache.itemSources.find(item->ItemId);
    if (sourceItr == cache.itemSources.end())
        return false;
    uint32 const sourceMask = sourceItr->second;
    return (sourceMask & ITEM_SOURCE_QUEST) &&
        !EnabledItemSources(sourceMask, false) &&
        !IsBoEQuestRewardAvailableWithoutQuest(item, sourceMask, cache);
}

void EnforcePrototypeMaxCounts(Player* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    struct PlannedItem
    {
        uint8 slot;
        bool keepsCurrent;
    };
    std::unordered_map<uint32, std::vector<PlannedItem>> planned;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        ItemPrototype const* item = plan[slot];
        if (!item || item->MaxCount <= 0)
            continue;
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        planned[item->ItemId].push_back({ slot,
            current && current->GetEntry() == item->ItemId });
    }

    for (auto& entry : planned)
    {
        ItemPrototype const* item = sObjectMgr.GetItemPrototype(entry.first);
        if (!item || item->MaxCount <= 0)
            continue;
        std::vector<PlannedItem>& items = entry.second;
        uint32 const owned = player->GetItemCount(entry.first, true);
        uint32 equippedOwned = 0;
        for (PlannedItem const& plannedItem : items)
            if (plannedItem.keepsCurrent)
                ++equippedOwned;
        uint32 const ownedOutsidePlan = owned > equippedOwned ?
            owned - equippedOwned : 0;
        uint32 const maximum = uint32(item->MaxCount);
        uint32 const planLimit = maximum > ownedOutsidePlan ?
            maximum - ownedOutsidePlan : 0;
        if (items.size() <= planLimit)
            continue;
        std::stable_sort(items.begin(), items.end(),
            [](PlannedItem const& left, PlannedItem const& right)
        {
            return left.keepsCurrent && !right.keepsCurrent;
        });
        for (size_t index = planLimit; index < items.size(); ++index)
        {
            uint8 const slot = items[index].slot;
            Item* current = player->GetItemByPos(
                INVENTORY_SLOT_BAG_0, slot);
            plan[slot] = current ? current->GetProto() : nullptr;
        }
    }
}

void EnforceQuestRewardCounts(Player* player, Weights const& weights,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    struct PlannedReward
    {
        uint8 slot;
        float score;
        bool keepsCurrent;
    };
    std::unordered_map<uint32, std::vector<PlannedReward>> planned;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        ItemPrototype const* item = plan[slot];
        if (!item || !RequiresQuestOnly(item, cache))
            continue;
        auto const limit = questRewards.maximumItemCounts.find(item->ItemId);
        if (limit == questRewards.maximumItemCounts.end() || !limit->second)
            continue;
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        float score = ScoreItemForPlayer(weights, player, item, cache);
        if (slot == EQUIPMENT_SLOT_MAINHAND && weights.twoHand)
            score *= item->InventoryType == INVTYPE_2HWEAPON ? 1.20f : 0.90f;
        else if (slot == EQUIPMENT_SLOT_OFFHAND &&
            item->Class == ITEM_CLASS_WEAPON)
            score *= 0.70f;
        planned[item->ItemId].push_back({ slot, score,
            current && current->GetEntry() == item->ItemId });
    }

    for (auto& entry : planned)
    {
        uint32 const maximum = questRewards.maximumItemCounts.at(entry.first);
        std::vector<PlannedReward>& items = entry.second;
        uint32 const owned = player->GetItemCount(entry.first, true);
        uint32 equippedOwned = 0;
        for (PlannedReward const& item : items)
            if (item.keepsCurrent)
                ++equippedOwned;
        uint32 const ownedOutsidePlan = owned > equippedOwned ?
            owned - equippedOwned : 0;
        uint32 const planLimit = maximum > ownedOutsidePlan ?
            maximum - ownedOutsidePlan : 0;
        if (items.size() <= planLimit)
            continue;
        std::sort(items.begin(), items.end(), [](PlannedReward const& left,
            PlannedReward const& right)
        {
            if (left.keepsCurrent != right.keepsCurrent)
                return left.keepsCurrent;
            return std::fabs(left.score - right.score) > 0.001f ?
                left.score > right.score : left.slot < right.slot;
        });
        for (size_t index = planLimit; index < items.size(); ++index)
        {
            uint8 const slot = items[index].slot;
            Item* current = player->GetItemByPos(
                INVENTORY_SLOT_BAG_0, slot);
            plan[slot] = current ? current->GetProto() : nullptr;
        }
    }

    ItemPrototype const* main = plan[EQUIPMENT_SLOT_MAINHAND];
    ItemPrototype const* off = plan[EQUIPMENT_SLOT_OFFHAND];
    if ((main || off) && !Compatible(player, weights, main, off))
    {
        Item* currentMain = player->GetItemByPos(
            INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item* currentOff = player->GetItemByPos(
            INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        plan[EQUIPMENT_SLOT_MAINHAND] =
            currentMain ? currentMain->GetProto() : nullptr;
        plan[EQUIPMENT_SLOT_OFFHAND] =
            currentOff ? currentOff->GetProto() : nullptr;
    }
}

void EnforceQuestChoiceExclusivity(Player* player, Weights const& weights,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    struct ChoiceRequest
    {
        uint8 slot;
        float score;
        bool keepsCurrent;
    };
    struct ChoiceGrant
    {
        uint32 questId;
        uint32 ordinal;
    };
    std::set<uint32> claimedQuests;
    for (auto const& entry : questRewards.allChoiceQuests)
    {
        ItemPrototype const* item = sObjectMgr.GetItemPrototype(entry.first);
        if (!item || !RequiresQuestChoice(item, cache, questRewards) ||
            !player->HasItemCount(entry.first, 1, true))
            continue;
        // An already generated choice reward consumes every quest that could
        // have supplied it; the character DB does not store its provenance.
        claimedQuests.insert(entry.second.begin(), entry.second.end());
    }

    std::vector<ChoiceRequest> requests;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        ItemPrototype const* item = plan[slot];
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item || (current && current->GetEntry() == item->ItemId) ||
            !RequiresQuestChoice(item, cache, questRewards))
            continue;
        float score = ScoreItemForPlayer(weights, player, item, cache);
        if (slot == EQUIPMENT_SLOT_MAINHAND && weights.twoHand)
            score *= item->InventoryType == INVTYPE_2HWEAPON ? 1.20f : 0.90f;
        else if (slot == EQUIPMENT_SLOT_OFFHAND && item->Class == ITEM_CLASS_WEAPON)
            score *= 0.70f;
        requests.push_back({ slot, score,
            current && current->GetEntry() == item->ItemId });
    }
    std::sort(requests.begin(), requests.end(), [](ChoiceRequest const& left,
        ChoiceRequest const& right)
    {
        if (left.keepsCurrent != right.keepsCurrent)
            return left.keepsCurrent;
        return std::fabs(left.score - right.score) > 0.001f ?
            left.score > right.score : left.slot < right.slot;
    });

    std::vector<std::vector<ChoiceGrant>> grants(requests.size());
    for (size_t requestIndex = 0; requestIndex < requests.size();
         ++requestIndex)
    {
        ItemPrototype const* item = plan[requests[requestIndex].slot];
        auto const quests = questRewards.choiceQuests.find(item->ItemId);
        if (quests == questRewards.choiceQuests.end())
            continue;
        for (uint32 questId : quests->second)
        {
            if (claimedQuests.count(questId))
                continue;
            auto const count = questRewards.choiceCounts.find(
                QuestItemKey(questId, item->ItemId));
            uint32 const capacity = count == questRewards.choiceCounts.end() ?
                1 : count->second;
            for (uint32 ordinal = 0; ordinal < capacity; ++ordinal)
                grants[requestIndex].push_back({ questId, ordinal });
        }
    }

    std::map<std::pair<uint32, uint32>, size_t> grantOwners;
    std::function<bool(size_t, std::set<std::pair<uint32, uint32>>&)> assign =
        [&](size_t requestIndex,
            std::set<std::pair<uint32, uint32>>& visited)
    {
        for (ChoiceGrant const& grant : grants[requestIndex])
        {
            std::pair<uint32, uint32> const key =
                { grant.questId, grant.ordinal };
            if (!visited.insert(key).second)
                continue;
            auto const owner = grantOwners.find(key);
            if (owner == grantOwners.end() || assign(owner->second, visited))
            {
                grantOwners[key] = requestIndex;
                return true;
            }
        }
        return false;
    };

    for (size_t requestIndex = 0; requestIndex < requests.size(); ++requestIndex)
    {
        std::set<std::pair<uint32, uint32>> visited;
        if (!assign(requestIndex, visited))
        {
            ChoiceRequest const& request = requests[requestIndex];
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, request.slot);
            plan[request.slot] = current ? current->GetProto() : nullptr;
        }
    }

    ItemPrototype const* main = plan[EQUIPMENT_SLOT_MAINHAND];
    ItemPrototype const* off = plan[EQUIPMENT_SLOT_OFFHAND];
    if ((main || off) && !Compatible(player, weights, main, off))
    {
        Item* currentMain = player->GetItemByPos(
            INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item* currentOff = player->GetItemByPos(
            INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        plan[EQUIPMENT_SLOT_MAINHAND] =
            currentMain ? currentMain->GetProto() : nullptr;
        plan[EQUIPMENT_SLOT_OFFHAND] =
            currentOff ? currentOff->GetProto() : nullptr;
    }
}

void ApplyLimitedItemRules(Player* player, Weights const& weights,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    for (uint8 pass = 0; pass < 3; ++pass)
    {
        EnforceQuestChoiceExclusivity(player, weights, cache, questRewards,
            plan);
        EnforceQuestRewardCounts(player, weights, cache, questRewards, plan);
        EnforcePrototypeMaxCounts(player, plan);
        RemoveDuplicateUniquePlannedItems(player, plan);
    }
}

bool TryAddPlanItem(Player* player, Weights const& weights,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards,
    uint8 slot, ItemPrototype const* item,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial = plan;
    trial[slot] = item;
    ApplyLimitedItemRules(player, weights, cache, questRewards, trial);
    if (trial[slot] != item)
        return false;
    for (uint8 other = EQUIPMENT_SLOT_START; other < EQUIPMENT_SLOT_END;
         ++other)
        if (other != slot && plan[other] && trial[other] != plan[other])
            return false;
    plan = trial;
    return true;
}

bool TrySetPlanHands(Player* player, Weights const& weights,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards,
    ItemPrototype const* main, ItemPrototype const* off,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& basePlan,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& result)
{
    if (!Compatible(player, weights, main, off))
        return false;
    result = basePlan;
    result[EQUIPMENT_SLOT_MAINHAND] = main;
    result[EQUIPMENT_SLOT_OFFHAND] = off;
    ApplyLimitedItemRules(player, weights, cache, questRewards, result);
    if (result[EQUIPMENT_SLOT_MAINHAND] != main ||
        result[EQUIPMENT_SLOT_OFFHAND] != off)
        return false;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (slot != EQUIPMENT_SLOT_MAINHAND && slot != EQUIPMENT_SLOT_OFFHAND &&
            basePlan[slot] && result[slot] != basePlan[slot])
            return false;
    return true;
}

void BackfillEmptyPlanSlots(Player* player, Weights const& weights,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards,
    std::array<std::vector<Candidate>, EQUIPMENT_SLOT_END> const& candidates,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND ||
            plan[slot])
            continue;
        for (Candidate const& candidate : candidates[slot])
        {
            if (candidate.score <= 0.001f)
                break;
            if (TryAddPlanItem(player, weights, cache, questRewards, slot,
                candidate.item, plan))
                break;
        }
    }

    ItemPrototype const* plannedMain = plan[EQUIPMENT_SLOT_MAINHAND];
    ItemPrototype const* plannedOff = plan[EQUIPMENT_SLOT_OFFHAND];
    if (plannedMain && plannedMain->InventoryType == INVTYPE_2HWEAPON)
        return;
    if (plannedMain && plannedOff)
        return;

    std::vector<Candidate> mainOptions;
    std::vector<Candidate> offOptions;
    if (plannedMain)
        mainOptions.push_back({ plannedMain,
            ScoreItemForPlayer(weights, player, plannedMain, cache) });
    else
    {
        size_t const limit = std::min<size_t>(
            candidates[EQUIPMENT_SLOT_MAINHAND].size(), 80);
        mainOptions.insert(mainOptions.end(),
            candidates[EQUIPMENT_SLOT_MAINHAND].begin(),
            candidates[EQUIPMENT_SLOT_MAINHAND].begin() + limit);
    }
    if (plannedOff)
        offOptions.push_back({ plannedOff,
            ScoreItemForPlayer(weights, player, plannedOff, cache) });
    else
    {
        size_t const limit = std::min<size_t>(
            candidates[EQUIPMENT_SLOT_OFFHAND].size(), 80);
        offOptions.insert(offOptions.end(),
            candidates[EQUIPMENT_SLOT_OFFHAND].begin(),
            candidates[EQUIPMENT_SLOT_OFFHAND].begin() + limit);
    }

    float best = -std::numeric_limits<float>::max();
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> bestPlan = plan;
    bool found = false;
    for (Candidate const& main : mainOptions)
    {
        float mainScore = main.score;
        if (weights.twoHand)
            mainScore *= main.item->InventoryType == INVTYPE_2HWEAPON ?
                1.20f : 0.90f;
        if (main.item->InventoryType == INVTYPE_2HWEAPON)
        {
            if (plannedOff)
                continue;
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
            if (mainScore > 0.001f && mainScore > best &&
                TrySetPlanHands(player, weights, cache, questRewards,
                    main.item, nullptr, plan, trial))
            {
                best = mainScore;
                bestPlan = trial;
                found = true;
            }
            continue;
        }
        for (Candidate const& off : offOptions)
        {
            float total = mainScore + off.score *
                (off.item->Class == ITEM_CLASS_WEAPON ? 0.70f : 1.0f);
            if (weights.dualWield && off.item->Class == ITEM_CLASS_WEAPON)
                total += 8.0f;
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
            if (total > 0.001f && total > best &&
                TrySetPlanHands(player, weights, cache, questRewards,
                    main.item, off.item, plan, trial))
            {
                best = total;
                bestPlan = trial;
                found = true;
            }
        }
        if (!plannedOff)
        {
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
            if (mainScore > 0.001f && mainScore > best &&
                TrySetPlanHands(player, weights, cache, questRewards,
                    main.item, nullptr, plan, trial))
            {
                best = mainScore;
                bestPlan = trial;
                found = true;
            }
        }
    }
    if (found)
        plan = bestPlan;
}

bool EquipPlannedItem(Player* player, uint8 slot, ItemPrototype const* item, bool replace,
    bool deleteReplaced)
{
    if (!item)
        return false;

    Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (current && current->GetEntry() == item->ItemId)
        return false;
    if (current && (!replace ||
        (deleteReplaced && (current->GetProto()->Flags & ITEM_FLAG_INDESTRUCTIBLE))))
        return false;

    Item* replacement = Item::CreateItem(item->ItemId, 1, player->GetObjectGuid());
    if (!replacement)
        return false;

    uint16 destination = 0;
    if (player->CanEquipItem(slot, destination, replacement, current != nullptr) != EQUIP_ERR_OK ||
        (current && player->CanUnequipItem(current->GetPos(), false) != EQUIP_ERR_OK))
    {
        delete replacement;
        return false;
    }

    if (current && deleteReplaced)
        player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    else if (current && !MoveEquippedItemToBag(player, slot))
    {
        delete replacement;
        return false;
    }

    player->ItemAddedQuestCheck(replacement->GetEntry(), 1);
    player->EquipItem(destination, replacement, true);
    return true;
}

#ifdef MANGOS_DEBUG
void LogEquipmentPlanBlock(Player* player, uint8 slot, Item* current,
    ItemPrototype const* candidate, char const* scope, char const* reason,
    InventoryResult result)
{
    if (!sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        return;
    sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
        "PlayerAutoProgression[debug]: equip-preflight player=%s scope=%s slot=%u current=%u candidate=%u result=%u reason=%s action=keep-current.",
        player->GetName(), scope, uint32(slot),
        current ? current->GetEntry() : 0,
        candidate ? candidate->ItemId : 0, uint32(result), reason);
}
#endif

uint32 PrepareEquipmentPlan(Player* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan,
    bool replace, bool deleteReplaced)
{
    uint32 blockedSlots = 0;
    std::vector<Item*> toStore;

    Item* currentMain = player->GetItemByPos(
        INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* currentOff = player->GetItemByPos(
        INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!plan[EQUIPMENT_SLOT_MAINHAND] && currentMain)
        plan[EQUIPMENT_SLOT_MAINHAND] = currentMain->GetProto();

    ItemPrototype const* mainPlan = plan[EQUIPMENT_SLOT_MAINHAND];
    bool const mainChanges = mainPlan &&
        (!currentMain || currentMain->GetEntry() != mainPlan->ItemId);
    bool const clearsCurrentOffhandForTwoHand = mainChanges && currentOff &&
        mainPlan->InventoryType == INVTYPE_2HWEAPON &&
        !plan[EQUIPMENT_SLOT_OFFHAND];
    if (!plan[EQUIPMENT_SLOT_OFFHAND] && currentOff &&
        !clearsCurrentOffhandForTwoHand)
        plan[EQUIPMENT_SLOT_OFFHAND] = currentOff->GetProto();

    ItemPrototype const* offPlan = plan[EQUIPMENT_SLOT_OFFHAND];
    bool const offChanges = offPlan &&
        (!currentOff || currentOff->GetEntry() != offPlan->ItemId);
    Item* handsToStore[2] = { nullptr, nullptr };
    int handCount = 0;
    if (mainChanges && currentMain)
        handsToStore[handCount++] = currentMain;
    if ((offChanges || clearsCurrentOffhandForTwoHand) && currentOff)
        handsToStore[handCount++] = currentOff;

    bool handsBlocked = false;
    uint8 handFailureSlot = NULL_SLOT;
    char const* handFailureReason = "unknown";
    InventoryResult handFailureResult = EQUIP_ERR_OK;
    auto blockHands = [&](uint8 slot, char const* reason,
        InventoryResult result)
    {
        if (handsBlocked)
            return;
        handsBlocked = true;
        handFailureSlot = slot;
        handFailureReason = reason;
        handFailureResult = result;
    };

    if (handCount && !replace)
        blockHands(handsToStore[0]->GetSlot(), "replace-disabled",
            EQUIP_ERR_OK);
    for (int i = 0; !handsBlocked && i < handCount; ++i)
    {
        InventoryResult const result =
            player->CanUnequipItem(handsToStore[i]->GetPos(), false);
        if (result != EQUIP_ERR_OK)
            blockHands(handsToStore[i]->GetSlot(), "cannot-unequip",
                result);
        else if (deleteReplaced &&
            (handsToStore[i]->GetProto()->Flags & ITEM_FLAG_INDESTRUCTIBLE))
            blockHands(handsToStore[i]->GetSlot(), "indestructible",
                EQUIP_ERR_CANT_DROP_SOULBOUND);
    }

    if (!handsBlocked && mainChanges)
    {
        uint16 destination = 0;
        InventoryResult const result = player->CanEquipNewItem(
            EQUIPMENT_SLOT_MAINHAND, destination, mainPlan->ItemId,
            currentMain != nullptr);
        bool const transitionalError = deleteReplaced &&
            clearsCurrentOffhandForTwoHand &&
            mainPlan->InventoryType == INVTYPE_2HWEAPON &&
            (result == EQUIP_ERR_ITEMS_CANT_BE_SWAPPED ||
             result == EQUIP_ERR_INVENTORY_FULL);
        if (result != EQUIP_ERR_OK && !transitionalError)
            blockHands(EQUIPMENT_SLOT_MAINHAND, "cannot-equip", result);
    }
    if (!handsBlocked && offChanges)
    {
        uint16 destination = 0;
        InventoryResult const result = player->CanEquipNewItem(
            EQUIPMENT_SLOT_OFFHAND, destination, offPlan->ItemId,
            currentOff != nullptr);
        bool const transitionalError =
            result == EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED &&
            currentMain &&
            currentMain->GetProto()->InventoryType == INVTYPE_2HWEAPON &&
            mainChanges;
        if (result != EQUIP_ERR_OK && !transitionalError)
            blockHands(EQUIPMENT_SLOT_OFFHAND, "cannot-equip", result);
    }

    if (!handsBlocked && !deleteReplaced && handCount)
    {
        std::vector<Item*> trial = toStore;
        trial.insert(trial.end(), handsToStore, handsToStore + handCount);
        InventoryResult const result =
            player->CanStoreItems(trial.data(), int(trial.size()));
        if (result != EQUIP_ERR_OK)
            blockHands(handsToStore[0]->GetSlot(), "cannot-store-replaced",
                result);
    }

    if (handsBlocked)
    {
        blockedSlots += mainChanges ? 1 : 0;
        blockedSlots += offChanges || clearsCurrentOffhandForTwoHand ? 1 : 0;
#ifdef MANGOS_DEBUG
        Item* failureCurrent = handFailureSlot == EQUIPMENT_SLOT_MAINHAND ?
            currentMain : handFailureSlot == EQUIPMENT_SLOT_OFFHAND ?
            currentOff : nullptr;
        ItemPrototype const* failureCandidate =
            handFailureSlot == EQUIPMENT_SLOT_MAINHAND ? mainPlan :
            handFailureSlot == EQUIPMENT_SLOT_OFFHAND ? offPlan : nullptr;
        LogEquipmentPlanBlock(player, handFailureSlot, failureCurrent,
            failureCandidate, "hands", handFailureReason,
            handFailureResult);
#endif
        plan[EQUIPMENT_SLOT_MAINHAND] =
            currentMain ? currentMain->GetProto() : nullptr;
        plan[EQUIPMENT_SLOT_OFFHAND] =
            currentOff ? currentOff->GetProto() : nullptr;
    }
    else if (!deleteReplaced)
        toStore.insert(toStore.end(), handsToStore,
            handsToStore + handCount);

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_MAINHAND ||
            slot == EQUIPMENT_SLOT_OFFHAND)
            continue;

        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        ItemPrototype const* candidate = plan[slot];
        if (!candidate && current)
        {
            plan[slot] = current->GetProto();
            candidate = plan[slot];
        }
        bool const changes = candidate &&
            (!current || current->GetEntry() != candidate->ItemId);
        if (!changes)
            continue;

        char const* failureReason = nullptr;
        InventoryResult failureResult = EQUIP_ERR_OK;
        if (current)
        {
            if (!replace)
                failureReason = "replace-disabled";
            else
            {
                InventoryResult const result =
                    player->CanUnequipItem(current->GetPos(), false);
                if (result != EQUIP_ERR_OK)
                {
                    failureReason = "cannot-unequip";
                    failureResult = result;
                }
                else if (deleteReplaced &&
                    (current->GetProto()->Flags & ITEM_FLAG_INDESTRUCTIBLE))
                {
                    failureReason = "indestructible";
                    failureResult = EQUIP_ERR_CANT_DROP_SOULBOUND;
                }
            }
        }

        if (!failureReason)
        {
            uint16 destination = 0;
            InventoryResult const result =
                player->CanEquipNewItem(slot, destination, candidate->ItemId,
                    current != nullptr);
            if (result != EQUIP_ERR_OK)
            {
                failureReason = "cannot-equip";
                failureResult = result;
            }
        }

        if (!failureReason && current && !deleteReplaced)
        {
            std::vector<Item*> trial = toStore;
            trial.push_back(current);
            InventoryResult const result =
                player->CanStoreItems(trial.data(), int(trial.size()));
            if (result != EQUIP_ERR_OK)
            {
                failureReason = "cannot-store-replaced";
                failureResult = result;
            }
        }

        if (failureReason)
        {
            ++blockedSlots;
#ifdef MANGOS_DEBUG
            LogEquipmentPlanBlock(player, slot, current, candidate, "slot",
                failureReason, failureResult);
#endif
            plan[slot] = current ? current->GetProto() : nullptr;
            continue;
        }

        if (current && !deleteReplaced)
            toStore.push_back(current);
    }
    return blockedSlots;
}

uint32 FinalizeEquipmentPlan(Player* player, Weights const& weights,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan,
    bool replace, bool deleteReplaced)
{
    uint32 blockedSlots = 0;
    for (uint8 pass = 0; pass < EQUIPMENT_SLOT_END; ++pass)
    {
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const before = plan;
        blockedSlots += PrepareEquipmentPlan(player, plan, replace,
            deleteReplaced);
        ApplyLimitedItemRules(player, weights, cache, questRewards, plan);
        if (replace)
            ProtectPlanAgainstSetBonusLoss(player, weights, plan);
        ApplyLimitedItemRules(player, weights, cache, questRewards, plan);
        if (plan == before)
            break;
    }
    return blockedSlots;
}

EnchantCandidate const* FindBestImprovement(Player* player,
    ItemPrototype const* item, uint8 equipmentSlot, Weights const& weights,
    AutoProgressionCache const& cache, float& bestScore)
{
    EnchantCandidate const* best = nullptr;
    uint32 bestLevel = 0;
    bestScore = -std::numeric_limits<float>::max();
    for (EnchantCandidate const& candidate : cache.improvements)
    {
        if (!ImprovementFits(player, item, equipmentSlot, candidate))
            continue;
        float const score = ScoreEnchantment(weights, item,
            sSpellItemEnchantmentStore.LookupEntry(candidate.enchantId));
        uint32 const level = ImprovementMinimumLevel(candidate);
        if (!best || score > bestScore + 0.001f ||
            (std::fabs(score - bestScore) <= 0.001f &&
             (level > bestLevel || (level == bestLevel && candidate.spellId > best->spellId))))
        {
            best = &candidate;
            bestScore = score;
            bestLevel = level;
        }
    }
    return best;
}

uint32 CountAvailableTrainerSpells(Player* player, AutoProgressionCache const& cache)
{
    TrainerCache const& trainers = cache.trainers;
    std::set<uint32> spells;
    auto collect = [&](std::vector<TrainerSource> const& sources)
    {
        for (TrainerSource const& trainerSource : sources)
        {
            TrainerSpellData const* trainer = trainerSource.isTemplate ?
                sObjectMgr.GetNpcTrainerTemplateSpells(trainerSource.id) :
                sObjectMgr.GetNpcTrainerSpells(trainerSource.id);
            if (!trainer)
                continue;
            for (auto const& entry : trainer->spellList)
                if (player->GetTrainerSpellState(&entry.second) == TRAINER_SPELL_GREEN)
                    spells.insert(entry.second.spell);
        }
    };
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_CLASS_TRAINERS) && player->GetClass() < 12)
        collect(trainers.classes[player->GetClass()]);
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_WEAPON_TRAINERS))
        collect(trainers.weapons);
    return uint32(spells.size());
}

char const* EquipmentSlotName(uint8 slot)
{
    static char const* names[EQUIPMENT_SLOT_END] =
    {
        "Head", "Neck", "Shoulders", "Shirt", "Chest", "Waist", "Legs", "Feet",
        "Wrists", "Hands", "Finger 1", "Finger 2", "Trinket 1", "Trinket 2",
        "Back", "Main hand", "Off hand", "Ranged", "Tabard"
    };
    return slot < EQUIPMENT_SLOT_END ? names[slot] : "Unknown";
}

std::string ItemSourceDescription(uint32 mask)
{
    if (!mask)
        return "other";
    std::ostringstream out;
    bool first = true;
    auto add = [&](uint32 source, char const* name)
    {
        if (!(mask & source))
            return;
        if (!first)
            out << ",";
        out << name;
        first = false;
    };
    add(ITEM_SOURCE_VENDOR, "vendor");
    add(ITEM_SOURCE_CRAFTED, "crafted");
    add(ITEM_SOURCE_QUEST, "quest");
    add(ITEM_SOURCE_WORLD_LOOT, "world");
    add(ITEM_SOURCE_DUNGEON_LOOT, "dungeon");
    add(ITEM_SOURCE_RAID_LOOT, "raid");
    add(ITEM_SOURCE_OTHER, "other");
    add(ITEM_SOURCE_RESTRICTED, "restricted");
    return out.str();
}

uint32 GetCachedItemSources(ItemPrototype const* item,
    AutoProgressionCache const& cache)
{
    if (!item)
        return 0;
    auto const itr = cache.itemSources.find(item->ItemId);
    return itr == cache.itemSources.end() ? 0 : itr->second;
}

char const* ItemModName(uint32 type)
{
    switch (type)
    {
        case ITEM_MOD_MANA: return "mana";
        case ITEM_MOD_HEALTH: return "health";
        case ITEM_MOD_AGILITY: return "agility";
        case ITEM_MOD_STRENGTH: return "strength";
        case ITEM_MOD_INTELLECT: return "intellect";
        case ITEM_MOD_SPIRIT: return "spirit";
        case ITEM_MOD_STAMINA: return "stamina";
        default: return "unknown";
    }
}

char const* ItemSpellTriggerName(uint32 trigger)
{
    switch (trigger)
    {
        case ITEM_SPELLTRIGGER_ON_USE: return "use";
        case ITEM_SPELLTRIGGER_ON_EQUIP: return "equip";
        case ITEM_SPELLTRIGGER_CHANCE_ON_HIT: return "proc";
        default: return "other";
    }
}

std::string DescribeScoredItem(ItemPrototype const* item, float score,
    AutoProgressionCache const& cache, Item const* instance)
{
    if (!item)
        return "(empty)";
    std::ostringstream out;
    out << "[" << item->ItemId << "] " << item->Name1 << " score=" << std::fixed <<
        std::setprecision(2) << score << " ilvl=" << uint32(item->ItemLevel);
    if (item->Armor)
        out << " armor=" << item->Armor;
    if (item->Block)
        out << " block=" << item->Block;
    float const dps = WeaponDps(item);
    if (dps > 0)
        out << " dps=" << std::setprecision(1) << dps;
    bool hasStats = false;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        if (!item->ItemStat[i].ItemStatValue)
            continue;
        out << (hasStats ? "," : " stats=") <<
            ItemModName(item->ItemStat[i].ItemStatType) << ":" <<
            item->ItemStat[i].ItemStatValue;
        hasStats = true;
    }
    bool hasResistances = false;
    auto addResistance = [&](char const* name, int32 value)
    {
        if (!value)
            return;
        out << (hasResistances ? "," : " res=") << name << ":" << value;
        hasResistances = true;
    };
    addResistance("holy", item->HolyRes);
    addResistance("fire", item->FireRes);
    addResistance("nature", item->NatureRes);
    addResistance("frost", item->FrostRes);
    addResistance("shadow", item->ShadowRes);
    addResistance("arcane", item->ArcaneRes);
    bool hasSpells = false;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (!item->Spells[i].SpellId)
            continue;
        out << (hasSpells ? "," : " spells=") <<
            ItemSpellTriggerName(item->Spells[i].SpellTrigger) << ":" <<
            item->Spells[i].SpellId;
        hasSpells = true;
    }
    if (item->ItemSet)
        out << " set=" << item->ItemSet;
    if (instance)
    {
        bool hasEnchants = false;
        auto addEnchant = [&](char const* name, EnchantmentSlot slot)
        {
            uint32 const enchantId = instance->GetEnchantmentId(slot);
            if (!enchantId)
                return;
            out << (hasEnchants ? "," : " enchants=") << name << ":" << enchantId;
            hasEnchants = true;
        };
        addEnchant("permanent", PERM_ENCHANTMENT_SLOT);
        addEnchant("property0", PROP_ENCHANTMENT_SLOT_0);
        addEnchant("property1", PROP_ENCHANTMENT_SLOT_1);
        addEnchant("property2", PROP_ENCHANTMENT_SLOT_2);
        addEnchant("property3", PROP_ENCHANTMENT_SLOT_3);
    }
    out << " source=" << ItemSourceDescription(GetCachedItemSources(item, cache));
    return out.str();
}

float UpgradePercent(float currentScore, float candidateScore)
{
    if (currentScore <= 0)
        return candidateScore > 0 ? 100.0f : 0;
    return (candidateScore / currentScore - 1.0f) * 100.0f;
}
}

namespace PlayerAutoProgression
{
CacheUpdateGuard::CacheUpdateGuard()
{
    BeginCacheUpdate();
}

CacheUpdateGuard::~CacheUpdateGuard()
{
    EndCacheUpdate();
}

CacheReadGuard::CacheReadGuard()
{
    BeginCacheRead();
}

CacheReadGuard::~CacheReadGuard()
{
    EndCacheRead();
}

uint32 LearnAvailableTrainerSpells(Player* player)
{
    CacheReadGuard cacheRead;
    if (!player || !player->IsInWorld())
        return 0;
#ifdef MANGOS_DEBUG
    uint32 const debugStarted = WorldTimer::getMSTime();
    uint32 debugPasses = 0;
#endif
    std::shared_ptr<AutoProgressionCache const> const cache =
        EnsureAutoProgressionCache();
    TrainerCache const& trainerCache = cache->trainers;

    uint32 learned = 0;
    for (uint8 pass = 0; pass < 20; ++pass)
    {
#ifdef MANGOS_DEBUG
        ++debugPasses;
#endif
        uint32 count = 0;
        if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_CLASS_TRAINERS) && player->GetClass() < 12)
            count += LearnSources(player, trainerCache.classes[player->GetClass()]);
        if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_WEAPON_TRAINERS))
            count += LearnSources(player, trainerCache.weapons);
        learned += count;
        if (!count)
            break;
    }

    if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_MAX_WEAPON_SKILLS))
        MaxCombatSkills(player);
#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: learn player=%s level=%u learned=%u passes=%u classSources=%u weaponSources=%u duration=%u ms.",
            player->GetName(), player->GetLevel(), learned, debugPasses,
            player->GetClass() < 12 ?
                uint32(trainerCache.classes[player->GetClass()].size()) : 0,
            uint32(trainerCache.weapons.size()),
            WorldTimer::getMSTimeDiffToNow(debugStarted));
#endif
    return learned;
}

uint32 EquipBestItems(Player* player)
{
    CacheReadGuard cacheRead;
    if (!player || !player->IsInWorld() ||
        player->GetLevel() < sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_MIN_LEVEL))
        return 0;

#ifdef MANGOS_DEBUG
    uint32 const debugStarted = WorldTimer::getMSTime();
    uint32 debugEligibleItems = 0;
    uint32 debugSlotCandidates = 0;
    uint32 debugHandPairsVisited = 0;
    uint32 debugHandConstraintChecks = 0;
    uint32 debugHandConstraintAccepted = 0;
#endif
    Weights const weights = GetWeights(player);
    std::array<std::vector<Candidate>, EQUIPMENT_SLOT_END> candidates;
    std::shared_ptr<AutoProgressionCache const> const cacheSnapshot =
        EnsureAutoProgressionCache();
    AutoProgressionCache const& cache = *cacheSnapshot;
    QuestRewardCatalog const questRewards =
        BuildQuestRewardCatalog(player, weights, cache);

    for (uint32 itemId : cache.equipmentItems)
    {
        ItemPrototype const* item = sObjectMgr.GetItemPrototype(itemId);
        if (!ItemEligibleForAutoEquip(player, item, cache, questRewards))
            continue;

#ifdef MANGOS_DEBUG
        ++debugEligibleItems;
#endif
        float const score = ScoreItemForPlayer(weights, player, item, cache);
        uint8 slots[4] = { NULL_SLOT, NULL_SLOT, NULL_SLOT, NULL_SLOT };
        item->GetAllowedEquipSlots(slots, player->GetClass(), player->CanDualWield());
        for (uint8 slot : slots)
            if (slot < EQUIPMENT_SLOT_END && SlotAllowed(weights, item, slot))
            {
                candidates[slot].push_back({ item, score });
#ifdef MANGOS_DEBUG
                ++debugSlotCandidates;
#endif
            }
    }

    for (auto& list : candidates)
        std::sort(list.begin(), list.end(), Better);

    auto addCurrentHandCandidate = [&](uint8 slot)
    {
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!current)
            return;
        auto& list = candidates[slot];
        float const currentScore =
            ScoreItemInstance(weights, player, current, cache);
        auto itr = std::find_if(list.begin(), list.end(),
            [current](Candidate const& candidate)
            { return candidate.item->ItemId == current->GetEntry(); });
        if (itr == list.end())
            list.push_back({ current->GetProto(),
                currentScore });
        else
            itr->score = currentScore;
        std::sort(list.begin(), list.end(), Better);
    };
    addCurrentHandCandidate(EQUIPMENT_SLOT_MAINHAND);
    addCurrentHandCandidate(EQUIPMENT_SLOT_OFFHAND);

    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> plan = {};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND)
            continue;
        for (Candidate const& candidate : candidates[slot])
        {
            if (candidate.score <= 0.001f)
                break;
            if (TryAddPlanItem(player, weights, cache, questRewards, slot,
                candidate.item, plan))
                break;
        }
    }

    uint8 const pairs[][2] =
    {
        { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 },
        { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 }
    };
    for (auto const& pair : pairs)
    {
        if (!plan[pair[0]] || !plan[pair[1]] || plan[pair[0]]->ItemId != plan[pair[1]]->ItemId ||
            !Unique(plan[pair[0]]))
            continue;
        plan[pair[1]] = nullptr;
        for (Candidate const& candidate : candidates[pair[1]])
            if (candidate.score > 0.001f &&
                candidate.item->ItemId != plan[pair[0]]->ItemId)
            {
                plan[pair[1]] = candidate.item;
                break;
            }
    }

    float best = -std::numeric_limits<float>::max();
    size_t const mainLimit = std::min<size_t>(candidates[EQUIPMENT_SLOT_MAINHAND].size(), 80);
    size_t const offLimit = std::min<size_t>(candidates[EQUIPMENT_SLOT_OFFHAND].size(), 80);
    for (size_t mi = 0; mi < mainLimit; ++mi)
    {
        Candidate const& main = candidates[EQUIPMENT_SLOT_MAINHAND][mi];
        float mainScore = main.score;
        if (weights.twoHand)
            mainScore *= main.item->InventoryType == INVTYPE_2HWEAPON ? 1.20f : 0.90f;

        if (main.item->InventoryType == INVTYPE_2HWEAPON)
        {
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
            if (mainScore > 0.001f && mainScore > best)
            {
#ifdef MANGOS_DEBUG
                ++debugHandConstraintChecks;
#endif
                if (TrySetPlanHands(player, weights, cache, questRewards,
                    main.item, nullptr, plan, trial))
                {
#ifdef MANGOS_DEBUG
                    ++debugHandConstraintAccepted;
#endif
                    best = mainScore;
                    plan = trial;
                }
            }
            continue;
        }

        for (size_t oi = 0; oi < offLimit; ++oi)
        {
#ifdef MANGOS_DEBUG
            ++debugHandPairsVisited;
#endif
            Candidate const& off = candidates[EQUIPMENT_SLOT_OFFHAND][oi];
            if (!Compatible(player, weights, main.item, off.item) ||
                (main.item->ItemId == off.item->ItemId && Unique(main.item)))
                continue;
            float total = mainScore + off.score * (off.item->Class == ITEM_CLASS_WEAPON ? 0.70f : 1.0f);
            if (weights.dualWield && off.item->Class == ITEM_CLASS_WEAPON)
                total += 8.0f;
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
            if (total > 0.001f && total > best)
            {
#ifdef MANGOS_DEBUG
                ++debugHandConstraintChecks;
#endif
                if (TrySetPlanHands(player, weights, cache, questRewards,
                    main.item, off.item, plan, trial))
                {
#ifdef MANGOS_DEBUG
                    ++debugHandConstraintAccepted;
#endif
                    best = total;
                    plan = trial;
                }
            }
        }
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
        if (mainScore > 0.001f && mainScore > best)
        {
#ifdef MANGOS_DEBUG
            ++debugHandConstraintChecks;
#endif
            if (TrySetPlanHands(player, weights, cache, questRewards,
                main.item, nullptr, plan, trial))
            {
#ifdef MANGOS_DEBUG
                ++debugHandConstraintAccepted;
#endif
                best = mainScore;
                plan = trial;
            }
        }
    }

    bool const replace = sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_REPLACE_EXISTING);
    if (replace)
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND)
                continue;
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!current)
                continue;
            if (!plan[slot] || current->GetEntry() == plan[slot]->ItemId)
            {
                plan[slot] = current->GetProto();
                continue;
            }
            if (!MeetsUpgradeThreshold(ScoreItemInstance(weights, player, current, cache),
                ScoreItemForPlayer(weights, player, plan[slot], cache)))
                plan[slot] = current->GetProto();
        }

        Item* currentMain = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item* currentOff = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        float currentHands = ScoreItemInstance(weights, player, currentMain, cache);
        if (currentMain && weights.twoHand)
            currentHands *= currentMain->GetProto()->InventoryType == INVTYPE_2HWEAPON ? 1.20f : 0.90f;
        if (currentOff)
            currentHands += ScoreItemInstance(weights, player, currentOff, cache) *
                (currentOff->GetProto()->Class == ITEM_CLASS_WEAPON ? 0.70f : 1.0f);
        if (weights.dualWield && currentOff &&
            currentOff->GetProto()->Class == ITEM_CLASS_WEAPON)
            currentHands += 8.0f;
        if ((currentMain || currentOff) && !MeetsUpgradeThreshold(currentHands, best))
        {
            plan[EQUIPMENT_SLOT_MAINHAND] = currentMain ? currentMain->GetProto() : nullptr;
            plan[EQUIPMENT_SLOT_OFFHAND] = currentOff ? currentOff->GetProto() : nullptr;
        }
    }
    else
    {
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (current)
                plan[slot] = current->GetProto();
        }
    }

    EnforceQuestChoiceExclusivity(player, weights, cache, questRewards, plan);
    EnforceQuestRewardCounts(player, weights, cache, questRewards, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    if (replace)
        ProtectPlanAgainstSetBonusLoss(player, weights, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    EnforceQuestChoiceExclusivity(player, weights, cache, questRewards, plan);
    EnforceQuestRewardCounts(player, weights, cache, questRewards, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    if (replace)
        ProtectPlanAgainstSetBonusLoss(player, weights, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    EnforceQuestChoiceExclusivity(player, weights, cache, questRewards, plan);
    EnforceQuestRewardCounts(player, weights, cache, questRewards, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    BackfillEmptyPlanSlots(player, weights, cache, questRewards, candidates,
        plan);
    ApplyLimitedItemRules(player, weights, cache, questRewards, plan);

    bool const deleteReplaced = sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_DELETE_REPLACED_ITEMS);
#ifdef MANGOS_DEBUG
    uint32 const debugBlockedSlots =
#endif
        FinalizeEquipmentPlan(player, weights, cache, questRewards, plan,
            replace, deleteReplaced);

#ifdef MANGOS_DEBUG
    uint32 debugPlannedChanges = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if ((!current && plan[slot]) ||
            (current && (!plan[slot] || current->GetEntry() != plan[slot]->ItemId)))
            ++debugPlannedChanges;
    }
#endif
    uint32 equipped = 0;

    ItemPrototype const* mainPlan = plan[EQUIPMENT_SLOT_MAINHAND];
    ItemPrototype const* offPlan = plan[EQUIPMENT_SLOT_OFFHAND];
    Item* oldMain = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* oldOff = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    bool const mainChanges = mainPlan && (!oldMain || oldMain->GetEntry() != mainPlan->ItemId);
    bool const offChanges = offPlan && (!oldOff || oldOff->GetEntry() != offPlan->ItemId);
    bool clearOffForTwoHand = mainChanges && mainPlan->InventoryType == INVTYPE_2HWEAPON && oldOff;

    bool handReady = !(player->IsInCombat() && mainChanges && offChanges &&
        offPlan->Class == ITEM_CLASS_WEAPON);
    Item* handsToStore[2] = { nullptr, nullptr };
    int handCount = 0;
    if (mainChanges && oldMain)
        handsToStore[handCount++] = oldMain;
    if ((offChanges || clearOffForTwoHand) && oldOff)
        handsToStore[handCount++] = oldOff;

    if (handCount)
    {
        if (!replace || (!deleteReplaced &&
            player->CanStoreItems(handsToStore, handCount) != EQUIP_ERR_OK))
            handReady = false;
        for (int i = 0; handReady && i < handCount; ++i)
            if (player->CanUnequipItem(handsToStore[i]->GetPos(), false) != EQUIP_ERR_OK ||
                (deleteReplaced && (handsToStore[i]->GetProto()->Flags & ITEM_FLAG_INDESTRUCTIBLE)))
                handReady = false;
    }

    Item* newMain = handReady && mainChanges ?
        Item::CreateItem(mainPlan->ItemId, 1, player->GetObjectGuid()) : nullptr;
    Item* newOff = handReady && offChanges ?
        Item::CreateItem(offPlan->ItemId, 1, player->GetObjectGuid()) : nullptr;
    uint16 mainDestination = 0;
    uint16 offDestination = 0;
    if ((mainChanges && !newMain) || (offChanges && !newOff))
        handReady = false;
    if (handReady && mainChanges)
    {
        InventoryResult const result = player->CanEquipItem(EQUIPMENT_SLOT_MAINHAND,
            mainDestination, newMain, oldMain != nullptr);
        bool const deleteTwoHandOffhand = deleteReplaced && clearOffForTwoHand &&
            (result == EQUIP_ERR_ITEMS_CANT_BE_SWAPPED || result == EQUIP_ERR_INVENTORY_FULL);
        if (deleteTwoHandOffhand)
            mainDestination = (uint16(INVENTORY_SLOT_BAG_0) << 8) | EQUIPMENT_SLOT_MAINHAND;
        else if (result != EQUIP_ERR_OK)
            handReady = false;
    }

    bool const changingCurrentTwoHand = oldMain &&
        oldMain->GetProto()->InventoryType == INVTYPE_2HWEAPON && mainChanges;
    if (handReady && offChanges)
    {
        InventoryResult const result = player->CanEquipItem(EQUIPMENT_SLOT_OFFHAND,
            offDestination, newOff, oldOff != nullptr);
        if (result == EQUIP_ERR_CANT_EQUIP_WITH_TWOHANDED && changingCurrentTwoHand)
            offDestination = (uint16(INVENTORY_SLOT_BAG_0) << 8) | EQUIPMENT_SLOT_OFFHAND;
        else if (result != EQUIP_ERR_OK)
            handReady = false;
    }

    bool mainStored = false;
    if (handReady && deleteReplaced)
    {
        if (mainChanges && oldMain)
            player->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND, true);
        if ((offChanges || clearOffForTwoHand) && oldOff)
            player->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
    }
    else if (handReady && mainChanges && oldMain)
    {
        mainStored = MoveEquippedItemToBag(player, EQUIPMENT_SLOT_MAINHAND);
        if (!mainStored)
            handReady = false;
    }
    if (handReady && !deleteReplaced && (offChanges || clearOffForTwoHand) && oldOff &&
        !MoveEquippedItemToBag(player, EQUIPMENT_SLOT_OFFHAND))
    {
        if (mainStored)
            RestoreStoredItem(player, EQUIPMENT_SLOT_MAINHAND, oldMain);
        handReady = false;
    }

    if (handReady)
    {
        if (newMain)
        {
            player->ItemAddedQuestCheck(newMain->GetEntry(), 1);
            player->EquipItem(mainDestination, newMain, true);
            newMain = nullptr;
            ++equipped;
        }
        if (newOff)
        {
            player->ItemAddedQuestCheck(newOff->GetEntry(), 1);
            player->EquipItem(offDestination, newOff, true);
            newOff = nullptr;
            ++equipped;
        }
    }
    delete newMain;
    delete newOff;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND)
            continue;
        equipped += EquipPlannedItem(player, slot, plan[slot], replace, deleteReplaced) ? 1 : 0;
    }
#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: equip player=%s level=%u catalog=%u eligible=%u slotCandidates=%u handPairs=%u handChecks=%u handAccepted=%u planned=%u blocked=%u equipped=%u result=complete duration=%u ms.",
            player->GetName(), player->GetLevel(),
            uint32(cache.equipmentItems.size()), debugEligibleItems,
            debugSlotCandidates, debugHandPairsVisited,
            debugHandConstraintChecks, debugHandConstraintAccepted,
            debugPlannedChanges, debugBlockedSlots, equipped,
            WorldTimer::getMSTimeDiffToNow(debugStarted));
#endif
    return equipped;
}

uint32 EnchantBestItems(Player* player)
{
    CacheReadGuard cacheRead;
    if (!player || !player->IsInWorld())
        return 0;

#ifdef MANGOS_DEBUG
    uint32 const debugStarted = WorldTimer::getMSTime();
    uint32 debugEquippedItems = 0;
    uint32 debugCandidatesFound = 0;
#endif
    Weights const weights = GetWeights(player);
    std::shared_ptr<AutoProgressionCache const> const cache =
        EnsureAutoProgressionCache();
    bool const replace = sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_REPLACE_EXISTING);
    uint32 enchanted = 0;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

#ifdef MANGOS_DEBUG
        ++debugEquippedItems;
#endif
        float bestScore = 0;
        EnchantCandidate const* best =
            FindBestImprovement(player, item->GetProto(), slot, weights, *cache,
                bestScore);
        if (!best || bestScore <= 0.001f)
            continue;

#ifdef MANGOS_DEBUG
        ++debugCandidatesFound;
#endif
        uint32 const bestEnchantId = best->enchantId;
        uint32 const currentEnchantId = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
        if (currentEnchantId == bestEnchantId || (currentEnchantId && !replace))
            continue;

        float currentScore = 0;
        if (currentEnchantId)
            currentScore = ScoreEnchantment(weights, item->GetProto(),
                sSpellItemEnchantmentStore.LookupEntry(currentEnchantId));
        if (currentEnchantId && bestScore <= currentScore + 0.001f)
            continue;

        player->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, false);
        item->SetEnchantment(PERM_ENCHANTMENT_SLOT, bestEnchantId, 0, 0,
            player->GetObjectGuid());
        player->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, true);
        ++enchanted;
    }
#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: enchant player=%s level=%u equippedItems=%u improvementCatalog=%u candidates=%u changed=%u duration=%u ms.",
            player->GetName(), player->GetLevel(), debugEquippedItems,
            uint32(cache->improvements.size()), debugCandidatesFound, enchanted,
            WorldTimer::getMSTimeDiffToNow(debugStarted));
#endif
    return enchanted;
}

bool RunOrDeferActions(Player* player, uint8 requestedActions,
    uint32& learned, uint32& equipped, uint32& enchanted);

void BuildStatus(Player* player, std::vector<std::string>& lines)
{
    CacheReadGuard cacheRead;
    lines.clear();
    if (!player)
    {
        lines.push_back("Auto progression: no player selected.");
        return;
    }

    std::shared_ptr<AutoProgressionCache const> const cacheSnapshot =
        EnsureAutoProgressionCache();
    AutoProgressionCache const& cache = *cacheSnapshot;
    std::ostringstream out;
    out << "Auto progression for " << player->GetName() << ": level " <<
        uint32(player->GetLevel()) << ", class " << uint32(player->GetClass()) <<
        ", talent tree " << LFGMgr::GetHighestTalentTree(player) << ".";
    lines.push_back(out.str());

    out.str("");
    out.clear();
    out << "Pending actions=" << uint32(player->GetPendingAutoProgressionActions()) <<
        ", debounce=" << player->GetAutoProgressionDelay() << " ms, cache: " <<
        cache.equipmentItems.size() << " equipment items, " <<
        cache.itemSources.size() << " source mappings, " <<
        cache.improvements.size() << " improvements.";
    lines.push_back(out.str());

    out.str("");
    out.clear();
    out << "Upgrade guard=" <<
        sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_MIN_UPGRADE_PERCENT) <<
        "%, highest armor bonus=" <<
        sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_ARMOR_CLASS_BONUS_PERCENT) <<
        "%, set scale=" << sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_SET_BONUS_SCALE) << ".";
    lines.push_back(out.str());

    out.str("");
    out.clear();
    out << "Sources: vendor=" << sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_VENDOR) <<
        " crafted=" << sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_CRAFTED) <<
        " quest=" << sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_INCLUDE_QUEST_REWARDS) <<
        " boe-without-quest=" << sWorld.getConfig(
            CONFIG_BOOL_AUTO_EQUIP_ALLOW_BOE_QUEST_REWARDS_WITHOUT_QUEST) <<
        " world=" << sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_WORLD_LOOT) <<
        " dungeon=" << sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_DUNGEON_LOOT) <<
        " raid=" << sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_RAID_LOOT) <<
        " other=" << sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SOURCE_OTHER) << ".";
    lines.push_back(out.str());
}

void BuildPreview(Player* player, std::vector<std::string>& lines)
{
    CacheReadGuard cacheRead;
    lines.clear();
    if (!player || !player->IsInWorld())
    {
        lines.push_back("Auto progression preview requires an online player.");
        return;
    }

#ifdef MANGOS_DEBUG
    uint32 const debugStarted = WorldTimer::getMSTime();
    uint32 debugEligibleItems = 0;
    uint32 debugSlotCandidates = 0;
    uint32 debugHandPairsVisited = 0;
    uint32 debugHandConstraintChecks = 0;
    uint32 debugHandConstraintAccepted = 0;
    uint32 debugBlockedSlots = 0;
#endif
    std::shared_ptr<AutoProgressionCache const> const cacheSnapshot =
        EnsureAutoProgressionCache();
    AutoProgressionCache const& cache = *cacheSnapshot;
    Weights const weights = GetWeights(player);
    QuestRewardCatalog const questRewards =
        BuildQuestRewardCatalog(player, weights, cache);
    bool const equipLevelEligible =
        player->GetLevel() >=
        sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_MIN_LEVEL);
    std::array<std::vector<Candidate>, EQUIPMENT_SLOT_END> candidates;
    if (equipLevelEligible)
    {
        for (uint32 itemId : cache.equipmentItems)
        {
            ItemPrototype const* item = sObjectMgr.GetItemPrototype(itemId);
            if (!ItemEligibleForAutoEquip(player, item, cache, questRewards))
                continue;
#ifdef MANGOS_DEBUG
            ++debugEligibleItems;
#endif
            float const score = ScoreItemForPlayer(weights, player, item, cache);
            uint8 slots[4] = { NULL_SLOT, NULL_SLOT, NULL_SLOT, NULL_SLOT };
            item->GetAllowedEquipSlots(slots, player->GetClass(), player->CanDualWield());
            for (uint8 slot : slots)
                if (slot < EQUIPMENT_SLOT_END && SlotAllowed(weights, item, slot))
                {
                    candidates[slot].push_back({ item, score });
#ifdef MANGOS_DEBUG
                    ++debugSlotCandidates;
#endif
                }
        }
    }
    for (auto& list : candidates)
        std::sort(list.begin(), list.end(), Better);

    auto addCurrentHandCandidate = [&](uint8 slot)
    {
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!current)
            return;
        auto& list = candidates[slot];
        float const currentScore =
            ScoreItemInstance(weights, player, current, cache);
        auto itr = std::find_if(list.begin(), list.end(),
            [current](Candidate const& candidate)
            { return candidate.item->ItemId == current->GetEntry(); });
        if (itr == list.end())
            list.push_back({ current->GetProto(),
                currentScore });
        else
            itr->score = currentScore;
        std::sort(list.begin(), list.end(), Better);
    };
    addCurrentHandCandidate(EQUIPMENT_SLOT_MAINHAND);
    addCurrentHandCandidate(EQUIPMENT_SLOT_OFFHAND);

    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> plan = {};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND)
            continue;
        for (Candidate const& candidate : candidates[slot])
        {
            if (candidate.score <= 0.001f)
                break;
            if (TryAddPlanItem(player, weights, cache, questRewards, slot,
                candidate.item, plan))
                break;
        }
    }

    uint8 const duplicatePairs[][2] =
    {
        { EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2 },
        { EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2 }
    };
    for (auto const& pair : duplicatePairs)
    {
        if (!plan[pair[0]] || !plan[pair[1]] ||
            plan[pair[0]]->ItemId != plan[pair[1]]->ItemId || !Unique(plan[pair[0]]))
            continue;
        plan[pair[1]] = nullptr;
        for (Candidate const& candidate : candidates[pair[1]])
            if (candidate.score > 0.001f &&
                candidate.item->ItemId != plan[pair[0]]->ItemId)
            {
                plan[pair[1]] = candidate.item;
                break;
            }
    }

    float bestHands = -std::numeric_limits<float>::max();
    size_t const mainLimit = std::min<size_t>(candidates[EQUIPMENT_SLOT_MAINHAND].size(), 80);
    size_t const offLimit = std::min<size_t>(candidates[EQUIPMENT_SLOT_OFFHAND].size(), 80);
    for (size_t mi = 0; mi < mainLimit; ++mi)
    {
        Candidate const& main = candidates[EQUIPMENT_SLOT_MAINHAND][mi];
        float mainScore = main.score;
        if (weights.twoHand)
            mainScore *= main.item->InventoryType == INVTYPE_2HWEAPON ? 1.20f : 0.90f;
        if (main.item->InventoryType == INVTYPE_2HWEAPON)
        {
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
            if (mainScore > 0.001f && mainScore > bestHands)
            {
#ifdef MANGOS_DEBUG
                ++debugHandConstraintChecks;
#endif
                if (TrySetPlanHands(player, weights, cache, questRewards,
                    main.item, nullptr, plan, trial))
                {
#ifdef MANGOS_DEBUG
                    ++debugHandConstraintAccepted;
#endif
                    bestHands = mainScore;
                    plan = trial;
                }
            }
            continue;
        }
        for (size_t oi = 0; oi < offLimit; ++oi)
        {
#ifdef MANGOS_DEBUG
            ++debugHandPairsVisited;
#endif
            Candidate const& off = candidates[EQUIPMENT_SLOT_OFFHAND][oi];
            if (!Compatible(player, weights, main.item, off.item) ||
                (main.item->ItemId == off.item->ItemId && Unique(main.item)))
                continue;
            float total = mainScore + off.score *
                (off.item->Class == ITEM_CLASS_WEAPON ? 0.70f : 1.0f);
            if (weights.dualWield && off.item->Class == ITEM_CLASS_WEAPON)
                total += 8.0f;
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
            if (total > 0.001f && total > bestHands)
            {
#ifdef MANGOS_DEBUG
                ++debugHandConstraintChecks;
#endif
                if (TrySetPlanHands(player, weights, cache, questRewards,
                    main.item, off.item, plan, trial))
                {
#ifdef MANGOS_DEBUG
                    ++debugHandConstraintAccepted;
#endif
                    bestHands = total;
                    plan = trial;
                }
            }
        }
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
        if (mainScore > 0.001f && mainScore > bestHands)
        {
#ifdef MANGOS_DEBUG
            ++debugHandConstraintChecks;
#endif
            if (TrySetPlanHands(player, weights, cache, questRewards,
                main.item, nullptr, plan, trial))
            {
#ifdef MANGOS_DEBUG
                ++debugHandConstraintAccepted;
#endif
                bestHands = mainScore;
                plan = trial;
            }
        }
    }

    bool const replace = sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_REPLACE_EXISTING);
    bool const deleteReplaced =
        sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_DELETE_REPLACED_ITEMS);
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND)
            continue;
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!current)
            continue;
        if (!replace || !plan[slot] ||
            !MeetsUpgradeThreshold(ScoreItemInstance(weights, player, current, cache),
                ScoreItemForPlayer(weights, player, plan[slot], cache)))
            plan[slot] = current->GetProto();
    }

    Item* currentMain = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* currentOff = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!replace)
    {
        if (currentMain)
            plan[EQUIPMENT_SLOT_MAINHAND] = currentMain->GetProto();
        if (currentOff)
            plan[EQUIPMENT_SLOT_OFFHAND] = currentOff->GetProto();
        if (plan[EQUIPMENT_SLOT_MAINHAND] &&
            plan[EQUIPMENT_SLOT_MAINHAND]->InventoryType == INVTYPE_2HWEAPON)
            plan[EQUIPMENT_SLOT_OFFHAND] = nullptr;
    }
    else if (currentMain || currentOff)
    {
        float currentHands = ScoreItemInstance(weights, player, currentMain, cache);
        if (currentMain && weights.twoHand)
            currentHands *= currentMain->GetProto()->InventoryType == INVTYPE_2HWEAPON ?
                1.20f : 0.90f;
        if (currentOff)
            currentHands += ScoreItemInstance(weights, player, currentOff, cache) *
                (currentOff->GetProto()->Class == ITEM_CLASS_WEAPON ? 0.70f : 1.0f);
        if (weights.dualWield && currentOff &&
            currentOff->GetProto()->Class == ITEM_CLASS_WEAPON)
            currentHands += 8.0f;
        if (!MeetsUpgradeThreshold(currentHands, bestHands))
        {
            plan[EQUIPMENT_SLOT_MAINHAND] = currentMain ? currentMain->GetProto() : nullptr;
            plan[EQUIPMENT_SLOT_OFFHAND] = currentOff ? currentOff->GetProto() : nullptr;
        }
    }

    EnforceQuestChoiceExclusivity(player, weights, cache, questRewards, plan);
    EnforceQuestRewardCounts(player, weights, cache, questRewards, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    if (replace)
        ProtectPlanAgainstSetBonusLoss(player, weights, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    EnforceQuestChoiceExclusivity(player, weights, cache, questRewards, plan);
    EnforceQuestRewardCounts(player, weights, cache, questRewards, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    if (replace)
        ProtectPlanAgainstSetBonusLoss(player, weights, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    EnforceQuestChoiceExclusivity(player, weights, cache, questRewards, plan);
    EnforceQuestRewardCounts(player, weights, cache, questRewards, plan);
    RemoveDuplicateUniquePlannedItems(player, plan);
    BackfillEmptyPlanSlots(player, weights, cache, questRewards, candidates,
        plan);
    ApplyLimitedItemRules(player, weights, cache, questRewards, plan);
#ifdef MANGOS_DEBUG
    debugBlockedSlots =
#endif
        FinalizeEquipmentPlan(player, weights, cache, questRewards, plan,
            replace, deleteReplaced);

    std::ostringstream header;
    header << "Preview for " << player->GetName() << ": " <<
        CountAvailableTrainerSpells(player, cache) <<
        " trainer spells currently learnable; equipment eligibility uses current skills.";
    lines.push_back(header.str());
    if (!equipLevelEligible)
    {
        std::ostringstream note;
        note << "AutoEquip is inactive below configured MinLevel " <<
            sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_MIN_LEVEL) <<
            "; only current-item enchant changes are shown.";
        lines.push_back(note.str());
    }

    uint32 changes = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        ItemPrototype const* candidate = plan[slot];
        if ((!current && !candidate) ||
            (current && candidate && current->GetEntry() == candidate->ItemId))
            continue;
        float const currentScore = ScoreItemInstance(weights, player, current, cache);
        float const candidateScore = candidate ?
            ScoreItemForPlayer(weights, player, candidate, cache) : 0;
        std::ostringstream out;
        out << EquipmentSlotName(slot) << ": " <<
            DescribeScoredItem(current ? current->GetProto() : nullptr,
                currentScore, cache, current) <<
            " -> " << DescribeScoredItem(candidate, candidateScore, cache, nullptr);
        if (candidate)
            out << " (" << std::showpos << std::fixed << std::setprecision(1) <<
                UpgradePercent(currentScore, candidateScore) << "%)";
        else
            out << " (removed for hand compatibility)";
        lines.push_back(out.str());
        ++changes;
    }

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        ItemPrototype const* plannedItem = plan[slot];
        if (!plannedItem)
            continue;
        Item* currentItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        bool const keepsCurrent = currentItem &&
            currentItem->GetEntry() == plannedItem->ItemId;
        float bestScore = 0;
        EnchantCandidate const* best =
            FindBestImprovement(player, plannedItem, slot, weights, cache,
                bestScore);
        if (!best || bestScore <= 0.001f)
            continue;
        uint32 const currentId = keepsCurrent ?
            currentItem->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) : 0;
        float currentScore = currentId ? ScoreEnchantment(weights, plannedItem,
            sSpellItemEnchantmentStore.LookupEntry(currentId)) : 0;
        if (currentId == best->enchantId ||
            (currentId && !sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_REPLACE_EXISTING)) ||
            (currentId && bestScore <= currentScore + 0.001f))
            continue;
        std::ostringstream out;
        out << EquipmentSlotName(slot) << " enchant on [" << plannedItem->ItemId <<
            "] " << plannedItem->Name1 << ": " << currentId << " (score " <<
            std::fixed << std::setprecision(2) << currentScore << ") -> " <<
            best->enchantId << " via spell " << best->spellId << " (score " <<
            bestScore << ").";
        lines.push_back(out.str());
        ++changes;
    }

    if (!changes)
        lines.push_back("No equipment or enchantment changes pass the configured rules.");
#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: preview player=%s level=%u catalog=%u eligible=%u slotCandidates=%u handPairs=%u handChecks=%u handAccepted=%u changes=%u lines=%u blocked=%u plan=%s duration=%u ms.",
            player->GetName(), player->GetLevel(),
            uint32(cache.equipmentItems.size()), debugEligibleItems,
            debugSlotCandidates, debugHandPairsVisited,
            debugHandConstraintChecks, debugHandConstraintAccepted,
            changes, uint32(lines.size()), debugBlockedSlots,
            debugBlockedSlots ? "sanitized" : "ready",
            WorldTimer::getMSTimeDiffToNow(debugStarted));
#endif
}

bool ApplyNow(Player* player, uint32& learned, uint32& equipped, uint32& enchanted)
{
#ifdef MANGOS_DEBUG
    uint32 const debugStarted = WorldTimer::getMSTime();
#endif
    learned = equipped = enchanted = 0;
    if (!player || !player->GetSession() || !player->IsInWorld() || !player->IsAlive() ||
        player->IsInCombat() ||
        player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_PENDING_STUNNED) ||
        player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISARMED))
    {
#ifdef MANGOS_DEBUG
        if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
                "PlayerAutoProgression[debug]: apply player=%s result=rejected session=%u world=%u alive=%u combat=%u stunned=%u disarmed=%u duration=%u ms.",
                player ? player->GetName() : "<null>",
                player && player->GetSession() ? 1 : 0,
                player && player->IsInWorld() ? 1 : 0,
                player && player->IsAlive() ? 1 : 0,
                player && player->IsInCombat() ? 1 : 0,
                player && player->HasUnitState(
                    UNIT_STATE_STUNNED | UNIT_STATE_PENDING_STUNNED) ? 1 : 0,
                player && player->HasFlag(
                    UNIT_FIELD_FLAGS, UNIT_FLAG_DISARMED) ? 1 : 0,
                WorldTimer::getMSTimeDiffToNow(debugStarted));
#endif
        return false;
    }

    bool const deferred = RunOrDeferActions(player,
        PENDING_AUTO_LEARN | PENDING_AUTO_EQUIP | PENDING_AUTO_ENCHANT,
        learned, equipped, enchanted);
#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: apply player=%s result=%s learned=%u equipped=%u enchanted=%u duration=%u ms.",
            player->GetName(), deferred ? "deferred" : "complete",
            learned, equipped, enchanted,
            WorldTimer::getMSTimeDiffToNow(debugStarted));
#endif
    return !deferred;
}

bool RunOrDeferActions(Player* player, uint8 requestedActions,
    uint32& learned, uint32& equipped, uint32& enchanted)
{
#ifdef MANGOS_DEBUG
    uint32 const debugStarted = WorldTimer::getMSTime();
#endif
    uint8 const actions = requestedActions | player->GetPendingAutoProgressionActions();
    if (!actions)
        return false;

    bool const equipmentBlocked = (actions & (PENDING_AUTO_EQUIP | PENDING_AUTO_ENCHANT)) &&
        (player->IsInCombat() || player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_PENDING_STUNNED) ||
         player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISARMED));
    if (!player->IsAlive() || equipmentBlocked)
    {
        player->AddPendingAutoProgressionActions(actions);
#ifdef MANGOS_DEBUG
        if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
            sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
                "PlayerAutoProgression[debug]: run player=%s requested=0x%02X effective=0x%02X result=deferred duration=%u ms.",
                player->GetName(), uint32(requestedActions), uint32(actions),
                WorldTimer::getMSTimeDiffToNow(debugStarted));
#endif
        return true;
    }

    player->ClearPendingAutoProgressionActions();
    learned = (actions & PENDING_AUTO_LEARN) ? LearnAvailableTrainerSpells(player) : 0;
    equipped = (actions & PENDING_AUTO_EQUIP) ? EquipBestItems(player) : 0;
    enchanted = (actions & PENDING_AUTO_ENCHANT) ? EnchantBestItems(player) : 0;
#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: run player=%s requested=0x%02X effective=0x%02X learned=%u equipped=%u enchanted=%u duration=%u ms.",
            player->GetName(), uint32(requestedActions), uint32(actions),
            learned, equipped, enchanted,
            WorldTimer::getMSTimeDiffToNow(debugStarted));
#endif
    return false;
}

void OnLevelUp(Player* player)
{
    if (!player || !player->GetSession() || !player->IsInWorld())
        return;

    uint32 learned = 0;
    uint8 actions = 0;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_ON_LEVEL_UP))
        actions |= PENDING_AUTO_LEARN;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_ON_LEVEL_UP))
        actions |= PENDING_AUTO_EQUIP;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_ON_LEVEL_UP))
        actions |= PENDING_AUTO_ENCHANT;

    uint32 equipped = 0;
    uint32 enchanted = 0;
    bool const deferred = RunOrDeferActions(player, actions, learned, equipped, enchanted);
    if (learned || equipped || enchanted)
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
            "PlayerAutoProgression: %s learned %u trainer spells, equipped %u items and enchanted %u items at level %u.",
            player->GetName(), learned, equipped, enchanted, player->GetLevel());
    if (deferred)
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
            "PlayerAutoProgression: %s equipment update queued until equipment changes are allowed.",
            player->GetName());
}

void OnTalentLearned(Player* player)
{
    if (!player || !player->GetSession() || !player->IsInWorld())
        return;

    uint8 actions = 0;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_ON_TALENT_LEARN))
        actions |= PENDING_AUTO_LEARN;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_ON_TALENT_LEARN))
        actions |= PENDING_AUTO_EQUIP;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_ON_TALENT_LEARN))
        actions |= PENDING_AUTO_ENCHANT;

    if (!actions)
        return;
    player->AddPendingAutoProgressionActions(actions);
    player->SetAutoProgressionDelay(
        sWorld.getConfig(CONFIG_UINT32_AUTO_PROGRESSION_TALENT_DEBOUNCE_MS));
}

void OnLogin(Player* player)
{
    if (!player || !player->GetSession() || !player->IsInWorld())
        return;
    uint8 actions = 0;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_ON_LOGIN))
        actions |= PENDING_AUTO_LEARN;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_ON_LOGIN))
        actions |= PENDING_AUTO_EQUIP;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_ON_LOGIN))
        actions |= PENDING_AUTO_ENCHANT;
    player->AddPendingAutoProgressionActions(actions);
}

void OnTalentsReset(Player* player)
{
    if (!player || !player->GetSession() || !player->IsInWorld())
        return;
    uint8 actions = 0;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_ON_TALENT_RESET))
        actions |= PENDING_AUTO_EQUIP;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_ON_TALENT_RESET))
        actions |= PENDING_AUTO_ENCHANT;
    if (!actions)
        return;
    player->AddPendingAutoProgressionActions(actions);
    player->SetAutoProgressionDelay(
        sWorld.getConfig(CONFIG_UINT32_AUTO_PROGRESSION_TALENT_DEBOUNCE_MS));
}

void OnPlayerUpdate(Player* player, uint32 diff)
{
    if (!player || !player->GetSession() || !player->IsInWorld())
        return;
    player->UpdateAutoProgressionDelay(diff);
    if (player->GetAutoProgressionDelay() ||
        !player->IsAlive() || player->IsInCombat() ||
        player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_PENDING_STUNNED) ||
        player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISARMED) ||
        !player->GetPendingAutoProgressionActions())
        return;

    uint32 learned = 0;
    uint32 equipped = 0;
    uint32 enchanted = 0;
    RunOrDeferActions(player, 0, learned, equipped, enchanted);
    if (learned || equipped || enchanted)
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
            "PlayerAutoProgression: %s completed deferred update: learned %u spells, equipped %u items and enchanted %u items.",
            player->GetName(), learned, equipped, enchanted);
}

void InvalidateCaches()
{
    AutoProgressionCacheState& state = GetAutoProgressionCacheState();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.dirty = true;
}
}

#include "PlayerAutoProgression.h"

#include "Bag.h"
#include "Conditions.h"
#include "Creature.h"
#include "Database/DatabaseEnv.h"
#include "DBCStores.h"
#include "GameEventMgr.h"
#include "Item.h"
#include "LootMgr.h"
#include "Mail.h"
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
#include "SystemConfig.h"
#include "Timer.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <ctime>
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
#include <tuple>
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
    ITEM_SOURCE_RESTRICTED = 0x80,
    // Created by a class spell (warlock Spellstones/Firestones): only the
    // creating class can ever hold these; see classCreatedItems.
    ITEM_SOURCE_CLASS_CREATED = 0x100
};

// A class quest that hands out spells and/or class reagents (shaman totems).
struct ClassQuestGrant
{
    uint32 questId = 0;
    uint32 minLevel = 0;
    uint32 requiredClasses = 0;
    uint32 requiredRaces = 0;
    std::vector<uint32> spells;
    std::vector<uint32> items;
};

// An obtainable item that teaches class spells on use (tomes, codices, ...).
struct SpellBook
{
    uint32 itemId = 0;
    std::vector<uint32> spells;
};

// A loot/vendor source that only applies when every listed condition holds
// for the character (faction-split raid loot such as the tier 2 helms and
// legs, patch-gated drops, game-event loot, reputation vendors, ...).
struct ConditionalItemSource
{
    uint32 source = 0;
    ConditionSource context = CONDITION_FROM_LOOT;
    std::vector<uint32> conditions;
};

struct AutoProgressionCache
{
    TrainerCache trainers;
    std::vector<EnchantCandidate> improvements;
    std::vector<uint32> equipmentItems;
    std::vector<uint32> ammoItems;
    std::vector<ClassQuestGrant> classQuestGrants;
    std::vector<SpellBook> spellBooks;
    std::unordered_map<uint32, uint32> itemSources;
    std::unordered_map<uint32, uint32> classCreatedItems; // item -> creator class mask
    std::unordered_map<uint32, std::vector<ConditionalItemSource>> conditionalSources;
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

// Per-thread nesting information. Readers may nest freely (only the outermost
// guard registers with the shared state) and a reader may also be entered from
// inside an update on the same thread. An update entered while the same thread
// still holds a read guard cannot wait for the readers to drain without
// deadlocking, so that case is logged and treated as nested.
thread_local uint32 tCacheReadDepth = 0;
thread_local bool tCacheReadRegistered = false;

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
    if (tCacheReadRegistered)
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR,
            "PlayerAutoProgression: cache update requested while the same thread holds a read guard; proceeding without exclusive access.");
        if (state.activeUpdates == 0)
            state.updateThread = current;
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
    if (tCacheReadDepth++ > 0)
        return;

    AutoProgressionCacheState& state = GetAutoProgressionCacheState();
    std::unique_lock<std::mutex> lock(state.mutex);
    if (state.activeUpdates && state.updateThread == std::this_thread::get_id())
    {
        // Reading from inside an update on the same thread: the update already
        // owns exclusive access, registering as reader would self-deadlock.
        tCacheReadRegistered = false;
        return;
    }
    state.condition.wait(lock, [&state]()
    {
        return state.activeUpdates == 0 && state.waitingUpdates == 0;
    });
    ++state.activeReaders;
    tCacheReadRegistered = true;
}

void EndCacheRead()
{
    MANGOS_ASSERT(tCacheReadDepth > 0);
    if (--tCacheReadDepth > 0)
        return;
    if (!tCacheReadRegistered)
        return;
    tCacheReadRegistered = false;

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
    PENDING_AUTO_ENCHANT = 0x04,
    PENDING_AUTO_TALENT = 0x08
};

bool AutoTalentEnabled()
{
    return sWorld.getConfig(CONFIG_BOOL_AUTO_TALENT_ON_LEVEL_UP) ||
        sWorld.getConfig(CONFIG_BOOL_AUTO_TALENT_ON_LOGIN);
}

// Bots keep their own premade spec/gear pipeline unless explicitly opted in.
bool AutoProgressionAppliesTo(Player const* player)
{
    if (!player || !player->GetSession())
        return false;
    return !player->IsBot() ||
        sWorld.getConfig(CONFIG_BOOL_AUTO_PROGRESSION_APPLY_TO_BOTS);
}

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

    auto const sourceLess = [](TrainerSource const& left,
        TrainerSource const& right)
    {
        if (left.id != right.id)
            return left.id < right.id;
        return left.isTemplate < right.isTemplate;
    };
    std::sort(cache.weapons.begin(), cache.weapons.end(), sourceLess);
    for (auto& sources : cache.classes)
        std::sort(sources.begin(), sources.end(), sourceLess);
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
    {
        if (!item || !item->item)
            continue;
        if (vendorAvailable && !item->conditionId)
            cache.itemSources[item->item] |= ITEM_SOURCE_VENDOR;
        else
        {
            cache.itemSources[item->item] |= ITEM_SOURCE_RESTRICTED;
            // Reputation/faction/event vendors: available once the character
            // meets the condition (evaluated per character, see
            // ResolveItemSources).
            if (vendorAvailable)
                cache.conditionalSources[item->item].push_back(
                    { ITEM_SOURCE_VENDOR, CONDITION_FROM_VENDOR, { item->conditionId } });
        }
    }
}

// Conditioned loot keeps its RESTRICTED marker (quest drops stay excluded) and
// additionally records the condition chains that unlock it per character.
void MarkConditionalItems(AutoProgressionCache& cache,
    LootConditionedItemMap const& conditioned, uint32 source)
{
    if (!source)
        return;
    for (auto const& entry : conditioned)
    {
        if (!entry.first)
            continue;
        cache.itemSources[entry.first] |= ITEM_SOURCE_RESTRICTED;
        for (std::vector<uint32> const& chain : entry.second)
            cache.conditionalSources[entry.first].push_back(
                { source, CONDITION_FROM_LOOT, chain });
    }
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
        LootConditionedItemMap conditioned;
        if (info->loot_id)
        {
            LootTemplates_Creature.CollectItemIds(info->loot_id, items, false, &conditioned);
            LootTemplates_Creature.CollectItemIds(info->loot_id, allItems, true);
        }
        if (info->pickpocket_loot_id)
        {
            LootTemplates_Pickpocketing.CollectItemIds(info->pickpocket_loot_id, items,
                false, &conditioned);
            LootTemplates_Pickpocketing.CollectItemIds(
                info->pickpocket_loot_id, allItems, true);
        }
        if (info->skinning_loot_id)
        {
            LootTemplates_Skinning.CollectItemIds(info->skinning_loot_id, items,
                false, &conditioned);
            LootTemplates_Skinning.CollectItemIds(
                info->skinning_loot_id, allItems, true);
        }
        MarkItems(cache.itemSources, allItems, ITEM_SOURCE_RESTRICTED);
        MarkItems(cache.itemSources, items, source);
        MarkConditionalItems(cache, conditioned, source);
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
        LootConditionedItemMap conditioned;
        LootTemplates_Gameobject.CollectItemIds(info->GetLootId(), items, false,
            &conditioned);
        LootTemplates_Gameobject.CollectItemIds(
            info->GetLootId(), allItems, true);
        auto const sourceItr = gameObjectSources.find(entry.first);
        uint32 const source = sourceItr != gameObjectSources.end() ?
            sourceItr->second : restrictedGameObjects.count(entry.first) ?
            ITEM_SOURCE_NONE : ITEM_SOURCE_OTHER;
        MarkItems(cache.itemSources, allItems, ITEM_SOURCE_RESTRICTED);
        MarkItems(cache.itemSources, items, source);
        MarkConditionalItems(cache, conditioned, source);
    }

    bool hasAvailableFishingArea = false;
    for (auto itr = sAreaStorage.begin<AreaEntry>();
         itr < sAreaStorage.end<AreaEntry>(); ++itr)
    {
        std::set<uint32> items;
        std::set<uint32> allItems;
        LootConditionedItemMap conditioned;
        LootTemplates_Fishing.CollectItemIds(itr->Id, items, false, &conditioned);
        LootTemplates_Fishing.CollectItemIds(itr->Id, allItems, true);
        MarkItems(cache.itemSources, allItems, ITEM_SOURCE_RESTRICTED);
        if (!sObjectMgr.IsMapLootDisabled(itr->MapId))
        {
            MarkItems(cache.itemSources, items, LootSourceForMap(itr->MapId));
            MarkConditionalItems(cache, conditioned, LootSourceForMap(itr->MapId));
            hasAvailableFishingArea = true;
        }
    }
    std::set<uint32> fallbackFishingItems;
    std::set<uint32> allFallbackFishingItems;
    LootConditionedItemMap fallbackFishingConditioned;
    LootTemplates_Fishing.CollectItemIds(0, fallbackFishingItems, false,
        &fallbackFishingConditioned);
    LootTemplates_Fishing.CollectItemIds(0, allFallbackFishingItems, true);
    MarkItems(cache.itemSources, allFallbackFishingItems,
        ITEM_SOURCE_RESTRICTED);
    if (hasAvailableFishingArea)
    {
        MarkItems(cache.itemSources, fallbackFishingItems,
            ITEM_SOURCE_WORLD_LOOT);
        MarkConditionalItems(cache, fallbackFishingConditioned,
            ITEM_SOURCE_WORLD_LOOT);
    }

    std::set<uint32> otherItems;
    std::set<uint32> allOtherItems;
    LootConditionedItemMap otherConditioned;
    LootTemplates_Item.CollectAllItemIds(otherItems, false, &otherConditioned);
    LootTemplates_Mail.CollectAllItemIds(otherItems, false, &otherConditioned);
    LootTemplates_Disenchant.CollectAllItemIds(otherItems, false, &otherConditioned);
    LootTemplates_Item.CollectAllItemIds(allOtherItems, true);
    LootTemplates_Mail.CollectAllItemIds(allOtherItems, true);
    LootTemplates_Disenchant.CollectAllItemIds(allOtherItems, true);
    MarkItems(cache.itemSources, allOtherItems, ITEM_SOURCE_RESTRICTED);
    MarkItems(cache.itemSources, otherItems, ITEM_SOURCE_OTHER);
    MarkConditionalItems(cache, otherConditioned, ITEM_SOURCE_OTHER);
    return availableCreatures;
}

// Reagent items referenced by the Totem fields of any spell (shaman totems).
std::set<uint32> CollectSpellTotemItems()
{
    std::set<uint32> items;
    for (uint32 spellId = 1; spellId < sSpellMgr.GetMaxSpellId(); ++spellId)
    {
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
        if (!spell)
            continue;
        for (uint32 totem : spell->Totem)
            if (totem)
                items.insert(totem);
    }
    return items;
}

// Class abilities are the spells listed under a SKILL_CATEGORY_CLASS skill
// line; profession recipes, languages and generic spells are excluded.
bool IsClassAbility(uint32 spellId)
{
    SkillLineAbilityMapBounds const bounds =
        sSpellMgr.GetSkillLineAbilityMapBoundsBySpellId(spellId);
    for (SkillLineAbilityMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
    {
        SkillLineEntry const* skill = itr->second ?
            sSkillLineStore.LookupEntry(itr->second->skillId) : nullptr;
        if (skill && skill->categoryId == SKILL_CATEGORY_CLASS)
            return true;
    }
    return false;
}

// Union of the class masks that may own a skill line (fallback when a
// SkillLineAbility entry carries no class mask of its own).
uint32 SkillLineClassMask(uint32 skillId)
{
    uint32 mask = 0;
    SkillRaceClassInfoMapBounds const bounds =
        sSpellMgr.GetSkillRaceClassInfoMapBounds(skillId);
    for (SkillRaceClassInfoMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
        if (itr->second)
            mask |= itr->second->classMask;
    return mask;
}

// Items that teach class abilities on use (AQ40 tomes, Dire Maul codices,
// Tranquilizing Shot, Polymorph variants, ...). Trainers never offer these.
void BuildSpellBooks(AutoProgressionCache& cache)
{
    for (auto const& entry : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const& item = entry.second;
        if (item.HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE))
            continue;

        SpellBook book;
        book.itemId = item.ItemId;
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
        {
            if (!item.Spells[slot].SpellId ||
                item.Spells[slot].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            SpellEntry const* useSpell = sSpellMgr.GetSpellEntry(item.Spells[slot].SpellId);
            if (!useSpell)
                continue;
            for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
            {
                uint32 const taught = useSpell->EffectTriggerSpell[effect];
                // Pet grimoires use the same effect but target the pet.
                if (useSpell->Effect[effect] != SPELL_EFFECT_LEARN_SPELL || !taught ||
                    useSpell->EffectImplicitTargetA[effect] == TARGET_UNIT_CASTER_PET ||
                    !sSpellMgr.GetSpellEntry(taught) || !IsClassAbility(taught) ||
                    std::find(book.spells.begin(), book.spells.end(), taught) != book.spells.end())
                    continue;
                book.spells.push_back(taught);
            }
        }
        if (!book.spells.empty())
            cache.spellBooks.push_back(std::move(book));
    }
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
        else if (item.Class == ITEM_CLASS_PROJECTILE &&
            item.InventoryType == INVTYPE_AMMO &&
            !item.HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE))
            cache.ammoItems.push_back(item.ItemId);
    }

    std::set<uint32> const availableCreatureEntries =
        MarkLootSources(cache);
    BuildTrainerCache(cache.trainers, availableCreatureEntries);
    cache.improvements = BuildImprovementCandidates();
    BuildSpellBooks(cache);

    for (uint32 id = 0; id < sObjectMgr.GetMaxSkillLineAbilityId(); ++id)
    {
        SkillLineAbilityEntry const* ability = sObjectMgr.GetSkillLineAbility(id);
        if (!ability)
            continue;
        SkillLineEntry const* skill = sSkillLineStore.LookupEntry(ability->skillId);
        if (!skill)
            continue;
        bool const profession = skill->categoryId == SKILL_CATEGORY_PROFESSION ||
            skill->categoryId == SKILL_CATEGORY_SECONDARY;
        bool const classSkill = skill->categoryId == SKILL_CATEGORY_CLASS;
        if (!profession && !classSkill)
            continue;
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(ability->spellId);
        if (!spell)
            continue;
        for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
        {
            if (spell->Effect[effect] != SPELL_EFFECT_CREATE_ITEM ||
                !spell->EffectItemType[effect])
                continue;
            uint32 const created = spell->EffectItemType[effect];
            if (sObjectMgr.IsSpellDisabled(ability->spellId))
                cache.itemSources[created] |= ITEM_SOURCE_RESTRICTED;
            else if (profession)
                cache.itemSources[created] |= ITEM_SOURCE_CRAFTED;
            else
            {
                // Class-conjured gear (warlock Spellstones/Firestones): only the
                // creating class can ever hold it.
                cache.itemSources[created] |= ITEM_SOURCE_CLASS_CREATED;
                cache.classCreatedItems[created] |= ability->classmask ?
                    ability->classmask : SkillLineClassMask(ability->skillId);
            }
        }
    }

    // Items produced by using another obtainable item (e.g. Sul'thraze from
    // combining the two Zul'Farrak blades) are reachable through that item.
    for (auto const& entry : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const& sourceItem = entry.second;
        if (sourceItem.HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE))
            continue;
        for (uint8 slot = 0; slot < MAX_ITEM_PROTO_SPELLS; ++slot)
        {
            if (!sourceItem.Spells[slot].SpellId ||
                sourceItem.Spells[slot].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                continue;
            SpellEntry const* useSpell = sSpellMgr.GetSpellEntry(sourceItem.Spells[slot].SpellId);
            if (!useSpell || sObjectMgr.IsSpellDisabled(useSpell->Id))
                continue;
            for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
                if (useSpell->Effect[effect] == SPELL_EFFECT_CREATE_ITEM &&
                    useSpell->EffectItemType[effect] &&
                    useSpell->EffectItemType[effect] != sourceItem.ItemId)
                    cache.itemSources[useSpell->EffectItemType[effect]] |= ITEM_SOURCE_OTHER;
        }
    }

    std::set<uint32> const totemItems = CollectSpellTotemItems();
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

        // Class quests are the only source for a number of core abilities
        // (stances, pets, poisons, forms, totems, priest racials, ...).
        if (quest->GetRequiredClasses())
        {
            ClassQuestGrant grant;
            grant.questId = entry.first;
            grant.minLevel = quest->GetMinLevel();
            grant.requiredClasses = quest->GetRequiredClasses();
            grant.requiredRaces = quest->GetRequiredRaces();
            uint32 const rewardSpells[] =
                { quest->GetRewSpellCast(), quest->GetRewSpell() };
            for (uint32 rewardSpellId : rewardSpells)
            {
                SpellEntry const* rewardSpell = rewardSpellId ?
                    sSpellMgr.GetSpellEntry(rewardSpellId) : nullptr;
                if (!rewardSpell)
                    continue;
                for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
                    if (rewardSpell->Effect[effect] == SPELL_EFFECT_LEARN_SPELL &&
                        rewardSpell->EffectTriggerSpell[effect] &&
                        rewardSpell->EffectImplicitTargetA[effect] != TARGET_UNIT_CASTER_PET &&
                        std::find(grant.spells.begin(), grant.spells.end(),
                            rewardSpell->EffectTriggerSpell[effect]) == grant.spells.end())
                        grant.spells.push_back(rewardSpell->EffectTriggerSpell[effect]);
            }
            for (uint8 index = 0; index < QUEST_REWARDS_COUNT; ++index)
            {
                uint32 const itemId = quest->RewItemId[index];
                ItemPrototype const* reward = itemId ?
                    sObjectMgr.GetItemPrototype(itemId) : nullptr;
                if (reward && reward->Class == ITEM_CLASS_REAGENT &&
                    totemItems.count(itemId))
                    grant.items.push_back(itemId);
            }
            if (!grant.spells.empty() || !grant.items.empty())
                cache.classQuestGrants.push_back(std::move(grant));
        }
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
            "PlayerAutoProgression[debug]: cache rebuilt in %u ms; equipment=%u, ammo=%u, sourceMappings=%u (restricted=%u), questRewardItems=%u (links=%u), classQuestGrants=%u, spellBooks=%u, trainerSources=%u, improvements=%u.",
            WorldTimer::getMSTimeDiffToNow(debugStarted),
            uint32(cache.equipmentItems.size()), uint32(cache.ammoItems.size()),
            uint32(cache.itemSources.size()),
            restrictedMappings, uint32(cache.rewardQuests.size()), rewardLinks,
            uint32(cache.classQuestGrants.size()), uint32(cache.spellBooks.size()),
            trainerSources, uint32(cache.improvements.size()));
    }
#endif
}

std::shared_ptr<AutoProgressionCache const> EnsureAutoProgressionCache()
{
    AutoProgressionCacheState& state = GetAutoProgressionCacheState();
    std::unique_lock<std::mutex> lock(state.mutex);
    // The build runs while holding the mutex, which also keeps updates out.
    // An update running on this very thread must not be waited for.
    std::thread::id const current = std::this_thread::get_id();
    state.condition.wait(lock, [&state, current]()
    {
        return state.activeUpdates == 0 || state.updateThread == current;
    });
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

// Evaluates a loot/vendor condition for one character without a map or loot
// source object: static conditions (patch, game events) and character checks
// (team, race/class, level, skills, quests, reputation, items, auras) are
// answered by the condition system itself; anything that needs the dropping
// creature, a map or instance state stays unavailable.
bool LootConditionHolds(Player const* player, uint32 conditionId,
    ConditionSource context, uint8 depth = 0)
{
    ConditionEntry const* condition = sConditionStorage.LookupEntry<ConditionEntry>(conditionId);
    if (!condition || depth > 8)
        return false;

    bool result = false;
    switch (condition->GetType())
    {
        case CONDITION_AND:
        case CONDITION_OR:
        {
            bool const isAnd = condition->GetType() == CONDITION_AND;
            result = isAnd;
            for (uint8 index = 0; index < 4; ++index)
            {
                uint32 const child = uint32(condition->GetValue(index));
                if (!child)
                    continue;
                bool const holds = LootConditionHolds(player, child, context, depth + 1);
                if (isAnd && !holds) { result = false; break; }
                if (!isAnd && holds) { result = true; break; }
            }
            break;
        }
        case CONDITION_NOT:
            result = !LootConditionHolds(player, uint32(condition->GetValue(0)),
                context, depth + 1);
            break;
        case CONDITION_NONE:
        case CONDITION_WOW_PATCH:
        case CONDITION_ACTIVE_GAME_EVENT:
        case CONDITION_ACTIVE_HOLIDAY:
        case CONDITION_SAVED_VARIABLE:
        case CONDITION_LOCAL_TIME:
        case CONDITION_TEAM:
        case CONDITION_RACE_CLASS:
        case CONDITION_LEVEL:
        case CONDITION_GENDER:
        case CONDITION_IS_PLAYER:
        case CONDITION_SKILL:
        case CONDITION_SKILL_BELOW:
        case CONDITION_SPELL:
        case CONDITION_AURA:
        case CONDITION_AD_COMMISSION_AURA:
        case CONDITION_ITEM:
        case CONDITION_ITEM_WITH_BANK:
        case CONDITION_ITEM_EQUIPPED:
        case CONDITION_QUESTREWARDED:
        case CONDITION_QUESTTAKEN:
        case CONDITION_QUEST_NONE:
        case CONDITION_QUESTAVAILABLE:
        case CONDITION_REPUTATION_RANK_MIN:
        case CONDITION_REPUTATION_RANK_MAX:
        case CONDITION_PVP_RANK:
        case CONDITION_AREA_EXPLORED:
            // Meets() applies the reverse-result flag itself.
            return condition->Meets(player, nullptr, nullptr, context);
        default:
            return false;
    }
    return condition->IsReversed() ? !result : result;
}

// Resolves the cached source mask of an item for one character. Conditioned
// loot/vendor sources count when their conditions hold for this character,
// class-conjured items (warlock Spellstones/Firestones) are available only to
// the creating class, and bind-on-pickup items without any known source
// (TCG/promo gear, deprecated DB rows without the NOT_OBTAINABLE flag) can
// never be obtained. Returns false when the item is unobtainable.
bool ResolveItemSources(Player const* player, ItemPrototype const* item,
    AutoProgressionCache const& cache, uint32& sourceMask, bool& ownClassCreated)
{
    sourceMask = 0;
    ownClassCreated = false;
    if (!item)
        return false;
    auto const sourceItr = cache.itemSources.find(item->ItemId);
    if (sourceItr != cache.itemSources.end())
        sourceMask = sourceItr->second;
    auto const conditionalItr = cache.conditionalSources.find(item->ItemId);
    if (conditionalItr != cache.conditionalSources.end())
    {
        for (ConditionalItemSource const& conditional : conditionalItr->second)
        {
            if (sourceMask & conditional.source)
                continue; // already granted unconditionally
            bool holds = true;
            for (uint32 conditionId : conditional.conditions)
                if (!LootConditionHolds(player, conditionId, conditional.context))
                {
                    holds = false;
                    break;
                }
            if (holds)
                sourceMask |= conditional.source;
        }
    }
    if (sourceMask & ITEM_SOURCE_CLASS_CREATED)
    {
        auto const creator = cache.classCreatedItems.find(item->ItemId);
        ownClassCreated = creator != cache.classCreatedItems.end() &&
            (creator->second & player->GetClassMask()) != 0;
        sourceMask &= ~uint32(ITEM_SOURCE_CLASS_CREATED);
    }
    if (ownClassCreated)
        return true;
    if (!sourceMask && (item->Bonding == BIND_WHEN_PICKED_UP ||
        item->Bonding == BIND_QUEST_ITEM || item->Bonding == BIND_QUEST_ITEM1))
        return false;
    return true;
}

// Teaches the spells behind a trainer entry directly, exactly like the GM
// ".learn all_trainer" helper does. Casting the trainer's learn spell would
// hand ownership of the Spell object to a SpellEvent on the player, so it
// must never be deleted by the caller; learning directly avoids that trap
// and the per-spell Spell/event overhead entirely.
bool LearnTrainerSpell(Player* player, TrainerSpell const& trainer)
{
    SpellEntry const* info = sSpellMgr.GetSpellEntry(trainer.spell);
    if (!info)
        return false;

    bool learned = false;
    for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
    {
        if (info->Effect[effect] != SPELL_EFFECT_LEARN_SPELL ||
            info->EffectImplicitTargetA[effect] == TARGET_UNIT_CASTER_PET)
            continue;
        uint32 const spellId = info->EffectTriggerSpell[effect];
        if (!spellId || player->HasSpell(spellId) ||
            !sSpellMgr.GetSpellEntry(spellId) ||
            !player->IsSpellFitByClassAndRace(spellId))
            continue;
        player->LearnSpell(spellId, false);
        learned = true;
    }
    return learned;
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
            if (attempted.count(trainer.spell) ||
                player->GetTrainerSpellState(&trainer) != TRAINER_SPELL_GREEN)
                continue;
            attempted.insert(trainer.spell);
            if (LearnTrainerSpell(player, trainer))
                ++learned;
        }
    }
    return learned;
}

bool ClassQuestGrantApplies(Player const* player, ClassQuestGrant const& grant)
{
    if (!(grant.requiredClasses & player->GetClassMask()))
        return false;
    if (grant.requiredRaces && !(grant.requiredRaces & player->GetRaceMask()))
        return false;
    if (grant.minLevel > player->GetLevel())
        return false;
    Quest const* quest = sObjectMgr.GetQuestTemplate(grant.questId);
    return quest && quest->IsActive() &&
        sObjectMgr.IsQuestTemplateLoaded(grant.questId);
}

// Spells that only class quests teach (stances, pets, poisons, forms, totem
// ranks, priest racials, ...). The quest itself stays available; finishing it
// later simply re-teaches already known spells.
uint32 LearnClassQuestSpells(Player* player, AutoProgressionCache const& cache)
{
    if (!sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_CLASS_QUEST_SPELLS))
        return 0;

    uint32 learned = 0;
    for (ClassQuestGrant const& grant : cache.classQuestGrants)
    {
        if (grant.spells.empty() || !ClassQuestGrantApplies(player, grant))
            continue;
        for (uint32 spellId : grant.spells)
        {
            if (player->HasSpell(spellId) || !sSpellMgr.GetSpellEntry(spellId) ||
                !player->IsSpellFitByClassAndRace(spellId) ||
                sObjectMgr.IsSpellDisabled(spellId))
                continue;
            player->LearnSpell(spellId, false);
            ++learned;
        }
    }
    return learned;
}

// Class reagents handed out by class quests (the four shaman totems). The
// client refuses every totem cast without the matching item in the bags.
uint32 GrantClassQuestItems(Player* player, AutoProgressionCache const& cache)
{
    if (!sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_CLASS_QUEST_ITEMS))
        return 0;

    uint32 granted = 0;
    for (ClassQuestGrant const& grant : cache.classQuestGrants)
    {
        if (grant.items.empty() || !ClassQuestGrantApplies(player, grant))
            continue;
        for (uint32 itemId : grant.items)
        {
            ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
            if (!proto || player->HasItemCount(itemId, 1, true) ||
                player->CanUseItem(proto, false) != EQUIP_ERR_OK)
                continue;
            ItemPosCountVec destination;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, destination, itemId, 1) != EQUIP_ERR_OK)
                continue;
            if (player->StoreNewItem(destination, itemId, true))
                ++granted;
        }
    }
    return granted;
}

bool SpellBookAvailable(Player* player, ItemPrototype const* book,
    AutoProgressionCache const& cache)
{
    if (!book || book->RequiredHonorRank || book->RequiredCityRank)
        return false;
    if (player->CanUseItem(book, false) != EQUIP_ERR_OK)
        return false;
    if (book->RequiredReputationFaction &&
        uint32(player->GetReputationRank(book->RequiredReputationFaction)) <
            book->RequiredReputationRank)
        return false;

    // Books follow the same acquisition switches as equipment.
    uint32 sourceMask = 0;
    bool ownClassCreated = false;
    if (!ResolveItemSources(player, book, cache, sourceMask, ownClassCreated))
        return false;
    return ownClassCreated || EnabledItemSources(sourceMask, true) != 0;
}

// Max-rank tomes/codices/grimoires (AQ40, Dire Maul, Molten Core, ...) and
// other item-taught class abilities that no trainer offers.
uint32 LearnSpellBooks(Player* player, AutoProgressionCache const& cache)
{
    if (!sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_SPELL_BOOKS))
        return 0;

    uint32 learned = 0;
    for (SpellBook const& book : cache.spellBooks)
    {
        bool needed = false;
        for (uint32 spellId : book.spells)
            if (!player->HasSpell(spellId))
                needed = true;
        if (!needed || !SpellBookAvailable(player,
                sObjectMgr.GetItemPrototype(book.itemId), cache))
            continue;
        for (uint32 spellId : book.spells)
        {
            if (player->HasSpell(spellId) ||
                !player->IsSpellFitByClassAndRace(spellId) ||
                sObjectMgr.IsSpellDisabled(spellId))
                continue;
            // Higher ranks still require the previous rank, like a trainer.
            if (SpellChainNode const* chain = sSpellMgr.GetSpellChainNode(spellId))
                if ((chain->prev && !player->HasSpell(chain->prev)) ||
                    (chain->req && !player->HasSpell(chain->req)))
                    continue;
            player->LearnSpell(spellId, false);
            ++learned;
        }
    }
    return learned;
}

// Level-bound class skills: weapons, Defense and the rogue-only Poisons and
// Lockpicking lines (poison recipes at trainers require Poisons skill values
// up to 280).
void MaxCombatSkills(Player* player)
{
    uint16 const maximum = uint16(std::min<uint32>(player->GetLevel() * 5, 300));
    uint16 const skills[] =
    {
        SKILL_SWORDS, SKILL_AXES, SKILL_BOWS, SKILL_GUNS, SKILL_MACES, SKILL_2H_SWORDS,
        SKILL_STAVES, SKILL_2H_MACES, SKILL_UNARMED, SKILL_2H_AXES, SKILL_DAGGERS,
        SKILL_THROWN, SKILL_CROSSBOWS, SKILL_WANDS, SKILL_POLEARMS, SKILL_FIST_WEAPONS,
        SKILL_DEFENSE, SKILL_POISONS, SKILL_LOCKPICKING
    };
    for (uint16 skill : skills)
        if (player->GetSkillValue(skill))
            player->SetSkill(skill, maximum, maximum);
}

struct SpellHitProfile
{
    uint32 spellId = 0;
    float share = 0;
    float talentBonus = 0;
};

struct WeaponSkillTalentProfile
{
    uint32 skillId = 0;
    float bonus = 0;
};

struct Weights
{
    float str = 0, agi = 0, sta = 0.25f, intl = 0, spi = 0;
    float armor = 0, blockValue = 0, weaponDps = 0, rangedDps = 0;
    float ap = 0, rap = 0, spell = 0, healing = 0, mp5 = 0;
    float meleeHit = 0, spellHit = 0, meleeCrit = 0, spellCrit = 0;
    float dodge = 0, parry = 0, block = 0, defense = 0, weaponSkill = 0;
    uint32 spellSchools = 0;
    float shadowScale = 1.0f, fireScale = 1.0f;
    bool tank = false, caster = false, healer = false;
    bool twoHand = false, dualWield = false, shield = false;
    std::array<SpellHitProfile, 3> spellHitProfiles = {};
    uint8 spellHitProfileCount = 0;
    std::array<WeaponSkillTalentProfile, 4> weaponSkillTalentProfiles = {};
    uint8 weaponSkillTalentProfileCount = 0;
};

// Offhand weapons contribute less than the mainhand; two-hander specs (Arms,
// Retribution, Enhancement) lose most of their special-attack value with a
// one-hand pair, so their offhand weapon counts even less.
float OffhandWeaponScale(Weights const& w)
{
    return w.twoHand ? 0.35f : 0.70f;
}

// How often a weapon's on-hit procs actually fire: melee weapon procs only
// matter for specs that swing (a hunter's stat sticks almost never do),
// ranged weapon procs only for specs that shoot.
float WeaponProcActivity(Weights const& w, ItemPrototype const* item)
{
    if (item && item->IsRangedWeapon())
        return std::min(1.0f, std::max(0.0f, w.rangedDps / 16.0f));
    return std::min(1.0f, std::max(0.0f, w.weaponDps / 13.0f));
}

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

uint32 HighestKnownSpellRank(Player const* player, uint32 baseSpellId)
{
    uint32 const first = sSpellMgr.GetFirstSpellInChain(baseSpellId);
    uint32 bestSpellId = 0;
    uint8 bestRank = 0;
    for (auto const& known : player->GetSpellMap())
    {
        if (known.second.state == PLAYERSPELL_REMOVED || known.second.disabled ||
            sSpellMgr.GetFirstSpellInChain(known.first) != first)
            continue;
        uint8 const rank = sSpellMgr.GetSpellRank(known.first);
        if (!bestSpellId || rank >= bestRank)
        {
            bestSpellId = known.first;
            bestRank = rank;
        }
    }
    return bestSpellId;
}

void AddSpellHitProfile(Player const* player, Weights& weights,
    uint32 spellId, float share)
{
    if (weights.spellHitProfileCount >= weights.spellHitProfiles.size())
        return;
    uint32 const knownSpellId = HighestKnownSpellRank(player, spellId);
    if (!knownSpellId)
        return;
    SpellHitProfile& profile =
        weights.spellHitProfiles[weights.spellHitProfileCount++];
    profile.spellId = knownSpellId;
    profile.share = share;
}

bool IsWeaponSkill(uint32 skill);

void PopulateTalentSpellHit(Player const* player, Weights& weights)
{
    for (uint32 talentId = 0; talentId < sTalentStore.GetNumRows(); ++talentId)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(talentId);
        if (!talent)
            continue;
        TalentTabEntry const* tab =
            sTalentTabStore.LookupEntry(talent->TalentTab);
        if (!tab || !(tab->ClassMask & player->GetClassMask()))
            continue;

        SpellEntry const* talentSpell = nullptr;
        for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        {
            uint32 const rankSpellId = talent->RankID[rank];
            if (rankSpellId && player->HasSpell(rankSpellId))
            {
                talentSpell = sSpellMgr.GetSpellEntry(rankSpellId);
                break;
            }
        }
        if (!talentSpell)
            continue;

        for (uint8 effect = 0; effect < MAX_EFFECT_INDEX; ++effect)
        {
            if (talentSpell->Effect[effect] != SPELL_EFFECT_APPLY_AURA)
                continue;
            float const amount = std::max(0.0f, float(
                talentSpell->CalculateSimpleValue(SpellEffectIndex(effect))));
            if (amount <= 0)
                continue;

            AuraType const aura =
                AuraType(talentSpell->EffectApplyAuraName[effect]);
            if (aura == SPELL_AURA_MOD_SKILL &&
                IsWeaponSkill(uint32(talentSpell->EffectMiscValue[effect])))
            {
                uint32 const skill =
                    uint32(talentSpell->EffectMiscValue[effect]);
                uint8 index = 0;
                while (index < weights.weaponSkillTalentProfileCount &&
                    weights.weaponSkillTalentProfiles[index].skillId != skill)
                    ++index;
                if (index == weights.weaponSkillTalentProfileCount &&
                    index < weights.weaponSkillTalentProfiles.size())
                {
                    weights.weaponSkillTalentProfiles[index].skillId = skill;
                    ++weights.weaponSkillTalentProfileCount;
                }
                if (index < weights.weaponSkillTalentProfiles.size())
                    weights.weaponSkillTalentProfiles[index].bonus += amount;
                continue;
            }
            if (aura == SPELL_AURA_MOD_SPELL_HIT_CHANCE)
            {
                for (uint8 i = 0; i < weights.spellHitProfileCount; ++i)
                    weights.spellHitProfiles[i].talentBonus += amount;
                continue;
            }
            if (aura != SPELL_AURA_ADD_FLAT_MODIFIER ||
                talentSpell->EffectMiscValue[effect] !=
                    SPELLMOD_RESIST_MISS_CHANCE)
                continue;

            uint64 const familyMask = sSpellMgr.GetSpellAffectMask(
                talentSpell->Id, SpellEffectIndex(effect));
            for (uint8 i = 0; i < weights.spellHitProfileCount; ++i)
            {
                SpellEntry const* primary = sSpellMgr.GetSpellEntry(
                    weights.spellHitProfiles[i].spellId);
                if (primary && primary->SpellFamilyName ==
                        talentSpell->SpellFamilyName &&
                    primary->IsFitToFamilyMask(familyMask))
                    weights.spellHitProfiles[i].talentBonus += amount;
            }
        }
    }

    float totalShare = 0;
    for (uint8 i = 0; i < weights.spellHitProfileCount; ++i)
        totalShare += weights.spellHitProfiles[i].share;
    if (totalShare > 0)
        for (uint8 i = 0; i < weights.spellHitProfileCount; ++i)
            weights.spellHitProfiles[i].share /= totalShare;
}

uint32 TalentTreeIndex(TalentTabEntry const* tab)
{
    if (!tab)
        return 3;
    // The 1.12.1 TalentTab.dbc shipped with this core labels both Mage
    // Arcane (81) and Fire (41) as page zero. Fire is the missing page one.
    if (tab->TalentTabID == 41)
        return 1;
    return tab->tabpage;
}

std::array<uint32, 3> TalentTreePoints(Player const* player)
{
    std::array<uint32, 3> points = {};
    for (uint32 id = 0; id < sTalentStore.GetNumRows(); ++id)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(id);
        if (!talent)
            continue;
        TalentTabEntry const* tab = sTalentTabStore.LookupEntry(
            talent->TalentTab);
        uint32 const tree = TalentTreeIndex(tab);
        if (!tab || tree >= points.size() ||
            !(tab->ClassMask & player->GetClassMask()))
            continue;
        for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            if (talent->RankID[rank] && player->HasSpell(
                    talent->RankID[rank]))
            {
                points[tree] += uint32(rank + 1);
                break;
            }
    }
    return points;
}

uint32 AutoProgressionTalentTree(Player* player)
{
    if (player->GetLevel() >= 10)
    {
        std::array<uint32, 3> const trees = TalentTreePoints(player);
        uint32 const total = trees[0] + trees[1] + trees[2];
        if (total > 0)
            return uint32(std::distance(trees.begin(),
                std::max_element(trees.begin(), trees.end())));
    }

    switch (player->GetClass())
    {
        case CLASS_SHAMAN: return 2;
        case CLASS_PRIEST: return 1;
        case CLASS_DRUID: return 2;
        case CLASS_WARRIOR: return 2;
        default: return 0;
    }
}

Weights GetWeights(Player* player)
{
    uint32 const tree = AutoProgressionTalentTree(player);
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
            // "+N Attack Power" items carry a melee and a ranged aura with the
            // same value; the ranged half is what matters and is scored via
            // rap, so the melee half must not double the item's value.
            w.ap = 0.1f;
            break;
        case CLASS_ROGUE:
            w = AgilityMelee(); w.agi = 2.5f;
            break;
        case CLASS_PRIEST:
            w = tree == 2 ? Caster() : Healer();
            if (tree == 0) w.spi = 1.0f;
            if (tree == 2)
            {
                w.spellSchools = SPELL_SCHOOL_MASK_SHADOW;
                AddSpellHitProfile(player, w, 15407, 0.45f); // Mind Flay
                AddSpellHitProfile(player, w, 8092, 0.30f);  // Mind Blast
                AddSpellHitProfile(player, w, 589, 0.25f);   // Shadow Word: Pain
            }
            else
                w.spellSchools = SPELL_SCHOOL_MASK_HOLY;
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
            if (tree == 0)
            {
                AddSpellHitProfile(player, w, 403, 0.55f);  // Lightning Bolt
                AddSpellHitProfile(player, w, 421, 0.25f);  // Chain Lightning
                AddSpellHitProfile(player, w, 8042, 0.20f); // Earth Shock
            }
            break;
        case CLASS_MAGE:
            w = Caster(); if (tree == 1) w.spellCrit = 10;
            w.spellSchools = tree == 0 ? SPELL_SCHOOL_MASK_ARCANE :
                tree == 1 ? SPELL_SCHOOL_MASK_FIRE : SPELL_SCHOOL_MASK_FROST;
            if (tree == 0)
            {
                AddSpellHitProfile(player, w, 5143, 0.65f); // Arcane Missiles
                AddSpellHitProfile(player, w, 1449, 0.35f); // Arcane Explosion
            }
            else if (tree == 1)
            {
                AddSpellHitProfile(player, w, 133, 0.65f);  // Fireball
                AddSpellHitProfile(player, w, 2948, 0.20f); // Scorch
                AddSpellHitProfile(player, w, 2136, 0.15f); // Fire Blast
            }
            else
            {
                AddSpellHitProfile(player, w, 116, 0.65f); // Frostbolt
                AddSpellHitProfile(player, w, 10, 0.20f);  // Blizzard
                AddSpellHitProfile(player, w, 120, 0.15f); // Cone of Cold
            }
            break;
        case CLASS_WARLOCK:
            w = Caster(); w.sta = 0.8f; w.spi = 0.25f;
            // Destruction is not synonymous with Fire: Shadow Bolt is a
            // Destruction spell and remains the default raid filler.
            w.spellCrit = tree == 2 ? 9.0f : 6.5f;
            w.spellSchools = SPELL_SCHOOL_MASK_FIRE | SPELL_SCHOOL_MASK_SHADOW;
            w.fireScale = tree == 2 ? 0.75f : 0.35f;
            if (player->GetRace() == RACE_GNOME)
                w.intl *= 1.05f; // Expansive Mind also scales Intellect from gear.
            AddSpellHitProfile(player, w, 686, tree == 0 ? 0.45f :
                tree == 1 ? 0.60f : 0.55f); // Shadow Bolt always matters.
            if (tree == 0)
            {
                AddSpellHitProfile(player, w, 172, 0.30f); // Corruption
                AddSpellHitProfile(player, w, 980, 0.25f); // Curse of Agony
            }
            else if (tree == 1)
            {
                AddSpellHitProfile(player, w, 172, 0.25f); // Corruption
                AddSpellHitProfile(player, w, 348, 0.15f); // Immolate
            }
            else
            {
                AddSpellHitProfile(player, w, 348, 0.25f);  // Immolate
                AddSpellHitProfile(player, w, 5676, 0.20f); // Searing Pain
            }
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
            if (tree == 0)
            {
                AddSpellHitProfile(player, w, 5176, 0.45f); // Wrath
                AddSpellHitProfile(player, w, 2912, 0.40f); // Starfire
                AddSpellHitProfile(player, w, 8921, 0.15f); // Moonfire
            }
            break;
        default: break;
    }
    PopulateTalentSpellHit(player, w);
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
    uint32 const relevant = magic & w.spellSchools;
    if (!relevant)
        return 0.0f;
    float scale = 0;
    if (relevant & SPELL_SCHOOL_MASK_SHADOW)
        scale = std::max(scale, w.shadowScale);
    if (relevant & SPELL_SCHOOL_MASK_FIRE)
        scale = std::max(scale, w.fireScale);
    uint32 const otherMagic = relevant &
        ~(SPELL_SCHOOL_MASK_SHADOW | SPELL_SCHOOL_MASK_FIRE);
    if (otherMagic)
        scale = 1.0f;
    return scale;
}

float PhysicalOffenseScale(Weights const& w)
{
    return std::max(std::max(w.ap, w.rap),
        std::max(w.weaponDps / 13.0f, w.rangedDps / 16.0f));
}

float ScoreTargetResistanceReduction(Weights const& w, float value,
    uint32 schoolMask)
{
    if (value <= 0)
        return 0;

    float score = 0;
    if (schoolMask & SPELL_SCHOOL_MASK_NORMAL)
        score += std::min(value, 3000.0f) * PhysicalOffenseScale(w) *
            0.04f * SecondaryScale();
    if ((schoolMask & SPELL_SCHOOL_MASK_MAGIC) && w.caster)
        score += std::min(value, 300.0f) * w.spell * 0.45f *
            SpellSchoolScale(w, schoolMask) * SecondaryScale();
    return score;
}

bool IsScoredAuraEffect(uint32 effect)
{
    // Relevant Vanilla item and set area auras (notably Atiesh) use PARTY.
    return effect == SPELL_EFFECT_APPLY_AURA ||
        effect == SPELL_EFFECT_APPLY_AREA_AURA_PARTY;
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

// consumableAbsorb: the caller (proc/use scoring) values absorb shields by
// absorbed damage per second itself; a shield is used up by damage, so its
// aura uptime says nothing about how much it prevents.
float ScoreAura(Weights const& w, SpellEntry const* spell, float trigger,
    uint8 depth = 0, bool consumableAbsorb = false)
{
    if (!spell || trigger <= 0)
        return 0;

    float score = 0;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (!IsScoredAuraEffect(spell->Effect[i]))
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
            case SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK:
                if (spell->Id != 18803 && positive && rawValue > 0 && w.caster)
                {
                    // Bound malformed data and any percent-style sentinels.
                    float const castSpeed = std::min(value, 100.0f);
                    score += castSpeed * (w.spell * 4.0f +
                        w.spellCrit * 0.35f) * SecondaryScale();
                }
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
                        depth + 1, consumableAbsorb);
                continue;
            case SPELL_AURA_MOD_DAMAGE_DONE:
                if (!positive && rawValue < 0)
                {
                    score += (hostileTarget ? 1.0f : -1.0f) * value *
                        (0.12f + w.sta * 0.05f) * SurvivalScale();
                    continue;
                }
                break;
            case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
                if (positive && rawValue > 0)
                {
                    uint32 const schoolMask = uint32(spell->EffectMiscValue[i]);
                    float weighted = 0;
                    if (schoolMask & SPELL_SCHOOL_MASK_MAGIC)
                        weighted += w.spell * 5.0f * SpellSchoolScale(w, schoolMask);
                    if (schoolMask & SPELL_SCHOOL_MASK_NORMAL)
                        weighted += std::max(w.weaponDps, w.rangedDps) * 0.4f;
                    score += value * weighted * SecondaryScale();
                }
                continue;
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
            case SPELL_AURA_MOD_MELEE_ATTACK_POWER_VERSUS:
                score += scoredValue * w.ap * 0.20f * PrimaryScale(); break;
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER_VERSUS:
                score += scoredValue * w.rap * 0.20f * PrimaryScale(); break;
            case SPELL_AURA_MOD_DAMAGE_DONE_VERSUS:
                score += scoredValue *
                    (w.spell * 5.0f + std::max(w.weaponDps, w.rangedDps) * 0.4f) *
                    0.20f * SecondaryScale();
                break;
            case SPELL_AURA_MOD_CRIT_PERCENT_VERSUS:
                score += scoredValue *
                    std::max(w.spellCrit, w.meleeCrit) * 0.20f *
                    SecondaryScale();
                break;
            case SPELL_AURA_MOD_FLAT_SPELL_DAMAGE_VERSUS:
                score += scoredValue * w.spell * 0.20f * SpellScale(); break;
            case SPELL_AURA_MOD_HEALTH_REGEN_IN_COMBAT:
                score += scoredValue * (0.05f + w.sta * 0.02f) *
                    SurvivalScale();
                break;
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
                if (hostileTarget && rawValue < 0)
                    score += ScoreTargetResistanceReduction(w, value,
                        uint32(spell->EffectMiscValue[i]));
                else
                    score += ScoreResistance(w, scoredValue,
                        uint32(spell->EffectMiscValue[i]));
                break;
            case SPELL_AURA_MOD_TARGET_RESISTANCE:
                if (rawValue < 0)
                    score += ScoreTargetResistanceReduction(w, value,
                        uint32(spell->EffectMiscValue[i]));
                break;
            case SPELL_AURA_MOD_POWER_REGEN:
            case SPELL_AURA_MOD_MANA_REGEN_INTERRUPT:
                score += scoredValue * w.mp5 * SecondaryScale(); break;
            case SPELL_AURA_MOD_POWER_REGEN_PERCENT:
                if (spell->EffectMiscValue[i] == POWER_MANA)
                    score += scoredValue * w.mp5 * 0.8f * SecondaryScale();
                break;
            case SPELL_AURA_MOD_INCREASE_HEALTH:
                score += ScoreItemMod(w, ITEM_MOD_HEALTH, scoredValue); break;
            case SPELL_AURA_MOD_INCREASE_ENERGY:
                if (spell->EffectMiscValue[i] == POWER_MANA)
                    score += ScoreItemMod(w, ITEM_MOD_MANA, scoredValue);
                break;
            case SPELL_AURA_SCHOOL_ABSORB:
                if (consumableAbsorb)
                    break; // valued per absorbed damage by the proc/use caller
                score += scoredValue * (0.30f + w.sta * 0.08f) * SurvivalScale(); break;
            case SPELL_AURA_MOD_SKILL:
            case SPELL_AURA_MOD_SKILL_TALENT:
                if (spell->EffectMiscValue[i] == SKILL_DEFENSE)
                    score += scoredValue * w.defense * SurvivalScale();
                else if (IsWeaponSkill(uint32(spell->EffectMiscValue[i])) &&
                    hostileTarget && rawValue < 0)
                    score += value * (0.08f + w.sta * 0.02f) *
                        SurvivalScale();
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
            case SPELL_AURA_MOD_SPELL_DAMAGE_OF_STAT_PERCENT:
                // Spirit is implicit in 1.12; miscValue is the school mask.
                score += scoredValue * w.spi * w.spell * 0.6f *
                    SpellSchoolScale(w,
                        uint32(spell->EffectMiscValue[i])) *
                    SpellScale();
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

float ProcUptime(SpellEntry const* spell, float ppm)
{
    if (!spell || ppm <= 0)
        return 0;
    int32 const duration = spell->GetDuration();
    return duration > 0 ?
        1.0f - std::exp(-ppm * float(duration) / 60000.0f) :
        std::min(1.0f, ppm / 60.0f);
}

float ProcAuraExposure(SpellEntry const* spell, float ppm)
{
    float const uptime = ProcUptime(spell, ppm);
    uint32 const stackLimit = std::min<uint32>(
        spell && spell->StackAmount ? spell->StackAmount : 1, 10);
    float exposure = 0;
    float stackChance = uptime;
    for (uint32 stack = 0; stack < stackLimit; ++stack)
    {
        exposure += stackChance;
        stackChance *= uptime;
    }
    return exposure;
}

float ScoreProcPayload(Weights const& w, ItemPrototype const* item, SpellEntry const* spell, float ppm, uint8 depth)
{
    if (!spell || ppm <= 0 || depth > 2)
        return 0;

    float const uptime = ProcUptime(spell, ppm);
    float score = ScoreAura(w, spell, ProcAuraExposure(spell, ppm), 0, true);
    if (spell->Id == 18803 && w.caster)
    {
        constexpr float savedCastSeconds = 1.0f;
        float const equivalentHaste = std::min(100.0f,
            100.0f * ppm * savedCastSeconds / 60.0f);
        score += equivalentHaste * (w.spell * 4.0f +
            w.spellCrit * 0.35f) * SecondaryScale();
    }

    float throughput = item && item->IsRangedWeapon() ? w.rangedDps : w.weaponDps;
    if (throughput <= 0)
        throughput = std::max(0.10f, w.spell * 0.35f);

    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        float const rawValue = float(
            spell->CalculateSimpleValue(SpellEffectIndex(i)));
        float const value = std::fabs(rawValue);
        float const perSecond = value * ppm / 60.0f;
        bool const hostileTarget = EffectTargetsHostile(spell, i);

        if (IsScoredAuraEffect(spell->Effect[i]) &&
            spell->EffectApplyAuraName[i] == SPELL_AURA_PERIODIC_DAMAGE)
        {
            uint32 const period = spell->EffectAmplitude[i] ?
                spell->EffectAmplitude[i] : 5000;
            if (hostileTarget && rawValue > 0 &&
                spell->GetDuration() >= int32(period))
                score += value * 1000.0f / float(period) * uptime *
                    throughput * WeaponScale();
            continue;
        }
        if (IsScoredAuraEffect(spell->Effect[i]) &&
            spell->EffectApplyAuraName[i] == SPELL_AURA_SCHOOL_ABSORB)
        {
            // A shield is consumed by incoming damage: a 500-point absorb once
            // per 30 minutes prevents 500 damage per 30 minutes, no matter how
            // long the aura would linger. Value it like the healing half of a
            // leech proc instead of by aura uptime.
            if (!hostileTarget && rawValue > 0)
                score += perSecond * (0.5f + w.sta * 0.1f) * SurvivalScale();
            continue;
        }

        switch (spell->Effect[i])
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            {
                // A fixed proc-DPS point is less valuable than permanent weapon
                // DPS, which also scales every white and special attack. Keeping
                // the distinction generic prevents fast flat-damage procs from
                // crowding out stat/proc enchants such as Crusader or Agility.
                float const fixedProcScale = item &&
                    item->Class == ITEM_CLASS_WEAPON ? 0.68f : 1.0f;
                score += (hostileTarget ? 1.0f : -1.0f) * perSecond *
                    throughput * WeaponScale() * fixedProcScale;
                break;
            }
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
            case SPELL_EFFECT_ADD_EXTRA_ATTACKS:
            {
                if (hostileTarget || rawValue <= 0 || !item ||
                    item->Class != ITEM_CLASS_WEAPON ||
                    item->IsRangedWeapon() || !item->Delay ||
                    w.weaponDps <= 0)
                    break;
                float const averageWeaponDamage = WeaponDps(item) *
                    float(item->Delay) / 1000.0f;
                float const extraAttacks = std::min(rawValue, 4.0f);
                float const damagePerSecond = extraAttacks *
                    averageWeaponDamage * ppm / 60.0f;
                score += damagePerSecond * w.weaponDps * WeaponScale();
                break;
            }
            case SPELL_EFFECT_HEALTH_LEECH:
                // The damage half is fixed proc damage just like SCHOOL_DAMAGE;
                // keep the survival/healing half separate and undiscounted.
                score += (hostileTarget ? 1.0f : -1.0f) *
                    perSecond * throughput * WeaponScale() * 0.68f;
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
    SpellItemEnchantmentEntry const* enchant, uint8 equipmentSlot = NULL_SLOT)
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
                float const throughput = item->IsRangedWeapon() ?
                    w.rangedDps : w.weaponDps;
                if (throughput > 0)
                    score += ScoreProcPayload(w, item, procSpell,
                        EnchantProcPpm(item, procSpell, enchant->amount[i]) *
                            WeaponProcActivity(w, item), 0);
                break;
            }
            case ITEM_ENCHANTMENT_TYPE_DAMAGE:
                if (item->Delay)
                    score += amount * 1000.0f / float(item->Delay) *
                        (item->IsRangedWeapon() ? w.rangedDps : w.weaponDps) *
                        WeaponScale() * (equipmentSlot ==
                            EQUIPMENT_SLOT_OFFHAND ? 0.70f : 1.0f);
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

float ScoreIntrinsicItemProperties(Weights const& w, Item const* item,
    uint8 equipmentSlot = NULL_SLOT)
{
    if (!item)
        return 0;
    float score = 0;
    for (uint8 slot = PROP_ENCHANTMENT_SLOT_0;
         slot <= PROP_ENCHANTMENT_SLOT_2; ++slot)
    {
        uint32 const enchantId = item->GetEnchantmentId(EnchantmentSlot(slot));
        if (!enchantId)
            continue;
        score += ScoreEnchantment(w, item->GetProto(),
            sSpellItemEnchantmentStore.LookupEntry(enchantId), equipmentSlot);
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
                float const throughput = item->IsRangedWeapon() ?
                    w.rangedDps : w.weaponDps;
                if (throughput <= 0)
                    break;
                float ppm = item->Spells[i].SpellPPMRate;
                if (ppm <= 0 && spell && spell->procChance > 100)
                    ppm = 1.0f;
                else if (ppm <= 0 && spell && spell->procChance && item->Delay)
                    ppm = float(spell->procChance) * 600.0f /
                        float(item->Delay);
                if (ppm > 0)
                    score += ScoreProcPayload(w, item, spell,
                        ppm * WeaponProcActivity(w, item), 0);
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

float ScoreItemInstance(Weights const& w, Player const* player,
    Item const* item, AutoProgressionCache const& cache)
{
    if (!item)
        return 0;
    // Profession enchants are replaceable and intentionally excluded. Random
    // property slots are generated as part of the item and remain intrinsic.
    return ScoreItemForPlayer(w, player, item->GetProto(), cache) +
        ScoreIntrinsicItemProperties(w, item, item->GetSlot());
}

bool IsSupportedScoredAura(AuraType aura)
{
    switch (aura)
    {
        case SPELL_AURA_MOD_ATTACKSPEED:
        case SPELL_AURA_MOD_MELEE_HASTE:
        case SPELL_AURA_MOD_DECREASE_SPEED:
        case SPELL_AURA_MOD_RANGED_HASTE:
        case SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK:
        case SPELL_AURA_MOD_INCREASE_SPEED:
        case SPELL_AURA_MOD_THREAT:
        case SPELL_AURA_PROC_TRIGGER_SPELL:
        case SPELL_AURA_MOD_DAMAGE_DONE:
        case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
        case SPELL_AURA_MOD_DAMAGE_DONE_CREATURE:
        case SPELL_AURA_MOD_MELEE_ATTACK_POWER_VERSUS:
        case SPELL_AURA_MOD_RANGED_ATTACK_POWER_VERSUS:
        case SPELL_AURA_MOD_DAMAGE_DONE_VERSUS:
        case SPELL_AURA_MOD_CRIT_PERCENT_VERSUS:
        case SPELL_AURA_MOD_FLAT_SPELL_DAMAGE_VERSUS:
        case SPELL_AURA_MOD_HEALTH_REGEN_IN_COMBAT:
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
        case SPELL_AURA_MOD_TARGET_RESISTANCE:
        case SPELL_AURA_MOD_POWER_REGEN:
        case SPELL_AURA_MOD_POWER_REGEN_PERCENT:
        case SPELL_AURA_MOD_MANA_REGEN_INTERRUPT:
        case SPELL_AURA_MOD_INCREASE_HEALTH:
        case SPELL_AURA_MOD_INCREASE_ENERGY:
        case SPELL_AURA_SCHOOL_ABSORB:
        case SPELL_AURA_MOD_SKILL:
        case SPELL_AURA_MOD_SKILL_TALENT:
        case SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE:
        case SPELL_AURA_MOD_STAT:
        case SPELL_AURA_MOD_SPELL_DAMAGE_OF_STAT_PERCENT:
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
        if (!IsScoredAuraEffect(spell->Effect[i]) ||
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

float StaticSpellHitFromAura(SpellEntry const* spell, uint8 depth = 0)
{
    if (!spell || depth > 2)
        return 0;
    float hit = 0;
    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (IsScoredAuraEffect(spell->Effect[i]) &&
            spell->EffectApplyAuraName[i] == SPELL_AURA_MOD_SPELL_HIT_CHANCE &&
            !EffectTargetsHostile(spell, i))
            hit += std::max(0.0f, float(
                spell->CalculateSimpleValue(SpellEffectIndex(i))));
        if (spell->Effect[i] == SPELL_EFFECT_TRIGGER_SPELL && depth < 2 &&
            spell->EffectTriggerSpell[i] != spell->Id)
            hit += StaticSpellHitFromAura(
                sSpellMgr.GetSpellEntry(spell->EffectTriggerSpell[i]), depth + 1);
    }
    return hit;
}
float StaticSpellHitFromEnchantment(SpellItemEnchantmentEntry const* enchant)
{
    if (!enchant)
        return 0;
    float hit = 0;
    for (uint8 i = 0; i < 3; ++i)
        if (enchant->type[i] == ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL)
            hit += StaticSpellHitFromAura(
                sSpellMgr.GetSpellEntry(enchant->spellid[i]));
    return hit;
}

float StaticSpellHitFromIntrinsicProperties(Item const* item)
{
    if (!item)
        return 0;
    float hit = 0;
    for (uint8 slot = PROP_ENCHANTMENT_SLOT_0;
         slot <= PROP_ENCHANTMENT_SLOT_2; ++slot)
    {
        uint32 const enchantId = item->GetEnchantmentId(EnchantmentSlot(slot));
        if (!enchantId)
            continue;
        hit += StaticSpellHitFromEnchantment(
            sSpellItemEnchantmentStore.LookupEntry(enchantId));
    }
    return hit;
}


float StaticSpellHitFromItem(ItemPrototype const* item)
{
    if (!item)
        return 0;
    float hit = 0;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (item->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP)
            hit += StaticSpellHitFromAura(
                sSpellMgr.GetSpellEntry(item->Spells[i].SpellId));
    return hit;
}

float ExpectedTemporarySpellHitFromItem(ItemPrototype const* item,
    Weights const& weights)
{
    if (!item)
        return 0;
    float expected = 0;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        _ItemSpell const& itemSpell = item->Spells[i];
        if (!itemSpell.SpellId)
            continue;
        SpellEntry const* spell = sSpellMgr.GetSpellEntry(itemSpell.SpellId);
        float ppm = 0;
        if (itemSpell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            ppm = ItemUsePpm(itemSpell, spell);
        else if (itemSpell.SpellTrigger == ITEM_SPELLTRIGGER_CHANCE_ON_HIT)
        {
            float const throughput = item->IsRangedWeapon() ?
                weights.rangedDps : weights.weaponDps;
            if (throughput <= 0)
                continue;
            ppm = itemSpell.SpellPPMRate;
            if (ppm <= 0 && spell && spell->procChance > 100)
                ppm = 1.0f;
            else if (ppm <= 0 && spell && spell->procChance && item->Delay)
                ppm = float(spell->procChance) * 600.0f /
                    float(item->Delay);
        }
        else
            continue;
        float const hit = StaticSpellHitFromAura(spell);
        if (hit <= 0 || ppm <= 0)
            continue;
        expected += hit * ProcUptime(spell, ppm);
    }
    return expected;
}

float StaticSpellHitFromExactSets(Player const* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& plan)
{
    std::unordered_map<uint32, uint32> counts;
    for (ItemPrototype const* item : plan)
        if (item && item->ItemSet)
            ++counts[item->ItemSet];

    float hit = 0;
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
                hit += StaticSpellHitFromAura(
                    sSpellMgr.GetSpellEntry(set->spells[i]));
    }
    return hit;
}

float UsefulSpellHit(Weights const& weights, float gearHit)
{
    constexpr float levelingTarget = 5.0f;
    if (gearHit <= 0)
        return 0;
    if (!weights.spellHitProfileCount)
        return std::min(gearHit, levelingTarget);

    float useful = 0;
    for (uint8 i = 0; i < weights.spellHitProfileCount; ++i)
    {
        SpellHitProfile const& profile = weights.spellHitProfiles[i];
        float const remaining = std::max(0.0f,
            levelingTarget - profile.talentBonus);
        useful += profile.share * std::min(gearHit, remaining);
    }
    return useful;
}

using WeaponSkillBonuses = std::unordered_map<uint32, float>;

void CollectStaticWeaponSkillsFromAura(SpellEntry const* spell,
    WeaponSkillBonuses& bonuses, uint8 depth = 0)
{
    if (!spell || depth > 2)
        return;

    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        if (IsScoredAuraEffect(spell->Effect[i]) &&
            (spell->EffectApplyAuraName[i] == SPELL_AURA_MOD_SKILL ||
             spell->EffectApplyAuraName[i] == SPELL_AURA_MOD_SKILL_TALENT) &&
            !EffectTargetsHostile(spell, i))
        {
            uint32 const skill = uint32(spell->EffectMiscValue[i]);
            float const amount = float(
                spell->CalculateSimpleValue(SpellEffectIndex(i)));
            if (amount > 0 && IsWeaponSkill(skill))
                bonuses[skill] += amount;
        }

        if (spell->Effect[i] == SPELL_EFFECT_TRIGGER_SPELL && depth < 2 &&
            spell->EffectTriggerSpell[i] &&
            spell->EffectTriggerSpell[i] != spell->Id)
            CollectStaticWeaponSkillsFromAura(
                sSpellMgr.GetSpellEntry(spell->EffectTriggerSpell[i]),
                bonuses, depth + 1);
    }
}

void CollectStaticWeaponSkillsFromEnchantment(
    SpellItemEnchantmentEntry const* enchant, WeaponSkillBonuses& bonuses)
{
    if (!enchant)
        return;
    for (uint8 i = 0; i < 3; ++i)
        if (enchant->type[i] == ITEM_ENCHANTMENT_TYPE_EQUIP_SPELL)
            CollectStaticWeaponSkillsFromAura(
                sSpellMgr.GetSpellEntry(enchant->spellid[i]), bonuses);
}

void CollectStaticWeaponSkillsFromIntrinsicProperties(Item const* item,
    WeaponSkillBonuses& bonuses)
{
    if (!item)
        return;
    for (uint8 slot = PROP_ENCHANTMENT_SLOT_0;
         slot <= PROP_ENCHANTMENT_SLOT_2; ++slot)
    {
        uint32 const enchantId =
            item->GetEnchantmentId(EnchantmentSlot(slot));
        if (enchantId)
            CollectStaticWeaponSkillsFromEnchantment(
                sSpellItemEnchantmentStore.LookupEntry(enchantId), bonuses);
    }
}

void CollectStaticWeaponSkillsFromItem(ItemPrototype const* item,
    WeaponSkillBonuses& bonuses)
{
    if (!item)
        return;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (item->Spells[i].SpellId &&
            item->Spells[i].SpellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP)
            CollectStaticWeaponSkillsFromAura(
                sSpellMgr.GetSpellEntry(item->Spells[i].SpellId), bonuses);
}

void CollectStaticWeaponSkillsFromExactSets(Player const* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& plan,
    WeaponSkillBonuses& bonuses)
{
    std::unordered_map<uint32, uint32> counts;
    for (ItemPrototype const* item : plan)
        if (item && item->ItemSet)
            ++counts[item->ItemSet];

    for (auto const& count : counts)
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
        for (uint8 i = 0; i < 8; ++i)
            if (set->spells[i] &&
                set->items_to_triggerspell[i] <= count.second)
                CollectStaticWeaponSkillsFromAura(
                    sSpellMgr.GetSpellEntry(set->spells[i]), bonuses);
    }
}

float UsefulWeaponSkill(float bonus)
{
    bonus = std::max(0.0f, std::min(bonus, 10.0f));
    return std::min(bonus, 5.0f) +
        std::max(0.0f, bonus - 5.0f) * 0.35f;
}

float PlannedWeaponSkillUsage(Weights const& weights,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& plan,
    uint32 skill)
{
    ItemPrototype const* main = plan[EQUIPMENT_SLOT_MAINHAND];
    ItemPrototype const* off = plan[EQUIPMENT_SLOT_OFFHAND];
    ItemPrototype const* ranged = plan[EQUIPMENT_SLOT_RANGED];

    bool const mainWeapon = main && main->Class == ITEM_CLASS_WEAPON;
    bool const offWeapon = off && off->Class == ITEM_CLASS_WEAPON;
    uint32 const mainSkill = mainWeapon ? main->GetProficiencySkill() : 0;
    uint32 const offSkill = offWeapon ? off->GetProficiencySkill() : 0;

    float meleeUsage = 0;
    if (mainSkill == skill)
        meleeUsage = offWeapon && offSkill != skill ? 0.65f : 1.0f;
    if (offSkill == skill)
        meleeUsage = mainWeapon && mainSkill != skill ?
            meleeUsage + 0.35f : 1.0f;

    float const meleeScale = std::min(1.0f,
        std::max(0.0f, weights.weaponDps / 13.0f));
    float usage = meleeUsage * meleeScale;

    if (ranged && ranged->Class == ITEM_CLASS_WEAPON &&
        ranged->GetProficiencySkill() == skill)
    {
        // Ranged attacks have no glancing blows; weapon skill only trims the
        // miss chance a little, unlike melee where +5 skill is worth several
        // percent of damage.
        constexpr float rangedSkillValue = 0.15f;
        float const rangedScale = std::min(1.0f,
            std::max(0.0f, weights.rangedDps / 16.0f)) * rangedSkillValue;
        usage = std::max(usage, rangedScale);
    }
    return usage;
}


float ScorePlannedWeaponSkills(Weights const& weights, Player const* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& plan)
{
    if (weights.weaponSkill <= 0)
        return 0;

    WeaponSkillBonuses itemBonuses;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        ItemPrototype const* item = plan[slot];
        if (!item)
            continue;
        CollectStaticWeaponSkillsFromItem(item, itemBonuses);
        Item const* current = player->GetItemByPos(
            INVENTORY_SLOT_BAG_0, slot);
        if (current && current->GetEntry() == item->ItemId)
            CollectStaticWeaponSkillsFromIntrinsicProperties(
                current, itemBonuses);
    }

    WeaponSkillBonuses setBonuses;
    CollectStaticWeaponSkillsFromExactSets(player, plan, setBonuses);

    std::set<uint32> skills;
    uint8 const weaponSlots[] =
    {
        EQUIPMENT_SLOT_MAINHAND, EQUIPMENT_SLOT_OFFHAND,
        EQUIPMENT_SLOT_RANGED
    };
    for (uint8 slot : weaponSlots)
    {
        ItemPrototype const* weapon = plan[slot];
        if (weapon && weapon->Class == ITEM_CLASS_WEAPON &&
            IsWeaponSkill(weapon->GetProficiencySkill()))
            skills.insert(weapon->GetProficiencySkill());
    }
    for (auto const& bonus : itemBonuses)
        skills.insert(bonus.first);
    for (auto const& bonus : setBonuses)
        skills.insert(bonus.first);
    for (uint8 i = 0; i < weights.weaponSkillTalentProfileCount; ++i)
        skills.insert(weights.weaponSkillTalentProfiles[i].skillId);

    float score = 0;
    float const setScale =
        sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_SET_BONUS_SCALE);
    for (uint32 skill : skills)
    {
        float const usage = PlannedWeaponSkillUsage(weights, plan, skill);
        if (usage <= 0)
            continue;

        float talentBonus = 0;
        for (uint8 i = 0; i < weights.weaponSkillTalentProfileCount; ++i)
            if (weights.weaponSkillTalentProfiles[i].skillId == skill)
            {
                talentBonus = weights.weaponSkillTalentProfiles[i].bonus;
                break;
            }
        float const permanent = std::max(0.0f,
            float(player->GetSkillBonusPermanent(uint16(skill))) + talentBonus);
        float const itemBonus = itemBonuses.count(skill) ?
            itemBonuses[skill] : 0;
        float const setBonus = setBonuses.count(skill) ?
            setBonuses[skill] : 0;
        float const itemUseful =
            UsefulWeaponSkill(permanent + itemBonus);
        float const setUseful =
            UsefulWeaponSkill(permanent + itemBonus + setBonus);
        score += (itemUseful + (setUseful - itemUseful) * setScale) *
            weights.weaponSkill * usage * SecondaryScale();
    }
    return score;
}

float ScoreEquipmentPlan(Weights const& w, Player const* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& plan)
{
    // Spell hit is contextual across the complete set. A fixed 5% target
    // covers normal leveling enemies up to two levels higher; learned talents
    // reduce only the spell families they really affect. Weapon skill is also
    // contextual: only bonuses matching the weapons in this plan are useful.
    // Strip both from per-item scores and add their whole-plan value below.
    Weights withoutContext = w;
    withoutContext.spellHit = 0;
    withoutContext.weaponSkill = 0;
    float score = 0;
    float itemSpellHit = 0;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        ItemPrototype const* item = plan[slot];
        if (!item)
            continue;
        float itemScore = ScoreItemBaseForPlayer(withoutContext, player, item);
        Item const* current = player->GetItemByPos(
            INVENTORY_SLOT_BAG_0, slot);
        if (current && current->GetEntry() == item->ItemId)
            itemScore += ScoreIntrinsicItemProperties(withoutContext, current, slot);
        if (slot == EQUIPMENT_SLOT_MAINHAND && w.twoHand)
            itemScore *= item->InventoryType == INVTYPE_2HWEAPON ? 1.20f : 0.90f;
        else if (slot == EQUIPMENT_SLOT_OFFHAND && item->Class == ITEM_CLASS_WEAPON)
            itemScore *= OffhandWeaponScale(w);
        score += itemScore;
        itemSpellHit += StaticSpellHitFromItem(item);
        if (current && current->GetEntry() == item->ItemId)
            itemSpellHit += StaticSpellHitFromIntrinsicProperties(current);
    }
    ItemPrototype const* offhand = plan[EQUIPMENT_SLOT_OFFHAND];
    if (w.dualWield && offhand && offhand->Class == ITEM_CLASS_WEAPON)
        score += 8.0f;
    score += ScoreExactSetBonuses(withoutContext, player, plan);
    score += ScorePlannedWeaponSkills(w, player, plan);

    if (w.spellHit > 0)
    {
        float const setSpellHit = StaticSpellHitFromExactSets(player, plan);
        float temporaryHit = 0;
        for (ItemPrototype const* item : plan)
            if (item)
                temporaryHit += ExpectedTemporarySpellHitFromItem(
                    item, w);

        float const usefulItems = UsefulSpellHit(w, itemSpellHit);
        float const usefulWithSets = UsefulSpellHit(w,
            itemSpellHit + setSpellHit);
        float const usefulWithTemporary = UsefulSpellHit(w,
            itemSpellHit + setSpellHit + temporaryHit);
        float const usefulSets = usefulWithSets - usefulItems;
        float const usefulTemporary = usefulWithTemporary - usefulWithSets;

        score += (usefulItems + usefulSets *
            sWorld.getConfig(CONFIG_FLOAT_AUTO_EQUIP_SET_BONUS_SCALE) +
            usefulTemporary) *
            w.spellHit * SecondaryScale();
    }
    return score;
}

bool IsPrimaryArmorSlot(uint8 slot)
{
    switch (slot)
    {
        case EQUIPMENT_SLOT_HEAD:
        case EQUIPMENT_SLOT_SHOULDERS:
        case EQUIPMENT_SLOT_CHEST:
        case EQUIPMENT_SLOT_WAIST:
        case EQUIPMENT_SLOT_LEGS:
        case EQUIPMENT_SLOT_FEET:
        case EQUIPMENT_SLOT_WRISTS:
        case EQUIPMENT_SLOT_HANDS:
            return true;
        default:
            return false;
    }
}

bool IsTankArmorClassCorrection(Player const* player, Weights const& weights,
    uint8 slot, ItemPrototype const* current, ItemPrototype const* candidate)
{
    if (!weights.tank || !IsPrimaryArmorSlot(slot) || !current ||
        !candidate || current->Class != ITEM_CLASS_ARMOR ||
        candidate->Class != ITEM_CLASS_ARMOR)
        return false;

    uint32 const highest = player->GetHighestKnownArmorProficiency();
    return highest && candidate->GetProficiencySkill() == highest &&
        current->GetProficiencySkill() != highest;
}

bool Compatible(Player* player, Weights const& weights,
    ItemPrototype const* main, ItemPrototype const* off);

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
    float const contextualWeaponSkillLoss = std::max(0.0f,
        ScorePlannedWeaponSkills(w, player, currentPlan) -
        ScorePlannedWeaponSkills(w, player, plan));
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
            WeaponSkillBonuses weaponSkillBonuses;
            CollectStaticWeaponSkillsFromAura(spell, weaponSkillBonuses);
            bool const losesContextualWeaponSkill =
                !weaponSkillBonuses.empty() && contextualWeaponSkillLoss > 0.001f;
            if (unsupported || (planScoreLower &&
                (bonusScore > 0.001f || losesContextualWeaponSkill)))
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
        if (!currentPlan[slot] ||
            !protectedSets.count(currentPlan[slot]->ItemSet))
            continue;

        bool const hand = slot == EQUIPMENT_SLOT_MAINHAND ||
            slot == EQUIPMENT_SLOT_OFFHAND;
        if (hand)
        {
            protectHands = true;
            continue;
        }
        if (!IsTankArmorClassCorrection(player, w, slot,
            currentPlan[slot], plan[slot]))
            plan[slot] = currentPlan[slot];
    }
    if (protectHands && Compatible(player, w,
        currentPlan[EQUIPMENT_SLOT_MAINHAND],
        currentPlan[EQUIPMENT_SLOT_OFFHAND]))
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
int64 ScoreSortKey(float score)
{
    return int64(std::llround(double(score) * 1000.0));
}

bool Better(Candidate const& a, Candidate const& b)
{
    if (ScoreSortKey(a.score) != ScoreSortKey(b.score))
        return ScoreSortKey(a.score) > ScoreSortKey(b.score);
    if (a.item->ItemLevel != b.item->ItemLevel)
        return a.item->ItemLevel > b.item->ItemLevel;
    return a.item->ItemId < b.item->ItemId;
}
void PreferTankArmorClassCandidates(Player const* player,
    Weights const& weights,
    std::array<std::vector<Candidate>, EQUIPMENT_SLOT_END>& candidates)
{
    if (!weights.tank)
        return;
    uint32 const highest = player->GetHighestKnownArmorProficiency();
    if (!highest)
        return;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (!IsPrimaryArmorSlot(slot))
            continue;
        std::vector<Candidate>& list = candidates[slot];
        bool const hasHighest = std::any_of(list.begin(), list.end(),
            [highest](Candidate const& candidate)
            {
                return candidate.item->Class == ITEM_CLASS_ARMOR &&
                    candidate.item->GetProficiencySkill() == highest;
            });
        if (!hasHighest)
            continue;
        list.erase(std::remove_if(list.begin(), list.end(),
            [highest](Candidate const& candidate)
            {
                return candidate.item->Class != ITEM_CLASS_ARMOR ||
                    candidate.item->GetProficiencySkill() != highest;
            }), list.end());
    }
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
        return !w.shield && !off;
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

bool MailReplacedItemsEnabled();

// Bags full: send the replaced item to the character's mailbox instead of
// blocking the upgrade (same sequence the mail handler uses).
bool MailEquippedItem(Player* player, uint8 slot)
{
    Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!item)
        return true;
    if (!MailReplacedItemsEnabled() || !player->GetSession())
        return false;

    player->MoveItemFromInventory(INVENTORY_SLOT_BAG_0, slot, true);
    CharacterDatabase.BeginTransaction(player->GetGUIDLow());
    item->DeleteFromInventoryDB();
    item->SaveToDB();
    CharacterDatabase.PExecute("UPDATE `item_instance` SET `owner_guid` = '%u' WHERE `guid`='%u'",
        player->GetGUIDLow(), item->GetGUIDLow());
    CharacterDatabase.CommitTransaction();

    MailDraft("Auto progression: replaced item",
        "This piece of gear was replaced by an upgrade while your bags were full.")
        .AddItem(item)
        .SendMailTo(MailReceiver(player, player->GetObjectGuid()),
            MailSender(MAIL_NORMAL, player->GetGUIDLow()), MAIL_CHECK_MASK_COPIED);
    return true;
}

bool StoreOrMailEquippedItem(Player* player, uint8 slot)
{
    if (MoveEquippedItemToBag(player, slot))
        return true;
    return MailEquippedItem(player, slot);
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
        if (ScoreSortKey(left.score) != ScoreSortKey(right.score))
            return ScoreSortKey(left.score) > ScoreSortKey(right.score);
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
        if (ScoreSortKey(choice.score) != ScoreSortKey(best.score) ||
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

    // Catalog pruning: items far below the character level cannot win a slot
    // against level-appropriate gear, but they still cost scoring and plan
    // optimization time for every candidate list they end up in.
    uint32 const levelWindow =
        sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_CANDIDATE_ITEM_LEVEL_WINDOW);
    if (levelWindow && item->ItemLevel + levelWindow < player->GetLevel())
        return false;

    uint32 sourceMask = 0;
    bool ownClassCreated = false;
    if (!ResolveItemSources(player, item, cache, sourceMask, ownClassCreated))
        return false;
    if (ownClassCreated)
        return true;
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
            score *= OffhandWeaponScale(weights);
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
            if (ScoreSortKey(left.score) != ScoreSortKey(right.score))
                return ScoreSortKey(left.score) > ScoreSortKey(right.score);
            return left.slot < right.slot;
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
            score *= OffhandWeaponScale(weights);
        requests.push_back({ slot, score,
            current && current->GetEntry() == item->ItemId });
    }
    std::sort(requests.begin(), requests.end(), [](ChoiceRequest const& left,
        ChoiceRequest const& right)
    {
        if (left.keepsCurrent != right.keepsCurrent)
            return left.keepsCurrent;
        if (ScoreSortKey(left.score) != ScoreSortKey(right.score))
            return ScoreSortKey(left.score) > ScoreSortKey(right.score);
        return left.slot < right.slot;
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
        ItemPrototype const* main = plan[EQUIPMENT_SLOT_MAINHAND];
        ItemPrototype const* off = plan[EQUIPMENT_SLOT_OFFHAND];
        if ((main || off) && !Compatible(player, weights, main, off))
        {
            // Constraint fallbacks must never resurrect an old hand layout
            // that violates the current spec (for example Prot 2H/no shield).
            // Clearing both makes TrySet reject it and lets Backfill choose a
            // different, fully valid pair.
            plan[EQUIPMENT_SLOT_MAINHAND] = nullptr;
            plan[EQUIPMENT_SLOT_OFFHAND] = nullptr;
        }
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

void OptimizeContextualPlan(Player* player, Weights const& weights,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards,
    std::array<std::vector<Candidate>, EQUIPMENT_SLOT_END> const& candidates,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    if (weights.spellHit <= 0 && weights.weaponSkill <= 0)
        return;

    auto slotPassesThreshold = [&](uint8 slot, ItemPrototype const* candidate)
    {
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!current || current->GetEntry() == candidate->ItemId)
            return true;
        if (IsTankArmorClassCorrection(player, weights, slot, current->GetProto(), candidate))
            return true;
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> without = plan;
        without[slot] = nullptr;
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> currentPlan = without;
        currentPlan[slot] = current->GetProto();
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> candidatePlan = without;
        candidatePlan[slot] = candidate;
        float const baseline = ScoreEquipmentPlan(weights, player, without);
        return MeetsUpgradeThreshold(
            std::max(0.0f, ScoreEquipmentPlan(weights, player, currentPlan) - baseline),
            std::max(0.0f, ScoreEquipmentPlan(weights, player, candidatePlan) - baseline));
    };
    auto handsPassThreshold = [&](ItemPrototype const* main,
        ItemPrototype const* off)
    {
        Item* currentMain = player->GetItemByPos(
            INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item* currentOff = player->GetItemByPos(
            INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        ItemPrototype const* currentMainProto =
            currentMain ? currentMain->GetProto() : nullptr;
        ItemPrototype const* currentOffProto =
            currentOff ? currentOff->GetProto() : nullptr;
        if ((currentMain || currentOff) && !Compatible(player, weights,
            currentMainProto, currentOffProto))
            return true;
        if ((!currentMain || currentMain->GetProto() == main) &&
            (!currentOff || currentOff->GetProto() == off))
            return true;
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> without = plan;
        without[EQUIPMENT_SLOT_MAINHAND] = nullptr;
        without[EQUIPMENT_SLOT_OFFHAND] = nullptr;
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> currentPlan = without;
        currentPlan[EQUIPMENT_SLOT_MAINHAND] =
            currentMain ? currentMain->GetProto() : nullptr;
        currentPlan[EQUIPMENT_SLOT_OFFHAND] =
            currentOff ? currentOff->GetProto() : nullptr;
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> candidatePlan = without;
        candidatePlan[EQUIPMENT_SLOT_MAINHAND] = main;
        candidatePlan[EQUIPMENT_SLOT_OFFHAND] = off;
        float const baseline = ScoreEquipmentPlan(weights, player, without);
        return MeetsUpgradeThreshold(
            std::max(0.0f, ScoreEquipmentPlan(weights, player, currentPlan) - baseline),
            std::max(0.0f, ScoreEquipmentPlan(weights, player, candidatePlan) - baseline));
    };

    std::vector<ItemPrototype const*> pairedHandCandidates[2];
    auto buildHandShortlist = [&](uint8 slot,
        std::vector<ItemPrototype const*>& result)
    {
        auto add = [&](ItemPrototype const* item)
        {
            if (item && std::find(result.begin(), result.end(), item) == result.end())
                result.push_back(item);
        };
        add(plan[slot]);
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        add(current ? current->GetProto() : nullptr);

        size_t const perMetric = 20;
        for (size_t i = 0; i < std::min(perMetric, candidates[slot].size()); ++i)
            add(candidates[slot][i].item);

        Weights withoutHit = weights;
        withoutHit.spellHit = 0;
        withoutHit.weaponSkill = 0;
        std::vector<Candidate> intrinsic = candidates[slot];
        for (Candidate& candidate : intrinsic)
            candidate.score = ScoreItemForPlayer(withoutHit, player,
                candidate.item, cache);
        std::sort(intrinsic.begin(), intrinsic.end(), Better);
        for (size_t i = 0; i < std::min(perMetric, intrinsic.size()); ++i)
            add(intrinsic[i].item);
    };
    buildHandShortlist(EQUIPMENT_SLOT_MAINHAND, pairedHandCandidates[0]);
    buildHandShortlist(EQUIPMENT_SLOT_OFFHAND, pairedHandCandidates[1]);

    for (uint8 pass = 0; pass < 3; ++pass)
    {
        bool changed = false;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_MAINHAND ||
                slot == EQUIPMENT_SLOT_OFFHAND)
                continue;
            float bestScore = ScoreEquipmentPlan(weights, player, plan);
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> bestPlan = plan;
            for (Candidate const& candidate : candidates[slot])
            {
                std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> quick = plan;
                quick[slot] = candidate.item;
                if (ScoreEquipmentPlan(weights, player, quick) <=
                    bestScore + 0.001f ||
                    !slotPassesThreshold(slot, candidate.item))
                    continue;
                std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial = plan;
                if (!TryAddPlanItem(player, weights, cache, questRewards,
                    slot, candidate.item, trial))
                    continue;
                float const score = ScoreEquipmentPlan(weights, player, trial);
                if (score > bestScore + 0.001f)
                {
                    bestScore = score;
                    bestPlan = trial;
                }
            }
            if (bestPlan != plan)
            {
                plan = bestPlan;
                changed = true;
            }
        }

        auto improveHands = [&](ItemPrototype const* main,
            ItemPrototype const* off, float& bestScore,
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& bestPlan)
        {
            if (!Compatible(player, weights, main, off) ||
                (off && main->ItemId == off->ItemId && Unique(main)))
                return;
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> quick = plan;
            quick[EQUIPMENT_SLOT_MAINHAND] = main;
            quick[EQUIPMENT_SLOT_OFFHAND] = off;
            if (ScoreEquipmentPlan(weights, player, quick) <=
                bestScore + 0.001f)
                return;
            if (!handsPassThreshold(main, off))
                return;
            std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> trial;
            if (!TrySetPlanHands(player, weights, cache, questRewards,
                main, off, plan, trial))
                return;
            float const score = ScoreEquipmentPlan(weights, player, trial);
            if (score > bestScore + 0.001f)
            {
                bestScore = score;
                bestPlan = trial;
            }
        };

        float bestHandsScore = ScoreEquipmentPlan(weights, player, plan);
        std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> bestHandsPlan = plan;
        ItemPrototype const* plannedOff = plan[EQUIPMENT_SLOT_OFFHAND];
        for (Candidate const& candidate :
             candidates[EQUIPMENT_SLOT_MAINHAND])
        {
            ItemPrototype const* main = candidate.item;
            improveHands(main,
                main->InventoryType == INVTYPE_2HWEAPON ? nullptr : plannedOff,
                bestHandsScore, bestHandsPlan);
            if (main->InventoryType != INVTYPE_2HWEAPON)
                improveHands(main, nullptr, bestHandsScore, bestHandsPlan);
        }
        if (bestHandsPlan != plan)
        {
            plan = bestHandsPlan;
            changed = true;
        }

        bestHandsScore = ScoreEquipmentPlan(weights, player, plan);
        bestHandsPlan = plan;
        for (ItemPrototype const* main : pairedHandCandidates[0])
        {
            if (!main || main->InventoryType == INVTYPE_2HWEAPON)
                continue;
            for (ItemPrototype const* off : pairedHandCandidates[1])
                improveHands(main, off, bestHandsScore, bestHandsPlan);
        }
        if (bestHandsPlan != plan)
        {
            plan = bestHandsPlan;
            changed = true;
        }

        bestHandsScore = ScoreEquipmentPlan(weights, player, plan);
        bestHandsPlan = plan;
        ItemPrototype const* plannedMain = plan[EQUIPMENT_SLOT_MAINHAND];
        if (plannedMain && plannedMain->InventoryType != INVTYPE_2HWEAPON)
            for (Candidate const& candidate :
                 candidates[EQUIPMENT_SLOT_OFFHAND])
                improveHands(plannedMain, candidate.item,
                    bestHandsScore, bestHandsPlan);
        if (bestHandsPlan != plan)
        {
            plan = bestHandsPlan;
            changed = true;
        }
        if (!changed)
            break;
    }
}

float MarginalSlotPlanScore(Weights const& weights, Player const* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& plan,
    uint8 slot, ItemPrototype const* item)
{
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> without = plan;
    without[slot] = nullptr;
    if (!item)
        return 0;
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> with = without;
    with[slot] = item;
    return std::max(0.0f, ScoreEquipmentPlan(weights, player, with) -
        ScoreEquipmentPlan(weights, player, without));
}

float MarginalHandsPlanScore(Weights const& weights, Player const* player,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> const& plan,
    ItemPrototype const* main, ItemPrototype const* off)
{
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> without = plan;
    without[EQUIPMENT_SLOT_MAINHAND] = nullptr;
    without[EQUIPMENT_SLOT_OFFHAND] = nullptr;
    if (!main && !off)
        return 0;
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> with = without;
    with[EQUIPMENT_SLOT_MAINHAND] = main;
    with[EQUIPMENT_SLOT_OFFHAND] = off;
    return std::max(0.0f, ScoreEquipmentPlan(weights, player, with) -
        ScoreEquipmentPlan(weights, player, without));
}

void EnforceFinalUpgradeThresholds(Player* player, Weights const& weights,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_MAINHAND || slot == EQUIPMENT_SLOT_OFFHAND)
            continue;
        Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!current || (plan[slot] && current->GetEntry() == plan[slot]->ItemId))
            continue;
        if (!plan[slot])
        {
            plan[slot] = current->GetProto();
            continue;
        }
        float const currentScore = MarginalSlotPlanScore(weights, player,
            plan, slot, current->GetProto());
        float const candidateScore = MarginalSlotPlanScore(weights, player,
            plan, slot, plan[slot]);
        if (!IsTankArmorClassCorrection(player, weights, slot, current->GetProto(), plan[slot]) &&
            !MeetsUpgradeThreshold(currentScore, candidateScore))
            plan[slot] = current->GetProto();
    }

    Item* currentMain = player->GetItemByPos(
        INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* currentOff = player->GetItemByPos(
        INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    bool const mainUnchanged = (!currentMain && !plan[EQUIPMENT_SLOT_MAINHAND]) ||
        (currentMain && plan[EQUIPMENT_SLOT_MAINHAND] &&
         currentMain->GetEntry() == plan[EQUIPMENT_SLOT_MAINHAND]->ItemId);
    bool const offUnchanged = (!currentOff && !plan[EQUIPMENT_SLOT_OFFHAND]) ||
        (currentOff && plan[EQUIPMENT_SLOT_OFFHAND] &&
         currentOff->GetEntry() == plan[EQUIPMENT_SLOT_OFFHAND]->ItemId);
    if ((currentMain || currentOff) && (!mainUnchanged || !offUnchanged) &&
        Compatible(player, weights,
            currentMain ? currentMain->GetProto() : nullptr,
            currentOff ? currentOff->GetProto() : nullptr))
    {
        float const currentScore = MarginalHandsPlanScore(weights, player, plan,
            currentMain ? currentMain->GetProto() : nullptr,
            currentOff ? currentOff->GetProto() : nullptr);
        float const candidateScore = MarginalHandsPlanScore(weights, player, plan,
            plan[EQUIPMENT_SLOT_MAINHAND], plan[EQUIPMENT_SLOT_OFFHAND]);
        if (!MeetsUpgradeThreshold(currentScore, candidateScore))
        {
            plan[EQUIPMENT_SLOT_MAINHAND] =
                currentMain ? currentMain->GetProto() : nullptr;
            plan[EQUIPMENT_SLOT_OFFHAND] =
                currentOff ? currentOff->GetProto() : nullptr;
        }
    }
}

void BackfillEmptyPlanSlots(Player* player, Weights const& weights,
    AutoProgressionCache const& cache, QuestRewardCatalog const& questRewards,
    std::array<std::vector<Candidate>, EQUIPMENT_SLOT_END> const& candidates,
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END>& plan)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (weights.tank && IsPrimaryArmorSlot(slot) && plan[slot])
        {
            uint32 const highest = player->GetHighestKnownArmorProficiency();
            bool const hasHighestCandidate = highest && std::any_of(
                candidates[slot].begin(), candidates[slot].end(),
                [highest](Candidate const& candidate)
                {
                    return candidate.item->Class == ITEM_CLASS_ARMOR &&
                        candidate.item->GetProficiencySkill() == highest;
                });
            if (hasHighestCandidate &&
                (plan[slot]->Class != ITEM_CLASS_ARMOR ||
                 plan[slot]->GetProficiencySkill() != highest))
            {
                // Quest/unique/max-count fallbacks can restore the currently
                // equipped lower armor class. Treat that fallback as empty so
                // the next legal highest-class candidate gets a chance.
                plan[slot] = nullptr;
            }
        }
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
    if ((plannedMain || plannedOff) &&
        !Compatible(player, weights, plannedMain, plannedOff))
    {
        plan[EQUIPMENT_SLOT_MAINHAND] = nullptr;
        plan[EQUIPMENT_SLOT_OFFHAND] = nullptr;
        plannedMain = nullptr;
        plannedOff = nullptr;
    }
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
                (off.item->Class == ITEM_CLASS_WEAPON ? OffhandWeaponScale(weights) : 1.0f);
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

// DeleteReplacedItems only destroys gear up to the configured quality; rarer
// pieces (epics by default) are moved to the bags instead so player-farmed
// items are never lost silently.
// Gear generated by AutoEquip is marked with the character as its creator
// ("Made by <name>" in the tooltip). That is what lets DeleteReplacedItems tell
// generated epics apart from gear the character farmed or was given.
bool IsGeneratedItem(Item const* item)
{
    return item && sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_MARK_GENERATED_ITEMS) &&
        !item->GetGuidValue(ITEM_FIELD_CREATOR).IsEmpty() &&
        item->GetGuidValue(ITEM_FIELD_CREATOR) == item->GetOwnerGuid();
}

Item* CreateGeneratedItem(Player* player, ItemPrototype const* item, uint32 count)
{
    Item* created = Item::CreateItem(item->ItemId, count, player->GetObjectGuid());
    if (created && sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_MARK_GENERATED_ITEMS))
        created->SetGuidValue(ITEM_FIELD_CREATOR, player->GetObjectGuid());
    return created;
}

bool DeletesReplacedItem(Item const* current, bool deleteReplaced)
{
    if (!current || !deleteReplaced)
        return false;
    // Audit items are synthetic; keeping the audit independent of bag space
    // keeps its results deterministic.
    if (PlayerAutoProgression::IsAuditExecution())
        return true;
    if (IsGeneratedItem(current))
        return true;
    return current->GetProto()->Quality <=
        sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_DELETE_REPLACED_MAX_QUALITY);
}

bool MailReplacedItemsEnabled()
{
    return sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_MAIL_REPLACED_ITEMS) &&
        !PlayerAutoProgression::IsAuditExecution();
}

// Thrown weapons are consumed per attack; a single throwing knife is useless.
uint32 PlannedItemCount(ItemPrototype const* item)
{
    if (item && item->Class == ITEM_CLASS_WEAPON &&
        item->SubClass == ITEM_SUBCLASS_WEAPON_THROWN)
        return std::max<uint32>(1, item->GetMaxStackSize());
    return 1;
}

bool EquipPlannedItem(Player* player, uint8 slot, ItemPrototype const* item, bool replace,
    bool deleteReplaced)
{
    if (!item)
        return false;

    Item* current = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (current && current->GetEntry() == item->ItemId)
        return false;
    bool const deleteCurrent = DeletesReplacedItem(current, deleteReplaced);
    if (current && (!replace ||
        (deleteCurrent && (current->GetProto()->Flags & ITEM_FLAG_INDESTRUCTIBLE))))
        return false;

    Item* replacement = CreateGeneratedItem(player, item, PlannedItemCount(item));
    if (!replacement)
        return false;

    uint16 destination = 0;
    if (player->CanEquipItem(slot, destination, replacement, current != nullptr) != EQUIP_ERR_OK ||
        (current && player->CanUnequipItem(current->GetPos(), false) != EQUIP_ERR_OK))
    {
        delete replacement;
        return false;
    }

    if (current && deleteCurrent)
        player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
    else if (current && !StoreOrMailEquippedItem(player, slot))
    {
        delete replacement;
        return false;
    }

    player->ItemAddedQuestCheck(replacement->GetEntry(), replacement->GetCount());
    player->EquipItem(destination, replacement, true);
    return true;
}

// A bow or gun without ammunition is dead weight (hunters would auto-shoot
// nothing). Mirrors the PlayerBot AddHunterAmmo helper, but only hands out
// ammunition that is obtainable through the configured acquisition sources.
uint32 SupplyAmmo(Player* player, AutoProgressionCache const& cache)
{
    if (!sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_SUPPLY_AMMO))
        return 0;

    Item* ranged = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
    if (!ranged || ranged->GetProto()->Class != ITEM_CLASS_WEAPON)
        return 0;

    uint32 ammoSubClass = 0;
    switch (ranged->GetProto()->SubClass)
    {
        case ITEM_SUBCLASS_WEAPON_GUN:
            ammoSubClass = ITEM_SUBCLASS_BULLET;
            break;
        case ITEM_SUBCLASS_WEAPON_BOW:
        case ITEM_SUBCLASS_WEAPON_CROSSBOW:
            ammoSubClass = ITEM_SUBCLASS_ARROW;
            break;
        default:
            return 0;
    }

    ItemPrototype const* best = nullptr;
    float bestDamage = 0;
    for (uint32 itemId : cache.ammoItems)
    {
        ItemPrototype const* proto = sObjectMgr.GetItemPrototype(itemId);
        if (!proto || proto->SubClass != ammoSubClass ||
            proto->RequiredLevel > player->GetLevel() ||
            proto->RequiredHonorRank || proto->RequiredCityRank ||
            player->CanUseAmmo(itemId) != EQUIP_ERR_OK)
            continue;
        if (proto->RequiredReputationFaction &&
            uint32(player->GetReputationRank(proto->RequiredReputationFaction)) <
                proto->RequiredReputationRank)
            continue;
        uint32 sourceMask = 0;
        bool ownClassCreated = false;
        if (!ResolveItemSources(player, proto, cache, sourceMask, ownClassCreated) ||
            (!ownClassCreated && !EnabledItemSources(sourceMask, true)))
            continue;
        float const damage = proto->Damage[0].DamageMin;
        if (!best || damage > bestDamage ||
            (damage == bestDamage && proto->ItemLevel < best->ItemLevel))
        {
            best = proto;
            bestDamage = damage;
        }
    }
    if (!best)
        return 0;

    // Ammunition already in the bags: keep and select anything of the right
    // type that is at least as good (bought or looted by the player), and
    // clear out obsolete stacks so every level does not leave another 200
    // arrows behind.
    struct AmmoStack
    {
        uint8 bag;
        uint8 slot;
        Item* item;
    };
    std::vector<AmmoStack> stacks;
    auto collect = [&](uint8 bag, uint8 slot, Item* item)
    {
        if (item && item->GetProto()->Class == ITEM_CLASS_PROJECTILE)
            stacks.push_back({ bag, slot, item });
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        collect(INVENTORY_SLOT_BAG_0, slot, player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = (Bag*)player->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                collect(bagSlot, uint8(slot), bag->GetItemByPos(uint8(slot)));

    Item* better = nullptr;
    for (AmmoStack const& stack : stacks)
    {
        ItemPrototype const* proto = stack.item->GetProto();
        if (proto->SubClass == ammoSubClass && proto->Damage[0].DamageMin >= bestDamage &&
            (!better || proto->Damage[0].DamageMin > better->GetProto()->Damage[0].DamageMin))
            better = stack.item;
    }
    if (better)
    {
        if (player->GetUInt32Value(PLAYER_AMMO_ID) != better->GetEntry())
            player->SetAmmo(better->GetEntry());
        return 0;
    }
    for (AmmoStack const& stack : stacks)
        if (stack.item->GetEntry() != best->ItemId &&
            stack.item->GetProto()->Damage[0].DamageMin < bestDamage)
            player->DestroyItem(stack.bag, stack.slot, true);

    uint32 supplied = 0;
    uint32 const stackSize = std::max<uint32>(1, best->GetMaxStackSize());
    if (player->GetItemCount(best->ItemId, false) < stackSize)
    {
        ItemPosCountVec destination;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, destination,
                best->ItemId, stackSize) == EQUIP_ERR_OK &&
            player->StoreNewItem(destination, best->ItemId, true))
            supplied = stackSize;
    }
    if (player->GetUInt32Value(PLAYER_AMMO_ID) != best->ItemId &&
        player->HasItemCount(best->ItemId, 1))
        player->SetAmmo(best->ItemId);
    return supplied;
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
        else if (DeletesReplacedItem(handsToStore[i], deleteReplaced) &&
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
        bool const transitionalError =
            DeletesReplacedItem(currentOff, deleteReplaced) &&
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

    std::vector<Item*> handsToKeep;
    for (int i = 0; i < handCount; ++i)
        if (!DeletesReplacedItem(handsToStore[i], deleteReplaced))
            handsToKeep.push_back(handsToStore[i]);
    if (!handsBlocked && !handsToKeep.empty() && !MailReplacedItemsEnabled())
    {
        std::vector<Item*> trial = toStore;
        trial.insert(trial.end(), handsToKeep.begin(), handsToKeep.end());
        InventoryResult const result =
            player->CanStoreItems(trial.data(), int(trial.size()));
        if (result != EQUIP_ERR_OK)
            blockHands(handsToKeep[0]->GetSlot(), "cannot-store-replaced",
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
    else
        toStore.insert(toStore.end(), handsToKeep.begin(), handsToKeep.end());

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

        bool const deleteCurrent = DeletesReplacedItem(current, deleteReplaced);
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
                else if (deleteCurrent &&
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

        if (!failureReason && current && !deleteCurrent && !MailReplacedItemsEnabled())
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

        if (current && !deleteCurrent)
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
        if (replace)
            EnforceFinalUpgradeThresholds(player, weights, plan);
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
    int64 bestKey = std::numeric_limits<int64>::min();
    bestScore = -std::numeric_limits<float>::max();
    for (EnchantCandidate const& candidate : cache.improvements)
    {
        if (!ImprovementFits(player, item, equipmentSlot, candidate))
            continue;
        float const score = ScoreEnchantment(weights, item,
            sSpellItemEnchantmentStore.LookupEntry(candidate.enchantId), equipmentSlot);
        uint32 const level = ImprovementMinimumLevel(candidate);
        int64 const scoreKey = ScoreSortKey(score);
        if (!best || scoreKey > bestKey ||
            (scoreKey == bestKey &&
             std::make_tuple(level, candidate.spellId,
                 candidate.enchantId, candidate.sourceItemId,
                 uint8(candidate.source), candidate.effectIndex) >
             std::make_tuple(bestLevel, best->spellId,
                 best->enchantId, best->sourceItemId,
                 uint8(best->source), best->effectIndex)))
        {
            best = &candidate;
            bestScore = score;
            bestLevel = level;
            bestKey = scoreKey;
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

// Read-only counterparts of LearnClassQuestSpells/LearnSpellBooks for status
// and preview output.
uint32 CountLearnableClassQuestSpells(Player* player, AutoProgressionCache const& cache)
{
    if (!sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_CLASS_QUEST_SPELLS))
        return 0;
    std::set<uint32> spells;
    for (ClassQuestGrant const& grant : cache.classQuestGrants)
    {
        if (!ClassQuestGrantApplies(player, grant))
            continue;
        for (uint32 spellId : grant.spells)
            if (!player->HasSpell(spellId) && sSpellMgr.GetSpellEntry(spellId) &&
                player->IsSpellFitByClassAndRace(spellId) &&
                !sObjectMgr.IsSpellDisabled(spellId))
                spells.insert(spellId);
    }
    return uint32(spells.size());
}

uint32 CountLearnableSpellBooks(Player* player, AutoProgressionCache const& cache)
{
    if (!sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_SPELL_BOOKS))
        return 0;
    std::set<uint32> spells;
    for (SpellBook const& book : cache.spellBooks)
    {
        bool checkedAvailability = false;
        bool available = false;
        for (uint32 spellId : book.spells)
        {
            if (player->HasSpell(spellId) ||
                !player->IsSpellFitByClassAndRace(spellId) ||
                sObjectMgr.IsSpellDisabled(spellId))
                continue;
            if (!checkedAvailability)
            {
                available = SpellBookAvailable(player,
                    sObjectMgr.GetItemPrototype(book.itemId), cache);
                checkedAvailability = true;
            }
            if (available)
                spells.insert(spellId);
        }
    }
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
    add(ITEM_SOURCE_CLASS_CREATED, "class-created");
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
        addEnchant("permanent-ignored", PERM_ENCHANTMENT_SLOT);
        addEnchant("property0", PROP_ENCHANTMENT_SLOT_0);
        addEnchant("property1", PROP_ENCHANTMENT_SLOT_1);
        addEnchant("property2", PROP_ENCHANTMENT_SLOT_2);
        addEnchant("held-ignored", PROP_ENCHANTMENT_SLOT_3);
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

thread_local bool gAuditExecutionActive = false;
uint32 gNextAuditItemLowGuid = 0xF0000000u;

struct AuditExecutionGuard
{
    AuditExecutionGuard() : previous(gAuditExecutionActive)
    {
        gAuditExecutionActive = true;
    }

    ~AuditExecutionGuard()
    {
        gAuditExecutionActive = previous;
    }

    bool previous;
};

struct AuditProfile
{
    uint8 classId = 0;
    uint8 raceId = 0;
    uint8 tree = 0;
};

struct AuditMatrixState
{
    uint64 runId = 0;
    std::vector<AuditProfile> profiles;
    std::atomic<bool> cancelRequested{ false };
    std::atomic<bool> finishing{ false };
    std::atomic<bool> finished{ false };
    std::atomic<uint32> currentProfile{ 0 };
    std::atomic<uint32> currentLevel{ 0 };
    std::atomic<uint32> completedSteps{ 0 };
    std::atomic<uint32> errors{ 0 };
    uint32 delayMs = 0;
    uint32 startedMs = 0;
    uint32 profileStartedMs = 0;
    uint32 profileErrorStart = 0;
    uint64 profileHash = 1469598103934665603ULL;
    uint64 aggregateHash = 1469598103934665603ULL;
    bool beginLogged = false;
    std::string result;
    std::string reason;
    uint64 configHash = 0;
    uint64 catalogHash = 0;
    std::shared_ptr<AutoProgressionCache const> cacheSnapshot;
    std::unique_ptr<WorldSession> session;
    std::unique_ptr<Player> player;
};

std::mutex gAuditMatrixMutex;
std::shared_ptr<AuditMatrixState> gAuditMatrix;
std::atomic<uint32> gAuditRunSequence{ 0 };

char const* AuditClassName(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR: return "warrior";
        case CLASS_PALADIN: return "paladin";
        case CLASS_HUNTER: return "hunter";
        case CLASS_ROGUE: return "rogue";
        case CLASS_PRIEST: return "priest";
        case CLASS_SHAMAN: return "shaman";
        case CLASS_MAGE: return "mage";
        case CLASS_WARLOCK: return "warlock";
        case CLASS_DRUID: return "druid";
        default: return "unknown";
    }
}

char const* AuditTreeName(uint8 classId, uint8 tree)
{
    static char const* const warrior[] =
        { "arms", "fury", "protection" };
    static char const* const paladin[] =
        { "holy", "protection", "retribution" };
    static char const* const hunter[] =
        { "beastmastery", "marksmanship", "survival" };
    static char const* const rogue[] =
        { "assassination", "combat", "subtlety" };
    static char const* const priest[] =
        { "discipline", "holy", "shadow" };
    static char const* const shaman[] =
        { "elemental", "enhancement", "restoration" };
    static char const* const mage[] =
        { "arcane", "fire", "frost" };
    static char const* const warlock[] =
        { "affliction", "demonology", "destruction" };
    static char const* const druid[] =
        { "balance", "feral", "restoration" };
    if (tree >= 3)
        return "unknown";
    switch (classId)
    {
        case CLASS_WARRIOR: return warrior[tree];
        case CLASS_PALADIN: return paladin[tree];
        case CLASS_HUNTER: return hunter[tree];
        case CLASS_ROGUE: return rogue[tree];
        case CLASS_PRIEST: return priest[tree];
        case CLASS_SHAMAN: return shaman[tree];
        case CLASS_MAGE: return mage[tree];
        case CLASS_WARLOCK: return warlock[tree];
        case CLASS_DRUID: return druid[tree];
        default: return "unknown";
    }
}

uint8 DefaultAuditRace(uint8 classId)
{
    switch (classId)
    {
        case CLASS_WARRIOR: return RACE_ORC;
        case CLASS_PALADIN: return RACE_HUMAN;
        case CLASS_HUNTER: return RACE_DWARF;
        case CLASS_ROGUE: return RACE_HUMAN;
        case CLASS_PRIEST: return RACE_HUMAN;
        case CLASS_SHAMAN: return RACE_ORC;
        case CLASS_MAGE: return RACE_GNOME;
        case CLASS_WARLOCK: return RACE_GNOME;
        case CLASS_DRUID: return RACE_NIGHTELF;
        default: return 0;
    }
}

std::string LowerAuditToken(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

uint8 ParseAuditClass(std::string const& token)
{
    uint8 const classes[] =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE,
        CLASS_PRIEST, CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK,
        CLASS_DRUID
    };
    for (uint8 classId : classes)
        if (token == AuditClassName(classId))
            return classId;
    return 0;
}

bool ParseAuditTree(uint8 classId, std::string const& token, uint8& tree)
{
    if (token.size() == 1 && token[0] >= '0' && token[0] <= '2')
    {
        tree = uint8(token[0] - '0');
        return true;
    }
    for (uint8 candidate = 0; candidate < 3; ++candidate)
        if (token == AuditTreeName(classId, candidate))
        {
            tree = candidate;
            return true;
        }
    if (classId == CLASS_WARRIOR && token == "prot")
        tree = 2;
    else if (classId == CLASS_PALADIN && token == "prot")
        tree = 1;
    else if (classId == CLASS_HUNTER && token == "bm")
        tree = 0;
    else if (classId == CLASS_PRIEST && token == "disc")
        tree = 0;
    else if (classId == CLASS_SHAMAN && token == "enhance")
        tree = 1;
    else if (classId == CLASS_DRUID && token == "resto")
        tree = 2;
    else
        return false;
    return true;
}

void AddAllAuditProfiles(std::vector<AuditProfile>& profiles)
{
    uint8 const classes[] =
    {
        CLASS_WARRIOR, CLASS_PALADIN, CLASS_HUNTER, CLASS_ROGUE,
        CLASS_PRIEST, CLASS_SHAMAN, CLASS_MAGE, CLASS_WARLOCK,
        CLASS_DRUID
    };
    for (uint8 classId : classes)
        for (uint8 tree = 0; tree < 3; ++tree)
            profiles.push_back({ classId, DefaultAuditRace(classId), tree });
}

void MixAuditHash(uint64& hash, uint32 value)
{
    for (uint8 byte = 0; byte < 4; ++byte)
    {
        hash ^= uint8(value >> (byte * 8));
        hash *= 1099511628211ULL;
    }
}

uint64 AuditConfigurationHash()
{
    uint64 hash = 1469598103934665603ULL;
    for (uint32 index = 0; index < CONFIG_UINT32_VALUE_COUNT; ++index)
        MixAuditHash(hash, sWorld.getConfig(eConfigUInt32Values(index)));
    for (uint32 index = 0; index < CONFIG_INT32_VALUE_COUNT; ++index)
        MixAuditHash(hash, uint32(sWorld.getConfig(eConfigInt32Values(index))));
    for (uint32 index = 0; index < CONFIG_FLOAT_VALUE_COUNT; ++index)
    {
        float const value = sWorld.getConfig(eConfigFloatValues(index));
        uint32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        MixAuditHash(hash, bits);
    }
    for (uint32 index = 0; index < CONFIG_BOOL_VALUE_COUNT; ++index)
        MixAuditHash(hash, sWorld.getConfig(eConfigBoolValues(index)) ? 1u : 0u);
    MixAuditHash(hash, uint32(sWorld.GetWowPatch()));
    return hash;
}

uint64 AuditCatalogHash(AutoProgressionCache const& cache)
{
    uint64 hash = 1469598103934665603ULL;
    std::vector<uint32> equipment = cache.equipmentItems;
    std::sort(equipment.begin(), equipment.end());
    for (uint32 itemId : equipment)
        MixAuditHash(hash, itemId);

    std::vector<std::pair<uint32, uint32>> sources(
        cache.itemSources.begin(), cache.itemSources.end());
    std::sort(sources.begin(), sources.end());
    for (auto const& source : sources)
    {
        MixAuditHash(hash, source.first);
        MixAuditHash(hash, source.second);
    }

    std::vector<EnchantCandidate> improvements = cache.improvements;
    std::sort(improvements.begin(), improvements.end(),
        [](EnchantCandidate const& left, EnchantCandidate const& right)
    {
        if (left.spellId != right.spellId)
            return left.spellId < right.spellId;
        if (left.enchantId != right.enchantId)
            return left.enchantId < right.enchantId;
        if (left.sourceItemId != right.sourceItemId)
            return left.sourceItemId < right.sourceItemId;
        if (left.source != right.source)
            return uint8(left.source) < uint8(right.source);
        return left.effectIndex < right.effectIndex;
    });
    for (EnchantCandidate const& candidate : improvements)
    {
        MixAuditHash(hash, candidate.spellId);
        MixAuditHash(hash, candidate.enchantId);
        MixAuditHash(hash, candidate.sourceItemId);
        MixAuditHash(hash, uint32(candidate.source));
        MixAuditHash(hash, candidate.effectIndex);
    }

    for (uint8 classId = 0; classId < 12; ++classId)
    {
        std::vector<TrainerSource> trainers = cache.trainers.classes[classId];
        std::sort(trainers.begin(), trainers.end(),
            [](TrainerSource const& left, TrainerSource const& right)
        {
            if (left.id != right.id)
                return left.id < right.id;
            return left.isTemplate < right.isTemplate;
        });
        for (TrainerSource const& trainer : trainers)
        {
            MixAuditHash(hash, classId);
            MixAuditHash(hash, trainer.id);
            MixAuditHash(hash, trainer.isTemplate ? 1u : 0u);
        }
    }
    std::vector<TrainerSource> weapons = cache.trainers.weapons;
    std::sort(weapons.begin(), weapons.end(),
        [](TrainerSource const& left, TrainerSource const& right)
    {
        if (left.id != right.id)
            return left.id < right.id;
        return left.isTemplate < right.isTemplate;
    });
    for (TrainerSource const& trainer : weapons)
    {
        MixAuditHash(hash, 12);
        MixAuditHash(hash, trainer.id);
        MixAuditHash(hash, trainer.isTemplate ? 1u : 0u);
    }
    return hash;
}

void WriteAuditLog(std::string const& payload)
{
    sLog.Out(LOG_BASIC, LOG_LVL_BASIC, "AP_AUDIT|%s", payload.c_str());
}

uint32 CurrentTalentRank(Player const* player, TalentEntry const* talent)
{
    for (int8 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
        if (talent->RankID[rank] && player->HasSpell(talent->RankID[rank]))
            return uint32(rank + 1);
    return 0;
}

uint32 AuditTalentTreeCapacity(Player const* player, uint8 tree)
{
    uint32 capacity = 0;
    for (uint32 id = 0; id < sTalentStore.GetNumRows(); ++id)
    {
        TalentEntry const* talent = sTalentStore.LookupEntry(id);
        if (!talent)
            continue;
        TalentTabEntry const* tab =
            sTalentTabStore.LookupEntry(talent->TalentTab);
        if (!tab || !(tab->ClassMask & player->GetClassMask()) ||
            TalentTreeIndex(tab) != tree)
            continue;
        for (uint8 rank = 0; rank < MAX_TALENT_RANK; ++rank)
            if (talent->RankID[rank])
                ++capacity;
    }
    return capacity;
}

uint32 HighestDependentTalentRow(uint32 talentId)
{
    uint32 row = 0;
    for (uint32 id = 0; id < sTalentStore.GetNumRows(); ++id)
    {
        TalentEntry const* candidate = sTalentStore.LookupEntry(id);
        if (candidate && candidate->DependsOn == talentId)
            row = std::max(row, candidate->Row);
    }
    return row;
}

bool LearnNextAuditTalent(Player* player, uint8 requestedTree,
    uint32& learnedTalentId, uint32& learnedRank)
{
    struct Candidate
    {
        TalentEntry const* talent;
        uint32 nextRank;
        uint32 dependentRow;
    };
    for (uint8 offset = 0; offset < 3; ++offset)
    {
        uint8 const tree = uint8((requestedTree + offset) % 3);
        std::vector<Candidate> candidates;
        for (uint32 id = 0; id < sTalentStore.GetNumRows(); ++id)
        {
            TalentEntry const* talent = sTalentStore.LookupEntry(id);
            if (!talent)
                continue;
            TalentTabEntry const* tab =
                sTalentTabStore.LookupEntry(talent->TalentTab);
            if (!tab || !(tab->ClassMask & player->GetClassMask()) ||
                TalentTreeIndex(tab) != tree)
                continue;
            uint32 const nextRank = CurrentTalentRank(player, talent);
            if (nextRank >= MAX_TALENT_RANK ||
                !talent->RankID[nextRank])
                continue;
            candidates.push_back({ talent, nextRank,
                HighestDependentTalentRow(talent->TalentID) });
        }
        std::sort(candidates.begin(), candidates.end(),
            [](Candidate const& left, Candidate const& right)
        {
            if (left.talent->Row != right.talent->Row)
                return left.talent->Row > right.talent->Row;
            if (left.dependentRow != right.dependentRow)
                return left.dependentRow > right.dependentRow;
            if (left.talent->Col != right.talent->Col)
                return left.talent->Col < right.talent->Col;
            return left.talent->TalentID < right.talent->TalentID;
        });
        for (Candidate const& candidate : candidates)
            if (player->LearnTalent(candidate.talent->TalentID,
                    candidate.nextRank))
            {
                learnedTalentId = candidate.talent->TalentID;
                learnedRank = candidate.nextRank + 1;
                return true;
            }
    }
    return false;
}

// Opt-in AutoTalent: spend every free point with the same deterministic
// "deepest legal talent in the active tree" policy the audit uses. The tree
// is the one the character already invests in (class fallback otherwise), so
// a player who spent even a single point keeps steering the spec.
uint32 SpendTalentPoints(Player* player)
{
    uint32 spent = 0;
    while (player->GetFreeTalentPoints() > 0)
    {
        uint32 talentId = 0;
        uint32 rank = 0;
        if (!LearnNextAuditTalent(player,
                uint8(AutoProgressionTalentTree(player)), talentId, rank))
            break;
        ++spent;
    }
    return spent;
}

std::string AuditGearSnapshot(Player const* player, uint64& levelHash)
{
    std::ostringstream out;
    bool first = true;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item const* item = player->GetItemByPos(
            INVENTORY_SLOT_BAG_0, slot);
        uint32 const itemId = item ? item->GetEntry() : 0;
        uint32 const enchantId = item ?
            item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) : 0;
        if (!first)
            out << ",";
        out << uint32(slot) << ":" << itemId << "@" << enchantId;
        first = false;
        MixAuditHash(levelHash, slot);
        MixAuditHash(levelHash, itemId);
        MixAuditHash(levelHash, enchantId);
    }
    return out.str();
}

uint64 AuditSpellHash(Player const* player)
{
    std::vector<uint32> spells;
    for (auto const& entry : player->GetSpellMap())
        if (entry.second.state != PLAYERSPELL_REMOVED &&
            !entry.second.disabled)
            spells.push_back(entry.first);
    std::sort(spells.begin(), spells.end());
    uint64 hash = 1469598103934665603ULL;
    for (uint32 spellId : spells)
        MixAuditHash(hash, spellId);
    return hash;
}

uint64 AuditSkillHash(Player const* player)
{
    uint64 hash = 1469598103934665603ULL;
    for (uint16 skillId = 1; skillId < MAX_SKILL_TYPE; ++skillId)
    {
        if (!player->HasSkill(skillId))
            continue;
        MixAuditHash(hash, skillId);
        MixAuditHash(hash, player->GetSkillValuePure(skillId));
        MixAuditHash(hash, player->GetSkillMaxPure(skillId));
        MixAuditHash(hash, uint16(player->GetSkillBonusPermanent(skillId)));
        MixAuditHash(hash, uint16(player->GetSkillBonusTemporary(skillId)));
    }
    return hash;
}

void AuditViolation(AuditMatrixState& state, AuditProfile const& profile,
    uint32 level, char const* code, std::string const& detail)
{
    state.errors.fetch_add(1);
    std::ostringstream out;
    out << "schema=1|run=" << state.runId <<
        "|event=violation|profile=" << state.currentProfile.load() <<
        "|class=" << AuditClassName(profile.classId) <<
        "|tree=" << AuditTreeName(profile.classId, profile.tree) <<
        "|race=" << uint32(profile.raceId) << "|level=" << level <<
        "|code=" << code << "|detail=" << detail;
    WriteAuditLog(out.str());
}

uint32 ValidateAuditLevel(AuditMatrixState& state,
    AuditProfile const& profile, Player* player, uint32 level)
{
    uint32 const before = state.errors.load();
    std::array<uint32, 3> const points = TalentTreePoints(player);
    uint32 const expectedPoints = level >= 10 ? level - 9 : 0;
    uint32 const actualPoints = points[0] + points[1] + points[2];
    if (actualPoints != expectedPoints || player->GetFreeTalentPoints() != 0)
    {
        std::ostringstream detail;
        detail << "expected=" << expectedPoints << ",actual=" << actualPoints <<
            ",free=" << player->GetFreeTalentPoints();
        AuditViolation(state, profile, level, "talent_point_count",
            detail.str());
    }
    uint32 const expectedTargetPoints = std::min(expectedPoints,
        AuditTalentTreeCapacity(player, profile.tree));
    if (points[profile.tree] != expectedTargetPoints)
    {
        std::ostringstream detail;
        detail << "expected_target=" << expectedTargetPoints <<
            ",actual_target=" << points[profile.tree] <<
            ",total=" << actualPoints;
        AuditViolation(state, profile, level, "talent_target_progression",
            detail.str());
    }
    if (level >= 10 && AutoProgressionTalentTree(player) != profile.tree)
    {
        std::ostringstream detail;
        detail << "expected=" << uint32(profile.tree) << ",actual=" <<
            AutoProgressionTalentTree(player);
        AuditViolation(state, profile, level, "talent_tree_mismatch",
            detail.str());
    }

    Weights const weights = GetWeights(player);
    Item* main = player->GetItemByPos(
        INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* off = player->GetItemByPos(
        INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    if (!main)
        AuditViolation(state, profile, level, "missing_mainhand",
            "no mainhand item equipped after AutoEquip");
    if (weights.shield && (!off ||
            off->GetProto()->InventoryType != INVTYPE_SHIELD))
        AuditViolation(state, profile, level, "missing_shield",
            "shield profile has no shield equipped after AutoEquip");
    if ((main || off) && !Compatible(player, weights,
            main ? main->GetProto() : nullptr,
            off ? off->GetProto() : nullptr))
        AuditViolation(state, profile, level, "invalid_hands",
            "main/offhand topology does not match the active profile");

    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> plan = {};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        plan[slot] = item->GetProto();
        if (item->GetProto()->RequiredLevel > level ||
            player->CanUseItem(item->GetProto(), false) != EQUIP_ERR_OK)
        {
            std::ostringstream detail;
            detail << "slot=" << uint32(slot) << ",item=" << item->GetEntry();
            AuditViolation(state, profile, level, "illegal_item",
                detail.str());
        }
    }

    std::shared_ptr<AutoProgressionCache const> const cache =
        EnsureAutoProgressionCache();
    float const planScore = ScoreEquipmentPlan(weights, player, plan);
    if (!std::isfinite(planScore))
        AuditViolation(state, profile, level, "non_finite_score",
            "equipment plan score is not finite");

    uint32 const remainingTrainerSpells =
        CountAvailableTrainerSpells(player, *cache);
    if (remainingTrainerSpells)
    {
        std::ostringstream detail;
        detail << "remaining=" << remainingTrainerSpells;
        AuditViolation(state, profile, level, "trainer_spells_remaining",
            detail.str());
    }

    if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_MAX_WEAPON_SKILLS))
    {
        uint16 const expected =
            uint16(std::min<uint32>(level * 5, 300));
        uint16 const skills[] =
        {
            SKILL_SWORDS, SKILL_AXES, SKILL_BOWS, SKILL_GUNS,
            SKILL_MACES, SKILL_2H_SWORDS, SKILL_STAVES,
            SKILL_2H_MACES, SKILL_UNARMED, SKILL_2H_AXES,
            SKILL_DAGGERS, SKILL_THROWN, SKILL_CROSSBOWS,
            SKILL_WANDS, SKILL_POLEARMS, SKILL_FIST_WEAPONS,
            SKILL_DEFENSE, SKILL_POISONS, SKILL_LOCKPICKING
        };
        for (uint16 skill : skills)
        {
            uint16 const value = player->GetSkillValuePure(skill);
            uint16 const maximum = player->GetSkillMaxPure(skill);
            if (!value && !maximum)
                continue;
            if (value != expected || maximum != expected)
            {
                std::ostringstream detail;
                detail << "skill=" << skill << ",expected=" << expected <<
                    ",value=" << value << ",max=" << maximum;
                AuditViolation(state, profile, level,
                    "combat_skill_not_maxed", detail.str());
            }
        }
    }

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        float bestScore = 0;
        EnchantCandidate const* best = FindBestImprovement(player,
            item->GetProto(), slot, weights, *cache, bestScore);
        uint32 const actual =
            item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
        float actualScore = 0;
        if (actual)
            actualScore = ScoreEnchantment(weights, item->GetProto(),
                sSpellItemEnchantmentStore.LookupEntry(actual), slot);
        if (best && bestScore > actualScore + 0.001f &&
            actual != best->enchantId)
        {
            std::ostringstream detail;
            detail << "slot=" << uint32(slot) << ",item=" <<
                item->GetEntry() << ",expected=" << best->enchantId <<
                ",actual=" << actual << ",best_score=" << bestScore <<
                ",actual_score=" << actualScore;
            AuditViolation(state, profile, level, "enchant_mismatch",
                detail.str());
        }
    }
    return state.errors.load() - before;
}

bool CreateAuditProfile(AuditMatrixState& state,
    AuditProfile const& profile, std::string& error)
{
    state.session.reset(new WorldSession(0, nullptr, SEC_PLAYER, 0,
        LOCALE_enUS));
    state.player.reset(new Player(state.session.get()));
    uint32 const profileIndex = state.currentProfile.load();
    uint32 const guid = 0xE0000000u +
        uint32(profile.classId) * 16u + uint32(profile.tree);
    std::ostringstream name;
    name << "APM" << uint32(profile.classId) << uint32(profile.tree);
    {
        AuditExecutionGuard guard;
        if (!state.player->Create(guid, name.str(), profile.raceId,
                profile.classId, GENDER_MALE, 0, 0, 0, 0, 0))
        {
            error = "Player::Create failed";
            return false;
        }
        state.player->AddStartingItems();
    }
    if (!state.player->IsSavingDisabled() ||
        state.player->IsInWorld() || state.session->GetPlayer() ||
        state.player->GetLevel() != 1)
    {
        error = "synthetic player safety invariant failed";
        return false;
    }

    state.profileStartedMs = WorldTimer::getMSTime();
    state.profileErrorStart = state.errors.load();
    state.profileHash = 1469598103934665603ULL;
    state.currentLevel.store(0);
    Weights const weights = GetWeights(state.player.get());
    std::ostringstream out;
    out << "schema=1|run=" << state.runId <<
        "|event=profile_begin|profile=" << profileIndex <<
        "|class=" << AuditClassName(profile.classId) <<
        "|class_id=" << uint32(profile.classId) <<
        "|tree=" << AuditTreeName(profile.classId, profile.tree) <<
        "|tree_id=" << uint32(profile.tree) <<
        "|race=" << uint32(profile.raceId) <<
        "|fallback_tree=" << AutoProgressionTalentTree(
            state.player.get()) <<
        "|tank=" << weights.tank << "|shield=" << weights.shield <<
        "|dual_wield=" << weights.dualWield <<
        "|two_hand=" << weights.twoHand;
    WriteAuditLog(out.str());
    return true;
}

void DestroyAuditProfile(AuditMatrixState& state)
{
    state.player.reset();
    state.session.reset();
}

void FinishAuditMatrix(AuditMatrixState& state, char const* result,
    std::string const& reason)
{
    bool expected = false;
    if (!state.finishing.compare_exchange_strong(expected, true))
        return;
    DestroyAuditProfile(state);
    state.result = result;
    state.reason = reason;
    std::ostringstream out;
    out << "schema=1|run=" << state.runId <<
        "|event=matrix_end|result=" << result <<
        "|reason=" << (reason.empty() ? "none" : reason) <<
        "|profiles=" << state.currentProfile.load() <<
        "|levels=" << state.completedSteps.load() <<
        "|errors=" << state.errors.load() <<
        "|hash=" << state.aggregateHash <<
        "|duration_ms=" <<
            WorldTimer::getMSTimeDiffToNow(state.startedMs);
    WriteAuditLog(out.str());
    state.finished.store(true);
}

bool ProcessAuditLevel(AuditMatrixState& state,
    AuditProfile const& profile, std::string& error)
{
    Player* player = state.player.get();
    uint32 const previousLevel = state.currentLevel.load();
    uint32 const level = previousLevel ? previousLevel + 1 : 1;
    uint32 learned = 0;
    uint32 equipped = 0;
    uint32 enchanted = 0;
    uint32 learnedTalentId = 0;
    uint32 learnedTalentRank = 0;
    uint32 const stepStarted = WorldTimer::getMSTime();
    std::array<uint32, EQUIPMENT_SLOT_END> beforeItems = {};
    std::array<uint32, EQUIPMENT_SLOT_END> beforeEnchants = {};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            beforeItems[slot] = item->GetEntry();
            beforeEnchants[slot] =
                item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
        }

    {
        AuditExecutionGuard guard;
        if (level > 1)
            player->GiveLevel(level);
        learned += PlayerAutoProgression::LearnAvailableTrainerSpells(player);
        if (level >= 10)
        {
            if (player->GetFreeTalentPoints() != 1 ||
                !LearnNextAuditTalent(player, profile.tree,
                    learnedTalentId, learnedTalentRank))
            {
                std::ostringstream reason;
                reason << "could not spend exactly one talent point at level " <<
                    level << " (free=" << player->GetFreeTalentPoints() << ")";
                error = reason.str();
                return false;
            }
            learned +=
                PlayerAutoProgression::LearnAvailableTrainerSpells(player);
        }
        equipped = PlayerAutoProgression::EquipBestItems(player);
        enchanted = PlayerAutoProgression::EnchantBestItems(player);
    }

    std::array<uint32, 3> const points = TalentTreePoints(player);
    uint64 levelHash = 1469598103934665603ULL;
    MixAuditHash(levelHash, profile.classId);
    MixAuditHash(levelHash, profile.tree);
    MixAuditHash(levelHash, level);
    MixAuditHash(levelHash, learnedTalentId);
    MixAuditHash(levelHash, learnedTalentRank);
    std::string const gear = AuditGearSnapshot(player, levelHash);
    uint32 gearCount = 0;
    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> auditPlan = {};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            ++gearCount;
            auditPlan[slot] = item->GetProto();
        }
    Weights const auditWeights = GetWeights(player);
    float const planScore = ScoreEquipmentPlan(auditWeights, player, auditPlan);
    uint64 const spellsHash = AuditSpellHash(player);
    uint64 const skillsHash = AuditSkillHash(player);
    MixAuditHash(levelHash, profile.raceId);
    MixAuditHash(levelHash, uint32(spellsHash));
    MixAuditHash(levelHash, uint32(spellsHash >> 32));
    MixAuditHash(levelHash, uint32(skillsHash));
    MixAuditHash(levelHash, uint32(skillsHash >> 32));
    std::ostringstream decisions;
    decisions << std::fixed << std::setprecision(2);
    bool hasDecision = false;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        uint32 const itemId = item ? item->GetEntry() : 0;
        uint32 const enchantId = item ?
            item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT) : 0;
        if (itemId == beforeItems[slot] &&
            enchantId == beforeEnchants[slot])
            continue;
        if (hasDecision)
            decisions << ",";
        float const itemScore = item ? ScoreItemForPlayer(auditWeights,
            player, item->GetProto(), *state.cacheSnapshot) : 0;
        float enchantScore = 0;
        if (item && enchantId)
            enchantScore = ScoreEnchantment(auditWeights, item->GetProto(),
                sSpellItemEnchantmentStore.LookupEntry(enchantId), slot);
        decisions << uint32(slot) << ":" << beforeItems[slot] << "@" <<
            beforeEnchants[slot] << ">" << itemId << "@" << enchantId <<
            ":item=" << itemScore << ":enchant=" << enchantScore;
        hasDecision = true;
    }
    if (!hasDecision)
        decisions << "none";
    MixAuditHash(state.profileHash, uint32(levelHash));
    MixAuditHash(state.profileHash, uint32(levelHash >> 32));

    uint32 const levelErrors =
        ValidateAuditLevel(state, profile, player, level);
    std::ostringstream out;
    out << "schema=1|run=" << state.runId <<
        "|event=level|profile=" << state.currentProfile.load() <<
        "|class=" << AuditClassName(profile.classId) <<
        "|tree=" << AuditTreeName(profile.classId, profile.tree) <<
        "|race=" << uint32(profile.raceId) <<
        "|level=" << level <<
        "|detected_tree=" << AutoProgressionTalentTree(player) <<
        "|points=" << points[0] << "," << points[1] << "," << points[2] <<
        "|free=" << player->GetFreeTalentPoints() <<
        "|talent=" << learnedTalentId << ":" << learnedTalentRank <<
        "|learned=" << learned << "|equipped=" << equipped <<
        "|enchanted=" << enchanted << "|errors=" << levelErrors <<
        "|gear_count=" << gearCount << "|plan_score=" << planScore <<
        "|spells_hash=" << spellsHash << "|skills_hash=" << skillsHash <<
        "|changes=" << decisions.str() <<
        "|gear=" << gear << "|hash=" << levelHash <<
        "|duration_ms=" << WorldTimer::getMSTimeDiffToNow(stepStarted);
    WriteAuditLog(out.str());

    state.currentLevel.store(level);
    state.completedSteps.fetch_add(1);
    if (level < 60)
        return true;

    uint32 const profileErrors =
        state.errors.load() - state.profileErrorStart;
    MixAuditHash(state.aggregateHash, uint32(state.profileHash));
    MixAuditHash(state.aggregateHash, uint32(state.profileHash >> 32));
    std::ostringstream end;
    end << "schema=1|run=" << state.runId <<
        "|event=profile_end|profile=" << state.currentProfile.load() <<
        "|class=" << AuditClassName(profile.classId) <<
        "|tree=" << AuditTreeName(profile.classId, profile.tree) <<
        "|result=" << (profileErrors ? "failed" : "ok") <<
        "|errors=" << profileErrors << "|hash=" << state.profileHash <<
        "|duration_ms=" <<
            WorldTimer::getMSTimeDiffToNow(state.profileStartedMs);
    WriteAuditLog(end.str());

    DestroyAuditProfile(state);
    state.currentProfile.fetch_add(1);
    state.currentLevel.store(0);
    return true;
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
    if (!player || (!player->IsInWorld() && !IsAuditExecution()))
        return 0;
#ifdef MANGOS_DEBUG
    uint32 const debugStarted = WorldTimer::getMSTime();
    uint32 debugPasses = 0;
#endif
    std::shared_ptr<AutoProgressionCache const> const cache =
        EnsureAutoProgressionCache();
    TrainerCache const& trainerCache = cache->trainers;
    bool const maxSkills = sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_MAX_WEAPON_SKILLS);

    // Class quest spells first: stances, pets, Poisons, forms and rank-1
    // totems/racials gate whole trainer chains behind them.
    uint32 const questSpells = LearnClassQuestSpells(player, *cache);
    uint32 const questItems = GrantClassQuestItems(player, *cache);
    if (maxSkills)
        MaxCombatSkills(player); // Poisons skill values gate the poison recipes

    uint32 learned = questSpells;
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

    // Item-taught max ranks build on the trainer ranks learned above.
    uint32 const bookSpells = LearnSpellBooks(player, *cache);
    learned += bookSpells;

    if (maxSkills)
        MaxCombatSkills(player);
#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: learn player=%s level=%u learned=%u (quest=%u, book=%u, questItems=%u) passes=%u classSources=%u weaponSources=%u duration=%u ms.",
            player->GetName(), player->GetLevel(), learned, questSpells,
            bookSpells, questItems, debugPasses,
            player->GetClass() < 12 ?
                uint32(trainerCache.classes[player->GetClass()].size()) : 0,
            uint32(trainerCache.weapons.size()),
            WorldTimer::getMSTimeDiffToNow(debugStarted));
#else
    (void)questItems;
#endif
    return learned;
}

uint32 EquipBestItems(Player* player)
{
    CacheReadGuard cacheRead;
    if (!player || (!player->IsInWorld() && !IsAuditExecution()) ||
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

    PreferTankArmorClassCandidates(player, weights, candidates);
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
            float total = mainScore + off.score * (off.item->Class == ITEM_CLASS_WEAPON ? OffhandWeaponScale(weights) : 1.0f);
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
            float const currentScore = MarginalSlotPlanScore(weights, player,
                plan, slot, current->GetProto());
            float const candidateScore = MarginalSlotPlanScore(weights, player,
                plan, slot, plan[slot]);
            if (!IsTankArmorClassCorrection(player, weights, slot, current->GetProto(), plan[slot]) &&
                !MeetsUpgradeThreshold(currentScore, candidateScore))
                plan[slot] = current->GetProto();
        }

        Item* currentMain = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        Item* currentOff = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
        float const currentHands = MarginalHandsPlanScore(weights, player, plan,
            currentMain ? currentMain->GetProto() : nullptr,
            currentOff ? currentOff->GetProto() : nullptr);
        float const plannedHands = MarginalHandsPlanScore(weights, player, plan,
            plan[EQUIPMENT_SLOT_MAINHAND], plan[EQUIPMENT_SLOT_OFFHAND]);
        if ((currentMain || currentOff) &&
            Compatible(player, weights,
                currentMain ? currentMain->GetProto() : nullptr,
                currentOff ? currentOff->GetProto() : nullptr) &&
            !MeetsUpgradeThreshold(currentHands, plannedHands))
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

    if (replace)
    {
        OptimizeContextualPlan(player, weights, cache, questRewards, candidates,
            plan);
        ApplyLimitedItemRules(player, weights, cache, questRewards, plan);
        ProtectPlanAgainstSetBonusLoss(player, weights, plan);
        ApplyLimitedItemRules(player, weights, cache, questRewards, plan);
    }
    bool const deleteReplaced = sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_DELETE_REPLACED_ITEMS);
    BackfillEmptyPlanSlots(player, weights, cache, questRewards, candidates,
        plan);
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
    bool const mainLeaves = mainChanges && oldMain;
    bool const offLeaves = (offChanges || clearOffForTwoHand) && oldOff;
    bool const mainDelete = mainLeaves && DeletesReplacedItem(oldMain, deleteReplaced);
    bool const offDelete = offLeaves && DeletesReplacedItem(oldOff, deleteReplaced);
    Item* handsToStore[2] = { nullptr, nullptr };
    int handCount = 0;
    if (mainLeaves)
        handsToStore[handCount++] = oldMain;
    if (offLeaves)
        handsToStore[handCount++] = oldOff;
    Item* handsToKeep[2] = { nullptr, nullptr };
    int keepCount = 0;
    if (mainLeaves && !mainDelete)
        handsToKeep[keepCount++] = oldMain;
    if (offLeaves && !offDelete)
        handsToKeep[keepCount++] = oldOff;

    if (handCount)
    {
        if (!replace || (keepCount && !MailReplacedItemsEnabled() &&
            player->CanStoreItems(handsToKeep, keepCount) != EQUIP_ERR_OK))
            handReady = false;
        for (int i = 0; handReady && i < handCount; ++i)
            if (player->CanUnequipItem(handsToStore[i]->GetPos(), false) != EQUIP_ERR_OK ||
                (DeletesReplacedItem(handsToStore[i], deleteReplaced) &&
                 (handsToStore[i]->GetProto()->Flags & ITEM_FLAG_INDESTRUCTIBLE)))
                handReady = false;
    }

    Item* newMain = handReady && mainChanges ?
        CreateGeneratedItem(player, mainPlan, PlannedItemCount(mainPlan)) : nullptr;
    Item* newOff = handReady && offChanges ?
        CreateGeneratedItem(player, offPlan, PlannedItemCount(offPlan)) : nullptr;
    uint16 mainDestination = 0;
    uint16 offDestination = 0;
    if ((mainChanges && !newMain) || (offChanges && !newOff))
        handReady = false;
    if (handReady && mainChanges)
    {
        InventoryResult const result = player->CanEquipItem(EQUIPMENT_SLOT_MAINHAND,
            mainDestination, newMain, oldMain != nullptr);
        bool const deleteTwoHandOffhand = offDelete && clearOffForTwoHand &&
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

    // Move protected items into the bags (or the mailbox) first - that can
    // still fail - and only then destroy the deletable ones, so a failure
    // never leaves a slot empty.
    bool mainStored = false;
    bool mainMailed = false;
    if (handReady && mainLeaves && !mainDelete)
    {
        mainStored = MoveEquippedItemToBag(player, EQUIPMENT_SLOT_MAINHAND);
        if (!mainStored)
            mainMailed = MailEquippedItem(player, EQUIPMENT_SLOT_MAINHAND);
        if (!mainStored && !mainMailed)
            handReady = false;
    }
    if (handReady && offLeaves && !offDelete &&
        !StoreOrMailEquippedItem(player, EQUIPMENT_SLOT_OFFHAND))
    {
        if (mainStored)
            RestoreStoredItem(player, EQUIPMENT_SLOT_MAINHAND, oldMain);
        handReady = false;
    }
    if (handReady)
    {
        if (mainDelete)
            player->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND, true);
        if (offDelete)
            player->DestroyItem(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND, true);
    }

    if (handReady)
    {
        if (newMain)
        {
            player->ItemAddedQuestCheck(newMain->GetEntry(), newMain->GetCount());
            player->EquipItem(mainDestination, newMain, true);
            newMain = nullptr;
            ++equipped;
        }
        if (newOff)
        {
            player->ItemAddedQuestCheck(newOff->GetEntry(), newOff->GetCount());
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

    uint32 const ammoSupplied = SupplyAmmo(player, cache);
#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: equip player=%s level=%u catalog=%u eligible=%u slotCandidates=%u handPairs=%u handChecks=%u handAccepted=%u planned=%u blocked=%u equipped=%u ammo=%u result=complete duration=%u ms.",
            player->GetName(), player->GetLevel(),
            uint32(cache.equipmentItems.size()), debugEligibleItems,
            debugSlotCandidates, debugHandPairsVisited,
            debugHandConstraintChecks, debugHandConstraintAccepted,
            debugPlannedChanges, debugBlockedSlots, equipped, ammoSupplied,
            WorldTimer::getMSTimeDiffToNow(debugStarted));
#else
    (void)ammoSupplied;
#endif
    return equipped;
}

uint32 EnchantBestItems(Player* player)
{
    CacheReadGuard cacheRead;
    if (!player || (!player->IsInWorld() && !IsAuditExecution()))
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
                sSpellItemEnchantmentStore.LookupEntry(currentEnchantId), slot);
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
        ", talent tree " << AutoProgressionTalentTree(player) << ".";
    lines.push_back(out.str());

    out.str("");
    out.clear();
    out << "Pending actions=" << uint32(player->GetPendingAutoProgressionActions()) <<
        " (persistent=" << (player->HasPersistentAutoProgressionPending() ? 1 : 0) <<
        "), debounce=" << player->GetAutoProgressionDelay() << " ms, cache: " <<
        cache.equipmentItems.size() << " equipment items, " <<
        cache.ammoItems.size() << " ammo items, " <<
        cache.itemSources.size() << " source mappings, " <<
        cache.improvements.size() << " improvements, " <<
        cache.classQuestGrants.size() << " class quest grants, " <<
        cache.spellBooks.size() << " spell books.";
    lines.push_back(out.str());

    out.str("");
    out.clear();
    out << "Learnable now: " << CountAvailableTrainerSpells(player, cache) <<
        " trainer spells, " << CountLearnableClassQuestSpells(player, cache) <<
        " class quest spells, " << CountLearnableSpellBooks(player, cache) <<
        " book spells; free talent points=" << player->GetFreeTalentPoints() <<
        " (AutoTalent " << (AutoTalentEnabled() ? "on" : "off") << ").";
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
    PreferTankArmorClassCandidates(player, weights, candidates);
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
                (off.item->Class == ITEM_CLASS_WEAPON ? OffhandWeaponScale(weights) : 1.0f);
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
        float const currentScore = MarginalSlotPlanScore(weights, player,
            plan, slot, current->GetProto());
        float const candidateScore = MarginalSlotPlanScore(weights, player,
            plan, slot, plan[slot]);
        if (!replace || !plan[slot] ||
            (!IsTankArmorClassCorrection(player, weights, slot, current->GetProto(), plan[slot]) &&
             !MeetsUpgradeThreshold(currentScore, candidateScore)))
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
    else if ((currentMain || currentOff) &&
        Compatible(player, weights,
            currentMain ? currentMain->GetProto() : nullptr,
            currentOff ? currentOff->GetProto() : nullptr))
    {
        float const currentHands = MarginalHandsPlanScore(weights, player, plan,
            currentMain ? currentMain->GetProto() : nullptr,
            currentOff ? currentOff->GetProto() : nullptr);
        float const plannedHands = MarginalHandsPlanScore(weights, player, plan,
            plan[EQUIPMENT_SLOT_MAINHAND], plan[EQUIPMENT_SLOT_OFFHAND]);
        if (!MeetsUpgradeThreshold(currentHands, plannedHands))
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
    if (replace)
    {
        OptimizeContextualPlan(player, weights, cache, questRewards, candidates,
            plan);
        ApplyLimitedItemRules(player, weights, cache, questRewards, plan);
        ProtectPlanAgainstSetBonusLoss(player, weights, plan);
        ApplyLimitedItemRules(player, weights, cache, questRewards, plan);
    }
    BackfillEmptyPlanSlots(player, weights, cache, questRewards, candidates,
        plan);
#ifdef MANGOS_DEBUG
    debugBlockedSlots =
#endif
        FinalizeEquipmentPlan(player, weights, cache, questRewards, plan,
            replace, deleteReplaced);

    std::ostringstream header;
    header << "Preview for " << player->GetName() << ": " <<
        CountAvailableTrainerSpells(player, cache) <<
        " trainer spells, " << CountLearnableClassQuestSpells(player, cache) <<
        " class quest spells and " << CountLearnableSpellBooks(player, cache) <<
        " book spells currently learnable; equipment eligibility uses current skills.";
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
        bool const hands = slot == EQUIPMENT_SLOT_MAINHAND ||
            slot == EQUIPMENT_SLOT_OFFHAND;
        float currentScore = 0;
        float candidateScore = 0;
        if (hands)
        {
            currentScore = MarginalHandsPlanScore(weights, player, plan,
                currentMain ? currentMain->GetProto() : nullptr,
                currentOff ? currentOff->GetProto() : nullptr);
            candidateScore = MarginalHandsPlanScore(weights, player, plan,
                plan[EQUIPMENT_SLOT_MAINHAND], plan[EQUIPMENT_SLOT_OFFHAND]);
        }
        else
        {
            currentScore = MarginalSlotPlanScore(weights, player, plan, slot,
                current ? current->GetProto() : nullptr);
            candidateScore = MarginalSlotPlanScore(weights, player, plan, slot,
                candidate);
        }
        std::ostringstream out;
        out << EquipmentSlotName(slot) << ": " <<
            DescribeScoredItem(current ? current->GetProto() : nullptr,
                currentScore, cache, current) <<
            " -> " << DescribeScoredItem(candidate, candidateScore, cache, nullptr);
        if (candidate)
            out << (hands ? " (hand-pair " : " (") << std::showpos <<
                std::fixed << std::setprecision(1) <<
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
            sSpellItemEnchantmentStore.LookupEntry(currentId), slot) : 0;
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

    uint8 actions = PENDING_AUTO_LEARN | PENDING_AUTO_EQUIP | PENDING_AUTO_ENCHANT;
    if (AutoTalentEnabled())
        actions |= PENDING_AUTO_TALENT;
    bool const deferred = RunOrDeferActions(player, actions,
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
        // Survives a logout in the middle of a fight (characters.extra_flags).
        player->SetPersistentAutoProgressionPending(true);
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
    player->SetPersistentAutoProgressionPending(false);

    // Talents first: talent abilities unlock trainer ranks and change the
    // spec-based gear weights. LearnTalent re-enters OnTalentLearned, which
    // queues a debounced follow-up; drop the parts this run already covers.
    uint32 talents = 0;
    if (actions & PENDING_AUTO_TALENT)
    {
        talents = SpendTalentPoints(player);
        uint8 const requeued = player->GetPendingAutoProgressionActions() & ~actions;
        player->ClearPendingAutoProgressionActions();
        if (requeued)
            player->AddPendingAutoProgressionActions(requeued);
        else
            player->SetAutoProgressionDelay(0);
    }
    learned = (actions & PENDING_AUTO_LEARN) ? LearnAvailableTrainerSpells(player) : 0;
    equipped = (actions & PENDING_AUTO_EQUIP) ? EquipBestItems(player) : 0;
    enchanted = (actions & PENDING_AUTO_ENCHANT) ? EnchantBestItems(player) : 0;
#ifdef MANGOS_DEBUG
    if (sLog.HasLogLevelOrHigher(LOG_LVL_DEBUG))
        sLog.Out(LOG_BASIC, LOG_LVL_DEBUG,
            "PlayerAutoProgression[debug]: run player=%s requested=0x%02X effective=0x%02X talents=%u learned=%u equipped=%u enchanted=%u duration=%u ms.",
            player->GetName(), uint32(requestedActions), uint32(actions),
            talents, learned, equipped, enchanted,
            WorldTimer::getMSTimeDiffToNow(debugStarted));
#else
    (void)talents;
#endif
    return false;
}

void OnLevelUp(Player* player)
{
    if (IsAuditExecution())
        return;
    if (!player || !player->GetSession() || !player->IsInWorld() ||
        !AutoProgressionAppliesTo(player))
        return;

    uint32 learned = 0;
    uint8 actions = 0;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_TALENT_ON_LEVEL_UP))
        actions |= PENDING_AUTO_TALENT;
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
            "PlayerAutoProgression: %s learned %u spells, equipped %u items and enchanted %u items at level %u.",
            player->GetName(), learned, equipped, enchanted, player->GetLevel());
    if (deferred)
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
            "PlayerAutoProgression: %s equipment update queued until equipment changes are allowed.",
            player->GetName());
}

void OnTalentLearned(Player* player)
{
    if (IsAuditExecution())
        return;
    if (!player || !player->GetSession() || !player->IsInWorld() ||
        !AutoProgressionAppliesTo(player))
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
    if (!player || !player->GetSession() || !player->IsInWorld() ||
        !AutoProgressionAppliesTo(player))
        return;
    uint8 actions = 0;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_TALENT_ON_LOGIN))
        actions |= PENDING_AUTO_TALENT;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_ON_LOGIN))
        actions |= PENDING_AUTO_LEARN;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_ON_LOGIN))
        actions |= PENDING_AUTO_EQUIP;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_ON_LOGIN))
        actions |= PENDING_AUTO_ENCHANT;
    // Resume an update that was deferred (combat, death) when the character
    // logged out; the exact action set is not stored, so run everything the
    // level-up trigger would have run. A brand-new character (no played time
    // yet) never saw a level-up either, so its level-1 state (warlock imp,
    // starting gear) is established the same way.
    if (player->HasPersistentAutoProgressionPending() ||
        player->GetTotalPlayedTime() == 0)
    {
        if (sWorld.getConfig(CONFIG_BOOL_AUTO_TALENT_ON_LEVEL_UP))
            actions |= PENDING_AUTO_TALENT;
        if (sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_ON_LEVEL_UP))
            actions |= PENDING_AUTO_LEARN;
        if (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_ON_LEVEL_UP))
            actions |= PENDING_AUTO_EQUIP;
        if (sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_ON_LEVEL_UP))
            actions |= PENDING_AUTO_ENCHANT;
        player->SetPersistentAutoProgressionPending(false);
    }
    player->AddPendingAutoProgressionActions(actions);
}

void OnTalentsReset(Player* player)
{
    if (!player || !player->GetSession() || !player->IsInWorld() ||
        !AutoProgressionAppliesTo(player))
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
bool IsAuditExecution()
{
    return gAuditExecutionActive;
}

uint32 GenerateAuditItemLowGuid()
{
    MANGOS_ASSERT(gAuditExecutionActive);
    MANGOS_ASSERT(gNextAuditItemLowGuid < 0xFFFFFFFEu);
    return gNextAuditItemLowGuid++;
}

bool StartAuditMatrix(char const* args, std::string& message)
{
    std::istringstream input(args ? args : "");
    std::string first;
    std::string second;
    std::string extra;
    input >> first;
    first = LowerAuditToken(first);
    if (first.empty())
    {
        message = "Usage: .autoprogression audit start all OR <class> <tree>.";
        return false;
    }

    std::vector<AuditProfile> profiles;
    if (first == "all")
    {
        if (input >> extra)
        {
            message = "The 'all' mode accepts no additional arguments.";
            return false;
        }
        AddAllAuditProfiles(profiles);
    }
    else
    {
        input >> second;
        second = LowerAuditToken(second);
        if (second.empty() || (input >> extra))
        {
            message = "Usage: .autoprogression audit start <class> <tree>.";
            return false;
        }
        uint8 const classId = ParseAuditClass(first);
        uint8 tree = 0;
        if (!classId || !ParseAuditTree(classId, second, tree))
        {
            message = "Unknown class/tree combination. Use English class and talent-tree names.";
            return false;
        }
        profiles.push_back({ classId, DefaultAuditRace(classId), tree });
    }

    if (sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL) != 60 ||
        sWorld.getConfig(CONFIG_UINT32_START_PLAYER_LEVEL) != 1 ||
        std::fabs(sWorld.getConfig(CONFIG_FLOAT_RATE_TALENT) - 1.0f) >
            0.001f)
    {
        message = "Audit requires MaxPlayerLevel=60, StartPlayerLevel=1 and Rate.Talent=1.";
        return false;
    }
    if (sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_MIN_LEVEL) != 1 ||
        !sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_CLASS_TRAINERS) ||
        !sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_WEAPON_TRAINERS) ||
        !sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_MAX_WEAPON_SKILLS) ||
        !sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_REPLACE_EXISTING) ||
        !sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_DELETE_REPLACED_ITEMS) ||
        !sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_REPLACE_EXISTING) ||
        sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_REQUIRE_DISCOVERED))
    {
        message = "Audit requires trainer/weapon learning, MaxWeaponSkills, AutoEquip.MinLevel=1, ReplaceExisting, DeleteReplacedItems, AutoEnchant.ReplaceExisting and RequireDiscovered=0.";
        return false;
    }

    std::lock_guard<std::mutex> lock(gAuditMatrixMutex);
    if (gAuditMatrix && !gAuditMatrix->finished.load())
    {
        message = "An auto-progression audit is already running.";
        return false;
    }

    std::shared_ptr<AuditMatrixState> state =
        std::make_shared<AuditMatrixState>();
    state->runId = uint64(std::time(nullptr)) * 1000ULL +
        uint64(gAuditRunSequence.fetch_add(1) % 1000);
    state->profiles = std::move(profiles);
    state->configHash = AuditConfigurationHash();
    state->cacheSnapshot = EnsureAutoProgressionCache();
    state->catalogHash = AuditCatalogHash(*state->cacheSnapshot);
    state->startedMs = WorldTimer::getMSTime();
    gAuditMatrix = state;

    std::ostringstream out;
    out << "Started auto-progression audit " << state->runId << " with " <<
        state->profiles.size() << " profile(s) and " <<
        state->profiles.size() * 60 << " level steps. Results use AP_AUDIT in Server.log.";
    message = out.str();
    return true;
}

void BuildAuditMatrixStatus(std::vector<std::string>& lines)
{
    lines.clear();
    std::shared_ptr<AuditMatrixState> state;
    {
        std::lock_guard<std::mutex> lock(gAuditMatrixMutex);
        state = gAuditMatrix;
    }
    if (!state)
    {
        lines.push_back("No auto-progression audit has been started.");
        return;
    }

    std::ostringstream out;
    if (state->finished.load())
    {
        out << "Audit " << state->runId << " finished: result=" <<
            state->result << ", reason=" <<
            (state->reason.empty() ? "none" : state->reason) <<
            ", levels=" << state->completedSteps.load() << ", errors=" <<
            state->errors.load() << ", hash=" << state->aggregateHash << ".";
        lines.push_back(out.str());
        return;
    }

    uint32 const profileIndex = state->currentProfile.load();
    out << "Audit " << state->runId << " running: profile " <<
        (profileIndex + 1) << "/" << state->profiles.size() << ", level " <<
        state->currentLevel.load() << "/60, completed " <<
        state->completedSteps.load() << "/" << state->profiles.size() * 60 <<
        ", errors=" << state->errors.load() << ".";
    lines.push_back(out.str());
    if (profileIndex < state->profiles.size())
    {
        AuditProfile const& profile = state->profiles[profileIndex];
        out.str("");
        out.clear();
        out << "Current profile: " << AuditClassName(profile.classId) << "/" <<
            AuditTreeName(profile.classId, profile.tree) << ", race " <<
            uint32(profile.raceId) << ".";
        lines.push_back(out.str());
    }
}

bool CancelAuditMatrix(std::string& message)
{
    std::shared_ptr<AuditMatrixState> state;
    {
        std::lock_guard<std::mutex> lock(gAuditMatrixMutex);
        state = gAuditMatrix;
    }
    if (!state || state->finished.load())
    {
        message = "No auto-progression audit is currently running.";
        return false;
    }
    state->cancelRequested.store(true);
    std::ostringstream out;
    out << "Cancellation requested for audit " << state->runId << ".";
    message = out.str();
    return true;
}

void UpdateAuditMatrix(uint32 diff)
{
    std::shared_ptr<AuditMatrixState> state;
    {
        std::lock_guard<std::mutex> lock(gAuditMatrixMutex);
        state = gAuditMatrix;
    }
    if (!state || state->finished.load())
        return;
    if (state->cancelRequested.load())
    {
        FinishAuditMatrix(*state, "cancelled", "operator_request");
        return;
    }

    // Every step runs a complete level (learn, equip, enchant) synchronously
    // on the world thread; the interval spreads that load out.
    state->delayMs += diff;
    if (state->delayMs < sWorld.getConfig(CONFIG_UINT32_AUTO_PROGRESSION_AUDIT_STEP_INTERVAL_MS))
        return;
    state->delayMs = 0;

    if (!state->beginLogged)
    {
        state->beginLogged = true;
        std::ostringstream begin;
        begin << "schema=1|run=" << state->runId <<
            "|event=matrix_begin|revision=" << REVISION_HASH <<
            "|patch=" << uint32(sWorld.GetWowPatch()) <<
            "|profiles=" << state->profiles.size() <<
            "|levels=" << state->profiles.size() * 60 <<
            "|mode=forced_core" <<
            "|config_hash=" << state->configHash <<
            "|cache_index_hash=" << state->catalogHash <<
            "|talent_policy=deepest_legal_target_then_next_tree_v1" <<
            "|quest_rewards=" <<
                sWorld.getConfig(
                    CONFIG_BOOL_AUTO_EQUIP_INCLUDE_QUEST_REWARDS) <<
            "|min_upgrade=" <<
                sWorld.getConfig(
                    CONFIG_FLOAT_AUTO_EQUIP_MIN_UPGRADE_PERCENT);
        WriteAuditLog(begin.str());
    }
    std::shared_ptr<AutoProgressionCache const> const currentCache =
        EnsureAutoProgressionCache();
    if (currentCache.get() != state->cacheSnapshot.get() ||
        AuditConfigurationHash() != state->configHash)
    {
        FinishAuditMatrix(*state, "failed", "environment_changed");
        return;
    }

    uint32 const profileIndex = state->currentProfile.load();
    if (profileIndex >= state->profiles.size())
    {
        FinishAuditMatrix(*state,
            state->errors.load() ? "completed_with_errors" : "success", "");
        return;
    }

    AuditProfile const& profile = state->profiles[profileIndex];
    std::string error;
    if (!state->player && !CreateAuditProfile(*state, profile, error))
    {
        FinishAuditMatrix(*state, "failed", error);
        return;
    }
    if (!ProcessAuditLevel(*state, profile, error))
    {
        uint32 const failedLevel = state->currentLevel.load() ?
            state->currentLevel.load() + 1 : 1;
        AuditViolation(*state, profile, failedLevel, "profile_aborted", error);
        MixAuditHash(state->profileHash, 0xFFFFFFFFu);
        MixAuditHash(state->profileHash, failedLevel);
        MixAuditHash(state->aggregateHash, uint32(state->profileHash));
        MixAuditHash(state->aggregateHash,
            uint32(state->profileHash >> 32));
        std::ostringstream end;
        end << "schema=1|run=" << state->runId <<
            "|event=profile_end|profile=" << profileIndex <<
            "|class=" << AuditClassName(profile.classId) <<
            "|tree=" << AuditTreeName(profile.classId, profile.tree) <<
            "|result=aborted|level=" << failedLevel <<
            "|errors=" << (state->errors.load() -
                state->profileErrorStart) <<
            "|reason=" << error << "|hash=" << state->profileHash;
        WriteAuditLog(end.str());
        DestroyAuditProfile(*state);
        state->currentProfile.fetch_add(1);
        state->currentLevel.store(0);
        return;
    }
    if (state->currentProfile.load() >= state->profiles.size())
        FinishAuditMatrix(*state,
            state->errors.load() ? "completed_with_errors" : "success", "");
}

void ShutdownAuditMatrix()
{
    std::shared_ptr<AuditMatrixState> state;
    {
        std::lock_guard<std::mutex> lock(gAuditMatrixMutex);
        state = gAuditMatrix;
    }
    if (state && !state->finished.load())
        FinishAuditMatrix(*state, "cancelled", "server_shutdown");
}
}

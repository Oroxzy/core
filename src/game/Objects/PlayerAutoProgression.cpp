#include "PlayerAutoProgression.h"

#include "Creature.h"
#include "DBCStores.h"
#include "Item.h"
#include "LFGMgr.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "SpellMgr.h"
#include "World.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
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
enum PendingAutoProgressionAction : uint8
{
    PENDING_AUTO_EQUIP = 0x01,
    PENDING_AUTO_ENCHANT = 0x02
};


struct EnchantCandidate
{
    uint32 spellId = 0;
    uint32 enchantId = 0;
    uint32 sourceItemId = 0;
    ImprovementSource source = ImprovementSource::None;
    uint8 effectIndex = 0;
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

bool ImprovementFits(Player* player, Item* item, EnchantCandidate const& candidate)
{
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(candidate.spellId);
    if (!spell || candidate.effectIndex >= MAX_EFFECT_INDEX ||
        spell->Effect[candidate.effectIndex] != SPELL_EFFECT_ENCHANT_ITEM ||
        uint32(spell->EffectMiscValue[candidate.effectIndex]) != candidate.enchantId ||
        !sSpellItemEnchantmentStore.LookupEntry(candidate.enchantId) ||
        !(spell->Targets & TARGET_FLAG_ITEM) ||
        !ImprovementSourceEnabled(candidate.source) ||
        sObjectMgr.IsSpellDisabled(candidate.spellId) ||
        !ImprovementSourceAvailable(player, candidate))
        return false;
    if (item->GetProto()->ItemLevel < spell->baseLevel ||
        !item->IsFitToSpellRequirements(spell))
        return false;
    if (spell->HasAttribute(SPELL_ATTR_HELD_ITEM_ONLY) &&
        item->GetSlot() != EQUIPMENT_SLOT_MAINHAND)
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

void BuildTrainerCache(TrainerCache& cache)
{
    for (auto const& entry : sObjectMgr.GetCreatureInfoMap())
    {
        CreatureInfo const* info = entry.second.get();
        if (!info || !(info->npc_flags & UNIT_NPC_FLAG_TRAINER))
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
    if (value <= 0)
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
    if (value <= 0)
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

float ScoreAura(Weights const& w, SpellEntry const* spell, float trigger, uint8 depth = 0)
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

        switch (aura)
        {
            case SPELL_AURA_MOD_ATTACKSPEED:
            case SPELL_AURA_MOD_MELEE_HASTE:
                if (positive && rawValue > 0)
                    score += value * std::max(w.meleeCrit * 0.7f, w.weaponDps * 0.5f) * SecondaryScale();
                else if (!positive && rawValue < 0)
                    score += value * (0.20f + w.sta * 0.08f) * SurvivalScale();
                continue;
            case SPELL_AURA_MOD_DECREASE_SPEED:
                if (!positive && rawValue < 0)
                    score += value * (0.10f + w.sta * 0.03f) * SurvivalScale();
                continue;
            case SPELL_AURA_MOD_RANGED_HASTE:
                if (positive && rawValue > 0)
                    score += value * w.rangedDps * 0.5f * SecondaryScale();
                else if (!positive && rawValue < 0)
                    score += value * (0.12f + w.sta * 0.04f) * SurvivalScale();
                continue;
            case SPELL_AURA_MOD_INCREASE_SPEED:
                score += value * (positive ? 0.6f : 0.15f) * SecondaryScale();
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
                    score += value * (0.12f + w.sta * 0.05f) * SurvivalScale();
                    continue;
                }
                break;
            default:
                break;
        }

        if (rawValue <= 0)
            continue;

        switch (aura)
        {
            case SPELL_AURA_MOD_DAMAGE_DONE:
                score += rawValue * w.spell *
                    SpellSchoolScale(w, uint32(spell->EffectMiscValue[i])) * SpellScale(); break;
            case SPELL_AURA_MOD_DAMAGE_DONE_CREATURE:
                score += rawValue * (w.spell + w.weaponDps / 13.0f) * 0.10f * SecondaryScale(); break;
            case SPELL_AURA_MOD_HEALING_DONE: score += rawValue * w.healing * SpellScale(); break;
            case SPELL_AURA_MOD_HEALING_DONE_PERCENT:
                score += rawValue * w.healing * 4.0f * SpellScale(); break;
            case SPELL_AURA_MOD_ATTACK_POWER: score += rawValue * w.ap * PrimaryScale(); break;
            case SPELL_AURA_MOD_RANGED_ATTACK_POWER: score += rawValue * w.rap * PrimaryScale(); break;
            case SPELL_AURA_MOD_HIT_CHANCE: score += rawValue * w.meleeHit * SecondaryScale(); break;
            case SPELL_AURA_MOD_SPELL_HIT_CHANCE: score += rawValue * w.spellHit * SecondaryScale(); break;
            case SPELL_AURA_MOD_CRIT_PERCENT: score += rawValue * w.meleeCrit * SecondaryScale(); break;
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
                score += rawValue * w.spellCrit * SecondaryScale(); break;
            case SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL:
                score += rawValue * w.spellCrit *
                    SpellSchoolScale(w, uint32(spell->EffectMiscValue[i])) * SecondaryScale(); break;
            case SPELL_AURA_MOD_DODGE_PERCENT: score += rawValue * w.dodge * SurvivalScale(); break;
            case SPELL_AURA_MOD_PARRY_PERCENT: score += rawValue * w.parry * SurvivalScale(); break;
            case SPELL_AURA_MOD_BLOCK_PERCENT: score += rawValue * w.block * SurvivalScale(); break;
            case SPELL_AURA_MOD_SHIELD_BLOCKVALUE:
                score += rawValue * w.blockValue * SurvivalScale(); break;
            case SPELL_AURA_MOD_SHIELD_BLOCKVALUE_PCT:
                score += rawValue * w.block * 0.6f * SurvivalScale(); break;
            case SPELL_AURA_MOD_RESISTANCE:
            case SPELL_AURA_MOD_RESISTANCE_EXCLUSIVE:
                score += ScoreResistance(w, rawValue, uint32(spell->EffectMiscValue[i])); break;
            case SPELL_AURA_MOD_POWER_REGEN:
            case SPELL_AURA_MOD_MANA_REGEN_INTERRUPT:
                score += rawValue * w.mp5 * SecondaryScale(); break;
            case SPELL_AURA_MOD_INCREASE_HEALTH:
                score += ScoreItemMod(w, ITEM_MOD_HEALTH, rawValue); break;
            case SPELL_AURA_MOD_INCREASE_ENERGY:
                if (spell->EffectMiscValue[i] == POWER_MANA)
                    score += ScoreItemMod(w, ITEM_MOD_MANA, rawValue);
                break;
            case SPELL_AURA_SCHOOL_ABSORB:
                score += rawValue * (0.30f + w.sta * 0.08f) * SurvivalScale(); break;
            case SPELL_AURA_MOD_SKILL:
            case SPELL_AURA_MOD_SKILL_TALENT:
                if (spell->EffectMiscValue[i] == SKILL_DEFENSE)
                    score += rawValue * w.defense * SurvivalScale();
                else if (IsWeaponSkill(uint32(spell->EffectMiscValue[i])))
                    score += rawValue * w.weaponSkill * SecondaryScale();
                break;
            case SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE:
                score += rawValue * ((w.str + w.agi + w.intl) * PrimaryScale() +
                    w.sta * SurvivalScale() + w.spi * SecondaryScale()) * 0.6f; break;
            case SPELL_AURA_MOD_STAT:
                if (spell->EffectMiscValue[i] < 0)
                    score += rawValue * ((w.str + w.agi + w.intl) * PrimaryScale() +
                        w.sta * SurvivalScale() + w.spi * SecondaryScale());
                else
                    score += ScoreItemMod(w, uint32(spell->EffectMiscValue[i] == STAT_STRENGTH ? ITEM_MOD_STRENGTH :
                        spell->EffectMiscValue[i] == STAT_AGILITY ? ITEM_MOD_AGILITY :
                        spell->EffectMiscValue[i] == STAT_STAMINA ? ITEM_MOD_STAMINA :
                        spell->EffectMiscValue[i] == STAT_INTELLECT ? ITEM_MOD_INTELLECT :
                        spell->EffectMiscValue[i] == STAT_SPIRIT ? ITEM_MOD_SPIRIT : MAX_ITEM_MOD), rawValue);
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
float EnchantProcPpm(Item const* item, SpellEntry const* procSpell, uint32 chance)
{
    float const configured = procSpell ? sSpellMgr.GetItemEnchantProcChance(procSpell->Id) : 0;
    if (configured > 0)
        return configured;

    uint32 const delay = item && item->GetProto()->Delay ? item->GetProto()->Delay : 0;
    if (chance && delay)
        return float(chance) * 600.0f / float(delay);
    return 1.0f;
}

float ScoreProcPayload(Weights const& w, Item const* item, SpellEntry const* spell, float ppm, uint8 depth)
{
    if (!spell || ppm <= 0 || depth > 2)
        return 0;

    int32 const duration = spell->GetDuration();
    float const uptime = duration > 0 ?
        1.0f - std::exp(-ppm * float(duration) / 60000.0f) :
        std::min(1.0f, ppm / 60.0f);
    float score = ScoreAura(w, spell, uptime);

    float throughput = item->GetProto()->IsRangedWeapon() ? w.rangedDps : w.weaponDps;
    if (throughput <= 0)
        throughput = std::max(0.10f, w.spell * 0.35f);

    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
    {
        float const value = float(std::max<int32>(
            spell->CalculateSimpleValue(SpellEffectIndex(i)), 0));
        float const perSecond = value * ppm / 60.0f;

        switch (spell->Effect[i])
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                score += perSecond * throughput * WeaponScale();
                break;
            case SPELL_EFFECT_HEALTH_LEECH:
                score += perSecond * throughput * WeaponScale();
                score += perSecond * (0.5f + w.sta * 0.1f) * SurvivalScale();
                break;
            case SPELL_EFFECT_HEAL:
            case SPELL_EFFECT_HEAL_MAX_HEALTH:
                score += perSecond * (w.healer ? w.healing * 0.25f : 0.5f + w.sta * 0.1f) *
                    SurvivalScale();
                break;
            case SPELL_EFFECT_ENERGIZE:
                score += perSecond * w.mp5 * 0.2f * SecondaryScale();
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

float ScoreEnchantment(Weights const& w, Item const* item, SpellItemEnchantmentEntry const* enchant)
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
                if (item->GetProto()->Delay)
                    score += amount * 1000.0f / float(item->GetProto()->Delay) *
                        (item->GetProto()->IsRangedWeapon() ? w.rangedDps : w.weaponDps) * WeaponScale();
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

float ScoreItem(Weights const& w, ItemPrototype const* item)
{
    float score = item->ItemLevel * 0.025f + item->Quality * 0.2f;
    for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        float const value = float(std::max<int32>(item->ItemStat[i].ItemStatValue, 0));
        switch (item->ItemStat[i].ItemStatType)
        {
            case ITEM_MOD_STRENGTH: score += value * w.str * PrimaryScale(); break;
            case ITEM_MOD_AGILITY: score += value * w.agi * PrimaryScale(); break;
            case ITEM_MOD_STAMINA: score += value * w.sta * SurvivalScale(); break;
            case ITEM_MOD_INTELLECT: score += value * w.intl * PrimaryScale(); break;
            case ITEM_MOD_SPIRIT: score += value * w.spi * SecondaryScale(); break;
            case ITEM_MOD_HEALTH: score += value * 0.1f * w.sta * SurvivalScale(); break;
            case ITEM_MOD_MANA: score += value / 15.0f * w.intl * PrimaryScale(); break;
            default: break;
        }
    }
    score += std::max<int32>(item->Armor, 0) * w.armor * SurvivalScale();
    score += item->Block * w.blockValue * SurvivalScale();

    float const dps = WeaponDps(item);
    score += dps * (item->IsRangedWeapon() ? w.rangedDps : w.weaponDps) * WeaponScale();

    for (uint8 i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        float trigger = 0;
        switch (item->Spells[i].SpellTrigger)
        {
            case ITEM_SPELLTRIGGER_ON_EQUIP: trigger = 1.0f; break;
            case ITEM_SPELLTRIGGER_ON_USE: trigger = 0.18f; break;
            case ITEM_SPELLTRIGGER_CHANCE_ON_HIT: trigger = 0.30f; break;
            default: break;
        }
        if (item->Spells[i].SpellId && trigger > 0)
            score += ScoreAura(w, sSpellMgr.GetSpellEntry(item->Spells[i].SpellId), trigger);
    }
    return score;
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
    std::set<uint32> itemIds;
    std::map<uint32, uint32> minimumLevelForPlayer;
};

QuestRewardCatalog BuildQuestRewardCatalog(Player const* player)
{
    QuestRewardCatalog catalog;
    for (auto const& entry : sObjectMgr.GetQuestTemplates())
    {
        Quest const* quest = entry.second.get();
        if (!quest || !sObjectMgr.IsQuestTemplateLoaded(entry.first))
            continue;

        bool const availableToPlayer = quest->IsActive() &&
            (!quest->GetRequiredRaces() || (quest->GetRequiredRaces() & player->GetRaceMask())) &&
            (!quest->GetRequiredClasses() || (quest->GetRequiredClasses() & player->GetClassMask()));

        auto addReward = [&](uint32 itemId)
        {
            if (!itemId)
                return;

            catalog.itemIds.insert(itemId);
            if (!availableToPlayer)
                return;

            auto const itr = catalog.minimumLevelForPlayer.find(itemId);
            if (itr == catalog.minimumLevelForPlayer.end() || quest->GetMinLevel() < itr->second)
                catalog.minimumLevelForPlayer[itemId] = quest->GetMinLevel();
        };

        for (uint32 itemId : quest->RewChoiceItemId)
            addReward(itemId);
        for (uint32 itemId : quest->RewItemId)
            addReward(itemId);
    }
    return catalog;
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
}

namespace PlayerAutoProgression
{
uint32 LearnAvailableTrainerSpells(Player* player)
{
    if (!player || !player->IsInWorld())
        return 0;
    TrainerCache trainerCache;
    BuildTrainerCache(trainerCache);

    uint32 learned = 0;
    for (uint8 pass = 0; pass < 20; ++pass)
    {
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
    return learned;
}

uint32 EquipBestItems(Player* player)
{
    if (!player || !player->IsInWorld() ||
        player->GetLevel() < sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_MIN_LEVEL))
        return 0;

    Weights const weights = GetWeights(player);
    std::array<std::vector<Candidate>, EQUIPMENT_SLOT_END> candidates;
    uint32 const maxQuality = sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_MAX_QUALITY);
    uint32 const levelBonus = sWorld.getConfig(CONFIG_UINT32_AUTO_EQUIP_MAX_ITEM_LEVEL_BONUS);
    bool const discovered = sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_REQUIRE_DISCOVERED);
    bool const includeQuestRewards = sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_INCLUDE_QUEST_REWARDS);
    QuestRewardCatalog const questRewards = BuildQuestRewardCatalog(player);

    for (auto const& entry : sObjectMgr.GetItemPrototypeMap())
    {
        ItemPrototype const* item = &entry.second;
        if (item->Class != ITEM_CLASS_WEAPON && item->Class != ITEM_CLASS_ARMOR)
            continue;
        if (!item->InventoryType || item->InventoryType == INVTYPE_BODY || item->InventoryType == INVTYPE_TABARD ||
            item->InventoryType == INVTYPE_BAG || item->InventoryType == INVTYPE_AMMO ||
            item->InventoryType == INVTYPE_QUIVER)
            continue;
        if (item->HasExtraFlag(ITEM_EXTRA_NOT_OBTAINABLE) || item->Quality > maxQuality ||
            item->Duration || item->RandomProperty || (discovered && !item->Discovered))
            continue;

        bool const isQuestReward = questRewards.itemIds.count(item->ItemId) != 0;
        if (isQuestReward)
        {
            if (!includeQuestRewards)
                continue;

            auto const questLevel = questRewards.minimumLevelForPlayer.find(item->ItemId);
            if (questLevel == questRewards.minimumLevelForPlayer.end() ||
                questLevel->second > player->GetLevel())
                continue;
        }
        if (item->RequiredLevel > player->GetLevel() ||
            (!item->RequiredLevel && item->ItemLevel > player->GetLevel() + levelBonus))
            continue;
        if (item->RequiredHonorRank || item->RequiredCityRank)
            continue;
        if (item->RequiredReputationFaction &&
            uint32(player->GetReputationRank(item->RequiredReputationFaction)) < item->RequiredReputationRank)
            continue;
        if (player->CanUseItem(item, false) != EQUIP_ERR_OK)
            continue;

        float const score = ScoreItem(weights, item);
        uint8 slots[4] = { NULL_SLOT, NULL_SLOT, NULL_SLOT, NULL_SLOT };
        item->GetAllowedEquipSlots(slots, player->GetClass(), player->CanDualWield());
        for (uint8 slot : slots)
            if (slot < EQUIPMENT_SLOT_END && SlotAllowed(weights, item, slot))
                candidates[slot].push_back({ item, score });
    }

    for (auto& list : candidates)
        std::sort(list.begin(), list.end(), Better);

    std::array<ItemPrototype const*, EQUIPMENT_SLOT_END> plan = {};
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        if (slot != EQUIPMENT_SLOT_MAINHAND && slot != EQUIPMENT_SLOT_OFFHAND && !candidates[slot].empty())
            plan[slot] = candidates[slot][0].item;

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
            if (candidate.item->ItemId != plan[pair[0]]->ItemId)
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
            if (mainScore > best)
            {
                best = mainScore; plan[EQUIPMENT_SLOT_MAINHAND] = main.item; plan[EQUIPMENT_SLOT_OFFHAND] = nullptr;
            }
            continue;
        }

        bool foundOffhand = false;
        for (size_t oi = 0; oi < offLimit; ++oi)
        {
            Candidate const& off = candidates[EQUIPMENT_SLOT_OFFHAND][oi];
            if (!Compatible(player, weights, main.item, off.item) ||
                (main.item->ItemId == off.item->ItemId && Unique(main.item)))
                continue;
            foundOffhand = true;
            float total = mainScore + off.score * (off.item->Class == ITEM_CLASS_WEAPON ? 0.70f : 1.0f);
            if (weights.dualWield && off.item->Class == ITEM_CLASS_WEAPON)
                total += 8.0f;
            if (total > best)
            {
                best = total; plan[EQUIPMENT_SLOT_MAINHAND] = main.item; plan[EQUIPMENT_SLOT_OFFHAND] = off.item;
            }
        }
        if (!foundOffhand && Compatible(player, weights, main.item, nullptr) && mainScore > best)
        {
            best = mainScore; plan[EQUIPMENT_SLOT_MAINHAND] = main.item; plan[EQUIPMENT_SLOT_OFFHAND] = nullptr;
        }
    }

    bool const replace = sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_REPLACE_EXISTING);
    bool const deleteReplaced = sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_DELETE_REPLACED_ITEMS);
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
    return equipped;
}

uint32 EnchantBestItems(Player* player)
{
    if (!player || !player->IsInWorld())
        return 0;

    std::vector<EnchantCandidate> const candidates = BuildImprovementCandidates();
    Weights const weights = GetWeights(player);
    bool const replace = sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_REPLACE_EXISTING);
    uint32 enchanted = 0;

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        EnchantCandidate const* best = nullptr;
        float bestScore = -std::numeric_limits<float>::max();
        uint32 bestLevel = 0;
        for (EnchantCandidate const& candidate : candidates)
        {
            if (!ImprovementFits(player, item, candidate))
                continue;

            SpellItemEnchantmentEntry const* enchant =
                sSpellItemEnchantmentStore.LookupEntry(candidate.enchantId);
            float const score = ScoreEnchantment(weights, item, enchant);
            uint32 const level = ImprovementMinimumLevel(candidate);
            if (!best || score > bestScore + 0.001f ||
                (std::fabs(score - bestScore) <= 0.001f &&
                 (level > bestLevel ||
                  (level == bestLevel && candidate.spellId > best->spellId))))
            {
                best = &candidate;
                bestScore = score;
                bestLevel = level;
            }
        }

        if (!best)
            continue;

        uint32 const bestEnchantId = best->enchantId;
        uint32 const currentEnchantId = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
        if (currentEnchantId == bestEnchantId || (currentEnchantId && !replace))
            continue;

        float currentScore = 0;
        if (currentEnchantId)
            currentScore = ScoreEnchantment(weights, item,
                sSpellItemEnchantmentStore.LookupEntry(currentEnchantId));
        if (currentEnchantId && bestScore <= currentScore + 0.001f)
            continue;

        player->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, false);
        item->SetEnchantment(PERM_ENCHANTMENT_SLOT, bestEnchantId, 0, 0,
            player->GetObjectGuid());
        player->ApplyEnchantment(item, PERM_ENCHANTMENT_SLOT, true);
        ++enchanted;
    }
    return enchanted;
}

bool RunOrDeferEquipmentActions(Player* player, uint8 requestedActions,
    uint32& equipped, uint32& enchanted)
{
    uint8 const actions = requestedActions | player->GetPendingAutoProgressionActions();
    if (!actions)
        return false;

    bool const equipmentBlocked = (actions & PENDING_AUTO_EQUIP) &&
        (player->IsInCombat() || player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_PENDING_STUNNED) ||
         player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISARMED));
    if (!player->IsAlive() || equipmentBlocked)
    {
        player->AddPendingAutoProgressionActions(requestedActions);
        return true;
    }

    player->ClearPendingAutoProgressionActions();
    equipped = (actions & PENDING_AUTO_EQUIP) ? EquipBestItems(player) : 0;
    enchanted = (actions & PENDING_AUTO_ENCHANT) ? EnchantBestItems(player) : 0;
    return false;
}

void OnLevelUp(Player* player)
{
    if (!player || !player->GetSession() || !player->IsInWorld())
        return;

    uint32 const learned = sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_ON_LEVEL_UP) ?
        LearnAvailableTrainerSpells(player) : 0;
    uint8 actions = 0;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_ON_LEVEL_UP))
        actions |= PENDING_AUTO_EQUIP;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_ON_LEVEL_UP))
        actions |= PENDING_AUTO_ENCHANT;

    uint32 equipped = 0;
    uint32 enchanted = 0;
    bool const deferred = RunOrDeferEquipmentActions(player, actions, equipped, enchanted);
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

    uint32 const learned = sWorld.getConfig(CONFIG_BOOL_AUTO_LEARN_ON_TALENT_LEARN) ?
        LearnAvailableTrainerSpells(player) : 0;
    uint8 actions = 0;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_EQUIP_ON_TALENT_LEARN))
        actions |= PENDING_AUTO_EQUIP;
    if (sWorld.getConfig(CONFIG_BOOL_AUTO_ENCHANT_ON_TALENT_LEARN))
        actions |= PENDING_AUTO_ENCHANT;

    uint32 equipped = 0;
    uint32 enchanted = 0;
    bool const deferred = RunOrDeferEquipmentActions(player, actions, equipped, enchanted);
    if (learned || equipped || enchanted)
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
            "PlayerAutoProgression: %s learned %u trainer spells, equipped %u items and enchanted %u items after a talent change.",
            player->GetName(), learned, equipped, enchanted);
    if (deferred)
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
            "PlayerAutoProgression: %s talent equipment update queued until equipment changes are allowed.",
            player->GetName());
}

void OnPlayerUpdate(Player* player)
{
    if (!player || !player->GetSession() || !player->IsInWorld() ||
        !player->IsAlive() || player->IsInCombat() ||
        player->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_PENDING_STUNNED) ||
        player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISARMED) ||
        !player->GetPendingAutoProgressionActions())
        return;

    uint32 equipped = 0;
    uint32 enchanted = 0;
    RunOrDeferEquipmentActions(player, 0, equipped, enchanted);
    if (equipped || enchanted)
        sLog.Out(LOG_BASIC, LOG_LVL_DETAIL,
            "PlayerAutoProgression: %s completed deferred update: equipped %u items and enchanted %u items.",
            player->GetName(), equipped, enchanted);
}
}

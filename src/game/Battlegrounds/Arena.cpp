/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Custom arena implementation for the 1.12 client, see Arena.h.
 */

#include "Arena.h"
#include "BattleGroundMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Creature.h"
#include "GameObject.h"
#include "ObjectMgr.h"
#include "Chat.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "World.h"
#include "Map.h"
#include "GridMap.h"
#include "Database/DatabaseEnv.h"
#include "ProgressBar.h"
#include "Utilities/Random.h"
#include "Policies/SingletonImp.h"

#include <sstream>
#include <algorithm>
#include <map>
#include <set>

INSTANTIATE_SINGLETON_1(ArenaMgr);

/*********************************************************/
/***                    LANGUAGES                      ***/
/*********************************************************/

/*
 * The arena writes no English anywhere. Every line it puts in front of a player is a `mangos_string`
 * with all nine columns filled (section 23 of sql/arena/world_arena.sql, generated from
 * tools/locales/arena_locales.tsv of the client patch), and these three are how the code reaches it.
 *
 * Which column is which is worth spelling out, because the two enums in Common.h disagree:
 * `content_loc<i>` is `LocaleConstant(i)`, so 1 koKR, 2 frFR, 3 deDE, 4 zhCN, 5 zhTW, 6 esES,
 * 7 esMX, 8 ruRU. DBLocaleConstant numbers the first three the other way round and belongs to a
 * different index. ObjectMgr::GetMangosString takes the DB index, which the session already holds.
 */
char const* ArenaMgr::Text(Player const* player, int32 entry)
{
    if (player && player->GetSession())
        return player->GetSession()->GetMangosString(entry);

    return sObjectMgr.GetMangosString(entry, DB_LOCALE_enUS);
}

std::string ArenaMgr::Textf(Player const* player, int32 entry, ...)
{
    char const* format = Text(player, entry);
    if (!format)
        return std::string();

    char buffer[2048];
    va_list ap;
    va_start(ap, entry);
    vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    return std::string(buffer);
}

char const* ArenaMgr::BracketName(Player const* player, ArenaType type)
{
    // 1v1 is entry+0, 2v2 entry+1, 3v3 entry+2, 5v5 entry+3 - the same order as GetArenaTypeIndex
    return Text(player, LANG_ARENA_BRACKET_FIRST + GetArenaTypeIndex(type));
}

char const* ArenaMgr::ClassName(Player const* reader, uint8 playerClass)
{
    // The class ids are not consecutive - 6 and 10 are the holes the death knight and the second
    // unused slot leave - so the nine strings are picked one by one rather than by arithmetic.
    switch (playerClass)
    {
        case CLASS_WARRIOR: return Text(reader, LANG_ARENA_CLASS_FIRST + 0);
        case CLASS_PALADIN: return Text(reader, LANG_ARENA_CLASS_FIRST + 1);
        case CLASS_HUNTER:  return Text(reader, LANG_ARENA_CLASS_FIRST + 2);
        case CLASS_ROGUE:   return Text(reader, LANG_ARENA_CLASS_FIRST + 3);
        case CLASS_PRIEST:  return Text(reader, LANG_ARENA_CLASS_FIRST + 4);
        case CLASS_SHAMAN:  return Text(reader, LANG_ARENA_CLASS_FIRST + 5);
        case CLASS_MAGE:    return Text(reader, LANG_ARENA_CLASS_FIRST + 6);
        case CLASS_WARLOCK: return Text(reader, LANG_ARENA_CLASS_FIRST + 7);
        case CLASS_DRUID:   return Text(reader, LANG_ARENA_CLASS_FIRST + 8);
    }
    return "";
}

/*********************************************************/
/***                     ARENA MGR                     ***/
/*********************************************************/

void ArenaMgr::LoadFromDB()
{
    m_disabledSpells.clear();
    m_itemMinPatch.clear();
    m_tempEnchantSpells.clear();

    if (!sWorld.getConfig(CONFIG_BOOL_ARENA_ENABLED))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Arenas are disabled (Arena.Enable = 0), skipping arena data.");
        return;
    }

    {
        //                                                               0        1      2      3      4
        std::unique_ptr<QueryResult> result(WorldDatabase.Query("SELECT `entry`, `1v1`, `2v2`, `3v3`, `5v5` FROM `disabled_arena_spells`"));
        uint32 count = 0;
        if (result)
        {
            BarGoLink bar(result->GetRowCount());
            do
            {
                bar.step();
                Field* fields = result->Fetch();

                uint32 spellId = fields[0].GetUInt32();
                SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
                if (!spell)
                {
                    sLog.Out(LOG_DBERROR, LOG_LVL_MINIMAL, "Table `disabled_arena_spells` has nonexistent spell %u, skipped.", spellId);
                    continue;
                }

                DisabledSpell& data = m_disabledSpells[spellId];
                for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
                    data.disabledForType[i] = fields[1 + i].GetUInt8() != 0;
                ++count;

                // A weapon oil or a sharpening stone is used OUTSIDE and leaves a temporary
                // enchantment behind, so refusing the spell inside the arena does nothing - the buff
                // walked in on the weapon. Remember which enchantment each forbidden spell applies,
                // so PrepareArenaPlayer can take it off at the door.
                //
                // Rogue poisons are applied the same way and are deliberately never collected here:
                // they are part of the class, not a consumable buff. None of them is in the table
                // today, and this keeps it that way even if one is added by mistake.
                if (spell->SpellFamilyName != SPELLFAMILY_ROGUE)
                {
                    for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
                        if (spell->Effect[i] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY && spell->EffectMiscValue[i] > 0)
                            m_tempEnchantSpells[uint32(spell->EffectMiscValue[i])] = spellId;
                }
            }
            while (result->NextRow());
        }
        else
        {
            BarGoLink bar(1);
            bar.step();
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded %u disabled arena spells", count);
    }

    // The patch in which an item was introduced. item_template holds one row per patch in which the
    // item changed, so the lowest patch is the one we want. Items available since 1.2 (patch 0) are not stored.
    {
        // HAVING drops the items introduced in 1.2, which is most of them: 17707 grouped rows come
        // back without it and only about 4500 are kept, so three quarters of the fetch and of the
        // progress bar were wasted.
        std::unique_ptr<QueryResult> result(WorldDatabase.Query("SELECT `entry`, MIN(`patch`) FROM `item_template` GROUP BY `entry` HAVING MIN(`patch`) > 0"));
        if (result)
        {
            BarGoLink bar(result->GetRowCount());
            do
            {
                bar.step();
                Field* fields = result->Fetch();
                m_itemMinPatch[fields[0].GetUInt32()] = fields[1].GetUInt8();
            }
            while (result->NextRow());
        }
        else
        {
            BarGoLink bar(1);
            bar.step();
        }
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> Loaded arena item patch data for " SIZEFMTD " items added after patch 1.2", m_itemMinPatch.size());
    }

    // which item each banned spell belongs to, for the ban list in the admin panel
    BuildSpellItemMap();
}

bool ArenaMgr::IsSpellDisabled(uint32 spellId, ArenaType type, bool fromItem) const
{
    if (type == ARENA_TYPE_NONE)
        return false;

    auto itr = m_disabledSpells.find(spellId);
    if (itr == m_disabledSpells.end())
        return false;

    if (!itr->second.disabledForType[GetArenaTypeIndex(type)])
        return false;

    // the class keeps its own spell, the trinket that borrowed the id does not
    return fromItem || m_itemOnlySpells.find(spellId) == m_itemOnlySpells.end();
}

void ArenaMgr::GetItemSpells(ItemPrototype const* proto, std::vector<uint32>& out)
{
    if (!proto)
        return;

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        if (proto->Spells[i].SpellId)
            out.push_back(proto->Spells[i].SpellId);
}

uint32 ArenaMgr::GetItemForSpell(uint32 spellId) const
{
    auto itr = m_spellItem.find(spellId);
    return itr != m_spellItem.end() ? itr->second : 0;
}

/*
 * Which item a banned spell belongs to. Most of the ban list is items - the table can only hold
 * spells, because a spell is what the cast check has to refuse - so without this the list reads as
 * a wall of spell names where an admin is looking for a potion.
 *
 * Built by walking the item prototypes once, after the ban list is loaded, and only remembering the
 * spells that are actually banned. The first item wins: several items share one spell (every rank of
 * a potion of the same kind), and for naming the row any of them does.
 */
void ArenaMgr::BuildSpellItemMap()
{
    m_spellItem.clear();
    m_itemOnlySpells.clear();
    if (m_disabledSpells.empty())
        return;

    for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
    {
        for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
        {
            uint32 const spellId = itr.second.Spells[i].SpellId;
            if (!spellId || m_disabledSpells.find(spellId) == m_disabledSpells.end())
                continue;
            if (m_spellItem.find(spellId) == m_spellItem.end())
                m_spellItem[spellId] = itr.second.ItemId;
        }
    }

    /*
     * Which of them a player owns in his own right. A spell that carries a RANK is a rung of a class
     * spell's ladder - Blizzard does not rank the effects it writes for a trinket - so those are the
     * ones an item merely borrowed, and banning the item must not take the ability away as well.
     *
     * Casting it without an item is then always allowed, which is why this stays narrow: a spell no
     * class can cast is never reached that way, so nothing is loosened by including it, but a spell
     * with no rank is left alone rather than guessed about.
     */
    for (auto const& itr : m_spellItem)
        if (sSpellMgr.GetSpellRank(itr.first))
            m_itemOnlySpells.insert(itr.first);

    if (!m_itemOnlySpells.empty())
        sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, ">> " SIZEFMTD " banned arena spells are class abilities an item borrowed - refused only when an item casts them", m_itemOnlySpells.size());
}

/*
 * Bans or unbans one spell, in memory and in the table, so a change made from the admin panel is
 * the same change somebody would have made in `disabled_arena_spells` by hand.
 *
 * The temporary enchantment map is kept in step here rather than by reloading everything: a ban is
 * one row, and LoadFromDB would go through the whole item table for the patch data as well.
 */
bool ArenaMgr::SetSpellDisabled(uint32 spellId, bool const perType[ARENA_TYPES_COUNT])
{
    SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId);
    if (!spell)
        return false;

    bool anyType = false;
    for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
        anyType = anyType || perType[i];

    // the enchantments this spell applies, so a weapon oil can be taken off at the door
    std::vector<uint32> enchants;
    if (spell->SpellFamilyName != SPELLFAMILY_ROGUE)
        for (uint8 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (spell->Effect[i] == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY && spell->EffectMiscValue[i] > 0)
                enchants.push_back(uint32(spell->EffectMiscValue[i]));

    if (anyType)
    {
        DisabledSpell& data = m_disabledSpells[spellId];
        for (uint8 i = 0; i < ARENA_TYPES_COUNT; ++i)
            data.disabledForType[i] = perType[i];

        for (uint32 enchantId : enchants)
            m_tempEnchantSpells[enchantId] = spellId;

        // remember which item this spell belongs to, so the ban list can name it
        if (m_spellItem.find(spellId) == m_spellItem.end())
        {
            for (auto const& itr : sObjectMgr.GetItemPrototypeMap())
            {
                bool found = false;
                for (int i = 0; i < MAX_ITEM_PROTO_SPELLS && !found; ++i)
                    found = itr.second.Spells[i].SpellId == spellId;
                if (found)
                {
                    m_spellItem[spellId] = itr.second.ItemId;
                    break;
                }
            }
        }

        /*
         * AND WHETHER THE CLASS OWNS IT IN ITS OWN RIGHT - the same rule BuildSpellItemMap
         * applies when the table is read at startup, which this path did not.
         *
         * A ban made from the panel or from .arena ban therefore behaved differently from the
         * identical ban read out of the table: m_itemOnlySpells stayed empty for it, and
         * IsSpellDisabled's last line then refused the spell to the CLASS as well. The Magic
         * Candle casts Fireball (Rank 1) and the Sprouted Frond casts Lesser Heal (Rank 2), so
         * banning either trinket in the panel took a mage's and a priest's own spell away with
         * it - until the next restart, when the table was re-read and it silently came back.
         */
        if (m_spellItem.find(spellId) != m_spellItem.end() && sSpellMgr.GetSpellRank(spellId))
            m_itemOnlySpells.insert(spellId);
        else
            m_itemOnlySpells.erase(spellId);

        /*
         * Only the four flags are written on a row that already exists. The descriptions in the
         * table were written by hand and say which item a spell belongs to ("CONSUMABLE Free Action
         * Potion"); REPLACE would have thrown that away and left the bare spell name behind every
         * time somebody changed a bracket from the panel.
         */
        std::string description = spell->SpellName[0];
        WorldDatabase.escape_string(description);
        WorldDatabase.PExecute("INSERT INTO `disabled_arena_spells` (`entry`, `1v1`, `2v2`, `3v3`, `5v5`, `description`) "
                               "VALUES (%u, %u, %u, %u, %u, '%s') "
                               "ON DUPLICATE KEY UPDATE `1v1` = VALUES(`1v1`), `2v2` = VALUES(`2v2`), "
                               "`3v3` = VALUES(`3v3`), `5v5` = VALUES(`5v5`)",
                               spellId, uint32(perType[0]), uint32(perType[1]), uint32(perType[2]), uint32(perType[3]),
                               description.c_str());
    }
    else
    {
        m_disabledSpells.erase(spellId);
        m_spellItem.erase(spellId);
        m_itemOnlySpells.erase(spellId);
        // only the entries this spell put there - two spells can apply the same enchantment, and the
        // other one may still be banned
        for (uint32 enchantId : enchants)
        {
            auto itr = m_tempEnchantSpells.find(enchantId);
            if (itr != m_tempEnchantSpells.end() && itr->second == spellId)
                m_tempEnchantSpells.erase(itr);
        }

        WorldDatabase.PExecute("DELETE FROM `disabled_arena_spells` WHERE `entry` = %u", spellId);
    }

    return true;
}

uint8 ArenaMgr::GetItemMinPatch(uint32 itemId) const
{
    auto itr = m_itemMinPatch.find(itemId);
    return itr != m_itemMinPatch.end() ? itr->second : 0;
}

bool ArenaMgr::HasExcessResistance(Player const* player, std::string* reason)
{
    // Counted from the equipped items rather than from the player's current resistance, on purpose:
    // at the arena orb he is still standing in the world with whatever buffs he happens to carry, and
    // those are stripped on entering anyway. What he actually brings into the match is his gear.
    struct SchoolCap
    {
        int32 name;                                     // mangos_string, "Fire Resistance" whole:
                                                        // German glues the two words together and
                                                        // French puts a preposition between them
        eConfigUInt32Values config;
        int32 ItemPrototype::* field;
    };

    static SchoolCap const schools[] =
    {
        { LANG_ARENA_RESISTANCE_FIRST + 0, CONFIG_UINT32_ARENA_MAX_RES_FIRE,   &ItemPrototype::FireRes   },
        { LANG_ARENA_RESISTANCE_FIRST + 1, CONFIG_UINT32_ARENA_MAX_RES_NATURE, &ItemPrototype::NatureRes },
        { LANG_ARENA_RESISTANCE_FIRST + 2, CONFIG_UINT32_ARENA_MAX_RES_FROST,  &ItemPrototype::FrostRes  },
        { LANG_ARENA_RESISTANCE_FIRST + 3, CONFIG_UINT32_ARENA_MAX_RES_SHADOW, &ItemPrototype::ShadowRes },
        { LANG_ARENA_RESISTANCE_FIRST + 4, CONFIG_UINT32_ARENA_MAX_RES_ARCANE, &ItemPrototype::ArcaneRes },
    };

    for (auto const& school : schools)
    {
        uint32 const cap = sWorld.getConfig(school.config);
        if (!cap)
            continue;

        int32 total = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
            if (Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                if (ItemPrototype const* proto = item->GetProto())
                    total += proto->*school.field;

        if (total > int32(cap))
        {
            if (reason)
                *reason = Textf(player, LANG_ARENA_GEAR_RESISTANCE, Text(player, school.name), total, cap);
            return true;
        }
    }

    return false;
}

uint32 ArenaMgr::GetForbiddenTempEnchantSpell(uint32 enchantId, ArenaType type) const
{
    auto itr = m_tempEnchantSpells.find(enchantId);
    if (itr == m_tempEnchantSpells.end())
        return 0;

    return IsSpellDisabled(itr->second, type) ? itr->second : 0;
}

char const* ArenaMgr::GetPatchName(uint8 patch)
{
    switch (patch)
    {
        case 0:  return "Patch 1.2: Mysteries of Maraudon";
        case 1:  return "Patch 1.3: Ruins of the Dire Maul";
        case 2:  return "Patch 1.4: The Call to War";
        case 3:  return "Patch 1.5: Battlegrounds";
        case 4:  return "Patch 1.6: Assault on Blackwing Lair";
        case 5:  return "Patch 1.7: Rise of the Blood God";
        case 6:  return "Patch 1.8: Dragons of Nightmare";
        case 7:  return "Patch 1.9: The Gates of Ahn'Qiraj";
        case 8:  return "Patch 1.10: Storms of Azeroth";
        case 9:  return "Patch 1.11: Shadow of the Necropolis";
        case 10: return "Patch 1.12: Drums of War";
    }
    return "Unknown patch";
}

bool ArenaMgr::IsItemForbidden(ItemPrototype const* proto, ArenaType type, std::string* reason, Player const* reader) const
{
    if (!proto)
        return false;

    uint32 const maxItemLevel = sWorld.getConfig(CONFIG_UINT32_ARENA_MAX_ITEM_LEVEL);
    uint32 const maxPatch = sWorld.getConfig(CONFIG_UINT32_ARENA_MAX_ITEM_PATCH);

    if (uint8 patch = GetItemMinPatch(proto->ItemId))
    {
        if (patch > maxPatch)
        {
            // the patch titles themselves stay English: they are marketing names no client ever
            // carried in any language
            if (reason)
                *reason = Textf(reader, LANG_ARENA_GEAR_ITEM_PATCH, GetPatchName(patch), GetPatchName(uint8(maxPatch)));
            return true;
        }
    }

    if (proto->ItemLevel > maxItemLevel)
    {
        if (reason)
            *reason = Textf(reader, LANG_ARENA_GEAR_ITEM_LEVEL, proto->ItemLevel, maxItemLevel);
        return true;
    }

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        uint32 spellId = proto->Spells[i].SpellId;
        if (spellId && IsSpellDisabled(spellId, type))
        {
            if (reason)
                *reason = Textf(reader, LANG_ARENA_GEAR_ITEM_BANNED, BracketName(reader, type));
            return true;
        }
    }

    return false;
}

/*********************************************************/
/***                       ARENA                       ***/
/*********************************************************/

/*
 * EVERYTHING A MATCH STARTS WITH, in one place.
 *
 * The constructor and Reset held fifty byte-identical lines each. Every new member had to be
 * entered twice, and forgetting the second one gives a fault that shows up only in the SECOND
 * match on that instance, which is the hardest kind to find - m_startVisibilityPushed was in
 * neither of them and was saved only by the order things happen in.
 *
 * The combat log is cleared HERE and not beside the other m_frameSpecCache clear inside
 * PushFrameData's slow name tick: that one fires every ten seconds and would wipe the log for
 * the whole of the match.
 */
void Arena::ResetArenaState()
{
    m_worldStateTimer = 0;
    m_matchTimer = 0;
    m_doorsDespawnTimer = 0;
    m_lastCountdownSecond = 0;
    m_playersReady = false;
    m_timeLimitReached = false;
    m_preparationExtended = false;
    m_startVisibilityPushed = false;
    m_tornadoTimer[0] = 0;
    m_tornadoTimer[1] = 0;
    m_waterfallTimer = 0;
    m_waterfallKnockbackTimer = 0;
    m_pipeKnockbackTimer = 0;
    m_pipeRecheckTimer = 0;
    m_pipeKnockbackCount = 0;
    m_waterfallState = WATERFALL_OFF;
    m_underMapCheckTimer = 0;

    m_framePushTimer = 0;
    m_frameNameTick = 0;
    m_frameNamesSent.clear();
    m_frameSpecCache.clear();
    m_frameRowCache.clear();
    m_castLineSent = false;

    m_combatLog.clear();
    m_logRoster.clear();
    m_logClass.clear();
    m_logMaxHp.clear();
    m_logSlot.clear();
    m_logCursor.clear();
    m_logHeaderSent.clear();
    m_logTrailerSent.clear();
    m_logSpells.clear();
    m_logDictCursor.clear();
    m_logDeaths.clear();
    m_logDrainTimer = 0;
    m_logDropped = 0;

    m_leaverIsParticipant = false;
    m_spectatorsRemoved = false;
    m_rated = false;
    m_ratedSettled = false;
    m_ratedRoster.clear();
    delete m_keptScore;
    m_keptScore = nullptr;
}

Arena::Arena()
{
    m_startDelayTimes[BG_STARTING_EVENT_FIRST]  = BG_START_DELAY_1M;
    m_startDelayTimes[BG_STARTING_EVENT_SECOND] = BG_START_DELAY_30S;
    m_startDelayTimes[BG_STARTING_EVENT_THIRD]  = BG_START_DELAY_15S;
    m_startDelayTimes[BG_STARTING_EVENT_FOURTH] = BG_START_DELAY_NONE;

    m_startMessageIds[BG_STARTING_EVENT_FIRST]  = BCT_ARENA_START_ONE_MINUTE;
    m_startMessageIds[BG_STARTING_EVENT_SECOND] = BCT_ARENA_START_HALF_MINUTE;
    m_startMessageIds[BG_STARTING_EVENT_THIRD]  = BCT_ARENA_START_FIFTEEN_SECONDS;
    m_startMessageIds[BG_STARTING_EVENT_FOURTH] = BCT_ARENA_HAS_BEGUN;

    ResetArenaState();
}

Arena::~Arena()
{
}

// Builds one chat line per listener and sends it, so a line whose argument is itself translated
// reaches everybody whole. `make` is handed the listener's database locale index.
template <typename MakeLine>
static void SendPerLanguage(BattleGround::BattleGroundPlayerMap const& players, MakeLine&& make)
{
    for (auto const& itr : players)
    {
        Player* player = sObjectMgr.GetPlayer(itr.first);
        if (!player || !player->GetSession())
            continue;

        std::string const line = make(player->GetSession()->GetSessionDbLocaleIndex());

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_BG_SYSTEM_NEUTRAL, line.c_str(), LANG_UNIVERSAL,
                                     CHAT_TAG_NONE, ObjectGuid());
        player->GetSession()->SendPacket(&data);
    }
}

void Arena::SendReasonToAll(int32 entry, int32 reasonEntry, uint32 reasonArg)
{
    SendPerLanguage(m_players, [entry, reasonEntry, reasonArg](int32 loc)
    {
        char reason[512];
        snprintf(reason, sizeof(reason), sObjectMgr.GetMangosString(reasonEntry, loc), reasonArg);

        char line[1024];
        snprintf(line, sizeof(line), sObjectMgr.GetMangosString(entry, loc), reason);
        return std::string(line);
    });
}

void Arena::SendNameAndStringToAll(int32 entry, char const* name, int32 stringEntry)
{
    std::string const who = name ? name : "";
    SendPerLanguage(m_players, [entry, &who, stringEntry](int32 loc)
    {
        char line[1024];
        snprintf(line, sizeof(line), sObjectMgr.GetMangosString(entry, loc), who.c_str(),
                 sObjectMgr.GetMangosString(stringEntry, loc));
        return std::string(line);
    });
}

void Arena::Reset()
{
    BattleGround::Reset();

    // ready check npcs are spawned during the preparation
    m_activeEvents[ARENA_EVENT_WATCHER_1] = 0;
    m_activeEvents[ARENA_EVENT_WATCHER_2] = 0;

    if (IsDalaranArena())
    {
        // water spouts / knockback reference points and the water doodads are always spawned,
        // the water itself is toggled with DoorOpen / DoorClose (see SetWaterActive)
        for (uint8 event1 = ARENA_EVENT_DS_WATERSPOUT_1; event1 <= ARENA_EVENT_DS_PIPE_KICKER_2; ++event1)
            m_activeEvents[event1] = 0;
        for (uint8 event1 = ARENA_EVENT_DS_DOODAD_SEWER01; event1 <= ARENA_EVENT_DS_DOODAD_WATERFALL_COLL; ++event1)
            m_activeEvents[event1] = 0;
    }

    // ARENA_EVENT_SHADOW_SIGHT is spawned ARENA_SHADOW_SIGHT_SPAWN_DELAY seconds after the gates opened.
    // It must be marked as inactive explicitly: SpawnEvent() reads m_activeEvents through operator[],
    // and a default-constructed entry (0) would look like "already spawned" for event2 = 0.
    m_activeEvents[ARENA_EVENT_SHADOW_SIGHT] = BG_EVENT_NONE;

    ResetArenaState();
}

/*********************************************************/
/***                      UPDATE                       ***/
/*********************************************************/

namespace
{
    // The prefix the AddOn listens on. Kept short: it rides in front of every payload.
    char const* const ARENA_FRAME_PREFIX = "ARENAFRM";

    /*
     * The most this will put in one payload.
     *
     * Well under what a chat message carries, and deliberately so: the prefix and its tab ride in
     * front of it, and the client is the side that has to survive whatever arrives. Only the cast
     * line is measured against it - the bars and the control effects are fixed width and cannot
     * reach it.
     */
    size_t const ARENA_FRAME_MAX_PAYLOAD = 200;

    /*
     * What the frames report, and why it is a MECHANIC and not a list of spell ids.
     *
     * Every rank of a spell is its own id in 1.12 - Polymorph alone is 118, 12824, 12825 and
     * 12826 - so a hand written id list is a list to keep up to date forever and to get wrong
     * once. The mechanic is what the spell DOES, it is the same across every rank, and it already
     * covers spells nobody thought to write down.
     *
     * The set is deliberately short. A wall of icons is noise; what a player needs off an enemy
     * frame at a glance is two things: can he act, and can he be hurt. MECHANIC_IMMUNE_SHIELD is
     * the second one whole - SharedDefines calls it "Divine (Blessing) Shield/Protection and Ice
     * Block" in as many words.
     */
    bool IsTrackedMechanic(uint32 mechanic)
    {
        switch (mechanic)
        {
            case MECHANIC_CHARM:
            case MECHANIC_DISORIENTED:          // Blind, Gouge
            case MECHANIC_FEAR:
            case MECHANIC_ROOT:
            case MECHANIC_SILENCE:
            case MECHANIC_SLEEP:
            case MECHANIC_STUN:
            case MECHANIC_FREEZE:
            case MECHANIC_KNOCKOUT:
            case MECHANIC_POLYMORPH:
            case MECHANIC_BANISH:
            case MECHANIC_HORROR:
            case MECHANIC_SAPPED:
            case MECHANIC_INVULNERABILITY:
            case MECHANIC_IMMUNE_SHIELD:
                return true;
        }
        return false;
    }

    /*
     * A WARRIOR STANCE AND NOTHING ELSE.
     *
     * Twelve of the eighteen lines in the first real log were "Berserker Stance" and "Battle
     * Stance" - a warrior dancing for Overpower. That is how the class is played, not something
     * that happened, and with damage and healing lines beside it there is no room for it.
     *
     * BY THE FORM, NOT BY THE AURA, and the difference is the whole point. Testing
     * SPELL_AURA_MOD_SHAPESHIFT would have been the obvious rule and is wrong at both ends: it
     * catches Stealth and Ghost Wolf, which belong in a combat log, and it does NOT catch a druid
     * form at all - every one of those carries a mechanic immunity as well, which SpellRowPriority
     * sorts on first. And a druid form is a real play here: SpellAuras casts 9033 off the back of
     * Cat, Bear, Travel, Aqua and Moonkin to shed roots and slows. The stance branch beside it
     * does nothing of the kind.
     *
     * Three named forms, no spell ids, so it survives a rank changing underneath it.
     */
    bool IsStanceOnly(SpellEntry const* info)
    {
        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (info->EffectApplyAuraName[i] == SPELL_AURA_MOD_SHAPESHIFT)
                switch (info->EffectMiscValue[i])
                {
                    case FORM_BATTLESTANCE:
                    case FORM_DEFENSIVESTANCE:
                    case FORM_BERSERKERSTANCE:
                        return true;
                }

        return false;
    }

    bool IsImmunityMechanic(uint32 mechanic)
    {
        return mechanic == MECHANIC_IMMUNE_SHIELD || mechanic == MECHANIC_INVULNERABILITY;
    }

    /*
     * DIMINISHING RETURNS, when Arena.FrameDiminishing is on.
     *
     * EIGHT categories in a fixed ORDER - which is not the same thing as a fixed position, and
     * this block said it was for as long as there were five of them. The client packs the hits
     * together and skips the categories that are not running, so the third icon is whatever the
     * third RUNNING category happens to be; each slot is identified by the category id in its own
     * field, which is why that id travels at all.
     *
     * Five of them are the ones that decide a vanilla arena - stun, fear, root, polymorph and the
     * sap/gouge group - and the three below them exist because 1.12 gives them clocks of their
     * own. The enum has more (disarm, silence, and the trigger variants), but those either barely
     * diminish here or are not something a player plans around.
     *
     * This is off the beaten path for this server, which reports what a watcher could in
     * principle have seen and DR is a hidden rule. It is behind a switch for exactly that reason:
     * a realm that thinks it is the wrong thing to show turns it off and nothing else changes.
     */
    struct DrCategory
    {
        DiminishingGroup group;
        uint32 id;                              // what goes on the wire
    };

    DrCategory const ARENA_DR_CATEGORIES[] =
    {
        { DIMINISHING_CONTROL_STUN, 1 },
        { DIMINISHING_FEAR,         2 },
        { DIMINISHING_CONTROL_ROOT, 3 },
        { DIMINISHING_POLYMORPH,    4 },
        { DIMINISHING_KNOCKOUT,     5 },

        /*
         * These three are NOT the ones above under another name. 1.12 gives each of them a clock
         * of its own, so a warlock's fear does not diminish a priest's and a kidney shot does not
         * diminish a hammer - and a row that folded them together would show one clock and hide
         * the other, which is worse than not showing them at all.
         *
         * Warlock fear is the reason this matters rather than a curiosity: it is the most common
         * control in the format, and while it had no category here the fear slot stayed empty for
         * the whole of it.
         */
        { DIMINISHING_WARLOCK_FEAR, 6 },
        { DIMINISHING_KIDNEYSHOT,   7 },
        { DIMINISHING_FREEZE,       8 },
    };

    enum ArenaAuraState
    {
        ARENA_AURA_RUNNING   = 0,               // it is on him now
        ARENA_AURA_COOLDOWN  = 1,               // used, and this is the wait
        ARENA_AURA_READY     = 2,               // he has it and it is available
        ARENA_AURA_CONTROL   = 3,               // a control effect: the AddOn puts it on the portrait
    };

    struct TrackedAura
    {
        uint32 id;                              // a mechanic (1..30) or a spell id (498 and up)
        int32 remaining;
        uint8 state;

        /*
         * The whole of the wait, in TENTHS of a second, so the AddOn can draw the clock sweep -
         * it needs how far through this is, and the remaining time alone does not say.
         *
         * Tenths rather than milliseconds because this is the seventh field on the busiest line
         * here. Seven entries of milliseconds comes to 189 bytes against a 200 byte cap, which
         * would start dropping a fighter's last slot the first time everyone used everything at
         * once; tenths costs 168 and a tenth of a second is far below what an eye reads off a
         * sweep. Zero means there is nothing to sweep.
         */
        uint32 total;
    };

    /*
     * Everything the frames show about one fighter.
     *
     * Three kinds go out together, and the STATE field tells them apart rather than the position:
     *
     *   CONTROL   a stun, a fear, a polymorph. The AddOn draws it on the portrait, over the spec
     *             icon, which is what sArena does too - a man who is sheeped should read as
     *             sheeped from his picture, not from an icon somewhere beside him.
     *
     *   RUNNING / COOLDOWN / READY   his class's row of long cooldowns, in a fixed order and
     *             ALWAYS all of them. Ready ones are sent as well, because the whole point of the
     *             row is that the third icon is the same spell every time, so it can be read by
     *             position - and because "his Ice Block is up" is worth as much as "it is down".
     *
     * Only what his class actually has. A warrior has no line in the mage row and never gets one.
     */
    // the window a spell has to sit in to earn a place in his row - the reasoning behind both
    // numbers is written out at WorthARowSlot, which is the only thing that reads them
    uint32 const ARENA_ROW_MIN_COOLDOWN = 30 * IN_MILLISECONDS;
    uint32 const ARENA_ROW_MAX_COOLDOWN = 60 * MINUTE * IN_MILLISECONDS;
    /*
     * Eight, and the number is now arithmetic rather than habit.
     *
     * It was six, and six was blamed on the wire while the wire was innocent - the worst line any
     * character on this server produced was 151 bytes against a 200 cap. Two things bought the
     * other two slots, both below in EmitRowEntry: the countdown travels in TENTHS instead of
     * milliseconds, and the row position went away because nothing on the far side ever read it.
     * That took a worst entry from 25 bytes to 20.
     *
     * The budget, worst case, spelled out so the next person can check it rather than guess:
     *
     *     "b|" and a twelve character name                     14
     *     the control entry, ",30167,36000,3,36000"            20
     *     eight row entries at 20                             160
     *                                                    ---------
     *                                                         194  of 200
     *
     * Nine would be 214 and would not fit. If the row ever has to grow past eight, the honest way
     * is a second message with a sequence number, not a bigger constant here.
     *
     * ONE INVARIANT, and it has been broken before:
     *
     *     ARENA_FRAME_MAX_PAYLOAD >= 14 + (1 + ARENA_MAX_ROW_SPELLS) * 20
     *
     * There used to be a second one - MAX_AURAS >= ARENA_MAX_ROW_SPELLS + MAX_ITEMS - from when
     * the client filled spells and gear out of a single counter. It no longer does: gear has its
     * own compartments beside the row, so the two numbers are independent and that rule would
     * only mislead the next person to read it.
     */
    size_t const ARENA_MAX_ROW_SPELLS = 8;

    uint32 SpellRowCooldown(SpellEntry const* info)
    {
        return info->RecoveryTime > info->CategoryRecoveryTime ?
               info->RecoveryTime : info->CategoryRecoveryTime;
    }

    /*
     * WHAT COMES FIRST, when eight slots have to hold ten or thirteen things.
     *
     * The row was ordered by cooldown alone, longest first, on the reasoning that a long wait
     * means a big effect. In 1.12 that is close to inverted at the top and at the bottom both: a
     * warlock led with Ritual of Doom, which needs five players and cannot physically be cast in a
     * 3v3, and a druid led with Tree Form - while Counterspell, the single most important button a
     * mage owns, has the SHORTEST cooldown of anything he can put here.
     *
     * The first attempt at a cure was eight tiers deep - immunity, control, mitigation, dispel,
     * mobility, power, sustain, filler - scored off mechanics and auras. Measured against all nine
     * classes it was NOT better than what it replaced. It called Vanish and Preparation filler
     * because neither carries an aura that names what it does, ranked Blade Flurry above Vanish,
     * and still cut Berserking. It moved the arbitrariness around rather than removing it.
     *
     * So: three tiers, and the middle one is simply "a spell". Cooldown still does the ordering,
     * which is a rule anybody can check by looking; the tiers only lift the handful of things that
     * decide a fight above it, and drop the handful that cannot happen in an arena below it.
     */
    enum RowPriority
    {
        ROW_PRIORITY_ELSEWHERE = 0,     // summons, shapeshifts, tracking: not in this fight
        ROW_PRIORITY_NORMAL    = 1,     // a spell with a cooldown, which is most of them
        ROW_PRIORITY_DECIDES   = 2,     // control, immunity, interrupt, and surviving the burst
    };

    bool HasAnyMechanic(SpellEntry const* info, bool (*test)(uint32))
    {
        if (test(info->Mechanic))
            return true;

        for (uint32 i = 0; i < MAX_EFFECT_INDEX; ++i)
            if (test(info->EffectMechanic[i]))
                return true;

        return false;
    }

    uint32 SpellRowPriority(SpellEntry const* info)
    {
        /*
         * THE ONES THAT DECIDE IT, and this question is asked FIRST so that nothing below can
         * demote a control effect on a technicality - Earthbind's root is a persistent area aura
         * like Hurricane is, and only the order of these two blocks keeps them apart.
         *
         * CONTROL is IsTrackedMechanic, the same question the diminishing half of this file
         * already asks, so the two cannot drift apart on what counts as crowd control.
         *
         * IMMUNITY twice over: the MECHANIC catches Ice Block and Divine Shield, and the AURA
         * catches the ones that are immune to only one thing - Fear Ward, Berserker Rage, Death
         * Wish. Cooldown still orders them inside the tier, so a thirty second fear break sits
         * below a half hour Shield Wall rather than above it, which is the trap the first draft
         * fell into.
         *
         * MITIGATION because Shield Wall and Ice Barrier and Evasion are the answer to being
         * trained, and being trained is how people die in a 1.12 arena. Reduce it, absorb it or
         * dodge it. Barkskin is the one this cannot see: in this database its reduction is an
         * ADD_FLAT_MODIFIER rather than a damage aura, so it stays an ordinary spell.
         *
         * INTERRUPT because a Counterspell at thirty seconds is worth more than anything a mage
         * waits half an hour for, and the old sort put it dead last of eleven.
         */
        if (HasAnyMechanic(info, IsTrackedMechanic) ||
            HasAnyMechanic(info, IsImmunityMechanic) ||
            info->HasAura(SPELL_AURA_MOD_ROOT) ||
            info->HasAura(SPELL_AURA_SCHOOL_IMMUNITY) ||
            info->HasAura(SPELL_AURA_MECHANIC_IMMUNITY) ||
            info->HasAura(SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN) ||
            info->HasAura(SPELL_AURA_SCHOOL_ABSORB) ||
            info->HasAura(SPELL_AURA_MOD_DODGE_PERCENT) ||
            info->HasEffect(SPELL_EFFECT_INTERRUPT_CAST))
            return ROW_PRIORITY_DECIDES;

        /*
         * Not in this fight.
         *
         * Three different effects summon things and the obvious one catches none of the three
         * that mattered: Inferno is SUMMON_DEMON, Ritual of Doom and Lightwell are TRANS_DOOR.
         * Ritual of Doom is the clearest case in the game for this tier existing - it requires
         * five players and cannot physically be cast in a 3v3, and it was the warlock's FIRST
         * icon. Totems are deliberately not here: they summon through the totem-slot effects and
         * a Grounding Totem is very much in this fight.
         *
         * PERSISTENT_AREA_AURA is Hurricane and Volley, ground you stand outside of rather than a
         * cooldown anybody plays around. Asked after control, because a root can be one too.
         */
        if (info->HasEffect(SPELL_EFFECT_SUMMON) ||
            info->HasEffect(SPELL_EFFECT_SUMMON_DEMON) ||
            info->HasEffect(SPELL_EFFECT_SUMMON_WILD) ||
            info->HasEffect(SPELL_EFFECT_SUMMON_GUARDIAN) ||
            info->HasEffect(SPELL_EFFECT_TRANS_DOOR) ||
            info->HasEffect(SPELL_EFFECT_PERSISTENT_AREA_AURA) ||
            info->HasAura(SPELL_AURA_MOD_SHAPESHIFT) ||
            info->HasAura(SPELL_AURA_TRACK_CREATURES) ||
            info->HasAura(SPELL_AURA_TRACK_RESOURCES))
            return ROW_PRIORITY_ELSEWHERE;

        return ROW_PRIORITY_NORMAL;
    }

    /*
     * WHAT EARNS A PLACE IN HIS ROW.
     *
     * This used to be nine hand written lists of five, and hand written lists rot: the rogue row
     * carried Blade Flurry at two minutes and not Blind at five, the warrior had no Intimidating
     * Shout, the hunter no Readiness. Eight of the nine were missing something, and every one of
     * those was somebody forgetting rather than deciding.
     *
     * So it is read off the man instead, the same way his gear is. The rules, each of them there
     * to keep something specific out:
     *
     *   THIRTY SECONDS at the bottom. Not two minutes, which was the obvious threshold and would
     *   have thrown away Counterspell - thirty seconds, and the single most important thing a
     *   mage presses. A Kick at ten seconds would spend its life blinking and stays out.
     *
     *   AN HOUR at the top, which is where utility stops being a fight and starts being a
     *   ceremony. Lay on Hands sits just under it and belongs; nothing above it does.
     *
     *   NOT PEACEFUL-ONLY. This is what keeps the six mage portals out, and they were the worst
     *   of the noise - the client's own attribute says a spell cannot be cast in combat, so it
     *   cannot matter in an arena. It also takes every hunter trap, which is a real loss and is
     *   written down at the call site rather than pretended away.
     *
     *   NOT A RESURRECTION. Rebirth and Reincarnation are half an hour and an hour of nothing
     *   anybody plays around.
     *
     *   NOT A TAUNT. A taunt cannot do anything to a player, and the old sort promoted them
     *   anyway: Challenging Shout at ten minutes took a warrior's fourth slot and Mocking Blow his
     *   sixth, while Challenging Roar took a druid's FIRST.
     *
     *   NOT PASSIVE, and not banned in this bracket - ArenaMgr already answers the second for the
     *   cast check, so the frames ask the same question rather than holding a second opinion.
     *
     * Racials come through this by themselves: Will of the Forsaken and Blood Fury are two minute
     * spells in his book like any other. Talents too, which is what a table could never do - the
     * spell list in the world database does not know that this rogue took Preparation.
     */
    bool WorthARowSlot(SpellEntry const* info, ArenaType type)
    {
        if (!info || info->IsPassiveSpell() || info->IsNonCombatSpell())
            return false;

        if (info->HasEffect(SPELL_EFFECT_RESURRECT) ||
            info->HasEffect(SPELL_EFFECT_SELF_RESURRECT) ||
            info->HasEffect(SPELL_EFFECT_RESURRECT_NEW))
            return false;

        if (info->HasEffect(SPELL_EFFECT_ATTACK_ME) || info->HasAura(SPELL_AURA_MOD_TAUNT))
            return false;

        uint32 const cooldown = SpellRowCooldown(info);
        if (cooldown < ARENA_ROW_MIN_COOLDOWN || cooldown > ARENA_ROW_MAX_COOLDOWN)
            return false;

        // banned here means he cannot press it, and an icon for that is a lie
        return !sArenaMgr.IsSpellDisabled(info->Id, type, false);
    }

    /*
     * His row, what matters most first, one entry per ability.
     *
     * The order used to be the cooldown alone and that is what SpellRowPriority above replaces;
     * the cooldown now only separates two spells of the same worth, longest first, because
     * between two stuns the one he waits longer for is the one worth watching.
     *
     * ONE ENTRY PER ABILITY is the part that had to be learned twice. This used to trust the
     * spellbook to sort ranks out by itself, on the grounds that learning rank two marks rank one
     * inactive. That is true, and it is true for a FIFTH of the game: the flag is only ever
     * written where SkillLineAbility names the successor, and 1241 of 6164 rows do. Sprint and
     * Vanish are in that fifth, which is why the rogue looked correct and hid the fault for weeks.
     *
     * Everything else kept every rank, all of them active, all with the identical cooldown - and
     * since the tie below breaks by ascending id, the LOWEST rank won the slot. A shaman's row was
     * five Stoneclaw Totems, ranks one through five, and the rank six he actually casts was the
     * one spell the cut threw away. A druid showed Tranquility three times and lost Bash and
     * Barkskin for it; a hunter showed Volley three times.
     *
     * So the two are folded together here. The chain table covers precisely the families the
     * successor field does not, and the successor field covers the ones the chain table has never
     * heard of - Vanish has no chain rows at all. Neither alone is enough; both together close it.
     */
    void FindRowSpells(Player* player, ArenaType type, std::vector<uint32>& out)
    {
        // keyed by the family's first rank, so every chain contributes exactly one icon
        std::map<uint32, std::pair<uint32, uint32>> best;    // family -> cooldown, spell

        for (auto const& itr : player->GetSpellMap())
        {
            PlayerSpell const& known = itr.second;
            if (known.state == PLAYERSPELL_REMOVED || known.disabled || !known.active)
                continue;

            SpellEntry const* info = sSpellMgr.GetSpellEntry(itr.first);
            if (!WorthARowSlot(info, type))
                continue;

            uint32 const family = sSpellMgr.GetFirstSpellInChain(itr.first);
            uint32 const cooldown = SpellRowCooldown(info);

            auto const seen = best.find(family);
            if (seen == best.end())
            {
                best[family] = std::make_pair(cooldown, itr.first);
                continue;
            }

            /*
             * Which rank represents the family: the one he presses, which is the highest he has.
             * A longer wait wins outright first, because a few chains lengthen with rank and the
             * frame should promise the wait he will actually sit through.
             */
            if (cooldown > seen->second.first ||
                (cooldown == seen->second.first &&
                 sSpellMgr.GetSpellRank(itr.first) > sSpellMgr.GetSpellRank(seen->second.second)))
                seen->second = std::make_pair(cooldown, itr.first);
        }

        struct RowCandidate
        {
            uint32 priority;
            uint32 cooldown;
            uint32 spell;
        };

        std::vector<RowCandidate> found;
        found.reserve(best.size());
        for (auto const& one : best)
        {
            SpellEntry const* info = sSpellMgr.GetSpellEntry(one.second.second);
            if (!info)
                continue;

            RowCandidate candidate;
            candidate.priority = SpellRowPriority(info);
            candidate.cooldown = one.second.first;
            candidate.spell    = one.second.second;
            found.push_back(candidate);
        }

        std::sort(found.begin(), found.end(),
                  [](RowCandidate const& a, RowCandidate const& b)
                  {
                      if (a.priority != b.priority)
                          return a.priority > b.priority;
                      if (a.cooldown != b.cooldown)
                          return a.cooldown > b.cooldown;
                      // the id breaks the last tie, so the order is the same every push
                      return a.spell < b.spell;
                  });

        for (auto const& one : found)
        {
            if (out.size() >= ARENA_MAX_ROW_SPELLS)
                break;
            out.push_back(one.spell);
        }
    }

    /*
     * The row is HANDED IN rather than looked up again.
     *
     * FindRowSpells walks the whole spellbook, and this used to call it while PushFrameData was
     * already calling it for the same man on the same tick. The caller holds the answer in a
     * cache now and passes it, so the walk happens once per fighter per ten seconds instead of
     * twice per fighter per half second.
     */
    void FindTrackedAuras(Player* player, std::vector<uint32> const& rowSpells,
                          std::vector<TrackedAura>& out)
    {
        /*
         * Everything his own row already reports, so the portrait does not repeat it.
         *
         * Ice Block and Blessing of Protection are immunities AND row cooldowns. Without this the
         * same bubble goes out twice - once as a mechanic on the portrait, once as a spell in the
         * row - and because an immunity outranks every real control effect below, it also buries
         * the fear that is the reason anyone pressed it.
         *
         * The row entry is the one worth keeping: it names the spell rather than the category,
         * and it is the same entry that stays behind afterwards to count the wait down, where the
         * mechanic would simply vanish with the aura. The portrait is for what is done TO him.
         */
        std::set<uint32> ownRow;
        for (uint32 const base : rowSpells)
        {
            /*
             * ONLY the ones that land on him.
             *
             * This set stops the portrait repeating what the row already reports, and that holds
             * only for a spell whose aura sits on its own caster - Ice Block, Blessing of
             * Protection, the racials that buff.
             *
             * Several row spells land on the ENEMY instead: War Stomp, Counterspell, Silence,
             * Scatter Shot, Death Coil. Their aura on a man means somebody did it TO him, and it
             * belongs on his portrait. Skipping it because the same spell happens to be in HIS
             * row too is how a tauren stunned by another tauren showed no control effect at all.
             */
            SpellEntry const* info = sSpellMgr.GetSpellEntry(base);
            if (!info || !info->IsPositiveSpell())
                continue;

            ownRow.insert(base);
        }

        // the control effect first, if there is one - it goes on the portrait
        uint32 control = 0;
        int32 controlLeft = 0;
        int32 controlWhole = 0;                 // the winner's full duration, for the portrait sweep

        for (auto const& itr : player->GetSpellAuraHolderMap())
        {
            SpellAuraHolder const* holder = itr.second;
            if (!holder)
                continue;

            SpellEntry const* info = holder->GetSpellProto();
            if (!info)
                continue;

            if (ownRow.find(info->Id) != ownRow.end())
                continue;

            uint32 found = info->Mechanic;
            for (uint32 i = 0; i < MAX_EFFECT_INDEX && !IsTrackedMechanic(found); ++i)
                found = info->EffectMechanic[i];

            if (!IsTrackedMechanic(found))
                continue;

            // heartbeat included: a polymorph reports the ten seconds it will really last, not
            // the fifty it nominally has - see SpellAuraHolder::GetEffectiveDuration
            int32 const left = holder->GetEffectiveDuration();

            // immunity outranks control, and among equals the one with the most left on it: that
            // is the one still there when you reach him. A permanent aura counts as the longest.
            bool const better = !control
                || (IsImmunityMechanic(found) && !IsImmunityMechanic(control))
                || (IsImmunityMechanic(found) == IsImmunityMechanic(control)
                    && (left < 0 || (controlLeft >= 0 && left > controlLeft)));

            if (better)
            {
                control = found;
                controlLeft = left;
                controlWhole = holder->GetAuraMaxDuration();
            }
        }

        if (control)
        {
            TrackedAura aura;
            aura.id = control;
            aura.remaining = controlLeft;
            aura.state = ARENA_AURA_CONTROL;

            /*
             * The sweep over the portrait needs how far through the effect is, and this field was
             * hard-wired to nought - so SetSweep, whose first test is "total <= 0", returned
             * without drawing anything. The wedge that shows a fear running out has never once
             * appeared; the icon simply sat there until it vanished.
             *
             * Tenths like every other total on this wire, and nought for a permanent aura, which
             * has nothing to sweep.
             */
            aura.total = controlWhole > 0 ? uint32(controlWhole) / 100 : 0;
            out.push_back(aura);
        }

        // his own spells, longest wait first, however many of them he has

        bool const clearStart = sWorld.getConfig(CONFIG_BOOL_ARENA_RESET_ALL_COOLDOWNS);

        for (uint32 const base : rowSpells)
        {
            /*
             * One id, not a rank chain, and now that is actually earned. FindRowSpells folds each
             * family down to the single rank he presses, using the chain table where the
             * spellbook's inactive flag is not written - which is four times out of five.
             *
             * Safe for the reading below: every family that collapses shares one spell category
             * across all its ranks, and GetExpireTime is asked with the category flag set, so the
             * wait comes back the same whichever rank went on the wire.
             */
            std::vector<uint32> ranks;
            ranks.push_back(base);

            TrackedAura aura;
            aura.id = base;                     // always the first rank, so the AddOn keeps one icon
            aura.remaining = 0;
            aura.state = ARENA_AURA_READY;
            aura.total = 0;

            bool decided = false;

            /*
             * Is it on him NOW - and this asks before anything else, without caring WHO cast it.
             *
             * The aura is on him whoever put it there. Fear Ward is a dwarf priest's racial, cast
             * on whoever is about to be feared, and the man wearing it often does not have it in
             * his own spellbook at all. Deciding this after the "does he know it" gate hid the
             * buff from every frame in the match.
             *
             * But it does care WHERE the spell lands. War Stomp, Counterspell, Silence, Scatter
             * Shot and Death Coil only ever sit on their victim, so an aura of one on this man
             * means he was hit by it, not that he is using it - and reading it as RUNNING put the
             * stun on the stunned tauren's own slot while the tauren who pressed it read READY.
             * For those the row says one thing only: whether it is off cooldown.
             */
            SpellEntry const* baseInfo = sSpellMgr.GetSpellEntry(base);
            bool const landsOnHim = baseInfo && baseInfo->IsPositiveSpell();

            if (landsOnHim)
            {
                for (uint32 id : ranks)
                {
                    if (SpellAuraHolder const* holder = player->GetSpellAuraHolder(id))
                    {
                        aura.remaining = holder->GetEffectiveDuration();
                        /*
                         * A PERMANENT aura has a maximum duration of -1, and casting that to
                         * uint32 before dividing gave 42949672 - eight digits on a field the
                         * payload budget above sizes at five, which is how a b| line quietly
                         * grows past the cap and drops a fighter's last cooldown. There is no
                         * sweep to draw for something that does not run out, so it is zero.
                         */
                        aura.total = holder->GetAuraMaxDuration() > 0 ?
                                     uint32(holder->GetAuraMaxDuration()) / 100 : 0;
                        aura.state = ARENA_AURA_RUNNING;
                        decided = true;
                        break;
                    }
                }
            }

            /*
             * ...or on his pet, for the ones that land there.
             *
             * Bestial Wrath is cast by the hunter and applies to the pet - implicit target 5 - so
             * the hunter never carries the aura and the slot sat on READY through all eighteen
             * seconds of it, which is exactly the window somebody reading the frames needs.
             */
            if (!decided && landsOnHim)
            {
                if (Pet* pet = player->GetPet())
                {
                    for (uint32 id : ranks)
                    {
                        if (SpellAuraHolder const* holder = pet->GetSpellAuraHolder(id))
                        {
                            aura.remaining = holder->GetEffectiveDuration();
                            // permanent means -1, and uint32(-1)/100 is 42949672 - see above
                            aura.total = holder->GetAuraMaxDuration() > 0 ?
                                         uint32(holder->GetAuraMaxDuration()) / 100 : 0;
                            aura.state = ARENA_AURA_RUNNING;
                            decided = true;
                            break;
                        }
                    }
                }
            }

            if (!decided && clearStart)
            {
                for (uint32 id : ranks)
                {
                    SpellEntry const* info = sSpellMgr.GetSpellEntry(id);
                    if (!info)
                        continue;

                    TimePoint expire;
                    bool permanent = false;
                    uint32 totalMs = 0;
                    // true: a sibling of a shared category counts, see GetExpireTime. Shield Wall
                    // puts Recklessness and Retaliation away too, and both used to read READY.
                    if (!player->GetExpireTime(info, expire, permanent, true, &totalMs) || permanent)
                        continue;

                    auto const now = player->GetMap()->GetCurrentClockTime();
                    if (expire <= now)
                        continue;

                    aura.remaining = int32(std::chrono::duration_cast<std::chrono::milliseconds>(expire - now).count());
                    aura.total = totalMs / 100;
                    aura.state = ARENA_AURA_COOLDOWN;
                    break;
                }
            }

            out.push_back(aura);
        }
    }

    /*
     * THE GEAR HE CAN PRESS.
     *
     * Vanilla arena is decided by trinkets as much as by spells - the insignia against a fear,
     * the engineer's gloves, whatever raid trinket somebody brought - and none of it was on the
     * frames. It cannot be a written list the way the class rows are: two hundred and fifty
     * trinkets carry a use effect and another hundred and forty pieces of other gear do, and what
     * a man is wearing is his business and changes between rounds.
     *
     * So it is read off him instead, every push. That costs a walk of nineteen slots twice a
     * second and buys two things a list could not: a trinket swapped mid match is on the frames
     * half a second later, and nothing has to be maintained when a patch adds an item.
     *
     * What the ban list forbids in this bracket is left out. It cannot be pressed, and an icon
     * for something nobody can use is worse than no icon - ArenaMgr already answers that question
     * for the cast check, so the frames ask the same one.
     *
     * The ITEM id goes on the wire rather than the spell's, because the client can turn an item
     * id into a picture and a name in the player's own language by itself, and there is no
     * generated table anywhere that would have to keep up with the item database.
     */
    /*
     * Two, because two is what the far side has room for.
     *
     * This was four, and the AddOn draws ONE - its lower compartment went to the talent tree
     * icon, and its loop reads "for k = 1, 1". So three quarters of this line was assembled,
     * measured, filtered per receiver and sent twice a second for every fighter, to be parsed and
     * thrown away. Two rather than one because MAX_ITEMS on the AddOn side is two: the number
     * here matches what the client can hold, not what it currently chooses to draw, so freeing
     * that second compartment needs no server change.
     */
    size_t const ARENA_MAX_ITEMS = 2;

    struct TrackedItem
    {
        uint32 itemId;
        int32 remaining;
        uint8 state;
        uint32 total;                           // tenths, as everywhere else on this wire
    };

    void FindUsableItems(Player* player, ArenaType type, std::vector<TrackedItem>& out)
    {
        bool const clearStart = sWorld.getConfig(CONFIG_BOOL_ARENA_RESET_ALL_COOLDOWNS);

        /*
         * Trinkets first. The four places fill in visit order, and visiting 0..18 put helmets and
         * belts ahead of the trinket slots - so an engineer's headpiece could crowd the insignia,
         * the one item every arena reads the frames FOR, off the line entirely.
         */
        static uint8 const slotOrder[] =
        {
            EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2,
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18,
        };

        for (uint8 slot : slotOrder)
        {
            if (out.size() >= ARENA_MAX_ITEMS)
                break;

            Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);

            if (!item)
                continue;

            ItemPrototype const* proto = item->GetProto();
            if (!proto)
                continue;

            for (int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
            {
                if (proto->Spells[s].SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
                    continue;

                uint32 const spellId = proto->Spells[s].SpellId;
                if (!spellId)
                    continue;

                SpellEntry const* info = sSpellMgr.GetSpellEntry(spellId);
                if (!info)
                    continue;

                // banned in this bracket: there is nothing for him to press
                if (sArenaMgr.IsSpellDisabled(spellId, type, true))
                    break;

                TrackedItem entry;
                entry.itemId = proto->ItemId;
                entry.remaining = 0;
                entry.state = ARENA_AURA_READY;
                entry.total = 0;

                if (clearStart)
                {
                    TimePoint expire;
                    bool permanent = false;
                    uint32 totalMs = 0;

                    // the proto rides along: potions and shared trinkets keep their cooldown in an
                    // ITEM category the spell knows nothing about, see GetExpireTime
                    if (player->GetExpireTime(info, expire, permanent, true, &totalMs, proto) && !permanent)
                    {
                        auto const now = player->GetMap()->GetCurrentClockTime();
                        if (expire > now)
                        {
                            entry.remaining = int32(std::chrono::duration_cast<std::chrono::milliseconds>(expire - now).count());
                            entry.total = totalMs / 100;
                            entry.state = ARENA_AURA_COOLDOWN;
                        }
                    }
                }

                out.push_back(entry);
                break;                          // one use effect an item is enough
            }
        }
    }

    /*
     * One addon message to one player.
     *
     * On 1.12 an addon message is marked by the LANGUAGE field, not by the message type:
     * LANG_ADDON, with the text carrying "PREFIX\tpayload" exactly the way the client's own
     * SendAddonMessage assembles it. SharedDefines.h has a CHAT_MSG_ADDON constant, but that is
     * the 2.4 way of doing it - the comment on LANG_ADDON says as much, and nothing in this
     * source uses the constant.
     *
     * Whisper rather than party, so it works for a spectator who is in no group and for a 1v1
     * where there is no party at all. The sender is the receiver himself, which keeps it out of
     * every ignore list.
     */
    void SendArenaAddon(Player* to, std::string const& payload)
    {
        if (!to || !to->GetSession())
            return;

        std::string text(ARENA_FRAME_PREFIX);
        text += "\t";
        text += payload;

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, text.c_str(), LANG_ADDON, CHAT_TAG_NONE,
                                     to->GetObjectGuid(), to->GetName());
        to->GetSession()->SendPacket(&data);
    }
}

/*
 * The bars of every fighter, to everybody who can see the match.
 *
 * Stateless on purpose: one line carries the whole roster AND its numbers, so a spectator who
 * arrives mid match is right on his first tick and nothing has to be re-sent when somebody dies or
 * leaves. It costs a few bytes more per tick than sending names once and indices afterwards, and
 * it saves having to keep two sides in step, which is where that kind of protocol goes wrong.
 *
 * Only while the match is running. During preparation the two sides are deliberately invisible to
 * each other (Unit::IsVisibleForOrDetect), and handing the enemy's health over a side channel
 * would undo exactly that.
 */
void Arena::PushFrameData(uint32 diff)
{
    if (m_framePushTimer > diff)
    {
        m_framePushTimer -= diff;
        return;
    }
    m_framePushTimer = ARENA_FRAME_PUSH_INTERVAL;

    /*
     * THE FIGHTERS, LOOKED UP ONCE.
     *
     * GetPlayers() was walked six times a push - names, bars, casts, cooldowns, gear, diminishing
     * - with a global guid lookup on every man in every one of them. They are all the same list.
     */
    std::vector<Player*> fighters;
    fighters.reserve(GetPlayers().size());
    for (auto const& itr : GetPlayers())
        if (Player* player = sObjectMgr.GetPlayer(itr.first))
            fighters.push_back(player);

    if (fighters.empty())
        return;

    /*
     * HIS ROW, LOOKED UP ONCE - and this is the expensive one.
     *
     * FindRowSpells walks the entire spellbook: some three hundred entries, each costing a spell
     * lookup, a chain lookup and a ban-list lookup. It ran twice per fighter on a name tick, once
     * for the name union and once inside FindTrackedAuras, which in a 5v5 is twenty full walks a
     * second on the map thread.
     *
     * A spellbook cannot change inside an arena - nobody learns a spell in a start box - so the
     * second walk was asking a question it already had the answer to. Refreshed with the spec
     * cache on the slow tick, which leaves one walk per fighter per ten seconds.
     */
    auto rowOf = [this](Player* player) -> std::vector<uint32> const&
    {
        ObjectGuid const guid = player->GetObjectGuid();
        auto found = m_frameRowCache.find(guid);
        if (found == m_frameRowCache.end())
        {
            std::vector<uint32> row;
            FindRowSpells(player, GetArenaType(), row);
            found = m_frameRowCache.emplace(guid, std::move(row)).first;
        }
        return found->second;
    };

    /*
     * WHO MAY SEE WHOM.
     *
     * The frames used to report every fighter to every receiver, which handed a stealthed rogue
     * to the other side the moment the gates opened - his name, his health, his cooldowns and his
     * diminishing returns, all of it, while he was standing in a corner being invisible. That is
     * not a display fault, it is the AddOn winning the opener.
     *
     * So every line is tagged with the man it is about and the question is asked once per pair.
     * IsVisibleForOrDetect is the client's own rule, so a teammate still sees him, somebody with
     * detection still sees him, and a vanish mid fight takes him off the frames again the same
     * way it takes him off the screen.
     */
    struct FighterLine
    {
        Player* who;
        std::string text;
    };

    auto const canSee = [](Player* fighter, Player* receiver) -> bool
    {
        if (!fighter || !receiver || fighter == receiver)
            return true;

        return fighter->IsVisibleForOrDetect(receiver, receiver, false);
    };

    /*
     * THE NAMES, every twentieth push.
     *
     * The 1.12 client cannot look a spell id up - there is no GetSpellInfo, and GetSpellName only
     * reaches the player's own spellbook, which is no use at all for an opponent's spells. So the
     * AddOn has an id and nothing to call it, and the only way it gets a name for a tooltip is if
     * the server spells it out. Same reason the cast bar carries its own name.
     *
     * What goes out is the union of every fighter's row, once, not per fighter: a name is a
     * property of the spell, not of the man holding it, and half the rows are shared anyway.
     * Localised per receiver like the cast line, with the same enUS fallback.
     */
    /*
     * AND THEY ARE FILTERED PER RECEIVER, exactly like the lines they name.
     *
     * A row name points at the man who holds it. Sent as one union to everybody, it said "one of
     * the ten people here knows Vanish and Preparation" - and since you know your own side, that
     * reads as "the enemy has a rogue" while the a| line is still deliberately withholding him.
     * A weaker channel than his health bar, but the same channel, and the frames exist to close
     * exactly this one.
     *
     * A comment here used to claim the cure was a fixed table of every class row and every
     * racial, identical in every match. That table was never written - the union below is what
     * the code has always sent. Carrying the pairs instead makes the strict version true, and
     * costs one std::set per receiver on a tick that happens once every ten seconds.
     */
    bool const sendNames = (m_frameNameTick++ % 20) == 0;
    std::vector<std::pair<Player*, uint32>> rowNamePairs;
    std::vector<std::pair<Player*, uint32>> drNamePairs;
    if (sendNames)
    {
        m_frameNamesSent.clear();
        m_frameSpecCache.clear();
        m_frameRowCache.clear();

        // every id any fighter's row could hold, gathered WITH the man it came from so the
        // receiver loop can drop the ones he is not allowed to know about
        for (Player* player : fighters)
            for (uint32 const id : rowOf(player))
                rowNamePairs.push_back({ player, id });
    }

    std::vector<FighterLine> unitEntries;
    for (Player* player : fighters)
    {
        ObjectGuid const guid = player->GetObjectGuid();

        /*
         * CURRENT AND MAXIMUM, not a percentage. The AddOn shows "2719/2832" on the bar the way
         * the default frames do, and a percentage cannot be turned back into that. Four numbers
         * instead of two costs about twelve bytes on a line that is nowhere near its cap, and
         * the client derives the percent for the bar fill itself.
         */
        uint32 const maxHealth = player->GetMaxHealth();
        uint32 const health = player->GetHealth();

        Powers const powerType = player->GetPowerType();
        uint32 const maxPower = player->GetMaxPower(powerType);
        uint32 const power = player->GetPower(powerType);

        /*
         * ONE LINE PER FIGHTER, like every other per-fighter line here - because one line for the
         * roster cannot hold a 5v5. Ten fighters at up to 33 bytes each come to 341 against the
         * 200 byte cap, so the old single line silently dropped the tail every push - and when
         * the receiver himself was in the dropped tail, his AddOn could not find its own name,
         * decided it was spectating, and drew both teams mixed together. The client accumulates
         * these by name and prunes what stops arriving.
         */
        /*
         * The spec tab walks the whole talent map, and talents do not change mid match - so it is
         * asked once per fighter and remembered, refreshed with the slow name tick just in case.
         */
        uint32 spec;
        auto const cached = m_frameSpecCache.find(guid);
        if (cached != m_frameSpecCache.end())
            spec = cached->second;
        else
        {
            spec = player->GetTalentSpecTab();
            m_frameSpecCache[guid] = spec;
        }

        std::ostringstream one;
        one << "a|" << player->GetName() << ","
            << uint32(player->GetClass()) << ","
            << (player->GetBGTeam() == ALLIANCE ? 0 : 1) << ","
            << health << "," << maxHealth << ","
            << power << "," << maxPower << ","
            << uint32(powerType) << ","
            << (player->IsAlive() ? 0 : 1) << ","
            << spec;

        FighterLine entry;
        entry.who = player;
        entry.text = one.str();
        unitEntries.push_back(entry);
    }

    if (unitEntries.empty())
        return;

    /*
     * Who is casting what.
     *
     * A separate message rather than more fields on the one above, for one reason: a spell name
     * is up to twenty five bytes and a chat payload is capped around 255. Five fighters with
     * their bars already come to about 150, and five casting at once would burst it. Casts are
     * their own line, they are usually none or one, and they simply do not go out when nobody is
     * casting.
     *
     * The name has to travel WITH the cast. The 1.12 client cannot look a spell id up: there is
     * no GetSpellInfo, and GetSpellName only reaches the player's own spellbook. If the server
     * does not spell it out, the AddOn has an id and nothing to show for it.
     */
    struct CastInfo
    {
        Player* who;
        std::string caster;
        SpellEntry const* spell;
        uint32 total;
        uint32 remaining;
    };
    std::vector<CastInfo> casts;

    for (Player* player : fighters)
    {
        Spell* spell = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!spell)
            spell = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!spell || !spell->m_spellInfo)
            continue;

        int32 const total = spell->GetCastTime();
        if (total <= 0)
            continue;                           // instant: there is no bar to draw

        CastInfo info;
        info.who = player;
        info.caster = player->GetName();
        info.spell = spell->m_spellInfo;
        info.total = uint32(total);
        info.remaining = spell->GetCastedTime();
        casts.push_back(info);
    }

    /*
     * The control effects and cooldowns: ONE LINE PER FIGHTER.
     *
     * They all shared a line while there were two of them each. Five do not fit - five fighters
     * with five entries comes to well past four hundred bytes against a two hundred ceiling - and
     * the old overflow guard dropped whole FIGHTERS off the tail, so the last two enemies would
     * simply have shown nothing at all. That is the exact failure these frames exist to prevent.
     *
     * A line each is about eighty bytes and cannot overflow. It is also sent for EVERY fighter
     * every tick, including the ones with nothing on them, which is what makes it stateless: an
     * empty line clears that man and nothing has to remember what was sent last time. The
     * AddOn's parser had to learn the same lesson - it used to wipe the whole table on every
     * message, which with a line per fighter would have left only the last one standing.
     *
     * No localisation, unlike the casts: the AddOn draws an icon and a countdown, not a word.
     */
    std::vector<FighterLine> auraLines;
    for (Player* player : fighters)
    {
        std::vector<TrackedAura> found;
        FindTrackedAuras(player, rowOf(player), found);

        std::ostringstream entry;
        entry << "b|" << player->GetName();

        for (size_t i = 0; i < found.size(); ++i)
        {
            /*
             * FOUR fields, and both of the savings are here.
             *
             * The countdown goes in TENTHS. An hour in milliseconds is seven digits and in tenths
             * it is five, and the AddOn never drew finer than a tenth anyway - FormatTime stops at
             * one decimal below ten seconds and whole seconds above it. A permanent aura still
             * travels as -1 and must not be divided into a 0, which would turn "forever" into
             * "gone" on every immunity on screen.
             *
             * The row position is gone. It was added so the fourth icon would mean the same thing
             * on two different hunters, and then the AddOn packed the icons by arrival order
             * anyway and never read the field once in three thousand lines. The order it wanted is
             * a property of the SORT, which is deterministic to the last tie - not of a number
             * riding along beside it.
             */
            std::ostringstream one;
            one << "," << found[i].id
                << "," << (found[i].remaining < 0 ? -1 : found[i].remaining / 100)
                << "," << uint32(found[i].state)
                << "," << found[i].total;

            if (size_t(entry.tellp()) + one.str().size() > ARENA_FRAME_MAX_PAYLOAD)
                break;

            entry << one.str();
        }

        FighterLine line;
        line.who = player;
        line.text = entry.str();
        auraLines.push_back(line);
    }

    /*
     * The DR line, one per fighter, and only when the switch is on.
     *
     * Sent for everybody every tick like the others, so an empty one clears him and nothing has
     * to be remembered. A category at level 0 with no time left is simply left out: the AddOn
     * shows those slots dimmed, and saying "this is at full" every half second would be several
     * hundred bytes a tick to say nothing.
     */
    /*
     * i|Name,itemId,remainingMs,state,totalTenths,...
     *
     * Its own line rather than more fields on the cooldown one: that is already seven entries and
     * about 168 of the 200 bytes a payload may carry, and two trinkets on the end of it would
     * push the racial off.
     */
    std::vector<FighterLine> itemLines;
    for (Player* player : fighters)
    {
        std::vector<TrackedItem> items;
        FindUsableItems(player, GetArenaType(), items);

        std::ostringstream entry;
        entry << "i|" << player->GetName();

        for (TrackedItem const& one : items)
        {
            std::ostringstream field;
            field << "," << one.itemId << "," << one.remaining
                  << "," << uint32(one.state) << "," << one.total;

            if (size_t(entry.tellp()) + field.str().size() > ARENA_FRAME_MAX_PAYLOAD)
                break;

            entry << field.str();
        }

        FighterLine line;
        line.who = player;
        line.text = entry.str();
        itemLines.push_back(line);
    }

    std::vector<FighterLine> drLines;
    if (sWorld.getConfig(CONFIG_BOOL_ARENA_FRAME_DIMINISHING))
    {
        for (Player* player : fighters)
        {
            std::ostringstream entry;
            entry << "d|" << player->GetName();

            for (DrCategory const& category : ARENA_DR_CATEGORIES)
            {
                /*
                 * IsDiminishing rather than a non-zero countdown: while the effect is still ON
                 * him the clock has not started, so there is no number - but the slot belongs on
                 * screen, lit in its colour, because that is precisely when it matters.
                 */
                if (!player->IsDiminishing(category.group))
                    continue;

                uint32 by = player->GetDiminishingSpell(category.group);

                /*
                 * NEGATIVE means "this is the effect running", positive "this is the wait after".
                 *
                 * The slot could only say one of the two and said the wrong one: it went quiet
                 * while the effect was still on him, which is exactly when somebody is looking at
                 * it, and the one number that matters then is how long he is held for. The sign
                 * carries which of the two it is, so it costs no field on a line that is already
                 * eight categories long.
                 *
                 * The duration comes from the aura itself, found through the spell this group
                 * last recorded - the same id the icon is drawn from.
                 */
                int32 left = int32(player->GetDiminishingReset(category.group));
                if (!left)
                {
                    /*
                     * The recorded spell is the LAST one that landed, and with two controls of one
                     * group overlapping it can wear off while the earlier one still holds him -
                     * then its holder is gone and the slot went out lit but numberless. So when the
                     * record answers nothing, whatever of that group is actually on him answers
                     * instead, and the icon follows it: that is the spell doing the holding.
                     */
                    SpellAuraHolder const* holder = by ? player->GetSpellAuraHolder(by) : nullptr;
                    if (!holder)
                    {
                        for (auto const& itrHolder : player->GetSpellAuraHolderMap())
                        {
                            SpellAuraHolder const* candidate = itrHolder.second;
                            if (!candidate || !candidate->GetSpellProto())
                                continue;
                            if (candidate->GetSpellProto()->GetDiminishingReturnsGroup(false) != category.group)
                                continue;
                            if (!holder || candidate->GetEffectiveDuration() > holder->GetEffectiveDuration())
                                holder = candidate;
                        }

                        if (holder)
                            by = holder->GetSpellProto()->Id;
                    }

                    if (holder)
                        left = -holder->GetEffectiveDuration();
                }

                uint32 const level = uint32(player->GetDiminishing(category.group));

                /*
                 * Measured like every other line here.
                 *
                 * Eight categories of four fields comes to about 142 bytes for the longest name a
                 * 1.12 character can have, so this does not bite today - but the a| line was once
                 * the only one without a cap, and that is exactly the shape of thing that stops
                 * being true the next time a field is added.
                 */
                std::ostringstream one;
                one << "," << category.id << "," << level << "," << left << "," << by;

                if (size_t(entry.tellp()) + one.str().size() > ARENA_FRAME_MAX_PAYLOAD)
                    break;

                entry << one.str();

                /*
                 * Named at once when it is new rather than on the next slow tick: unlike a class
                 * row, this id changes during the match and its slot only lives fifteen seconds.
                 * As a PAIR, because a name that points at a man has to pass the same visibility
                 * check his lines do - a receiver who missed one this way gets it with the next
                 * slow tick, which resends every recorded id.
                 */
                if (by && (sendNames || m_frameNamesSent.find(by) == m_frameNamesSent.end()))
                    drNamePairs.push_back({ player, by });
            }

            FighterLine line;
            line.who = player;
            line.text = entry.str();
            drLines.push_back(line);
        }
    }

    // everybody on the map: the fighters and the visitors watching them.
    // HasBgMap and not a null test on GetBgMap, which ASSERTS rather than returning nullptr - the
    // old condition was always true and guarded nothing. Same correction as in DrainCombatLog.
    if (HasBgMap())
    {
        Map* map = GetBgMap();
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* receiver = itr->getSource();
            if (!receiver)
                continue;

            /*
             * WHO HE IS, TOLD RATHER THAN LEFT TO BE WORKED OUT.
             *
             * s|0 and s|1 are the side he is fighting on; a bare s| means he is only watching.
             *
             * The AddOn used to infer this by looking for its own name in the roster, and that
             * inference has one failure it cannot see: "my line has not arrived yet" and "I am a
             * spectator" are the same observation. The server's own comment further up records
             * the shape of it - a receiver who fell off the end of the old single a| line
             * decided he was spectating and drew both teams mixed together.
             *
             * It matters more now than it did then. A spectator gets one window per team, so a
             * fighter mistaken for a spectator for a single frame gets a second window flashed
             * onto his screen.
             *
             * Sent on every push rather than once, for the same reason the bars are: this line
             * carries the whole answer, so somebody who reloads or walks in mid match is right
             * on his first tick and nothing has to be re-sent when he does.
             */
            auto const asFighter = m_players.find(receiver->GetObjectGuid());
            SendArenaAddon(receiver, asFighter == m_players.end() ? "s|"
                           : (asFighter->second.playerTeam == ALLIANCE ? "s|0" : "s|1"));

            // his language, for the two lines that carry words: the names and the cast bar
            LocaleConstant const locale = receiver->GetSession() ?
                                          receiver->GetSession()->GetSessionDbcLocale() : LOCALE_enUS;

            /*
             * Visibility is a question about a PAIR, and it used to be asked four or five times
             * per pair - once per line kind. Asked once here, and every filter below reads the
             * answer from the set.
             */
            std::set<Player*> visible;
            for (FighterLine const& entry : unitEntries)
                if (canSee(entry.who, receiver))
                    visible.insert(entry.who);

            for (FighterLine const& entry : unitEntries)
                if (visible.count(entry.who))
                    SendArenaAddon(receiver, entry.text);

            for (FighterLine const& entry : auraLines)
                if (visible.count(entry.who))
                    SendArenaAddon(receiver, entry.text);

            for (FighterLine const& entry : drLines)
                if (visible.count(entry.who))
                    SendArenaAddon(receiver, entry.text);

            for (FighterLine const& entry : itemLines)
                if (visible.count(entry.who))
                    SendArenaAddon(receiver, entry.text);

            /*
             * Batched up to the payload cap and never truncated in the middle of a name: a name
             * that does not fit starts the next line instead of being cut in half, because half a
             * name in a tooltip is worse than no tooltip.
             */
            if (!rowNamePairs.empty() || !drNamePairs.empty())
            {
                std::set<uint32> receiverIds;
                for (auto const& pair : rowNamePairs)
                    if (visible.count(pair.first))
                        receiverIds.insert(pair.second);
                for (auto const& pair : drNamePairs)
                    if (visible.count(pair.first))
                        receiverIds.insert(pair.second);

                std::ostringstream nameLine;
                nameLine << "n|";
                size_t written = 0;

                for (uint32 id : receiverIds)
                {
                    SpellEntry const* info = sSpellMgr.GetSpellEntry(id);
                    if (!info)
                        continue;

                    std::string name = info->SpellName[locale].empty() ?
                                       info->SpellName[LOCALE_enUS] : info->SpellName[locale];
                    if (name.empty())
                        continue;

                    // a comma in a name would split the field, and the AddOn splits on commas
                    std::replace(name.begin(), name.end(), ',', ' ');

                    std::ostringstream one;
                    one << id << "," << name << ",";

                    if (size_t(nameLine.tellp()) + one.str().size() > ARENA_FRAME_MAX_PAYLOAD)
                    {
                        if (written)
                            SendArenaAddon(receiver, nameLine.str());
                        nameLine.str("");
                        nameLine.clear();
                        nameLine << "n|";
                        written = 0;
                    }

                    nameLine << one.str();
                    ++written;
                }

                if (written)
                    SendArenaAddon(receiver, nameLine.str());
            }

            if (casts.empty())
            {
                // cleared once, not on every tick for the rest of the match
                if (m_castLineSent)
                    SendArenaAddon(receiver, "c|");
                continue;
            }

            /*
             * Built per receiver, because the spell name is localised and this is the one place
             * where two watchers legitimately need different bytes. It costs nothing worth
             * counting: at most a couple of casters and a handful of watchers, twice a second.
             */
            /*
             * Capped, because this is the one line that can run away.
             *
             * The bars are fixed width - five fighters come to about 150 bytes whatever happens.
             * A spell NAME is not: thirty characters is normal and a Cyrillic one is two bytes a
             * character, so five casting at once could put this past 380. A chat payload does not
             * survive that, and five mages casting in a 5v5 is a Tuesday, not a corner case.
             *
             * Dropping the tail is the right failure: the casts are in no meaningful order, and a
             * frame with no bar is a frame that says nothing, which is much better than a payload
             * the client throws away entirely - that would take the health bars down with it.
             */
            std::ostringstream castLine;
            castLine << "c|";
            size_t written = 0;
            for (size_t i = 0; i < casts.size(); ++i)
            {
                if (!visible.count(casts[i].who))
                    continue;

                std::string const& name = casts[i].spell->SpellName[locale].empty() ?
                                          casts[i].spell->SpellName[LOCALE_enUS] :
                                          casts[i].spell->SpellName[locale];

                // 22: four commas, two times in milliseconds, and the icon id
                if (castLine.tellp() + std::streamoff(name.size() + casts[i].caster.size() + 22)
                    > std::streamoff(ARENA_FRAME_MAX_PAYLOAD))
                    break;

                if (written)
                    castLine << ";";
                /*
                 * THE ICON ID RIDES WITH THE NAME, and it is the id from SpellIcon.dbc rather
                 * than the spell's own.
                 *
                 * The bar had a question mark on it since the day it was written, and no amount
                 * of client side work could have fixed that: 1.12 cannot look a foreign spell up
                 * at all - no GetSpellInfo, and GetSpellName reaches only your own book. Which is
                 * the same reason the NAME has to travel, and it has travelled from the start.
                 *
                 * The icon id and not the spell id, because the AddOn's table is then the whole
                 * of SpellIcon.dbc - a thousand rows that cannot go stale. A spell-keyed table
                 * would have to guess which spells get cast, and a guessed table is exactly how
                 * Blind went missing from the cooldown row for weeks.
                 */
                castLine << casts[i].caster << "," << name << ","
                         << casts[i].total << "," << casts[i].remaining
                         << "," << casts[i].spell->SpellIconID;
                ++written;
            }

            if (written)
                SendArenaAddon(receiver, castLine.str());
            else if (m_castLineSent)
                SendArenaAddon(receiver, "c|");
        }
    }

    for (auto const& pair : rowNamePairs)
        m_frameNamesSent.insert(pair.second);
    for (auto const& pair : drNamePairs)
        m_frameNamesSent.insert(pair.second);

    m_castLineSent = !casts.empty();
}

void Arena::Update(uint32 diff)
{
    // visitors are removed shortly before the participants - or right away when the last participant is
    // gone (the base class deletes an empty arena on the spot and the map would unload under their feet
    // without the status slot ever being cleared)
    bool const aboutToBeDeleted = !GetPlayersSize() && !GetInvitedCount(HORDE) && !GetInvitedCount(ALLIANCE);
    if (!m_spectatorsRemoved && ((GetStatus() == STATUS_WAIT_LEAVE && GetEndTime() <= diff) || aboutToBeDeleted))
    {
        m_spectatorsRemoved = true;
        RemoveSpectators();
    }

    if (GetStatus() == STATUS_WAIT_JOIN)
        UpdatePreparation(diff);

    if (GetStatus() == STATUS_IN_PROGRESS)
    {
        /*
         * The two sides were invisible to each other while the boxes were closed (see
         * Unit::IsVisibleForOrDetect), and grid visibility is only refreshed when somebody moves more
         * than ten yards. At the moment the gates open nobody has moved at all, so it is pushed once
         * from here.
         *
         * From HERE and not from StartingEventOpenDoors, which is where it was and where it did
         * nothing: BattleGround::Update calls that function and sets STATUS_IN_PROGRESS only
         * afterwards, so a refresh inside it still reads WAIT_JOIN and re-confirms the very hiding it
         * is supposed to lift. A caster who opened from inside his box then had no enemy on his
         * client at all - no nameplate, no /target, nothing - until he had run ten yards.
         */
        if (!m_startVisibilityPushed)
        {
            m_startVisibilityPushed = true;
            for (auto const& itr : GetPlayers())
                if (Player* player = sObjectMgr.GetPlayer(itr.first))
                    player->UpdateVisibilityAndView();
        }

        m_matchTimer += diff;

        // doors are removed a few seconds after they opened
        if (m_doorsDespawnTimer)
        {
            if (m_doorsDespawnTimer <= diff)
            {
                m_doorsDespawnTimer = 0;
                StartingEventDespawnDoors();
            }
            else
                m_doorsDespawnTimer -= diff;
        }

        PushFrameData(diff);

        if (IsNagrandArena() && sWorld.getConfig(CONFIG_BOOL_ARENA_NAGRAND_TORNADO))
            UpdateNagrand(diff);
        else if (IsDalaranArena())
            UpdateDalaran(diff);

        // rescue players that fell below the arena floor (bad vmaps / knockback accidents)
        if (m_underMapCheckTimer <= diff)
        {
            m_underMapCheckTimer = ARENA_UNDER_MAP_CHECK_INTERVAL;
            CheckPlayersUnderMap();
        }
        else
            m_underMapCheckTimer -= diff;

        if (!m_timeLimitReached && m_matchTimer >= sWorld.getConfig(CONFIG_UINT32_ARENA_TIME_LIMIT_MINUTES) * MINUTE * IN_MILLISECONDS)
        {
            m_timeLimitReached = true;
            PSendMessageToAll(LANG_ARENA_TIME_LIMIT_REACHED, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr);
            CheckWinConditions();
        }
    }

    // world states (alive players, remaining time) once per second during preparation and fight
    if (GetStatus() == STATUS_WAIT_JOIN || GetStatus() == STATUS_IN_PROGRESS)
    {
        if (m_worldStateTimer <= diff)
        {
            m_worldStateTimer = ARENA_WORLD_STATE_UPDATE_INTERVAL;
            UpdateWorldStates();
            if (GetStatus() == STATUS_IN_PROGRESS)
                CheckWinConditions();
        }
        else
            m_worldStateTimer -= diff;
    }

    // must be the last call, the base class may delete the battleground
    /*
     * The log goes out after the gates close, over the leave window, at the cadence the frames
     * used. It cannot ride m_framePushTimer: that one is only touched inside PushFrameData,
     * which is unreachable once the status leaves IN_PROGRESS, so it would tick once and stop.
     */
    if (GetStatus() == STATUS_WAIT_LEAVE && !m_combatLog.empty())
        DrainCombatLog(diff);

    BattleGround::Update(diff);
}

void Arena::UpdatePreparation(uint32 diff)
{
    // nothing to do before the first player entered (base class starts the timers then)
    if (!(m_events & BG_STARTING_EVENT_1))
        return;

    // The watchers are spawned with the map rather than by a call of ours, so this waits for them to
    // appear. It runs every tick on purpose: two creatures and an aura check each is nothing, and it
    // puts the colours back should they ever fall off.
    ApplyWatcherTeamColours();

    // ready check: as soon as everybody told the arena watcher that he is ready, the gates open early
    if (!m_playersReady && AreAllPlayersReady())
    {
        m_playersReady = true;
        int32 const readyDelay = int32(sWorld.getConfig(CONFIG_UINT32_ARENA_READY_START_DELAY_SECONDS) * IN_MILLISECONDS);
        if (GetStartDelayTime() > readyDelay)
        {
            SetStartDelayTime(readyDelay);
            m_events |= (BG_STARTING_EVENT_2 | BG_STARTING_EVENT_3);    // skip the 30 / 15 seconds warnings
        }
    }

    // the seconds are counted out at the end, all but the first - see ARENA_COUNTDOWN_DURATION
    int32 const delay = GetStartDelayTime();
    if (delay > 0 && delay < int32(ARENA_COUNTDOWN_DURATION))
    {
        uint32 const second = uint32((delay + IN_MILLISECONDS - 1) / IN_MILLISECONDS);
        if (second != m_lastCountdownSecond)
        {
            m_lastCountdownSecond = second;
            PlaySoundToAll(SOUND_ARENA_COUNTDOWN_TICK);
            PSendMessageToAll(LANG_ARENA_START_COUNTDOWN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, second);
        }
    }
}

void Arena::UpdateNagrand(uint32 diff)
{
    for (uint8 i = 0; i < 2; ++i)
    {
        if (m_tornadoTimer[i] <= diff)
            m_tornadoTimer[i] = SummonTornado() ? urand(ARENA_NA_TORNADO_INTERVAL_MIN, ARENA_NA_TORNADO_INTERVAL_MAX) : uint32(ARENA_NA_TORNADO_RETRY_DELAY);
        else
            m_tornadoTimer[i] -= diff;
    }
}

void Arena::UpdateDalaran(uint32 diff)
{
    // waterfall cycle: warning (sound + water visual) -> collision + knockbacks -> off -> pause
    if (m_waterfallTimer <= diff)
    {
        switch (m_waterfallState)
        {
            case WATERFALL_OFF:
                PlaySoundToAll(SOUND_ARENA_DS_WATER_INCOMING);
                SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL, true);
                SetWaterActive(ARENA_EVENT_DS_DOODAD_SEWER01, true);
                m_waterfallState = WATERFALL_WARNING;
                m_waterfallTimer = ARENA_DS_WATERFALL_WARNING_DURATION;
                break;
            case WATERFALL_WARNING:
                SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL_COLL, true);
                m_waterfallState = WATERFALL_ON;
                m_waterfallTimer = ARENA_DS_WATERFALL_DURATION;
                m_waterfallKnockbackTimer = 0;
                break;
            case WATERFALL_ON:
                SetWaterActive(ARENA_EVENT_DS_DOODAD_SEWER01, false);
                SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL, false);
                SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL_COLL, false);
                m_waterfallState = WATERFALL_OFF;
                m_waterfallTimer = urand(ARENA_DS_WATERFALL_INTERVAL_MIN, ARENA_DS_WATERFALL_INTERVAL_MAX);
                break;
        }
    }
    else
        m_waterfallTimer -= diff;

    if (m_waterfallState == WATERFALL_ON)
    {
        if (m_waterfallKnockbackTimer <= diff)
        {
            DoWaterfallKick();
            m_waterfallKnockbackTimer = ARENA_DS_WATERFALL_KNOCKBACK_INTERVAL;
        }
        else
            m_waterfallKnockbackTimer -= diff;
    }

    // players are flushed out of the starting pipes after the gates opened
    if (m_pipeKnockbackCount < ARENA_DS_PIPE_KNOCKBACK_COUNT)
    {
        if (m_pipeKnockbackTimer <= diff)
        {
            KickFromPipes();
            DoWaterFlush();
            ++m_pipeKnockbackCount;
            m_pipeKnockbackTimer = ARENA_DS_PIPE_KNOCKBACK_INTERVAL;
        }
        else
            m_pipeKnockbackTimer -= diff;
    }
    // afterwards: anybody who climbed back into a pipe is flushed out again
    // (server side replacement for the WotLK area triggers 5347 / 5348 which have no 1.12 data)
    else if (m_pipeRecheckTimer <= diff)
    {
        m_pipeRecheckTimer = ARENA_DS_PIPE_RECHECK_INTERVAL;

        bool playerInPipe = false;
        for (uint8 spoutEvent = ARENA_EVENT_DS_WATERSPOUT_1; spoutEvent <= ARENA_EVENT_DS_WATERSPOUT_2 && !playerInPipe; ++spoutEvent)
        {
            Creature* spout = GetBgMap()->GetCreature(GetSingleCreatureGuid(spoutEvent, 0));
            if (!spout)
                continue;

            for (const auto& itr : m_players)
            {
                Player* player = sObjectMgr.GetPlayer(itr.first);
                if (!player || !player->IsAlive() || player->IsArenaSpectator() || !player->IsInMap(spout))
                    continue;

                // the pipes are the only place that high above the arena floor (floor z ~3-7, pipes z ~14)
                if (player->GetPositionZ() > 12.0f && spout->GetDistance(player) < 25.0f)
                {
                    playerInPipe = true;
                    break;
                }
            }
        }

        if (playerInPipe)
        {
            KickFromPipes();
            DoWaterFlush();
        }
    }
    else
        m_pipeRecheckTimer -= diff;
}

void Arena::CheckPlayersUnderMap()
{
    // Fallback floor heights per arena: slightly below the lowest legal fight spot.
    // Players that fall through the WMO floor come to rest on the ADT terrain below it
    // (Dalaran has a flat terrain plane at z = 0 under the whole arena), so the threshold
    // must sit ABOVE that resting height or they would never be rescued.
    float minZ;
    switch (GetArenaMapType())
    {
        case ARENA_MAP_NAGRAND:     minZ = 10.0f;  break;   // floor / start rooms ~12.1
        case ARENA_MAP_BLADES_EDGE: minZ = -5.0f;  break;   // lower fight floor 1.0-4.5 (navmesh), terrain 0.5 directly under it:
                                                            // a z threshold cannot separate fallen players from fighters, only a real void fall is caught
        case ARENA_MAP_LORDAERON:   minZ = 30.0f;  break;   // floor ~32.5
        case ARENA_MAP_DALARAN:     minZ = 1.0f;   break;   // floor ~3.2, water channel ~2.8, terrain plane at 0
        case ARENA_MAP_TIGERS_PEAK: minZ = 370.0f; break;   // plateau terrain ~380.7, platforms ~381.5
        case ARENA_MAP_TOLVIRON:    minZ = 14.0f;  break;   // fight floor ~24.4 (Blizzard's own teleport targets sit at 24.41)
        default:                    return;
    }

    for (const auto& itr : m_players)
    {
        Player* player = sObjectMgr.GetPlayer(itr.first);
        // Ghosts are rescued too. A player who died is an ordinary ghost now, and one that slipped
        // through the floor would lie under the map for the rest of the match, out of reach of the
        // team mate who is supposed to resurrect him. Visitors are left alone - they fly about the
        // arena on purpose and are not part of the match.
        if (!player || player->IsArenaSpectator() || player->GetMap() != GetBgMap())
            continue;

        if (player->GetPositionZ() < minZ)
        {
            float x, y, z, o;
            GetTeamStartLoc(player->GetBGTeam(), x, y, z, o);
            player->NearTeleportTo(x, y, z, o);
        }
    }
}

/*********************************************************/
/***                  STARTING EVENTS                  ***/
/*********************************************************/

void Arena::StartingEventCloseDoors()
{
    // Dalaran: the water is running during the preparation, purely visual
    if (IsDalaranArena())
    {
        SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL, true);
        SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL_COLL, true);
        SetWaterActive(ARENA_EVENT_DS_DOODAD_SEWER01, true);
    }

    ApplyArenaWeather();

    UpdateWorldStates();
}

void Arena::StartMatchNow()
{
    if (GetStatus() != STATUS_WAIT_JOIN)
        return;

    // the last second is left standing so the normal countdown path runs once and opens the doors,
    // plays the sound and spawns what it always spawns - nothing here duplicates that work
    m_playersReady = true;
    m_events |= (BG_STARTING_EVENT_1 | BG_STARTING_EVENT_2 | BG_STARTING_EVENT_3);
    if (GetStartDelayTime() > int32(IN_MILLISECONDS))
        SetStartDelayTime(int32(IN_MILLISECONDS));
}

uint32 Arena::GetArenaZoneId() const
{
    switch (GetArenaMapType())
    {
        case ARENA_MAP_NAGRAND:     return ARENA_NA_ZONE_ID;
        case ARENA_MAP_BLADES_EDGE: return ARENA_BE_ZONE_ID;
        case ARENA_MAP_LORDAERON:   return ARENA_RL_ZONE_ID;
        case ARENA_MAP_DALARAN:     return ARENA_DS_ZONE_ID;
        case ARENA_MAP_TIGERS_PEAK: return ARENA_TP_ZONE_ID;
        case ARENA_MAP_TOLVIRON:    return ARENA_TV_ZONE_ID;
    }
    return 0;
}

uint8 Arena::GetSuitableWeather(WeatherType* out, uint8 max) const
{
    // WEATHER_TYPE_STORM is the sandstorm, so it belongs to Uldum and nowhere else, and snow belongs on
    // the mountain rather than in a desert or a sewer. Fine weather is always an option.
    static WeatherType const desert[]    = { WEATHER_TYPE_FINE, WEATHER_TYPE_STORM };
    static WeatherType const mountain[]  = { WEATHER_TYPE_FINE, WEATHER_TYPE_SNOW };
    static WeatherType const temperate[] = { WEATHER_TYPE_FINE, WEATHER_TYPE_RAIN };
    static WeatherType const indoors[]   = { WEATHER_TYPE_FINE };

    WeatherType const* kinds;
    uint8 count;
    switch (GetArenaMapType())
    {
        case ARENA_MAP_TOLVIRON:    kinds = desert;    count = 2; break;   // Uldum
        case ARENA_MAP_TIGERS_PEAK: kinds = mountain;  count = 2; break;   // snowy peak
        case ARENA_MAP_DALARAN:     kinds = indoors;   count = 1; break;   // underground, weather is not visible
        default:                    kinds = temperate; count = 2; break;   // Nagrand, Blade's Edge, Lordaeron
    }

    if (count > max)
        count = max;
    for (uint8 i = 0; i < count; ++i)
        out[i] = kinds[i];
    return count;
}

void Arena::SetArenaWeather(WeatherType type, float grade)
{
    // permanently, so the world's own weather regeneration cannot clear it mid match
    if (uint32 const zoneId = GetArenaZoneId())
        GetBgMap()->SetWeather(zoneId, type, grade, true);
}

void Arena::ApplyArenaWeather()
{
    if (!GetArenaZoneId())
        return;

    // Tiger's Peak sits on a snowy mountain and keeps its snow whether the option is on or not.
    if (!sWorld.getConfig(CONFIG_BOOL_ARENA_RANDOM_WEATHER))
    {
        if (IsTigersPeakArena())
            SetArenaWeather(WEATHER_TYPE_SNOW, 0.5f);
        return;
    }

    WeatherType kinds[4];
    uint8 const count = GetSuitableWeather(kinds, 4);
    WeatherType const type = kinds[urand(0, count - 1)];
    // fine weather carries no grade; the others get a light to heavy roll
    SetArenaWeather(type, (type == WEATHER_TYPE_FINE) ? 0.0f : frand(0.3f, 0.9f));
}

void Arena::StartingEventOpenDoors()
{
    OpenDoorEvent(BG_EVENT_DOOR);
    m_doorsDespawnTimer = ARENA_DOORS_DESPAWN_DELAY;

    PlaySoundToAll(SOUND_ARENA_LET_THE_GAMES_BEGIN);

    // The four TBC arenas get their gate sound from the door model's own $GO trigger. The Dalaran
    // sewer door and the Tol'Viron gate have none, so theirs is played from the doors themselves -
    // they are visible, so the sound stays positional like the others.
    uint32 const doorSound = IsDalaranArena()  ? SOUND_ARENA_DS_DOOR_OPEN
                           : IsTolvironArena() ? SOUND_ARENA_TV_DOOR_OPEN : 0;
    if (doorSound)
        for (auto const& guid : m_eventObjects[MAKE_PAIR32(BG_EVENT_DOOR, 0)].gameobjects)
            if (GameObject* door = GetBgMap()->GetGameObject(guid))
                door->PlayDistanceSound(doorSound);

    // the ready check npcs leave, the shadow sight orbs come later
    SpawnEvent(ARENA_EVENT_WATCHER_1, 0, false, true);
    SpawnEvent(ARENA_EVENT_WATCHER_2, 0, false, true);
    SpawnEvent(ARENA_EVENT_SHADOW_SIGHT, 0, true, false, ARENA_SHADOW_SIGHT_SPAWN_DELAY);

    // a team did not show up: no fight (except in .debug bg mode) - and no repair / cooldown reset either
    if ((!GetPlayersCountByTeam(ALLIANCE) || !GetPlayersCountByTeam(HORDE)) && !sBattleGroundMgr.isTesting())
    {
        EndBattleGround(TEAM_NONE);
        return;
    }

    // who is actually standing in the boxes decides whether this counts for the rating
    DetermineRated();

    for (const auto& itr : m_players)
        if (Player* player = sObjectMgr.GetPlayer(itr.first))
            ResetPlayerForFight(player);

    if (IsDalaranArena())
    {
        SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL, false);
        SetWaterActive(ARENA_EVENT_DS_DOODAD_WATERFALL_COLL, false);
        SetWaterActive(ARENA_EVENT_DS_DOODAD_SEWER01, false);
        m_waterfallState = WATERFALL_OFF;
        m_waterfallTimer = ARENA_DS_FIRST_WATERFALL_DELAY;
        m_waterfallKnockbackTimer = 0;
        m_pipeKnockbackCount = 0;
        m_pipeKnockbackTimer = ARENA_DS_PIPE_KNOCKBACK_FIRST_DELAY;
    }
    else if (IsNagrandArena())
    {
        m_tornadoTimer[0] = ARENA_NA_FIRST_TORNADO_DELAY;
        m_tornadoTimer[1] = urand(ARENA_NA_TORNADO_INTERVAL_MIN, ARENA_NA_TORNADO_INTERVAL_MAX);
    }

    m_matchTimer = 0;
    m_timeLimitReached = false;
    m_startVisibilityPushed = false;
}

/*********************************************************/
/***                      PLAYERS                      ***/
/*********************************************************/

void Arena::AddPlayer(Player* player)
{
    BattleGround::AddPlayer(player);

    // A row of his may still be lying there: leaving keeps it (see RemovePlayerAtLeave), and somebody
    // who walked out during the preparation can be sent back into the same match by the queue. It is
    // replaced, not added to - but it has to be freed first, or the old one is lost with nobody
    // holding it any more.
    BattleGroundScoreMap::iterator old = m_playerScores.find(player->GetObjectGuid());
    if (old != m_playerScores.end())
    {
        delete old->second;
        m_playerScores.erase(old);
    }

    ArenaScore* score = new ArenaScore;
    // His rating as he walks in, so the column shows something sensible for the whole match instead
    // of a zero that turns into a number at the very end. Read once, here: the scoreboard packet is
    // rebuilt on every frame the client has the score window open and must not look anything up.
    score->newRating = sArenaRatingMgr.GetRating(player->GetObjectGuid(), GetArenaType());
    score->team = player->GetBGTeam();
    m_playerScores[player->GetObjectGuid()] = score;

    // (the other arena queues were already left in HandleBattleFieldPortOpcode - world thread; the map
    // thread we run in here must not touch the queue containers of other maps)

    PrepareArenaPlayer(player);

    // late arrival (invited during preparation, ported after the gates opened): no preparation aura,
    // he joins the fight combat-ready instead - the aura would otherwise stay on for the whole match
    if (GetStatus() == STATUS_IN_PROGRESS)
        ResetPlayerForFight(player);
    else
        player->AddAura(SPELL_ARENA_PREPARATION);

    // The colours belong to the player from the moment he stands in his box, not from the moment he
    // reports ready - waiting for the ready check left both teams standing there unmarked. Visitors
    // watching through the orb belong to no team and stay unmarked.
    if (!player->IsArenaSpectator())
        ApplyTeamAura(player);

    SendNameAndStringToAll(LANG_ARENA_PLAYER_JOINED, player->GetName(),
                           player->GetBGTeam() == HORDE ? LANG_ARENA_TEAM_GREEN_NAME : LANG_ARENA_TEAM_GOLD_NAME);

    UpdateWorldStates();
}

void Arena::RemovePlayerAtLeave(ObjectGuid guid, bool transport, bool sendPacket)
{
    // the base class erases the player from m_players before it calls RemovePlayer - remember whether
    // this was a fighter or only a visitor of the match
    m_leaverIsParticipant = m_players.find(guid) != m_players.end();

    /*
     * Keep his line on the scoreboard. The base class deletes the score row of anybody who leaves,
     * which is right for a battleground - but in an arena it means the losing side simply is not
     * there any more when the winner reads the result, and a man who walked out mid match leaves no
     * trace at all. His row is copied out of the way here and put back afterwards; from then on it
     * belongs to nobody, which is why it carries his team and his rating change itself.
     */
    if (m_leaverIsParticipant)
    {
        BattleGroundScoreMap::const_iterator score = m_playerScores.find(guid);
        if (score != m_playerScores.end())
            m_keptScore = new ArenaScore(*static_cast<ArenaScore*>(score->second));
    }

    BattleGround::RemovePlayerAtLeave(guid, transport, sendPacket);
    m_leaverIsParticipant = false;

    // Arena::RemovePlayer, called from in there, puts it back. If it did not - a visitor, or a guid
    // the base did not reach - nothing owns it any more.
    delete m_keptScore;
    m_keptScore = nullptr;
}

void Arena::RemovePlayer(Player* player, ObjectGuid guid)
{
    // His scoreboard row goes back in before anything else happens here: the very next thing this
    // function does can be CheckWinConditions, and a match that ends there builds its final
    // scoreboard on the spot. One tick later would be too late for the result everybody reads.
    if (m_keptScore)
    {
        if (m_playerScores.find(guid) == m_playerScores.end())
            m_playerScores[guid] = m_keptScore;
        else
            delete m_keptScore;
        m_keptScore = nullptr;
    }

    // visitors leave silently, participants are announced - including a dead one. IsArenaSpectator
    // is set only for an orb visitor these days (Player::SetArenaSpectator has the same note): a
    // fighter who dies stays an ordinary ghost and is still part of the match.
    bool const announce = GetStatus() < STATUS_WAIT_LEAVE && (!player || !player->IsArenaSpectator());
    if (announce)
    {
        PlaySoundToAll(SOUND_ARENA_PLAYER_LEFT);
        if (player)
            PSendMessageToAll(LANG_ARENA_PLAYER_LEFT, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, player->GetName());
    }

    if (player)
        RestorePlayer(player, m_leaverIsParticipant);

    // Somebody walked out before the gates opened, so the match is short of players. The queue can
    // send a replacement - BattleGround::RemovePlayerAtLeave puts the instance back among the invite
    // candidates right after this - but only while more of the countdown is left than an invite needs
    // to be accepted (BattleGroundQueue::FillPlayersToBg). With a one minute preparation that window
    // shuts after thirty seconds, and anybody leaving later left the others watching the clock run
    // down to a no-show draw. So the countdown is held open long enough for one replacement to arrive.
    //
    // Once per match. Otherwise two friends could keep a lobby open indefinitely by taking turns
    // leaving and rejoining.
    if (GetStatus() == STATUS_WAIT_JOIN && GetPlayersSize() < GetMaxPlayers() && !m_preparationExtended)
    {
        int32 const needed = int32(BattleGroundMgr::GetInviteAcceptWaitTime(GetTypeID())) + ARENA_REFILL_LOAD_TIME;
        if (GetStartDelayTime() < needed)
        {
            SetStartDelayTime(needed);
            m_lastCountdownSecond = 0;                      // let the last ten seconds be announced again
            m_preparationExtended = true;
        }
    }

    // the leaving player is already removed from the player list here
    if (GetStatus() == STATUS_IN_PROGRESS)
        CheckWinConditions();
    UpdateWorldStates();
}

void Arena::PrepareArenaPlayer(Player* player)
{
    ArenaType const type = GetArenaType();

    // no gm / cheat states inside the arena
    if (player->IsGameMaster())
        player->SetGameMaster(false);
    if (!player->IsGMVisible())
        player->SetGMVisible(true);
    player->SetCheatGod(false);

    // The cheat switches survived into the match, and one of them turned every other rule off:
    // Spell::CheckCast returns SPELL_CAST_OK on its very first line for PLAYER_CHEAT_NO_CHECK_CAST,
    // so the whole disabled_arena_spells list was skipped for anybody who had it on. The rest are
    // just as decisive in a fight. Only the pure debug switches are left alone.
    for (uint16 const cheat : { PLAYER_CHEAT_FLY, PLAYER_CHEAT_NO_COOLDOWN, PLAYER_CHEAT_NO_CAST_TIME,
                                PLAYER_CHEAT_NO_POWER, PLAYER_CHEAT_DEBUFF_IMMUNITY, PLAYER_CHEAT_ALWAYS_CRIT,
                                PLAYER_CHEAT_NO_CHECK_CAST, PLAYER_CHEAT_ALWAYS_PROC })
    {
        if (player->HasCheatOption(PlayerCheatOptions(cheat)))
            player->RemoveCheatOption(PlayerCheatOptions(cheat));
    }

    player->UnequipForbiddenArenaItems(type);
    RemoveForbiddenTempEnchants(player, type);
    player->Unmount();

    // fresh start: no buffs (the preparation aura is applied by AddPlayer). The free repair and the
    // cooldown reset happen in ResetPlayerForFight when the gates open - entering and leaving during
    // the preparation must not hand out anything (free repairs / cooldown resets on demand).
    player->RemoveAllAurasOnDeath();

    if (Pet* pet = player->GetPet())
    {
        pet->RemoveAllAurasOnDeath();
        pet->SetHealth(pet->GetMaxHealth());
        pet->SetPower(POWER_MANA, pet->GetMaxPower(POWER_MANA));
        if (pet->GetPetType() == HUNTER_PET)
            pet->SetPower(POWER_HAPPINESS, pet->GetMaxPower(POWER_HAPPINESS));
    }
}

void Arena::RemoveForbiddenTempEnchants(Player* player, ArenaType type)
{
    // A weapon oil, a sharpening stone or a weightstone is applied at the vendor and rides in on the
    // weapon, so listing its spell in disabled_arena_spells only stops it being applied INSIDE - the
    // buff itself was already on. Anything the table forbids comes off here instead.
    //
    // Rogue poisons are never in this map (ArenaMgr::LoadFromDB skips the rogue spell family), so a
    // rogue keeps his poisons whatever the table says. They are class equipment, not a consumable.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        uint32 const enchantId = item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
        if (!enchantId)
            continue;

        uint32 const spellId = sArenaMgr.GetForbiddenTempEnchantSpell(enchantId, type);
        if (!spellId)
            continue;

        player->ApplyEnchantment(item, TEMP_ENCHANTMENT_SLOT, false);
        item->ClearEnchantment(TEMP_ENCHANTMENT_SLOT, true);

        if (SpellEntry const* spell = sSpellMgr.GetSpellEntry(spellId))
            ChatHandler(player).PSendSysMessage(LANG_ARENA_ENCHANT_REMOVED, spellId, spell->SpellName[0].c_str(),
                                                item->GetProto()->Name1, ArenaMgr::BracketName(player, type));
    }
}

void Arena::ResetPlayerForFight(Player* player)
{
    if (player->IsArenaSpectator())
        return;

    // died during the preparation (Hellfire, a fall): the fight starts for everybody on their feet
    if (!player->IsAlive())
    {
        player->ResurrectPlayer(1.0f);
        player->SpawnCorpseBones();
    }

    player->RemoveAurasDueToSpell(SPELL_ARENA_PREPARATION);
    player->RemoveAurasDueToSpell(SPELL_ARENA_RECENTLY_BANDAGED);
    player->RemoveShortDurationBuffs(ARENA_SHORT_BUFF_DURATION);
    player->DurabilityRepairAll(false, 0.0f);
    ResetArenaCooldowns(player);
    player->SetHealthPercent(100.0f);
    player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    player->SetPower(POWER_ENERGY, player->GetMaxPower(POWER_ENERGY));
    player->SetPower(POWER_RAGE, 0);
    ApplyTeamAura(player);

    if (Pet* pet = player->GetPet())
    {
        pet->RemoveShortDurationBuffs(ARENA_SHORT_BUFF_DURATION);
        pet->RemoveAllCooldowns();
        pet->SetHealth(pet->GetMaxHealth());
        pet->SetPower(POWER_MANA, pet->GetMaxPower(POWER_MANA));
    }
}

void Arena::RestorePlayer(Player* player, bool participant)
{
    // The spectator state is deliberately NOT cleared here. This runs while he is still standing in
    // the arena and the far teleport has not happened yet, so restoring his visibility now makes him
    // appear in the middle of the map for the length of the handshake. BattleGroundMap::Remove clears
    // it at the one moment he has actually left.
    player->Unmount();
    player->CombatStopWithPets(true);

    // visitors keep their buffs (they never fought), fighters leave the arena clean
    if (!participant)
        return;

    player->RemoveAllAurasOnDeath();

    if (Pet* pet = player->GetPet())
    {
        if (pet->IsControlled())
        {
            pet->RemoveAllAurasOnDeath();
            pet->CombatStop();
            if (pet->IsAlive())
                pet->SetHealth(pet->GetMaxHealth());
        }
    }
}

void Arena::ResetArenaCooldowns(Player* player)
{
    if (sWorld.getConfig(CONFIG_BOOL_ARENA_RESET_ALL_COOLDOWNS))
        player->RemoveAllCooldowns();
    else
    {
        // like TBC: only cooldowns shorter than 10 minutes are reset
        player->RemoveSomeCooldown([](SpellEntry const& spell)
        {
            return spell.RecoveryTime < ARENA_COOLDOWN_RESET_MAX_DURATION && spell.CategoryRecoveryTime < ARENA_COOLDOWN_RESET_MAX_DURATION;
        });
    }
}

uint32 Arena::GetTeamBannerSpell(Team side, bool horde)
{
    if (side == HORDE)
        return horde ? SPELL_ARENA_TEAM_GREEN_HORDE : SPELL_ARENA_TEAM_GREEN;
    return horde ? SPELL_ARENA_TEAM_GOLD_HORDE : SPELL_ARENA_TEAM_GOLD;
}

void Arena::ApplyTeamAura(Player* player)
{
    // The colour is the arena side he was put on, the crest is the faction he actually plays - a
    // Horde player can end up on the gold side, and then he carries a gold banner with the Horde
    // crest. Blizzard has all four; only this pick was missing.
    uint32 const spellId = GetTeamBannerSpell(player->GetBGTeam(), player->GetTeam() == HORDE);
    if (!player->HasAura(spellId))
        player->AddAura(spellId);
}

bool Arena::IsPlayerReady(ObjectGuid guid) const
{
    auto const itr = m_playerScores.find(guid);
    return itr != m_playerScores.end() && static_cast<ArenaScore*>(itr->second)->ready;
}

bool Arena::SetPlayerReady(Player* player)
{
    // Reports whether it took. Without a score there is nothing to mark, and the gossip would
    // otherwise keep offering "I'm ready!" and replay the sound and the watcher's yell on every click.
    auto const itr = m_playerScores.find(player->GetObjectGuid());
    if (itr == m_playerScores.end())
        return false;

    static_cast<ArenaScore*>(itr->second)->ready = true;
    return true;
}

void Arena::ApplyWatcherTeamColours()
{
    // Which watcher belongs to which team is decided by where he stands rather than by his spawn
    // event, so this holds for every arena without a table of its own.
    float ax, ay, az, ao, hx, hy, hz, ho;
    GetTeamStartLoc(ALLIANCE, ax, ay, az, ao);
    GetTeamStartLoc(HORDE, hx, hy, hz, ho);
    for (uint8 const event : { ARENA_EVENT_WATCHER_1, ARENA_EVENT_WATCHER_2 })
    {
        for (ObjectGuid const& guid : m_eventObjects[MAKE_PAIR32(event, 0)].creatures)
        {
            Creature* watcher = GetBgMap()->GetCreature(guid);
            if (!watcher)
                continue;

            Team const side = watcher->GetDistance2d(hx, hy) < watcher->GetDistance2d(ax, ay) ? HORDE : ALLIANCE;

            // he carries the crest of the faction standing in his box, so the banner reads the same
            // as the one on their backs. A mixed side takes the majority, alliance on a tie.
            int32 balance = 0;
            for (const auto& itr : m_players)
                if (Player const* player = sObjectMgr.GetPlayer(itr.first))
                    if (player->GetBGTeam() == side)
                        balance += player->GetTeam() == HORDE ? 1 : -1;

            uint32 const spellId = GetTeamBannerSpell(side, balance > 0);
            if (watcher->HasAura(spellId))
                continue;

            // the side can still fill up while they gather, so an earlier guess is taken off again
            for (uint32 const other : { SPELL_ARENA_TEAM_GOLD, SPELL_ARENA_TEAM_GREEN,
                                        SPELL_ARENA_TEAM_GOLD_HORDE, SPELL_ARENA_TEAM_GREEN_HORDE })
                if (other != spellId)
                    watcher->RemoveAurasDueToSpell(other);

            watcher->AddAura(spellId);
        }
    }
}

bool Arena::AreAllPlayersReady() const
{
    if (GetPlayersSize() < GetMaxPlayers())
        return false;

    for (const auto& itr : m_players)
    {
        if (!IsPlayerReady(itr.first))
            return false;
    }
    return true;
}

void Arena::SendPacketToAll(WorldPacket* packet)
{
    // the template has no map; visitors are on the map but not in m_players, participants that are
    // being ported out are in m_players but no longer on the map - so send to the union of both
    if (!HasBgMap())
    {
        BattleGround::SendPacketToAll(packet);
        return;
    }

    BattleGroundMap* map = GetBgMap();
    std::set<ObjectGuid> sent;
    for (Map::PlayerList::const_iterator itr = map->GetPlayers().begin(); itr != map->GetPlayers().end(); ++itr)
    {
        if (Player* player = itr->getSource())
        {
            player->GetSession()->SendPacket(packet);
            sent.insert(player->GetObjectGuid());
        }
    }

    for (const auto& itr : m_players)
        if (sent.find(itr.first) == sent.end())
            if (Player* player = sObjectMgr.GetPlayer(itr.first))
                player->GetSession()->SendPacket(packet);
}

/*
 * THE GUIDS FIRST, AND THE REMOVALS AFTERWARDS, and that is not style.
 *
 * LeaveBattleground ends in TeleportToBGEntryPoint, TeleportTo calls oldmap->Remove on the spot,
 * and Map::Remove delinks the very node this loop is standing on. LinkedListElement::delink sets
 * iNext to nullptr, and the iterator's ++ reads exactly that - so it did not crash, it silently
 * stopped after the FIRST spectator. And since m_spectatorsRemoved is set once and never cleared,
 * everybody after him stayed in the instance for the rest of its life.
 */
void Arena::RemoveSpectators()
{
    if (!HasBgMap())
        return;

    std::vector<ObjectGuid> leaving;
    Map::PlayerList const& players = GetBgMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* player = itr->getSource();
        if (!player || !player->IsArenaSpectator())
            continue;

        // participants are handled by the base class, visitors are only known to the map
        if (m_players.find(player->GetObjectGuid()) != m_players.end())
            continue;

        leaving.push_back(player->GetObjectGuid());
    }

    for (ObjectGuid const& guid : leaving)
        if (Player* player = sObjectMgr.GetPlayer(guid))
            player->LeaveBattleground(true);
}

/*********************************************************/
/***                   FIGHT / SCORES                  ***/
/*********************************************************/

/*
 * WHO GETS A SLOT IN THE LOG.
 *
 * Append only. m_players loses a man the moment he leaves, so anything derived from its order
 * renumbers everybody still in it - a log written half before and half after a leaver would
 * silently reassign names. A slot handed out here is never handed out again.
 */
uint8 Arena::LogSlotFor(Unit* who)
{
    if (!who)
        return 0xFF;

    ObjectGuid const guid = who->GetObjectGuid();
    auto const found = m_logSlot.find(guid);
    if (found != m_logSlot.end())
        return found->second;

    if (m_logRoster.size() >= 0xFE)             // 0xFF is reserved for "nobody"
        return 0xFF;

    uint8 const slot = uint8(m_logRoster.size());
    m_logRoster.push_back(who->GetName() ? who->GetName() : "?");

    // nought for anything that is not a player - a Voidwalker has a class field but not one
    // RAID_CLASS_COLORS knows, and the AddOn paints slot class 0 in neutral grey
    m_logClass.push_back(who->IsPlayer() ? uint8(who->GetClass()) : uint8(0));
    m_logMaxHp.push_back(who->GetMaxHealth());

    m_logSlot[guid] = slot;
    return slot;
}

/*
 * One event. Called from the spell hook and from HandleKillPlayer, both on the map's own thread.
 *
 * The cap is a real cap and a dropped event is counted rather than ignored: the trailer carries
 * the number so the AddOn can say the log is short instead of pretending it is whole.
 */
int32 Arena::LogEvent(Unit* actor, Unit* victim, SpellEntry const* info, uint8 kind,
                      uint32 amount, uint32 hp, bool haveNumbers, bool crit)
{
    if (GetStatus() != STATUS_IN_PROGRESS || !actor)
        return -1;

    if (m_combatLog.size() >= ARENA_LOG_MAX_EVENTS)
    {
        ++m_logDropped;
        return -1;
    }

    /*
     * A stance change is marked HERE and not at the call site, so a second caller cannot bypass
     * it, and only when the cast actually landed - kind 2 is a miss and kind 7 is a death, and
     * neither of those is ever a stance.
     */
    if (kind == 1 && info && IsStanceOnly(info))
        kind = 3;

    /*
     * THE ACTOR FIRST, AND A REFUSAL IS COUNTED.
     *
     * LogSlotFor hands back 0xFF once the roster is full, and dropping the event on that used to
     * happen silently - so a saturated roster threw work away while the trailer still reported
     * the log as whole, and the AddOn would have shown a truncated log with no warning on it.
     *
     * The victim is resolved only after the actor has a slot, so an event that is about to be
     * discarded cannot spend the last free slot on a name nothing will ever refer to.
     */
    uint8 const actorSlot = LogSlotFor(actor);
    if (actorSlot == 0xFF)
    {
        ++m_logDropped;
        return -1;
    }

    ArenaLogEvent one;
    one.atDeci      = m_matchTimer / 100;
    one.spellId     = info ? info->Id : 0;
    one.amount      = amount;
    one.hp          = hp;
    one.haveNumbers = haveNumbers;

    /*
     * SET HERE, WITH THE NUMBER IT DESCRIBES, and never as a second event afterwards. A crit is
     * an adjective on an amount, not an occurrence of its own; appending a separate "and that one
     * was a crit" entry would have needed no signature change at all, which is exactly why it is
     * tempting - and it decorates the wrong line the moment anything is logged in between, which
     * a triggered spell, a split or an inline proc does routinely.
     */
    one.flags       = crit ? uint8(ARENA_LOG_FLAG_CRIT) : uint8(0);

    one.kind        = kind;
    one.actor       = actorSlot;
    one.victim      = LogSlotFor(victim);

    m_combatLog.push_back(one);
    return int32(m_combatLog.size()) - 1;
}

/*
 * The numbers of an entry that is already written.
 *
 * The cast is recorded before the spell's effects run, and the damage is known a moment later -
 * so "Mortal Strike" and "1240" are one line rather than two. Spell carries the index across that
 * gap, which is exact where a backwards search through the buffer would have been a guess: there
 * is no bound on how many events a triggered spell, a split, or an inline proc can append in
 * between.
 *
 * The index is checked rather than trusted. A match can end between the cast and the impact of a
 * slow projectile, and Reset would have emptied the buffer underneath it.
 */
void Arena::LogFill(int32 index, uint32 amount, uint32 hp)
{
    if (index < 0 || size_t(index) >= m_combatLog.size())
        return;

    ArenaLogEvent& one = m_combatLog[index];
    one.amount = amount;
    one.hp = hp;
    one.haveNumbers = true;
}

/*
 * SENDING IT, once the gates have closed.
 *
 * The cursor is PER RECEIVER. A single shared position is the easy way to get this quietly
 * wrong: somebody who drops and comes back inside the leave window, or a spectator who walks in
 * halfway through the drain, would receive a fragment and never know. An unknown guid defaults
 * to nought and simply gets the whole thing.
 *
 * No visibility filter here, unlike every line the frames send. That is safe only because this
 * is after the match - the scoreboard is public by then. It would NOT be safe live: a running
 * feed would hand back exactly what the stealth filter exists to withhold.
 *
 * Three things go out, in order: the roster, the spell names, then the events. The AddOn can
 * render an event only once it holds both of the first two, and they are small enough to be
 * gone within a second or two of the gates closing.
 */
void Arena::DrainCombatLog(uint32 diff)
{
    if (m_logDrainTimer > diff)
    {
        m_logDrainTimer -= diff;
        return;
    }
    m_logDrainTimer = ARENA_FRAME_PUSH_INTERVAL;

    /*
     * THE DISTINCT SPELLS, collected once on the first tick after the gates close.
     *
     * The events carry a spell id and an icon but no NAME, and the 1.12 client cannot turn an
     * id into a name - there is no GetSpellInfo, and GetSpellName reaches only your own book.
     * Putting the name in every event would be the obvious fix and the wrong one: thirty
     * characters times seven hundred events is fourteen kilobytes of mostly the same words.
     *
     * A dictionary instead. A 3v3 uses perhaps eighty distinct spells, so this is under two
     * kilobytes and each name travels once.
     */
    if (m_logSpells.empty() && !m_combatLog.empty())
    {
        std::set<uint32> distinct;
        for (size_t i = 0; i < m_combatLog.size(); ++i)
            if (m_combatLog[i].spellId && distinct.insert(m_combatLog[i].spellId).second)
                m_logSpells.push_back(m_combatLog[i].spellId);
    }

    /*
     * HasBgMap, not a null test on GetBgMap: that one ASSERTS rather than returning nullptr, so
     * "if (!map)" was dead code guarding nothing. It matters here more than anywhere else in this
     * file - the drain is the one thing that runs while the match is being torn down.
     */
    if (!HasBgMap())
        return;

    BattleGroundMap* map = GetBgMap();

    for (auto const& itr : map->GetPlayers())
    {
        Player* receiver = itr.getSource();
        if (!receiver || !receiver->GetSession())
            continue;

        ObjectGuid const guid = receiver->GetObjectGuid();
        uint32 budget = ARENA_LOG_LINES_PER_TICK;

        /*
         * THE ROSTER, and only once: the a| line stops when the match does, so somebody arriving
         * now has no names, no class colours and no health pools without it. It goes out whole
         * and off budget, because nothing at all can be drawn before it lands.
         *
         * Not "one or two lines", which this used to claim. The roster is not the fighters: the
         * cast hook needs only the TARGET to be a player, so every warlock pet and every totem a
         * shaman drops takes a slot of its own, with a creature name that beats a player's twelve
         * characters. A long 5v5 with shamans reaches thirty or forty of them.
         *
         * THE STARTING SLOT IS IN THE LINE, and that is not decoration. A 5v5 with pets overruns
         * ARENA_FRAME_MAX_PAYLOAD, the header splits, and a second line carrying no offset would
         * be read as slots nought upward - every name after the split attributed to the wrong
         * man. So it says where it begins.
         */
        if (m_logHeaderSent.find(guid) == m_logHeaderSent.end())
        {
            size_t s = 0;
            do
            {
                size_t const began = s;
                std::ostringstream head;
                head << "g|" << (m_matchTimer / 100) << "," << s;

                while (s < m_logRoster.size())
                {
                    std::ostringstream one;
                    one << ";" << m_logRoster[s] << "," << uint32(m_logClass[s])
                        << "," << m_logMaxHp[s];

                    // the first entry of a line always goes in, even if it alone is over the
                    // limit: refusing it would leave s standing still and spin this loop forever
                    if (s > began &&
                        size_t(head.tellp()) + one.str().size() > ARENA_FRAME_MAX_PAYLOAD)
                        break;

                    head << one.str();
                    ++s;
                }

                SendArenaAddon(receiver, head.str());
            }
            while (s < m_logRoster.size());

            m_logHeaderSent.insert(guid);
        }

        // his language, because these are words and two watchers legitimately need different bytes
        LocaleConstant const locale = receiver->GetSession()->GetSessionDbcLocale();

        size_t& dict = m_logDictCursor[guid];
        while (dict < m_logSpells.size() && budget)
        {
            size_t const began = dict;
            std::ostringstream line;
            line << "y|";

            while (dict < m_logSpells.size())
            {
                SpellEntry const* info = sSpellMgr.GetSpellEntry(m_logSpells[dict]);
                if (!info)
                {
                    ++dict;                     // it went away since the cast; skip it silently
                    continue;
                }

                std::string const& name = info->SpellName[locale].empty() ?
                                          info->SpellName[LOCALE_enUS] : info->SpellName[locale];

                // the icon rides HERE and not on every event: it is a property of the spell, and
                // sending it per event cost five bytes a line to say the same thing again
                std::ostringstream one;
                one << ";" << m_logSpells[dict] << "," << uint32(info->SpellIconID) << "," << name;

                if (dict > began &&
                    size_t(line.tellp()) + one.str().size() > ARENA_FRAME_MAX_PAYLOAD)
                    break;

                line << one.str();
                ++dict;
            }

            if (line.str().size() > 2)          // a run of missing spells writes an empty line
                SendArenaAddon(receiver, line.str());
            --budget;
        }

        if (!budget)
            continue;

        size_t& cursor = m_logCursor[guid];
        while (cursor < m_combatLog.size() && budget)
        {
            /*
             * An absolute base per line and a small delta inside it. That saves bytes, and more
             * usefully it means a line that never arrives cannot shift the timeline of the ones
             * that do - every line carries its own anchor.
             */
            size_t const began = cursor;
            uint32 const base = m_combatLog[cursor].atDeci;
            std::ostringstream line;
            line << "l|" << base;

            while (cursor < m_combatLog.size())
            {
                ArenaLogEvent const& ev = m_combatLog[cursor];

                // out of the delta reach; the next line anchors again
                if (cursor > began && ev.atDeci - base > 999)
                    break;

                /*
                 * AN EMPTY FIELD IS NOT A ZERO, and the last two fields depend on it.
                 *
                 * A hit that a shield ate whole never reaches the damage hook, so its cast entry
                 * carries no amount at all - and writing a bare 0 there would read as a hit that
                 * did nothing, which is the opposite of what happened. Empty means "not
                 * recorded", exactly as the victim field already does for an event with no
                 * target.
                 */
                std::ostringstream one;
                one << ";" << (ev.atDeci - base) << ","
                    << uint32(ev.actor) << ","
                    << (ev.victim == 0xFF ? std::string() : std::to_string(uint32(ev.victim))) << ","
                    << ev.spellId << ","
                    << uint32(ev.kind) << ",";

                if (ev.haveNumbers)
                    one << ev.amount << "," << ev.hp;
                else
                    one << ",";

                /*
                 * FIELD EIGHT, WRITTEN ONLY WHEN THERE IS SOMETHING IN IT.
                 *
                 * An entry is seven fields today and eight when the flags byte is set, and the
                 * AddOn tells them apart the way it already tells the current shape from the old
                 * one: by counting, with "seven or more" rather than "seven exactly". That is what
                 * makes this safe in BOTH directions of a rolling deploy - an AddOn that has never
                 * heard of flags takes the branch it takes today and drops the field, and one that
                 * has, reading a server that has not, finds nothing there and claims no crit.
                 *
                 * OMISSION IS THE ENCODING OF NOUGHT, and that is only allowed because a flags
                 * byte has no third state. The two fields above CANNOT do this - "no amount
                 * recorded" and "an amount of nought" are different things and the empty field is
                 * what keeps them apart - but a flag is either raised or it is not.
                 *
                 * It sits OUTSIDE the branch above on purpose, and that is the whole point of
                 * taking the crit at the cast. A Pyroblast a shield ate whole has no amount and no
                 * health and knows perfectly well that it crit; folding this inside would throw
                 * away the one line worth reading.
                 *
                 * NOTHING MAY EVER BE APPENDED AFTER THIS. Because absence is positional, a ninth
                 * field would make an entry that omitted its flags look like one that set them,
                 * and every ordinary hit in the log would turn into a crit. That is why the field
                 * is an integer bitmask and not a bare "1": whatever wants saying next is a bit in
                 * here, at the same two bytes. See ARENA_LOG_FLAG_CRIT.
                 */
                if (ev.flags)
                    one << "," << uint32(ev.flags);

                if (cursor > began &&
                    size_t(line.tellp()) + one.str().size() > ARENA_FRAME_MAX_PAYLOAD)
                    break;

                line << one.str();
                ++cursor;
            }

            SendArenaAddon(receiver, line.str());
            --budget;
        }

        /*
         * The trailer, ONCE - it used to go out on every tick once the cursor ran dry, which is
         * two minutes of it. Its ABSENCE is load bearing: a log with no z| is an incomplete one,
         * and the count inside lets the AddOn say so instead of pretending it is whole.
         */
        if (cursor >= m_combatLog.size() && dict >= m_logSpells.size() &&
            m_logTrailerSent.find(guid) == m_logTrailerSent.end())
        {
            std::ostringstream tail;
            tail << "z|" << m_combatLog.size() << "," << m_logDropped;
            SendArenaAddon(receiver, tail.str());
            m_logTrailerSent.insert(guid);
        }
    }
}

void Arena::HandleKillPlayer(Player* pVictim, Player* pKiller)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    BattleGround::HandleKillPlayer(pVictim, pKiller);

    // no insignia in arenas
    pVictim->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE);

    // no self resurrection (Reincarnation, Twisting Nether ...): dead is dead in the arena
    pVictim->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);

    /*
     * ONCE PER MAN, and the FIRST time is the one worth having.
     *
     * A Spirit of Redemption priest reaches this twice for one death. Unit::Kill casts the form
     * (27827) and then falls through to HandleKillPlayer at the bottom of the same function, so
     * the first call arrives WITH the killer and with the form already on him; fifteen seconds
     * later the form ends, 27965 kills him for real, and the second call arrives with no killer
     * at all.
     *
     * This used to test "does he NOT have 27827", which kept exactly the wrong one of the two:
     * the killing blow was thrown away and the log showed the priest dying alone, fifteen seconds
     * after he actually fell.
     *
     * A set rather than another aura test, because it does not care WHY the second call happens -
     * a death is a once-per-man event in an arena, where there is no resurrection at all
     * (PLAYER_SELF_RES_SPELL is cleared two lines above).
     *
     * No killer is also the environmental and self-inflicted case: he is recorded as his own
     * actor, and the AddOn renders that as "died".
     */
    if (m_logDeaths.insert(pVictim->GetObjectGuid()).second)
        // with numbers, so the health line does not simply stop at the row that matters most:
        // nought of his pool draws the empty socket rather than no bar at all
        LogEvent(pKiller ? static_cast<Unit*>(pKiller) : static_cast<Unit*>(pVictim),
                 pVictim, nullptr, 7, 0, 0, true);

    PlaySoundToAll(SOUND_ARENA_KILL);
    CheckWinConditions();
}

void Arena::UpdatePlayerScore(Player* source, uint32 type, uint32 value)
{
    BattleGroundScoreMap::iterator itr = m_playerScores.find(source->GetObjectGuid());
    if (itr == m_playerScores.end())
        return;

    switch (type)
    {
        case SCORE_DAMAGE_DONE:
            ((ArenaScore*)itr->second)->damageDone += value;
            break;
        case SCORE_HEALING_DONE:
            ((ArenaScore*)itr->second)->healingDone += value;
            break;
        default:
            BattleGround::UpdatePlayerScore(source, type, value);
            break;
    }
}

/*
 * Counted from the scoreboard rows and not from the player list, so that damage dealt by somebody
 * who has since left still counts for his side. He is on the scoreboard everybody reads at the end
 * (his row is kept, see RemovePlayerAtLeave), and a time limit decided on a different tally than
 * the one on screen is a result nobody can check - worse, it would let a player hand the win to the
 * other side by walking out with his damage in the last seconds.
 *
 * The row carries the side itself: a leaver has no team in the battleground any more.
 */
uint32 Arena::GetTeamDamageDone(Team team) const
{
    uint32 damage = 0;
    for (const auto& itr : m_playerScores)
    {
        ArenaScore const* score = static_cast<ArenaScore const*>(itr.second);
        if (score->team == team)
            damage += score->damageDone;
    }
    return damage;
}

uint32 Arena::GetRemainingTime() const
{
    uint32 const limit = sWorld.getConfig(CONFIG_UINT32_ARENA_TIME_LIMIT_MINUTES) * MINUTE * IN_MILLISECONDS;

    /*
     * BEFORE the match, not "any time it is not running".
     *
     * EndBattleGround sets STATUS_WAIT_LEAVE and then calls UpdateWorldStates, so with the old
     * test the very last clock the players saw was the FULL time limit - a fight decided at
     * 1:40 ended with 25:00 on the screen. m_matchTimer stops advancing at the same moment, so
     * after the gates close it already holds the answer.
     */
    if (GetStatus() < STATUS_IN_PROGRESS)
        return limit;

    return m_matchTimer < limit ? limit - m_matchTimer : 0;
}

void Arena::CheckWinConditions()
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    uint32 const aliveAlliance = GetAlivePlayersCountByTeam(ALLIANCE);
    uint32 const aliveHorde = GetAlivePlayersCountByTeam(HORDE);

    // in .debug bg mode a match can run with one team only
    bool const testing = sBattleGroundMgr.isTesting();
    bool const allianceOut = !aliveAlliance && (!testing || GetPlayersCountByTeam(ALLIANCE));
    bool const hordeOut = !aliveHorde && (!testing || GetPlayersCountByTeam(HORDE));

    if (allianceOut && hordeOut)
        EndBattleGround(TEAM_NONE);
    else if (allianceOut)
        EndBattleGround(HORDE);
    else if (hordeOut)
        EndBattleGround(ALLIANCE);
    else if (m_timeLimitReached)
    {
        // time is up: the team with the most damage done wins
        uint32 const damageAlliance = GetTeamDamageDone(ALLIANCE);
        uint32 const damageHorde = GetTeamDamageDone(HORDE);
        if (damageAlliance > damageHorde)
            EndBattleGround(ALLIANCE);
        else if (damageHorde > damageAlliance)
            EndBattleGround(HORDE);
        else
            EndBattleGround(TEAM_NONE);
    }
}

/*********************************************************/
/***                      RATING                       ***/
/*********************************************************/

/*
 * Everybody on this side came in as one party that filled the bracket by itself.
 *
 * Asked of how they QUEUED, not of the party they happen to be in when the gates open. A party is
 * not a fixed thing: the orb hands out the invite, and in the minutes until the doors open anybody
 * may be invited into a group, leave one, or disband one - the group invite has no idea a
 * battleground queue exists. Reading the party here let three players who queued as strangers group
 * up in the start box and collect a premade's rating, and let a party that saw it was losing
 * disband itself to make the match unrated. The queue's own answer cannot be edited afterwards.
 *
 * A side of one is a premade by definition, which is what makes every 1v1 rated without anybody
 * having to form a group: a solo entry fills the 1v1 bracket exactly, so the queue already marks it
 * as full (BattleGroundQueue::StampArenaRating).
 *
 * The party is still collected, but only for the caller: it is used to refuse a match in which one
 * party stands on both sides, and it decides nothing about this side.
 */
bool Arena::IsSidePremade(Team team, Group** party) const
{
    if (party)
        *party = nullptr;

    Group* common = nullptr;
    uint32 count = 0;
    bool premade = true;

    for (const auto& itr : m_players)
    {
        if (itr.second.playerTeam != team)
            continue;

        Player* player = sObjectMgr.GetPlayer(itr.first);
        if (!player)                                        // gone already: can not prove he came with the others
            return false;

        ++count;
        if (!player->QueuedAsFullArenaGroup())
            premade = false;

        // the party is only collected for the caller, it decides nothing here
        Group* own = player->GetOriginalGroup();
        if (count == 1)
            common = own;
        else if (own != common)
            common = nullptr;
    }

    if (party)
        *party = common;

    return count > 0 && premade;
}

/*
 * Does this match count, and who is in it. Asked once, when the gates open: at that point the
 * boxes hold exactly the people who are going to fight, which is not true any earlier - the queue
 * can still be sending a replacement during the preparation.
 */
void Arena::DetermineRated()
{
    m_rated = false;
    m_ratedSettled = false;
    m_ratedRoster.clear();

    /*
     * Whichever way this goes, the players are told. An unrated match used to look exactly like a
     * rated one until the result came in with no numbers on it, and "why did that not count" is not
     * a question anybody should have to ask twice.
     */
    ArenaType const type = GetArenaType();
    if (!sArenaRatingMgr.IsRatedBracket(type))
    {
        SendReasonToAll(LANG_ARENA_NOT_RATED,
                        sArenaRatingMgr.IsAvailable() ? LANG_ARENA_NR_RATING_OFF : LANG_ARENA_NR_NO_TABLE);
        return;
    }

    // Both sides complete, or nothing. A rating taken off an uneven match says nothing about
    // anybody, and .debug bg matches (which may run one-sided) have no business in a ladder.
    if (GetPlayersCountByTeam(ALLIANCE) != uint32(type) || GetPlayersCountByTeam(HORDE) != uint32(type))
    {
        SendReasonToAll(LANG_ARENA_NOT_RATED, LANG_ARENA_NR_NOT_FULL);
        return;
    }

    /*
     * A ladder is a comparison, and it only means something between characters who have everything
     * their class is ever going to give them. Below the level cap a match is decided by who levelled
     * further as much as by who played better, so those fights happen - the arena is open to anybody
     * the queue lets in - they simply do not move anybody's rating.
     *
     * Asked of the whole field rather than of one side: a single character short of the cap makes
     * the whole result meaningless, not just his own.
     */
    uint32 const minLevel = sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_MIN_LEVEL);
    for (const auto& itr : m_players)
    {
        Player* player = sObjectMgr.GetPlayer(itr.first);
        if (player && player->GetLevel() >= minLevel)
            continue;

        // he left between the gates and this line, so his level cannot be established - and a rating
        // handed out on an assumption is worse than one not handed out at all
        SendReasonToAll(LANG_ARENA_NOT_RATED, LANG_ARENA_NR_LEVEL, minLevel);
        return;
    }

    /*
     * BOTH SIDES ARE ASKED, ALWAYS - and && would not do it.
     *
     * IsSidePremade writes the party through its out parameter even when it returns FALSE, and
     * that party is the whole point of the second call. Written as one && expression, a
     * non-premade alliance side short-circuited the horde call away, so hordeParty kept the
     * nullptr from the line above and the "one party on both sides" test below compared a real
     * group against nothing and never fired.
     *
     * It failed in exactly the case it exists for. Four friends in one party queue SEPARATELY:
     * each entry is one man, so StampArenaRating marks none of them as a full arena group,
     * neither side is premade, and mode 2 rates the match without ever looking at parties. They
     * could hand each other rating for as long as they liked.
     */
    Group* allianceParty = nullptr;
    Group* hordeParty = nullptr;
    bool const alliancePremade = IsSidePremade(ALLIANCE, &allianceParty);
    bool const hordePremade = IsSidePremade(HORDE, &hordeParty);
    bool const bothPremade = alliancePremade && hordePremade;

    /*
     * One party standing on both sides is not a match, it is a transfer, and the premade rule can
     * not see it: each side really is one party - the same one. Its members only have to queue
     * separately, which the mixed queue then sorts across the two sides, and they can hand each
     * other rating for as long as they like. Asked whatever the mode says, because mode 2 rates
     * such a match without ever looking at parties at all.
     */
    if (allianceParty && allianceParty == hordeParty)
    {
        SendReasonToAll(LANG_ARENA_NOT_RATED, LANG_ARENA_NR_SAME_PARTY);
        return;
    }

    if (sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_MODE) == ARENA_RATED_PREMADE && !bothPremade)
    {
        SendReasonToAll(LANG_ARENA_NOT_RATED, LANG_ARENA_NR_NEED_PARTY);
        return;
    }

    for (const auto& itr : m_players)
    {
        ArenaRatingEntry const entry = sArenaRatingMgr.Get(itr.first, type);

        ArenaRatedParticipant participant;
        participant.guid = itr.first;
        participant.team = itr.second.playerTeam;
        participant.rating = entry.rating;
        participant.mmr = entry.mmr;
        m_ratedRoster.push_back(participant);
    }

    m_rated = true;
    PSendMessageToAll(LANG_ARENA_RATED_MATCH, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr);
}

/*
 * Books the result. Called before the base class builds the final scoreboard packet, because the
 * new rating and the change travel in it as two more columns.
 *
 * Everybody on the roster is settled, including whoever walked out or logged off in between: the
 * alternative is a free pass for leaving a lost match, and the deserter debuff alone has never
 * stopped anyone.
 */
void Arena::ApplyRatedResult(Team winner)
{
    if (!m_rated || m_ratedSettled)
        return;

    // m_rated itself stays set - the match WAS rated, and the scoreboard still standing open for
    // the next two minutes should not start claiming otherwise. Only the booking happens once.
    m_ratedSettled = true;
    ArenaType const type = GetArenaType();

    uint32 mmrSum[BG_TEAMS_COUNT] = { 0, 0 };
    uint32 headCount[BG_TEAMS_COUNT] = { 0, 0 };
    for (auto const& participant : m_ratedRoster)
    {
        BattleGroundTeamIndex const idx = GetTeamIndexByTeamId(participant.team);
        mmrSum[idx] += participant.mmr;
        ++headCount[idx];
    }

    if (!headCount[BG_TEAM_ALLIANCE] || !headCount[BG_TEAM_HORDE])
        return;

    // The side's matchmaking rating is the average of its members, exactly as on retail: a player
    // is matched by what his side is worth, not by what he alone is worth.
    uint32 const teamMmr[BG_TEAMS_COUNT] =
    {
        mmrSum[BG_TEAM_ALLIANCE] / headCount[BG_TEAM_ALLIANCE],
        mmrSum[BG_TEAM_HORDE] / headCount[BG_TEAM_HORDE]
    };

    for (auto const& participant : m_ratedRoster)
    {
        BattleGroundTeamIndex const own = GetTeamIndexByTeamId(participant.team);
        BattleGroundTeamIndex const other = GetOtherTeamIndex(own);
        bool const won = winner != TEAM_NONE && participant.team == winner;

        int32 ratingChange;
        int32 mmrChange;
        if (winner == TEAM_NONE)
        {
            // A draw costs both sides the same and leaves the matchmaking rating untouched: nobody
            // proved anything, so the queue should keep pairing them the way it did.
            ratingChange = -int32(sWorld.getConfig(CONFIG_UINT32_ARENA_RATED_DRAW_LOSS));
            mmrChange = 0;
        }
        else
        {
            ratingChange = ArenaRatingMgr::GetRatingMod(participant.rating, teamMmr[other], won);
            mmrChange = ArenaRatingMgr::GetMatchmakerRatingMod(teamMmr[own], teamMmr[other], won);
        }

        ArenaRatingEntry const result = sArenaRatingMgr.Apply(participant.guid, type, ratingChange, mmrChange, won);

        // the scoreboard columns - a leaver has one too, his row is kept (see RemovePlayerAtLeave),
        // so what the match cost him is on the board next to everybody else's
        BattleGroundScoreMap::const_iterator score = m_playerScores.find(participant.guid);
        if (score != m_playerScores.end())
        {
            ArenaScore* arenaScore = static_cast<ArenaScore*>(score->second);
            arenaScore->newRating = result.rating;
            // what he really gained, after the floor at zero clipped the loss
            arenaScore->ratingChange = int32(result.rating) - int32(participant.rating);
        }

        // and in words, because the scoreboard closes and the chat log does not
        if (Player* player = sObjectMgr.GetPlayer(participant.guid))
            ChatHandler(player).PSendSysMessage(LANG_ARENA_RATING_RESULT, ArenaMgr::BracketName(player, type),
                                                result.rating, int32(result.rating) - int32(participant.rating), result.mmr);
    }

    m_ratedRoster.clear();
}

void Arena::EndBattleGround(Team winner)
{
    if (GetStatus() == STATUS_WAIT_LEAVE)
        return;

    // before the base class: it builds the final scoreboard packet, which carries the new rating
    ApplyRatedResult(winner);

    for (const auto& itr : m_players)
        if (Player* player = sObjectMgr.GetPlayer(itr.first))
            RestorePlayer(player, true);

    BattleGround::EndBattleGround(winner);

    if (winner == TEAM_NONE)
    {
        SendMessageToAll(BCT_ARENA_DRAW, CHAT_MSG_BG_SYSTEM_NEUTRAL);
        // no-show abort: remove everybody quickly; a real draw keeps the normal scoreboard time
        if (m_matchTimer < MINUTE * IN_MILLISECONDS)
            SetEndTime(ARENA_TIME_TO_AUTOREMOVE_ABORTED);
    }

    UpdateWorldStates();
}

/*********************************************************/
/***                    WORLD STATES                   ***/
/*********************************************************/

void Arena::UpdateWorldStates()
{
    UpdateWorldState(WORLD_STATE_ARENA_ALIVE_PLAYERS_RED, GetAlivePlayersCountByTeam(HORDE));
    UpdateWorldState(WORLD_STATE_ARENA_ALIVE_PLAYERS_BLUE, GetAlivePlayersCountByTeam(ALLIANCE));
    UpdateTimeWorldStates(GetRemainingTime());
}

void Arena::UpdateTimeWorldStates(uint32 remainingMs)
{
    uint32 const totalSeconds = remainingMs / IN_MILLISECONDS;
    UpdateWorldState(WORLD_STATE_ARENA_TIME_MINUTES, totalSeconds / MINUTE);
    UpdateWorldState(WORLD_STATE_ARENA_TIME_SECONDS, totalSeconds % MINUTE);
}

void Arena::FillInitialWorldStates(WorldPacket& data, uint32& count)
{
    uint32 const totalSeconds = GetRemainingTime() / IN_MILLISECONDS;
    FillInitialWorldState(data, count, WORLD_STATE_ARENA_ALIVE_PLAYERS_RED, GetAlivePlayersCountByTeam(HORDE));
    FillInitialWorldState(data, count, WORLD_STATE_ARENA_ALIVE_PLAYERS_BLUE, GetAlivePlayersCountByTeam(ALLIANCE));
    FillInitialWorldState(data, count, WORLD_STATE_ARENA_TIME_MINUTES, totalSeconds / MINUTE);
    FillInitialWorldState(data, count, WORLD_STATE_ARENA_TIME_SECONDS, totalSeconds % MINUTE);
}

/*********************************************************/
/***                   NAGRAND ARENA                   ***/
/*********************************************************/

/*
 * A random point on the arena floor - the one place that knows where the sand is.
 *
 * The old version took a point on a ring up to 45 yd out and walked there whatever was in the way.
 * The Ring of Trials is not a circle: on the gate axis the floor reaches past 50 yd, across it far
 * less, so a point picked blind lands in a wall often enough. Every candidate is now measured
 * against the floor height and rejected if it is not standing on sand, which needs no knowledge of
 * the shape at all.
 *
 * Returns false if no point could be found, which the caller treats as "try again shortly".
 */
bool ArenaMgr::PickNagrandFloorPoint(Map* map, float& x, float& y, float& z)
{
    for (uint8 tries = 0; tries < 10; ++tries)
    {
        float const angle = frand(0.0f, 2.0f * M_PI_F);
        float const distance = frand(ARENA_NA_TORNADO_RADIUS_MIN, ARENA_NA_TORNADO_RADIUS_MAX);
        float const cx = ARENA_NA_CENTER_X + distance * cos(angle);
        float const cy = ARENA_NA_CENTER_Y + distance * sin(angle);

        float const groundZ = map->GetHeight(cx, cy, ARENA_NA_FLOOR_Z + 5.0f, true, 20.0f);
        if (groundZ <= INVALID_HEIGHT || std::fabs(groundZ - ARENA_NA_FLOOR_Z) > ARENA_NA_FLOOR_TOLERANCE)
            continue;                                       // a wall, a ramp, or the pit outside the ring

        x = cx;
        y = cy;
        z = groundZ + 0.05f;
        return true;
    }
    return false;
}

bool Arena::SummonTornado()
{
    float x, y, z;
    if (!ArenaMgr::PickNagrandFloorPoint(GetBgMap(), x, y, z))
        return false;                                       // no floor point this time, the caller retries

    if (!GetBgMap()->SummonCreature(NPC_ARENA_TORNADO, x, y, z, frand(0.0f, 2.0f * M_PI_F), TEMPSUMMON_MANUAL_DESPAWN, 0))
    {
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "[Arena] Could not summon tornado on map %u instance %u.", GetMapId(), GetInstanceID());
        return false;
    }
    return true;
}

/*********************************************************/
/***                   DALARAN SEWERS                  ***/
/*********************************************************/

// The water doodads are doors: closed = water visible / solid, open = water gone.
void Arena::SetWaterActive(uint8 event1, bool active)
{
    BGObjects const& objects = m_eventObjects[MAKE_PAIR32(event1, 0)].gameobjects;
    for (const auto& guid : objects)
    {
        if (active)
            DoorClose(guid);
        else
            DoorOpen(guid);
    }
}

void Arena::DoWaterFlush()
{
    for (uint8 event1 = ARENA_EVENT_DS_WATERSPOUT_1; event1 <= ARENA_EVENT_DS_WATERSPOUT_2; ++event1)
        if (Creature* waterSpout = GetBgMap()->GetCreature(GetSingleCreatureGuid(event1, 0)))
        {
            waterSpout->CastSpell(waterSpout, SPELL_ARENA_DS_FLUSH, true);

            // The spell itself is mute (see SOUND_ARENA_DS_WATER_FLUSH). The spouts only wear the
            // invisible stalker model 11686 - they are ordinary units the client loads, which is why
            // their spell is visible - so the sound comes out of the pipe it belongs to instead of
            // being played flat to everybody.
            waterSpout->PlayDistanceSound(SOUND_ARENA_DS_WATER_FLUSH);
        }
}

void Arena::DoWaterfallKick()
{
    Creature* kicker = GetBgMap()->GetCreature(GetSingleCreatureGuid(ARENA_EVENT_DS_WATERFALL_KICKER, 0));
    if (!kicker)
        return;

    for (const auto& itr : m_players)
    {
        Player* player = sObjectMgr.GetPlayer(itr.first);
        if (!player || !player->IsAlive() || player->IsArenaSpectator() || !player->IsInMap(kicker))
            continue;

        if (kicker->GetDistance(player) < 5.0f)
        {
            player->KnockBackFrom(kicker, 5.0f, 20.0f);
        }
    }
}

// Players standing inside a starting pipe are pushed straight through the pipe into the arena.
// The direction is the line from the pipe kicker to the water spout of the same pipe, so nobody
// gets thrown against the pipe walls.
void Arena::KickFromPipes()
{
    for (uint8 event1 = ARENA_EVENT_DS_PIPE_KICKER_1; event1 <= ARENA_EVENT_DS_PIPE_KICKER_2; ++event1)
    {
        Creature* kicker = GetBgMap()->GetCreature(GetSingleCreatureGuid(event1, 0));
        if (!kicker)
            continue;

        Creature* target = nullptr;
        for (uint8 spoutEvent = ARENA_EVENT_DS_WATERSPOUT_1; spoutEvent <= ARENA_EVENT_DS_WATERSPOUT_2 && !target; ++spoutEvent)
            if (Creature* spout = GetBgMap()->GetCreature(GetSingleCreatureGuid(spoutEvent, 0)))
                if (spout->GetDistance(kicker) < 30.0f)
                    target = spout;

        if (!target)
            continue;

        float const angle = kicker->GetAngle(target);
        for (const auto& itr : m_players)
        {
            Player* player = sObjectMgr.GetPlayer(itr.first);
            if (!player || !player->IsAlive() || player->IsArenaSpectator() || !player->IsInMap(kicker))
                continue;

            float const distance = kicker->GetDistance(player);
            if (distance >= 53.0f)
                continue;

            float const speed = 20.0f + std::max(0.0f, 40.0f - distance);
            player->SetLaunched(true);
            player->KnockBack(angle, speed, 8.0f);
        }
    }
}

bool Arena::HandleAreaTrigger(Player* player, uint32 trigger)
{
    if (!IsDalaranArena())
        return false;

    switch (trigger)
    {
        // somebody went back into the pipes after the flush: kick him out again
        case AT_ARENA_DS_PIPE_1:
        case AT_ARENA_DS_PIPE_2:
            if (GetStatus() == STATUS_IN_PROGRESS && m_pipeKnockbackCount >= ARENA_DS_PIPE_KNOCKBACK_COUNT)
            {
                KickFromPipes();
                DoWaterFlush();
            }
            return true;
        // outside of the arena: back in
        case AT_ARENA_DS_OUTSIDE_1:
            player->NearTeleportTo(1290.44f, 744.96f, 3.16f, 1.6f);
            return true;
        case AT_ARENA_DS_OUTSIDE_2:
            player->NearTeleportTo(1292.6f, 837.07f, 3.161f, 4.7f);
            return true;
        case AT_ARENA_DS_OUTSIDE_3:
            player->NearTeleportTo(1250.68f, 790.86f, 3.16f, 0.0f);
            return true;
        case AT_ARENA_DS_OUTSIDE_4:
            player->NearTeleportTo(1332.50f, 790.9f, 3.16f, 3.14f);
            return true;
        case AT_ARENA_DS_UNDER_MAP_1:
        case AT_ARENA_DS_UNDER_MAP_2:
        case AT_ARENA_DS_UNDER_MAP_3:
            player->NearTeleportTo(1330.0f, 800.0f, 3.16f, player->GetOrientation());
            return true;
    }
    return false;
}

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

    m_worldStateTimer = 0;
    m_matchTimer = 0;
    m_doorsDespawnTimer = 0;
    m_lastCountdownSecond = 0;
    m_playersReady = false;
    m_timeLimitReached = false;
    m_preparationExtended = false;
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
    m_castLineSent = false;
    m_auraLineSent = false;
    m_leaverIsParticipant = false;
    m_spectatorsRemoved = false;
    m_rated = false;
    m_ratedSettled = false;
    m_ratedRoster.clear();
    delete m_keptScore;
    m_keptScore = nullptr;
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

    m_worldStateTimer = 0;
    m_matchTimer = 0;
    m_doorsDespawnTimer = 0;
    m_lastCountdownSecond = 0;
    m_playersReady = false;
    m_timeLimitReached = false;
    m_preparationExtended = false;
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
    m_castLineSent = false;
    m_auraLineSent = false;
    m_leaverIsParticipant = false;
    m_spectatorsRemoved = false;
    m_rated = false;
    m_ratedSettled = false;
    m_ratedRoster.clear();
    delete m_keptScore;
    m_keptScore = nullptr;
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

    bool IsImmunityMechanic(uint32 mechanic)
    {
        return mechanic == MECHANIC_IMMUNE_SHIELD || mechanic == MECHANIC_INVULNERABILITY;
    }

    /*
     * The cooldowns the frames report.
     *
     * Every id was read out of the client's own Spell.dbc rather than remembered, and one of them
     * came back wrong when it was: 12292 is Sweeping Strikes, not Death Wish, which is 12328.
     *
     * These are buffs, not mechanics, so unlike the control effects there is no way round naming
     * them one by one - and that is survivable here because almost every big cooldown in 1.12 has
     * exactly one rank, which is the thing that makes a spell id list rot everywhere else.
     */
    uint32 const ARENA_TRACKED_COOLDOWNS[] =
    {
        1719,  20230, 871,   12975, 12328, 12292,          // warrior
        5277,  13877, 13750,                               // rogue
        19263, 3045,  19574, 5384,                         // hunter
        11958, 12051, 11129, 12042,                        // mage
        14751, 6346,                                       // priest
        642,   1020,  498,   5573,  1022,  20216,          // paladin
        16188, 16166,                                      // shaman
        22812, 17116, 29166,                               // druid
        7744,  20589, 20594, 20600, 20554, 26296, 26297,   // racials
    };

    struct TrackedAura
    {
        uint32 id;                              // a mechanic (1..30) or a spell id (498 and up)
        int32 remaining;
        bool onCooldown;                        // false: happening now. true: used, and this is the wait
    };

    // Two of them, and the frames have room for two beside the bar. The first version sent one
    // because the icon sat INSIDE the health bar, where a second would have had nowhere to go.
    size_t const ARENA_FRAME_MAX_AURAS = 2;

    /*
     * The effects on this player worth a place on his frame, best first.
     *
     * Not all of them: three or more at once is a wall of icons that says less than two do, and
     * the two that matter are always at the front of this order. Immunity outranks control -
     * being unable to hurt him matters more than his being unable to move - and among equals the
     * one with the most time left wins, because that is the one still there when you reach him.
     */
    void FindTrackedAuras(Player* player, std::vector<TrackedAura>& out)
    {
        // what is on him right now
        for (auto const& itr : player->GetSpellAuraHolderMap())
        {
            SpellAuraHolder const* holder = itr.second;
            if (!holder)
                continue;

            SpellEntry const* info = holder->GetSpellProto();
            if (!info)
                continue;

            uint32 found = info->Mechanic;
            for (uint32 i = 0; i < MAX_EFFECT_INDEX && !IsTrackedMechanic(found); ++i)
                found = info->EffectMechanic[i];

            if (!IsTrackedMechanic(found))
                continue;

            TrackedAura aura;
            aura.id = found;
            aura.remaining = holder->GetAuraDuration();
            aura.onCooldown = false;
            out.push_back(aura);
        }

        /*
         * And the big cooldowns: running, or waiting to come back.
         *
         * Nothing is reported for a spell that has not been used, which is the whole rule - the
         * frames must never claim something nobody watched. Arena.ResetAllCooldowns defaults to
         * true, so a match starts with every cooldown clear and one that is running was started
         * HERE, in front of the people looking at it. With that switch off this would leak what
         * happened outside, so it is checked rather than assumed.
         */
        bool const clearStart = sWorld.getConfig(CONFIG_BOOL_ARENA_RESET_ALL_COOLDOWNS);

        for (uint32 spellId : ARENA_TRACKED_COOLDOWNS)
        {
            SpellEntry const* info = sSpellMgr.GetSpellEntry(spellId);
            if (!info)
                continue;

            if (SpellAuraHolder const* holder = player->GetSpellAuraHolder(spellId))
            {
                TrackedAura aura;
                aura.id = spellId;
                aura.remaining = holder->GetAuraDuration();
                aura.onCooldown = false;
                out.push_back(aura);
                continue;
            }

            if (!clearStart)
                continue;

            TimePoint expire;
            bool permanent = false;
            if (!player->GetExpireTime(info, expire, permanent) || permanent)
                continue;

            auto const now = player->GetMap()->GetCurrentClockTime();
            if (expire <= now)
                continue;

            TrackedAura aura;
            aura.id = spellId;
            aura.remaining = int32(std::chrono::duration_cast<std::chrono::milliseconds>(expire - now).count());
            aura.onCooldown = true;
            out.push_back(aura);
        }

        /*
         * What is happening beats what is merely waiting, immunity beats the rest, and among
         * equals the one with the most time left wins - it is the one still there when you reach
         * him. A permanent aura counts as the longest there is, not as the shortest, which is
         * what a -1 would otherwise sort as.
         */
        std::sort(out.begin(), out.end(), [](TrackedAura const& a, TrackedAura const& b)
        {
            if (a.onCooldown != b.onCooldown)
                return !a.onCooldown;

            bool const ai = IsImmunityMechanic(a.id);
            bool const bi = IsImmunityMechanic(b.id);
            if (ai != bi)
                return ai;

            if ((a.remaining < 0) != (b.remaining < 0))
                return a.remaining < 0;

            return a.remaining > b.remaining;
        });

        if (out.size() > ARENA_FRAME_MAX_AURAS)
            out.resize(ARENA_FRAME_MAX_AURAS);
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

    std::ostringstream payload;
    payload << "a|";

    bool any = false;
    for (auto const& itr : GetPlayers())
    {
        Player* player = sObjectMgr.GetPlayer(itr.first);
        if (!player)
            continue;

        uint32 const maxHealth = player->GetMaxHealth();
        uint32 const health = maxHealth ? uint32((uint64(player->GetHealth()) * 100) / maxHealth) : 0;

        Powers const powerType = player->GetPowerType();
        uint32 const maxPower = player->GetMaxPower(powerType);
        uint32 const power = maxPower ? uint32((uint64(player->GetPower(powerType)) * 100) / maxPower) : 0;

        if (any)
            payload << ";";
        any = true;

        payload << player->GetName() << ","
                << uint32(player->GetClass()) << ","
                << (player->GetBGTeam() == ALLIANCE ? 0 : 1) << ","
                << health << ","
                << power << ","
                << uint32(powerType) << ","
                << (player->IsAlive() ? 0 : 1);
    }

    if (!any)
        return;

    std::string const line = payload.str();

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
        std::string caster;
        SpellEntry const* spell;
        uint32 total;
        uint32 remaining;
    };
    std::vector<CastInfo> casts;

    for (auto const& itr : GetPlayers())
    {
        Player* player = sObjectMgr.GetPlayer(itr.first);
        if (!player)
            continue;

        Spell* spell = player->GetCurrentSpell(CURRENT_GENERIC_SPELL);
        if (!spell)
            spell = player->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
        if (!spell || !spell->m_spellInfo)
            continue;

        int32 const total = spell->GetCastTime();
        if (total <= 0)
            continue;                           // instant: there is no bar to draw

        CastInfo info;
        info.caster = player->GetName();
        info.spell = spell->m_spellInfo;
        info.total = uint32(total);
        info.remaining = spell->GetCastedTime();
        casts.push_back(info);
    }

    /*
     * The control effects, in one line for everybody.
     *
     * Unlike the casts this needs no localisation: the AddOn draws an icon and a countdown, not a
     * word, so the same bytes serve every watcher. That is also why it is a mechanic on the wire
     * and not a name - the icon table lives in the AddOn and is fifteen entries long.
     */
    std::ostringstream auras;
    bool anyAura = false;
    for (auto const& itr : GetPlayers())
    {
        Player* player = sObjectMgr.GetPlayer(itr.first);
        if (!player)
            continue;

        std::vector<TrackedAura> found;
        FindTrackedAuras(player, found);
        if (found.empty())
            continue;

        // measured against the same ceiling the cast line uses, and for the same reason: it is
        // the tail that goes, not the packet
        std::ostringstream entry;
        entry << player->GetName();
        for (size_t i = 0; i < found.size(); ++i)
            entry << "," << found[i].id << "," << found[i].remaining
                  << "," << (found[i].onCooldown ? 1 : 0);

        if (size_t(auras.tellp()) + entry.str().size() + 3 > ARENA_FRAME_MAX_PAYLOAD)
            break;

        if (!anyAura)
            auras << "b|";
        else
            auras << ";";
        anyAura = true;

        // a duration of -1 is permanent; the AddOn shows the icon without a countdown
        auras << entry.str();
    }
    std::string const auraLine = anyAura ? auras.str() : std::string();

    // everybody on the map: the fighters and the visitors watching them
    if (Map* map = GetBgMap())
    {
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
        {
            Player* receiver = itr->getSource();
            if (!receiver)
                continue;

            SendArenaAddon(receiver, line);

            if (!auraLine.empty())
                SendArenaAddon(receiver, auraLine);
            else if (m_auraLineSent)
                SendArenaAddon(receiver, "b|");

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
            LocaleConstant const locale = receiver->GetSession() ?
                                          receiver->GetSession()->GetSessionDbcLocale() : LOCALE_enUS;

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
                std::string const& name = casts[i].spell->SpellName[locale].empty() ?
                                          casts[i].spell->SpellName[LOCALE_enUS] :
                                          casts[i].spell->SpellName[locale];

                if (castLine.tellp() + std::streamoff(name.size() + casts[i].caster.size() + 16)
                    > std::streamoff(ARENA_FRAME_MAX_PAYLOAD))
                    break;

                if (written)
                    castLine << ";";
                castLine << casts[i].caster << "," << name << ","
                         << casts[i].total << "," << casts[i].remaining;
                ++written;
            }

            if (written)
                SendArenaAddon(receiver, castLine.str());
            else if (m_castLineSent)
                SendArenaAddon(receiver, "c|");
        }
    }

    m_castLineSent = !casts.empty();
    m_auraLineSent = anyAura;
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

    // spectators (visitors and dead participants) leave silently, fighters are announced
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

void Arena::RemoveSpectators()
{
    Map::PlayerList const& players = GetBgMap()->GetPlayers();
    for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
    {
        Player* player = itr->getSource();
        if (!player || !player->IsArenaSpectator())
            continue;

        // participants are handled by the base class, visitors are only known to the map
        if (m_players.find(player->GetObjectGuid()) != m_players.end())
            continue;

        player->LeaveBattleground(true);
    }
}

/*********************************************************/
/***                   FIGHT / SCORES                  ***/
/*********************************************************/

void Arena::HandleKillPlayer(Player* pVictim, Player* pKiller)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    BattleGround::HandleKillPlayer(pVictim, pKiller);

    // no insignia in arenas
    pVictim->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE);

    // no self resurrection (Reincarnation, Twisting Nether ...): dead is dead in the arena
    pVictim->SetUInt32Value(PLAYER_SELF_RES_SPELL, 0);

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
    if (GetStatus() != STATUS_IN_PROGRESS)
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

    Group* allianceParty = nullptr;
    Group* hordeParty = nullptr;
    bool const bothPremade = IsSidePremade(ALLIANCE, &allianceParty) && IsSidePremade(HORDE, &hordeParty);

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

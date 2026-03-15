#include "scriptPCH.h"
#include "Player.h"
#include "Pet.h"

#include <map>
#include <string>
#include <vector>

namespace
{
    // -------------------------------------------------------------------------
    // Configuration
    // -------------------------------------------------------------------------
    static const uint32 kBuffNpcEntry            = 80000;
    static const float  kTriggerDistance         = 0.20f;
    static const uint32 kBuffDurationMs          = 2 * HOUR * IN_MILLISECONDS;
    static const uint32 kPerPlayerBuffCooldownMs = 5000;
    static const bool   kUseCooldownAura         = false;

    // -------------------------------------------------------------------------
    // Visual and control spells
    // -------------------------------------------------------------------------
    static const uint32 SPELL_VISUAL_RED_LIGHTNING  = 24240;
    static const uint32 SPELL_KNOCKBACK_VISUAL      = 10689;
    static const uint32 SPELL_BUFF_COOLDOWN         = 8000;
    static const uint32 SPELL_REMOVE_OLD_DUMMY_AURA = 15007;
    static const uint32 SPELL_HAPPY_PET             = 24716;

    // -------------------------------------------------------------------------
    // Temporary support talents
    // These auras are applied for a moment so the generated raid buffs use the
    // strongest improved ranks without requiring an actual buffing raid setup.
    // -------------------------------------------------------------------------
    static const uint32 TALENT_IMPROVED_BATTLE_SHOUT_R5       = 12861;
    static const uint32 TALENT_RESTORATIVE_TOTEMS_R5          = 16208;
    static const uint32 TALENT_ENHANCING_TOTEMS_R2            = 16295;
    static const uint32 TALENT_IMPROVED_BLESSING_OF_MIGHT_R5  = 20048;
    static const uint32 TALENT_IMPROVED_BLESSING_OF_WISDOM_R2 = 20245;
    static const uint32 TALENT_IMPROVED_MARK_OF_THE_WILD_R5   = 17055;
    static const uint32 TALENT_IMPROVED_DEVOTION_AURA_R5      = 20142;
    static const uint32 TALENT_IMPROVED_IMP_R3                = 18696;

    // -------------------------------------------------------------------------
    // Talent signature spells used for role and spec detection.
    // -------------------------------------------------------------------------
    static const uint32 SPELL_THICK_HIDE_R5          = 16933;
    static const uint32 SPELL_FERAL_CHARGE           = 16979;
    static const uint32 SPELL_BLOOD_FRENZY_R2        = 16954;
    static const uint32 SPELL_SHARPENED_CLAWS_R3     = 16942;
    static const uint32 SPELL_CAT_FORM               = 768;
    static const uint32 SPELL_BEAR_FORM              = 5487;
    static const uint32 SPELL_DIRE_BEAR_FORM         = 9634;

    static const uint32 SPELL_MORTAL_STRIKE          = 12294;
    static const uint32 SPELL_BLOODTHIRST            = 23881;
    static const uint32 SPELL_SHIELD_SLAM            = 23922;
    static const uint32 SPELL_LAST_STAND             = 12975;
    static const uint32 SPELL_HOLY_SHOCK             = 20473;
    static const uint32 SPELL_HOLY_SHIELD            = 20925;
    static const uint32 SPELL_BLESSING_OF_SANCTUARY  = 20911;
    static const uint32 SPELL_SHADOWFORM             = 15473;
    static const uint32 SPELL_STORMSTRIKE            = 17364;
    static const uint32 SPELL_MANA_TIDE_TOTEM        = 16190;
    static const uint32 SPELL_NATURES_SWIFTNESS_SHM  = 16188;
    static const uint32 SPELL_MOONKIN_FORM           = 24858;
    static const uint32 SPELL_SWIFTMEND              = 18562;
    static const uint32 SPELL_NATURES_SWIFTNESS_DRU  = 17116;
    static const uint32 SPELL_COMBUSTION             = 11129;
    static const uint32 SPELL_ICE_BARRIER            = 11426;
    static const uint32 SPELL_COLD_SNAP              = 11958;

    enum BuffSpells
    {
        // Major world buffs
        SPELL_ECHOES_OF_LORDAERON_ALLIANCE       = 1386,
        SPELL_ECHOES_OF_LORDAERON_HORDE          = 29520,
        SPELL_WARCHIEFS_BLESSING                 = 16609,
        SPELL_RALLYING_CRY_OF_THE_DRAGONSLAYER   = 22888,
        SPELL_SPIRIT_OF_ZANDALAR                 = 24425,
        SPELL_SONGFLOWER_SERENADE                = 15366,
        SPELL_SLIPKIKS_SAVVY                     = 22820,
        SPELL_FENGUS_FEROCITY                    = 22817,
        SPELL_MOLDARS_MOXIE                      = 22818,
        SPELL_TRACES_OF_SILITHYST                = 29534,
        SPELL_SOUL_REVIVAL                       = 28681,
        SPELL_ELUNES_BLESSING                    = 26393,
        SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE      = 23768,
        SPELL_SAYGES_DARK_FORTUNE_OF_STRENGTH    = 23735,
        SPELL_SAYGES_DARK_FORTUNE_OF_AGILITY     = 23736,
        SPELL_SAYGES_DARK_FORTUNE_OF_SPIRIT      = 23738,
        SPELL_SAYGES_DARK_FORTUNE_OF_STAMINA     = 23737,
        SPELL_SAYGES_DARK_FORTUNE_OF_INTELLECT   = 23766,

        // Raid and class buffs
        SPELL_ARCANE_BRILLIANCE                  = 23028,
        SPELL_PRAYER_OF_FORTITUDE                = 21564,
        SPELL_PRAYER_OF_SPIRIT                   = 27681,
        SPELL_MOONKIN_AURA                       = 24907,
        SPELL_TRUESHOT_AURA                      = 20906,
        SPELL_LEADER_OF_THE_PACK                 = 24932,
        SPELL_GREATER_BLESSING_OF_KINGS          = 25898,
        SPELL_GREATER_BLESSING_OF_MIGHT          = 25916,
        SPELL_GREATER_BLESSING_OF_WISDOM         = 25918,
        SPELL_MARK_OF_THE_WILD                   = 9885,
        SPELL_BATTLE_SHOUT                       = 25289,
        SPELL_GRACE_OF_AIR                       = 25360,
        SPELL_STRENGTH_OF_EARTH                  = 25362,
        SPELL_MANA_SPRING                        = 10494,
        SPELL_BLOOD_PACT                         = 11767,
        SPELL_DEVOTION_AURA                      = 10293,

        // Consumables and event buffs
        SPELL_HOLY_MIGHTSTONE                    = 24833,
        SPELL_BUTTERMILK_DELIGHT                 = 27720,
        SPELL_SWEET_SURPRISE                     = 27722,
        SPELL_VERY_BERRY_CREAM                   = 27721,
        SPELL_DARK_DESIRE                        = 27723,
        SPELL_HEADMASTERS_CHARGE                 = 18264,
        SPELL_BLESSING_OF_BLACKFATHOM            = 8733,
        SPELL_GROUND_SCORPOK_ASSAY               = 10669,
        SPELL_FURY_OF_THE_BOGLING                = 5665,
        SPELL_FLASK_OF_THE_TITANS                = 17626,
        SPELL_FLASK_OF_DISTILLED_WISDOM          = 17627,
        SPELL_FLASK_OF_SUPREME_POWER             = 17628,
        SPELL_GREATER_ARCANE_ELIXIR              = 17539,
        SPELL_GREATER_STONESHIELD_POTION         = 17540,
        SPELL_ELIXIR_OF_THE_MONGOOSE             = 17538,
        SPELL_ELIXIR_OF_THE_SAGES                = 17535,
        SPELL_ELIXIR_OF_SUPERIOR_DEFENSE         = 11348,
        SPELL_ELIXIR_OF_SHADOW_POWER             = 11474,
        SPELL_MAGEBLOOD_POTION                   = 24363,
        SPELL_ELIXIR_OF_GREATER_FIREPOWER        = 26276,
        SPELL_ELIXIR_OF_FROST_POWER              = 21920,
        SPELL_ELIXIR_OF_FORTITUDE                = 3593,
        SPELL_WINTERFALL_FIREWATER               = 17038,
        SPELL_JUJU_MIGHT                         = 16329,
        SPELL_JUJU_POWER                         = 16323,
        SPELL_SMOKED_DESERT_DUMPLINGS            = 24799,
        SPELL_BLESSED_SUNFRUIT                   = 18125,
        SPELL_GRILLED_SQUID                      = 18192,
        SPELL_ROIDS                              = 10667,
        SPELL_RUNN_TUM_TUBER_SURPRISE            = 22730,
        SPELL_SWIFTNESS_OF_ZANZA                 = 24383,
        SPELL_CEREBRAL_CORTEX_COMPOUND           = 10692,
        SPELL_BLOODKELP_ELIXIR_OF_DODGING        = 27653,
        SPELL_RUMSEY_RUM_BLACK_LABEL             = 25804,
        SPELL_NIGHTFIN_SOUP                      = 18194,

        // Un'Goro crystal buffs
        SPELL_CRYSTAL_WARD                       = 15233,
        SPELL_CRYSTAL_FORCE                      = 15231,
        SPELL_CRYSTAL_SPIRE                      = 15279
    };

    enum PlayerRole
    {
        ROLE_TANK = 0,
        ROLE_MELEE_DPS,
        ROLE_CASTER_DPS,
        ROLE_HEALER
    };

    static const uint32 kTemporaryBuffTalents[] =
    {
        TALENT_IMPROVED_BATTLE_SHOUT_R5,
        TALENT_RESTORATIVE_TOTEMS_R5,
        TALENT_ENHANCING_TOTEMS_R2,
        TALENT_IMPROVED_BLESSING_OF_MIGHT_R5,
        TALENT_IMPROVED_BLESSING_OF_WISDOM_R2,
        TALENT_IMPROVED_MARK_OF_THE_WILD_R5,
        TALENT_IMPROVED_DEVOTION_AURA_R5,
        TALENT_IMPROVED_IMP_R3
    };

    void AddTemporaryBuffTalents(Unit* pUnit)
    {
        if (!pUnit)
            return;

        for (uint32 i = 0; i < sizeof(kTemporaryBuffTalents) / sizeof(uint32); ++i)
        {
            const uint32 talentSpell = kTemporaryBuffTalents[i];
            if (!pUnit->HasSpell(talentSpell) && !pUnit->HasAura(talentSpell))
                pUnit->AddAura(talentSpell);
        }
    }

    void RemoveTemporaryBuffTalents(Unit* pUnit)
    {
        if (!pUnit)
            return;

        for (uint32 i = 0; i < sizeof(kTemporaryBuffTalents) / sizeof(uint32); ++i)
        {
            const uint32 talentSpell = kTemporaryBuffTalents[i];
            if (!pUnit->HasSpell(talentSpell) && pUnit->HasAura(talentSpell))
                pUnit->RemoveAurasDueToSpell(talentSpell);
        }
    }

    void ApplyTimedBuff(Unit* pUnit, uint32 spellId)
    {
        if (!pUnit || !spellId)
            return;

        SpellEntry const* pSpellInfo = sSpellMgr.GetSpellEntry(spellId);
        if (!pSpellInfo)
            return;

        if (pSpellInfo->spellLevel > pUnit->GetLevel())
            return;

        pUnit->RemoveAurasDueToSpell(spellId);

        AddTemporaryBuffTalents(pUnit);

        SpellAuraHolder* pHolder = pUnit->AddAura(spellId, NULL, nullptr);
        if (pHolder)
        {
            pHolder->SetAuraMaxDuration(kBuffDurationMs);
            pHolder->SetAuraDuration(kBuffDurationMs);
            pHolder->SetPermanent(false);
            pHolder->UpdateAuraDuration();

            if (Spells::GetSpellSpecific(spellId) == SPELL_BLESSING)
                pHolder->SetCasterGuid(NULL);
        }

        RemoveTemporaryBuffTalents(pUnit);
    }

    void ApplyBuffList(Unit* pUnit, const std::vector<uint32>& spells)
    {
        if (!pUnit)
            return;

        for (std::vector<uint32>::const_iterator itr = spells.begin(); itr != spells.end(); ++itr)
            ApplyTimedBuff(pUnit, *itr);
    }

    void FillHealthAndMana(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        pPlayer->SetHealth(pPlayer->GetMaxHealth());

        if (pPlayer->GetMaxPower(POWER_MANA) > 0)
            pPlayer->SetPower(POWER_MANA, pPlayer->GetMaxPower(POWER_MANA));
    }

    bool IsLikelyDruidBearTank(Player* pPlayer)
    {
        if (!pPlayer || pPlayer->GetClass() != CLASS_DRUID)
            return false;

        if (pPlayer->HasAura(SPELL_BEAR_FORM) || pPlayer->HasAura(SPELL_DIRE_BEAR_FORM))
            return true;

        if (pPlayer->HasAura(SPELL_CAT_FORM))
            return false;

        if (pPlayer->HasSpell(SPELL_FERAL_CHARGE))
            return true;

        int32 bearScore = 0;
        int32 catScore  = 0;

        if (pPlayer->HasSpell(SPELL_THICK_HIDE_R5))
            bearScore += 2;

        if (pPlayer->HasSpell(SPELL_BLOOD_FRENZY_R2))
            catScore += 2;

        if (pPlayer->HasSpell(SPELL_SHARPENED_CLAWS_R3))
            catScore += 1;

        return bearScore > catScore;
    }

    bool IsWarriorProtection(Player* pPlayer) { return pPlayer && (pPlayer->HasSpell(SPELL_SHIELD_SLAM) || pPlayer->HasSpell(SPELL_LAST_STAND)); }
    bool IsWarriorArms(Player* pPlayer)       { return pPlayer && pPlayer->HasSpell(SPELL_MORTAL_STRIKE); }
    bool IsWarriorFury(Player* pPlayer)       { return pPlayer && pPlayer->HasSpell(SPELL_BLOODTHIRST); }
    bool IsPaladinProtection(Player* pPlayer) { return pPlayer && (pPlayer->HasSpell(SPELL_HOLY_SHIELD) || pPlayer->HasSpell(SPELL_BLESSING_OF_SANCTUARY)); }
    bool IsPaladinHoly(Player* pPlayer)       { return pPlayer && pPlayer->HasSpell(SPELL_HOLY_SHOCK); }
    bool IsPriestShadow(Player* pPlayer)      { return pPlayer && pPlayer->HasSpell(SPELL_SHADOWFORM); }
    bool IsShamanEnhancement(Player* pPlayer) { return pPlayer && pPlayer->HasSpell(SPELL_STORMSTRIKE); }
    bool IsShamanRestoration(Player* pPlayer) { return pPlayer && (pPlayer->HasSpell(SPELL_MANA_TIDE_TOTEM) || pPlayer->HasSpell(SPELL_NATURES_SWIFTNESS_SHM)); }
    bool IsDruidRestoration(Player* pPlayer)  { return pPlayer && (pPlayer->HasSpell(SPELL_SWIFTMEND) || pPlayer->HasSpell(SPELL_NATURES_SWIFTNESS_DRU)); }
    bool IsDruidBalance(Player* pPlayer)      { return pPlayer && pPlayer->HasSpell(SPELL_MOONKIN_FORM); }
    bool IsMageFire(Player* pPlayer)          { return pPlayer && pPlayer->HasSpell(SPELL_COMBUSTION); }
    bool IsMageFrost(Player* pPlayer)         { return pPlayer && (pPlayer->HasSpell(SPELL_ICE_BARRIER) || pPlayer->HasSpell(SPELL_COLD_SNAP)); }
    bool IsShamanElemental(Player* pPlayer)   { return pPlayer && !IsShamanEnhancement(pPlayer) && !IsShamanRestoration(pPlayer); }

    std::string GetDetectedSpecName(Player* pPlayer)
    {
        if (!pPlayer)
            return "Unknown";

        switch (pPlayer->GetClass())
        {
            case CLASS_WARRIOR:
                if (IsWarriorProtection(pPlayer))
                    return "Warrior Protection";
                if (IsWarriorArms(pPlayer))
                    return "Warrior Arms";
                if (IsWarriorFury(pPlayer))
                    return "Warrior Fury";
                return "Warrior Unknown";

            case CLASS_PALADIN:
                if (IsPaladinProtection(pPlayer))
                    return "Paladin Protection";
                if (IsPaladinHoly(pPlayer))
                    return "Paladin Holy";
                return "Paladin Retribution";

            case CLASS_HUNTER:
                return "Hunter";

            case CLASS_ROGUE:
                return "Rogue";

            case CLASS_PRIEST:
                return IsPriestShadow(pPlayer) ? "Priest Shadow" : "Priest Healer";

            case CLASS_MAGE:
                if (IsMageFire(pPlayer))
                    return "Mage Fire";
                if (IsMageFrost(pPlayer))
                    return "Mage Frost";
                return "Mage Arcane";

            case CLASS_WARLOCK:
                return "Warlock";

            case CLASS_SHAMAN:
                if (IsShamanEnhancement(pPlayer))
                    return "Shaman Enhancement";
                if (IsShamanRestoration(pPlayer))
                    return "Shaman Restoration";
                return "Shaman Elemental";

            case CLASS_DRUID:
                if (IsDruidRestoration(pPlayer))
                    return "Druid Restoration";
                if (IsDruidBalance(pPlayer))
                    return "Druid Balance";
                return IsLikelyDruidBearTank(pPlayer) ? "Druid Feral Bear" : "Druid Feral Cat";

            default:
                return "Unknown";
        }
    }

    PlayerRole GetPlayerRole(Player* pPlayer)
    {
        if (!pPlayer)
            return ROLE_MELEE_DPS;

        switch (pPlayer->GetClass())
        {
            case CLASS_WARRIOR:
                return IsWarriorProtection(pPlayer) ? ROLE_TANK : ROLE_MELEE_DPS;

            case CLASS_PALADIN:
                if (IsPaladinProtection(pPlayer))
                    return ROLE_TANK;
                if (IsPaladinHoly(pPlayer))
                    return ROLE_HEALER;
                return ROLE_MELEE_DPS;

            case CLASS_HUNTER:
            case CLASS_ROGUE:
                return ROLE_MELEE_DPS;

            case CLASS_PRIEST:
                return IsPriestShadow(pPlayer) ? ROLE_CASTER_DPS : ROLE_HEALER;

            case CLASS_MAGE:
            case CLASS_WARLOCK:
                return ROLE_CASTER_DPS;

            case CLASS_SHAMAN:
                if (IsShamanEnhancement(pPlayer))
                    return ROLE_MELEE_DPS;
                if (IsShamanRestoration(pPlayer))
                    return ROLE_HEALER;
                return ROLE_CASTER_DPS;

            case CLASS_DRUID:
                if (IsDruidRestoration(pPlayer))
                    return ROLE_HEALER;
                if (IsDruidBalance(pPlayer))
                    return ROLE_CASTER_DPS;
                return IsLikelyDruidBearTank(pPlayer) ? ROLE_TANK : ROLE_MELEE_DPS;

            default:
                break;
        }

        return ROLE_MELEE_DPS;
    }

    void ApplyAllianceMeleeBlessings(Player* pPlayer)
    {
        if (!pPlayer || pPlayer->GetTeam() != ALLIANCE)
            return;

        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_KINGS);
        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_MIGHT);
    }

    void ApplyAllianceHunterBlessings(Player* pPlayer)
    {
        if (!pPlayer || pPlayer->GetTeam() != ALLIANCE)
            return;

        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_KINGS);
        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_MIGHT);
        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_WISDOM);
    }

    void ApplyAllianceCasterBlessings(Player* pPlayer)
    {
        if (!pPlayer || pPlayer->GetTeam() != ALLIANCE)
            return;

        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_KINGS);
        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_WISDOM);
    }

    void ApplyCoreWorldBuffs(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        pPlayer->RemoveAllAurasOnDeath();
        pPlayer->RemoveAurasDueToSpell(SPELL_REMOVE_OLD_DUMMY_AURA);

        if (pPlayer->GetTeam() == HORDE)
            ApplyTimedBuff(pPlayer, SPELL_ECHOES_OF_LORDAERON_HORDE);
        else
            ApplyTimedBuff(pPlayer, SPELL_ECHOES_OF_LORDAERON_ALLIANCE);

        ApplyBuffList(pPlayer,
        {
            SPELL_HOLY_MIGHTSTONE,
            SPELL_WARCHIEFS_BLESSING,
            SPELL_RALLYING_CRY_OF_THE_DRAGONSLAYER,
            SPELL_SPIRIT_OF_ZANDALAR,
            SPELL_SONGFLOWER_SERENADE,
            SPELL_ELUNES_BLESSING,
            SPELL_TRACES_OF_SILITHYST,
            SPELL_SOUL_REVIVAL
        });
    }

    void ApplyRoleRaidBuffs(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        const PlayerRole role = GetPlayerRole(pPlayer);

        switch (role)
        {
            case ROLE_TANK:
                ApplyBuffList(pPlayer,
                {
                    SPELL_MOLDARS_MOXIE,
                    SPELL_PRAYER_OF_FORTITUDE,
                    SPELL_MARK_OF_THE_WILD,
                    SPELL_BATTLE_SHOUT,
                    SPELL_BLOOD_PACT
                });

                if (pPlayer->GetTeam() == HORDE)
                    ApplyTimedBuff(pPlayer, SPELL_STRENGTH_OF_EARTH);
                else
                    ApplyAllianceMeleeBlessings(pPlayer);
                break;

            case ROLE_MELEE_DPS:
                ApplyBuffList(pPlayer,
                {
                    SPELL_FENGUS_FEROCITY,
                    SPELL_MARK_OF_THE_WILD,
                    SPELL_LEADER_OF_THE_PACK,
                    SPELL_TRUESHOT_AURA,
                    SPELL_BATTLE_SHOUT
                });

                if (pPlayer->GetClass() == CLASS_HUNTER)
                {
                    if (pPlayer->GetTeam() == HORDE)
                    {
                        ApplyTimedBuff(pPlayer, SPELL_STRENGTH_OF_EARTH);
                        ApplyTimedBuff(pPlayer, SPELL_GRACE_OF_AIR);
                    }
                    else
                    {
                        ApplyAllianceHunterBlessings(pPlayer);
                    }
                }
                else
                {
                    if (pPlayer->GetTeam() == HORDE)
                    {
                        ApplyTimedBuff(pPlayer, SPELL_STRENGTH_OF_EARTH);
                        ApplyTimedBuff(pPlayer, SPELL_GRACE_OF_AIR);
                    }
                    else
                    {
                        ApplyAllianceMeleeBlessings(pPlayer);
                    }
                }
                break;

            case ROLE_CASTER_DPS:
                ApplyBuffList(pPlayer,
                {
                    SPELL_SLIPKIKS_SAVVY,
                    SPELL_ARCANE_BRILLIANCE,
                    SPELL_PRAYER_OF_SPIRIT,
                    SPELL_MOONKIN_AURA,
                    SPELL_MARK_OF_THE_WILD
                });

                if (pPlayer->GetTeam() == HORDE)
                    ApplyTimedBuff(pPlayer, SPELL_MANA_SPRING);
                else
                    ApplyAllianceCasterBlessings(pPlayer);
                break;

            case ROLE_HEALER:
                ApplyBuffList(pPlayer,
                {
                    SPELL_SLIPKIKS_SAVVY,
                    SPELL_ARCANE_BRILLIANCE,
                    SPELL_PRAYER_OF_SPIRIT,
                    SPELL_MARK_OF_THE_WILD
                });

                if (pPlayer->GetTeam() == HORDE)
                    ApplyTimedBuff(pPlayer, SPELL_MANA_SPRING);
                else
                    ApplyAllianceCasterBlessings(pPlayer);
                break;
        }

        if (pPlayer->GetClass() == CLASS_PALADIN && role != ROLE_HEALER)
            ApplyTimedBuff(pPlayer, SPELL_PRAYER_OF_SPIRIT);
    }

    void ApplyUniversalManaConsumes(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        ApplyBuffList(pPlayer,
        {
            SPELL_HEADMASTERS_CHARGE,
            SPELL_CEREBRAL_CORTEX_COMPOUND,
            SPELL_RUNN_TUM_TUBER_SURPRISE,
            SPELL_BLESSING_OF_BLACKFATHOM
        });
    }

    void ApplyTankConsumes(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        ApplyBuffList(pPlayer,
        {
            SPELL_SAYGES_DARK_FORTUNE_OF_STAMINA,
            SPELL_FLASK_OF_THE_TITANS,
            SPELL_ELIXIR_OF_FORTITUDE,
            SPELL_ELIXIR_OF_SUPERIOR_DEFENSE,
            SPELL_GREATER_STONESHIELD_POTION,
            SPELL_ELIXIR_OF_THE_MONGOOSE,
            SPELL_ROIDS,
            SPELL_BLOODKELP_ELIXIR_OF_DODGING,
            SPELL_RUMSEY_RUM_BLACK_LABEL,
            SPELL_CRYSTAL_WARD,
            SPELL_CRYSTAL_SPIRE,
            SPELL_BUTTERMILK_DELIGHT
        });
    }

    void ApplyMeleeConsumes(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        ApplyBuffList(pPlayer,
        {
            SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
            SPELL_FURY_OF_THE_BOGLING,
            SPELL_ELIXIR_OF_THE_MONGOOSE,
            SPELL_WINTERFALL_FIREWATER,
            SPELL_JUJU_MIGHT,
            SPELL_JUJU_POWER,
            SPELL_ROIDS,
            SPELL_GROUND_SCORPOK_ASSAY,
            SPELL_BLESSED_SUNFRUIT,
            SPELL_SMOKED_DESERT_DUMPLINGS,
            SPELL_GRILLED_SQUID,
            SPELL_DARK_DESIRE
        });
    }

    void ApplyCasterConsumes(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        ApplyBuffList(pPlayer,
        {
            SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
            SPELL_FLASK_OF_SUPREME_POWER,
            SPELL_GREATER_ARCANE_ELIXIR,
            SPELL_MAGEBLOOD_POTION,
            SPELL_CRYSTAL_FORCE,
            SPELL_NIGHTFIN_SOUP,
            SPELL_VERY_BERRY_CREAM
        });

        ApplyUniversalManaConsumes(pPlayer);
    }

    void ApplyHealerConsumes(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        ApplyBuffList(pPlayer,
        {
            SPELL_SAYGES_DARK_FORTUNE_OF_INTELLECT,
            SPELL_FLASK_OF_DISTILLED_WISDOM,
            SPELL_ELIXIR_OF_THE_SAGES,
            SPELL_MAGEBLOOD_POTION,
            SPELL_CRYSTAL_FORCE,
            SPELL_NIGHTFIN_SOUP,
            SPELL_SWEET_SURPRISE
        });

        ApplyUniversalManaConsumes(pPlayer);
    }

    void ApplyClassSpecificConsumes(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        const PlayerRole role = GetPlayerRole(pPlayer);

        switch (role)
        {
            case ROLE_TANK:
                ApplyTankConsumes(pPlayer);

                if (pPlayer->GetClass() == CLASS_WARRIOR)
                {
                    ApplyTimedBuff(pPlayer, SPELL_MOLDARS_MOXIE);
                    ApplyTimedBuff(pPlayer, SPELL_SAYGES_DARK_FORTUNE_OF_STAMINA);
                }
                else if (pPlayer->GetClass() == CLASS_PALADIN)
                {
                    ApplyTimedBuff(pPlayer, SPELL_MOLDARS_MOXIE);
                    ApplyTimedBuff(pPlayer, SPELL_SAYGES_DARK_FORTUNE_OF_STAMINA);
                    ApplyTimedBuff(pPlayer, SPELL_DEVOTION_AURA);
                }
                else if (pPlayer->GetClass() == CLASS_DRUID)
                {
                    ApplyTimedBuff(pPlayer, SPELL_MOLDARS_MOXIE);
                    if (pPlayer->GetTeam() == ALLIANCE)
                        ApplyTimedBuff(pPlayer, SPELL_DEVOTION_AURA);
                }
                break;

            case ROLE_MELEE_DPS:
                ApplyMeleeConsumes(pPlayer);

                if (pPlayer->GetClass() == CLASS_HUNTER)
                {
                    ApplyTimedBuff(pPlayer, SPELL_SWIFTNESS_OF_ZANZA);
                }
                else if (pPlayer->GetClass() == CLASS_PALADIN)
                {
                    ApplyTimedBuff(pPlayer, SPELL_VERY_BERRY_CREAM);
                }
                else if (pPlayer->GetClass() == CLASS_SHAMAN)
                {
                    // Keep the original behaviour from your working version.
                    ApplyTimedBuff(pPlayer, SPELL_VERY_BERRY_CREAM);
                }
                else if (pPlayer->GetClass() == CLASS_DRUID && pPlayer->GetTeam() == ALLIANCE)
                {
                    ApplyTimedBuff(pPlayer, SPELL_DEVOTION_AURA);
                }
                break;

            case ROLE_CASTER_DPS:
                ApplyCasterConsumes(pPlayer);

                if (pPlayer->GetClass() == CLASS_MAGE)
                {
                    if (IsMageFire(pPlayer))
                        ApplyTimedBuff(pPlayer, SPELL_ELIXIR_OF_GREATER_FIREPOWER);
                    else if (IsMageFrost(pPlayer))
                        ApplyTimedBuff(pPlayer, SPELL_ELIXIR_OF_FROST_POWER);
                }
                else if (pPlayer->GetClass() == CLASS_WARLOCK)
                {
                    ApplyTimedBuff(pPlayer, SPELL_ELIXIR_OF_SHADOW_POWER);
                }
                else if (pPlayer->GetClass() == CLASS_SHAMAN)
                {
                    if (IsShamanElemental(pPlayer))
                        ApplyTimedBuff(pPlayer, SPELL_ELIXIR_OF_GREATER_FIREPOWER);
                }
                else if (pPlayer->GetClass() == CLASS_PRIEST)
                {
                    if (IsPriestShadow(pPlayer))
                        ApplyTimedBuff(pPlayer, SPELL_ELIXIR_OF_SHADOW_POWER);
                }
                break;

            case ROLE_HEALER:
                ApplyHealerConsumes(pPlayer);
                break;
        }
    }

    void ApplyHunterPetBuffs(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        Pet* pPet = pPlayer->GetPet();
        if (!pPet || !pPet->IsAlive())
            return;

        if (pPlayer->GetTeam() == ALLIANCE)
        {
            ApplyTimedBuff(pPet, SPELL_GREATER_BLESSING_OF_KINGS);
            ApplyTimedBuff(pPet, SPELL_GREATER_BLESSING_OF_MIGHT);
        }
        else
        {
            ApplyTimedBuff(pPet, SPELL_STRENGTH_OF_EARTH);
            ApplyTimedBuff(pPet, SPELL_GRACE_OF_AIR);
        }

        ApplyBuffList(pPet,
        {
            SPELL_WARCHIEFS_BLESSING,
            SPELL_RALLYING_CRY_OF_THE_DRAGONSLAYER,
            SPELL_SPIRIT_OF_ZANDALAR,
            SPELL_FENGUS_FEROCITY,
            SPELL_SOUL_REVIVAL,
            SPELL_LEADER_OF_THE_PACK,
            SPELL_MARK_OF_THE_WILD,
            SPELL_TRUESHOT_AURA,
            SPELL_BATTLE_SHOUT
        });

        ApplyTimedBuff(pPlayer, SPELL_HAPPY_PET);
    }

    void FullBuffPlayer(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        ApplyCoreWorldBuffs(pPlayer);
        ApplyRoleRaidBuffs(pPlayer);
        ApplyClassSpecificConsumes(pPlayer);
        ApplyHunterPetBuffs(pPlayer);
        FillHealthAndMana(pPlayer);
    }
}

struct npc_buff_machineAI : public ScriptedAI
{
    explicit npc_buff_machineAI(Creature* pCreature)
        : ScriptedAI(pCreature)
    {
        // Explicitly enable MoveInLineOfSight processing for this passive NPC.
        m_creature->EnableMoveInLosEvent();
    }

    std::map<uint32, uint32> m_playerCooldowns;

    void Reset() override
    {
        // Re-enable LoS processing after resets and clear the short anti-spam map.
        m_playerCooldowns.clear();
        m_creature->EnableMoveInLosEvent();
    }

    void UpdateCooldowns(uint32 diff)
    {
        for (std::map<uint32, uint32>::iterator itr = m_playerCooldowns.begin(); itr != m_playerCooldowns.end(); )
        {
            if (itr->second <= diff)
                m_playerCooldowns.erase(itr++);
            else
            {
                itr->second -= diff;
                ++itr;
            }
        }
    }

    bool IsRecentlyProcessed(Player* pPlayer) const
    {
        if (!pPlayer)
            return true;

        return m_playerCooldowns.find(pPlayer->GetGUIDLow()) != m_playerCooldowns.end();
    }

    void MarkProcessed(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        m_playerCooldowns[pPlayer->GetGUIDLow()] = kPerPlayerBuffCooldownMs;
    }

    bool CanProcessPlayer(Player* pPlayer)
    {
        if (!pPlayer || !pPlayer->IsAlive())
            return false;

        if (!m_creature->IsWithinDistInMap(pPlayer, kTriggerDistance))
            return false;

        if (IsRecentlyProcessed(pPlayer))
            return false;

        return true;
    }

    void ProcessPlayer(Player* pPlayer)
    {
        if (!CanProcessPlayer(pPlayer))
            return;

        MarkProcessed(pPlayer);

        if (pPlayer->IsInCombat())
        {
            pPlayer->CastSpell(pPlayer, SPELL_KNOCKBACK_VISUAL, true);
            m_creature->MonsterWhisper("Buff Machine: You are in combat.", pPlayer);
            return;
        }

        if (kUseCooldownAura && pPlayer->HasAura(SPELL_BUFF_COOLDOWN))
            return;

        pPlayer->DuelComplete(DUEL_INTERRUPTED);
        FullBuffPlayer(pPlayer);
        pPlayer->CastSpell(pPlayer, SPELL_VISUAL_RED_LIGHTNING, true);

        if (kUseCooldownAura)
            pPlayer->AddAura(SPELL_BUFF_COOLDOWN);

        std::string whisper = "Buff Machine: Full buffs applied. Detected spec: ";
        whisper += GetDetectedSpecName(pPlayer);
        m_creature->MonsterWhisper(whisper.c_str(), pPlayer);
    }

    void MoveInLineOfSight(Unit* pWho) override
    {
        if (m_creature->GetEntry() != kBuffNpcEntry)
        {
            ScriptedAI::MoveInLineOfSight(pWho);
            return;
        }

        if (!pWho || pWho->GetTypeId() != TYPEID_PLAYER)
            return;

        Player* pPlayer = pWho->ToPlayer();
        if (!pPlayer)
            return;

        ProcessPlayer(pPlayer);
    }

    void UpdateAI(const uint32 diff) override
    {
        if (m_creature->GetEntry() != kBuffNpcEntry)
            return;

        // Only maintain the short per-player cooldowns.
        // Buffing itself is handled directly by MoveInLineOfSight.
        UpdateCooldowns(diff);
    }
};

CreatureAI* GetAI_npc_buff_machine(Creature* pCreature)
{
    return new npc_buff_machineAI(pCreature);
}

void AddSC_npc_buff_machine()
{
    Script* pNewScript = new Script;
    pNewScript->Name = "npc_buff_machine";
    pNewScript->GetAI = &GetAI_npc_buff_machine;
    pNewScript->RegisterSelf();
}

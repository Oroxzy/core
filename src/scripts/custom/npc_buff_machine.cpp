#include "scriptPCH.h"
#include "Player.h"
#include "Pet.h"

#include <vector>

namespace
{
    // ------------------------------------------------------------
    // Konfiguration
    // ------------------------------------------------------------
    static const uint32 kBuffNpcEntry                = 80000;
    static const float  kTriggerDistance             = 0.20f;
    static const uint32 kBuffDurationMs              = 2 * HOUR * IN_MILLISECONDS;

    // Visuell / Logik
    static const uint32 SPELL_VISUAL_RED_LIGHTNING   = 24240;
    static const uint32 SPELL_KNOCKBACK_VISUAL       = 10689;
    static const uint32 SPELL_BUFF_COOLDOWN          = 8000;
    static const uint32 SPELL_REMOVE_OLD_DUMMY_AURA  = 15007;
    static const uint32 SPELL_HAPPY_PET              = 24716;

    // Talent-Hilfsauras, damit verbesserte Gruppenbuffs mit den richtigen Rängen laufen.
    static const uint32 TALENT_IMPROVED_BATTLE_SHOUT_R5       = 12861;
    static const uint32 TALENT_RESTORATIVE_TOTEMS_R5          = 16208;
    static const uint32 TALENT_ENHANCING_TOTEMS_R2            = 16295;
    static const uint32 TALENT_IMPROVED_BLESSING_OF_MIGHT_R5  = 20048;
    static const uint32 TALENT_IMPROVED_BLESSING_OF_WISDOM_R2 = 20245;
    static const uint32 TALENT_IMPROVED_MARK_OF_THE_WILD_R5   = 17055;
    static const uint32 TALENT_IMPROVED_DEVOTION_AURA_R5      = 20142;
    static const uint32 TALENT_IMPROVED_IMP_R3                = 18696;

    enum BuffSpells
    {
        // ------------------------------------------------------------
        // World buffs
        // ------------------------------------------------------------
        SPELL_ECHOES_OF_LORDAERON_ALLIANCE     = 1386,
        SPELL_ECHOES_OF_LORDAERON_HORDE        = 29520,
        SPELL_WARCHIEFS_BLESSING               = 16609,
        SPELL_RALLYING_CRY_OF_THE_DRAGONSLAYER = 22888,
        SPELL_SPIRIT_OF_ZANDALAR               = 24425,
        SPELL_SONGFLOWER_SERENADE              = 15366,
        SPELL_SLIPKIKS_SAVVY                   = 22820,
        SPELL_FENGUS_FEROCITY                  = 22817,
        SPELL_MOLDARS_MOXIE                    = 22818,
        SPELL_TRACES_OF_SILITHYST              = 29534,
        SPELL_SOUL_REVIVAL                     = 28681,
        SPELL_ELUNES_BLESSING                  = 26393,
        SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE     = 23768,
        SPELL_SAYGES_DARK_FORTUNE_OF_STRENGTH   = 23735,
        SPELL_SAYGES_DARK_FORTUNE_OF_AGILITY    = 23736,
        SPELL_SAYGES_DARK_FORTUNE_OF_SPIRIT     = 23738,
        SPELL_SAYGES_DARK_FORTUNE_OF_STAMINA    = 23737,
        SPELL_SAYGES_DARK_FORTUNE_OF_INTELLIGENCE = 23766,

        // ------------------------------------------------------------
        // Klassen-/Raidbuffs
        // ------------------------------------------------------------
        SPELL_ARCANE_BRILLIANCE                = 23028,
        SPELL_PRAYER_OF_FORTITUDE              = 21564,
        SPELL_PRAYER_OF_SPIRIT                 = 27681,
        SPELL_MOONKIN_AURA                     = 24907,
        SPELL_TRUESHOT_AURA                    = 20906,
        SPELL_LEADER_OF_THE_PACK               = 24932,
        SPELL_GREATER_BLESSING_OF_KINGS        = 25898,
        SPELL_GREATER_BLESSING_OF_MIGHT        = 25916,
        SPELL_GREATER_BLESSING_OF_WISDOM       = 25918,
        SPELL_MARK_OF_THE_WILD                 = 9885,
        SPELL_BATTLE_SHOUT                     = 25289,
        SPELL_GRACE_OF_AIR                     = 25360,
        SPELL_STRENGTH_OF_EARTH                = 25362,
        SPELL_MANA_SPRING                      = 10494,
        SPELL_BLOOD_PACT                       = 11767,
        SPELL_DEVOTION_AURA                    = 10293,

        // ------------------------------------------------------------
        // Spezielle Event-/Consume-Buffs
        // ------------------------------------------------------------
        SPELL_HOLY_MIGHTSTONE                  = 24833,
        SPELL_BUTTERMILK_DELIGHT               = 27720,
        SPELL_SWEET_SURPRISE                   = 27722,
        SPELL_VERY_BERRY_CREAM                 = 27721,
        SPELL_DARK_DESIRE                      = 27723,
        SPELL_HEADMASTERS_CHARGE               = 18264,
        SPELL_BLESSING_OF_BLACKFATHOM          = 8733,
        SPELL_GROUND_SCORPOK_ASSAY             = 10669,
        SPELL_FURY_OF_THE_BOGLING              = 5665,
        SPELL_FLASK_OF_THE_TITANS              = 17626,
        SPELL_FLASK_OF_DISTILLED_WISDOM        = 17627,
        SPELL_FLASK_OF_SUPREME_POWER           = 17628,
        SPELL_GREATER_ARCANE_ELIXIR            = 17539,
        SPELL_GREATER_STONESHIELD_POTION       = 17540,
        SPELL_ELIXIR_OF_THE_MONGOOSE           = 17538,
        SPELL_ELIXIR_OF_THE_SAGES              = 17535,
        SPELL_ELIXIR_OF_SUPERIOR_DEFENSE       = 11348,
        SPELL_ELIXIR_OF_SHADOW_POWER           = 11474,
        SPELL_MAGEBLOOD_POTION                 = 24363,
        SPELL_ELIXIR_OF_GREATER_FIREPOWER      = 26276,
        SPELL_ELIXIR_OF_FROST_POWER            = 21920,
        SPELL_ELIXIR_OF_FORTITUDE              = 3593,
        SPELL_WINTERFALL_FIREWATER             = 17038,
        SPELL_JUJU_MIGHT                       = 16329,
        SPELL_JUJU_POWER                       = 16323,
        SPELL_SMOKED_DESERT_DUMPLINGS          = 24799,
        SPELL_BLESSED_SUNFRUIT                 = 18125,
        SPELL_GRILLED_SQUID                    = 18192,
        SPELL_ROIDS                            = 10667,
        SPELL_RUNN_TUM_TUBER_SURPRISE          = 22730,
        SPELL_SWIFTNESS_OF_ZANZA               = 24383,
        SPELL_CEREBRAL_CORTEX_COMPOUND         = 10692,
        SPELL_BLOODKELP_ELIXIR_OF_DODGING      = 27653,
        SPELL_RUMSEY_RUM_BLACK_LABEL           = 25804,
        SPELL_NIGHTFIN_SOUP                    = 18194,

        // ------------------------------------------------------------
        // Un'Goro Crystals
        // ------------------------------------------------------------
        SPELL_CRYSTAL_WARD                     = 15233,
        SPELL_CRYSTAL_FORCE                    = 15231,
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

    void ApplyAllianceBlessings(Player* pPlayer, bool includeWisdom)
    {
        if (!pPlayer || pPlayer->GetTeam() != ALLIANCE)
            return;

        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_KINGS);
        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_MIGHT);

        if (includeWisdom)
            ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_WISDOM);
    }

    void ApplyAllianceCasterBlessings(Player* pPlayer)
    {
        if (!pPlayer || pPlayer->GetTeam() != ALLIANCE)
            return;

        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_KINGS);
        ApplyTimedBuff(pPlayer, SPELL_GREATER_BLESSING_OF_WISDOM);
    }

    void ApplySharedWorldBuffs(Player* pPlayer)
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
            SPELL_ELUNES_BLESSING
        });
    }

    void FillHealthAndMana(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        pPlayer->SetHealth(pPlayer->GetMaxHealth());

        if (pPlayer->GetMaxPower(POWER_MANA) > 0)
            pPlayer->SetPower(POWER_MANA, pPlayer->GetMaxPower(POWER_MANA));
    }

    void ApplyHunterPetBuffs(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        Pet* pPet = pPlayer->GetPet();
        if (!pPet)
            return;

        if (!pPet->isControlled())
            return;

        if (pPet->getPetType() != HUNTER_PET)
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

    void ApplySharedClassBuffs(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        switch (pPlayer->GetClass())
        {
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
            case CLASS_HUNTER:
                if (pPlayer->GetTeam() == HORDE)
                {
                    ApplyTimedBuff(pPlayer, SPELL_GRACE_OF_AIR);
                    ApplyTimedBuff(pPlayer, SPELL_STRENGTH_OF_EARTH);
                }
                ApplyBuffList(pPlayer,
                {
                    SPELL_FENGUS_FEROCITY,
                    SPELL_TRACES_OF_SILITHYST,
                    SPELL_SOUL_REVIVAL,
                    SPELL_LEADER_OF_THE_PACK,
                    SPELL_MARK_OF_THE_WILD,
                    SPELL_TRUESHOT_AURA,
                    SPELL_BATTLE_SHOUT
                });
                break;

            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
                if (pPlayer->GetTeam() == HORDE)
                    ApplyTimedBuff(pPlayer, SPELL_MANA_SPRING);

                ApplyBuffList(pPlayer,
                {
                    SPELL_SLIPKIKS_SAVVY,
                    SPELL_TRACES_OF_SILITHYST,
                    SPELL_SOUL_REVIVAL,
                    SPELL_PRAYER_OF_SPIRIT,
                    SPELL_MOONKIN_AURA,
                    SPELL_ARCANE_BRILLIANCE,
                    SPELL_MARK_OF_THE_WILD
                });
                break;

            case CLASS_PALADIN:
            {
                const uint32 talentTabId = pPlayer->GetTalentTabID();

                if (talentTabId == PaladinHoly)
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SLIPKIKS_SAVVY,
                        SPELL_TRACES_OF_SILITHYST,
                        SPELL_SOUL_REVIVAL,
                        SPELL_PRAYER_OF_SPIRIT,
                        SPELL_MOONKIN_AURA,
                        SPELL_ARCANE_BRILLIANCE,
                        SPELL_MARK_OF_THE_WILD
                    });
                }
                else
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SLIPKIKS_SAVVY,
                        SPELL_FENGUS_FEROCITY,
                        SPELL_TRACES_OF_SILITHYST,
                        SPELL_SOUL_REVIVAL,
                        SPELL_LEADER_OF_THE_PACK,
                        SPELL_PRAYER_OF_SPIRIT,
                        SPELL_MOONKIN_AURA,
                        SPELL_ARCANE_BRILLIANCE,
                        SPELL_MARK_OF_THE_WILD,
                        SPELL_TRUESHOT_AURA,
                        SPELL_BATTLE_SHOUT
                    });
                }
                break;
            }

            case CLASS_SHAMAN:
            {
                const uint32 talentTabId = pPlayer->GetTalentTabID();

                if (pPlayer->GetTeam() == HORDE)
                    ApplyTimedBuff(pPlayer, SPELL_MANA_SPRING);

                if (talentTabId == ShamanEnhancement)
                {
                    if (pPlayer->GetTeam() == HORDE)
                    {
                        ApplyTimedBuff(pPlayer, SPELL_STRENGTH_OF_EARTH);
                        ApplyTimedBuff(pPlayer, SPELL_GRACE_OF_AIR);
                    }

                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SLIPKIKS_SAVVY,
                        SPELL_FENGUS_FEROCITY,
                        SPELL_TRACES_OF_SILITHYST,
                        SPELL_SOUL_REVIVAL,
                        SPELL_LEADER_OF_THE_PACK,
                        SPELL_MOONKIN_AURA,
                        SPELL_TRUESHOT_AURA,
                        SPELL_BATTLE_SHOUT
                    });
                }
                else
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SLIPKIKS_SAVVY,
                        SPELL_TRACES_OF_SILITHYST,
                        SPELL_SOUL_REVIVAL,
                        SPELL_PRAYER_OF_SPIRIT,
                        SPELL_MOONKIN_AURA,
                        SPELL_ARCANE_BRILLIANCE,
                        SPELL_MARK_OF_THE_WILD
                    });
                }
                break;
            }

            case CLASS_DRUID:
            {
                const uint32 talentTabId = pPlayer->GetTalentTabID();

                if (talentTabId == DruidFeralCombat)
                {
                    if (pPlayer->GetTeam() == HORDE)
                    {
                        ApplyTimedBuff(pPlayer, SPELL_MANA_SPRING);
                        ApplyTimedBuff(pPlayer, SPELL_STRENGTH_OF_EARTH);
                        ApplyTimedBuff(pPlayer, SPELL_GRACE_OF_AIR);
                    }

                    ApplyBuffList(pPlayer,
                    {
                        SPELL_FENGUS_FEROCITY,
                        SPELL_TRACES_OF_SILITHYST,
                        SPELL_SOUL_REVIVAL,
                        SPELL_LEADER_OF_THE_PACK,
                        SPELL_MARK_OF_THE_WILD,
                        SPELL_TRUESHOT_AURA,
                        SPELL_BATTLE_SHOUT
                    });
                }
                else
                {
                    if (pPlayer->GetTeam() == HORDE)
                        ApplyTimedBuff(pPlayer, SPELL_MANA_SPRING);

                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SLIPKIKS_SAVVY,
                        SPELL_TRACES_OF_SILITHYST,
                        SPELL_SOUL_REVIVAL,
                        SPELL_PRAYER_OF_SPIRIT,
                        SPELL_MOONKIN_AURA,
                        SPELL_ARCANE_BRILLIANCE,
                        SPELL_MARK_OF_THE_WILD
                    });
                }
                break;
            }

            default:
                break;
        }

        switch (pPlayer->GetClass())
        {
            case CLASS_WARRIOR:
            case CLASS_ROGUE:
            case CLASS_HUNTER:
            {
                ApplyAllianceBlessings(pPlayer, pPlayer->GetClass() == CLASS_HUNTER);
                break;
            }

            case CLASS_MAGE:
            case CLASS_PRIEST:
            case CLASS_WARLOCK:
            {
                ApplyAllianceCasterBlessings(pPlayer);
                break;
            }

            case CLASS_PALADIN:
            {
                if (pPlayer->GetTalentTabID() == PaladinHoly)
                    ApplyAllianceCasterBlessings(pPlayer);
                else
                    ApplyAllianceBlessings(pPlayer, true);
                break;
            }

            case CLASS_SHAMAN:
            {
                if (pPlayer->GetTalentTabID() == ShamanEnhancement)
                    ApplyAllianceBlessings(pPlayer, true);
                else
                    ApplyAllianceCasterBlessings(pPlayer);
                break;
            }

            case CLASS_DRUID:
            {
                if (pPlayer->GetTalentTabID() == DruidFeralCombat)
                    ApplyAllianceBlessings(pPlayer, false);
                else
                    ApplyAllianceCasterBlessings(pPlayer);
                break;
            }

            default:
                break;
        }
    }

    void ApplySpecConsumes(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        switch (pPlayer->GetClass())
        {
            case CLASS_WARRIOR:
                if (pPlayer->GetTalentTabID() == WarriorProtection)
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_STAMINA,
                        SPELL_MOLDARS_MOXIE,
                        SPELL_PRAYER_OF_FORTITUDE,
                        SPELL_FLASK_OF_THE_TITANS,
                        SPELL_ELIXIR_OF_FORTITUDE,
                        SPELL_ELIXIR_OF_THE_MONGOOSE,
                        SPELL_ELIXIR_OF_SUPERIOR_DEFENSE,
                        SPELL_BLOOD_PACT,
                        SPELL_GREATER_STONESHIELD_POTION,
                        SPELL_CRYSTAL_WARD,
                        SPELL_ROIDS,
                        SPELL_BLOODKELP_ELIXIR_OF_DODGING,
                        SPELL_RUMSEY_RUM_BLACK_LABEL,
                        SPELL_BUTTERMILK_DELIGHT
                    });

                    if (pPlayer->GetTeam() == ALLIANCE)
                        ApplyTimedBuff(pPlayer, SPELL_DEVOTION_AURA);
                }
                else
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                        SPELL_FURY_OF_THE_BOGLING,
                        SPELL_ELIXIR_OF_THE_MONGOOSE,
                        SPELL_WINTERFALL_FIREWATER,
                        SPELL_JUJU_MIGHT,
                        SPELL_JUJU_POWER,
                        SPELL_GROUND_SCORPOK_ASSAY,
                        SPELL_ROIDS,
                        SPELL_GRILLED_SQUID,
                        SPELL_BLESSED_SUNFRUIT,
                        SPELL_SMOKED_DESERT_DUMPLINGS,
                        SPELL_DARK_DESIRE
                    });
                }
                break;

            case CLASS_PRIEST:
                if (pPlayer->GetTalentTabID() == PriestShadow)
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                        SPELL_FLASK_OF_SUPREME_POWER,
                        SPELL_GREATER_ARCANE_ELIXIR,
                        SPELL_ELIXIR_OF_SHADOW_POWER,
                        SPELL_MAGEBLOOD_POTION,
                        SPELL_CRYSTAL_FORCE,
                        SPELL_NIGHTFIN_SOUP,
                        SPELL_VERY_BERRY_CREAM
                    });
                }
                else
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_INTELLIGENCE,
                        SPELL_FLASK_OF_DISTILLED_WISDOM,
                        SPELL_ELIXIR_OF_THE_SAGES,
                        SPELL_MAGEBLOOD_POTION,
                        SPELL_NIGHTFIN_SOUP,
                        SPELL_CRYSTAL_FORCE,
                        SPELL_SWEET_SURPRISE
                    });
                }
                break;

            case CLASS_PALADIN:
                if (pPlayer->GetTalentTabID() == PaladinProtection)
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_STAMINA,
                        SPELL_MOLDARS_MOXIE,
                        SPELL_PRAYER_OF_FORTITUDE,
                        SPELL_FLASK_OF_THE_TITANS,
                        SPELL_ELIXIR_OF_FORTITUDE,
                        SPELL_ELIXIR_OF_THE_MONGOOSE,
                        SPELL_ELIXIR_OF_SUPERIOR_DEFENSE,
                        SPELL_BLOOD_PACT,
                        SPELL_GREATER_STONESHIELD_POTION,
                        SPELL_CRYSTAL_WARD,
                        SPELL_ROIDS,
                        SPELL_BLOODKELP_ELIXIR_OF_DODGING,
                        SPELL_RUMSEY_RUM_BLACK_LABEL,
                        SPELL_DEVOTION_AURA,
                        SPELL_VERY_BERRY_CREAM,
                        SPELL_BUTTERMILK_DELIGHT
                    });
                }
                else if (pPlayer->GetTalentTabID() == PaladinRetribution)
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                        SPELL_ELIXIR_OF_THE_MONGOOSE,
                        SPELL_JUJU_MIGHT,
                        SPELL_ROIDS,
                        SPELL_WINTERFALL_FIREWATER,
                        SPELL_SMOKED_DESERT_DUMPLINGS,
                        SPELL_VERY_BERRY_CREAM,
                        SPELL_DARK_DESIRE
                    });
                }
                else
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_INTELLIGENCE,
                        SPELL_FLASK_OF_DISTILLED_WISDOM,
                        SPELL_ELIXIR_OF_THE_SAGES,
                        SPELL_MAGEBLOOD_POTION,
                        SPELL_NIGHTFIN_SOUP,
                        SPELL_CRYSTAL_FORCE,
                        SPELL_SWEET_SURPRISE
                    });
                }
                break;

            case CLASS_ROGUE:
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
                break;

            case CLASS_MAGE:
                ApplyBuffList(pPlayer,
                {
                    SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                    SPELL_FLASK_OF_SUPREME_POWER,
                    SPELL_GREATER_ARCANE_ELIXIR,
                    SPELL_MAGEBLOOD_POTION,
                    SPELL_CRYSTAL_FORCE,
                    SPELL_HEADMASTERS_CHARGE,
                    SPELL_BLESSING_OF_BLACKFATHOM,
                    SPELL_CEREBRAL_CORTEX_COMPOUND,
                    SPELL_RUNN_TUM_TUBER_SURPRISE,
                    SPELL_VERY_BERRY_CREAM
                });

                if (pPlayer->GetTalentTabID() == MageFire)
                    ApplyTimedBuff(pPlayer, SPELL_ELIXIR_OF_GREATER_FIREPOWER);
                else
                    ApplyTimedBuff(pPlayer, SPELL_ELIXIR_OF_FROST_POWER);
                break;

            case CLASS_SHAMAN:
                if (pPlayer->GetTalentTabID() == ShamanElementalCombat)
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                        SPELL_FLASK_OF_SUPREME_POWER,
                        SPELL_GREATER_ARCANE_ELIXIR,
                        SPELL_MAGEBLOOD_POTION,
                        SPELL_CRYSTAL_FORCE,
                        SPELL_HEADMASTERS_CHARGE,
                        SPELL_BLESSING_OF_BLACKFATHOM,
                        SPELL_CEREBRAL_CORTEX_COMPOUND,
                        SPELL_RUNN_TUM_TUBER_SURPRISE,
                        SPELL_ELIXIR_OF_GREATER_FIREPOWER,
                        SPELL_VERY_BERRY_CREAM
                    });
                }
                else if (pPlayer->GetTalentTabID() == ShamanEnhancement)
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                        SPELL_FLASK_OF_DISTILLED_WISDOM,
                        SPELL_FURY_OF_THE_BOGLING,
                        SPELL_ELIXIR_OF_THE_MONGOOSE,
                        SPELL_WINTERFALL_FIREWATER,
                        SPELL_JUJU_MIGHT,
                        SPELL_ROIDS,
                        SPELL_GROUND_SCORPOK_ASSAY,
                        SPELL_JUJU_POWER,
                        SPELL_VERY_BERRY_CREAM,
                        SPELL_SMOKED_DESERT_DUMPLINGS,
                        SPELL_DARK_DESIRE
                    });
                }
                else
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_INTELLIGENCE,
                        SPELL_FLASK_OF_DISTILLED_WISDOM,
                        SPELL_ELIXIR_OF_THE_SAGES,
                        SPELL_MAGEBLOOD_POTION,
                        SPELL_NIGHTFIN_SOUP,
                        SPELL_CRYSTAL_FORCE,
                        SPELL_SWEET_SURPRISE
                    });
                }
                break;

            case CLASS_HUNTER:
                ApplyBuffList(pPlayer,
                {
                    SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                    SPELL_FLASK_OF_DISTILLED_WISDOM,
                    SPELL_ELIXIR_OF_THE_MONGOOSE,
                    SPELL_WINTERFALL_FIREWATER,
                    SPELL_JUJU_MIGHT,
                    SPELL_ROIDS,
                    SPELL_GROUND_SCORPOK_ASSAY,
                    SPELL_JUJU_POWER,
                    SPELL_BLESSED_SUNFRUIT,
                    SPELL_SMOKED_DESERT_DUMPLINGS,
                    SPELL_GRILLED_SQUID,
                    SPELL_DARK_DESIRE,
                    SPELL_SWIFTNESS_OF_ZANZA
                });
                break;

            case CLASS_DRUID:
                if (pPlayer->GetTalentTabID() == DruidBalance)
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                        SPELL_FLASK_OF_SUPREME_POWER,
                        SPELL_GREATER_ARCANE_ELIXIR,
                        SPELL_ELIXIR_OF_SHADOW_POWER,
                        SPELL_MAGEBLOOD_POTION,
                        SPELL_CRYSTAL_FORCE,
                        SPELL_NIGHTFIN_SOUP,
                        SPELL_VERY_BERRY_CREAM
                    });
                }
                else if (pPlayer->GetTalentTabID() == DruidFeralCombat)
                {
                    if (pPlayer->HasSpell(16933))
                    {
                        ApplyBuffList(pPlayer,
                        {
                            SPELL_SAYGES_DARK_FORTUNE_OF_STAMINA,
                            SPELL_MOLDARS_MOXIE,
                            SPELL_PRAYER_OF_FORTITUDE,
                            SPELL_FLASK_OF_THE_TITANS,
                            SPELL_ELIXIR_OF_FORTITUDE,
                            SPELL_ELIXIR_OF_THE_MONGOOSE,
                            SPELL_ELIXIR_OF_SUPERIOR_DEFENSE,
                            SPELL_BLOOD_PACT,
                            SPELL_GREATER_STONESHIELD_POTION,
                            SPELL_CRYSTAL_WARD,
                            SPELL_ROIDS,
                            SPELL_BLOODKELP_ELIXIR_OF_DODGING,
                            SPELL_RUMSEY_RUM_BLACK_LABEL,
                            SPELL_DARK_DESIRE,
                            SPELL_BUTTERMILK_DELIGHT
                        });

                        if (pPlayer->GetTeam() == ALLIANCE)
                            ApplyTimedBuff(pPlayer, SPELL_DEVOTION_AURA);
                    }
                    else
                    {
                        ApplyBuffList(pPlayer,
                        {
                            SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                            SPELL_FLASK_OF_DISTILLED_WISDOM,
                            SPELL_FURY_OF_THE_BOGLING,
                            SPELL_MAGEBLOOD_POTION,
                            SPELL_ELIXIR_OF_THE_MONGOOSE,
                            SPELL_WINTERFALL_FIREWATER,
                            SPELL_JUJU_MIGHT,
                            SPELL_ROIDS,
                            SPELL_GROUND_SCORPOK_ASSAY,
                            SPELL_JUJU_POWER,
                            SPELL_BLESSED_SUNFRUIT,
                            SPELL_SMOKED_DESERT_DUMPLINGS,
                            SPELL_GRILLED_SQUID,
                            SPELL_DARK_DESIRE
                        });

                        if (pPlayer->GetTeam() == ALLIANCE)
                            ApplyTimedBuff(pPlayer, SPELL_DEVOTION_AURA);
                    }
                }
                else
                {
                    ApplyBuffList(pPlayer,
                    {
                        SPELL_SAYGES_DARK_FORTUNE_OF_INTELLIGENCE,
                        SPELL_FLASK_OF_DISTILLED_WISDOM,
                        SPELL_ELIXIR_OF_THE_SAGES,
                        SPELL_MAGEBLOOD_POTION,
                        SPELL_NIGHTFIN_SOUP,
                        SPELL_CRYSTAL_FORCE,
                        SPELL_SWEET_SURPRISE
                    });
                }
                break;

            case CLASS_WARLOCK:
                ApplyBuffList(pPlayer,
                {
                    SPELL_SAYGES_DARK_FORTUNE_OF_DAMAGE,
                    SPELL_FLASK_OF_SUPREME_POWER,
                    SPELL_GREATER_ARCANE_ELIXIR,
                    SPELL_ELIXIR_OF_SHADOW_POWER,
                    SPELL_MAGEBLOOD_POTION,
                    SPELL_CRYSTAL_FORCE,
                    SPELL_HEADMASTERS_CHARGE,
                    SPELL_BLESSING_OF_BLACKFATHOM,
                    SPELL_RUNN_TUM_TUBER_SURPRISE,
                    SPELL_VERY_BERRY_CREAM
                });
                break;

            default:
                break;
        }
    }

    void FullBuffPlayer(Player* pPlayer)
    {
        if (!pPlayer)
            return;

        ApplySharedWorldBuffs(pPlayer);
        ApplySharedClassBuffs(pPlayer);
        ApplySpecConsumes(pPlayer);
        ApplyHunterPetBuffs(pPlayer);
    }
}

struct npc_buff_machineAI : public ScriptedAI
{
    explicit npc_buff_machineAI(Creature* pCreature) : ScriptedAI(pCreature) { }

    void MoveInLineOfSight(Unit* pWho) override
    {
        if (!pWho)
            return;

        if (m_creature->GetEntry() != kBuffNpcEntry)
        {
            ScriptedAI::MoveInLineOfSight(pWho);
            return;
        }

        if (pWho->GetTypeId() != TYPEID_PLAYER)
            return;

        if (!m_creature->IsWithinDistInMap(pWho, kTriggerDistance))
            return;

        Player* pPlayer = pWho->ToPlayer();
        if (!pPlayer || !pPlayer->IsAlive())
            return;

        if (pPlayer->IsInCombat())
        {
            pPlayer->CastSpell(pPlayer, SPELL_KNOCKBACK_VISUAL, true);
            m_creature->MonsterWhisper("ERROR CODE 404 (YOU ARE IN COMBAT).", pPlayer);
            return;
        }

        if (pPlayer->HasAura(SPELL_BUFF_COOLDOWN))
            return;

        pPlayer->DuelComplete(DUEL_INTERRUPTED);
        pPlayer->RemoveAllSpellCooldown();
        pPlayer->ResetCharges();

        FullBuffPlayer(pPlayer);
        FillHealthAndMana(pPlayer);

        pPlayer->CastSpell(pPlayer, SPELL_VISUAL_RED_LIGHTNING, true);
        pPlayer->AddAura(SPELL_BUFF_COOLDOWN);
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

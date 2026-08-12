#ifndef MANGOS_PLAYER_AUTO_PROGRESSION_H
#define MANGOS_PLAYER_AUTO_PROGRESSION_H

#include "Common.h"

class Player;

namespace PlayerAutoProgression
{
    void OnLevelUp(Player* player);
    void OnTalentLearned(Player* player);
    void OnPlayerUpdate(Player* player);

    uint32 LearnAvailableTrainerSpells(Player* player);
    uint32 EquipBestItems(Player* player);
    uint32 EnchantBestItems(Player* player);
}

#endif

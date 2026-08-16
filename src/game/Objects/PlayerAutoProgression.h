#ifndef MANGOS_PLAYER_AUTO_PROGRESSION_H
#define MANGOS_PLAYER_AUTO_PROGRESSION_H

#include "Common.h"

#include <string>
#include <vector>

class Player;

namespace PlayerAutoProgression
{
    class CacheUpdateGuard
    {
    public:
        CacheUpdateGuard();
        ~CacheUpdateGuard();
        CacheUpdateGuard(CacheUpdateGuard const&) = delete;
        CacheUpdateGuard& operator=(CacheUpdateGuard const&) = delete;
    };

    class CacheReadGuard
    {
    public:
        CacheReadGuard();
        ~CacheReadGuard();
        CacheReadGuard(CacheReadGuard const&) = delete;
        CacheReadGuard& operator=(CacheReadGuard const&) = delete;
    };

    void OnLogin(Player* player);
    void OnLevelUp(Player* player);
    void OnTalentLearned(Player* player);
    void OnTalentsReset(Player* player);
    void OnPlayerUpdate(Player* player, uint32 diff);

    uint32 LearnAvailableTrainerSpells(Player* player);
    uint32 EquipBestItems(Player* player);
    uint32 EnchantBestItems(Player* player);

    void BuildStatus(Player* player, std::vector<std::string>& lines);
    void BuildPreview(Player* player, std::vector<std::string>& lines);
    bool ApplyNow(Player* player, uint32& learned, uint32& equipped, uint32& enchanted);

    bool StartAuditMatrix(char const* args, std::string& message);
    void BuildAuditMatrixStatus(std::vector<std::string>& lines);
    bool CancelAuditMatrix(std::string& message);
    void UpdateAuditMatrix(uint32 diff);
    void ShutdownAuditMatrix();
    bool IsAuditExecution();
    uint32 GenerateAuditItemLowGuid();
}

#endif

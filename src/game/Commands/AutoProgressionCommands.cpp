/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "Chat.h"
#include "Player.h"
#include "PlayerAutoProgression.h"

#include <string>
#include <vector>

bool ChatHandler::HandleAutoProgressionStatusCommand(char* args)
{
    Player* target = nullptr;
    if (!ExtractPlayerTarget(&args, &target))
        return false;
    if (HasLowerSecurity(target))
        return false;

    std::vector<std::string> lines;
    PlayerAutoProgression::BuildStatus(target, lines);
    for (std::string const& line : lines)
        SendSysMessage(line.c_str());
    return true;
}

bool ChatHandler::HandleAutoProgressionPreviewCommand(char* args)
{
    Player* target = nullptr;
    if (!ExtractPlayerTarget(&args, &target))
        return false;
    if (HasLowerSecurity(target))
        return false;

    std::vector<std::string> lines;
    PlayerAutoProgression::BuildPreview(target, lines);
    for (std::string const& line : lines)
        SendSysMessage(line.c_str());
    return true;
}

bool ChatHandler::HandleAutoProgressionApplyCommand(char* args)
{
    Player* target = nullptr;
    if (!ExtractPlayerTarget(&args, &target))
        return false;
    if ((!GetSession() || target != GetSession()->GetPlayer()) &&
        HasLowerSecurity(target, ObjectGuid(), true))
        return false;

    uint32 learned = 0;
    uint32 equipped = 0;
    uint32 enchanted = 0;
    if (!PlayerAutoProgression::ApplyNow(target, learned, equipped, enchanted))
    {
        PSendSysMessage("Auto progression could not be applied to %s in the current state.",
            GetNameLink(target).c_str());
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage("Auto progression applied to %s: learned %u spells, equipped %u items, enchanted %u items.",
        GetNameLink(target).c_str(), learned, equipped, enchanted);
    return true;
}

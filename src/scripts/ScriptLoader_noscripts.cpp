#include "scriptPCH.h"

void AddScripts()
{
}

/*
 * Arena.h declares these two for the chat command the player's arena window talks to, and the real
 * ones live in custom_arena.cpp next to the orb whose gossip menu shares them. A build configured
 * without scripts still links the command, so it needs something to link against - and answering
 * "no" is the honest answer for a server that has no arena orb script either.
 */
bool ArenaJoinQueueFromWindow(Player* /*player*/, ArenaType /*type*/)
{
    return false;
}

bool ArenaLeaveQueueFromWindow(Player* /*player*/, ArenaType /*type*/)
{
    return false;
}

bool ArenaSpectateFromWindow(Player* /*player*/, uint32 /*instanceId*/)
{
    return false;
}

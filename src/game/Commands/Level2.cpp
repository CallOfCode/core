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
 */

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "Database/DatabaseImpl.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "Item.h"
#include "Player.h"
#include "TemporarySummon.h"
#include "Totem.h"
#include "Pet.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "Opcodes.h"
#include "Chat.h"
#include "ObjectAccessor.h"
#include "MapManager.h"
#include "Language.h"
#include "World.h"
#include "GameEventMgr.h"
#include "ScriptMgr.h"
#include "SpellMgr.h"
#include "MapPersistentStateMgr.h"
#include "AccountMgr.h"
#include "WaypointManager.h"
#include "DBCStores.h"
#include "Util.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "WaypointMovementGenerator.h"
#include <cctype>
#include <iostream>
#include <fstream>
#include <map>
#include <typeinfo>
#include "Formulas.h"
#include "AsyncCommandHandlers.h"

#include "TargetedMovementGenerator.h"                      // for HandleNpcUnFollowCommand

static uint32 ReputationRankStrIndex[MAX_REPUTATION_RANK] =
{
    LANG_REP_HATED,    LANG_REP_HOSTILE, LANG_REP_UNFRIENDLY, LANG_REP_NEUTRAL,
    LANG_REP_FRIENDLY, LANG_REP_HONORED, LANG_REP_REVERED,    LANG_REP_EXALTED
};

//mute player for some times
bool ChatHandler::HandleMuteCommand(char* args)
{
    char* nameStr = ExtractOptNotLastArg(&args);

    Player* target;
    ObjectGuid target_guid;
    std::string target_name;
    if (!ExtractPlayerTarget(&nameStr, &target, &target_guid, &target_name))
        return false;

    uint32 notspeaktime;
    if (!ExtractUInt32(&args, notspeaktime))
        return false;

    std::string givenReason;
    if (char* givenReasonC = ExtractQuotedOrLiteralArg(&args))
        givenReason = givenReasonC;

    uint32 account_id = target ? target->GetSession()->GetAccountId() : sObjectMgr.GetPlayerAccountIdByGUID(target_guid);

    // find only player from same account if any
    if (!target)
    {
        if (WorldSession* session = sWorld.FindSession(account_id))
            target = session->GetPlayer();
    }

    // must have strong lesser security level
    if (HasLowerSecurity(target, target_guid, true))
        return false;

    time_t mutetime = time(NULL) + notspeaktime * 60;

    if (target)
        target->GetSession()->m_muteTime = mutetime;

    LoginDatabase.PExecute("UPDATE account SET mutetime = " UI64FMTD " WHERE id = '%u'", uint64(mutetime), account_id);

    if (target)
        ChatHandler(target).PSendSysMessage(LANG_YOUR_CHAT_DISABLED, notspeaktime);

    std::string nameLink = playerLink(target_name);

    PSendSysMessage(LANG_YOU_DISABLE_CHAT, nameLink.c_str(), notspeaktime);

    // Add warning to the account
    std::string authorName = m_session ? m_session->GetPlayerName() : "Console";
    PlayerCacheData const* playerData = sObjectMgr.GetPlayerDataByGUID(target_guid);
    ASSERT(playerData);
    std::stringstream reason;
    reason << playerData->sName << " muted " << notspeaktime << " minutes";
    if (givenReason != "")
        reason << " for \"" << givenReason << "\"";
    sWorld.WarnAccount(playerData->uiAccount, authorName, reason.str(), "WARNING");
    return true;
}

//unmute player
bool ChatHandler::HandleUnmuteCommand(char* args)
{
    Player* target;
    ObjectGuid target_guid;
    std::string target_name;
    if (!ExtractPlayerTarget(&args, &target, &target_guid, &target_name))
        return false;

    uint32 account_id = target ? target->GetSession()->GetAccountId() : sObjectMgr.GetPlayerAccountIdByGUID(target_guid);

    // find only player from same account if any
    if (!target)
    {
        if (WorldSession* session = sWorld.FindSession(account_id))
            target = session->GetPlayer();
    }

    // must have strong lesser security level
    if (HasLowerSecurity(target, target_guid, true))
        return false;

    if (target)
    {
        if (target->CanSpeak())
        {
            SendSysMessage(LANG_CHAT_ALREADY_ENABLED);
            SetSentErrorMessage(true);
            return false;
        }

        target->GetSession()->m_muteTime = 0;
    }

    LoginDatabase.PExecute("UPDATE account SET mutetime = '0' WHERE id = '%u'", account_id);

    if (target)
        ChatHandler(target).PSendSysMessage(LANG_YOUR_CHAT_ENABLED);

    std::string nameLink = playerLink(target_name);

    PSendSysMessage(LANG_YOU_ENABLE_CHAT, nameLink.c_str());
    return true;
}

void ChatHandler::ShowTriggerTargetListHelper(uint32 id, AreaTriggerTeleport const* at, bool subpart /*= false*/)
{
    if (m_session)
    {
        char dist_buf[50];
        if (!subpart)
        {
            float dist = m_session->GetPlayer()->GetDistance2d(at->target_X, at->target_Y);
            snprintf(dist_buf, 50, GetMangosString(LANG_TRIGGER_DIST), dist);
        }
        else
            dist_buf[0] = '\0';

        PSendSysMessage(LANG_TRIGGER_TARGET_LIST_CHAT,
                        subpart ? " -> " : "", id, id, at->target_mapId, at->target_X, at->target_Y, at->target_Z, dist_buf);
    }
    else
        PSendSysMessage(LANG_TRIGGER_TARGET_LIST_CONSOLE,
                        subpart ? " -> " : "", id, at->target_mapId, at->target_X, at->target_Y, at->target_Z);
}

void ChatHandler::ShowTriggerListHelper(AreaTriggerEntry const * atEntry)
{

    char const* tavern = sObjectMgr.IsTavernAreaTrigger(atEntry->id) ? GetMangosString(LANG_TRIGGER_TAVERN) : "";
    char const* quest = sObjectMgr.GetQuestForAreaTrigger(atEntry->id) ? GetMangosString(LANG_TRIGGER_QUEST) : "";

    if (m_session)
    {
        float dist = m_session->GetPlayer()->GetDistance2d(atEntry->x, atEntry->y);
        char dist_buf[50];
        snprintf(dist_buf, 50, GetMangosString(LANG_TRIGGER_DIST), dist);

        PSendSysMessage(LANG_TRIGGER_LIST_CHAT,
                        atEntry->id, atEntry->id, atEntry->mapid, atEntry->x, atEntry->y, atEntry->z, dist_buf, tavern, quest);
    }
    else
        PSendSysMessage(LANG_TRIGGER_LIST_CONSOLE,
                        atEntry->id, atEntry->mapid, atEntry->x, atEntry->y, atEntry->z, tavern, quest);

    if (AreaTriggerTeleport const* at = sObjectMgr.GetAreaTriggerTeleport(atEntry->id))
        ShowTriggerTargetListHelper(atEntry->id, at, true);
}

bool ChatHandler::HandleTriggerCommand(char* args)
{
    AreaTriggerEntry const* atEntry = NULL;

    Player* pl = m_session ? m_session->GetPlayer() : NULL;

    // select by args
    if (*args)
    {
        uint32 atId;
        if (!ExtractUint32KeyFromLink(&args, "Hareatrigger", atId))
            return false;

        if (!atId)
            return false;

        atEntry = sObjectMgr.GetAreaTrigger(atId);

        if (!atEntry)
        {
            PSendSysMessage(LANG_COMMAND_GOAREATRNOTFOUND, atId);
            SetSentErrorMessage(true);
            return false;
        }
    }
    // find nearest
    else
    {
        if (!m_session)
            return false;

        float dist2 = MAP_SIZE * MAP_SIZE;

        Player* pl = m_session->GetPlayer();

        // Search triggers
        for (uint32 id = 0; id < sObjectMgr.GetMaxAreaTriggerId(); ++id)
        {
            AreaTriggerEntry const *atTestEntry = sObjectMgr.GetAreaTrigger(id);
            if (!atTestEntry)
                continue;

            if (atTestEntry->mapid != m_session->GetPlayer()->GetMapId())
                continue;

            float dx = atTestEntry->x - pl->GetPositionX();
            float dy = atTestEntry->y - pl->GetPositionY();

            float test_dist2 = dx * dx + dy * dy;

            if (test_dist2 >= dist2)
                continue;

            dist2 = test_dist2;
            atEntry = atTestEntry;
        }

        if (!atEntry)
        {
            SendSysMessage(LANG_COMMAND_NOTRIGGERFOUND);
            SetSentErrorMessage(true);
            return false;
        }
    }

    ShowTriggerListHelper(atEntry);

    int loc_idx = GetSessionDbLocaleIndex();

    AreaTriggerTeleport const* at = sObjectMgr.GetAreaTriggerTeleport(atEntry->id);
    if (at)
        PSendSysMessage(LANG_TRIGGER_REQ_LEVEL, at->requiredLevel);

    if (uint32 quest_id = sObjectMgr.GetQuestForAreaTrigger(atEntry->id))
    {
        SendSysMessage(LANG_TRIGGER_EXPLORE_QUEST);
        ShowQuestListHelper(quest_id, loc_idx, pl);
    }

    if (at)
    {
        if (at->requiredItem || at->requiredItem2)
        {
            SendSysMessage(LANG_TRIGGER_REQ_ITEMS);

            if (at->requiredItem)
                ShowItemListHelper(at->requiredItem, loc_idx, pl);
            if (at->requiredItem2)
                ShowItemListHelper(at->requiredItem2, loc_idx, pl);
        }

        if (at->requiredQuest)
        {
            SendSysMessage(LANG_TRIGGER_REQ_QUEST);
            ShowQuestListHelper(at->requiredQuest, loc_idx, pl);
        }
    }

    return true;
}

bool ChatHandler::HandleTriggerActiveCommand(char* /*args*/)
{
    uint32 counter = 0;                                     // Counter for figure out that we found smth.

    Player* pl = m_session->GetPlayer();

    // Search in AreaTable.dbc
    for (uint32 id = 0; id < sObjectMgr.GetMaxAreaTriggerId(); ++id)
    {
        AreaTriggerEntry const *atEntry = sObjectMgr.GetAreaTrigger(id);
        if (!atEntry)
            continue;

        if (!IsPointInAreaTriggerZone(atEntry, pl->GetMapId(), pl->GetPositionX(), pl->GetPositionY(), pl->GetPositionZ()))
            continue;

        ShowTriggerListHelper(atEntry);

        ++counter;
    }

    if (counter == 0)                                      // if counter == 0 then we found nth
        SendSysMessage(LANG_COMMAND_NOTRIGGERFOUND);

    return true;
}

bool ChatHandler::HandleTriggerNearCommand(char* args)
{
    float distance = (!*args) ? 10.0f : (float)atof(args);
    float dist2 =  distance * distance;
    uint32 counter = 0;                                     // Counter for figure out that we found smth.

    Player* pl = m_session->GetPlayer();

    // Search triggers
    for (uint32 id = 0; id < sObjectMgr.GetMaxAreaTriggerId(); ++id)
    {
        AreaTriggerEntry const *atEntry = sObjectMgr.GetAreaTrigger(id);
        if (!atEntry)
            continue;

        if (atEntry->mapid != m_session->GetPlayer()->GetMapId())
            continue;

        float dx = atEntry->x - pl->GetPositionX();
        float dy = atEntry->y - pl->GetPositionY();

        if (dx * dx + dy * dy > dist2)
            continue;

        ShowTriggerListHelper(atEntry);

        ++counter;
    }

    // Search trigger targets
    for (uint32 id = 0; id < sObjectMgr.GetMaxAreaTriggerId(); ++id)
    {
        AreaTriggerEntry const *atEntry = sObjectMgr.GetAreaTrigger(id);
        if (!atEntry)
            continue;

        AreaTriggerTeleport const* at = sObjectMgr.GetAreaTriggerTeleport(atEntry->id);
        if (!at)
            continue;

        if (at->target_mapId != m_session->GetPlayer()->GetMapId())
            continue;

        float dx = at->target_X - pl->GetPositionX();
        float dy = at->target_Y - pl->GetPositionY();

        if (dx * dx + dy * dy > dist2)
            continue;

        ShowTriggerTargetListHelper(atEntry->id, at);

        ++counter;
    }

    if (counter == 0)                                      // if counter == 0 then we found nth
        SendSysMessage(LANG_COMMAND_NOTRIGGERFOUND);

    return true;
}

static char const* const areatriggerKeys[] =
{
    "Hareatrigger",
    "Hareatrigger_target",
    NULL
};

bool ChatHandler::HandleGoTriggerCommand(char* args)
{
    Player* _player = m_session->GetPlayer();

    if (!*args)
        return false;

    char *atIdStr = ExtractKeyFromLink(&args, areatriggerKeys);
    if (!atIdStr)
        return false;

    uint32 atId;
    if (!ExtractUInt32(&atIdStr, atId))
        return false;

    if (!atId)
        return false;

    AreaTriggerEntry const* atEntry = sObjectMgr.GetAreaTrigger(atId);
    if (!atEntry)
    {
        PSendSysMessage(LANG_COMMAND_GOAREATRNOTFOUND, atId);
        SetSentErrorMessage(true);
        return false;
    }

    bool to_target = ExtractLiteralArg(&args, "target");
    if (!to_target && *args)                                // can be fail also at syntax error
        return false;

    if (to_target)
    {
        AreaTriggerTeleport const* at = sObjectMgr.GetAreaTriggerTeleport(atId);
        if (!at)
        {
            PSendSysMessage(LANG_AREATRIGER_NOT_HAS_TARGET, atId);
            SetSentErrorMessage(true);
            return false;
        }

        return HandleGoHelper(_player, at->target_mapId, at->target_X, at->target_Y, &at->target_Z);
    }
    else
        return HandleGoHelper(_player, atEntry->mapid, atEntry->x, atEntry->y, &atEntry->z);
}

bool ChatHandler::HandleGoGraveyardCommand(char* args)
{
    Player* _player = m_session->GetPlayer();

    uint32 gyId;
    if (!ExtractUInt32(&args, gyId))
        return false;

    WorldSafeLocsEntry const* gy = sWorldSafeLocsStore.LookupEntry(gyId);
    if (!gy)
    {
        PSendSysMessage(LANG_COMMAND_GRAVEYARDNOEXIST, gyId);
        SetSentErrorMessage(true);
        return false;
    }

    return HandleGoHelper(_player, gy->map_id, gy->x, gy->y, &gy->z);
}

enum CreatureLinkType
{
    CREATURE_LINK_RAW               = -1,                   // non-link case
    CREATURE_LINK_GUID              = 0,
    CREATURE_LINK_ENTRY             = 1,
};

static char const* const creatureKeys[] =
{
    "Hcreature",
    "Hcreature_entry",
    NULL
};

/** \brief Teleport the GM to the specified creature
*
* .go creature <GUID>     --> TP using creature.guid
* .go creature azuregos   --> TP player to the mob with this name
*                             Warning: If there is more than one mob with this name
*                                      you will be teleported to the first one that is found.
* .go creature id 6109    --> TP player to the mob, that has this creature_template.entry
*                             Warning: If there is more than one mob with this "id"
*                                      you will be teleported to the first one that is found.
*/
//teleport to creature
bool ChatHandler::HandleGoCreatureCommand(char* args)
{
    if (!*args)
        return false;

    Player* _player = m_session->GetPlayer();

    // "id" or number or [name] Shift-click form |color|Hcreature:creature_id|h[name]|h|r
    int crType;
    char* pParam1 = ExtractKeyFromLink(&args, creatureKeys, &crType);
    if (!pParam1)
        return false;

    // User wants to teleport to the NPC's template entry
    if (crType == CREATURE_LINK_RAW && strcmp(pParam1, "id") == 0)
    {
        // number or [name] Shift-click form |color|Hcreature_entry:creature_id|h[name]|h|r
        pParam1 = ExtractKeyFromLink(&args, "Hcreature_entry");
        if (!pParam1)
            return false;

        crType = CREATURE_LINK_ENTRY;
    }

    CreatureData const* data = NULL;

    uint32 lowguid = 0;
    switch (crType)
    {
        case CREATURE_LINK_ENTRY:
        {
            uint32 tEntry;
            if (!ExtractUInt32(&pParam1, tEntry))
                return false;

            if (!tEntry)
                return false;

            if (!ObjectMgr::GetCreatureTemplate(tEntry))
            {
                SendSysMessage(LANG_COMMAND_GOCREATNOTFOUND);
                SetSentErrorMessage(true);
                return false;
            }

            FindCreatureData worker(tEntry, m_session ? m_session->GetPlayer() : NULL);

            sObjectMgr.DoCreatureData(worker);

            CreatureDataPair const* dataPair = worker.GetResult();
            if (!dataPair)
            {
                SendSysMessage(LANG_COMMAND_GOCREATNOTFOUND);
                SetSentErrorMessage(true);
                return false;
            }

            data = &dataPair->second;
            lowguid = dataPair->first;
            break;
        }
        case CREATURE_LINK_GUID:
        {
            if (!ExtractUInt32(&pParam1, lowguid))
                return false;

            data = sObjectMgr.GetCreatureData(lowguid);
            if (!data)
            {
                SendSysMessage(LANG_COMMAND_GOCREATNOTFOUND);
                SetSentErrorMessage(true);
                return false;
            }
            break;
        }
        case CREATURE_LINK_RAW:
        {
            if (ExtractUInt32(&pParam1, lowguid))
            {
                data = sObjectMgr.GetCreatureData(lowguid);
                if (!data)
                {
                    SendSysMessage(LANG_COMMAND_GOCREATNOTFOUND);
                    SetSentErrorMessage(true);
                    return false;
                }
            }
            // Number is invalid - maybe the user specified the mob's name
            else
            {
                std::string name = pParam1;
                WorldDatabase.escape_string(name);
                QueryResult *result = WorldDatabase.PQuery("SELECT guid FROM creature, creature_template WHERE creature.id = creature_template.entry AND creature_template.name " _LIKE_ " " _CONCAT3_("'%%'", "'%s'", "'%%'"), name.c_str());
                if (!result)
                {
                    SendSysMessage(LANG_COMMAND_GOCREATNOTFOUND);
                    SetSentErrorMessage(true);
                    return false;
                }

                FindCreatureData worker(0, m_session ? m_session->GetPlayer() : NULL);

                do
                {
                    Field *fields = result->Fetch();
                    uint32 guid = fields[0].GetUInt32();

                    CreatureDataPair const* cr_data = sObjectMgr.GetCreatureDataPair(guid);
                    if (!cr_data)
                        continue;

                    worker(*cr_data);

                }
                while (result->NextRow());

                delete result;

                CreatureDataPair const* dataPair = worker.GetResult();
                if (!dataPair)
                {
                    SendSysMessage(LANG_COMMAND_GOCREATNOTFOUND);
                    SetSentErrorMessage(true);
                    return false;
                }

                data = &dataPair->second;
                lowguid = dataPair->first;
            }
            break;
        }
    }

    float x, y, z;
    x = data->posX;
    y = data->posY;
    z = data->posZ;
    if (Creature* creature = _player->GetMap()->GetCreature(data->GetObjectGuid(lowguid)))
        creature->GetPosition(x, y, z);
    return HandleGoHelper(_player, data->mapid, x, y, &z);
}

enum GameobjectLinkType
{
    GAMEOBJECT_LINK_RAW             = -1,                   // non-link case
    GAMEOBJECT_LINK_GUID            = 0,
    GAMEOBJECT_LINK_ENTRY           = 1,
};

static char const* const gameobjectKeys[] =
{
    "Hgameobject",
    "Hgameobject_entry",
    NULL
};

//teleport to gameobject
bool ChatHandler::HandleGoObjectCommand(char* args)
{
    Player* _player = m_session->GetPlayer();

    // number or [name] Shift-click form |color|Hgameobject:go_guid|h[name]|h|r
    int goType;
    char* pParam1 = ExtractKeyFromLink(&args, gameobjectKeys, &goType);
    if (!pParam1)
        return false;

    // User wants to teleport to the GO's template entry
    if (goType == GAMEOBJECT_LINK_RAW && strcmp(pParam1, "id") == 0)
    {
        // number or [name] Shift-click form |color|Hgameobject_entry:creature_id|h[name]|h|r
        pParam1 = ExtractKeyFromLink(&args, "Hgameobject_entry");
        if (!pParam1)
            return false;

        goType = GAMEOBJECT_LINK_ENTRY;
    }

    GameObjectData const* data = NULL;

    switch (goType)
    {
        case CREATURE_LINK_ENTRY:
        {
            uint32 tEntry;
            if (!ExtractUInt32(&pParam1, tEntry))
                return false;

            if (!tEntry)
                return false;

            if (!ObjectMgr::GetGameObjectInfo(tEntry))
            {
                SendSysMessage(LANG_COMMAND_GOOBJNOTFOUND);
                SetSentErrorMessage(true);
                return false;
            }

            FindGOData worker(tEntry, m_session ? m_session->GetPlayer() : NULL);

            sObjectMgr.DoGOData(worker);

            GameObjectDataPair const* dataPair = worker.GetResult();

            if (!dataPair)
            {
                SendSysMessage(LANG_COMMAND_GOOBJNOTFOUND);
                SetSentErrorMessage(true);
                return false;
            }

            data = &dataPair->second;
            break;
        }
        case GAMEOBJECT_LINK_GUID:
        {
            uint32 lowguid;
            if (!ExtractUInt32(&pParam1, lowguid))
                return false;

            // by DB guid
            data = sObjectMgr.GetGOData(lowguid);
            if (!data)
            {
                SendSysMessage(LANG_COMMAND_GOOBJNOTFOUND);
                SetSentErrorMessage(true);
                return false;
            }
            break;
        }
        case GAMEOBJECT_LINK_RAW:
        {
            uint32 lowguid;
            if (ExtractUInt32(&pParam1, lowguid))
            {
                // by DB guid
                data = sObjectMgr.GetGOData(lowguid);
                if (!data)
                {
                    SendSysMessage(LANG_COMMAND_GOOBJNOTFOUND);
                    SetSentErrorMessage(true);
                    return false;
                }
            }
            else
            {
                std::string name = pParam1;
                WorldDatabase.escape_string(name);
                QueryResult *result = WorldDatabase.PQuery("SELECT guid FROM gameobject, gameobject_template WHERE gameobject.id = gameobject_template.entry AND gameobject_template.name " _LIKE_ " " _CONCAT3_("'%%'", "'%s'", "'%%'"), name.c_str());
                if (!result)
                {
                    SendSysMessage(LANG_COMMAND_GOOBJNOTFOUND);
                    SetSentErrorMessage(true);
                    return false;
                }

                FindGOData worker(0, m_session ? m_session->GetPlayer() : NULL);

                do
                {
                    Field *fields = result->Fetch();
                    uint32 guid = fields[0].GetUInt32();

                    GameObjectDataPair const* go_data = sObjectMgr.GetGODataPair(guid);
                    if (!go_data)
                        continue;

                    worker(*go_data);

                }
                while (result->NextRow());

                delete result;

                GameObjectDataPair const* dataPair = worker.GetResult();
                if (!dataPair)
                {
                    SendSysMessage(LANG_COMMAND_GOOBJNOTFOUND);
                    SetSentErrorMessage(true);
                    return false;
                }

                data = &dataPair->second;
            }
            break;
        }
    }

    return HandleGoHelper(_player, data->mapid, data->posX, data->posY, &data->posZ);
}

bool ChatHandler::HandleGameObjectTargetCommand(char* args)
{
    Player* pl = m_session->GetPlayer();
    QueryResult *result;
    GameEventMgr::ActiveEvents const& activeEventsList = sGameEventMgr.GetActiveEventList();
    if (*args)
    {
        // number or [name] Shift-click form |color|Hgameobject_entry:go_id|h[name]|h|r
        char* cId = ExtractKeyFromLink(&args, "Hgameobject_entry");
        if (!cId)
            return false;

        uint32 id;
        if (ExtractUInt32(&cId, id))
        {
            result = WorldDatabase.PQuery("SELECT guid, id, position_x, position_y, position_z, orientation, map, (POW(position_x - '%f', 2) + POW(position_y - '%f', 2) + POW(position_z - '%f', 2)) AS order_ FROM gameobject WHERE map = '%i' AND id = '%u' ORDER BY order_ ASC LIMIT 1",
                                          pl->GetPositionX(), pl->GetPositionY(), pl->GetPositionZ(), pl->GetMapId(), id);
        }
        else
        {
            std::string name = cId;
            WorldDatabase.escape_string(name);
            result = WorldDatabase.PQuery(
                         "SELECT guid, id, position_x, position_y, position_z, orientation, map, (POW(position_x - %f, 2) + POW(position_y - %f, 2) + POW(position_z - %f, 2)) AS order_ "
                         "FROM gameobject,gameobject_template WHERE gameobject_template.entry = gameobject.id AND map = %i AND name " _LIKE_ " " _CONCAT3_("'%%'", "'%s'", "'%%'")" ORDER BY order_ ASC LIMIT 1",
                         pl->GetPositionX(), pl->GetPositionY(), pl->GetPositionZ(), pl->GetMapId(), name.c_str());
        }
    }
    else
    {
        std::ostringstream eventFilter;
        eventFilter << " AND (event IS NULL ";
        bool initString = true;

        for (GameEventMgr::ActiveEvents::const_iterator itr = activeEventsList.begin(); itr != activeEventsList.end(); ++itr)
        {
            if (initString)
            {
                eventFilter  <<  "OR event IN (" << *itr;
                initString = false;
            }
            else
                eventFilter << "," << *itr;
        }

        if (!initString)
            eventFilter << "))";
        else
            eventFilter << ")";

        result = WorldDatabase.PQuery("SELECT gameobject.guid, id, position_x, position_y, position_z, orientation, map, "
                                      "(POW(position_x - %f, 2) + POW(position_y - %f, 2) + POW(position_z - %f, 2)) AS order_ FROM gameobject "
                                      "LEFT OUTER JOIN game_event_gameobject on gameobject.guid=game_event_gameobject.guid WHERE map = '%i' %s ORDER BY order_ ASC LIMIT 10",
                                      m_session->GetPlayer()->GetPositionX(), m_session->GetPlayer()->GetPositionY(), m_session->GetPlayer()->GetPositionZ(), m_session->GetPlayer()->GetMapId(), eventFilter.str().c_str());
    }

    if (!result)
    {
        SendSysMessage(LANG_COMMAND_TARGETOBJNOTFOUND);
        return true;
    }

    bool found = false;
    float x, y, z, o;
    uint32 lowguid, id;
    uint16 mapid, pool_id;

    do
    {
        Field *fields = result->Fetch();
        lowguid = fields[0].GetUInt32();
        id =      fields[1].GetUInt32();
        x =       fields[2].GetFloat();
        y =       fields[3].GetFloat();
        z =       fields[4].GetFloat();
        o =       fields[5].GetFloat();
        mapid =   fields[6].GetUInt16();
        pool_id = sPoolMgr.IsPartOfAPool<GameObject>(lowguid);
        if (!pool_id || pl->GetMap()->GetPersistentState()->IsSpawnedPoolObject<GameObject>(lowguid))
            found = true;
    }
    while (result->NextRow() && (!found));

    delete result;

    if (!found)
    {
        PSendSysMessage(LANG_GAMEOBJECT_NOT_EXIST, id);
        return false;
    }

    GameObjectInfo const* goI = ObjectMgr::GetGameObjectInfo(id);

    if (!goI)
    {
        PSendSysMessage(LANG_GAMEOBJECT_NOT_EXIST, id);
        return false;
    }

    GameObject* target = m_session->GetPlayer()->GetMap()->GetGameObject(ObjectGuid(HIGHGUID_GAMEOBJECT, id, lowguid));

    PSendSysMessage(LANG_GAMEOBJECT_DETAIL, lowguid, goI->name, lowguid, id, x, y, z, mapid, o);

    if (target)
    {
        time_t curRespawnDelay = target->GetRespawnTimeEx() - time(NULL);
        if (curRespawnDelay < 0)
            curRespawnDelay = 0;

        std::string curRespawnDelayStr = secsToTimeString(curRespawnDelay, true);
        std::string defRespawnDelayStr = secsToTimeString(target->GetRespawnDelay(), true);

        PSendSysMessage(LANG_COMMAND_RAWPAWNTIMES, defRespawnDelayStr.c_str(), curRespawnDelayStr.c_str());

        ShowNpcOrGoSpawnInformation<GameObject>(target->GetGUIDLow());
    }
    return true;
}

//delete object by selection or guid
bool ChatHandler::HandleGameObjectDeleteCommand(char* args)
{
    // number or [name] Shift-click form |color|Hgameobject:go_guid|h[name]|h|r
    uint32 lowguid;
    if (!ExtractUint32KeyFromLink(&args, "Hgameobject", lowguid))
        return false;

    if (!lowguid)
        return false;

    GameObject* obj = NULL;

    // by DB guid
    if (GameObjectData const* go_data = sObjectMgr.GetGOData(lowguid))
        obj = GetGameObjectWithGuid(lowguid, go_data->id);

    if (!obj)
    {
        PSendSysMessage(LANG_COMMAND_OBJNOTFOUND, lowguid);
        SetSentErrorMessage(true);
        return false;
    }

    if (ObjectGuid ownerGuid = obj->GetOwnerGuid())
    {
        Unit* owner = ObjectAccessor::GetUnit(*m_session->GetPlayer(), ownerGuid);
        if (!owner || !ownerGuid.IsPlayer())
        {
            PSendSysMessage(LANG_COMMAND_DELOBJREFERCREATURE, obj->GetGUIDLow(), ownerGuid.GetString().c_str());
            SetSentErrorMessage(true);
            return false;
        }

        owner->RemoveGameObject(obj, false);
    }

    obj->SetRespawnTime(0);                                 // not save respawn time
    obj->Delete();
    obj->DeleteFromDB();

    PSendSysMessage(LANG_COMMAND_DELOBJMESSAGE, obj->GetGUIDLow());

    return true;
}

//turn selected object
bool ChatHandler::HandleGameObjectTurnCommand(char* args)
{
    // number or [name] Shift-click form |color|Hgameobject:go_id|h[name]|h|r
    uint32 lowguid;
    if (!ExtractUint32KeyFromLink(&args, "Hgameobject", lowguid))
        return false;

    if (!lowguid)
        return false;

    GameObject* obj = NULL;

    // by DB guid
    if (GameObjectData const* go_data = sObjectMgr.GetGOData(lowguid))
        obj = GetGameObjectWithGuid(lowguid, go_data->id);

    if (!obj)
    {
        PSendSysMessage(LANG_COMMAND_OBJNOTFOUND, lowguid);
        SetSentErrorMessage(true);
        return false;
    }

    float o;
    if (!ExtractOptFloat(&args, o, m_session->GetPlayer()->GetOrientation()))
        return false;

    Map* map = obj->GetMap();
    map->Remove(obj, false);

    obj->Relocate(obj->GetPositionX(), obj->GetPositionY(), obj->GetPositionZ(), o);
    obj->UpdateRotationFields();

    map->Add(obj);

    obj->SaveToDB();
    obj->Refresh();

    PSendSysMessage(LANG_COMMAND_TURNOBJMESSAGE, obj->GetGUIDLow(), obj->GetGOInfo()->name, obj->GetGUIDLow());

    return true;
}

bool ChatHandler::HandleGameObjectInfoCommand(char* args)
{
    // number or [name] Shift-click form |color|Hgameobject:go_id|h[name]|h|r
    uint32 lowguid;
    if (!ExtractUint32KeyFromLink(&args, "Hgameobject", lowguid))
        return false;

    if (!lowguid)
        return false;

    GameObject* pGameObject = nullptr;

    // by DB guid
    if (GameObjectData const* go_data = sObjectMgr.GetGOData(lowguid))
        pGameObject = GetGameObjectWithGuid(lowguid, go_data->id);

    if (!pGameObject)
    {
        PSendSysMessage(LANG_COMMAND_OBJNOTFOUND, lowguid);
        SetSentErrorMessage(true);
        return false;
    }
    
    PSendSysMessage("Entry: %u, GUID: %u\nName: %s\nType: %u, Display Id: %u\nGO State: %u, Loot State: %u", pGameObject->GetEntry(), pGameObject->GetGUIDLow(), pGameObject->GetGOInfo()->name, pGameObject->GetGoType(), pGameObject->GetDisplayId(), pGameObject->GetGoState(), pGameObject->getLootState());
    SendSysMessage(pGameObject->isSpawned() ? "Object is spawned." : "Not spawned.");

    return true;
}

//move selected object
bool ChatHandler::HandleGameObjectMoveCommand(char* args)
{
    // number or [name] Shift-click form |color|Hgameobject:go_guid|h[name]|h|r
    uint32 lowguid;
    if (!ExtractUint32KeyFromLink(&args, "Hgameobject", lowguid))
        return false;

    if (!lowguid)
        return false;

    GameObject* obj = NULL;

    // by DB guid
    if (GameObjectData const* go_data = sObjectMgr.GetGOData(lowguid))
        obj = GetGameObjectWithGuid(lowguid, go_data->id);

    if (!obj)
    {
        PSendSysMessage(LANG_COMMAND_OBJNOTFOUND, lowguid);
        SetSentErrorMessage(true);
        return false;
    }

    if (!args)
    {
        Player *chr = m_session->GetPlayer();

        Map* map = obj->GetMap();
        map->Remove(obj, false);

        obj->Relocate(chr->GetPositionX(), chr->GetPositionY(), chr->GetPositionZ(), obj->GetOrientation());
        obj->SetFloatValue(GAMEOBJECT_POS_X, chr->GetPositionX());
        obj->SetFloatValue(GAMEOBJECT_POS_Y, chr->GetPositionY());
        obj->SetFloatValue(GAMEOBJECT_POS_Z, chr->GetPositionZ());

        map->Add(obj);
    }
    else
    {
        float x;
        if (!ExtractFloat(&args, x))
            return false;

        float y;
        if (!ExtractFloat(&args, y))
            return false;

        float z;
        if (!ExtractFloat(&args, z))
            return false;

        if (!MapManager::IsValidMapCoord(obj->GetMapId(), x, y, z))
        {
            PSendSysMessage(LANG_INVALID_TARGET_COORD, x, y, obj->GetMapId());
            SetSentErrorMessage(true);
            return false;
        }

        Map* map = obj->GetMap();
        map->Remove(obj, false);

        obj->Relocate(x, y, z, obj->GetOrientation());
        obj->SetFloatValue(GAMEOBJECT_POS_X, x);
        obj->SetFloatValue(GAMEOBJECT_POS_Y, y);
        obj->SetFloatValue(GAMEOBJECT_POS_Z, z);

        map->Add(obj);
    }

    obj->SaveToDB();
    obj->Refresh();

    PSendSysMessage(LANG_COMMAND_MOVEOBJMESSAGE, obj->GetGUIDLow(), obj->GetGOInfo()->name, obj->GetGUIDLow());

    return true;
}

//spawn go
bool ChatHandler::HandleGameObjectAddCommand(char* args)
{
    // number or [name] Shift-click form |color|Hgameobject_entry:go_id|h[name]|h|r
    uint32 id;
    if (!ExtractUint32KeyFromLink(&args, "Hgameobject_entry", id))
        return false;

    if (!id)
        return false;

    int32 spawntimeSecs;
    if (!ExtractOptInt32(&args, spawntimeSecs, 0))
        return false;

    const GameObjectInfo *gInfo = ObjectMgr::GetGameObjectInfo(id);

    if (!gInfo)
    {
        PSendSysMessage(LANG_GAMEOBJECT_NOT_EXIST, id);
        SetSentErrorMessage(true);
        return false;
    }

    if (gInfo->displayId && !sGameObjectDisplayInfoStore.LookupEntry(gInfo->displayId))
    {
        // report to DB errors log as in loading case
        sLog.outErrorDb("Gameobject (Entry %u GoType: %u) have invalid displayId (%u), not spawned.", id, gInfo->type, gInfo->displayId);
        PSendSysMessage(LANG_GAMEOBJECT_HAVE_INVALID_DATA, id);
        SetSentErrorMessage(true);
        return false;
    }

    Player *chr = m_session->GetPlayer();
    float x = float(chr->GetPositionX());
    float y = float(chr->GetPositionY());
    float z = float(chr->GetPositionZ());
    float o = float(chr->GetOrientation());
    Map *map = chr->GetMap();

    GameObject* pGameObj = new GameObject;

    // used guids from specially reserved range (can be 0 if no free values)
    uint32 db_lowGUID = sObjectMgr.GenerateStaticGameObjectLowGuid();
    if (!db_lowGUID)
    {
        SendSysMessage(LANG_NO_FREE_STATIC_GUID_FOR_SPAWN);
        SetSentErrorMessage(true);
        return false;
    }

    if (!pGameObj->Create(db_lowGUID, gInfo->id, map, x, y, z, o, 0.0f, 0.0f, 0.0f, 0.0f, GO_ANIMPROGRESS_DEFAULT, GO_STATE_READY))
    {
        delete pGameObj;
        return false;
    }

    if (spawntimeSecs)
        pGameObj->SetRespawnTime(spawntimeSecs);

    // fill the gameobject data and save to the db
    pGameObj->SaveToDB(map->GetId());

    // this will generate a new guid if the object is in an instance
    if (!pGameObj->LoadFromDB(db_lowGUID, map))
    {
        delete pGameObj;
        return false;
    }

    DEBUG_LOG(GetMangosString(LANG_GAMEOBJECT_CURRENT), gInfo->name, db_lowGUID, x, y, z, o);

    map->Add(pGameObj);

    sObjectMgr.AddGameobjectToGrid(db_lowGUID, sObjectMgr.GetGOData(db_lowGUID));

    PSendSysMessage(LANG_GAMEOBJECT_ADD, id, gInfo->name, db_lowGUID, x, y, z);
    return true;
}

bool ChatHandler::HandleGameObjectNearCommand(char* args)
{
    float distance;
    if (!ExtractOptFloat(&args, distance, 10.0f))
        return false;

    uint32 count = 0;

    Player* pl = m_session->GetPlayer();
    QueryResult *result = WorldDatabase.PQuery("SELECT guid, id, position_x, position_y, position_z, map, "
                          "(POW(position_x - '%f', 2) + POW(position_y - '%f', 2) + POW(position_z - '%f', 2)) AS order_ "
                          "FROM gameobject WHERE map='%u' AND (POW(position_x - '%f', 2) + POW(position_y - '%f', 2) + POW(position_z - '%f', 2)) <= '%f' ORDER BY order_",
                          pl->GetPositionX(), pl->GetPositionY(), pl->GetPositionZ(),
                          pl->GetMapId(), pl->GetPositionX(), pl->GetPositionY(), pl->GetPositionZ(), distance * distance);

    if (result)
    {
        do
        {
            Field *fields = result->Fetch();
            uint32 guid = fields[0].GetUInt32();
            uint32 entry = fields[1].GetUInt32();
            float x = fields[2].GetFloat();
            float y = fields[3].GetFloat();
            float z = fields[4].GetFloat();
            int mapid = fields[5].GetUInt16();

            GameObjectInfo const * gInfo = ObjectMgr::GetGameObjectInfo(entry);

            if (!gInfo)
                continue;

            PSendSysMessage(LANG_GO_MIXED_LIST_CHAT, guid, PrepareStringNpcOrGoSpawnInformation<GameObject>(guid).c_str(), entry, guid, gInfo->name, x, y, z, mapid);

            ++count;
        }
        while (result->NextRow());

        delete result;
    }

    PSendSysMessage(LANG_COMMAND_NEAROBJMESSAGE, distance, count);
    return true;
}

class NearestGameObjectInObjectRangeCheck
{
    public:
        NearestGameObjectInObjectRangeCheck(WorldObject const& obj, float range) : i_obj(obj), i_range(range) {}
        WorldObject const& GetFocusObject() const { return i_obj; }
        bool operator()(GameObject* go)
        {
            if (i_obj.IsWithinDistInMap(go, i_range))
            {
                i_range = i_obj.GetDistance(go);        // use found GO range as new range limit for next check
                return true;
            }
            return false;
        }
        float GetLastRange() const { return i_range; }
    private:
        WorldObject const& i_obj;
        float  i_range;
};


bool ChatHandler::HandleGameObjectSelectCommand(char*)
{
    const float dist = 10.0f;
    Player* player = m_session->GetPlayer();

    GameObject* go = NULL;
    CellPair pair(MaNGOS::ComputeCellPair(player->GetPositionX(), player->GetPositionY()));
    Cell cell(pair);
    cell.SetNoCreate();

    NearestGameObjectInObjectRangeCheck go_check(*player, dist);
    MaNGOS::GameObjectLastSearcher<NearestGameObjectInObjectRangeCheck> searcher(go, go_check);

    TypeContainerVisitor<MaNGOS::GameObjectLastSearcher<NearestGameObjectInObjectRangeCheck>, GridTypeMapContainer> go_searcher(searcher);
    cell.Visit(pair, go_searcher, *(player->GetMap()), *player, dist);
    if (go)
    {
        player->SetSelectedGobj(go->GetObjectGuid());
        GameObjectInfo const* data = go->GetGOInfo();
        PSendSysMessage(LANG_GO_MIXED_LIST_CHAT, go->GetDBTableGUIDLow(), PrepareStringNpcOrGoSpawnInformation<GameObject>(go->GetObjectGuid()).c_str(), go->GetEntry(), go->GetDBTableGUIDLow(), data ? data->name : go->GetName(), go->GetPositionX(), go->GetPositionY(), go->GetPositionZ(), go->GetMapId());
    }
    else
        SendSysMessage(LANG_COMMAND_NOGAMEOBJECTFOUND);
    return true;
}

GameObject* ChatHandler::getSelectedGameObject()
{
    Player* player = m_session->GetPlayer();
    return player->GetMap()->GetGameObject(player->GetSelectedGobj());
}

bool ChatHandler::HandleGameObjectDespawnCommand(char*)
{
    GameObject* go = getSelectedGameObject();
    if (!go)
    {
        SendSysMessage(LANG_COMMAND_NOGAMEOBJECTFOUND);
        return false;
    }
    go->Despawn();
    return true;
}

bool ChatHandler::HandleGameObjectToggleCommand(char* args)
{
    uint32 lowguid;
    GameObject* obj = NULL;

    if (!ExtractUint32KeyFromLink(&args, "Hgameobject", lowguid))
    {
        // no args passed, search for already selected GO
        obj = getSelectedGameObject();
        if (!obj)
        {
            PSendSysMessage(LANG_COMMAND_NOGAMEOBJECTFOUND);
            SetSentErrorMessage(true);
            return false;
        }
    }
    else
    {
        // lowguid arg found, search DB for GO
        if (GameObjectData const* go_data = sObjectMgr.GetGOData(lowguid))
            obj = GetGameObjectWithGuid(lowguid,go_data->id);

        if (!obj)
        {
            PSendSysMessage(LANG_COMMAND_OBJNOTFOUND, lowguid);
            SetSentErrorMessage(true);
            return false;
        }
    }

    if (obj->getLootState() == GO_READY || obj->getLootState() == GO_JUST_DEACTIVATED)
        obj->UseDoorOrButton();
    else
        obj->ResetDoorOrButton();

    return true;
}

bool ChatHandler::HandleGameObjectResetCommand(char*)
{
    GameObject* go = getSelectedGameObject();
    if (!go)
    {
        SendSysMessage(LANG_COMMAND_NOGAMEOBJECTFOUND);
        return false;
    }
    go->ResetDoorOrButton();
    return true;
}

bool ChatHandler::HandleGameObjectRespawnCommand(char*)
{
    GameObject* go = getSelectedGameObject();
    if (!go)
    {
        SendSysMessage(LANG_COMMAND_NOGAMEOBJECTFOUND);
        return false;
    }
    go->Respawn();
    return true;
}

bool ChatHandler::HandleGUIDCommand(char* /*args*/)
{
    ObjectGuid guid = m_session->GetPlayer()->GetSelectionGuid();

    if (!guid)
    {
        SendSysMessage(LANG_NO_SELECTION);
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage(LANG_OBJECT_GUID, guid.GetString().c_str());
    return true;
}

void ChatHandler::ShowFactionListHelper(FactionEntry const * factionEntry, LocaleConstant loc, FactionState const* repState /*= NULL*/, Player * target /*= NULL */)
{
    std::string name = factionEntry->name[loc];
    // send faction in "id - [faction] rank reputation [visible] [at war] [own team] [unknown] [invisible] [inactive]" format
    // or              "id - [faction] [no reputation]" format
    std::ostringstream ss;
    if (m_session)
        ss << factionEntry->ID << " - |cffffffff|Hfaction:" << factionEntry->ID << "|h[" << name << " " << localeNames[loc] << "]|h|r";
    else
        ss << factionEntry->ID << " - " << name << " " << localeNames[loc];

    if (repState)                               // and then target!=NULL also
    {
        ReputationRank rank = target->GetReputationMgr().GetRank(factionEntry);
        std::string rankName = GetMangosString(ReputationRankStrIndex[rank]);

        ss << " " << rankName << "|h|r (" << target->GetReputationMgr().GetReputation(factionEntry) << ")";

        if (repState->Flags & FACTION_FLAG_VISIBLE)
            ss << GetMangosString(LANG_FACTION_VISIBLE);
        if (repState->Flags & FACTION_FLAG_AT_WAR)
            ss << GetMangosString(LANG_FACTION_ATWAR);
        if (repState->Flags & FACTION_FLAG_PEACE_FORCED)
            ss << GetMangosString(LANG_FACTION_PEACE_FORCED);
        if (repState->Flags & FACTION_FLAG_HIDDEN)
            ss << GetMangosString(LANG_FACTION_HIDDEN);
        if (repState->Flags & FACTION_FLAG_INVISIBLE_FORCED)
            ss << GetMangosString(LANG_FACTION_INVISIBLE_FORCED);
        if (repState->Flags & FACTION_FLAG_INACTIVE)
            ss << GetMangosString(LANG_FACTION_INACTIVE);
    }
    else if (target)
        ss << GetMangosString(LANG_FACTION_NOREPUTATION);

    SendSysMessage(ss.str().c_str());
}

bool ChatHandler::HandleLookupFactionCommand(char* args)
{
    if (!*args)
        return false;

    // Can be NULL at console call
    Player *target = getSelectedPlayer();

    std::string namepart = args;
    std::wstring wnamepart;

    if (!Utf8toWStr(namepart, wnamepart))
        return false;

    // converting string that we try to find to lower case
    wstrToLower(wnamepart);

    uint32 counter = 0;                                     // Counter for figure out that we found smth.

    for (uint32 id = 0; id < sObjectMgr.GetMaxFactionId(); ++id)
    {
        FactionEntry const *factionEntry = sObjectMgr.GetFactionEntry(id);
        if (factionEntry)
        {
            int loc = GetSessionDbcLocale();
            std::string name = factionEntry->name[loc];
            if (name.empty())
                continue;

            if (!Utf8FitTo(name, wnamepart))
            {
                loc = 0;
                for (; loc < MAX_DBC_LOCALE; ++loc)
                {
                    if (loc == GetSessionDbcLocale())
                        continue;

                    name = factionEntry->name[loc];
                    if (name.empty())
                        continue;

                    if (Utf8FitTo(name, wnamepart))
                        break;
                }
            }

            if (loc < MAX_DBC_LOCALE)
            {
                FactionState const* repState = target ? target->GetReputationMgr().GetState(factionEntry) : NULL;
                ShowFactionListHelper(factionEntry, LocaleConstant(loc), repState, target);
                counter++;
            }
        }
    }

    if (counter == 0)                                       // if counter == 0 then we found nth
        SendSysMessage(LANG_COMMAND_FACTION_NOTFOUND);
    return true;
}

bool ChatHandler::HandleModifyRepCommand(char* args)
{
    if (!*args)
        return false;

    Player* target = NULL;
    target = getSelectedPlayer();

    if (!target)
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    // check online security
    if (HasLowerSecurity(target))
        return false;

    uint32 factionId;
    if (!ExtractUint32KeyFromLink(&args, "Hfaction", factionId))
        return false;

    if (!factionId)
        return false;

    int32 amount = 0;
    if (!ExtractInt32(&args, amount))
    {
        char *rankTxt = ExtractLiteralArg(&args);
        if (!rankTxt)
            return false;

        std::string rankStr = rankTxt;
        std::wstring wrankStr;
        if (!Utf8toWStr(rankStr, wrankStr))
            return false;
        wstrToLower(wrankStr);

        int r = 0;
        amount = -42000;
        for (; r < MAX_REPUTATION_RANK; ++r)
        {
            std::string rank = GetMangosString(ReputationRankStrIndex[r]);
            if (rank.empty())
                continue;

            std::wstring wrank;
            if (!Utf8toWStr(rank, wrank))
                continue;

            wstrToLower(wrank);

            if (wrank.substr(0, wrankStr.size()) == wrankStr)
            {
                int32 delta;
                if (!ExtractOptInt32(&args, delta, 0) || (delta < 0) || (delta > ReputationMgr::PointsInRank[r] - 1))
                {
                    PSendSysMessage(LANG_COMMAND_FACTION_DELTA, (ReputationMgr::PointsInRank[r] - 1));
                    SetSentErrorMessage(true);
                    return false;
                }
                amount += delta;
                break;
            }
            amount += ReputationMgr::PointsInRank[r];
        }
        if (r >= MAX_REPUTATION_RANK)
        {
            PSendSysMessage(LANG_COMMAND_FACTION_INVPARAM, rankTxt);
            SetSentErrorMessage(true);
            return false;
        }
    }

    FactionEntry const *factionEntry = sObjectMgr.GetFactionEntry(factionId);

    if (!factionEntry)
    {
        PSendSysMessage(LANG_COMMAND_FACTION_UNKNOWN, factionId);
        SetSentErrorMessage(true);
        return false;
    }

    if (factionEntry->reputationListID < 0)
    {
        PSendSysMessage(LANG_COMMAND_FACTION_NOREP_ERROR, factionEntry->name[GetSessionDbcLocale()], factionId);
        SetSentErrorMessage(true);
        return false;
    }

    target->GetReputationMgr().SetReputation(factionEntry, amount);
    PSendSysMessage(LANG_COMMAND_MODIFY_REP, factionEntry->name[GetSessionDbcLocale()], factionId,
                    GetNameLink(target).c_str(), target->GetReputationMgr().GetReputation(factionEntry));
    return true;
}

//-----------------------Npc Commands-----------------------
//add spawn of creature
bool ChatHandler::HandleNpcAddCommand(char* args)
{
    if (!*args)
        return false;

    uint32 id;
    if (!ExtractUint32KeyFromLink(&args, "Hcreature_entry", id))
        return false;

    CreatureInfo const *cinfo = ObjectMgr::GetCreatureTemplate(id);
    if (!cinfo)
    {
        PSendSysMessage(LANG_COMMAND_INVALIDCREATUREID, id);
        SetSentErrorMessage(true);
        return false;
    }

    Player *chr = m_session->GetPlayer();
    CreatureCreatePos pos(chr, chr->GetOrientation());
    Map *map = chr->GetMap();

    Creature* pCreature = new Creature;

    // used guids from specially reserved range (can be 0 if no free values)
    uint32 lowguid = sObjectMgr.GenerateStaticCreatureLowGuid();
    if (!lowguid)
    {
        SendSysMessage(LANG_NO_FREE_STATIC_GUID_FOR_SPAWN);
        SetSentErrorMessage(true);
        return false;
    }

    if (!pCreature->Create(lowguid, pos, cinfo))
    {
        delete pCreature;
        return false;
    }

    pCreature->SaveToDB(map->GetId());

    uint32 db_guid = pCreature->GetGUIDLow();

    // To call _LoadGoods(); _LoadQuests(); CreateTrainerSpells();
    pCreature->LoadFromDB(db_guid, map);

    map->Add(pCreature);
    sObjectMgr.AddCreatureToGrid(db_guid, sObjectMgr.GetCreatureData(db_guid));
    return true;
}

bool ChatHandler::HandleNpcSummonCommand(char* args)
{
    if (!*args)
        return false;

    uint32 id;
    if (!ExtractUint32KeyFromLink(&args, "Hcreature_entry", id))
        return false;

    CreatureInfo const *cinfo = ObjectMgr::GetCreatureTemplate(id);
    if (!cinfo)
    {
        PSendSysMessage(LANG_COMMAND_INVALIDCREATUREID, id);
        SetSentErrorMessage(true);
        return false;
    }

    Player *chr = m_session->GetPlayer();
    chr->SummonCreature(id, chr->GetPositionX(), chr->GetPositionY(), chr->GetPositionZ(), chr->GetOrientation());
    return true;
}

//add item in vendorlist
bool ChatHandler::HandleNpcAddVendorItemCommand(char* args)
{
    uint32 itemId;
    if (!ExtractUint32KeyFromLink(&args, "Hitem", itemId))
    {
        SendSysMessage(LANG_COMMAND_NEEDITEMSEND);
        SetSentErrorMessage(true);
        return false;
    }

    uint32 maxcount;
    if (!ExtractOptUInt32(&args, maxcount, 0))
        return false;

    uint32 incrtime;
    if (!ExtractOptUInt32(&args, incrtime, 0))
        return false;

    uint32 itemflags;
    if (!ExtractOptUInt32(&args, itemflags, 0))
        return false;

    Creature* vendor = getSelectedCreature();

    uint32 vendor_entry = vendor ? vendor->GetEntry() : 0;

    if (!sObjectMgr.IsVendorItemValid(false, "npc_vendor", vendor_entry, itemId, maxcount, incrtime, m_session->GetPlayer()))
    {
        SetSentErrorMessage(true);
        return false;
    }

    sObjectMgr.AddVendorItem(vendor_entry, itemId, maxcount, incrtime, itemflags);

    ItemPrototype const* pProto = ObjectMgr::GetItemPrototype(itemId);

    PSendSysMessage(LANG_ITEM_ADDED_TO_LIST, itemId, pProto->Name1, maxcount, incrtime);
    return true;
}

//del item from vendor list
bool ChatHandler::HandleNpcDelVendorItemCommand(char* args)
{
    if (!*args)
        return false;

    Creature* vendor = getSelectedCreature();
    if (!vendor || !vendor->isVendor())
    {
        SendSysMessage(LANG_COMMAND_VENDORSELECTION);
        SetSentErrorMessage(true);
        return false;
    }

    uint32 itemId;
    if (!ExtractUint32KeyFromLink(&args, "Hitem", itemId))
    {
        SendSysMessage(LANG_COMMAND_NEEDITEMSEND);
        SetSentErrorMessage(true);
        return false;
    }

    if (!sObjectMgr.RemoveVendorItem(vendor->GetEntry(), itemId))
    {
        PSendSysMessage(LANG_ITEM_NOT_IN_LIST, itemId);
        SetSentErrorMessage(true);
        return false;
    }

    ItemPrototype const* pProto = ObjectMgr::GetItemPrototype(itemId);

    PSendSysMessage(LANG_ITEM_DELETED_FROM_LIST, itemId, pProto->Name1);
    return true;
}

//show info about AI
bool ChatHandler::HandleNpcAIInfoCommand(char* /*args*/)
{
    Creature* pTarget = getSelectedCreature();

    if (!pTarget)
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage(LANG_NPC_AI_HEADER, pTarget->GetEntry());

    std::string strScript = pTarget->GetScriptName();
    std::string strAI = pTarget->GetAIName();
    char const* cstrAIClass = pTarget->AI() ? typeid(*pTarget->AI()).name() : " - ";

    PSendSysMessage(LANG_NPC_AI_NAMES,
                    strAI.empty() ? " - " : strAI.c_str(),
                    cstrAIClass ? cstrAIClass : " - ",
                    strScript.empty() ? " - " : strScript.c_str());

    if (pTarget->AI())
        pTarget->AI()->GetAIInformation(*this);

    return true;
}

//change level of creature or pet
bool ChatHandler::HandleNpcChangeLevelCommand(char* args)
{
    if (!*args)
        return false;

    uint8 lvl = (uint8) atoi(args);
    if (lvl < 1 || lvl > sWorld.getConfig(CONFIG_UINT32_MAX_PLAYER_LEVEL) + 3)
    {
        SendSysMessage(LANG_BAD_VALUE);
        SetSentErrorMessage(true);
        return false;
    }

    Creature* pCreature = getSelectedCreature();
    if (!pCreature)
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    if (pCreature->IsPet())
        ((Pet*)pCreature)->GivePetLevel(lvl);
    else
    {
        pCreature->SetMaxHealth(100 + 30 * lvl);
        pCreature->SetHealth(100 + 30 * lvl);
        pCreature->SetLevel(lvl);

        if (pCreature->HasStaticDBSpawnData())
            pCreature->SaveToDB();
    }

    return true;
}

//set npcflag of creature
bool ChatHandler::HandleNpcFlagCommand(char* args)
{
    if (!*args)
        return false;

    uint32 npcFlags = (uint32) atoi(args);

    Creature* pCreature = getSelectedCreature();

    if (!pCreature)
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    pCreature->SetUInt32Value(UNIT_NPC_FLAGS, npcFlags);

    WorldDatabase.PExecuteLog("UPDATE creature_template SET npcflag = '%u' WHERE entry = '%u'", npcFlags, pCreature->GetEntry());

    SendSysMessage(LANG_VALUE_SAVED_REJOIN);

    return true;
}

bool ChatHandler::HandleNpcDeleteCommand(char* args)
{
    Creature* unit = NULL;

    if (*args)
    {
        // number or [name] Shift-click form |color|Hcreature:creature_guid|h[name]|h|r
        uint32 lowguid;
        if (!ExtractUint32KeyFromLink(&args, "Hcreature", lowguid))
            return false;

        if (!lowguid)
            return false;

        if (CreatureData const* data = sObjectMgr.GetCreatureData(lowguid))
            unit = m_session->GetPlayer()->GetMap()->GetCreature(data->GetObjectGuid(lowguid));
    }
    else
        unit = getSelectedCreature();

    if (!unit)
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    switch (unit->GetSubtype())
    {
        case CREATURE_SUBTYPE_GENERIC:
        {
            unit->CombatStop();
            if (CreatureData const* data = sObjectMgr.GetCreatureData(unit->GetGUIDLow()))
            {
                Creature::AddToRemoveListInMaps(unit->GetGUIDLow(), data);
                Creature::DeleteFromDB(unit->GetGUIDLow(), data);
            }
            else
                unit->AddObjectToRemoveList();
            break;
        }
        case CREATURE_SUBTYPE_PET:
            ((Pet*)unit)->Unsummon(PET_SAVE_AS_CURRENT);
            break;
        case CREATURE_SUBTYPE_TOTEM:
            ((Totem*)unit)->UnSummon();
            break;
        case CREATURE_SUBTYPE_TEMPORARY_SUMMON:
            ((TemporarySummon*)unit)->UnSummon();
            break;
        default:
            return false;
    }

    SendSysMessage(LANG_COMMAND_DELCREATMESSAGE);

    return true;
}

//move selected creature
bool ChatHandler::HandleNpcMoveCommand(char* args)
{
    uint32 lowguid = 0;

    Creature* pCreature = getSelectedCreature();

    if (!pCreature)
    {
        // number or [name] Shift-click form |color|Hcreature:creature_guid|h[name]|h|r
        if (!ExtractUint32KeyFromLink(&args, "Hcreature", lowguid))
            return false;

        CreatureData const* data = sObjectMgr.GetCreatureData(lowguid);
        if (!data)
        {
            PSendSysMessage(LANG_COMMAND_CREATGUIDNOTFOUND, lowguid);
            SetSentErrorMessage(true);
            return false;
        }

        Player* player = m_session->GetPlayer();

        if (player->GetMapId() != data->mapid)
        {
            PSendSysMessage(LANG_COMMAND_CREATUREATSAMEMAP, lowguid);
            SetSentErrorMessage(true);
            return false;
        }

        pCreature = player->GetMap()->GetCreature(data->GetObjectGuid(lowguid));
    }
    else
        lowguid = pCreature->GetGUIDLow();

    float x = m_session->GetPlayer()->GetPositionX();
    float y = m_session->GetPlayer()->GetPositionY();
    float z = m_session->GetPlayer()->GetPositionZ();
    float o = m_session->GetPlayer()->GetOrientation();

    if (pCreature)
    {
        if (CreatureData const* data = sObjectMgr.GetCreatureData(pCreature->GetGUIDLow()))
        {
            const_cast<CreatureData*>(data)->posX = x;
            const_cast<CreatureData*>(data)->posY = y;
            const_cast<CreatureData*>(data)->posZ = z;
            const_cast<CreatureData*>(data)->orientation = o;
        }
        pCreature->GetMap()->CreatureRelocation(pCreature, x, y, z, o);
        pCreature->GetMotionMaster()->Initialize();
        if (pCreature->isAlive())                           // dead creature will reset movement generator at respawn
        {
            pCreature->SetDeathState(JUST_DIED);
            pCreature->Respawn();
        }
    }

    WorldDatabase.PExecuteLog("UPDATE creature SET position_x = '%f', position_y = '%f', position_z = '%f', orientation = '%f' WHERE guid = '%u'", x, y, z, o, lowguid);
    PSendSysMessage(LANG_COMMAND_CREATUREMOVED);
    return true;
}

/**HandleNpcSetMoveTypeCommand
 * Set the movement type for an NPC.<br/>
 * <br/>
 * Valid movement types are:
 * <ul>
 * <li> stay - NPC wont move </li>
 * <li> random - NPC will move randomly according to the spawndist </li>
 * <li> way - NPC will move with given waypoints set </li>
 * </ul>
 * additional parameter: NODEL - so no waypoints are deleted, if you
 *                       change the movement type
 */
bool ChatHandler::HandleNpcSetMoveTypeCommand(char* args)
{
    // 3 arguments:
    // GUID (optional - you can also select the creature)
    // stay|random|way (determines the kind of movement)
    // NODEL (optional - tells the system NOT to delete any waypoints)
    //        this is very handy if you want to do waypoints, that are
    //        later switched on/off according to special events (like escort
    //        quests, etc)

    uint32 lowguid;
    Creature* pCreature;
    if (!ExtractUInt32(&args, lowguid))                     // case .setmovetype $move_type (with selected creature)
    {
        pCreature = getSelectedCreature();
        if (!pCreature || !pCreature->HasStaticDBSpawnData())
            return false;
        lowguid = pCreature->GetGUIDLow();
    }
    else                                                    // case .setmovetype #creature_guid $move_type (with guid)
    {
        CreatureData const* data = sObjectMgr.GetCreatureData(lowguid);
        if (!data)
        {
            PSendSysMessage(LANG_COMMAND_CREATGUIDNOTFOUND, lowguid);
            SetSentErrorMessage(true);
            return false;
        }

        Player* player = m_session->GetPlayer();

        if (player->GetMapId() != data->mapid)
        {
            PSendSysMessage(LANG_COMMAND_CREATUREATSAMEMAP, lowguid);
            SetSentErrorMessage(true);
            return false;
        }

        pCreature = player->GetMap()->GetCreature(data->GetObjectGuid(lowguid));
    }

    MovementGeneratorType move_type;
    char* type_str = ExtractLiteralArg(&args);
    if (!type_str)
        return false;

    if (strncmp(type_str, "stay", strlen(type_str)) == 0)
        move_type = IDLE_MOTION_TYPE;
    else if (strncmp(type_str, "random", strlen(type_str)) == 0)
        move_type = RANDOM_MOTION_TYPE;
    else if (strncmp(type_str, "way", strlen(type_str)) == 0)
        move_type = WAYPOINT_MOTION_TYPE;
    else
        return false;

    bool doNotDelete = ExtractLiteralArg(&args, "NODEL") != NULL;
    if (!doNotDelete && *args)                              // need fail if false in result wrong literal
        return false;

    // now lowguid is low guid really existing creature
    // and pCreature point (maybe) to this creature or NULL

    // update movement type
    if (!doNotDelete)
        sWaypointMgr.DeletePath(lowguid);

    if (pCreature)
    {
        pCreature->SetDefaultMovementType(move_type);
        pCreature->GetMotionMaster()->Initialize();
        if (pCreature->isAlive())                           // dead creature will reset movement generator at respawn
        {
            pCreature->SetDeathState(JUST_DIED);
            pCreature->Respawn();
        }
        pCreature->SaveToDB();
    }

    if (doNotDelete)
        PSendSysMessage(LANG_MOVE_TYPE_SET_NODEL, type_str);
    else
        PSendSysMessage(LANG_MOVE_TYPE_SET, type_str);

    return true;
}

//set model of creature
bool ChatHandler::HandleNpcSetModelCommand(char* args)
{
    if (!*args)
        return false;

    uint32 displayId = (uint32) atoi(args);

    Creature *pCreature = getSelectedCreature();

    if (!pCreature || pCreature->IsPet())
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    pCreature->SetDisplayId(displayId);
    pCreature->SetNativeDisplayId(displayId);

    if (pCreature->HasStaticDBSpawnData())
        pCreature->SaveToDB();

    return true;
}
//set faction of creature
bool ChatHandler::HandleNpcFactionIdCommand(char* args)
{
    if (!*args)
        return false;

    uint32 factionId = (uint32) atoi(args);

    if (!sObjectMgr.GetFactionTemplateEntry(factionId))
    {
        PSendSysMessage(LANG_WRONG_FACTION, factionId);
        SetSentErrorMessage(true);
        return false;
    }

    Creature* pCreature = getSelectedCreature();

    if (!pCreature)
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    pCreature->setFaction(factionId);

    // faction is set in creature_template - not inside creature

    // update in memory
    if (CreatureInfo const *cinfo = pCreature->GetCreatureInfo())
    {
        const_cast<CreatureInfo*>(cinfo)->faction_A = factionId;
        const_cast<CreatureInfo*>(cinfo)->faction_H = factionId;
    }

    // and DB
    WorldDatabase.PExecuteLog("UPDATE creature_template SET faction_A = '%u', faction_H = '%u' WHERE entry = '%u'", factionId, factionId, pCreature->GetEntry());

    return true;
}
//set spawn dist of creature
bool ChatHandler::HandleNpcSpawnDistCommand(char* args)
{
    if (!*args)
        return false;

    float option = (float)atof(args);
    if (option < 0.0f)
    {
        SendSysMessage(LANG_BAD_VALUE);
        return false;
    }

    MovementGeneratorType mtype = IDLE_MOTION_TYPE;
    if (option > 0.0f)
        mtype = RANDOM_MOTION_TYPE;

    Creature *pCreature = getSelectedCreature();
    uint32 u_guidlow = 0;

    if (pCreature)
        u_guidlow = pCreature->GetGUIDLow();
    else
        return false;

    pCreature->SetRespawnRadius((float)option);
    pCreature->SetDefaultMovementType(mtype);
    pCreature->GetMotionMaster()->Initialize();
    if (pCreature->isAlive())                               // dead creature will reset movement generator at respawn
    {
        pCreature->SetDeathState(JUST_DIED);
        pCreature->Respawn();
    }

    WorldDatabase.PExecuteLog("UPDATE creature SET spawndist=%f, MovementType=%i WHERE guid=%u", option, mtype, u_guidlow);
    PSendSysMessage(LANG_COMMAND_SPAWNDIST, option);
    return true;
}
//spawn time handling
bool ChatHandler::HandleNpcSpawnTimeCommand(char* args)
{
    uint32 stime;
    if (!ExtractUInt32(&args, stime))
        return false;

    Creature *pCreature = getSelectedCreature();
    if (!pCreature)
    {
        PSendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    uint32 u_guidlow = pCreature->GetGUIDLow();

    WorldDatabase.PExecuteLog("UPDATE creature SET spawntimesecsmin=%i WHERE guid=%u", stime, u_guidlow);
    pCreature->SetRespawnDelay(stime);
    PSendSysMessage(LANG_COMMAND_SPAWNTIME, stime);

    return true;
}
//npc follow handling
bool ChatHandler::HandleNpcFollowCommand(char* /*args*/)
{
    Player *player = m_session->GetPlayer();
    Creature *creature = getSelectedCreature();

    if (!creature)
    {
        PSendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    // Follow player - Using pet's default dist and angle
    creature->GetMotionMaster()->MoveFollow(player, PET_FOLLOW_DIST, PET_FOLLOW_ANGLE);

    PSendSysMessage(LANG_CREATURE_FOLLOW_YOU_NOW, creature->GetName());
    return true;
}
//npc unfollow handling
bool ChatHandler::HandleNpcUnFollowCommand(char* /*args*/)
{
    Player *player = m_session->GetPlayer();
    Creature *creature = getSelectedCreature();

    if (!creature)
    {
        PSendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    if (creature->GetMotionMaster()->empty() ||
            creature->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
    {
        PSendSysMessage(LANG_CREATURE_NOT_FOLLOW_YOU);
        SetSentErrorMessage(true);
        return false;
    }

    FollowMovementGenerator<Creature> const* mgen
        = static_cast<FollowMovementGenerator<Creature> const*>((creature->GetMotionMaster()->top()));

    if (mgen->GetTarget() != player)
    {
        PSendSysMessage(LANG_CREATURE_NOT_FOLLOW_YOU);
        SetSentErrorMessage(true);
        return false;
    }

    // reset movement
    creature->GetMotionMaster()->MovementExpired(true);

    PSendSysMessage(LANG_CREATURE_NOT_FOLLOW_YOU_NOW, creature->GetName());
    return true;
}
//npc tame handling
bool ChatHandler::HandleNpcTameCommand(char* /*args*/)
{
    Creature *creatureTarget = getSelectedCreature();

    if (!creatureTarget || creatureTarget->IsPet())
    {
        PSendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    Player *player = m_session->GetPlayer();

    if (player->GetPetGuid())
    {
        SendSysMessage(LANG_YOU_ALREADY_HAVE_PET);
        SetSentErrorMessage(true);
        return false;
    }

    player->CastSpell(creatureTarget, 13481, true);         // Tame Beast, triggered effect
    return true;
}

//npc deathstate handling
bool ChatHandler::HandleNpcSetDeathStateCommand(char* args)
{
    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }

    Creature* pCreature = getSelectedCreature();
    if (!pCreature || !pCreature->HasStaticDBSpawnData())
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        SetSentErrorMessage(true);
        return false;
    }

    if (value)
        pCreature->SetDeadByDefault(true);
    else
        pCreature->SetDeadByDefault(false);

    pCreature->SaveToDB();

    pCreature->Respawn();

    return true;
}

//TODO: NpcCommands that need to be fixed :

bool ChatHandler::HandleNpcNameCommand(char* /*args*/)
{
    /* Temp. disabled
    if (!*args)
        return false;

    if (strlen((char*)args)>75)
    {
        PSendSysMessage(LANG_TOO_LONG_NAME, strlen((char*)args)-75);
        return true;
    }

    for (uint8 i = 0; i < strlen(args); ++i)
    {
        if (!isalpha(args[i]) && args[i]!=' ')
        {
            SendSysMessage(LANG_CHARS_ONLY);
            return false;
        }
    }

    ObjectGuid guid = m_session->GetPlayer()->GetSelectionGuid();
    if (guid.IsEmpty())
    {
        SendSysMessage(LANG_NO_SELECTION);
        return true;
    }

    Creature* pCreature = ObjectAccessor::GetCreature(*m_session->GetPlayer(), guid);

    if (!pCreature)
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        return true;
    }

    pCreature->SetName(args);
    uint32 idname = objmgr.AddCreatureTemplate(pCreature->GetName());
    pCreature->SetUInt32Value(OBJECT_FIELD_ENTRY, idname);

    pCreature->SaveToDB();
    */

    return true;
}

bool ChatHandler::HandleNpcSubNameCommand(char* /*args*/)
{
    /* Temp. disabled

    if (!*args)
        args = "";

    if (strlen((char*)args)>75)
    {

        PSendSysMessage(LANG_TOO_LONG_SUBNAME, strlen((char*)args)-75);
        return true;
    }

    for (uint8 i = 0; i < strlen(args); i++)
    {
        if (!isalpha(args[i]) && args[i]!=' ')
        {
            SendSysMessage(LANG_CHARS_ONLY);
            return false;
        }
    }

    ObjectGuid guid = m_session->GetPlayer()->GetSelectionGuid();
    if (guid.IsEmpty())
    {
        SendSysMessage(LANG_NO_SELECTION);
        return true;
    }

    Creature* pCreature = ObjectAccessor::GetCreature(*m_session->GetPlayer(), guid);

    if (!pCreature)
    {
        SendSysMessage(LANG_SELECT_CREATURE);
        return true;
    }

    uint32 idname = objmgr.AddCreatureSubName(pCreature->GetName(),args,pCreature->GetUInt32Value(UNIT_FIELD_DISPLAYID));
    pCreature->SetUInt32Value(OBJECT_FIELD_ENTRY, idname);

    pCreature->SaveToDB();
    */
    return true;
}

//move item to other slot
bool ChatHandler::HandleItemMoveCommand(char* args)
{
    if (!*args)
        return false;
    uint8 srcslot, dstslot;

    char* pParam1 = strtok(args, " ");
    if (!pParam1)
        return false;

    char* pParam2 = strtok(NULL, " ");
    if (!pParam2)
        return false;

    srcslot = (uint8)atoi(pParam1);
    dstslot = (uint8)atoi(pParam2);

    if (srcslot == dstslot)
        return true;

    if (!m_session->GetPlayer()->IsValidPos(INVENTORY_SLOT_BAG_0, srcslot, true))
        return false;

    // can be autostore pos
    if (!m_session->GetPlayer()->IsValidPos(INVENTORY_SLOT_BAG_0, dstslot, false))
        return false;

    uint16 src = ((INVENTORY_SLOT_BAG_0 << 8) | srcslot);
    uint16 dst = ((INVENTORY_SLOT_BAG_0 << 8) | dstslot);

    m_session->GetPlayer()->SwapItem(src, dst);

    return true;
}

//demorph player or unit
bool ChatHandler::HandleDeMorphCommand(char* /*args*/)
{
    Unit *target = getSelectedUnit();
    if (!target)
        target = m_session->GetPlayer();


    // check online security
    else if (target->GetTypeId() == TYPEID_PLAYER && HasLowerSecurity((Player*)target))
        return false;

    target->DeMorph();

    return true;
}

//morph creature or player
bool ChatHandler::HandleModifyMorphCommand(char* args)
{
    if (!*args)
        return false;

    uint16 display_id = (uint16)atoi(args);

    Unit *target = getSelectedUnit();
    if (!target)
        target = m_session->GetPlayer();

    // check online security
    else if (target->GetTypeId() == TYPEID_PLAYER && HasLowerSecurity((Player*)target))
        return false;

    target->SetDisplayId(display_id);

    return true;
}

//kick player
bool ChatHandler::HandleKickPlayerCommand(char *args)
{
    Player* target;
    if (!ExtractPlayerTarget(&args, &target))
        return false;

    if (m_session && target == m_session->GetPlayer())
    {
        SendSysMessage(LANG_COMMAND_KICKSELF);
        SetSentErrorMessage(true);
        return false;
    }

    // check online security
    if (HasLowerSecurity(target))
        return false;

    // send before target pointer invalidate
    PSendSysMessage(LANG_COMMAND_KICKMESSAGE, GetNameLink(target).c_str());
    // First kick: close socket but keep player online
    if (target->GetSession()->IsConnected())
        target->GetSession()->KickPlayer();
    else
        target->GetSession()->KickDisconnectedFromWorld();
    return true;
}

bool ChatHandler::HandleGroupInfoCommand(char* args)
{
    Player* target;
    ObjectGuid target_guid;
    std::string target_name;
    if (!ExtractPlayerTarget(&args, &target, &target_guid, &target_name))
        return false;

    if (!target)
    {
        PSendSysMessage(LANG_NO_CHAR_SELECTED);
        return false;
    }

    auto group = target->GetGroup();

    if (!group)
    {
        std::string nameLink = GetNameLink(target);
        PSendSysMessage(LANG_NOT_IN_GROUP, nameLink.c_str());
        return false;
    }

    std::vector<std::string> names;

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
    {
        if (Player* player = itr->getSource())
        {
            names.emplace_back(player->GetName());
        }
    }

    std::stringstream stream;

    for (std::size_t i = 0, j = names.size(); i != j; ++i)
    {
        stream << names[i];
        
        if (i + 1 != j)
        {
            stream << ", ";
        }
    }
    
    PSendSysMessage(LANG_GROUP_INFO, (group->isRaidGroup() ? "Raid" : "Party"),
                    playerLink(std::to_string(group->GetId())).c_str(), playerLink(group->GetLeaderName()).c_str(),
                    group->GetMembersCount(), stream.str().c_str());
    return true;
}

//show info of player
bool ChatHandler::HandlePInfoCommand(char* args)
{
    Player* target;
    ObjectGuid target_guid;
    std::string target_name;
    if (!ExtractPlayerTarget(&args, &target, &target_guid, &target_name, true))
        return false;

    if (HasLowerSecurity(target, target ? ObjectGuid() : target_guid))
        return false;

    PInfoHandler::HandlePInfoCommand(m_session, target, target_guid, target_name);

    return true;
}
/// Helper function
inline Creature* Helper_CreateWaypointFor(Creature* wpOwner, WaypointPathOrigin wpOrigin, int32 pathId, uint32 wpId, WaypointNode const* wpNode, CreatureInfo const* waypointInfo)
{
    TemporarySummonWaypoint* wpCreature = new TemporarySummonWaypoint(wpOwner->GetObjectGuid(), wpId, pathId, (uint32)wpOrigin);

    CreatureCreatePos pos(wpOwner->GetMap(), wpNode->x, wpNode->y, wpNode->z, wpNode->orientation != 100.0f ? wpNode->orientation : 0.0f);

    if (!wpCreature->Create(wpOwner->GetMap()->GenerateLocalLowGuid(HIGHGUID_UNIT), pos, waypointInfo))
    {
        delete wpCreature;
        return NULL;
    }

    wpCreature->SetVisibility(VISIBILITY_OFF);
    wpCreature->SetSummonPoint(pos);

    wpCreature->SetActiveObjectState(true);

    wpCreature->Summon(TEMPSUMMON_TIMED_DESPAWN, 5 * MINUTE * IN_MILLISECONDS); // Also initializes the AI and MMGen
    return wpCreature;
}
inline void UnsummonVisualWaypoints(Player const* player, ObjectGuid ownerGuid)
{
    std::list<Creature*> waypoints;
    MaNGOS::AllCreaturesOfEntryInRange checkerForWaypoint(player, VISUAL_WAYPOINT, SIZE_OF_GRIDS);
    MaNGOS::CreatureListSearcher<MaNGOS::AllCreaturesOfEntryInRange> searcher(waypoints, checkerForWaypoint);
    Cell::VisitGridObjects(player, searcher, SIZE_OF_GRIDS);

    for (std::list<Creature*>::iterator itr = waypoints.begin(); itr != waypoints.end(); ++itr)
    {
        if ((*itr)->GetSubtype() != CREATURE_SUBTYPE_TEMPORARY_SUMMON)
            continue;

        TemporarySummonWaypoint* wpTarget = dynamic_cast<TemporarySummonWaypoint*>(*itr);
        if (!wpTarget)
            continue;

        if (wpTarget->GetSummonerGuid() == ownerGuid)
            wpTarget->UnSummon();
    }
}

/** Add a waypoint to a creature
 * .wp add [dbGuid] [pathId] [source]
 *
 * The user can either select an npc or provide its dbGuid.
 * Also the user can specify pathId and source if wanted.
 *
 * The user can even select a visual waypoint - then the new waypoint
 * is placed *after* the selected one - this makes insertion of new
 * waypoints possible.
 *
 * .wp add [pathId] [source]
 * -> adds a waypoint to the currently selected creature, to path pathId in source-storage
 *
 * .wp add guid [pathId] [source]
 * -> if no npc is selected, expect the creature provided with guid argument
 *
 * @return true - command did succeed, false - something went wrong
 */
bool ChatHandler::HandleWpAddCommand(char* args)
{
    DEBUG_LOG("DEBUG: HandleWpAddCommand");

    CreatureInfo const* waypointInfo = ObjectMgr::GetCreatureTemplate(VISUAL_WAYPOINT);
    if (!waypointInfo || waypointInfo->GetHighGuid() != HIGHGUID_UNIT)
        return false;                                       // must exist as normal creature in mangos.sql 'creature_template'

    Creature* targetCreature = getSelectedCreature();
    WaypointPathOrigin wpDestination = PATH_NO_PATH;        ///< into which storage
    int32 wpPathId = 0;                                     ///< along which path
    uint32 wpPointId = 0;                                   ///< pointId if a waypoint was selected, in this case insert after
    Creature* wpOwner;

    if (targetCreature)
    {
        // Check if the user did specify a visual waypoint
        if (targetCreature->GetEntry() == VISUAL_WAYPOINT && targetCreature->GetSubtype() == CREATURE_SUBTYPE_TEMPORARY_SUMMON)
        {
            TemporarySummonWaypoint* wpTarget = dynamic_cast<TemporarySummonWaypoint*>(targetCreature);
            if (!wpTarget)
            {
                PSendSysMessage(LANG_WAYPOINT_VP_SELECT);
                SetSentErrorMessage(true);
                return false;
            }

            // Who moves along this waypoint?
            wpOwner = targetCreature->GetMap()->GetAnyTypeCreature(wpTarget->GetSummonerGuid());
            if (!wpOwner)
            {
                PSendSysMessage(LANG_WAYPOINT_NOTFOUND_NPC, wpTarget->GetSummonerGuid().GetString().c_str());
                SetSentErrorMessage(true);
                return false;
            }
            wpDestination = (WaypointPathOrigin)wpTarget->GetPathOrigin();
            wpPathId = wpTarget->GetPathId();
            wpPointId = wpTarget->GetWaypointId() + 1;      // Insert as next waypoint
        }
        else // normal creature selected
            wpOwner = targetCreature;
    }
    else //!targetCreature - first argument must be dbGuid
    {
        uint32 dbGuid;
        if (!ExtractUInt32(&args, dbGuid))
        {
            PSendSysMessage(LANG_WAYPOINT_NOGUID);
            SetSentErrorMessage(true);
            return false;
        }

        CreatureData const* data = sObjectMgr.GetCreatureData(dbGuid);
        if (!data)
        {
            PSendSysMessage(LANG_WAYPOINT_CREATNOTFOUND, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }

        if (m_session->GetPlayer()->GetMapId() != data->mapid)
        {
            PSendSysMessage(LANG_COMMAND_CREATUREATSAMEMAP, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }

        wpOwner = m_session->GetPlayer()->GetMap()->GetAnyTypeCreature(data->GetObjectGuid(dbGuid));
        if (!wpOwner)
        {
            PSendSysMessage(LANG_WAYPOINT_CREATNOTFOUND, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }
    }

    if (wpDestination == PATH_NO_PATH)                      // No Waypoint selected, parse additional params
    {
        if (ExtractOptInt32(&args, wpPathId, 0))            // Fill path-id and source
        {
            uint32 src = (uint32)PATH_NO_PATH;
            if (ExtractOptUInt32(&args, src, src))
                wpDestination = (WaypointPathOrigin)src;
            else // pathId provided but no destination
            {
                if (wpPathId != 0)
                    wpDestination = PATH_FROM_ENTRY;        // Multiple Paths must only be assigned by entry
            }
        }

        if (wpDestination == PATH_NO_PATH)                  // No overwrite params. Do best estimate
        {
            if (wpOwner->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
                if (WaypointMovementGenerator<Creature> const* wpMMGen = dynamic_cast<WaypointMovementGenerator<Creature> const*>(wpOwner->GetMotionMaster()->GetCurrent()))
                    wpMMGen->GetPathInformation(wpPathId, wpDestination);

            // Get information about default path if no current path. If no default path, prepare data dependendy on uniqueness
            if (wpDestination == PATH_NO_PATH && !sWaypointMgr.GetDefaultPath(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), &wpDestination))
            {
                wpDestination = PATH_FROM_ENTRY;                // Default place to store paths
                if (wpOwner->HasStaticDBSpawnData())
                {
                    QueryResult* result = WorldDatabase.PQuery("SELECT COUNT(id) FROM creature WHERE id = %u", wpOwner->GetEntry());
                    if (result && result->Fetch()[0].GetUInt32() != 1)
                        wpDestination = PATH_FROM_GUID;
                    delete result;
                }
            }
        }
    }

    // All arguments parsed
    // wpOwner will get a new waypoint inserted into wpPath = GetPathFromOrigin(wpOwner, wpDestination, wpPathId) at wpPointId

    float x, y, z;
    m_session->GetPlayer()->GetPosition(x, y, z);
    if (!sWaypointMgr.AddNode(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpPointId, wpDestination, x, y, z))
    {
        PSendSysMessage(LANG_WAYPOINT_NOTCREATED, wpPointId, wpOwner->GetGuidStr().c_str(), wpPathId, WaypointManager::GetOriginString(wpDestination).c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // Unsummon old visuals, summon new ones
    UnsummonVisualWaypoints(m_session->GetPlayer(), wpOwner->GetObjectGuid());
    WaypointPath const* wpPath = sWaypointMgr.GetPathFromOrigin(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpPathId, wpDestination);
    for (WaypointPath::const_iterator itr = wpPath->begin(); itr != wpPath->end(); ++itr)
    {
        if (!Helper_CreateWaypointFor(wpOwner, wpDestination, wpPathId, itr->first, &itr->second, waypointInfo))
        {
            PSendSysMessage(LANG_WAYPOINT_VP_NOTCREATED, VISUAL_WAYPOINT);
            SetSentErrorMessage(true);
            return false;
        }
    }

    PSendSysMessage(LANG_WAYPOINT_ADDED, wpPointId, wpOwner->GetGuidStr().c_str(), wpPathId, WaypointManager::GetOriginString(wpDestination).c_str());

    return true;
}                                                           // HandleWpAddCommand

/**
 * .wp modify waittime | scriptid | orientation | del | move [dbGuid, id] [value]
 *
 * waittime <Delay>
 *   User has selected a visual waypoint before.
 *   Delay <Delay> is added to this waypoint. Everytime the
 *   NPC comes to this waypoint, it will wait Delay millieseconds.
 *
 * waittime <DBGuid> <WPNUM> <Delay>
 *   User has not selected visual waypoint before.
 *   For the waypoint <WPNUM> for the NPC with <DBGuid>
 *   an delay Delay is added to this waypoint
 *   Everytime the NPC comes to this waypoint, it will wait Delay millieseconds.
 *
 * scriptid <scriptId>
 *   User has selected a visual waypoint before.
 *   <scriptId> is added to this waypoint. Everytime the
 *   NPC comes to this waypoint, the DBScript scriptId is executed.
 *
 * scriptid <DBGuid> <WPNUM> <scriptId>
 *   User has not selected visual waypoint before.
 *   For the waypoint <WPNUM> for the NPC with <DBGuid>
 *   an emote <scriptId> is added.
 *   Everytime the NPC comes to this waypoint, the DBScript scriptId is executed.
 *
 * orientation [DBGuid, WpNum] <Orientation>
 *   Set the orientation of the selected waypoint or waypoint given with DbGuid/ WpId
 *   to the value of <Orientation>.
 *
 * del [DBGuid, WpId]
 *   Remove the selected waypoint or waypoint given with DbGuid/ WpId.
 *
 * move [DBGuid, WpId]
 *   Move the selected waypoint or waypoint given with DbGuid/ WpId to player's current positiion.
 */
bool ChatHandler::HandleWpModifyCommand(char* args)
{
    DEBUG_LOG("DEBUG: HandleWpModifyCommand");

    if (!*args)
        { return false; }

    CreatureInfo const* waypointInfo = ObjectMgr::GetCreatureTemplate(VISUAL_WAYPOINT);
    if (!waypointInfo || waypointInfo->GetHighGuid() != HIGHGUID_UNIT)
        { return false; }                                       // must exist as normal creature in mangos.sql 'creature_template'

    // first arg: add del text emote spell waittime move
    char* subCmd_str = ExtractLiteralArg(&args);
    if (!subCmd_str)
    {
        return false;
    }

    std::string subCmd = subCmd_str;
    // Check
    // Remember: "show" must also be the name of a column!
    if ((subCmd != "waittime") && (subCmd != "scriptid") && (subCmd != "orientation") && (subCmd != "del") && (subCmd != "move"))
    {
        return false;
    }

    // Next arg is: <GUID> <WPNUM> <ARGUMENT>

    // Did user provide a GUID or did the user select a creature?
    Creature* targetCreature = getSelectedCreature();       // Expect a visual waypoint to be selected
    Creature* wpOwner;                               // Who moves along the waypoint
    uint32 wpId = 0;
    WaypointPathOrigin wpSource = PATH_NO_PATH;
    int32 wpPathId = 0;

    if (targetCreature)
    {
        DEBUG_LOG("DEBUG: HandleWpModifyCommand - User did select an NPC");

        // Check if the user did specify a visual waypoint
        if (targetCreature->GetEntry() != VISUAL_WAYPOINT || targetCreature->GetSubtype() != CREATURE_SUBTYPE_TEMPORARY_SUMMON)
        {
            PSendSysMessage(LANG_WAYPOINT_VP_SELECT);
            SetSentErrorMessage(true);
            return false;
        }
        TemporarySummonWaypoint* wpTarget = dynamic_cast<TemporarySummonWaypoint*>(targetCreature);
        if (!wpTarget)
        {
            PSendSysMessage(LANG_WAYPOINT_VP_SELECT);
            SetSentErrorMessage(true);
            return false;
        }

        // Who moves along this waypoint?
        wpOwner = targetCreature->GetMap()->GetAnyTypeCreature(wpTarget->GetSummonerGuid());
        if (!wpOwner)
        {
            PSendSysMessage(LANG_WAYPOINT_NOTFOUND_NPC, wpTarget->GetSummonerGuid().GetString().c_str());
            SetSentErrorMessage(true);
            return false;
        }
        wpId = wpTarget->GetWaypointId();

        wpPathId = wpTarget->GetPathId();
        wpSource = (WaypointPathOrigin)wpTarget->GetPathOrigin();
    }
    else
    {
        uint32 dbGuid = 0;
        // User did provide <GUID> <WPNUM>
        if (!ExtractUInt32(&args, dbGuid))
        {
            SendSysMessage(LANG_WAYPOINT_NOGUID);
            SetSentErrorMessage(true);
            return false;
        }

        if (!ExtractUInt32(&args, wpId))
        {
            SendSysMessage(LANG_WAYPOINT_NOWAYPOINTGIVEN);
            SetSentErrorMessage(true);
            return false;
        }

        CreatureData const* data = sObjectMgr.GetCreatureData(dbGuid);
        if (!data)
        {
            PSendSysMessage(LANG_WAYPOINT_CREATNOTFOUND, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }

        wpOwner = m_session->GetPlayer()->GetMap()->GetAnyTypeCreature(data->GetObjectGuid(dbGuid));
        if (!wpOwner)
        {
            PSendSysMessage(LANG_WAYPOINT_CREATNOTFOUND, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }
    }

    if (wpSource == PATH_NO_PATH)                           // No waypoint selected
    {
        if (wpOwner->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
            if (WaypointMovementGenerator<Creature> const* wpMMGen = dynamic_cast<WaypointMovementGenerator<Creature> const*>(wpOwner->GetMotionMaster()->GetCurrent()))
                wpMMGen->GetPathInformation(wpPathId, wpSource);

        if (wpSource == PATH_NO_PATH)
            sWaypointMgr.GetDefaultPath(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), &wpSource);
    }

    WaypointPath const* wpPath = sWaypointMgr.GetPathFromOrigin(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpPathId, wpSource);
    if (!wpPath)
    {
        PSendSysMessage(LANG_WAYPOINT_NOTFOUNDPATH, wpOwner->GetGuidStr().c_str(), wpPathId, WaypointManager::GetOriginString(wpSource).c_str());
        SetSentErrorMessage(true);
        return false;
    }

    WaypointPath::const_iterator point = wpPath->find(wpId);
    if (point == wpPath->end())
    {
        PSendSysMessage(LANG_WAYPOINT_NOTFOUND, wpId, wpOwner->GetGuidStr().c_str(), wpPathId, WaypointManager::GetOriginString(wpSource).c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // If no visual WP was selected, but we are not going to remove it
    if (!targetCreature && subCmd != "del")
    {
        targetCreature = Helper_CreateWaypointFor(wpOwner, wpSource, wpPathId, wpId, &(point->second), waypointInfo);
        if (!targetCreature)
        {
            PSendSysMessage(LANG_WAYPOINT_VP_NOTCREATED, VISUAL_WAYPOINT);
            SetSentErrorMessage(true);
            return false;
        }
    }

    if (subCmd == "del")                                    // Remove WP, no additional command required
    {
        sWaypointMgr.DeleteNode(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpId, wpPathId, wpSource);

        if (TemporarySummonWaypoint* wpCreature = dynamic_cast<TemporarySummonWaypoint*>(targetCreature))
            wpCreature->UnSummon();

        if (wpPath->empty())
        {
            wpOwner->SetDefaultMovementType(RANDOM_MOTION_TYPE);
            wpOwner->GetMotionMaster()->Initialize();
            if (wpOwner->isAlive())                         // Dead creature will reset movement generator at respawn
            {
                wpOwner->SetDeathState(JUST_DIED);
                wpOwner->Respawn();
            }
            wpOwner->SaveToDB();
        }

        PSendSysMessage(LANG_WAYPOINT_REMOVED);
        return true;
    }
    else if (subCmd == "move")                              // Move to player position, no additional command required
    {
        float x, y, z;
        m_session->GetPlayer()->GetPosition(x, y, z);

        // Move visual waypoint
        targetCreature->NearTeleportTo(x, y, z, targetCreature->GetOrientation());

        sWaypointMgr.SetNodePosition(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpId, wpPathId, wpSource, x, y, z);

        PSendSysMessage(LANG_WAYPOINT_CHANGED);
        return true;
    }
    else if (subCmd == "waittime")
    {
        uint32 waittime;
        if (!ExtractUInt32(&args, waittime))
            return false;

        sWaypointMgr.SetNodeWaittime(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpId, wpPathId, wpSource, waittime);
    }
    else if (subCmd == "scriptid")
    {
        uint32 scriptId;
        if (!ExtractUInt32(&args, scriptId))
            return false;

        if (!sWaypointMgr.SetNodeScriptId(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpId, wpPathId, wpSource, scriptId))
            PSendSysMessage(LANG_WAYPOINT_INFO_UNK_SCRIPTID, scriptId);
    }
    else if (subCmd == "orientation")
    {
        float ori;
        if (!ExtractFloat(&args, ori))
            return false;

        sWaypointMgr.SetNodeOrientation(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpId, wpPathId, wpSource, ori);
    }

    PSendSysMessage(LANG_WAYPOINT_CHANGED_NO, subCmd_str);
    return true;
}

/**
 * .wp show info | on | off | first | last [dbGuid] [pathId [wpOrigin] ]
 *
 * info -> User has selected a visual waypoint before
 *
 * on -> User has selected an NPC; all visual waypoints for this
 *       NPC are added to the world
 *
 * on <dbGuid> -> User did not select an NPC - instead the dbGuid of the
 *              NPC is provided. All visual waypoints for this NPC
 *              are added from the world.
 *
 * off -> User has selected an NPC; all visual waypoints for this
 *        NPC are removed from the world.
 */
bool ChatHandler::HandleWpShowCommand(char* args)
{
    DEBUG_LOG("DEBUG: HandleWpShowCommand");

    if (!*args)
        { return false; }

    CreatureInfo const* waypointInfo = ObjectMgr::GetCreatureTemplate(VISUAL_WAYPOINT);
    if (!waypointInfo || waypointInfo->GetHighGuid() != HIGHGUID_UNIT)
        { return false; }                                       // must exist as normal creature in mangos.sql 'creature_template'

    // first arg: info, on, off, first, last

    char* subCmd_str = ExtractLiteralArg(&args);
    if (!subCmd_str)
        return false;
    std::string subCmd = subCmd_str;                        ///< info, on, off, first, last

    uint32 dbGuid = 0;
    int32 wpPathId = 0;
    WaypointPathOrigin wpOrigin = PATH_NO_PATH;

    // User selected an npc?
    Creature* targetCreature = getSelectedCreature();
    if (targetCreature)
    {
        if (ExtractOptInt32(&args, wpPathId, 0))            // Fill path-id and source
        {
            uint32 src;
            if (ExtractOptUInt32(&args, src, (uint32)PATH_NO_PATH))
                wpOrigin = (WaypointPathOrigin)src;
        }
    }
    else    // Guid must be provided
    {
        if (!ExtractUInt32(&args, dbGuid))                  // No creature selected and no dbGuid provided
            return false;

        if (ExtractOptInt32(&args, wpPathId, 0))            // Fill path-id and source
        {
            uint32 src = (uint32)PATH_NO_PATH;
            if (ExtractOptUInt32(&args, src, src))
                wpOrigin = (WaypointPathOrigin)src;
        }

        // Params now parsed, check them
        CreatureData const* data = sObjectMgr.GetCreatureData(dbGuid);
        if (!data)
        {
            PSendSysMessage(LANG_WAYPOINT_CREATNOTFOUND, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }

        targetCreature = m_session->GetPlayer()->GetMap()->GetCreature(data->GetObjectGuid(dbGuid));
        if (!targetCreature)
        {
            PSendSysMessage(LANG_WAYPOINT_CREATNOTFOUND, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }
    }

    Creature* wpOwner;                                      // Npc that is moving
    TemporarySummonWaypoint* wpTarget = NULL;               // Define here for wp-info command

    // Show info for the selected waypoint (Step one: get moving npc)
    if (subCmd == "info")
    {
        // Check if the user did specify a visual waypoint
        if (targetCreature->GetEntry() != VISUAL_WAYPOINT || targetCreature->GetSubtype() != CREATURE_SUBTYPE_TEMPORARY_SUMMON)
        {
            PSendSysMessage(LANG_WAYPOINT_VP_SELECT);
            SetSentErrorMessage(true);
            return false;
        }
        wpTarget = dynamic_cast<TemporarySummonWaypoint*>(targetCreature);
        if (!wpTarget)
        {
            PSendSysMessage(LANG_WAYPOINT_VP_SELECT);
            SetSentErrorMessage(true);
            return false;
        }

        // Who moves along this waypoint?
        wpOwner = targetCreature->GetMap()->GetAnyTypeCreature(wpTarget->GetSummonerGuid());
        if (!wpOwner)
        {
            PSendSysMessage(LANG_WAYPOINT_NOTFOUND_NPC, wpTarget->GetSummonerGuid().GetString().c_str());
            SetSentErrorMessage(true);
            return false;
        }

        // Ignore params, use information of selected waypoint!
        wpOrigin = (WaypointPathOrigin)wpTarget->GetPathOrigin();
        wpPathId = wpTarget->GetPathId();
    }
    else
        wpOwner = targetCreature;

    // Get the path
    WaypointPath* wpPath = NULL;
    if (wpOrigin != PATH_NO_PATH)                           // Might have been provided by param
        wpPath = sWaypointMgr.GetPathFromOrigin(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpPathId, wpOrigin);
    else
    {
        if (wpOwner->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
            if (WaypointMovementGenerator<Creature> const* wpMMGen = dynamic_cast<WaypointMovementGenerator<Creature> const*>(wpOwner->GetMotionMaster()->GetCurrent()))
            {
                wpMMGen->GetPathInformation(wpPathId, wpOrigin);
                wpPath = sWaypointMgr.GetPathFromOrigin(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpPathId, wpOrigin);
            }

        if (wpOrigin == PATH_NO_PATH)
            wpPath = sWaypointMgr.GetDefaultPath(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), &wpOrigin);
    }

    if (!wpPath || wpPath->empty())
    {
        PSendSysMessage(LANG_WAYPOINT_NOTFOUNDPATH, wpOwner->GetGuidStr().c_str(), wpPathId, WaypointManager::GetOriginString(wpOrigin).c_str());
        SetSentErrorMessage(true);
        return false;
    }

    // Show info for the selected waypoint (Step two: Show actual info)
    if (subCmd == "info")
    {
        // Find the waypoint
        WaypointPath::const_iterator point = wpPath->find(wpTarget->GetWaypointId());
        if (point == wpPath->end())
        {
            PSendSysMessage(LANG_WAYPOINT_NOTFOUND, wpTarget->GetWaypointId(), wpOwner->GetGuidStr().c_str(), wpPathId, WaypointManager::GetOriginString(wpOrigin).c_str());
            SetSentErrorMessage(true);
            return false;
        }

        PSendSysMessage(LANG_WAYPOINT_INFO_TITLE, wpTarget->GetWaypointId(), wpOwner->GetGuidStr().c_str(), wpPathId, WaypointManager::GetOriginString(wpOrigin).c_str());
        PSendSysMessage(LANG_WAYPOINT_INFO_WAITTIME, point->second.delay);
        PSendSysMessage(LANG_WAYPOINT_INFO_ORI, point->second.orientation);
        PSendSysMessage(LANG_WAYPOINT_INFO_SCRIPTID, point->second.script_id);
        if (wpOrigin == PATH_FROM_SPECIAL)
            PSendSysMessage(LANG_WAYPOINT_INFO_AISCRIPT, wpOwner->GetScriptName().c_str());
        if (WaypointBehavior* behaviour = point->second.behavior)
        {
            PSendSysMessage(" ModelId1: %u", behaviour->model1);
            PSendSysMessage(" ModelId2: %u", behaviour->model2);
            PSendSysMessage(" Emote: %u", behaviour->emote);
            PSendSysMessage(" Spell: %u", behaviour->spell);
            for (int i = 0;  i < MAX_WAYPOINT_TEXT; ++i)
                PSendSysMessage(" TextId%i: %i \'%s\'", i + 1, behaviour->textid[i], (behaviour->textid[i] ? sObjectMgr.GetBroadcastText(behaviour->textid[i], m_session->GetSessionDbLocaleIndex()) : ""));
        }

        return true;
    }

    if (subCmd == "on")
    {
        UnsummonVisualWaypoints(m_session->GetPlayer(), wpOwner->GetObjectGuid());

        for (WaypointPath::const_iterator pItr = wpPath->begin(); pItr != wpPath->end(); ++pItr)
        {
            if (!Helper_CreateWaypointFor(wpOwner, wpOrigin, wpPathId, pItr->first, &(pItr->second), waypointInfo))
            {
                printf("error %s wpPathId %i", wpOwner->GetName(), wpPathId);
                PSendSysMessage(LANG_WAYPOINT_VP_NOTCREATED, VISUAL_WAYPOINT);
                SetSentErrorMessage(true);
                return false;
            }
        }

        return true;
    }

    if (subCmd == "first")
    {
        if (!Helper_CreateWaypointFor(wpOwner, wpOrigin, wpPathId, wpPath->begin()->first, &(wpPath->begin()->second), waypointInfo))
        {
            PSendSysMessage(LANG_WAYPOINT_VP_NOTCREATED, VISUAL_WAYPOINT);
            SetSentErrorMessage(true);
            return false;
        }

        // player->PlayerTalkClass->SendPointOfInterest(x, y, 6, 6, 0, "First Waypoint");
        return true;
    }

    if (subCmd == "last")
    {
        if (!Helper_CreateWaypointFor(wpOwner, wpOrigin, wpPathId, wpPath->rbegin()->first, &(wpPath->rbegin()->second), waypointInfo))
        {
            PSendSysMessage(LANG_WAYPOINT_VP_NOTCREATED, VISUAL_WAYPOINT);
            SetSentErrorMessage(true);
            return false;
        }

        // player->PlayerTalkClass->SendPointOfInterest(x, y, 6, 6, 0, "Last Waypoint");
        return true;
    }

    if (subCmd == "off")
    {
        UnsummonVisualWaypoints(m_session->GetPlayer(), wpOwner->GetObjectGuid());
        PSendSysMessage(LANG_WAYPOINT_VP_ALLREMOVED);
        return true;
    }

    return false;
}                                                           // HandleWpShowCommand

                                                            /// [Guid if no selected unit] <filename> [pathId [wpOrigin] ]
bool ChatHandler::HandleWpExportCommand(char* args)
{
    if (!*args)
        return false;

    Creature* wpOwner;
    WaypointPathOrigin wpOrigin = PATH_NO_PATH;
    int32 wpPathId = 0;

    if (Creature* targetCreature = getSelectedCreature())
    {
        // Check if the user did specify a visual waypoint
        if (targetCreature->GetEntry() == VISUAL_WAYPOINT && targetCreature->GetSubtype() == CREATURE_SUBTYPE_TEMPORARY_SUMMON)
        {
            TemporarySummonWaypoint* wpTarget = dynamic_cast<TemporarySummonWaypoint*>(targetCreature);
            if (!wpTarget)
            {
                PSendSysMessage(LANG_WAYPOINT_VP_SELECT);
                SetSentErrorMessage(true);
                return false;
            }

            // Who moves along this waypoint?
            wpOwner = targetCreature->GetMap()->GetAnyTypeCreature(wpTarget->GetSummonerGuid());
            if (!wpOwner)
            {
                PSendSysMessage(LANG_WAYPOINT_NOTFOUND_NPC, wpTarget->GetSummonerGuid().GetString().c_str());
                SetSentErrorMessage(true);
                return false;
            }
            wpOrigin = (WaypointPathOrigin)wpTarget->GetPathOrigin();
            wpPathId = wpTarget->GetPathId();
        }
        else // normal creature selected
            wpOwner = targetCreature;
    }
    else
    {
        uint32 dbGuid;
        if (!ExtractUInt32(&args, dbGuid))
        {
            PSendSysMessage(LANG_WAYPOINT_NOGUID);
            SetSentErrorMessage(true);
            return false;
        }

        CreatureData const* data = sObjectMgr.GetCreatureData(dbGuid);
        if (!data)
        {
            PSendSysMessage(LANG_WAYPOINT_CREATNOTFOUND, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }

        if (m_session->GetPlayer()->GetMapId() != data->mapid)
        {
            PSendSysMessage(LANG_COMMAND_CREATUREATSAMEMAP, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }

        wpOwner = m_session->GetPlayer()->GetMap()->GetAnyTypeCreature(data->GetObjectGuid(dbGuid));
        if (!wpOwner)
        {
            PSendSysMessage(LANG_WAYPOINT_CREATNOTFOUND, dbGuid);
            SetSentErrorMessage(true);
            return false;
        }
    }

    // wpOwner is now known, in case of export by visual waypoint also the to be exported path
    char* export_str = ExtractLiteralArg(&args);
    if (!export_str)
    {
        PSendSysMessage(LANG_WAYPOINT_ARGUMENTREQ, "export");
        SetSentErrorMessage(true);
        return false;
    }

    if (wpOrigin == PATH_NO_PATH)                           // No WP selected, Extract optional arguments
    {
        if (ExtractOptInt32(&args, wpPathId, 0))            // Fill path-id and source
        {
            uint32 src = (uint32)PATH_NO_PATH;
            if (ExtractOptUInt32(&args, src, src))
                wpOrigin = (WaypointPathOrigin)src;
            else // pathId provided but no destination
            {
                if (wpPathId != 0)
                    wpOrigin = PATH_FROM_ENTRY;             // Multiple Paths must only be assigned by entry
            }
        }

        if (wpOrigin == PATH_NO_PATH)
        {
            if (wpOwner->GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE)
                if (WaypointMovementGenerator<Creature> const* wpMMGen = dynamic_cast<WaypointMovementGenerator<Creature> const*>(wpOwner->GetMotionMaster()->GetCurrent()))
                    wpMMGen->GetPathInformation(wpPathId, wpOrigin);
            if (wpOrigin == PATH_NO_PATH)
                sWaypointMgr.GetDefaultPath(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), &wpOrigin);
        }
    }

    WaypointPath const* wpPath = sWaypointMgr.GetPathFromOrigin(wpOwner->GetEntry(), wpOwner->GetGUIDLow(), wpPathId, wpOrigin);
    if (!wpPath || wpPath->empty())
    {
        PSendSysMessage(LANG_WAYPOINT_NOTHINGTOEXPORT);
        SetSentErrorMessage(true);
        return false;
    }

    std::ofstream outfile;
    outfile.open(export_str);

    std::string table;
    char const* key_field;
    uint32 key;
    switch (wpOrigin)
    {
    case PATH_FROM_ENTRY: key = wpOwner->GetEntry();    key_field = "entry";    table = "creature_movement_template"; break;
    case PATH_FROM_GUID: key = wpOwner->GetGUIDLow();   key_field = "id";       table = "creature_movement"; break;
    case PATH_FROM_SPECIAL: key = wpOwner->GetEntry();  key_field = "id";    table = "creature_movement_special"; break;
    case PATH_NO_PATH:
        return false;
    }

    outfile << "DELETE FROM " << table << " WHERE " << key_field << "=" << key << ";\n";
    outfile << "INSERT INTO " << table << " (" << key_field << ", point, position_x, position_y, position_z, orientation, waittime, script_id) VALUES\n";

    WaypointPath::const_iterator itr = wpPath->begin();
    uint32 countDown = wpPath->size();
    for (; itr != wpPath->end(); ++itr, --countDown)
    {
        outfile << "(" << key << ",";
        outfile << itr->first << ",";
        outfile << itr->second.x << ",";
        outfile << itr->second.y << ",";
        outfile << itr->second.z << ",";
        outfile << itr->second.orientation << ",";
        outfile << itr->second.delay << ",";
        outfile << itr->second.script_id << ")";
        if (countDown > 1)
            outfile << ",\n";
        else
            outfile << ";\n";
    }

    PSendSysMessage(LANG_WAYPOINT_EXPORTED);
    outfile.close();

    return true;
}

//rename characters
bool ChatHandler::HandleCharacterRenameCommand(char* args)
{
    Player* target;
    ObjectGuid target_guid;
    std::string target_name;
    if (!ExtractPlayerTarget(&args, &target, &target_guid, &target_name))
        return false;

    if (target)
    {
        // check online security
        if (HasLowerSecurity(target))
            return false;

        PSendSysMessage(LANG_RENAME_PLAYER, GetNameLink(target).c_str());
        target->SetAtLoginFlag(AT_LOGIN_RENAME);
        CharacterDatabase.PExecute("UPDATE characters SET at_login = at_login | '1' WHERE guid = '%u'", target->GetGUIDLow());
    }
    else
    {
        // check offline security
        if (HasLowerSecurity(NULL, target_guid))
            return false;

        std::string oldNameLink = playerLink(target_name);

        PSendSysMessage(LANG_RENAME_PLAYER_GUID, oldNameLink.c_str(), target_guid.GetCounter());
        CharacterDatabase.PExecute("UPDATE characters SET at_login = at_login | '1' WHERE guid = '%u'", target_guid.GetCounter());
    }

    return true;
}

bool ChatHandler::HandleCharacterReputationCommand(char* args)
{
    Player* target;
    if (!ExtractPlayerTarget(&args, &target))
        return false;

    LocaleConstant loc = GetSessionDbcLocale();

    FactionStateList const& targetFSL = target->GetReputationMgr().GetStateList();
    for (FactionStateList::const_iterator itr = targetFSL.begin(); itr != targetFSL.end(); ++itr)
    {
        FactionEntry const *factionEntry = sObjectMgr.GetFactionEntry(itr->second.ID);

        ShowFactionListHelper(factionEntry, loc, &itr->second, target);
    }
    return true;
}

bool ChatHandler::HandleCharacterHasItemCommand(char* args)
{
    if(!*args)
        return false;

    uint32 itemId = 0;
    if (!ExtractUInt32(&args, itemId))
        return false;

    Player* plTarget;
    ObjectGuid target_guid;
    std::string target_name;

    if (!ExtractPlayerTarget(&args, &plTarget, &target_guid, &target_name))
        return false;

    ItemPrototype const* pItem = ObjectMgr::GetItemPrototype(itemId);

    if (!pItem)
    {
        PSendSysMessage("Unknown item %u", itemId);
        SetSentErrorMessage(true);
        return false;
    }

    uint32 itemCount = 0;

    if (plTarget)
    {
        itemCount = plTarget->GetItemCount(itemId, true);
    }
    else
    {
        std::unique_ptr<QueryResult> result(CharacterDatabase.PQuery(
            "SELECT SUM(count) AS item_count FROM item_instance ii WHERE itemEntry = %u and owner_guid = %u",
            itemId, target_guid.GetCounter()
        ));

        if (result)
        {
            auto fields = result->Fetch();
            itemCount = fields[0].GetUInt32();
        }
    }

    PSendSysMessage("%s's amount of %s (id %u) is: %u", target_name.c_str(), GetItemLink(pItem).c_str(), itemId, itemCount);
    return true;
}

//change standstate
bool ChatHandler::HandleModifyStandStateCommand(char* args)
{
    uint32 anim_id;
    if (!ExtractUInt32(&args, anim_id))
        return false;

    if (!sEmotesStore.LookupEntry(anim_id))
        return false;

    m_session->GetPlayer()->HandleEmoteState(anim_id);

    return true;
}

bool ChatHandler::HandleHonorShow(char* /*args*/)
{
    Player *target = getSelectedPlayer();
    if (!target)
        target = m_session->GetPlayer();

    int8 highest_rank               = target->GetHonorMgr().GetHighestRank().visualRank;
    uint32 dishonorable_kills       = target->GetUInt32Value(PLAYER_FIELD_LIFETIME_DISHONORABLE_KILLS);
    uint32 honorable_kills          = target->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);
    uint32 today_honorable_kills    = target->GetUInt16Value(PLAYER_FIELD_SESSION_KILLS, 0);
    uint32 today_dishonorable_kills = target->GetUInt16Value(PLAYER_FIELD_SESSION_KILLS, 1);
    uint32 yesterday_kills          = target->GetUInt32Value(PLAYER_FIELD_YESTERDAY_KILLS);
    uint32 yesterday_honor          = target->GetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION);
    uint32 this_week_kills          = target->GetUInt32Value(PLAYER_FIELD_THIS_WEEK_KILLS);
    uint32 this_week_honor          = target->GetUInt32Value(PLAYER_FIELD_THIS_WEEK_CONTRIBUTION);
    uint32 last_week_kills          = target->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_KILLS);
    uint32 last_week_honor          = target->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_CONTRIBUTION);
    uint32 last_week_standing       = target->GetUInt32Value(PLAYER_FIELD_LAST_WEEK_RANK);

    static int16 alliance_ranks[HONOR_RANK_COUNT] =
    {
        LANG_NO_RANK,
        LANG_RANK_PARIAH,
        LANG_RANK_OUTLAW,
        LANG_RANK_EXILED,
        LANG_RANK_DISHONORED,
        LANG_ALI_PRIVATE,
        LANG_ALI_CORPORAL,
        LANG_ALI_SERGEANT,
        LANG_ALI_MASTER_SERGEANT,
        LANG_ALI_SERGEANT_MAJOR,
        LANG_ALI_KNIGHT,
        LANG_ALI_KNIGHT_LIEUTENANT,
        LANG_ALI_KNIGHT_CAPTAIN,
        LANG_ALI_KNIGHT_CHAMPION,
        LANG_ALI_LIEUTENANT_COMMANDER,
        LANG_ALI_COMMANDER,
        LANG_ALI_MARSHAL,
        LANG_ALI_FIELD_MARSHAL,
        LANG_ALI_GRAND_MARSHAL,
        //LANG_GAME_MASTER
    };
    static int16 horde_ranks[HONOR_RANK_COUNT] =
    {
        LANG_NO_RANK,
        LANG_RANK_PARIAH,
        LANG_RANK_OUTLAW,
        LANG_RANK_EXILED,
        LANG_RANK_DISHONORED,
        LANG_HRD_SCOUT,
        LANG_HRD_GRUNT,
        LANG_HRD_SERGEANT,
        LANG_HRD_SENIOR_SERGEANT,
        LANG_HRD_FIRST_SERGEANT,
        LANG_HRD_STONE_GUARD,
        LANG_HRD_BLOOD_GUARD,
        LANG_HRD_LEGIONNARE,
        LANG_HRD_CENTURION,
        LANG_HRD_CHAMPION,
        LANG_HRD_LIEUTENANT_GENERAL,
        LANG_HRD_GENERAL,
        LANG_HRD_WARLORD,
        LANG_HRD_HIGH_WARLORD,
        //LANG_GAME_MASTER
    };
    char const* rank_name = NULL;
    char const* hrank_name = NULL;

    uint32 honor_rank = target->GetHonorMgr().GetRank().visualRank;

    if (honor_rank >= HONOR_RANK_COUNT || highest_rank >= HONOR_RANK_COUNT)
    {
        PSendSysMessage("CrashAlert : R%u et HR%u", honor_rank, highest_rank);
        return false;
    }

    if (target->GetTeam() == ALLIANCE)
    {
        rank_name = GetMangosString(alliance_ranks[ honor_rank ]);
        hrank_name = GetMangosString(alliance_ranks[ highest_rank ]);
    }
    else if (target->GetTeam() == HORDE)
    {
        rank_name = GetMangosString(horde_ranks[ honor_rank ]);
        hrank_name = GetMangosString(horde_ranks[ highest_rank ]);
    }
    else
    {
        rank_name = GetMangosString(LANG_NO_RANK);
        hrank_name = GetMangosString(LANG_NO_RANK);
    }

    PSendSysMessage(LANG_RANK, target->GetName(), rank_name, honor_rank);
    PSendSysMessage(LANG_HONOR_TODAY, today_honorable_kills, today_dishonorable_kills);
    PSendSysMessage(LANG_HONOR_YESTERDAY, yesterday_kills, yesterday_honor);
    PSendSysMessage(LANG_HONOR_THIS_WEEK, this_week_kills, this_week_honor);
    PSendSysMessage(LANG_HONOR_LAST_WEEK, last_week_kills, last_week_honor, last_week_standing);
    PSendSysMessage(LANG_HONOR_LIFE, target->GetHonorMgr().GetRankPoints(), honorable_kills, dishonorable_kills, highest_rank, hrank_name);

    return true;
}

bool ChatHandler::HandleHonorAddCommand(char* args)
{
    if (!*args)
        return false;

    Player *target = getSelectedPlayer();
    if (!target)
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    // check online security
    if (HasLowerSecurity(target))
        return false;

    float amount = (float)atof(args);
    target->GetHonorMgr().Add(amount, OTHER);
    return true;
}

bool ChatHandler::HandleHonorAddKillCommand(char* /*args*/)
{
    Unit *target = getSelectedUnit();
    if (!target)
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    if (target == m_session->GetPlayer())
        return false;

    m_session->GetPlayer()->RewardHonor(target, 1);
    return true;
}

bool ChatHandler::HandleModifyHonorCommand(char* args)
{
    if (!*args)
        return false;

    Player *target = getSelectedPlayer();
    if (!target)
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    char* field = ExtractLiteralArg(&args);
    if (!field)
        return false;

    int32 amount;
    if (!ExtractInt32(&args, amount))
        return false;

    // hack code
    if (hasStringAbbr(field, "points"))
    {
        if (amount < 0 || amount > 255)
            return false;
        // rank points is sent to client with same size of uint8(255) for each rank
        target->SetByteValue(PLAYER_FIELD_BYTES2, 0, amount);
    }
    else if (hasStringAbbr(field, "rank"))
    {
        if (amount < 0 || amount >= HONOR_RANK_COUNT)
            return false;
        target->SetByteValue(PLAYER_BYTES_3, 3, amount);
    }
    else if (hasStringAbbr(field, "todaykills"))
        target->SetUInt16Value(PLAYER_FIELD_SESSION_KILLS, 0, (uint32)amount);
    else if (hasStringAbbr(field, "yesterdaykills"))
        target->SetUInt32Value(PLAYER_FIELD_YESTERDAY_KILLS, (uint32)amount);
    else if (hasStringAbbr(field, "yesterdayhonor"))
        target->SetUInt32Value(PLAYER_FIELD_YESTERDAY_CONTRIBUTION, (uint32)amount);
    else if (hasStringAbbr(field, "thisweekkills"))
        target->SetUInt32Value(PLAYER_FIELD_THIS_WEEK_KILLS, (uint32)amount);
    else if (hasStringAbbr(field, "thisweekhonor"))
        target->SetUInt32Value(PLAYER_FIELD_THIS_WEEK_CONTRIBUTION, (uint32)amount);
    else if (hasStringAbbr(field, "lastweekkills"))
        target->SetUInt32Value(PLAYER_FIELD_LAST_WEEK_KILLS, (uint32)amount);
    else if (hasStringAbbr(field, "lastweekhonor"))
        target->SetUInt32Value(PLAYER_FIELD_LAST_WEEK_CONTRIBUTION, (uint32)amount);
    else if (hasStringAbbr(field, "lastweekstanding"))
        target->SetUInt32Value(PLAYER_FIELD_LAST_WEEK_RANK, (uint32)amount);
    else if (hasStringAbbr(field, "lifetimedishonorablekills"))
        target->SetUInt32Value(PLAYER_FIELD_LIFETIME_DISHONORABLE_KILLS, (uint32)amount);
    else if (hasStringAbbr(field, "lifetimehonorablekills"))
        target->SetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS, (uint32)amount);

    PSendSysMessage(LANG_COMMAND_MODIFY_HONOR, field, target->GetName(), hasStringAbbr(field, "rank") ? amount : (uint32)amount);

    return true;
}

bool ChatHandler::HandleHonorResetCommand(char* /*args*/)
{
    Player *target = getSelectedPlayer();
    if (!target)
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    target->GetHonorMgr().Reset();
    return true;
}

bool ChatHandler::HandleLookupEventCommand(char* args)
{
    if (!*args)
        return false;

    std::string namepart = args;
    std::wstring wnamepart;

    // converting string that we try to find to lower case
    if (!Utf8toWStr(namepart, wnamepart))
        return false;

    wstrToLower(wnamepart);

    uint32 counter = 0;

    GameEventMgr::GameEventDataMap const& events = sGameEventMgr.GetEventMap();

    for (uint32 id = 1; id < events.size(); ++id)
    {
        if (!sGameEventMgr.IsValidEvent(id))
            continue;

        GameEventData const& eventData = events[id];

        std::string descr = eventData.description;
        if (descr.empty())
            continue;

        if (Utf8FitTo(descr, wnamepart))
        {
            char const* active = sGameEventMgr.IsActiveEvent(id) ? GetMangosString(LANG_ACTIVE) : "";

            if (m_session)
                PSendSysMessage(LANG_EVENT_ENTRY_LIST_CHAT, id, id, eventData.description.c_str(), active);
            else
                PSendSysMessage(LANG_EVENT_ENTRY_LIST_CONSOLE, id, eventData.description.c_str(), active);

            ++counter;
        }
    }

    if (counter == 0)
        SendSysMessage(LANG_NOEVENTFOUND);

    return true;
}

bool ChatHandler::HandleEventListCommand(char* args)
{
    uint32 counter = 0;
    bool all = false;
    std::string arg = args;
    if (arg == "all")
        all = true;

    GameEventMgr::GameEventDataMap const& events = sGameEventMgr.GetEventMap();

    char const* active = GetMangosString(LANG_ACTIVE);
    char const* inactive = GetMangosString(LANG_FACTION_INACTIVE);
    char const* state = "";

    for (uint32 event_id = 0; event_id < events.size(); ++event_id)
    {
        if (!sGameEventMgr.IsValidEvent(event_id))
            continue;

        if (!sGameEventMgr.IsActiveEvent(event_id))
        {
            if (!all)
                continue;
            state = inactive;
        }
        else
            state = active;

        GameEventData const& eventData = events[event_id];

        if (m_session)
            PSendSysMessage(LANG_EVENT_ENTRY_LIST_CHAT, event_id, event_id, eventData.description.c_str(), state);
        else
            PSendSysMessage(LANG_EVENT_ENTRY_LIST_CONSOLE, event_id, eventData.description.c_str(), state);

        ++counter;
    }

    if (counter == 0)
        SendSysMessage(LANG_NOEVENTFOUND);

    return true;
}

bool ChatHandler::HandleEventInfoCommand(char* args)
{
    if (!*args)
        return false;

    // id or [name] Shift-click form |color|Hgameevent:id|h[name]|h|r
    uint32 event_id;
    if (!ExtractUint32KeyFromLink(&args, "Hgameevent", event_id))
        return false;

    GameEventMgr::GameEventDataMap const& events = sGameEventMgr.GetEventMap();

    if (!sGameEventMgr.IsValidEvent(event_id))
    {
        SendSysMessage(LANG_EVENT_NOT_EXIST);
        SetSentErrorMessage(true);
        return false;
    }

    GameEventData const& eventData = events[event_id];

    char const* activeStr = sGameEventMgr.IsActiveEvent(event_id) ? GetMangosString(LANG_ACTIVE) : "";

    std::string startTimeStr = TimeToTimestampStr(eventData.start);
    std::string endTimeStr = TimeToTimestampStr(eventData.end);

    uint32 delay = sGameEventMgr.NextCheck(event_id);
    time_t nextTime = time(NULL) + delay;
    std::string nextStr = nextTime >= eventData.start && nextTime < eventData.end ? TimeToTimestampStr(time(NULL) + delay) : "-";

    std::string occurenceStr = secsToTimeString(eventData.occurence * MINUTE);
    std::string lengthStr = secsToTimeString(eventData.length * MINUTE);

    PSendSysMessage(LANG_EVENT_INFO, event_id, eventData.description.c_str(), activeStr,
                    startTimeStr.c_str(), endTimeStr.c_str(), occurenceStr.c_str(), lengthStr.c_str(),
                    nextStr.c_str());
    return true;
}

bool ChatHandler::HandleEventStartCommand(char* args)
{
    if (!*args)
        return false;

    // id or [name] Shift-click form |color|Hgameevent:id|h[name]|h|r
    uint32 event_id;
    if (!ExtractUint32KeyFromLink(&args, "Hgameevent", event_id))
        return false;

    GameEventMgr::GameEventDataMap const& events = sGameEventMgr.GetEventMap();

    if (!sGameEventMgr.IsValidEvent(event_id))
    {
        SendSysMessage(LANG_EVENT_NOT_EXIST);
        SetSentErrorMessage(true);
        return false;
    }

    GameEventData const& eventData = events[event_id];
    if (!eventData.isValid())
    {
        SendSysMessage(LANG_EVENT_NOT_EXIST);
        SetSentErrorMessage(true);
        return false;
    }

    if (sGameEventMgr.IsActiveEvent(event_id))
    {
        PSendSysMessage(LANG_EVENT_ALREADY_ACTIVE, event_id);
        SetSentErrorMessage(true);
        return false;
    }

    if (!sGameEventMgr.IsEnabled(event_id))
    {
        PSendSysMessage(LANG_EVENT_DISABLED, event_id);
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage(LANG_EVENT_STARTED, event_id, eventData.description.c_str());
    sGameEventMgr.StartEvent(event_id, true);
    return true;
}

bool ChatHandler::HandleEventStopCommand(char* args)
{
    if (!*args)
        return false;

    // id or [name] Shift-click form |color|Hgameevent:id|h[name]|h|r
    uint32 event_id;
    if (!ExtractUint32KeyFromLink(&args, "Hgameevent", event_id))
        return false;

    GameEventMgr::GameEventDataMap const& events = sGameEventMgr.GetEventMap();

    if (!sGameEventMgr.IsValidEvent(event_id))
    {
        SendSysMessage(LANG_EVENT_NOT_EXIST);
        SetSentErrorMessage(true);
        return false;
    }

    GameEventData const& eventData = events[event_id];
    if (!eventData.isValid())
    {
        SendSysMessage(LANG_EVENT_NOT_EXIST);
        SetSentErrorMessage(true);
        return false;
    }

    if (!sGameEventMgr.IsActiveEvent(event_id))
    {
        PSendSysMessage(LANG_EVENT_NOT_ACTIVE, event_id);
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage(LANG_EVENT_STOPPED, event_id, eventData.description.c_str());
    sGameEventMgr.StopEvent(event_id, true);
    return true;
}

bool ChatHandler::HandleEventEnableCommand(char* args)
{
    if (!*args)
        return false;

    // id or [name] Shift-click form |color|Hgameevent:id|h[name]|h|r
    uint32 event_id;
    if (!ExtractUint32KeyFromLink(&args, "Hgameevent", event_id))
        return false;

    GameEventMgr::GameEventDataMap const& events = sGameEventMgr.GetEventMap();

    if (!sGameEventMgr.IsValidEvent(event_id))
    {
        SendSysMessage(LANG_EVENT_NOT_EXIST);
        SetSentErrorMessage(true);
        return false;
    }

    GameEventData const& eventData = events[event_id];
    if (!eventData.isValid())
    {
        SendSysMessage(LANG_EVENT_NOT_EXIST);
        SetSentErrorMessage(true);
        return false;
    }

    if (sGameEventMgr.IsEnabled(event_id))
    {
        PSendSysMessage(LANG_EVENT_ALREADY_ENABLED, event_id);
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage(LANG_EVENT_ENABLED, event_id, eventData.description.c_str());
    sGameEventMgr.EnableEvent(event_id, true);
    return true;
}

bool ChatHandler::HandleEventDisableCommand(char* args)
{
    if (!*args)
        return false;

    // id or [name] Shift-click form |color|Hgameevent:id|h[name]|h|r
    uint32 event_id;
    if (!ExtractUint32KeyFromLink(&args, "Hgameevent", event_id))
        return false;

    GameEventMgr::GameEventDataMap const& events = sGameEventMgr.GetEventMap();

    if (!sGameEventMgr.IsValidEvent(event_id))
    {
        SendSysMessage(LANG_EVENT_NOT_EXIST);
        SetSentErrorMessage(true);
        return false;
    }

    GameEventData const& eventData = events[event_id];
    if (!eventData.isValid())
    {
        SendSysMessage(LANG_EVENT_NOT_EXIST);
        SetSentErrorMessage(true);
        return false;
    }

    if (!sGameEventMgr.IsEnabled(event_id))
    {
        PSendSysMessage(LANG_EVENT_ALREADY_DISABLED, event_id);
        SetSentErrorMessage(true);
        return false;
    }

    PSendSysMessage(LANG_EVENT_DISABLED, event_id, eventData.description.c_str());
    sGameEventMgr.EnableEvent(event_id, false);
    return true;
}

bool ChatHandler::HandleCombatStopCommand(char* args)
{
    Player* target;
    if (!ExtractPlayerTarget(&args, &target))
        return false;

    // check online security
    if (HasLowerSecurity(target))
        return false;

    target->CombatStop();
    target->getHostileRefManager().deleteReferences();
    return true;
}

void ChatHandler::HandleLearnSkillRecipesHelper(Player* player, uint32 skill_id)
{
    uint32 classmask = player->getClassMask();

    for (uint32 j = 0; j < sObjectMgr.GetMaxSkillLineAbilityId(); ++j)
    {
        SkillLineAbilityEntry const *skillLine = sObjectMgr.GetSkillLineAbility(j);
        if (!skillLine)
            continue;

        // wrong skill
        if (skillLine->skillId != skill_id)
            continue;

        // not high rank
        if (skillLine->forward_spellid)
            continue;

        // skip racial skills
        if (skillLine->racemask != 0)
            continue;

        // skip wrong class skills
        if (skillLine->classmask && (skillLine->classmask & classmask) == 0)
            continue;

        SpellEntry const* spellInfo = sSpellMgr.GetSpellEntry(skillLine->spellId);
        if (!spellInfo || !SpellMgr::IsSpellValid(spellInfo, player, false))
            continue;

        player->learnSpell(skillLine->spellId, false);
    }
}

bool ChatHandler::HandleLearnAllCraftsCommand(char* /*args*/)
{
    for (uint32 i = 0; i < sSkillLineStore.GetNumRows(); ++i)
    {
        SkillLineEntry const *skillInfo = sSkillLineStore.LookupEntry(i);
        if (!skillInfo)
            continue;

        if (skillInfo->categoryId == SKILL_CATEGORY_PROFESSION || skillInfo->categoryId == SKILL_CATEGORY_SECONDARY)
            HandleLearnSkillRecipesHelper(m_session->GetPlayer(), skillInfo->id);
    }

    SendSysMessage(LANG_COMMAND_LEARN_ALL_CRAFT);
    return true;
}

bool ChatHandler::HandleLearnAllRecipesCommand(char* args)
{
    //  Learns all recipes of specified profession and sets skill to max
    //  Example: .learn all_recipes enchanting

    Player* target = getSelectedPlayer();
    if (!target)
    {
        SendSysMessage(LANG_PLAYER_NOT_FOUND);
        return false;
    }

    if (!*args)
        return false;

    std::wstring wnamepart;

    if (!Utf8toWStr(args, wnamepart))
        return false;

    // converting string that we try to find to lower case
    wstrToLower(wnamepart);

    std::string name;

    SkillLineEntry const *targetSkillInfo = NULL;
    for (uint32 i = 1; i < sSkillLineStore.GetNumRows(); ++i)
    {
        SkillLineEntry const *skillInfo = sSkillLineStore.LookupEntry(i);
        if (!skillInfo)
            continue;

        if (skillInfo->categoryId != SKILL_CATEGORY_PROFESSION && skillInfo->categoryId != SKILL_CATEGORY_SECONDARY)
            continue;

        int loc = GetSessionDbcLocale();
        name = skillInfo->name[loc];
        if (name.empty())
            continue;

        if (!Utf8FitTo(name, wnamepart))
        {
            loc = 0;
            for (; loc < MAX_DBC_LOCALE; ++loc)
            {
                if (loc == GetSessionDbcLocale())
                    continue;

                name = skillInfo->name[loc];
                if (name.empty())
                    continue;

                if (Utf8FitTo(name, wnamepart))
                    break;
            }
        }

        if (loc < MAX_DBC_LOCALE)
        {
            targetSkillInfo = skillInfo;
            break;
        }
    }

    if (!targetSkillInfo)
        return false;

    HandleLearnSkillRecipesHelper(target, targetSkillInfo->id);

    uint16 maxLevel = target->GetPureMaxSkillValue(targetSkillInfo->id);
    target->SetSkill(targetSkillInfo->id, maxLevel, maxLevel);
    PSendSysMessage(LANG_COMMAND_LEARN_ALL_RECIPES, name.c_str());
    return true;
}

bool ChatHandler::HandleLookupAccountEmailCommand(char* args)
{
    char* emailStr = ExtractQuotedOrLiteralArg(&args);
    if (!emailStr)
        return false;

    uint32 limit;
    if (!ExtractOptUInt32(&args, limit, 100))
        return false;

    std::string email = emailStr;
    LoginDatabase.escape_string(email);

    // No wildcard in front, cannot use table index
    LoginDatabase.AsyncPQuery(AccountSearchHandler::HandleAccountLookupResult, GetAccountId(), limit,
        //      0   1         2        3        4
        "SELECT id, username, last_ip, 0, expansion FROM account WHERE email " _LIKE_ " " _CONCAT2_("'%s'", "'%%'") " LIMIT %u",
        email.c_str(), limit);

    return true;
}

bool ChatHandler::HandleLookupAccountIpCommand(char* args)
{
    return ShowAccountIpListHelper(args, false);
}

bool ChatHandler::HandleLookupAccountIponlineCommand(char* args)
{
    return ShowAccountIpListHelper(args, true);
}

bool ChatHandler::HandleLookupAccountNameCommand(char* args)
{
    char* accountStr = ExtractQuotedOrLiteralArg(&args);
    if (!accountStr)
        return false;

    uint32 limit;
    if (!ExtractOptUInt32(&args, limit, 100))
        return false;

    std::string account = accountStr;
    if (!AccountMgr::normalizeString(account))
        return false;

    LoginDatabase.escape_string(account);

    LoginDatabase.AsyncPQuery(AccountSearchHandler::HandleAccountLookupResult, GetAccountId(), limit,
        //      0   1         2        3        4
        "SELECT id, username, last_ip, 0, expansion FROM account WHERE username " _LIKE_ " " _CONCAT3_("'%%'", "'%s'", "'%%'") " LIMIT %u",
        account.c_str(), limit);

    return true;
}

bool ChatHandler::ShowAccountIpListHelper(char* args, bool onlineonly)
{
    char* ipStr = ExtractQuotedOrLiteralArg(&args);
    if (!ipStr)
        return false;

    uint32 limit;
    if (!ExtractOptUInt32(&args, limit, 100))
        return false;

    std::string ip = ipStr;
    LoginDatabase.escape_string(ip);

    const char *query = onlineonly
        ? "SELECT id, username, last_ip, 0, expansion FROM account WHERE online = 1 AND last_ip " _LIKE_ " " _CONCAT2_("'%s'", "'%%'") " LIMIT %u"
        : "SELECT id, username, last_ip, 0, expansion FROM account WHERE                last_ip " _LIKE_ " " _CONCAT2_("'%s'", "'%%'") " LIMIT %u";

    LoginDatabase.AsyncPQuery(AccountSearchHandler::HandleAccountLookupResult, GetAccountId(), limit, query, ip.c_str(), limit);

    return true;
}

bool ChatHandler::HandleLookupPlayerIpCommand(char* args)
{
    char* ipStr = ExtractQuotedOrLiteralArg(&args);
    if (!ipStr)
        return false;

    uint32 limit;
    if (!ExtractOptUInt32(&args, limit, 100))
        return false;

    QueryResult* result = NULL;
    std::string ip = ipStr;
    LoginDatabase.escape_string(ip);
    result = LoginDatabase.PQuery("SELECT id, username FROM account WHERE last_ip " _LIKE_ " " _CONCAT2_("'%s'", "'%%'"), ip.c_str());

    return LookupPlayerSearchCommand(result, &limit);
}

bool ChatHandler::HandleLookupPlayerAccountCommand(char* args)
{
    char* accountStr = ExtractQuotedOrLiteralArg(&args);
    if (!accountStr)
        return false;

    uint32 limit;
    if (!ExtractOptUInt32(&args, limit, 100))
        return false;

    std::string account = accountStr;
    if (!AccountMgr::normalizeString(account))
        return false;

    LoginDatabase.escape_string(account);

    QueryResult* result = LoginDatabase.PQuery("SELECT id,username FROM account WHERE username " _LIKE_ " " _CONCAT2_("'%s'", "'%%'"), account.c_str());

    return LookupPlayerSearchCommand(result, &limit);
}

bool ChatHandler::HandleLookupPlayerEmailCommand(char* args)
{
    char* emailStr = ExtractQuotedOrLiteralArg(&args);
    if (!emailStr)
        return false;

    uint32 limit;
    if (!ExtractOptUInt32(&args, limit, 100))
        return false;

    std::string email = emailStr;
    LoginDatabase.escape_string(email);

    QueryResult* result = LoginDatabase.PQuery("SELECT id,username FROM account WHERE email " _LIKE_ " " _CONCAT2_("'%s'", "'%%'"), email.c_str());

    return LookupPlayerSearchCommand(result, &limit);
}

bool ChatHandler::HandleLookupPlayerNameCommand(char* args)
{
    char* nameStr = ExtractQuotedOrLiteralArg(&args);
    if (!nameStr)
        return false;

    uint32 limit;
    if (!ExtractOptUInt32(&args, limit, 100))
        return false;

    uint32 limit_original = limit;
    std::string name = nameStr;
    CharacterDatabase.escape_string(name);

    CharacterDatabase.AsyncPQuery(&PlayerSearchHandler::HandlePlayerCharacterLookupResult,
        GetAccountId(), limit_original,
        "SELECT guid, name, race, class, level FROM characters WHERE name " _LIKE_ " " _CONCAT3_("'%%'", "'%s'", "'%%'"),
        name.c_str());

    return true;
}

bool ChatHandler::HandleLookupPlayerCharacterCommand(char* args)
{
    char* nameStr = ExtractQuotedOrLiteralArg(&args);
    if (!nameStr)
        return false;

    uint32 limit;
    if (!ExtractOptUInt32(&args, limit, 100))
        return false;

    QueryResult* result = NULL;
    std::string normalizedName = nameStr;
    if (normalizePlayerName(normalizedName))
        if (PlayerCacheData const* data = sObjectMgr.GetPlayerDataByName(normalizedName))
            if (result = LoginDatabase.PQuery("SELECT id, last_ip FROM account WHERE id = %u", data->uiAccount))
            {
                Field* fields = result->Fetch();
                uint32 id = fields[0].GetInt32();
                std::string ip = fields[1].GetCppString();
                delete result;

                AccountTypes security = sAccountMgr.GetSecurity(id);
                if (GetAccessLevel() < security || (GetAccessLevel() < SEC_ADMINISTRATOR && security > SEC_PLAYER))
                    return LookupPlayerSearchCommand(NULL, &limit);

                LoginDatabase.escape_string(ip);
                result = LoginDatabase.PQuery("SELECT id, username FROM account WHERE last_ip = '%s'", ip.c_str());
            }

    return LookupPlayerSearchCommand(result, &limit);
}

bool ChatHandler::LookupPlayerSearchCommand(QueryResult* result, uint32* limit)
{
    if (!result)
    {
        PSendSysMessage(LANG_NO_PLAYERS_FOUND);
        SetSentErrorMessage(true);
        return false;
    }

    uint32 limit_original = limit ? *limit : 100;

    uint32 limit_local = limit_original;

    if (!limit)
        limit = &limit_local;

    PlayerSearchQueryHolder* holder = new PlayerSearchQueryHolder(m_session ? m_session->GetAccountId() : 0, limit_local);
    holder->SetSize(result->GetRowCount());
    uint32 count = 0;
    do
    {
        // Definitely can't stay below the limit if we've found beyond the limit in accounts
        if (limit && count == *limit)
            break;

        Field* fields = result->Fetch();
        uint32 acc_id = fields[0].GetUInt32();
        std::string acc_name = fields[1].GetCppString();

        holder->SetPQuery(count, "SELECT guid, name, race, class, level FROM characters WHERE account = %u", acc_id);
        holder->AddAccountInfo(count, acc_id, acc_name);

        ++count;
    }
    while (result->NextRow());
    delete result;

    CharacterDatabase.DelayQueryHolder(&PlayerSearchHandler::HandlePlayerAccountSearchResult, holder, 0);
    return true;
}

void ChatHandler::ShowPoolListHelper(uint16 pool_id)
{
    PoolTemplateData const& pool_template = sPoolMgr.GetPoolTemplate(pool_id);
    if (m_session)
        PSendSysMessage(LANG_POOL_ENTRY_LIST_CHAT,
                        pool_id, pool_id, pool_template.description.c_str(), pool_template.IsAutoSpawn() ? 1 : 0, pool_template.MaxLimit,
                        sPoolMgr.GetPoolCreatures(pool_id).size(), sPoolMgr.GetPoolGameObjects(pool_id).size(), sPoolMgr.GetPoolPools(pool_id).size());
    else
        PSendSysMessage(LANG_POOL_ENTRY_LIST_CONSOLE,
                        pool_id, pool_template.description.c_str(), pool_template.IsAutoSpawn() ? 1 : 0, pool_template.MaxLimit,
                        sPoolMgr.GetPoolCreatures(pool_id).size(), sPoolMgr.GetPoolGameObjects(pool_id).size(), sPoolMgr.GetPoolPools(pool_id).size());
}

bool ChatHandler::HandleLookupPoolCommand(char * args)
{
    if (!*args)
        return false;

    std::string namepart = args;

    Player* player = m_session ? m_session->GetPlayer() : NULL;
    MapPersistentState* mapState = player ? player->GetMap()->GetPersistentState() : NULL;

    strToLower(namepart);

    uint32 counter = 0;

    // spawn pools for expected map or for not initialized shared pools state for non-instanceable maps
    for (uint16 pool_id = 0; pool_id < sPoolMgr.GetMaxPoolId(); ++pool_id)
    {
        PoolTemplateData const& pool_template = sPoolMgr.GetPoolTemplate(pool_id);

        std::string desc = pool_template.description;
        strToLower(desc);

        if (desc.find(namepart) == std::wstring::npos)
            continue;

        ShowPoolListHelper(pool_id);
        ++counter;
    }

    if (counter == 0)
        SendSysMessage(LANG_NO_POOL);

    return true;
}

bool ChatHandler::HandlePoolListCommand(char* args)
{
    Player* player = m_session->GetPlayer();

    MapPersistentState* mapState = player->GetMap()->GetPersistentState();

    if (!mapState->GetMapEntry()->Instanceable())
    {
        PSendSysMessage(LANG_POOL_LIST_NON_INSTANCE, mapState->GetMapEntry()->name[GetSessionDbcLocale()], mapState->GetMapId());
        SetSentErrorMessage(false);
        return false;
    }

    uint32 counter = 0;

    // spawn pools for expected map or for not initialized shared pools state for non-instanceable maps
    for (uint16 pool_id = 0; pool_id < sPoolMgr.GetMaxPoolId(); ++pool_id)
    {
        if (sPoolMgr.GetPoolTemplate(pool_id).CanBeSpawnedAtMap(mapState->GetMapEntry()))
        {
            ShowPoolListHelper(pool_id);
            ++counter;
        }
    }

    if (counter == 0)
        PSendSysMessage(LANG_NO_POOL_FOR_MAP, mapState->GetMapEntry()->name[GetSessionDbcLocale()], mapState->GetMapId());

    return true;
}

bool ChatHandler::HandlePoolUpdateCommand(char* args)
{
    Player* player = m_session->GetPlayer();

    MapPersistentState* mapState = player->GetMap()->GetPersistentState();
    SpawnedPoolData& spawns = mapState->GetSpawnedPoolData();

    // shared continent pools data expected too big for show
    uint32 pool_id = 0;
    if (!ExtractUint32KeyFromLink(&args, "Hpool", pool_id))
        return false;

    PoolGroup<GameObject>& gobjGroup = sPoolMgr.GetPoolGameObjects(pool_id);
    PoolTemplateData const& pool_template = sPoolMgr.GetPoolTemplate(pool_id);

    PSendSysMessage("Pool #%u: %u objects spawned [limit = %u]", pool_id, spawns.GetSpawnedObjects(pool_id), pool_template.MaxLimit);
    sPoolMgr.UpdatePool<GameObject>(*mapState, pool_id);
    sPoolMgr.UpdatePool<Creature>(*mapState, pool_id);
    return true;
}

bool ChatHandler::HandlePoolSpawnsCommand(char* args)
{
    Player* player = m_session->GetPlayer();

    MapPersistentState* mapState = player->GetMap()->GetPersistentState();

    // shared continent pools data expected too big for show
    uint32 pool_id = 0;
    if (!ExtractUint32KeyFromLink(&args, "Hpool", pool_id) && !mapState->GetMapEntry()->Instanceable())
    {
        PSendSysMessage(LANG_POOL_SPAWNS_NON_INSTANCE, mapState->GetMapEntry()->name[GetSessionDbcLocale()], mapState->GetMapId());
        SetSentErrorMessage(false);
        return false;
    }

    SpawnedPoolData const& spawns = mapState->GetSpawnedPoolData();

    SpawnedPoolObjects const& crSpawns = spawns.GetSpawnedCreatures();
    for (SpawnedPoolObjects::const_iterator itr = crSpawns.begin(); itr != crSpawns.end(); ++itr)
        if (!pool_id || pool_id == sPoolMgr.IsPartOfAPool<Creature>(*itr))
            if (CreatureData const* data = sObjectMgr.GetCreatureData(*itr))
                if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data->id))
                    PSendSysMessage(LANG_CREATURE_LIST_CHAT, *itr, PrepareStringNpcOrGoSpawnInformation<Creature>(*itr).c_str(),
                                    *itr, info->Name, data->posX, data->posY, data->posZ, data->mapid);

    SpawnedPoolObjects const& goSpawns = spawns.GetSpawnedGameobjects();
    for (SpawnedPoolObjects::const_iterator itr = goSpawns.begin(); itr != goSpawns.end(); ++itr)
        if (!pool_id || pool_id == sPoolMgr.IsPartOfAPool<GameObject>(*itr))
            if (GameObjectData const* data = sObjectMgr.GetGOData(*itr))
                if (GameObjectInfo const* info = ObjectMgr::GetGameObjectInfo(data->id))
                    PSendSysMessage(LANG_GO_LIST_CHAT, *itr, PrepareStringNpcOrGoSpawnInformation<GameObject>(*itr).c_str(),
                                    *itr, info->name, data->posX, data->posY, data->posZ, data->mapid);

    return true;
}

bool ChatHandler::HandlePoolInfoCommand(char* args)
{
    // id or [name] Shift-click form |color|Hpool:id|h[name]|h|r
    uint32 pool_id;
    if (!ExtractUint32KeyFromLink(&args, "Hpool", pool_id))
        return false;

    Player* player = m_session ? m_session->GetPlayer() : NULL;

    MapPersistentState* mapState = player ? player->GetMap()->GetPersistentState() : NULL;
    SpawnedPoolData const* spawns = mapState ? &mapState->GetSpawnedPoolData() : NULL;

    std::string active_str = GetMangosString(LANG_ACTIVE);

    PoolTemplateData const& pool_template = sPoolMgr.GetPoolTemplate(pool_id);
    uint32 mother_pool_id = sPoolMgr.IsPartOfAPool<Pool>(pool_id);
    if (!mother_pool_id)
        PSendSysMessage(LANG_POOL_INFO_HEADER, pool_id, pool_template.IsAutoSpawn(), pool_template.MaxLimit);
    else
    {
        PoolTemplateData const& mother_template = sPoolMgr.GetPoolTemplate(mother_pool_id);
        if (m_session)
            PSendSysMessage(LANG_POOL_INFO_HEADER_CHAT, pool_id, mother_pool_id, mother_pool_id, mother_template.description.c_str(),
                            pool_template.IsAutoSpawn(), pool_template.MaxLimit);
        else
            PSendSysMessage(LANG_POOL_INFO_HEADER_CONSOLE, pool_id, mother_pool_id, mother_template.description.c_str(),
                            pool_template.IsAutoSpawn(), pool_template.MaxLimit);
    }

    PoolGroup<Creature> const& poolCreatures = sPoolMgr.GetPoolCreatures(pool_id);
    SpawnedPoolObjects const* crSpawns = spawns ? &spawns->GetSpawnedCreatures() : NULL;

    PoolObjectList const& poolCreaturesEx = poolCreatures.GetExplicitlyChanced();
    if (!poolCreaturesEx.empty())
    {
        SendSysMessage(LANG_POOL_CHANCE_CREATURE_LIST_HEADER);
        for (PoolObjectList::const_iterator itr = poolCreaturesEx.begin(); itr != poolCreaturesEx.end(); ++itr)
        {
            if (CreatureData const* data = sObjectMgr.GetCreatureData(itr->guid))
            {
                if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data->id))
                {
                    char const* active = crSpawns && crSpawns->find(itr->guid) != crSpawns->end() ? active_str.c_str() : "";
                    if (m_session)
                        PSendSysMessage(LANG_POOL_CHANCE_CREATURE_LIST_CHAT, itr->guid, PrepareStringNpcOrGoSpawnInformation<Creature>(itr->guid).c_str(),
                                        itr->guid, info->Name, data->posX, data->posY, data->posZ, data->mapid, itr->chance, active);
                    else
                        PSendSysMessage(LANG_POOL_CHANCE_CREATURE_LIST_CONSOLE, itr->guid, PrepareStringNpcOrGoSpawnInformation<Creature>(itr->guid).c_str(),
                                        info->Name, data->posX, data->posY, data->posZ, data->mapid, itr->chance, active);
                }
            }
        }
    }

    PoolObjectList const& poolCreaturesEq = poolCreatures.GetEqualChanced();
    if (!poolCreaturesEq.empty())
    {
        SendSysMessage(LANG_POOL_CREATURE_LIST_HEADER);
        for (PoolObjectList::const_iterator itr = poolCreaturesEq.begin(); itr != poolCreaturesEq.end(); ++itr)
        {
            if (CreatureData const* data = sObjectMgr.GetCreatureData(itr->guid))
            {
                if (CreatureInfo const* info = ObjectMgr::GetCreatureTemplate(data->id))
                {
                    char const* active = crSpawns && crSpawns->find(itr->guid) != crSpawns->end() ? active_str.c_str() : "";
                    if (m_session)
                        PSendSysMessage(LANG_POOL_CREATURE_LIST_CHAT, itr->guid, PrepareStringNpcOrGoSpawnInformation<Creature>(itr->guid).c_str(),
                                        itr->guid, info->Name, data->posX, data->posY, data->posZ, data->mapid, active);
                    else
                        PSendSysMessage(LANG_POOL_CREATURE_LIST_CONSOLE, itr->guid, PrepareStringNpcOrGoSpawnInformation<Creature>(itr->guid).c_str(),
                                        info->Name, data->posX, data->posY, data->posZ, data->mapid, active);
                }
            }
        }
    }

    PoolGroup<GameObject> const& poolGameObjects = sPoolMgr.GetPoolGameObjects(pool_id);
    SpawnedPoolObjects const* goSpawns = spawns ? &spawns->GetSpawnedGameobjects() : NULL;

    PoolObjectList const& poolGameObjectsEx = poolGameObjects.GetExplicitlyChanced();
    if (!poolGameObjectsEx.empty())
    {
        SendSysMessage(LANG_POOL_CHANCE_GO_LIST_HEADER);
        for (PoolObjectList::const_iterator itr = poolGameObjectsEx.begin(); itr != poolGameObjectsEx.end(); ++itr)
        {
            if (GameObjectData const* data = sObjectMgr.GetGOData(itr->guid))
            {
                if (GameObjectInfo const* info = ObjectMgr::GetGameObjectInfo(data->id))
                {
                    char const* active = goSpawns && goSpawns->find(itr->guid) != goSpawns->end() ? active_str.c_str() : "";
                    if (m_session)
                        PSendSysMessage(LANG_POOL_CHANCE_GO_LIST_CHAT, itr->guid, PrepareStringNpcOrGoSpawnInformation<GameObject>(itr->guid).c_str(),
                                        itr->guid, info->name, data->posX, data->posY, data->posZ, data->mapid, itr->chance, active);
                    else
                        PSendSysMessage(LANG_POOL_CHANCE_GO_LIST_CONSOLE, itr->guid, PrepareStringNpcOrGoSpawnInformation<GameObject>(itr->guid).c_str(),
                                        info->name, data->posX, data->posY, data->posZ, data->mapid, itr->chance, active);
                }
            }
        }
    }

    PoolObjectList const& poolGameObjectsEq = poolGameObjects.GetEqualChanced();
    if (!poolGameObjectsEq.empty())
    {
        SendSysMessage(LANG_POOL_GO_LIST_HEADER);
        for (PoolObjectList::const_iterator itr = poolGameObjectsEq.begin(); itr != poolGameObjectsEq.end(); ++itr)
        {
            if (GameObjectData const* data = sObjectMgr.GetGOData(itr->guid))
            {
                if (GameObjectInfo const* info = ObjectMgr::GetGameObjectInfo(data->id))
                {
                    char const* active = goSpawns && goSpawns->find(itr->guid) != goSpawns->end() ? active_str.c_str() : "";
                    if (m_session)
                        PSendSysMessage(LANG_POOL_GO_LIST_CHAT, itr->guid, PrepareStringNpcOrGoSpawnInformation<GameObject>(itr->guid).c_str(),
                                        itr->guid, info->name, data->posX, data->posY, data->posZ, data->mapid, active);
                    else
                        PSendSysMessage(LANG_POOL_GO_LIST_CONSOLE, itr->guid, PrepareStringNpcOrGoSpawnInformation<GameObject>(itr->guid).c_str(),
                                        info->name, data->posX, data->posY, data->posZ, data->mapid, active);
                }
            }
        }
    }

    PoolGroup<Pool> const& poolPools = sPoolMgr.GetPoolPools(pool_id);
    SpawnedPoolPools const* poolSpawns = spawns ? &spawns->GetSpawnedPools() : NULL;

    PoolObjectList const& poolPoolsEx = poolPools.GetExplicitlyChanced();
    if (!poolPoolsEx.empty())
    {
        SendSysMessage(LANG_POOL_CHANCE_POOL_LIST_HEADER);
        for (PoolObjectList::const_iterator itr = poolPoolsEx.begin(); itr != poolPoolsEx.end(); ++itr)
        {
            PoolTemplateData const& itr_template = sPoolMgr.GetPoolTemplate(itr->guid);
            char const* active = poolSpawns && poolSpawns->find(itr->guid) != poolSpawns->end() ? active_str.c_str() : "";
            if (m_session)
                PSendSysMessage(LANG_POOL_CHANCE_POOL_LIST_CHAT, itr->guid,
                                itr->guid, itr_template.description.c_str(), itr_template.IsAutoSpawn() ? 1 : 0, itr_template.MaxLimit,
                                sPoolMgr.GetPoolCreatures(itr->guid).size(), sPoolMgr.GetPoolGameObjects(itr->guid).size(), sPoolMgr.GetPoolPools(itr->guid).size(),
                                itr->chance, active);
            else
                PSendSysMessage(LANG_POOL_CHANCE_POOL_LIST_CONSOLE, itr->guid,
                                itr_template.description.c_str(), itr_template.IsAutoSpawn() ? 1 : 0, itr_template.MaxLimit,
                                sPoolMgr.GetPoolCreatures(itr->guid).size(), sPoolMgr.GetPoolGameObjects(itr->guid).size(), sPoolMgr.GetPoolPools(itr->guid).size(),
                                itr->chance, active);
        }
    }

    PoolObjectList const& poolPoolsEq = poolPools.GetEqualChanced();
    if (!poolPoolsEq.empty())
    {
        SendSysMessage(LANG_POOL_POOL_LIST_HEADER);
        for (PoolObjectList::const_iterator itr = poolPoolsEq.begin(); itr != poolPoolsEq.end(); ++itr)
        {
            PoolTemplateData const& itr_template = sPoolMgr.GetPoolTemplate(itr->guid);
            char const* active = poolSpawns && poolSpawns->find(itr->guid) != poolSpawns->end() ? active_str.c_str() : "";
            if (m_session)
                PSendSysMessage(LANG_POOL_POOL_LIST_CHAT, itr->guid,
                                itr->guid, itr_template.description.c_str(), itr_template.IsAutoSpawn() ? 1 : 0, itr_template.MaxLimit,
                                sPoolMgr.GetPoolCreatures(itr->guid).size(), sPoolMgr.GetPoolGameObjects(itr->guid).size(), sPoolMgr.GetPoolPools(itr->guid).size(),
                                active);
            else
                PSendSysMessage(LANG_POOL_POOL_LIST_CONSOLE, itr->guid,
                                itr_template.description.c_str(), itr_template.IsAutoSpawn() ? 1 : 0, itr_template.MaxLimit,
                                sPoolMgr.GetPoolCreatures(itr->guid).size(), sPoolMgr.GetPoolGameObjects(itr->guid).size(), sPoolMgr.GetPoolPools(itr->guid).size(),
                                active);
        }
    }
    return true;
}

/// Triggering corpses expire check in world
bool ChatHandler::HandleServerCorpsesCommand(char* /*args*/)
{
    sObjectAccessor.RemoveOldCorpses();
    return true;
}

bool ChatHandler::HandleRepairitemsCommand(char* args)
{
    Player* target;
    if (!ExtractPlayerTarget(&args, &target))
        return false;

    // check online security
    if (HasLowerSecurity(target))
        return false;

    // Repair items
    target->DurabilityRepairAll(false, 0);

    PSendSysMessage(LANG_YOU_REPAIR_ITEMS, GetNameLink(target).c_str());
    if (needReportToTarget(target))
        ChatHandler(target).PSendSysMessage(LANG_YOUR_ITEMS_REPAIRED, GetNameLink().c_str());
    return true;
}

bool ChatHandler::HandleWaterwalkCommand(char* args)
{
    bool value;
    if (!ExtractOnOff(&args, value))
    {
        SendSysMessage(LANG_USE_BOL);
        SetSentErrorMessage(true);
        return false;
    }

    Player *player = getSelectedPlayer();

    if (!player)
    {
        PSendSysMessage(LANG_NO_CHAR_SELECTED);
        SetSentErrorMessage(true);
        return false;
    }

    // check online security
    if (HasLowerSecurity(player))
        return false;

    if (value)
        player->SetMovement(MOVE_WATER_WALK);               // ON
    else
        player->SetMovement(MOVE_LAND_WALK);                // OFF

    PSendSysMessage(LANG_YOU_SET_WATERWALK, args, GetNameLink(player).c_str());
    if (needReportToTarget(player))
        ChatHandler(player).PSendSysMessage(LANG_YOUR_WATERWALK_SET, args, GetNameLink().c_str());
    return true;
}

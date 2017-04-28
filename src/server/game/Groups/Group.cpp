/*
* Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
* Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
*
* This program is free software; you can redistribute it and/or modify it
* under the terms of the GNU General Public License as published by the
* Free Software Foundation; either version 2 of the License, or (at your
* option) any later version.
*
* This program is distributed in the hope that it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License along
* with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "Common.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Player.h"
#include "World.h"
#include "ObjectMgr.h"
#include "GroupMgr.h"
#include "Group.h"
#include "Formulas.h"
#include "ObjectAccessor.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "MapManager.h"
#include "InstanceSaveMgr.h"
#include "MapInstanced.h"
#include "Util.h"
#include "LFGMgr.h"
#include "UpdateFieldFlags.h"
#include "RaidEncountersMgr.h"

Roll::Roll(uint64 _guid, LootItem const& li) : itemGUID(_guid), itemid(li.itemid),
    itemRandomPropId(li.randomPropertyId), itemRandomSuffix(li.randomSuffix), itemCount(li.count),
    totalPlayersRolling(0), totalNeed(0), totalGreed(0), totalPass(0), itemSlot(0),
    rollVoteMask(ROLL_ALL_TYPE_NO_DISENCHANT), lootedGUID(0LL)
{
}

Roll::~Roll()
{
}

void Roll::setLoot(Loot* pLoot)
{
    link(pLoot, this);
}

Loot* Roll::getLoot()
{
    return getTarget();
}

Group::Group() : m_leaderGuid(0), m_leaderName(""), m_groupType(GROUPTYPE_NORMAL),
    m_dungeonDifficulty(REGULAR_DIFFICULTY), m_raidDifficulty(MAN10_DIFFICULTY),
    m_bgGroup(NULL), m_bfGroup(NULL), m_lootMethod(FREE_FOR_ALL), m_lootThreshold(ITEM_QUALITY_UNCOMMON), m_looterGuid(0),
    m_subGroupsCounts(NULL), m_guid(0), m_counter(0), m_maxEnchantingLevel(0), m_dbStoreId(0), m_readyCheckCount(0), m_readyCheck(false), m_membersInInstance(0)
{
    for (uint8 i = 0; i < TARGETICONCOUNT; ++i)
        m_targetIcons[i] = 0;

    uint32 lowguid = sGroupMgr->GenerateGroupId();
    m_guid = MAKE_NEW_GUID(lowguid, 0, HighGuid::Group);

    m_RaidMarkers.resize(eRaidMarkersMisc::MaxRaidMarkers);
}

Group::~Group()
{
    if (m_bgGroup)
    {
        sLog->outDebug(LOG_FILTER_BATTLEGROUND, "Group::~Group: battleground group being deleted.");
        if (m_bgGroup->GetBgRaid(ALLIANCE) == this) m_bgGroup->SetBgRaid(ALLIANCE, NULL);
        else if (m_bgGroup->GetBgRaid(HORDE) == this) m_bgGroup->SetBgRaid(HORDE, NULL);
        else sLog->outError(LOG_FILTER_GENERAL, "Group::~Group: battleground group is not linked to the correct battleground.");
    }
    Rolls::iterator itr;
    while (!RollId.empty())
    {
        itr = RollId.begin();
        Roll *r = *itr;
        RollId.erase(itr);
        delete(r);
    }

    // it is undefined whether objectmgr (which stores the groups) or instancesavemgr
    // will be unloaded first so we must be prepared for both cases
    // this may unload some instance saves
    for (uint8 i = 0; i < MAX_DIFFICULTY; ++i)
        for (BoundInstancesMap::iterator itr2 = m_boundInstances[i].begin(); itr2 != m_boundInstances[i].end(); ++itr2)
            itr2->second.save->RemoveGroup(this);

    // Sub group counters clean up
    delete[] m_subGroupsCounts;
}

bool Group::Create(Player* leader)
{
    uint64 leaderGuid = leader->GetGUID();

    m_leaderGuid = leaderGuid;
    m_leaderName = leader->GetName();

    leader->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_GROUP_LEADER);

    m_groupType  = (isBGGroup() || isBFGroup()) ? GROUPTYPE_BGRAID : GROUPTYPE_NORMAL;

    if (m_groupType & GROUPTYPE_RAID)
        _initRaidSubGroupsCounter();

    m_lootMethod = GROUP_LOOT;
    m_lootThreshold = ITEM_QUALITY_UNCOMMON;
    m_looterGuid = leaderGuid;

    m_dungeonDifficulty = REGULAR_DIFFICULTY;
    m_raidDifficulty = MAN10_DIFFICULTY;

    if (!isBGGroup() && !isBFGroup())
    {
        m_dungeonDifficulty = leader->GetDungeonDifficulty();
        m_raidDifficulty = isLFGGroup() ? RAID_TOOL_DIFFICULTY : leader->GetRaidDifficulty();

        m_dbStoreId = sGroupMgr->GenerateNewGroupDbStoreId();

        sGroupMgr->RegisterGroupDbStoreId(m_dbStoreId, this);

        // Store group in database
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_GROUP);

        uint8 index = 0;

        stmt->setUInt32(index++, m_dbStoreId);
        stmt->setUInt32(index++, GUID_LOPART(m_leaderGuid));
        stmt->setUInt8(index++, uint8(m_lootMethod));
        stmt->setUInt32(index++, GUID_LOPART(m_looterGuid));
        stmt->setUInt8(index++, uint8(m_lootThreshold));
        stmt->setUInt32(index++, uint32(m_targetIcons[0]));
        stmt->setUInt32(index++, uint32(m_targetIcons[1]));
        stmt->setUInt32(index++, uint32(m_targetIcons[2]));
        stmt->setUInt32(index++, uint32(m_targetIcons[3]));
        stmt->setUInt32(index++, uint32(m_targetIcons[4]));
        stmt->setUInt32(index++, uint32(m_targetIcons[5]));
        stmt->setUInt32(index++, uint32(m_targetIcons[6]));
        stmt->setUInt32(index++, uint32(m_targetIcons[7]));
        stmt->setUInt8(index++, uint8(m_groupType));
        stmt->setUInt32(index++, uint8(m_dungeonDifficulty));
        stmt->setUInt32(index++, uint8(m_raidDifficulty));

        CharacterDatabase.Execute(stmt);

        ASSERT(AddMember(leader)); // If the leader can't be added to a new group because it appears full, something is clearly wrong.

        Player::ConvertInstancesToGroup(leader, this, false);
    }
    else if (!AddMember(leader))
        return false;

    return true;
}

void Group::LoadGroupFromDB(Field* fields)
{
    m_dbStoreId = fields[15].GetUInt32();
    m_guid = MAKE_NEW_GUID(sGroupMgr->GenerateGroupId(), 0, HighGuid::Group);
    m_leaderGuid = MAKE_NEW_GUID(fields[0].GetUInt32(), 0, HighGuid::Player);
    m_leaderName = fields[18].GetString();

    if (m_leaderName.empty())
    {
        // group leader not exist
        if (!sObjectMgr->GetPlayerNameByGUID(fields[0].GetUInt32(), m_leaderName))
            return;
    }

    m_lootMethod = LootMethod(fields[1].GetUInt8());
    m_looterGuid = MAKE_NEW_GUID(fields[2].GetUInt32(), 0, HighGuid::Player);
    m_lootThreshold = ItemQualities(fields[3].GetUInt8());

    for (uint8 i = 0; i < TARGETICONCOUNT; ++i)
        m_targetIcons[i] = fields[4+i].GetUInt32();

    m_groupType  = GroupType(fields[12].GetUInt8());
    if (m_groupType & GROUPTYPE_RAID)
        _initRaidSubGroupsCounter();

    uint32 diff = fields[13].GetUInt8();
    if (diff >= MAX_DUNGEON_DIFFICULTY)
        m_dungeonDifficulty = REGULAR_DIFFICULTY;
    else
        m_dungeonDifficulty = Difficulty(diff);

    uint32 r_diff = fields[14].GetUInt8();
    if (r_diff >= MAX_RAID_DIFFICULTY)
        m_raidDifficulty = MAN10_DIFFICULTY;
    else
        m_raidDifficulty = Difficulty(r_diff);

    if (m_groupType & GROUPTYPE_LFG)
        sLFGMgr->_LoadFromDB(fields, GetGUID());
}

void Group::LoadMemberFromDB(uint32 guidLow, uint8 memberFlags, uint8 subgroup, uint8 roles, std::string name)
{
    MemberSlot member;
    member.guid = MAKE_NEW_GUID(guidLow, 0, HighGuid::Player);
    member.name = name;

    if (member.name.empty())
    {
        // skip non-existed member
        if (!sObjectMgr->GetPlayerNameByGUID(member.guid, member.name))
        {
            PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GROUP_MEMBER);
            stmt->setUInt32(0, guidLow);
            CharacterDatabase.Execute(stmt);
            return;
        }
    }

    member.group = subgroup;
    member.flags = memberFlags;
    member.roles = roles;

    m_memberSlots.push_back(member);

    SubGroupCounterIncrease(subgroup);

    if (isLFGGroup())
    {
        LfgDungeonSet Dungeons;
        Dungeons.insert(sLFGMgr->GetDungeon(GetGUID()));
        sLFGMgr->SetSelectedDungeons(member.guid, Dungeons);
        sLFGMgr->SetState(member.guid, sLFGMgr->GetState(GetGUID()));
    }
}

void Group::ChangeFlagEveryoneAssistant(bool apply)
{
    if (apply)
        m_groupType = GroupType(m_groupType | GROUPTYPE_EVERYONE_IS_ASSISTANT);
    else
        m_groupType = GroupType(m_groupType &~ GROUPTYPE_EVERYONE_IS_ASSISTANT);

    this->SendUpdate();
}

void Group::ConvertToLFG()
{
    m_groupType = GroupType(m_groupType | GROUPTYPE_LFG | GROUPTYPE_UNK1);
    m_lootMethod = NEED_BEFORE_GREED;
    if (!isBGGroup() && !isBFGroup())
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_TYPE);

        stmt->setUInt8(0, uint8(m_groupType));
        stmt->setUInt32(1, m_dbStoreId);

        CharacterDatabase.Execute(stmt);
    }

    SendUpdate();
}

void Group::ConvertToRaid()
{
    m_groupType = GroupType(m_groupType | GROUPTYPE_RAID);

    _initRaidSubGroupsCounter();

    if (!isBGGroup() && !isBFGroup())
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_TYPE);

        stmt->setUInt8(0, uint8(m_groupType));
        stmt->setUInt32(1, m_dbStoreId);

        CharacterDatabase.Execute(stmt);
    }

    SendUpdate();

    // update quest related GO states (quest activity dependent from raid membership)
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
        if (Player* player = ObjectAccessor::FindPlayer(citr->guid))
            player->UpdateForQuestWorldObjects();
}

void Group::ConvertToGroup()
{
    if (m_memberSlots.size() > 5)
        return; // What message error should we send?

    m_groupType = GroupType(GROUPTYPE_NORMAL);

    if (m_subGroupsCounts)
    {
        delete[] m_subGroupsCounts;
        m_subGroupsCounts = NULL;
    }

    if (!isBGGroup() && !isBFGroup())
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_TYPE);

        stmt->setUInt8(0, uint8(m_groupType));
        stmt->setUInt32(1, m_dbStoreId);

        CharacterDatabase.Execute(stmt);
    }

    SendUpdate();

    // update quest related GO states (quest activity dependent from raid membership)
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
        if (Player* player = ObjectAccessor::FindPlayer(citr->guid))
            player->UpdateForQuestWorldObjects();
}

bool Group::AddInvite(Player* player)
{
    if (!player || player->GetGroupInvite())
        return false;
    Group* group = player->GetGroup();
    if (group && (group->isBGGroup() || group->isBFGroup()))
        group = player->GetOriginalGroup();
    if (group)
        return false;

    RemoveInvite(player);

    m_inviteesLock.acquire();
    m_invitees.insert(player->GetGUID());
    m_inviteesLock.release();

    player->SetGroupInvite(this->GetGUID());

    sScriptMgr->OnGroupInviteMember(this, player->GetGUID());

    return true;
}

bool Group::AddLeaderInvite(Player* player)
{
    if (!AddInvite(player))
        return false;

    m_leaderGuid = player->GetGUID();
    m_leaderName = player->GetName();
    player->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_GROUP_LEADER);
    return true;
}

void Group::RemoveInvite(Player* player)
{
    if (!player)
        return;

    // Critical section
    {
        TRINITY_WRITE_GUARD(ACE_RW_Thread_Mutex, m_invitesLock);

        m_invitees.erase(player->GetGUID());
        player->SetGroupInvite(0);
    }
}

void Group::RemoveAllInvites()
{
    m_inviteesLock.acquire();
    for (InvitesList::iterator itr=m_invitees.begin(); itr != m_invitees.end(); ++itr)
        if (Player* plr = sObjectAccessor->FindPlayer(*itr))
            plr->SetGroupInvite(0);

    m_invitees.clear();
    m_inviteesLock.release();
}

Player* Group::GetInvited(uint64 guid) const
{
    for (InvitesList::const_iterator itr = m_invitees.begin(); itr != m_invitees.end(); ++itr)
    {
        if ((*itr) == guid)
            return sObjectAccessor->FindPlayer(*itr);
    }
    return NULL;
}

Player* Group::GetInvited(const std::string& name) const
{
    m_inviteesLock.acquire();
    for (InvitesList::const_iterator itr = m_invitees.begin(); itr != m_invitees.end(); ++itr)
    {
        Player* plr = sObjectAccessor->FindPlayer(*itr);
        if (!plr)
            continue;

        if (plr->GetName() == name)
        {
            m_inviteesLock.release();
            return plr;
        }
    }
    m_inviteesLock.release();
    return NULL;
}

bool Group::AddMember(Player* player)
{
    // Get first not-full group
    uint8 subGroup = 0;
    if (m_subGroupsCounts)
    {
        bool groupFound = false;
        for (; subGroup < MAX_RAID_SUBGROUPS; ++subGroup)
        {
            if (m_subGroupsCounts[subGroup] < MAXGROUPSIZE)
            {
                groupFound = true;
                break;
            }
        }
        // We are raid group and no one slot is free
        if (!groupFound)
            return false;
    }

    MemberSlot member;
    member.guid      = player->GetGUID();
    member.name      = player->GetName();
    member.group     = subGroup;
    member.flags     = 0;
    member.roles     = 0;
    m_memberSlots.push_back(member);

    SubGroupCounterIncrease(subGroup);

    if (player)
    {
        player->SetGroupInvite(0);
        if (player->GetGroup() && (isBGGroup() || isBFGroup())) // if player is in group and he is being added to BG raid group, then call SetBattlegroundRaid()
            player->SetBattlegroundOrBattlefieldRaid(this, subGroup);
        else if (player->GetGroup()) //if player is in bg raid and we are adding him to normal group, then call SetOriginalGroup()
            player->SetOriginalGroup(this, subGroup);
        else //if player is not in group, then call set group
            player->SetGroup(this, subGroup);

        // if the same group invites the player back, cancel the homebind timer
        InstanceGroupBind* bind = GetBoundInstance(player);
        if (bind && bind->save->GetInstanceId() == player->GetInstanceId())
            player->m_InstanceValid = true;
    }

    if (!isRaidGroup())                                      // reset targetIcons for non-raid-groups
    {
        for (uint8 i = 0; i < TARGETICONCOUNT; ++i)
            m_targetIcons[i] = 0;
    }

    // insert into the table if we're not a battleground group
    if (!isBGGroup() && !isBFGroup())
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_INS_GROUP_MEMBER);

        stmt->setUInt32(0, m_dbStoreId);
        stmt->setUInt32(1, GUID_LOPART(member.guid));
        stmt->setUInt8(2, member.flags);
        stmt->setUInt8(3, member.group);
        stmt->setUInt8(4, member.roles);

        CharacterDatabase.Execute(stmt);

    }

    SendUpdate();
    sScriptMgr->OnGroupAddMember(this, player->GetGUID());

    if (player)
    {
        if (!IsLeader(player->GetGUID()) && !isBGGroup() && !isBFGroup())
        {
            // reset the new member's instances, unless he is currently in one of them
            // including raid/heroic instances that they are not permanently bound to!
            player->ResetInstances(INSTANCE_RESET_GROUP_JOIN, false);
            player->ResetInstances(INSTANCE_RESET_GROUP_JOIN, true);

            if (player->getLevel() >= LEVELREQUIREMENT_HEROIC)
            {
                if (player->GetDungeonDifficulty() != GetDungeonDifficulty())
                {
                    player->SetDungeonDifficulty(GetDungeonDifficulty());
                    player->SendDungeonDifficulty(true);
                }
                if (player->GetRaidDifficulty() != GetRaidDifficulty())
                {
                    player->SetRaidDifficulty(GetRaidDifficulty());
                    player->SendRaidDifficulty(true);
                }
            }
        }
        player->SetGroupUpdateFlag(GROUP_UPDATE_FULL);
        UpdatePlayerOutOfRange(player);

        // quest related GO state dependent from raid membership
        if (isRaidGroup())
            player->UpdateForQuestWorldObjects();

        {
            // Broadcast new player group member fields to rest of the group
            player->SetFieldNotifyFlag(UF_FLAG_PARTY_MEMBER);

            UpdateData groupData(player->GetMapId());
            WorldPacket groupDataPacket;

            // Broadcast group members' fields to player
            for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
            {
                if (itr->getSource() == player)
                    continue;

                if (Player* member = itr->getSource())
                {
                    if (player->HaveAtClient(member))   // must be on the same map, or shit will break
                    {
                        member->SetFieldNotifyFlag(UF_FLAG_PARTY_MEMBER);
                        member->BuildValuesUpdateBlockForPlayer(&groupData, player);
                        member->RemoveFieldNotifyFlag(UF_FLAG_PARTY_MEMBER);
                    }

                    if (member->HaveAtClient(player))
                    {
                        UpdateData newData(player->GetMapId());
                        WorldPacket newDataPacket;
                        player->BuildValuesUpdateBlockForPlayer(&newData, member);
                        if (newData.HasData())
                        {
                            if (newData.BuildPacket(&newDataPacket))
                                member->SendDirectMessage(&newDataPacket);
                        }
                    }
                }
            }

            if (groupData.HasData())
                if (groupData.BuildPacket(&groupDataPacket))
                    player->SendDirectMessage(&groupDataPacket);

            player->RemoveFieldNotifyFlag(UF_FLAG_PARTY_MEMBER);
        }

        if (m_maxEnchantingLevel < player->GetSkillValue(SKILL_ENCHANTING))
            m_maxEnchantingLevel = player->GetSkillValue(SKILL_ENCHANTING);
    }

    return true;
}

bool Group::RemoveMember(uint64 guid, const RemoveMethod &method /*= GROUP_REMOVEMETHOD_DEFAULT*/, uint64 kicker /*= 0*/, const char* reason /*= NULL*/)
{
    BroadcastGroupUpdate();

    sScriptMgr->OnGroupRemoveMember(this, guid, method, kicker, reason);

    // LFG group vote kick handled in scripts
    if (isLFGGroup() && method == GROUP_REMOVEMETHOD_KICK)
        return m_memberSlots.size();

    // remove member and change leader (if need) only if strong more 2 members _before_ member remove (BG/BF allow 1 member group)
    if (GetMembersCount() > ((isBGGroup() || isLFGGroup() || isBFGroup()) ? 1u : 2u))
    {
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (player)
        {
            // Battleground group handling
            if (isBGGroup() || isBFGroup())
                player->RemoveFromBattlegroundOrBattlefieldRaid();
            else
                // Regular group
            {
                if (player->GetOriginalGroup() == this)
                    player->SetOriginalGroup(NULL);
                else
                    player->SetGroup(NULL);

                // quest related GO state dependent from raid membership
                player->UpdateForQuestWorldObjects();
            }

            if (method == GROUP_REMOVEMETHOD_KICK)
            {
                WorldPacket data(SMSG_GROUP_UNINVITE, 0);
                player->GetSession()->SendPacket(&data);
            }

            Group::SendUpdatePlayerAtLeave(player, this, m_counter++);

            _homebindIfInstance(player);
        }

        // Remove player from group in DB
        if (!isBGGroup() && !isBFGroup())
        {
            PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GROUP_MEMBER);
            stmt->setUInt32(0, GUID_LOPART(guid));
            CharacterDatabase.Execute(stmt);
            DelinkMember(guid);
        }

        // Reevaluate group enchanter if the leaving player had enchanting skill or the player is offline
        if ((player && player->GetSkillValue(SKILL_ENCHANTING)) || !player)
            ResetMaxEnchantingLevel();

        // Remove player from loot rolls
        for (Rolls::iterator it = RollId.begin(); it != RollId.end(); ++it)
        {
            Roll* roll = *it;
            Roll::PlayerVote::iterator itr2 = roll->playerVote.find(guid);
            if (itr2 == roll->playerVote.end())
                continue;

            if (itr2->second == GREED || itr2->second == DISENCHANT)
                --roll->totalGreed;
            else if (itr2->second == NEED)
                --roll->totalNeed;
            else if (itr2->second == PASS)
                --roll->totalPass;

            if (itr2->second != NOT_VALID)
                --roll->totalPlayersRolling;

            roll->playerVote.erase(itr2);

            CountRollVote(guid, roll->itemGUID, MAX_ROLL_TYPE);
        }

        // Update subgroups
        member_witerator slot = _getMemberWSlot(guid);
        if (slot != m_memberSlots.end())
        {
            SubGroupCounterDecrease(slot->group);
            m_memberSlots.erase(slot);
        }

        // Pick new leader if necessary
        if (m_leaderGuid == guid)
        {
            for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
            {
                if (ObjectAccessor::FindPlayer(itr->guid))
                {
                    ChangeLeader(itr->guid);
                    break;
                }
            }
        }

        SendUpdate();

        if (isLFGGroup() && GetMembersCount() == 1)
        {
            Player* Leader = ObjectAccessor::FindPlayer(GetLeaderGUID());
            LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(sLFGMgr->GetDungeon(GetGUID()));
            if ((Leader && dungeon && Leader->isAlive() && Leader->GetMapId() != uint32(dungeon->map)) || !dungeon)
            {
                Disband();
                return false;
            }
        }

        if (m_memberMgr.getSize() < ((isLFGGroup() || isBGGroup()) ? 1u : 2u))
            Disband();

        return true;
    }
    // If group size before player removal <= 2 then disband it
    else
    {
        Disband();
        return false;
    }
}

void Group::ChangeLeader(uint64 newLeaderGuid)
{
    member_witerator slot = _getMemberWSlot(newLeaderGuid);

    if (slot == m_memberSlots.end())
        return;

    Player* newLeader = ObjectAccessor::FindPlayer(slot->guid);

    // Don't allow switching leader to offline players
    if (!newLeader)
        return;

    sScriptMgr->OnGroupChangeLeader(this, m_leaderGuid, newLeaderGuid);

    if (!isBGGroup() && !isBFGroup())
    {
        SQLTransaction trans = CharacterDatabase.BeginTransaction();

        // Remove the groups permanent instance bindings
        for (uint8 i = 0; i < MAX_DIFFICULTY; ++i)
        {
            for (BoundInstancesMap::iterator itr = m_boundInstances[i].begin(); itr != m_boundInstances[i].end();)
            {
                // Do not unbind saves of instances that already have map created (a newLeader entered)
                // forcing a new instance with another leader requires group disbanding (confirmed on retail)
                if (itr->second.perm && !sMapMgr->FindMap(itr->first, itr->second.save->GetInstanceId()))
                {
                    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GROUP_INSTANCE_PERM_BINDING);
                    stmt->setUInt32(0, m_dbStoreId);
                    stmt->setUInt32(1, itr->second.save->GetInstanceId());
                    trans->Append(stmt);

                    itr->second.save->RemoveGroup(this);
                    m_boundInstances[i].erase(itr++);
                }
                else
                    ++itr;
            }
        }

        // Copy the permanent binds from the new leader to the group
        Player::ConvertInstancesToGroup(newLeader, this, true);

        // Update the group leader
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_LEADER);

        stmt->setUInt32(0, newLeader->GetGUIDLow());
        stmt->setUInt32(1, m_dbStoreId);

        trans->Append(stmt);

        CharacterDatabase.CommitTransaction(trans);
    }

    Player* oldLeader = ObjectAccessor::FindPlayer(m_leaderGuid);

    if (oldLeader)
        oldLeader->RemoveFlag(PLAYER_FLAGS, PLAYER_FLAGS_GROUP_LEADER);

    m_leaderGuid = newLeader->GetGUID();
    m_leaderName = newLeader->GetName();

    newLeader->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_GROUP_LEADER);

    ToggleGroupMemberFlag(slot, MEMBER_FLAG_ASSISTANT, false);

    uint8 leaderNameLen = slot->name.size();
    
    WorldPacket data(SMSG_GROUP_SET_LEADER, 1 + 1 + leaderNameLen);
    data.WriteBits(slot->name.size(), 6);
    data << uint8(0);
    data.append(slot->name.c_str(), slot->name.size());

    BroadcastPacket(&data, true);
}

void Group::Disband(bool hideDestroy /* = false */)
{
    sScriptMgr->OnGroupDisband(this);

    Player* player;
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        player = ObjectAccessor::FindPlayer(citr->guid);
        if (!player)
            continue;

        //we cannot call _removeMember because it would invalidate member iterator
        //if we are removing player from battleground raid
        if (isBGGroup() || isBFGroup())
            player->RemoveFromBattlegroundOrBattlefieldRaid();
        else
        {
            //we can remove player who is in battleground from his original group
            if (player->GetOriginalGroup() == this)
                player->SetOriginalGroup(NULL);
            else
                player->SetGroup(NULL);
        }

        // quest related GO state dependent from raid membership
        if (isRaidGroup())
            player->UpdateForQuestWorldObjects();

        if (!player->GetSession())
            continue;

        if (!hideDestroy)
        {
            WorldPacket data(SMSG_GROUP_DESTROYED, 0);
            player->GetSession()->SendPacket(&data);
        }

        //we already removed player from group and in player->GetGroup() is his original group, send update
        if (Group* group = player->GetGroup())
            group->SendUpdate();
        else
            Group::SendUpdatePlayerAtLeave(player, this, m_counter++);

        _homebindIfInstance(player);
    }

    RollId.clear();
    m_memberSlots.clear();

    RemoveAllInvites();

    RemoveAllRaidMarkers();

    if (!isBGGroup() && !isBFGroup())
    {
        SQLTransaction trans = CharacterDatabase.BeginTransaction();

        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GROUP);
        stmt->setUInt32(0, m_dbStoreId);
        trans->Append(stmt);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GROUP_MEMBER_ALL);
        stmt->setUInt32(0, m_dbStoreId);
        trans->Append(stmt);

        CharacterDatabase.CommitTransaction(trans);

        ResetInstances(INSTANCE_RESET_GROUP_DISBAND, false, NULL);
        ResetInstances(INSTANCE_RESET_GROUP_DISBAND, true, NULL);

        stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_LFG_DATA);
        stmt->setUInt32(0, m_dbStoreId);
        CharacterDatabase.Execute(stmt);

        sGroupMgr->FreeGroupDbStoreId(this);
    }

    sGroupMgr->RemoveGroup(this);

    delete this;
}

/*********************************************************/
/***                   LOOT SYSTEM                     ***/
/*********************************************************/

void Group::SendLootStartRoll(uint32 countDown, uint32 mapid, const Roll& r)
{
    ObjectGuid guid = MAKE_NEW_GUID(GUID_LOPART(r.lootedGUID), 0, HighGuid::Loot);

    WorldPacket data(SMSG_LOOT_START_ROLL, (8+4+4+4+4+4+4+1));
    data << uint8(r.rollVoteMask);
    
    /* Item modifiers, see BuildItemModifiers */
    data << uint32(0);

    data << uint32(r.itemCount);
    data << uint32(0);                          // unk
    data << uint32(mapid);
    data << uint32(r.itemid);
    data << uint8(r.totalPlayersRolling);
    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(r.itemid))
        data << uint32(proto->DisplayInfoID);
    else
        data << uint32(0);
    data << uint32(0);                          // unk
    data << uint32(countDown);
    data.WriteBit(guid[0]);
    data.WriteBit(!false);                      // unk inverse
    data.WriteBits(0, 3);                       // unk
    data.WriteBit(guid[3]);
    data.WriteBit(false);                       // unk
    data.WriteBit(guid[5]);
    data.WriteBits(0, 2);
    data.WriteBit(r.itemSlot == 0);
    data.WriteBit(guid[4]);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[6]);
    data.WriteBit(guid[2]);

    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(guid[0]);
    // if unk inverse bit
    // data << uint8
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[3]);
    if (r.itemSlot != 0)
        data << uint8(r.itemSlot);

    for (Roll::PlayerVote::const_iterator itr=r.playerVote.begin(); itr != r.playerVote.end(); ++itr)
    {
        Player* p = ObjectAccessor::FindPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second == NOT_EMITED_YET)
            p->GetSession()->SendPacket(&data);
    }
}

void Group::SendLootStartRollToPlayer(uint32 countDown, uint32 mapId, Player* p, bool canNeed, Roll const& r)
{
    if (!p || !p->GetSession())
        return;

    WorldPacket data(SMSG_LOOT_START_ROLL, (8 + 4 + 4 + 4 + 4 + 4 + 4 + 1));
    ObjectGuid guid = MAKE_NEW_GUID(GUID_LOPART(r.lootedGUID), 0, HighGuid::Loot);

    data << uint8(r.rollVoteMask);
    
    /* Item modifiers, see BuildItemModifiers */
    data << uint32(0);

    data << uint32(r.itemCount);
    data << uint32(0);                          // unk
    data << uint32(mapId);
    data << uint32(r.itemid);
    data << uint8(r.totalPlayersRolling);
    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(r.itemid))
        data << uint32(proto->DisplayInfoID);
    else
        data << uint32(0);
    data << uint32(0);                          // unk
    data << uint32(countDown);
    data.WriteBit(guid[0]);
    data.WriteBit(!false);                      // unk inverse
    data.WriteBits(0, 3);                       // unk
    data.WriteBit(guid[3]);
    data.WriteBit(false);                       // unk
    data.WriteBit(guid[5]);
    data.WriteBits(0, 2);
    data.WriteBit(r.itemSlot == 0);
    data.WriteBit(guid[4]);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[6]);
    data.WriteBit(guid[2]);

    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(guid[0]);
    // if unk inverse bit
    // data << uint8
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[3]);
    if (r.itemSlot != 0)
        data << uint8(r.itemSlot);
    
    p->GetSession()->SendPacket(&data);
}

void Group::SendLootRoll(uint64 sourceGuid, uint64 targetGuid, uint8 rollNumber, uint8 rollType, Roll const& roll)
{
    WorldPacket data(SMSG_LOOT_ROLL, (8+4+8+4+4+4+1+1+1));
    
    ObjectGuid itemGuid = MAKE_NEW_GUID(GUID_LOPART(roll.lootedGUID), 0, HighGuid::Loot);
    ObjectGuid playerGuid = targetGuid;

    data.WriteBits(0, 2);
    data.WriteBit(playerGuid[7]);
    data.WriteBit(playerGuid[6]);
    data.WriteBits(5, 3);
    data.WriteBit(itemGuid[5]);
    data.WriteBit(itemGuid[0]);
    data.WriteBit(playerGuid[0]);
    data.WriteBit(playerGuid[1]);
    data.WriteBit(itemGuid[1]);
    data.WriteBit(playerGuid[2]);
    data.WriteBit(true);                        // unkbit1 inverse
    data.WriteBit(itemGuid[7]);
    data.WriteBit(itemGuid[4]);
    data.WriteBit(false);                       // unkbit2
    data.WriteBit(itemGuid[6]);
    data.WriteBit(itemGuid[3]);
    data.WriteBit(itemGuid[2]);
    data.WriteBit(roll.itemSlot == 0);          // has loot slot
    data.WriteBit(playerGuid[5]);
    data.WriteBit(playerGuid[3]);
    data.WriteBit(playerGuid[4]);
    data.WriteBit(false);                        // unkbit4

    if (roll.itemSlot != 0)
        data << uint8(roll.itemSlot);

    data << uint32(roll.itemid);
    data.WriteByteSeq(playerGuid[3]);
    data.WriteByteSeq(playerGuid[4]);
    data.WriteByteSeq(itemGuid[0]);
    data << uint32(roll.itemCount);

    /* Item modifiers, see BuildItemModifiers */
    data << uint32(0);

    data.WriteByteSeq(itemGuid[2]);
    data.WriteByteSeq(itemGuid[3]);
    //if (!unkbit1)
        //data << uint8;
    data << uint32(0);                          // unk
    data.WriteByteSeq(itemGuid[6]);
    data << uint32(rollNumber);
    data.WriteByteSeq(itemGuid[7]);
    data << uint32(0);
    data.WriteByteSeq(playerGuid[1]);
    data.WriteByteSeq(playerGuid[0]);
    data.WriteByteSeq(playerGuid[2]);
    data.WriteByteSeq(playerGuid[6]);
    data.WriteByteSeq(itemGuid[4]);
    data.WriteByteSeq(playerGuid[7]);
    data.WriteByteSeq(itemGuid[1]);
    data.WriteByteSeq(playerGuid[5]);
    data << uint8(rollType);                 
    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(roll.itemid))
        data << uint32(proto->DisplayInfoID);
    else
        data << uint32(0);
    data.WriteByteSeq(itemGuid[5]);

    for (Roll::PlayerVote::const_iterator itr = roll.playerVote.begin(); itr != roll.playerVote.end(); ++itr)
    {
        Player* p = ObjectAccessor::FindPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != NOT_VALID)
            p->GetSession()->SendPacket(&data);
    }
}

void Group::SendLootRollWon(uint64 sourceGuid, uint64 targetGuid, uint8 rollNumber, uint8 rollType, Roll const& roll)
{
    WorldPacket data(SMSG_LOOT_ROLL_WON, (8 + 4 + 4 + 4 + 4 + 8 + 1 + 1));
    ObjectGuid guid = targetGuid;
    ObjectGuid target = MAKE_NEW_GUID(GUID_LOPART(roll.lootedGUID), 0, HighGuid::Loot);

    data.WriteBit(guid[7]);
    data.WriteBit(target[6]);
    data.WriteBits(0, 2); //Type
    data.WriteBits(5, 3); //UIType
    data.WriteBit(target[2]);
    data.WriteBit(target[1]);
    data.WriteBit(guid[1]);
    data.WriteBit(!false); // LootItemType
    data.WriteBit(target[0]);
    data.WriteBit(guid[3]);
    data.WriteBit(guid[0]);
    data.WriteBit(target[5]);
    data.WriteBit(roll.itemSlot == 0); // LootListID
    data.WriteBit(target[7]);
    data.WriteBit(guid[6]);
    data.WriteBit(false); // CanTradeToTapList
    data.WriteBit(guid[2]);
    data.WriteBit(target[3]);
    data.WriteBit(target[4]);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[4]);

    data.WriteByteSeq(target[3]);
    data << uint32(roll.itemid);
    data << uint8(rollType);
    data.WriteByteSeq(target[6]);
    if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(roll.itemid))
        data << uint32(proto->DisplayInfoID);
    else
        data << uint32(0);
    if (roll.itemSlot != 0)
        data << uint8(roll.itemSlot); 

    /* Item modifiers, see BuildItemModifiers */
    data << uint32(0);

    data.WriteByteSeq(target[1]);
    data << uint32(roll.itemRandomSuffix);
    data << uint32(rollNumber);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(target[4]);
    // if unk inverse bit
    // data << uint8
    data << uint32(roll.itemCount);
    data.WriteByteSeq(target[7]);
    data.WriteByteSeq(target[0]);
    data << int32(roll.itemRandomPropId);
    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(target[5]);
    data.WriteByteSeq(guid[0]);
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(target[2]);
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[3]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[4]);

    for (Roll::PlayerVote::const_iterator itr = roll.playerVote.begin(); itr != roll.playerVote.end(); ++itr)
    {
        Player* p = ObjectAccessor::FindPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != NOT_VALID)
            p->GetSession()->SendPacket(&data);
    }
}

void Group::SendLootAllPassed(Roll const& roll)
{
    WorldPacket data(SMSG_LOOT_ALL_PASSED, (8+4+4+4+4));
    ObjectGuid guid = MAKE_NEW_GUID(GUID_LOPART(roll.lootedGUID), 0, HighGuid::Loot);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[7]);
    data.WriteBit(!false); // unkbit1 inverse
    data.WriteBit(guid[1]);
    data.WriteBit(guid[0]);
    data.WriteBit(!true); // unkbit2 inverse
    data.WriteBit(guid[4]);
    data.WriteBit(false);
    data.WriteBit(guid[6]);
    data.WriteBits(0, 2);
    data.WriteBits(5, 3);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[3]);

    // There should be item count, item suffix/property id, item slot, item entry, item count...

    data << uint32(roll.itemid);
    data << uint32(roll.itemid);
    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[3]);
    data.WriteByteSeq(guid[1]);
    if (false) // unkbit1 inverse
        data << uint8(1);
    data << uint32(roll.itemid);
    data.WriteByteSeq(guid[0]);
    if (true) // unkbit2 inverse
        data << uint8(1);
    data << uint32(roll.itemid);
    data.WriteByteSeq(guid[7]);
    data << uint32(roll.itemid);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[2]);

    /* Item modifiers, see BuildItemModifiers */
    data << uint32(0);

    for (Roll::PlayerVote::const_iterator itr = roll.playerVote.begin(); itr != roll.playerVote.end(); ++itr)
    {
        Player* player = ObjectAccessor::FindPlayer(itr->first);
        if (!player || !player->GetSession())
            continue;

        if (itr->second != NOT_VALID)
            player->GetSession()->SendPacket(&data);
    }
}

void Group::SendLootRollsComplete(Roll const& roll)
{
    WorldPacket data(SMSG_LOOT_ROLLS_COMPLETE, (8 + 4 + 4 + 4 + 4 + 8 + 1 + 1));
    ObjectGuid guid = MAKE_NEW_GUID(GUID_LOPART(roll.lootedGUID), 0, HighGuid::Loot);

    uint8 bitOrder[8] = { 6, 2, 5, 4, 7, 0, 1, 3 };
    data.WriteBitInOrder(guid, bitOrder);

    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[0]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[5]);
    data << uint8(1/*roll.itemSlot*/); // ?
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[3]);

    for (Roll::PlayerVote::const_iterator itr = roll.playerVote.begin(); itr != roll.playerVote.end(); ++itr)
    {
        Player* p = ObjectAccessor::FindPlayer(itr->first);
        if (!p || !p->GetSession())
            continue;

        if (itr->second != NOT_VALID)
            p->GetSession()->SendPacket(&data);
    }
}

// notify group members which player is the allowed looter for the given creature
void Group::SendLooter(Creature* creature, Player* groupLooter)
{
    ASSERT(creature);

    // groupLooter->GetPackGUID() 40 - 44

    WorldPacket data(SMSG_LOOT_LIST, 2 + 8 + (groupLooter ? 9 : 0));

    ObjectGuid creatureGuid = creature->GetGUID();
    ObjectGuid groupLooterGuid = groupLooter ? groupLooter->GetGUID() : 0;

    data.WriteBit(creatureGuid[1]);
    data.WriteBit(false); // unk guid
    data.WriteBit(creatureGuid[0]);

    if (false) // unk guid
    {
    }

    data.WriteBit(creatureGuid[4]);
    data.WriteBit(creatureGuid[7]);
    data.WriteBit(creatureGuid[3]);
    data.WriteBit(creatureGuid[6]);
    data.WriteBit(groupLooterGuid);

    if (groupLooterGuid)
    {
        uint8 bitOrder[8] = {0, 4, 2, 1, 7, 3, 5, 6};
        data.WriteBitInOrder(groupLooterGuid, bitOrder);
    }

    data.WriteBit(creatureGuid[2]);
    data.WriteBit(creatureGuid[5]);

    if (groupLooterGuid)
    {
        uint8 byteOrder[8] = {1, 7, 6, 2, 4, 5, 0, 3};
        data.WriteBytesSeq(groupLooterGuid, byteOrder);
    }

    uint8 byteOrder[8] = {1, 7, 3, 6, 5, 4, 0, 2};
    data.WriteBytesSeq(creatureGuid, byteOrder);

    /*
    WorldPacket data(SMSG_LOOT_LIST, (8+8));
    data << uint64(creature->GetGUID());
    //data << uint8(0); // unk1

    if (groupLooter)
        data.append(groupLooter->GetPackGUID());
    else
        data << uint64(0);
    data << uint64(0);
*/

    BroadcastPacket(&data, false);
}

void Group::GroupLoot(Loot* loot, WorldObject* pLootedObject)
{
    std::vector<LootItem>::iterator i;
    ItemTemplate const* item;
    uint8 itemSlot = 0;

    for (i = loot->items.begin(); i != loot->items.end(); ++i, ++itemSlot)
    {
        if (i->freeforall)
            continue;

        item = sObjectMgr->GetItemTemplate(i->itemid);
        if (!item)
        {
            //sLog->outDebug(LOG_FILTER_GENERAL, "Group::GroupLoot: missing item prototype for item with id: %d", i->itemid);
            continue;
        }

        //roll for over-threshold item if it's one-player loot
        if (item->Quality >= uint32(m_lootThreshold))
        {
            uint64 newitemGUID = MAKE_NEW_GUID(sObjectMgr->GetGenerator<HighGuid::Item>().Generate(), 0, HighGuid::Item);
            Roll* r = new Roll(newitemGUID, *i);
            r->lootedGUID = pLootedObject->GetGUID();

            //a vector is filled with only near party members
            for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* member = itr->getSource();
                if (!member || !member->GetSession())
                    continue;
                if (i->AllowedForPlayer(member))
                {
                    if (member->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
                    {
                        r->totalPlayersRolling++;

                        if (member->GetPassOnGroupLoot())
                        {
                            r->playerVote[member->GetGUID()] = PASS;
                            r->totalPass++;
                            // can't broadcast the pass now. need to wait until all rolling players are known.
                        }
                        else
                            r->playerVote[member->GetGUID()] = NOT_EMITED_YET;
                    }
                }
            }

            if (r->totalPlayersRolling > 0)
            {
                r->setLoot(loot);
                r->itemSlot = itemSlot;
                if (item->DisenchantID && m_maxEnchantingLevel >= item->RequiredDisenchantSkill)
                    r->rollVoteMask |= ROLL_FLAG_TYPE_DISENCHANT;

                loot->items[itemSlot].is_blocked = true;

                // If there is any "auto pass", broadcast the pass now.
                if (r->totalPass)
                {
                    for (Roll::PlayerVote::const_iterator itr=r->playerVote.begin(); itr != r->playerVote.end(); ++itr)
                    {
                        Player* p = ObjectAccessor::FindPlayer(itr->first);
                        if (!p || !p->GetSession())
                            continue;

                        if (itr->second == PASS)
                            SendLootRoll(newitemGUID, p->GetGUID(), 128, ROLL_PASS, *r);
                    }
                }

                SendLootStartRoll(60000, pLootedObject->GetMapId(), *r);

                RollId.push_back(r);

                if (Creature* creature = pLootedObject->ToCreature())
                {
                    creature->m_groupLootTimer = 60000;
                    creature->lootingGroupLowGUID = GetLowGUID();
                }
                else if (GameObject* go = pLootedObject->ToGameObject())
                {
                    go->m_groupLootTimer = 60000;
                    go->lootingGroupLowGUID = GetLowGUID();
                }
            }
            else
                delete r;
        }
        else
            i->is_underthreshold = true;
    }

    for (i = loot->quest_items.begin(); i != loot->quest_items.end(); ++i, ++itemSlot)
    {
        if (!i->follow_loot_rules)
            continue;

        item = sObjectMgr->GetItemTemplate(i->itemid);
        if (!item)
        {
            //sLog->outDebug(LOG_FILTER_GENERAL, "Group::GroupLoot: missing item prototype for item with id: %d", i->itemid);
            continue;
        }

        uint64 newitemGUID = MAKE_NEW_GUID(sObjectMgr->GetGenerator<HighGuid::Item>().Generate(), 0, HighGuid::Item);
        Roll* r = new Roll(newitemGUID, *i);
        r->lootedGUID = pLootedObject->GetGUID();

        //a vector is filled with only near party members
        for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* member = itr->getSource();
            if (!member || !member->GetSession())
                continue;

            if (i->AllowedForPlayer(member))
            {
                if (member->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
                {
                    r->totalPlayersRolling++;
                    r->playerVote[member->GetGUID()] = NOT_EMITED_YET;
                }
            }
        }

        if (r->totalPlayersRolling > 0)
        {
            r->setLoot(loot);
            r->itemSlot = itemSlot;

            loot->quest_items[itemSlot - loot->items.size()].is_blocked = true;

            SendLootStartRoll(60000, pLootedObject->GetMapId(), *r);

            RollId.push_back(r);

            if (Creature* creature = pLootedObject->ToCreature())
            {
                creature->m_groupLootTimer = 60000;
                creature->lootingGroupLowGUID = GetLowGUID();
            }
            else if (GameObject* go = pLootedObject->ToGameObject())
            {
                go->m_groupLootTimer = 60000;
                go->lootingGroupLowGUID = GetLowGUID();
            }
        }
        else
            delete r;
    }
}

void Group::NeedBeforeGreed(Loot* loot, WorldObject* lootedObject)
{
    ItemTemplate const* item;
    uint8 itemSlot = 0;
    for (std::vector<LootItem>::iterator i = loot->items.begin(); i != loot->items.end(); ++i, ++itemSlot)
    {
        if (i->freeforall)
            continue;

        item = sObjectMgr->GetItemTemplate(i->itemid);

        //roll for over-threshold item if it's one-player loot
        if (item && item->Quality >= uint32(m_lootThreshold))
        {
            uint64 newitemGUID = MAKE_NEW_GUID(sObjectMgr->GetGenerator<HighGuid::Item>().Generate(), 0, HighGuid::Item);
            Roll* r = new Roll(newitemGUID, *i);
            r->lootedGUID = lootedObject->GetGUID();

            for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
            {
                Player* playerToRoll = itr->getSource();
                if (!playerToRoll || !playerToRoll->GetSession())
                    continue;

                bool allowedForPlayer = i->AllowedForPlayer(playerToRoll);
                if (allowedForPlayer && playerToRoll->IsWithinDistInMap(lootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
                {
                    r->totalPlayersRolling++;
                    if (playerToRoll->GetPassOnGroupLoot())
                    {
                        r->playerVote[playerToRoll->GetGUID()] = PASS;
                        r->totalPass++;
                        // can't broadcast the pass now. need to wait until all rolling players are known.
                    }
                    else
                        r->playerVote[playerToRoll->GetGUID()] = NOT_EMITED_YET;
                }
            }

            if (r->totalPlayersRolling > 0)
            {
                r->setLoot(loot);
                r->itemSlot = itemSlot;
                if (item->DisenchantID && m_maxEnchantingLevel >= item->RequiredDisenchantSkill)
                    r->rollVoteMask |= ROLL_FLAG_TYPE_DISENCHANT;

                if (item->Flags2 & ITEM_FLAGS_EXTRA_NEED_ROLL_DISABLED)
                    r->rollVoteMask &= ~ROLL_FLAG_TYPE_NEED;

                loot->items[itemSlot].is_blocked = true;

                //Broadcast Pass and Send Rollstart
                for (Roll::PlayerVote::const_iterator itr = r->playerVote.begin(); itr != r->playerVote.end(); ++itr)
                {
                    Player* p = ObjectAccessor::FindPlayer(itr->first);
                    if (!p || !p->GetSession())
                        continue;

                    if (itr->second == PASS)
                        SendLootRoll(newitemGUID, p->GetGUID(), 128, ROLL_PASS, *r);
                    else
                        SendLootStartRollToPlayer(60000, lootedObject->GetMapId(), p, p->CanRollForItemInLFG(item, lootedObject) == EQUIP_ERR_OK, *r);
                }

                RollId.push_back(r);

                if (Creature* creature = lootedObject->ToCreature())
                {
                    creature->m_groupLootTimer = 60000;
                    creature->lootingGroupLowGUID = GetLowGUID();
                }
                else if (GameObject* go = lootedObject->ToGameObject())
                {
                    go->m_groupLootTimer = 60000;
                    go->lootingGroupLowGUID = GetLowGUID();
                }
            }
            else
                delete r;
        }
        else
            i->is_underthreshold = true;
    }

    for (std::vector<LootItem>::iterator i = loot->quest_items.begin(); i != loot->quest_items.end(); ++i, ++itemSlot)
    {
        if (!i->follow_loot_rules)
            continue;

        item = sObjectMgr->GetItemTemplate(i->itemid);
        uint64 newitemGUID = MAKE_NEW_GUID(sObjectMgr->GetGenerator<HighGuid::Item>().Generate(), 0, HighGuid::Item);
        Roll* r = new Roll(newitemGUID, *i);
        r->lootedGUID = lootedObject->GetGUID();

        for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* playerToRoll = itr->getSource();
            if (!playerToRoll || !playerToRoll->GetSession())
                continue;

            bool allowedForPlayer = i->AllowedForPlayer(playerToRoll);
            if (allowedForPlayer && playerToRoll->IsWithinDistInMap(lootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
            {
                r->totalPlayersRolling++;
                r->playerVote[playerToRoll->GetGUID()] = NOT_EMITED_YET;
            }
        }

        if (r->totalPlayersRolling > 0)
        {
            r->setLoot(loot);
            r->itemSlot = itemSlot;

            loot->quest_items[itemSlot - loot->items.size()].is_blocked = true;

            //Broadcast Pass and Send Rollstart
            for (Roll::PlayerVote::const_iterator itr = r->playerVote.begin(); itr != r->playerVote.end(); ++itr)
            {
                Player* p = ObjectAccessor::FindPlayer(itr->first);
                if (!p || !p->GetSession())
                    continue;

                if (itr->second == PASS)
                    SendLootRoll(newitemGUID, p->GetGUID(), 128, ROLL_PASS, *r);
                else
                    SendLootStartRollToPlayer(60000, lootedObject->GetMapId(), p, p->CanRollForItemInLFG(item, lootedObject) == EQUIP_ERR_OK, *r);
            }

            RollId.push_back(r);

            if (Creature* creature = lootedObject->ToCreature())
            {
                creature->m_groupLootTimer = 60000;
                creature->lootingGroupLowGUID = GetLowGUID();
            }
            else if (GameObject* go = lootedObject->ToGameObject())
            {
                go->m_groupLootTimer = 60000;
                go->lootingGroupLowGUID = GetLowGUID();
            }
        }
        else
            delete r;
    }
}

void Group::MasterLoot(Loot* loot, WorldObject* pLootedObject)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "Group::MasterLoot (SMSG_MASTER_LOOT_CANDIDATE_LIST)");
    uint32 real_count = 0;

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* looter = itr->getSource();
        if (!looter->IsInWorld())
            continue;

        if (loot && loot->AllowedPlayers.IsEnabled())
        {
            if (!loot->AllowedPlayers.HasPlayerGuid(looter->GetGUID()))
                continue;
        }

        if (looter->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
            ++real_count;
    }

    ObjectGuid guid_looted = pLootedObject->GetGUID();
    sObjectMgr->setLootViewGUID(guid_looted, pLootedObject->GetGUID());

    WorldPacket data(SMSG_MASTER_LOOT_CANDIDATE_LIST, 3 + 1 + 8 + (real_count*9));
    data.WriteBit(guid_looted[6]);
    data.WriteBit(guid_looted[4]);
    data.WriteBit(guid_looted[1]);
    data.WriteBits(real_count, 24);
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* looter = itr->getSource();
        if (!looter->IsInWorld())
            continue;

        if (loot && loot->AllowedPlayers.IsEnabled())
        {
            if (!loot->AllowedPlayers.HasPlayerGuid(looter->GetGUID()))
                continue;
        }

        if (looter->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
        {
            ObjectGuid guid = looter->GetGUID();

            data.WriteBit(guid[3]);
            data.WriteBit(guid[2]);
            data.WriteBit(guid[1]);
            data.WriteBit(guid[6]);
            data.WriteBit(guid[0]);
            data.WriteBit(guid[7]);
            data.WriteBit(guid[5]);
            data.WriteBit(guid[4]);
        }
    }
    data.WriteBit(guid_looted[5]);
    data.WriteBit(guid_looted[7]);
    data.WriteBit(guid_looted[3]);
    data.WriteBit(guid_looted[0]);
    data.WriteBit(guid_looted[2]);

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* looter = itr->getSource();
        if (!looter->IsInWorld())
            continue;

        if (loot && loot->AllowedPlayers.IsEnabled())
        {
            if (!loot->AllowedPlayers.HasPlayerGuid(looter->GetGUID()))
                continue;
        }

        if (looter->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
        {
            ObjectGuid guid = looter->GetGUID();

            data.WriteByteSeq(guid[0]);
            data.WriteByteSeq(guid[5]);
            data.WriteByteSeq(guid[7]);
            data.WriteByteSeq(guid[1]);
            data.WriteByteSeq(guid[2]);
            data.WriteByteSeq(guid[4]);
            data.WriteByteSeq(guid[3]);
            data.WriteByteSeq(guid[6]);
        }
    }
    data.WriteByteSeq(guid_looted[4]);
    data.WriteByteSeq(guid_looted[0]);
    data.WriteByteSeq(guid_looted[5]);
    data.WriteByteSeq(guid_looted[1]);
    data.WriteByteSeq(guid_looted[3]);
    data.WriteByteSeq(guid_looted[2]);
    data.WriteByteSeq(guid_looted[7]);
    data.WriteByteSeq(guid_looted[6]);
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* looter = itr->getSource();

        if (loot && loot->AllowedPlayers.IsEnabled())
        {
            if (!loot->AllowedPlayers.HasPlayerGuid(looter->GetGUID()))
                continue;
        }

        if (looter->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
            looter->GetSession()->SendPacket(&data);
    }
}

void Group::DoRollForAllMembers(ObjectGuid guid, uint8 slot, uint32 mapid, Loot* loot, LootItem& item, Player* player)
{
    WorldObject* pLootedObject = nullptr;
    if (IS_CRE_OR_VEH_GUID(guid))
        pLootedObject = player->GetMap()->GetCreature(guid);
    else if (IS_GAMEOBJECT_GUID(guid))
        pLootedObject = player->GetMap()->GetGameObject(guid);

    if (!pLootedObject)
        return;

    uint64 newitemGUID = MAKE_NEW_GUID(sObjectMgr->GetGenerator<HighGuid::Item>().Generate(), 0, HighGuid::Item);
    Roll* r = new Roll(newitemGUID, item);
    r->lootedGUID = guid;

    //a vector is filled with only near party members
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* member = itr->getSource();
        if (!member || !member->GetSession())
            continue;

        if (item.AllowedForPlayer(member))
        {
            if (member->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
            {
                r->totalPlayersRolling++;
                r->playerVote[member->GetGUID()] = NOT_EMITED_YET;
            }
        }
    }

    if (r->totalPlayersRolling > 0)
    {
        r->setLoot(loot);
        r->itemSlot = slot;

        RollId.push_back(r);
    }

    SendLootStartRoll(180000, mapid, *r);
    
    if (!(r->totalPlayersRolling > 0))
        delete r;
}

void Group::CountRollVote(uint64 playerGUID, uint8 slot, uint8 Choice)
{
    Rolls::iterator rollI = GetRoll(slot);
    if (rollI == RollId.end())
        return;
    Roll* roll = *rollI;

    Roll::PlayerVote::iterator itr = roll->playerVote.find(playerGUID);
    // this condition means that player joins to the party after roll begins
    if (itr == roll->playerVote.end())
        return;

    if (roll->getLoot())
        if (roll->getLoot()->items.empty())
            return;

    switch (Choice)
    {
    case ROLL_PASS:                                     // Player choose pass
        SendLootRoll(0, playerGUID, 0, ROLL_PASS, *roll);
        ++roll->totalPass;
        itr->second = PASS;
        break;
    case ROLL_NEED:                                     // player choose Need
        SendLootRoll(0, playerGUID, 0, ROLL_NEED, *roll);
        ++roll->totalNeed;
        itr->second = NEED;
        break;
    case ROLL_GREED:                                    // player choose Greed
        SendLootRoll(0, playerGUID, 0, ROLL_GREED, *roll);
        ++roll->totalGreed;
        itr->second = GREED;
        break;
    case ROLL_DISENCHANT:                               // player choose Disenchant
        SendLootRoll(0, playerGUID, 0, ROLL_DISENCHANT, *roll);
        ++roll->totalGreed;
        itr->second = DISENCHANT;
        break;
    }

    if (roll->totalPass + roll->totalNeed + roll->totalGreed >= roll->totalPlayersRolling)
        CountTheRoll(rollI);
}

//called when roll timer expires
void Group::EndRoll(Loot* pLoot)
{
    for (Rolls::iterator itr = RollId.begin(); itr != RollId.end();)
    {
        if ((*itr)->getLoot() == pLoot) {
            CountTheRoll(itr);           //i don't have to edit player votes, who didn't vote ... he will pass
            itr = RollId.begin();
        }
        else
            ++itr;
    }
}

void Group::CountTheRoll(Rolls::iterator rollI)
{
    Roll* roll = *rollI;
    if (!roll->isValid())                                   // is loot already deleted ?
    {
        RollId.erase(rollI);
        delete roll;
        return;
    }

    //end of the roll
    if (roll->totalNeed > 0)
    {
        if (!roll->playerVote.empty())
        {
            uint8 maxresul = 0;
            uint64 maxguid  = (*roll->playerVote.begin()).first;
            Player* player;

            for (Roll::PlayerVote::const_iterator itr=roll->playerVote.begin(); itr != roll->playerVote.end(); ++itr)
            {
                if (itr->second != NEED)
                    continue;

                uint8 randomN = urand(1, 100);
                SendLootRoll(0, itr->first, randomN, ROLL_NEED, *roll);
                if (maxresul < randomN)
                {
                    maxguid  = itr->first;
                    maxresul = randomN;
                }
            }
            SendLootRollWon(0, maxguid, maxresul, ROLL_NEED, *roll);
            player = ObjectAccessor::FindPlayer(maxguid);

            if (player && player->GetSession())
            {
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_NEED_ON_LOOT, roll->itemid, maxresul);

                ItemPosCountVec dest;
                LootItem* item = &(roll->itemSlot >= roll->getLoot()->items.size() ? roll->getLoot()->quest_items[roll->itemSlot - roll->getLoot()->items.size()] : roll->getLoot()->items[roll->itemSlot]);
                InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, roll->itemid, item->count);
                if (msg == EQUIP_ERR_OK)
                {
                    item->is_looted = true;
                    roll->getLoot()->NotifyItemRemoved(roll->itemSlot);
                    roll->getLoot()->unlootedCount--;
                    AllowedLooterSet looters = item->GetAllowedLooters();
                    player->StoreNewItem(dest, roll->itemid, true, item->randomPropertyId, looters);
                }
                else
                {
                    item->is_blocked = false;
                    player->SendEquipError(msg, NULL, NULL, roll->itemid);
                }
            }
        }
    }
    else if (roll->totalGreed > 0)
    {
        if (!roll->playerVote.empty())
        {
            uint8 maxresul = 0;
            uint64 maxguid = (*roll->playerVote.begin()).first;
            Player* player;
            RollVote rollvote = NOT_VALID;

            Roll::PlayerVote::iterator itr;
            for (itr = roll->playerVote.begin(); itr != roll->playerVote.end(); ++itr)
            {
                if (itr->second != GREED && itr->second != DISENCHANT)
                    continue;

                uint8 randomN = urand(1, 100);
                SendLootRoll(0, itr->first, randomN, itr->second, *roll);
                if (maxresul < randomN)
                {
                    maxguid  = itr->first;
                    maxresul = randomN;
                    rollvote = itr->second;
                }
            }
            if (rollvote != NOT_VALID)
                SendLootRollWon(0, maxguid, maxresul, rollvote, *roll);
            player = ObjectAccessor::FindPlayer(maxguid);

            if (player && player->GetSession())
            {
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_ROLL_GREED_ON_LOOT, roll->itemid, maxresul);

                LootItem* item = &(roll->itemSlot >= roll->getLoot()->items.size() ? roll->getLoot()->quest_items[roll->itemSlot - roll->getLoot()->items.size()] : roll->getLoot()->items[roll->itemSlot]);

                if (rollvote == GREED)
                {
                    ItemPosCountVec dest;
                    InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, roll->itemid, item->count);
                    if (msg == EQUIP_ERR_OK)
                    {
                        item->is_looted = true;
                        roll->getLoot()->NotifyItemRemoved(roll->itemSlot);
                        roll->getLoot()->unlootedCount--;
                        AllowedLooterSet looters = item->GetAllowedLooters();
                        player->StoreNewItem(dest, roll->itemid, true, item->randomPropertyId, looters);
                    }
                    else
                    {
                        item->is_blocked = false;
                        player->SendEquipError(msg, NULL, NULL, roll->itemid);
                    }
                }
                else if (rollvote == DISENCHANT)
                {
                    item->is_looted = true;
                    roll->getLoot()->NotifyItemRemoved(roll->itemSlot);
                    roll->getLoot()->unlootedCount--;
                    ItemTemplate const* pProto = sObjectMgr->GetItemTemplate(roll->itemid);
                    player->AutoStoreLoot(pProto->DisenchantID, LootTemplates_Disenchant, true);
                    player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_CAST_SPELL, 13262); // Disenchant
                }
            }
        }
    }
    else
    {
        SendLootAllPassed(*roll);

        // remove is_blocked so that the item is lootable by all players
        LootItem* item = &(roll->itemSlot >= roll->getLoot()->items.size() ? roll->getLoot()->quest_items[roll->itemSlot - roll->getLoot()->items.size()] : roll->getLoot()->items[roll->itemSlot]);
        if (item)
            item->is_blocked = false;
    }

    //SendLootRollsComplete(*roll);

    RollId.erase(rollI);
    delete roll;
}

void Group::SetTargetIcon(uint8 id, ObjectGuid whoGuid, ObjectGuid targetGuid)
{
    if (id >= TARGETICONCOUNT)
        return;

    // Clean other icons
    if (targetGuid != 0)
        for (int i = 0; i < TARGETICONCOUNT; ++i)
            if (m_targetIcons[i] == targetGuid)
                SetTargetIcon(i, 0, 0);

    m_targetIcons[id] = targetGuid;

    WorldPacket data(SMSG_RAID_TARGET_UPDATE_SINGLE, (1+8+1+8));

    ObjectGuid guid1 = whoGuid;
    ObjectGuid guid2 = targetGuid;

    data.WriteBit(guid1[7]);
    data.WriteBit(guid1[4]);
    data.WriteBit(guid2[2]);
    data.WriteBit(guid2[7]);
    data.WriteBit(guid1[2]);
    data.WriteBit(guid2[3]);
    data.WriteBit(guid1[0]);
    data.WriteBit(guid1[3]);
    data.WriteBit(guid2[0]);
    data.WriteBit(guid2[6]);
    data.WriteBit(guid1[5]);
    data.WriteBit(guid1[6]);
    data.WriteBit(guid1[1]);
    data.WriteBit(guid2[4]);
    data.WriteBit(guid2[1]);
    data.WriteBit(guid2[5]);

    data.WriteByteSeq(guid1[5]);
    data << uint8(id);
    data.WriteByteSeq(guid1[2]);
    data << uint8(0);                                       // set targets
    data.WriteByteSeq(guid1[0]);
    data.WriteByteSeq(guid2[0]);
    data.WriteByteSeq(guid2[7]);
    data.WriteByteSeq(guid1[6]);
    data.WriteByteSeq(guid2[3]);
    data.WriteByteSeq(guid1[1]);
    data.WriteByteSeq(guid2[5]);
    data.WriteByteSeq(guid1[3]);
    data.WriteByteSeq(guid1[4]);
    data.WriteByteSeq(guid2[2]);
    data.WriteByteSeq(guid2[6]);
    data.WriteByteSeq(guid1[7]);
    data.WriteByteSeq(guid2[4]);
    data.WriteByteSeq(guid2[1]);

    BroadcastPacket(&data, true);
}

void Group::SendTargetIconList(WorldSession* session)
{
    if (!session)
        return;

    WorldPacket data(SMSG_RAID_TARGET_UPDATE_ALL, (1+TARGETICONCOUNT*9));
    ByteBuffer dataBuffer;

    uint32 count = 0;
    for (uint8 i = 0; i < TARGETICONCOUNT; ++i)
    {
        if (m_targetIcons[i] == 0)
            continue;

        ++count;
    }

    data << uint8(1);                                       // list targets
    data.WriteBits(count, 23);

    for (uint8 i = 0; i < TARGETICONCOUNT; ++i)
    {
        if (m_targetIcons[i] == 0)
            continue;

        ObjectGuid guid = m_targetIcons[i];

        uint8 bitOrder[8] = { 4, 1, 7, 2, 5, 3, 0, 6 };
        data.WriteBitInOrder(guid, bitOrder);

        dataBuffer.WriteByteSeq(guid[7]);
        dataBuffer.WriteByteSeq(guid[6]);
        dataBuffer.WriteByteSeq(guid[4]);
        dataBuffer.WriteByteSeq(guid[0]);
        dataBuffer.WriteByteSeq(guid[2]);
        dataBuffer << uint8(i);
        dataBuffer.WriteByteSeq(guid[5]);
        dataBuffer.WriteByteSeq(guid[1]);
        dataBuffer.WriteByteSeq(guid[3]);
    }

    data.append(dataBuffer);
    session->SendPacket(&data);
}

void Group::SendUpdate()
{
    for (member_witerator witr = m_memberSlots.begin(); witr != m_memberSlots.end(); ++witr)
        SendUpdateToPlayer(witr->guid, &(*witr));
}

void Group::SendUpdatePlayerAtLeave(Player const* player, Group const* group, uint32 counter)
{
    if (!player || !player->GetSession())
        return;

    ObjectGuid leaderGuid = 0LL;
    ObjectGuid groupGuid = group ? group->GetGUID() : 0LL;
    ObjectGuid looterGuid = group ? group->GetLooterGuid() : 0LL;

    uint8 groupId = 0;

    if (group)
    {
        member_citerator citr = group->_getMemberCSlot(player->GetGUID());
        if (citr != group->m_memberSlots.end())
            groupId = citr->group;
    }

    WorldPacket data(SMSG_PARTY_UPDATE, 4 + 1 + 1 + 1 + 4 + 1 + 8 + 1 + 8 + 3);
    data << int32(-1);
    data << uint8(group ? group->GetGroupType() : 0x00);
    data << uint8(0);
    data << uint8(groupId);
    data << uint32(counter);

    data.WriteBit(group != nullptr);                      // has difficulty
    data.WriteBit(groupGuid[5]);
    data.WriteBits(0, 21);
    data.WriteBit(groupGuid[7]);
    data.WriteBit(leaderGuid[1]);
    data.WriteBit(0);
    data.WriteBit(leaderGuid[5]);
    data.WriteBit(groupGuid[0]);
    data.WriteBit(groupGuid[4]);
    data.WriteBit(leaderGuid[2]);
    data.WriteBit(groupGuid[3]);
    data.WriteBit(leaderGuid[0]);
    data.WriteBit(leaderGuid[4]);
    data.WriteBit(groupGuid[1]);
    data.WriteBit(groupGuid[2]);
    data.WriteBit(groupGuid[6]);
    data.WriteBit(leaderGuid[3]);
    data.WriteBit(leaderGuid[6]);
    data.WriteBit(leaderGuid[7]);

    data.WriteBit(group != nullptr);                      // has loot mode

    if (group != nullptr)
    {
        data.WriteBit(looterGuid[3]);
        data.WriteBit(looterGuid[7]);
        data.WriteBit(looterGuid[4]);
        data.WriteBit(looterGuid[5]);
        data.WriteBit(looterGuid[6]);
        data.WriteBit(looterGuid[0]);
        data.WriteBit(looterGuid[1]);
        data.WriteBit(looterGuid[2]);
    }

    if (group != nullptr)
    {
        data.WriteByteSeq(looterGuid[3]);
        data.WriteByteSeq(looterGuid[5]);
        data << uint8(group->GetLootMethod());                    // loot method
        data.WriteByteSeq(looterGuid[0]);
        data.WriteByteSeq(looterGuid[1]);
        data.WriteByteSeq(looterGuid[2]);
        data.WriteByteSeq(looterGuid[4]);
        data.WriteByteSeq(looterGuid[6]);
        data << uint8(group->GetLootThreshold());                 // loot threshold
        data.WriteByteSeq(looterGuid[7]);
    }

    data.WriteByteSeq(leaderGuid[1]);
    data.WriteByteSeq(groupGuid[3]);
    if (group != nullptr)
    {
        data << uint32(group->GetDungeonDifficulty()); 
        data << uint32(group->GetRaidDifficulty());            
    }
    data.WriteByteSeq(leaderGuid[5]);
    data.WriteByteSeq(groupGuid[2]);
    data.WriteByteSeq(leaderGuid[0]);
    data.WriteByteSeq(groupGuid[6]);
    data.WriteByteSeq(groupGuid[5]);
    data.WriteByteSeq(groupGuid[4]);
    data.WriteByteSeq(leaderGuid[2]);
    data.WriteByteSeq(leaderGuid[3]);
    data.WriteByteSeq(leaderGuid[6]);
    data.WriteByteSeq(groupGuid[0]);
    data.WriteByteSeq(leaderGuid[7]);
    data.WriteByteSeq(groupGuid[7]);
    data.WriteByteSeq(leaderGuid[4]);
    data.WriteByteSeq(groupGuid[1]);

    player->GetSession()->SendPacket(&data);
}

void Group::SendUpdateToPlayer(uint64 playerGUID, MemberSlot* slot)
{
    Player* player = ObjectAccessor::FindPlayer(playerGUID);
    if (!player || !player->GetSession() || player->GetGroup() != this)
        return;

    if (!slot)
    {
        member_witerator witr = _getMemberWSlot(playerGUID);

        if (witr == m_memberSlots.end()) // if there is no MemberSlot for such a player
            return;

        slot = &(*witr);
    }

    uint8 groupPosition;
    uint8 i = 0;

    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        if(citr->group != slot->group)
            continue;

        if(citr->guid == slot->guid)
        {
            groupPosition = i;
            break;
        }

        i++;
    }

    ByteBuffer memberData(32);
    ObjectGuid groupGuid = GetGUID();
    ObjectGuid leaderGuid = GetLeaderGUID();
    ObjectGuid looterGuid = GetLooterGuid();

    bool sendDifficulty = true;
    bool hasLooterData = true;

    WorldPacket data(SMSG_PARTY_UPDATE, 4 + 3 + 4 + 1 + 8 + 1 + 8 + 3 + (GetMembersCount() * (4 + 2 + 8)) + 1 + 8 + 2 + 4 + 4);
    data << uint32(groupPosition);
    data << uint8(m_groupType);                         // group type (flags in 3.3)
    data << uint8(slot->roles);
    data << uint8(slot->group);
    data << uint32(m_counter++);                        // 3.3, value increases every time this packet gets sent

    data.WriteBit(sendDifficulty);                      // has dungeon and raid difficulty
    data.WriteBit(groupGuid[5]);
    data.WriteBits(GetMembersCount(), 21);
    data.WriteBit(groupGuid[7]);
    data.WriteBit(leaderGuid[1]);

    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        ObjectGuid memberGuid = citr->guid;
        std::string memberName = citr->name;

        Player* member = ObjectAccessor::FindPlayer(memberGuid);

        uint8 onlineState = member ? MEMBER_STATUS_ONLINE : MEMBER_STATUS_OFFLINE;
        onlineState = onlineState | ((isBGGroup() || isBFGroup()) ? MEMBER_STATUS_PVP : 0);

        data.WriteBit(memberGuid[1]);
        data.WriteBit(memberGuid[2]);
        data.WriteBit(memberGuid[4]);
        data.WriteBit(memberGuid[6]);
        data.WriteBit(memberGuid[0]);
        data.WriteBit(memberGuid[3]);
        data.WriteBits(memberName.size(), 6);
        data.WriteBit(memberGuid[5]);
        data.WriteBit(memberGuid[7]);

        // uint8 values need rechecking
        memberData.WriteByteSeq(memberGuid[6]);
        memberData.WriteByteSeq(memberGuid[3]);
        memberData << uint8(citr->flags);               // See enum GroupMemberFlags
        memberData.WriteByteSeq(memberGuid[2]);
        memberData.WriteByteSeq(memberGuid[1]);
        memberData << uint8(onlineState);               // online-state
        memberData.WriteByteSeq(memberGuid[4]);
        memberData.WriteByteSeq(memberGuid[5]);
        memberData << uint8(citr->roles);               // Lfg Roles
        memberData.WriteString(memberName);
        memberData << uint8(citr->group);               // groupid
        memberData.WriteByteSeq(memberGuid[7]);
        memberData.WriteByteSeq(memberGuid[0]);
    }

    data.WriteBit(isLFGGroup());                           // is LFG
    data.WriteBit(leaderGuid[5]);
    data.WriteBit(groupGuid[0]);
    data.WriteBit(groupGuid[4]);
    data.WriteBit(leaderGuid[2]);
    data.WriteBit(groupGuid[3]);
    data.WriteBit(leaderGuid[0]);
    data.WriteBit(leaderGuid[4]);
    data.WriteBit(groupGuid[1]);
    data.WriteBit(groupGuid[2]);
    data.WriteBit(groupGuid[6]);
    data.WriteBit(leaderGuid[3]);
    data.WriteBit(leaderGuid[6]);
    data.WriteBit(leaderGuid[7]);

    if (isLFGGroup())
    {
        data.WriteBit(isLFGGroup() && !isRaidGroup());
        data.WriteBit(false);
    }

    data.WriteBit(hasLooterData);                      // has loot mode

    if (hasLooterData)
    {
        data.WriteBit(looterGuid[3]);
        data.WriteBit(looterGuid[7]);
        data.WriteBit(looterGuid[4]);
        data.WriteBit(looterGuid[5]);
        data.WriteBit(looterGuid[6]);
        data.WriteBit(looterGuid[0]);
        data.WriteBit(looterGuid[1]);
        data.WriteBit(looterGuid[2]);
    }

    if (hasLooterData)
    {
        data.WriteByteSeq(looterGuid[3]);
        data.WriteByteSeq(looterGuid[5]);
        data << uint8(m_lootMethod);                    // loot method
        data.WriteByteSeq(looterGuid[0]);
        data.WriteByteSeq(looterGuid[1]);
        data.WriteByteSeq(looterGuid[2]);
        data.WriteByteSeq(looterGuid[4]);
        data.WriteByteSeq(looterGuid[6]);
        data << uint8(m_lootThreshold);                 // loot threshold
        data.WriteByteSeq(looterGuid[7]);
    }

    data.WriteByteSeq(leaderGuid[1]);

    if (isLFGGroup())
    {
        data << uint8(0);
        data << uint8(0);
        data << uint8(1);
        data << uint32(sLFGMgr->GetDungeon(GetGUID())); // dungeon entry
        data << uint8(0);
        data << uint32(100663758); // join time
        data << float(1.0f);
        data << uint8(GetMembersCount() - 1);
    }

    data.WriteByteSeq(groupGuid[3]);
    data.append(memberData);

    if (sendDifficulty)
    {
        data << uint32(m_dungeonDifficulty); 
        data << uint32(m_raidDifficulty);            
    }

    data.WriteByteSeq(leaderGuid[5]);
    data.WriteByteSeq(groupGuid[2]);
    data.WriteByteSeq(leaderGuid[0]);
    data.WriteByteSeq(groupGuid[6]);
    data.WriteByteSeq(groupGuid[5]);
    data.WriteByteSeq(groupGuid[4]);
    data.WriteByteSeq(leaderGuid[2]);
    data.WriteByteSeq(leaderGuid[3]);
    data.WriteByteSeq(leaderGuid[6]);
    data.WriteByteSeq(groupGuid[0]);
    data.WriteByteSeq(leaderGuid[7]);
    data.WriteByteSeq(groupGuid[7]);
    data.WriteByteSeq(leaderGuid[4]);
    data.WriteByteSeq(groupGuid[1]);

    player->GetSession()->SendPacket(&data);
}

void Group::UpdatePlayerOutOfRange(Player* player)
{
    if (!player || !player->IsInWorld())
        return;

    WorldPacket data;
    player->GetSession()->BuildPartyMemberStatsChangedPacket(player, &data, player->GetGroupUpdateFlag(), player->GetGUID());

    Player* member;
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        member = itr->getSource();
        if (member && !member->IsWithinDist(player, member->GetSightRange(), false))
            member->GetSession()->SendPacket(&data);
    }
}

void Group::BroadcastAddonMessagePacket(WorldPacket* packet, const std::string& prefix, bool ignorePlayersInBGRaid, int group, uint64 ignore)
{
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (!player || (ignore != 0 && player->GetGUID() == ignore) || (ignorePlayersInBGRaid && player->GetGroup() != this))
            continue;

        if (WorldSession* session = player->GetSession())
            if (session && (group == -1 || itr->getSubGroup() == group))
                if (session->IsAddonRegistered(prefix))
                    session->SendPacket(packet);
    }
}

void Group::BroadcastPacket(WorldPacket* packet, bool ignorePlayersInBGRaid, int group, uint64 ignore)
{
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (!player || (ignore != 0 && player->GetGUID() == ignore) || (ignorePlayersInBGRaid && player->GetGroup() != this))
            continue;

        if (player->GetSession() && (group == -1 || itr->getSubGroup() == group))
            player->GetSession()->SendPacket(packet);
    }
}

void Group::BroadcastReadyCheck(WorldPacket* packet)
{
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (player && player->GetSession())
            if (IsLeader(player->GetGUID()) || IsAssistant(player->GetGUID()) || m_groupType & GROUPTYPE_EVERYONE_IS_ASSISTANT)
                player->GetSession()->SendPacket(packet);
    }
}

void Group::OfflineReadyCheck()
{
    ObjectGuid grpGUID = GetGUID();
    WorldPacket data;
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        Player* player = ObjectAccessor::FindPlayer(citr->guid);
        if (!player || !player->GetSession())
        {
            bool ready = false;
            ObjectGuid plGUID = citr->guid;

            data.Initialize(SMSG_RAID_READY_CHECK_RESPONSE, 19);

            data.WriteBit(plGUID[1]);
            data.WriteBit(plGUID[3]);
            data.WriteBit(plGUID[7]);
            data.WriteBit(plGUID[0]);
            data.WriteBit(grpGUID[4]);
            data.WriteBit(grpGUID[7]);
            data.WriteBit(plGUID[2]);
            data.WriteBit(ready);
            data.WriteBit(grpGUID[2]);
            data.WriteBit(grpGUID[6]);
            data.WriteBit(plGUID[4]);
            data.WriteBit(plGUID[5]);
            data.WriteBit(grpGUID[1]);
            data.WriteBit(grpGUID[0]);
            data.WriteBit(grpGUID[5]);
            data.WriteBit(grpGUID[3]);
            data.WriteBit(plGUID[6]);

            data.WriteByteSeq(plGUID[2]);
            data.WriteByteSeq(plGUID[3]);
            data.WriteByteSeq(plGUID[7]);
            data.WriteByteSeq(grpGUID[1]);
            data.WriteByteSeq(grpGUID[7]);
            data.WriteByteSeq(plGUID[1]);
            data.WriteByteSeq(plGUID[0]);
            data.WriteByteSeq(grpGUID[2]);
            data.WriteByteSeq(grpGUID[3]);
            data.WriteByteSeq(plGUID[6]);
            data.WriteByteSeq(grpGUID[0]);
            data.WriteByteSeq(plGUID[5]);
            data.WriteByteSeq(plGUID[4]);
            data.WriteByteSeq(grpGUID[4]);
            data.WriteByteSeq(grpGUID[5]);
            data.WriteByteSeq(grpGUID[6]);

            BroadcastReadyCheck(&data);

            m_readyCheckCount++;
        }
    }
}

bool Group::_setMembersGroup(uint64 guid, uint8 group)
{
    member_witerator slot = _getMemberWSlot(guid);
    if (slot == m_memberSlots.end())
        return false;

    slot->group = group;

    SubGroupCounterIncrease(group);

    if (!isBGGroup() && !isBFGroup())
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_MEMBER_SUBGROUP);

        stmt->setUInt8(0, group);
        stmt->setUInt32(1, GUID_LOPART(guid));

        CharacterDatabase.Execute(stmt);
    }

    return true;
}

bool Group::SameSubGroup(Player const* member1, Player const* member2) const
{
    if (!member1 || !member2)
        return false;

    if (member1->GetGroup() != this || member2->GetGroup() != this)
        return false;
    else
        return member1->GetSubGroup() == member2->GetSubGroup();
}

// Allows setting sub groups both for online or offline members
void Group::ChangeMembersGroup(uint64 guid, uint8 group)
{
    // Only raid groups have sub groups
    if (!isRaidGroup())
        return;

    // Check if player is really in the raid
    member_witerator slot = _getMemberWSlot(guid);
    if (slot == m_memberSlots.end())
        return;

    // Abort if the player is already in the target sub group
    uint8 prevSubGroup = GetMemberGroup(guid);
    if (prevSubGroup == group)
        return;

    // Update the player slot with the new sub group setting
    slot->group = group;

    // Increase the counter of the new sub group..
    SubGroupCounterIncrease(group);

    // ..and decrease the counter of the previous one
    SubGroupCounterDecrease(prevSubGroup);

    // Preserve new sub group in database for non-raid groups
    if (!isBGGroup() && !isBFGroup())
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_MEMBER_SUBGROUP);

        stmt->setUInt8(0, group);
        stmt->setUInt32(1, GUID_LOPART(guid));

        CharacterDatabase.Execute(stmt);
    }

    // In case the moved player is online, update the player object with the new sub group references
    if (Player* player = ObjectAccessor::FindPlayer(guid))
    {
        if (player->GetGroup() == this)
            player->GetGroupRef().setSubGroup(group);
        else
        {
            // If player is in BG raid, it is possible that he is also in normal raid - and that normal raid is stored in m_originalGroup reference
            prevSubGroup = player->GetOriginalSubGroup();
            player->GetOriginalGroupRef().setSubGroup(group);
        }
    }

    // Broadcast the changes to the group
    SendUpdate();
}

// Retrieve the next Round-Roubin player for the group
//
// No update done if loot method is Master or FFA.
//
// If the RR player is not yet set for the group, the first group member becomes the round-robin player.
// If the RR player is set, the next player in group becomes the round-robin player.
//
// If ifneed is true,
//      the current RR player is checked to be near the looted object.
//      if yes, no update done.
//      if not, he loses his turn.
void Group::UpdateLooterGuid(WorldObject* pLootedObject, bool ifneed)
{
    switch (GetLootMethod())
    {
        case MASTER_LOOT:
        case FREE_FOR_ALL:
            return;
        default:
            // round robin style looting applies for all low
            // quality items in each loot method except free for all and master loot
            break;
    }

    uint64 oldLooterGUID = GetLooterGuid();
    member_citerator guid_itr = _getMemberCSlot(oldLooterGUID);
    if (guid_itr != m_memberSlots.end())
    {
        if (ifneed)
        {
            // not update if only update if need and ok
            Player* looter = ObjectAccessor::FindPlayer(guid_itr->guid);
            if (looter && looter->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
                return;
        }
        ++guid_itr;
    }

    // search next after current
    Player* pNewLooter = NULL;
    for (member_citerator itr = guid_itr; itr != m_memberSlots.end(); ++itr)
    {
        if (Player* player = ObjectAccessor::FindPlayer(itr->guid))
            if (player->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
            {
                pNewLooter = player;
                break;
            }
    }

    if (!pNewLooter)
    {
        // search from start
        for (member_citerator itr = m_memberSlots.begin(); itr != guid_itr; ++itr)
        {
            if (Player* player = ObjectAccessor::FindPlayer(itr->guid))
                if (player->IsWithinDistInMap(pLootedObject, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE), false))
                {
                    pNewLooter = player;
                    break;
                }
        }
    }

    if (pNewLooter)
    {
        if (oldLooterGUID != pNewLooter->GetGUID())
        {
            SetLooterGuid(pNewLooter->GetGUID());
            SendUpdate();
        }
    }
    else
    {
        SetLooterGuid(0);
        SendUpdate();
    }
}

GroupJoinBattlegroundResult Group::CanJoinBattlegroundQueue(Battleground const* bgOrTemplate, BattlegroundQueueTypeId bgQueueTypeId, uint32 MinPlayerCount, uint32 /*MaxPlayerCount*/, bool isRated, uint32 arenaSlot)
{
    // check if this group is LFG group
    if (isLFGGroup())
        return ERR_LFG_CANT_USE_BATTLEGROUND;

    BattlemasterListEntry const* bgEntry = sBattlemasterListStore.LookupEntry(bgOrTemplate->GetTypeID());
    if (!bgEntry)
        return ERR_BATTLEGROUND_JOIN_FAILED;            // shouldn't happen

    // check for min / max count
    uint32 memberscount = GetMembersCount();

    if (memberscount > bgEntry->maxGroupSize)                // no MinPlayerCount for battlegrounds
        return ERR_BATTLEGROUND_NONE;                        // ERR_GROUP_JOIN_BATTLEGROUND_TOO_MANY handled on client side

    // get a player as reference, to compare other players' stats to (arena team id, queue id based on level, etc.)
    Player* reference = GetFirstMember()->getSource();
    // no reference found, can't join this way
    if (!reference)
        return ERR_BATTLEGROUND_JOIN_FAILED;

    PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgOrTemplate->GetMapId(), reference->getLevel());
    if (!bracketEntry)
        return ERR_BATTLEGROUND_JOIN_FAILED;

    uint32 team = reference->GetTeam();

    BattlegroundQueueTypeId bgQueueTypeIdRandom = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_RB, 0);

    // check every member of the group to be able to join
    memberscount = 0;
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next(), ++memberscount)
    {
        Player* member = itr->getSource();
        // offline member? don't let join
        if (!member)
            return ERR_BATTLEGROUND_JOIN_FAILED;
        // don't allow cross-faction join as group
        if (member->GetTeam() != team)
            return ERR_BATTLEGROUND_JOIN_TIMED_OUT;
        // not in the same battleground level braket, don't let join
        PvPDifficultyEntry const* memberBracketEntry = GetBattlegroundBracketByLevel(bracketEntry->mapId, member->getLevel());
        if (memberBracketEntry != bracketEntry)
            return ERR_BATTLEGROUND_JOIN_RANGE_INDEX;
        // don't let join if someone from the group is already in that bg queue
        if (member->InBattlegroundQueueForBattlegroundQueueType(bgQueueTypeId))
            return ERR_BATTLEGROUND_JOIN_FAILED;            // not blizz-like
        // don't let join if someone from the group is in bg queue random
        if (member->InBattlegroundQueueForBattlegroundQueueType(bgQueueTypeIdRandom))
            return ERR_IN_RANDOM_BG;
        // don't let join to bg queue random if someone from the group is already in bg queue
        if (bgOrTemplate->GetTypeID() == BATTLEGROUND_RB && member->InBattlegroundQueue())
            return ERR_IN_NON_RANDOM_BG;
        // check for deserter debuff in case not arena queue
        if (bgOrTemplate->GetTypeID() != BATTLEGROUND_AA && !member->CanJoinToBattleground())
            return ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS;
        // check if member can join any more battleground queues
        if (!member->HasFreeBattlegroundQueueId())
            return ERR_BATTLEGROUND_TOO_MANY_QUEUES;        // not blizz-like
        // check if someone in party is using dungeon system
        if (member->isUsingLfg())
            return ERR_LFG_CANT_USE_BATTLEGROUND;
        // check is someone in party is loading or teleporting
        if (member->GetSession()->PlayerLoading() || member->IsBeingTeleported())
            return ERR_BATTLEGROUND_JOIN_FAILED;
    }

    // only check for MinPlayerCount since MinPlayerCount == MaxPlayerCount for arenas...
    if (bgOrTemplate->isArena() && memberscount != MinPlayerCount)
        return ERR_ARENA_TEAM_PARTY_SIZE;

    return ERR_BATTLEGROUND_NONE;
}

//===================================================
//============== Roll ===============================
//===================================================

void Roll::targetObjectBuildLink()
{
    // called from link()
    getTarget()->addLootValidatorRef(this);
}

void Group::SetDungeonDifficulty(Difficulty difficulty)
{
    m_dungeonDifficulty = difficulty;
    if (!isBGGroup() && !isBFGroup())
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_DIFFICULTY);

        stmt->setUInt8(0, uint8(m_dungeonDifficulty));
        stmt->setUInt32(1, m_dbStoreId);

        CharacterDatabase.Execute(stmt);
    }

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (!player->GetSession())
            continue;

        player->SetDungeonDifficulty(difficulty);
        player->SendDungeonDifficulty(true);
    }

    SendUpdate();
}

void Group::SetRaidDifficulty(Difficulty difficulty)
{
    m_raidDifficulty = difficulty;
    if (!isBGGroup() && !isBFGroup())
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_RAID_DIFFICULTY);

        stmt->setUInt8(0, uint8(m_raidDifficulty));
        stmt->setUInt32(1, m_dbStoreId);

        CharacterDatabase.Execute(stmt);
    }

    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (!player->GetSession())
            continue;

        player->SetRaidDifficulty(difficulty);
        player->SendRaidDifficulty(true);
    }

    SendUpdate();
}

bool Group::InCombatToInstance(uint32 instanceId)
{
    for (GroupReference* itr = GetFirstMember(); itr != NULL; itr = itr->next())
    {
        Player* player = itr->getSource();
        if (player && !player->getAttackers().empty() && player->GetInstanceId() == instanceId && (player->GetMap()->IsRaidOrHeroicDungeon()))
            for (std::set<Unit*>::const_iterator i = player->getAttackers().begin(); i != player->getAttackers().end(); ++i)
                if ((*i) && (*i)->GetTypeId() == TYPEID_UNIT && (*i)->ToCreature()->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_INSTANCE_BIND)
                    return true;
    }
    return false;
}

void Group::ResetInstances(uint8 method, bool isRaid, Player* SendMsgTo)
{
    if (isBGGroup() || isBFGroup())
        return;

    // method can be INSTANCE_RESET_ALL, INSTANCE_RESET_CHANGE_DIFFICULTY, INSTANCE_RESET_GROUP_DISBAND

    // we assume that when the difficulty changes, all instances that can be reset will be
    Difficulty diff = GetDifficulty(isRaid);

    if (RaidEncountersMap* encounters = sRaidEncountersMgr->GetEncountersForPlayer(GUID_LOPART(GetLeaderGUID())))
    {
        for (RaidEncountersMap::iterator itr = encounters->begin(); itr != encounters->end(); )
        {
            if (method == INSTANCE_RESET_ALL)
            {
                ++itr;
                continue;
            }

            bool isEmpty = true;
            // if the map is loaded, reset it
            Map* map = sMapMgr->FindRaidMap(itr->first, &itr->second);
            if (map && map->IsRaid())
            {
                isEmpty = ((InstanceMap*)map)->Reset(method);
            }

            if (itr->second.IsBinded())
            {
                ++itr;
                continue;
            }

            if (isEmpty)
            {
                if (SendMsgTo)
                {
                    SendMsgTo->SendResetInstanceSuccess(itr->first);
                }

                itr->second.DeleteRespawnTimes();
                itr = encounters->erase(itr);
            }
            else
            {
                if (SendMsgTo)
                {
                    SendMsgTo->SendResetInstanceFailed(3, itr->first);
                }

                ++itr;
            }
        }
    }

    for (BoundInstancesMap::iterator itr = m_boundInstances[diff].begin(); itr != m_boundInstances[diff].end();)
    {
        InstanceSave* instanceSave = itr->second.save;
        const MapEntry* entry = sMapStore.LookupEntry(itr->first);
        if (!entry || entry->IsRaid() != isRaid || (!instanceSave->CanReset() && method != INSTANCE_RESET_GROUP_DISBAND))
        {
            ++itr;
            continue;
        }

        if (method == INSTANCE_RESET_ALL)
        {
            // the "reset all instances" method can only reset normal maps
            if (entry->map_type == MAP_RAID || diff == HEROIC_DIFFICULTY)
            {
                ++itr;
                continue;
            }
        }

        bool isEmpty = true;
        // if the map is loaded, reset it
        Map* map = sMapMgr->FindMap(instanceSave->GetMapId(), instanceSave->GetInstanceId());
        if (map && map->IsDungeon() && !(method == INSTANCE_RESET_GROUP_DISBAND && !instanceSave->CanReset()))
        {
            if (instanceSave->CanReset())
                isEmpty = ((InstanceMap*)map)->Reset(method);
            else
                isEmpty = !map->HavePlayers();
        }

        if (SendMsgTo)
        {
            if (isEmpty)
                SendMsgTo->SendResetInstanceSuccess(instanceSave->GetMapId());
            else
                SendMsgTo->SendResetInstanceFailed(3, instanceSave->GetMapId());
        }

        if (isEmpty || method == INSTANCE_RESET_GROUP_DISBAND || method == INSTANCE_RESET_CHANGE_DIFFICULTY)
        {
            // do not reset the instance, just unbind if others are permanently bound to it
            if (isEmpty && instanceSave->CanReset())
                instanceSave->DeleteFromDB();
            else
            {
                PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GROUP_INSTANCE_BY_INSTANCE);

                stmt->setUInt32(0, instanceSave->GetInstanceId());

                CharacterDatabase.Execute(stmt);
            }


            // i don't know for sure if hash_map iterators
            m_boundInstances[diff].erase(itr);
            itr = m_boundInstances[diff].begin();
            // this unloads the instance save unless online players are bound to it
            // (eg. permanent binds or GM solo binds)
            instanceSave->RemoveGroup(this);
        }
        else
            ++itr;
    }
}

InstanceGroupBind* Group::GetBoundInstance(Player* player)
{
    uint32 mapid = player->GetMapId();
    MapEntry const* mapEntry = sMapStore.LookupEntry(mapid);
    return GetBoundInstance(mapEntry);
}

InstanceGroupBind* Group::GetBoundInstance(Map* aMap)
{
    // Currently spawn numbering not different from map difficulty
    Difficulty difficulty = GetDifficulty(aMap->IsRaid());

    // some instances only have one difficulty
    GetDownscaledMapDifficultyData(aMap->GetId(), difficulty);

    BoundInstancesMap::iterator itr = m_boundInstances[difficulty].find(aMap->GetId());
    if (itr != m_boundInstances[difficulty].end())
        return &itr->second;
    else
        return NULL;
}

InstanceGroupBind* Group::GetBoundInstance(MapEntry const* mapEntry)
{
    if (!mapEntry)
        return NULL;

    Difficulty difficulty = GetDifficulty(mapEntry->IsRaid());

    // some instances only have one difficulty
    GetDownscaledMapDifficultyData(mapEntry->MapID, difficulty);

    BoundInstancesMap::iterator itr = m_boundInstances[difficulty].find(mapEntry->MapID);
    if (itr != m_boundInstances[difficulty].end())
        return &itr->second;
    else
        return NULL;
}

InstanceGroupBind* Group::GetBoundInstance(Difficulty difficulty, uint32 mapId)
{
    // some instances only have one difficulty
    GetDownscaledMapDifficultyData(mapId, difficulty);

    BoundInstancesMap::iterator itr = m_boundInstances[difficulty].find(mapId);
    if (itr != m_boundInstances[difficulty].end())
        return &itr->second;
    else
        return NULL;
}

InstanceGroupBind* Group::BindToInstance(InstanceSave* save, bool permanent, bool load)
{
    if (!save || isBGGroup() || isBFGroup())
        return NULL;

    InstanceGroupBind& bind = m_boundInstances[save->GetDifficulty()][save->GetMapId()];
    if (!load && (!bind.save || permanent != bind.perm || save != bind.save))
    {
        PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_GROUP_INSTANCE);

        stmt->setUInt32(0, m_dbStoreId);
        stmt->setUInt32(1, save->GetInstanceId());
        stmt->setBool(2, permanent);

        CharacterDatabase.Execute(stmt);
    }

    if (bind.save != save)
    {
        if (bind.save)
            bind.save->RemoveGroup(this);
        save->AddGroup(this);
    }

    bind.save = save;
    bind.perm = permanent;
    if (!load)
        sLog->outDebug(LOG_FILTER_MAPS, "Group::BindToInstance: Group (guid: %u, storage id: %u) is now bound to map %d, instance %d, difficulty %d",
        GUID_LOPART(GetGUID()), m_dbStoreId, save->GetMapId(), save->GetInstanceId(), save->GetDifficulty());

    return &bind;
}

void Group::UnbindInstance(uint32 mapid, uint8 difficulty, bool unload)
{
    BoundInstancesMap::iterator itr = m_boundInstances[difficulty].find(mapid);
    if (itr != m_boundInstances[difficulty].end())
    {
        if (!unload)
        {
            PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GROUP_INSTANCE_BY_GUID);

            stmt->setUInt32(0, m_dbStoreId);
            stmt->setUInt32(1, itr->second.save->GetInstanceId());

            CharacterDatabase.Execute(stmt);
        }

        itr->second.save->RemoveGroup(this);                // save can become invalid
        m_boundInstances[difficulty].erase(itr);
    }
}

void Group::_homebindIfInstance(Player* player)
{
    if (player && !player->isGameMaster() && sMapStore.LookupEntry(player->GetMapId())->IsDungeon())
        player->m_InstanceValid = false;
}

void Group::BroadcastGroupUpdate(void)
{
    // FG: HACK: force flags update on group leave - for values update hack
    // -- not very efficient but safe
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        Player* pp = ObjectAccessor::FindPlayer(citr->guid);
        if (pp && pp->IsInWorld())
        {
            pp->ForceValuesUpdateAtIndex(UNIT_FIELD_BYTES_2);
            pp->ForceValuesUpdateAtIndex(UNIT_FIELD_FACTIONTEMPLATE);
            sLog->outDebug(LOG_FILTER_GENERAL, "-- Forced group value update for '%s'", pp->GetName());
        }
    }
}

void Group::ResetMaxEnchantingLevel()
{
    m_maxEnchantingLevel = 0;
    Player* pMember = NULL;
    for (member_citerator citr = m_memberSlots.begin(); citr != m_memberSlots.end(); ++citr)
    {
        pMember = ObjectAccessor::FindPlayer(citr->guid);
        if (pMember && m_maxEnchantingLevel < pMember->GetSkillValue(SKILL_ENCHANTING))
            m_maxEnchantingLevel = pMember->GetSkillValue(SKILL_ENCHANTING);
    }
}

void Group::SetLootMethod(LootMethod method)
{
    m_lootMethod = method;
}

void Group::SetLooterGuid(uint64 guid)
{
    m_looterGuid = guid;
}

void Group::SetLootThreshold(ItemQualities threshold)
{
    m_lootThreshold = threshold;
}

void Group::SetLfgRoles(uint64 guid, const uint8 roles)
{
    member_witerator slot = _getMemberWSlot(guid);
    if (slot == m_memberSlots.end())
        return;

    slot->roles = roles;
    SendUpdate();
}

bool Group::IsFull() const
{
    return isRaidGroup() ? (m_memberSlots.size() >= MAXRAIDSIZE) : (m_memberSlots.size() >= MAXGROUPSIZE);
}

bool Group::isLFGGroup() const
{
    return m_groupType & GROUPTYPE_LFG;
}

bool Group::isRaidGroup() const
{
    return m_groupType & GROUPTYPE_RAID;
}

bool Group::isBGGroup() const
{
    return m_bgGroup != NULL;
}

bool Group::isBFGroup() const
{
    return m_bfGroup != NULL;
}

bool Group::IsCreated() const
{
    return GetMembersCount() > 0;
}

bool Group::IsGuildGroup(uint32 p_GuildID, bool p_SameMap, bool p_SameInstanceID) const
{
    uint32 l_MapID = 0;
    uint32 l_InstanceID = 0;
    uint32 l_Counter = 0;

    std::vector<Player*> l_Members;

    /// First we populate the array
    for (GroupReference const* l_Iter = GetFirstMember(); l_Iter != NULL; l_Iter = l_Iter->next()) ///< Loop trough all members
    {
        if (Player* l_Player = l_Iter->getSource())
        {
            if (l_Player->GetGuildId() == p_GuildID) ///< Check if it has a guild
                l_Members.push_back(l_Player);
        }
    }

    bool l_IsOkay = false;
    l_Counter = l_Members.size();
    for (std::vector<Player*>::iterator l_Iter = l_Members.begin(); l_Iter != l_Members.end(); ++l_Iter) ///< Iterate through players
    {
        if (Player* l_Player = (*l_Iter))
        {
            if (l_Player->GetMap() == nullptr)
                continue;

            if (l_MapID == 0)
                l_MapID = l_Player->GetMapId();

            if (l_InstanceID == 0)
                l_InstanceID = l_Player->GetInstanceId();

            if (l_Player->GetMap()->IsNonRaidDungeon() && !l_IsOkay)
            {
                if (l_Counter >= 3)
                    l_IsOkay = true;
            }

            if (l_Player->GetMap()->IsRaid() && !l_IsOkay)
            {
                switch (l_Player->GetMap()->GetDifficulty())
                {
                    case MAN10_DIFFICULTY:
                    case MAN10_HEROIC_DIFFICULTY:
                        if (l_Counter >= 8)
                            l_IsOkay = true;
                        break;
                    case MAN25_DIFFICULTY:
                    case MAN25_HEROIC_DIFFICULTY:
                    case RAID_TOOL_DIFFICULTY:
                        if (l_Counter >= 20)
                            l_IsOkay = true;
                        break;
                    case MAN40_DIFFICULTY:
                    case DYNAMIC_DIFFICULTY:
                        if (l_Counter >= 30)
                            l_IsOkay = true;
                        break;
                    default:
                        break;
                }
            }

            if (l_Player->GetMap()->IsBattleArena() && !l_IsOkay)
            {
                if (l_Counter == GetMembersCount())
                    l_IsOkay = true;
            }

            if (l_Player->GetMap()->IsBattleground() && !l_IsOkay)
            {
                if (Battleground* l_Battleground = l_Player->GetBattleground())
                {
                    if (l_Counter >= uint32(l_Battleground->GetMaxPlayers() * 0.8f))
                        l_IsOkay = true;
                }
            }

            /// ToDo: Check 40-player raids: 10/40
            if (p_SameMap && (l_MapID != l_Player->GetMapId()))
                return false;

            if (p_SameInstanceID && (l_InstanceID != l_Player->GetInstanceId()))
                return false;
        }
    }

    return l_IsOkay;
}

uint64 Group::GetLeaderGUID() const
{
    return m_leaderGuid;
}

uint64 Group::GetGUID() const
{
    return m_guid;
}

uint32 Group::GetLowGUID() const
{
    return GUID_LOPART(m_guid);
}

const char * Group::GetLeaderName() const
{
    return m_leaderName.c_str();
}

LootMethod Group::GetLootMethod() const
{
    return m_lootMethod;
}

uint64 Group::GetLooterGuid() const
{
    return m_looterGuid;
}

ItemQualities Group::GetLootThreshold() const
{
    return m_lootThreshold;
}

bool Group::IsMember(uint64 guid) const
{
    return _getMemberCSlot(guid) != m_memberSlots.end();
}

bool Group::IsLeader(uint64 guid) const
{
    return (GetLeaderGUID() == guid);
}

uint64 Group::GetMemberGUID(const std::string& name)
{
    for (member_citerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
        if (itr->name == name)
            return itr->guid;
    return 0;
}

bool Group::IsAssistant(uint64 guid) const
{
    member_citerator mslot = _getMemberCSlot(guid);
    if (mslot == m_memberSlots.end())
        return false;
    return mslot->flags & MEMBER_FLAG_ASSISTANT;
}

bool Group::SameSubGroup(uint64 guid1, uint64 guid2) const
{
    member_citerator mslot2 = _getMemberCSlot(guid2);
    if (mslot2 == m_memberSlots.end())
        return false;
    return SameSubGroup(guid1, &*mslot2);
}

bool Group::SameSubGroup(uint64 guid1, MemberSlot const* slot2) const
{
    member_citerator mslot1 = _getMemberCSlot(guid1);
    if (mslot1 == m_memberSlots.end() || !slot2)
        return false;
    return (mslot1->group == slot2->group);
}

bool Group::HasFreeSlotSubGroup(uint8 subgroup) const
{
    return (m_subGroupsCounts && m_subGroupsCounts[subgroup] < MAXGROUPSIZE);
}

uint32 Group::GetActiveMarkers() const
{
    uint32 l_Mask = eRaidMarkersFlags::RaidMarkerNone;

    for (RaidMarker const& l_Marker : GetRaidMarkers())
    {
        if (l_Marker.Mask)
            l_Mask |= l_Marker.Mask;
    }

    return l_Mask;
}

uint32 Group::CountActiveMarkers() const
{
    uint32 l_Count = 0;

    for (RaidMarker const& l_Marker : GetRaidMarkers())
    {
        if (l_Marker.Mask)
            ++l_Count;
    }

    return l_Count;
}

void Group::SendRaidMarkersUpdate()
{
    std::vector<RaidMarker> const& l_RaidMarkers = GetRaidMarkers();
    WorldPacket l_Data(Opcodes::SMSG_RAID_MARKERS_CHANGED, 10);
    ByteBuffer l_DataBuffer;

    l_Data << uint8(0); ///< Party Index
    l_Data << uint32(GetActiveMarkers());

    l_Data.WriteBits(CountActiveMarkers(), 3);

    for (RaidMarker const& l_Marker : l_RaidMarkers)
    {
        if (!l_Marker.Mask)
            continue;

        ObjectGuid l_TransGuid = 0;

        uint8 l_BitsOrder[8] = { 6, 7, 4, 0, 5, 1, 3, 2 };
        l_Data.WriteBitInOrder(l_TransGuid, l_BitsOrder);

        l_DataBuffer << float(l_Marker.PosX);
        l_DataBuffer.WriteByteSeq(l_TransGuid[2]);
        l_DataBuffer.WriteByteSeq(l_TransGuid[1]);
        l_DataBuffer.WriteByteSeq(l_TransGuid[7]);
        l_DataBuffer.WriteByteSeq(l_TransGuid[5]);
        l_DataBuffer << float(l_Marker.PosZ);
        l_DataBuffer.WriteByteSeq(l_TransGuid[3]);
        l_DataBuffer.WriteByteSeq(l_TransGuid[0]);
        l_DataBuffer << float(l_Marker.PosY);
        l_DataBuffer.WriteByteSeq(l_TransGuid[4]);
        l_DataBuffer << uint32(l_Marker.MapID);
        l_DataBuffer.WriteByteSeq(l_TransGuid[6]);
    }

    if (l_DataBuffer.size())
        l_Data.append(l_DataBuffer);

    BroadcastPacket(&l_Data, true);
}

void Group::AddRaidMarker(uint8 p_Slot, uint32 p_MapID, float p_X, float p_Y, float p_Z)
{
    if (p_Slot >= eRaidMarkersMisc::MaxRaidMarkers)
        return;

    uint32 l_Mask = eRaidMarkersFlags::RaidMarkerNone;

    RaidMarker l_RaidMarker;
    l_RaidMarker.MapID  = p_MapID;
    l_RaidMarker.PosX   = p_X;
    l_RaidMarker.PosY   = p_Y;
    l_RaidMarker.PosZ   = p_Z;
    l_RaidMarker.Slot   = p_Slot;

    switch (p_Slot)
    {
        case eRaidMarkersMisc::SlotBlue:
            l_Mask = eRaidMarkersFlags::RaidMarkerBlue;
            break;
        case eRaidMarkersMisc::SlotGreen:
            l_Mask = eRaidMarkersFlags::RaidMarkerGreen;
            break;
        case eRaidMarkersMisc::SlotPurple:
            l_Mask = eRaidMarkersFlags::RaidMarkerPurple;
            break;
        case eRaidMarkersMisc::SlotRed:
            l_Mask = eRaidMarkersFlags::RaidMarkerRed;
            break;
        case eRaidMarkersMisc::SlotYellow:
            l_Mask = eRaidMarkersFlags::RaidMarkerYellow;
            break;
        default:
            break;
    }

    l_RaidMarker.Mask = l_Mask;

    m_RaidMarkers[p_Slot] = l_RaidMarker;
    SendRaidMarkersUpdate();
}

void Group::RemoveRaidMarker(uint8 p_Slot)
{
    if (p_Slot >= eRaidMarkersMisc::MaxRaidMarkers)
        return;

    m_RaidMarkers[p_Slot] = RaidMarker();
    SendRaidMarkersUpdate();
}

void Group::RemoveAllRaidMarkers()
{
    for (uint8 l_I = 0; l_I < eRaidMarkersMisc::MaxRaidMarkers; ++l_I)
        m_RaidMarkers[l_I] = RaidMarker();

    SendRaidMarkersUpdate();
}

uint8 Group::GetMemberGroup(uint64 guid) const
{
    member_citerator mslot = _getMemberCSlot(guid);
    if (mslot == m_memberSlots.end())
        return (MAX_RAID_SUBGROUPS+1);
    return mslot->group;
}

void Group::SetBattlegroundGroup(Battleground* bg)
{
    m_bgGroup = bg;
}

void Group::SetBattlefieldGroup(Battlefield *bg)
{
    m_bfGroup = bg;
}

void Group::setGroupMemberRole(uint64 guid, uint32 role)
{
    for (auto member = m_memberSlots.begin(); member != m_memberSlots.end(); ++member)
    {
        if (member->guid == guid)
        {
            member->roles = role;
            break;
        }
    }
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_MEMBER_ROLE);
    if (stmt != nullptr)
    {
        stmt->setUInt8(0, role);
        stmt->setUInt32(1, GUID_LOPART(guid));
        CharacterDatabase.Execute(stmt);
    }
}

uint32 Group::getGroupMemberRole(uint64 guid)
{
    for (auto member = m_memberSlots.begin(); member != m_memberSlots.end(); ++member)
        if (member->guid == guid)
            return member->roles;

    return 0;
}

void Group::SetGroupMemberFlag(uint64 guid, bool apply, GroupMemberFlags flag)
{
    // Assistants, main assistants and main tanks are only available in raid groups
    if (!isRaidGroup())
        return;

    // Check if player is really in the raid
    member_witerator slot = _getMemberWSlot(guid);
    if (slot == m_memberSlots.end())
        return;

    // Do flag specific actions, e.g ensure uniqueness
    switch (flag) {
    case MEMBER_FLAG_MAINASSIST:
        RemoveUniqueGroupMemberFlag(MEMBER_FLAG_MAINASSIST);         // Remove main assist flag from current if any.
        break;
    case MEMBER_FLAG_MAINTANK:
        RemoveUniqueGroupMemberFlag(MEMBER_FLAG_MAINTANK);           // Remove main tank flag from current if any.
        break;
    case MEMBER_FLAG_ASSISTANT:
        break;
    default:
        return;                                                      // This should never happen
    }

    // Switch the actual flag
    ToggleGroupMemberFlag(slot, flag, apply);

    // Preserve the new setting in the db
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_GROUP_MEMBER_FLAG);

    stmt->setUInt8(0, slot->flags);
    stmt->setUInt32(1, GUID_LOPART(guid));

    CharacterDatabase.Execute(stmt);

    // Broadcast the changes to the group
    SendUpdate();
}

Difficulty Group::GetDifficulty(bool isRaid) const
{
    return isRaid ? m_raidDifficulty : m_dungeonDifficulty;
}

Difficulty Group::GetDungeonDifficulty() const
{
    return m_dungeonDifficulty;
}

Difficulty Group::GetRaidDifficulty() const
{
    return m_raidDifficulty;
}

bool Group::isRollLootActive() const
{
    return !RollId.empty();
}

Group::Rolls::iterator Group::GetRoll(uint8 slot)
{
    Rolls::iterator iter;
    for (iter=RollId.begin(); iter != RollId.end(); ++iter)
        if ((*iter)->itemSlot == slot && (*iter)->isValid())
            return iter;
    return RollId.end();
}

void Group::LinkMember(GroupReference* pRef)
{
    m_memberMgr.insertFirst(pRef);
}

void Group::DelinkMember(uint64 guid)
{
    GroupReference* ref = m_memberMgr.getFirst();
    while (ref)
    {
        GroupReference* nextRef = ref->next();
        if (ref->getSource()->GetGUID() == guid)
        {
            ref->unlink();
            break;
        }
        ref = nextRef;
    }
}

Group::BoundInstancesMap& Group::GetBoundInstances(Difficulty difficulty)
{
    return m_boundInstances[difficulty];
}

void Group::_initRaidSubGroupsCounter()
{
    // Sub group counters initialization
    if (!m_subGroupsCounts)
        m_subGroupsCounts = new uint8[MAX_RAID_SUBGROUPS];

    memset((void*)m_subGroupsCounts, 0, (MAX_RAID_SUBGROUPS)*sizeof(uint8));

    for (member_citerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
        ++m_subGroupsCounts[itr->group];
}

Group::member_citerator Group::_getMemberCSlot(uint64 Guid) const
{
    for (member_citerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
        if (itr->guid == Guid)
            return itr;
    return m_memberSlots.end();
}

Group::member_witerator Group::_getMemberWSlot(uint64 Guid)
{
    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
        if (itr->guid == Guid)
            return itr;
    return m_memberSlots.end();
}

void Group::SubGroupCounterIncrease(uint8 subgroup)
{
    if (m_subGroupsCounts)
        ++m_subGroupsCounts[subgroup];
}

void Group::SubGroupCounterDecrease(uint8 subgroup)
{
    if (m_subGroupsCounts)
        --m_subGroupsCounts[subgroup];
}

void Group::RemoveUniqueGroupMemberFlag(GroupMemberFlags flag)
{
    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
        if (itr->flags & flag)
            itr->flags &= ~flag;
}

void Group::ToggleGroupMemberFlag(member_witerator slot, uint8 flag, bool apply)
{
    if (apply)
        slot->flags |= flag;
    else
        slot->flags &= ~flag;
}

void Group::OfflineMemberLost(uint64 guid, uint32 againstMatchmakerRating, uint8 slot, int32 MatchmakerRatingChange)
{
    // Called for offline player after ending rated arena match!
    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
    {
        if (itr->guid == guid)
        {
            if (Player* p = ObjectAccessor::FindPlayer(guid))
            {
                // update personal rating
                int32 mod = Arena::GetRatingMod(p->GetArenaPersonalRating(slot), againstMatchmakerRating, false);
                p->SetArenaPersonalRating(slot, p->GetArenaPersonalRating(slot) + mod);

                // update matchmaker rating
                p->SetArenaMatchMakerRating(slot, p->GetArenaMatchMakerRating(slot) + MatchmakerRatingChange);

                // update personal played stats
                p->IncrementWeekGames(slot);
                p->IncrementSeasonGames(slot);
                return;
            }
        }
    }
}

void Group::MemberLost(Player* player, uint32 againstMatchmakerRating, uint8 slot, int32 MatchmakerRatingChange)
{
    // Called for each participant of a match after losing
    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
    {
        if (itr->guid == player->GetGUID())
        {
            // Update personal rating
            int32 mod = Arena::GetRatingMod(player->GetArenaPersonalRating(slot), againstMatchmakerRating, false);
            player->SetArenaPersonalRating(slot, player->GetArenaPersonalRating(slot) + mod);

            // Update matchmaker rating
            player->SetArenaMatchMakerRating(slot, player->GetArenaMatchMakerRating(slot) + MatchmakerRatingChange);

            // Update personal played stats
            player->IncrementWeekGames(slot);
            player->IncrementSeasonGames(slot);
            return;
        }
    }
}

uint32 Group::GetRating(uint8 slot)
{
    uint32 rating = 0;
    uint32 count = 0;
    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
    {
        if (Player* player = ObjectAccessor::FindPlayer(itr->guid))
        {
            rating += player->GetArenaPersonalRating(slot);
            ++count;
        }
    }

    if (!count)
        count = 1;

    rating /= count;
    return rating;
}

void Group::WonAgainst(uint32 Own_MMRating, uint32 Opponent_MMRating, int32& rating_change, uint8 slot)
{
    // Called when the team has won
    // Change in Matchmaker rating
    int32 mod = Arena::GetMatchmakerRatingMod(Own_MMRating, Opponent_MMRating, true);

    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
    {
        if (Player* player = ObjectAccessor::FindPlayer(itr->guid))
        {
            // Change in Team Rating
            rating_change = Arena::GetRatingMod(player->GetArenaPersonalRating(slot), Opponent_MMRating, true);

            if (player->GetArenaPersonalRating(slot) < 1000 && rating_change < 0)
                rating_change = 0;

            if (player->GetArenaPersonalRating(slot) < 1000)
                rating_change = 96;

            if (player->GetBattleground())
                for (Battleground::BattlegroundScoreMap::const_iterator itr2 = player->GetBattleground()->GetPlayerScoresBegin(); itr2 != player->GetBattleground()->GetPlayerScoresEnd(); ++itr2)
                    if (itr2->first == itr->guid)
                        itr2->second->RatingChange = rating_change;

            player->SetArenaPersonalRating(slot, player->GetArenaPersonalRating(slot) + rating_change);
            player->SetArenaMatchMakerRating(slot, player->GetArenaMatchMakerRating(slot) + mod);

            player->IncrementWeekWins(slot);
            player->IncrementSeasonWins(slot);
            player->IncrementWeekGames(slot);
            player->IncrementSeasonGames(slot);
        }
    }
}

void Group::LostAgainst(uint32 Own_MMRating, uint32 Opponent_MMRating, int32& rating_change, uint8 slot)
{
    // Called when the team has lost
    // Change in Matchmaker Rating
    int32 mod = Arena::GetMatchmakerRatingMod(Own_MMRating, Opponent_MMRating, false);

    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
    {
        if (Player* player = ObjectAccessor::FindPlayer(itr->guid))
        {
            // Change in Team Rating
            rating_change = Arena::GetRatingMod(player->GetArenaPersonalRating(slot), Opponent_MMRating, false);

            if (player->GetArenaPersonalRating(slot) < 1000 && rating_change < 0)
                rating_change = 0;

            if (player->GetBattleground())
                for (Battleground::BattlegroundScoreMap::const_iterator itr2 = player->GetBattleground()->GetPlayerScoresBegin(); itr2 != player->GetBattleground()->GetPlayerScoresEnd(); ++itr2)
                    if (itr2->first == itr->guid)
                        itr2->second->RatingChange = rating_change;

            player->SetArenaPersonalRating(slot, player->GetArenaPersonalRating(slot) + rating_change);
            player->SetArenaMatchMakerRating(slot, player->GetArenaMatchMakerRating(slot) + mod);

            player->IncrementWeekGames(slot);
            player->IncrementSeasonGames(slot);
        }
    }
}

void Group::FinishGame(int32 rating_change, uint8 slot)
{
    for (member_witerator itr = m_memberSlots.begin(); itr != m_memberSlots.end(); ++itr)
    {
        if (Player* player = ObjectAccessor::FindPlayer(itr->guid))
        {
            if (player->GetArenaPersonalRating(slot) < 1000 && rating_change < 0)
                rating_change = 0;

            if (player->GetBattleground())
                for (Battleground::BattlegroundScoreMap::const_iterator itr2 = player->GetBattleground()->GetPlayerScoresBegin(); itr2 != player->GetBattleground()->GetPlayerScoresEnd(); ++itr2)
                    if (itr2->first == itr->guid)
                        itr2->second->RatingChange = rating_change;

            player->SetArenaPersonalRating(slot, player->GetArenaPersonalRating(slot) + rating_change);
            player->IncrementWeekGames(slot);
            player->IncrementSeasonGames(slot);
        }
    }
}

bool Group::CanEnterInInstance()
{
    uint8 maxplayers = 0;

    if (!isRaidGroup())
    {
        maxplayers = 5;
    }
    else
    {
        switch (GetRaidDifficulty())
        {
            case SCENARIO_DIFFICULTY:
            case SCENARIO_HEROIC_DIFFICULTY:
                maxplayers = 3;
                break;
            case HEROIC_DIFFICULTY:
            case CHALLENGE_MODE_DIFFICULTY:
                maxplayers = 5;
                break;
            case MAN10_DIFFICULTY:
            case MAN10_HEROIC_DIFFICULTY:
                maxplayers = 10;
                break;
            case MAN25_DIFFICULTY:
            case MAN25_HEROIC_DIFFICULTY:
            case DYNAMIC_DIFFICULTY:
            case RAID_TOOL_DIFFICULTY:
                maxplayers = 25;
                break;
            case MAN40_DIFFICULTY:
                maxplayers = 40;
                break;
        }
    }

    if (m_membersInInstance < maxplayers)
        return true;
    else
        return false;
}

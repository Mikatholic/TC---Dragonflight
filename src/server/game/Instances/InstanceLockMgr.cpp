/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
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

#include "InstanceLockMgr.h"
#include "DatabaseEnv.h"
#include "DB2Stores.h"
#include "Errors.h"
#include "GameTime.h"
#include "Log.h"
#include "MapManager.h"
#include "Player.h" // for TransferAbortReason
#include "World.h"

InstanceLockData::InstanceLockData() = default;
InstanceLockData::~InstanceLockData() = default;

InstanceLock::InstanceLock(uint32 mapId, Difficulty difficultyId, InstanceResetTimePoint expiryTime, uint32 instanceId)
    : _mapId(mapId), _difficultyId(difficultyId), _instanceId(instanceId), _expiryTime(expiryTime), _extended(false)
{
}

InstanceLock::~InstanceLock() = default;

bool InstanceLock::IsExpired() const
{
    return _expiryTime < GameTime::GetSystemTime();
}

InstanceResetTimePoint InstanceLock::GetEffectiveExpiryTime() const
{
    if (!IsExtended())
        return GetExpiryTime();

    MapDb2Entries entries{ _mapId, _difficultyId };

    // return next reset time
    if (IsExpired())
        return sInstanceLockMgr.GetNextResetTime(entries);

    // if not expired, return expiration time + 1 reset period
    return GetExpiryTime() + Seconds(entries.MapDifficulty->GetRaidDuration());
}

SharedInstanceLockData::SharedInstanceLockData() = default;

SharedInstanceLockData::~SharedInstanceLockData()
{
    // Cleanup database
    if (InstanceId)
        sInstanceLockMgr.OnSharedInstanceLockDataDelete(InstanceId);
}

SharedInstanceLock::SharedInstanceLock(uint32 mapId, Difficulty difficultyId, InstanceResetTimePoint expiryTime, uint32 instanceId,
    std::shared_ptr<SharedInstanceLockData> sharedData)
    : InstanceLock(mapId, difficultyId, expiryTime, instanceId), _sharedData(std::move(sharedData))
{
}

MapDb2Entries::MapDb2Entries(uint32 mapId, Difficulty difficulty)
    : Map(sMapStore.AssertEntry(mapId)), MapDifficulty(ASSERT_NOTNULL(sDB2Manager.GetMapDifficultyData(mapId, difficulty)))
{
}

MapDb2Entries::MapDb2Entries(MapEntry const* map, MapDifficultyEntry const* mapDifficulty)
    : Map(map), MapDifficulty(mapDifficulty)
{
}

InstanceLockKey MapDb2Entries::GetKey() const
{
    return { MapDifficulty->MapID, MapDifficulty->LockID };
}

bool MapDb2Entries::IsInstanceIdBound() const
{
    return !Map->IsFlexLocking() && !MapDifficulty->IsUsingEncounterLocks();
}

InstanceLockUpdateEvent::InstanceLockUpdateEvent(InstanceLockUpdateEvent&& other) noexcept = default;
InstanceLockUpdateEvent& InstanceLockUpdateEvent::operator=(InstanceLockUpdateEvent&&) noexcept = default;
InstanceLockUpdateEvent::~InstanceLockUpdateEvent() = default;

InstanceLockMgr::InstanceLockMgr() = default;

InstanceLockMgr::~InstanceLockMgr() = default;

void InstanceLockMgr::Load()
{
    std::unordered_map<uint32, std::shared_ptr<SharedInstanceLockData>> instanceLockDataById;

    //                                                                 0     1                        2
    if (QueryResult result = CharacterDatabase.Query("SELECT instanceId, data, completedEncountersMask FROM instance"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 instanceId = fields[0].GetUInt32();

            std::shared_ptr<SharedInstanceLockData> data = std::make_shared<SharedInstanceLockData>();
            data->Data = fields[1].GetString();
            data->CompletedEncountersMask = fields[2].GetUInt32();
            data->InstanceId = instanceId;

            instanceLockDataById[instanceId] = std::move(data);

        } while (result->NextRow());
    }

    //                                                          0      1       2           3           4     5                        6           7         8
    if (QueryResult result = CharacterDatabase.Query("SELECT guid, mapId, lockId, instanceId, difficulty, data, completedEncountersMask, expiryTime, extended FROM character_instance_lock"))
    {
        do
        {
            Field* fields = result->Fetch();

            ObjectGuid playerGuid = ObjectGuid::Create<HighGuid::Player>(fields[0].GetUInt64());
            uint32 mapId = fields[1].GetUInt32();
            uint32 lockId = fields[2].GetUInt32();
            uint32 instanceId = fields[3].GetUInt32();
            Difficulty difficulty = Difficulty(fields[4].GetUInt8());
            InstanceResetTimePoint expiryTime = std::chrono::system_clock::from_time_t(time_t(fields[7].GetUInt64()));

            // Mark instance id as being used
            sMapMgr->RegisterInstanceId(instanceId);

            InstanceLock* instanceLock;
            if (MapDb2Entries{ mapId, difficulty }.IsInstanceIdBound())
            {
                auto sharedDataItr = instanceLockDataById.find(instanceId);
                if (sharedDataItr == instanceLockDataById.end())
                {
                    TC_LOG_ERROR("instance.locks", "Missing instance data for instance id based lock (id %u)", instanceId);
                    CharacterDatabase.PQuery("DELETE FROM character_instance_lock WHERE instanceId = %u", instanceId);
                    continue;
                }

                instanceLock = new SharedInstanceLock(mapId, difficulty, expiryTime, instanceId, sharedDataItr->second);
                _instanceLockDataById[instanceId] = std::weak_ptr<SharedInstanceLockData>(sharedDataItr->second);
            }
            else
                instanceLock = new InstanceLock(mapId, difficulty, expiryTime, instanceId);

            instanceLock->GetData()->Data = fields[5].GetString();
            instanceLock->GetData()->CompletedEncountersMask = fields[6].GetUInt32();
            instanceLock->SetExtended(fields[8].GetBool());

            _instanceLocksByPlayer[playerGuid][InstanceLockKey{ mapId, lockId }].reset(instanceLock);

        } while (result->NextRow());
    }
}

void InstanceLockMgr::Unload()
{
    _unloading = true;
    _instanceLocksByPlayer.clear();
    _instanceLockDataById.clear();
}

TransferAbortReason InstanceLockMgr::CanJoinInstanceLock(ObjectGuid const& playerGuid, MapDb2Entries const& entries, InstanceLock const* instanceLock) const
{
    if (!entries.MapDifficulty->HasResetSchedule())
        return TRANSFER_ABORT_NONE;

    InstanceLock const* playerInstanceLock = FindActiveInstanceLock(playerGuid, entries);
    if (!playerInstanceLock)
        return TRANSFER_ABORT_NONE;

    if (entries.Map->IsFlexLocking())
    {
        // compare completed encounters - if instance has any encounters unkilled in players lock then cannot enter
        if (playerInstanceLock->GetData()->CompletedEncountersMask & ~instanceLock->GetData()->CompletedEncountersMask)
            return TRANSFER_ABORT_ALREADY_COMPLETED_ENCOUNTER;

        return TRANSFER_ABORT_NONE;
    }

    if (!entries.MapDifficulty->IsUsingEncounterLocks() && playerInstanceLock->GetInstanceId() && playerInstanceLock->GetInstanceId() != instanceLock->GetInstanceId())
        return TRANSFER_ABORT_LOCKED_TO_DIFFERENT_INSTANCE;

    return TRANSFER_ABORT_NONE;
}

InstanceLock* InstanceLockMgr::FindInstanceLock(LockMap const& locks, ObjectGuid const& playerGuid, MapDb2Entries const& entries)
{
    auto playerLocksItr = locks.find(playerGuid);
    if (playerLocksItr == locks.end())
        return nullptr;

    auto lockItr = playerLocksItr->second.find(entries.GetKey());
    if (lockItr == playerLocksItr->second.end())
        return nullptr;

    return lockItr->second.get();
}

InstanceLock* InstanceLockMgr::FindActiveInstanceLock(ObjectGuid const& playerGuid, MapDb2Entries const& entries) const
{
    return FindActiveInstanceLock(playerGuid, entries, false, true);
}

InstanceLock* InstanceLockMgr::FindActiveInstanceLock(ObjectGuid const& playerGuid, MapDb2Entries const& entries, bool ignoreTemporary, bool ignoreExpired) const
{
    InstanceLock* lock = FindInstanceLock(_instanceLocksByPlayer, playerGuid, entries);

    // Ignore expired and not extended locks
    if (lock && (!lock->IsExpired() || lock->IsExtended() || !ignoreExpired))
        return lock;

    if (ignoreTemporary)
        return nullptr;

    return FindInstanceLock(_temporaryInstanceLocksByPlayer, playerGuid, entries);
}

std::vector<InstanceLock const*> InstanceLockMgr::GetInstanceLocksForPlayer(ObjectGuid const& playerGuid) const
{
    std::vector<InstanceLock const*> locks;
    auto playerLocksItr = _instanceLocksByPlayer.find(playerGuid);
    if (playerLocksItr != _instanceLocksByPlayer.end())
    {
        locks.reserve(playerLocksItr->second.size());
        std::transform(playerLocksItr->second.begin(), playerLocksItr->second.end(), std::back_inserter(locks),
            [](PlayerLockMap::value_type const& p) { return p.second.get(); });
    }

    return locks;
}

InstanceLock* InstanceLockMgr::CreateInstanceLockForNewInstance(ObjectGuid const& playerGuid, MapDb2Entries const& entries, uint32 instanceId)
{
    if (!entries.MapDifficulty->HasResetSchedule())
        return nullptr;

    InstanceLock* instanceLock;
    if (entries.IsInstanceIdBound())
    {
        std::shared_ptr<SharedInstanceLockData> sharedData = std::make_shared<SharedInstanceLockData>();
        _instanceLockDataById[instanceId] = sharedData;
        instanceLock = new SharedInstanceLock(entries.MapDifficulty->MapID, Difficulty(entries.MapDifficulty->DifficultyID),
            GetNextResetTime(entries), 0, std::move(sharedData));
    }
    else
        instanceLock = new InstanceLock(entries.MapDifficulty->MapID, Difficulty(entries.MapDifficulty->DifficultyID),
            GetNextResetTime(entries), 0);

    _temporaryInstanceLocksByPlayer[playerGuid][entries.GetKey()].reset(instanceLock);
    TC_LOG_DEBUG("instance.locks", "[%u-%s | %u-%s] Created new temporary instance lock for %s in instance %u",
        entries.Map->ID, entries.Map->MapName[sWorld->GetDefaultDbcLocale()],
        uint32(entries.MapDifficulty->DifficultyID), sDifficultyStore.AssertEntry(entries.MapDifficulty->DifficultyID)->Name[sWorld->GetDefaultDbcLocale()],
        playerGuid.ToString().c_str(), instanceId);
    return instanceLock;
}

InstanceLock* InstanceLockMgr::UpdateInstanceLockForPlayer(CharacterDatabaseTransaction trans, ObjectGuid const& playerGuid,
    MapDb2Entries const& entries, InstanceLockUpdateEvent&& updateEvent)
{
    InstanceLock* instanceLock = FindActiveInstanceLock(playerGuid, entries, true, true);
    if (!instanceLock)
    {
        // Move lock from temporary storage if it exists there
        // This is to avoid destroying expired locks before any boss is killed in a fresh lock
        // player can still change his mind, exit instance and reactivate old lock
        auto playerLocksItr = _temporaryInstanceLocksByPlayer.find(playerGuid);
        if (playerLocksItr != _temporaryInstanceLocksByPlayer.end())
        {
            auto lockItr = playerLocksItr->second.find(entries.GetKey());
            if (lockItr != playerLocksItr->second.end())
            {
                instanceLock = lockItr->second.release();
                _instanceLocksByPlayer[playerGuid][entries.GetKey()].reset(instanceLock);

                playerLocksItr->second.erase(lockItr);
                if (playerLocksItr->second.empty())
                    _temporaryInstanceLocksByPlayer.erase(playerLocksItr);

                TC_LOG_DEBUG("instance.locks", "[%u-%s | %u-%s] Promoting temporary lock to permanent for %s in instance %u",
                    entries.Map->ID, entries.Map->MapName[sWorld->GetDefaultDbcLocale()],
                    uint32(entries.MapDifficulty->DifficultyID), sDifficultyStore.AssertEntry(entries.MapDifficulty->DifficultyID)->Name[sWorld->GetDefaultDbcLocale()],
                    playerGuid.ToString().c_str(), updateEvent.InstanceId);
            }
        }
    }

    if (!instanceLock)
    {
        if (entries.IsInstanceIdBound())
        {
            auto sharedDataItr = _instanceLockDataById.find(updateEvent.InstanceId);
            ASSERT(sharedDataItr != _instanceLockDataById.end());
            ASSERT(!sharedDataItr->second.expired());

            instanceLock = new SharedInstanceLock(entries.MapDifficulty->MapID, Difficulty(entries.MapDifficulty->DifficultyID),
                GetNextResetTime(entries), updateEvent.InstanceId, sharedDataItr->second.lock());
            ASSERT(static_cast<SharedInstanceLock*>(instanceLock)->GetSharedData()->InstanceId == updateEvent.InstanceId);
        }
        else
            instanceLock = new InstanceLock(entries.MapDifficulty->MapID, Difficulty(entries.MapDifficulty->DifficultyID),
                GetNextResetTime(entries), updateEvent.InstanceId);

        _instanceLocksByPlayer[playerGuid][entries.GetKey()].reset(instanceLock);
        TC_LOG_DEBUG("instance.locks", "[%u-%s | %u-%s] Created new instance lock for %s in instance %u",
            entries.Map->ID, entries.Map->MapName[sWorld->GetDefaultDbcLocale()],
            uint32(entries.MapDifficulty->DifficultyID), sDifficultyStore.AssertEntry(entries.MapDifficulty->DifficultyID)->Name[sWorld->GetDefaultDbcLocale()],
            playerGuid.ToString().c_str(), updateEvent.InstanceId);
    }
    else
    {
        if (entries.IsInstanceIdBound())
        {
            ASSERT(!instanceLock->GetInstanceId() || instanceLock->GetInstanceId() == updateEvent.InstanceId);
            auto sharedDataItr = _instanceLockDataById.find(updateEvent.InstanceId);
            ASSERT(sharedDataItr != _instanceLockDataById.end());
            ASSERT(sharedDataItr->second.lock().get() == static_cast<SharedInstanceLock*>(instanceLock)->GetSharedData());
        }

        instanceLock->SetInstanceId(updateEvent.InstanceId);
    }

    instanceLock->GetData()->Data = std::move(updateEvent.NewData);
    if (updateEvent.CompletedEncounter)
    {
        instanceLock->GetData()->CompletedEncountersMask |= 1u << updateEvent.CompletedEncounter->Bit;
        TC_LOG_DEBUG("instance.locks", "[%u-%s | %u-%s] Instance lock for %s in instance %u gains completed encounter [%u-%s]",
            entries.Map->ID, entries.Map->MapName[sWorld->GetDefaultDbcLocale()],
            uint32(entries.MapDifficulty->DifficultyID), sDifficultyStore.AssertEntry(entries.MapDifficulty->DifficultyID)->Name[sWorld->GetDefaultDbcLocale()],
            playerGuid.ToString().c_str(), updateEvent.InstanceId,
            updateEvent.CompletedEncounter->ID, updateEvent.CompletedEncounter->Name[sWorld->GetDefaultDbcLocale()]);
    }

    // Synchronize map completed encounters into players completed encounters for UI
    if (!entries.MapDifficulty->IsUsingEncounterLocks())
        instanceLock->GetData()->CompletedEncountersMask |= updateEvent.InstanceCompletedEncountersMask;

    if (instanceLock->IsExpired())
    {
        ASSERT(instanceLock->IsExtended(), "Instance lock must have been extended to create instance map from it");
        instanceLock->SetExpiryTime(GetNextResetTime(entries));
        instanceLock->SetExtended(false);
        TC_LOG_DEBUG("instance.locks", "[%u-%s | %u-%s] Expired instance lock for %s in instance %u is now active",
            entries.Map->ID, entries.Map->MapName[sWorld->GetDefaultDbcLocale()],
            uint32(entries.MapDifficulty->DifficultyID), sDifficultyStore.AssertEntry(entries.MapDifficulty->DifficultyID)->Name[sWorld->GetDefaultDbcLocale()],
            playerGuid.ToString().c_str(), updateEvent.InstanceId);
    }

    // TODO: DB SAVE IN TRANSACTION
    trans->PAppend("DELETE FROM character_instance_lock WHERE guid=" UI64FMTD " AND mapId=%u AND lockId=%d",
        playerGuid.GetCounter(),
        entries.MapDifficulty->MapID,
        entries.MapDifficulty->LockID);
    std::string escapedData = instanceLock->GetData()->Data;
    CharacterDatabase.EscapeString(escapedData);
    trans->PAppend("INSERT INTO character_instance_lock (guid, mapId, lockId, instanceId, difficulty, data, completedEncountersMask, entranceWorldSafeLocId, expiryTime, extended) VALUES (" UI64FMTD ", %u, %d, %u, %d, \"%s\", %u, %u, " UI64FMTD ", %d)",
        playerGuid.GetCounter(),
        entries.MapDifficulty->MapID,
        entries.MapDifficulty->LockID,
        instanceLock->GetInstanceId(),
        entries.MapDifficulty->DifficultyID,
        escapedData.c_str(),
        instanceLock->GetData()->CompletedEncountersMask,
        instanceLock->GetData()->EntranceWorldSafeLocId,
        uint64(std::chrono::system_clock::to_time_t(instanceLock->GetExpiryTime())),
        instanceLock->IsExtended() ? 1 : 0);

    return instanceLock;
}

void InstanceLockMgr::UpdateSharedInstanceLock(CharacterDatabaseTransaction trans, InstanceLockUpdateEvent&& updateEvent)
{
    auto sharedDataItr = _instanceLockDataById.find(updateEvent.InstanceId);
    ASSERT(sharedDataItr != _instanceLockDataById.end());
    std::shared_ptr<SharedInstanceLockData> sharedData = sharedDataItr->second.lock();
    ASSERT(sharedData && sharedData->InstanceId == updateEvent.InstanceId);
    sharedData->Data = std::move(updateEvent.NewData);
    if (updateEvent.CompletedEncounter)
    {
        sharedData->CompletedEncountersMask |= 1u << updateEvent.CompletedEncounter->Bit;
        TC_LOG_DEBUG("instance.locks", "Instance %u gains completed encounter [%u-%s]",
            updateEvent.InstanceId, updateEvent.CompletedEncounter->ID, updateEvent.CompletedEncounter->Name[sWorld->GetDefaultDbcLocale()]);
    }

    trans->PAppend("DELETE FROM instance2 WHERE instanceId=%u",
        sharedData->InstanceId);
    std::string escapedData = sharedData->Data;
    CharacterDatabase.EscapeString(escapedData);
    trans->PAppend("INSERT INTO instance2 (instanceId, data, completedEncountersMask, entranceWorldSafeLocId) VALUES (%u, \"%s\", %u, %u)",
        sharedData->InstanceId,
        escapedData.c_str(),
        sharedData->CompletedEncountersMask,
        sharedData->EntranceWorldSafeLocId);
}

void InstanceLockMgr::OnSharedInstanceLockDataDelete(uint32 instanceId)
{
    if (_unloading)
        return;

    _instanceLockDataById.erase(instanceId);
    CharacterDatabase.PExecute("DELETE FROM instance2 WHERE instanceId=%u", instanceId);
    TC_LOG_DEBUG("instance.locks", "Deleting instance %u as it is no longer referenced by any player", instanceId);
}

void InstanceLockMgr::UpdateInstanceLockExtensionForPlayer(ObjectGuid const& playerGuid, MapDb2Entries const& entries, bool extended)
{
    if (InstanceLock* instanceLock = FindActiveInstanceLock(playerGuid, entries, true, false))
    {
        instanceLock->SetExtended(extended);
        CharacterDatabase.PExecute("UPDATE character_instance_lock SET extended = %d WHERE guid = " UI64FMTD " AND mapId = %u AND lockId = %d",
            extended ? 1 : 0,
            playerGuid.GetCounter(),
            entries.MapDifficulty->MapID,
            entries.MapDifficulty->LockID);

        TC_LOG_DEBUG("instance.locks", "[%u-%s | %u-%s] Instance lock for %s is %s extended",
            entries.Map->ID, entries.Map->MapName[sWorld->GetDefaultDbcLocale()],
            uint32(entries.MapDifficulty->DifficultyID), sDifficultyStore.AssertEntry(entries.MapDifficulty->DifficultyID)->Name[sWorld->GetDefaultDbcLocale()],
            playerGuid.ToString().c_str(), extended ? "now" : "no longer");
    }
}

#include "Config.h"

InstanceResetTimePoint InstanceLockMgr::GetNextResetTime(MapDb2Entries const& entries)
{
    tm dateTime = *GameTime::GetDateAndTime();
    dateTime.tm_sec = 0;
    dateTime.tm_min = 0;
    int32 resetHour = sConfigMgr->GetIntDefault("ResetSchedule.DailyHour", 9);
    switch (entries.MapDifficulty->ResetInterval)
    {
        case MAP_DIFFICULTY_RESET_DAILY:
        {
            if (dateTime.tm_hour >= resetHour)
                ++dateTime.tm_mday;

            dateTime.tm_hour = resetHour;
            break;
        }
        case MAP_DIFFICULTY_RESET_WEEKLY:
        {
            int32 resetDay = sConfigMgr->GetIntDefault("ResetSchedule.WeeklyDay", 2);
            int32 daysAdjust = resetDay - dateTime.tm_wday;
            if (dateTime.tm_wday > resetDay || (dateTime.tm_wday == resetDay && dateTime.tm_hour >= resetHour))
                daysAdjust += 7; // passed it for current week, grab time from next week

            dateTime.tm_hour = resetHour;
            dateTime.tm_mday += daysAdjust;
            break;
        }
        default:
            break;
    }

    return std::chrono::system_clock::from_time_t(mktime(&dateTime));
}

InstanceLockMgr& InstanceLockMgr::Instance()
{
    static InstanceLockMgr instance;
    return instance;
}
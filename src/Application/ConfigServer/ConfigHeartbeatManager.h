#ifndef CONFIGHEARTBEATMANAGER_H
#define CONFIGHEARTBEATMANAGER_H

#include "System/Containers/InSortedList.h"
#include "System/Events/Countdown.h"
#include "Application/Common/ClusterMessage.h"

class Heartbeat;        // forward
class ConfigServer;     // forward

#define DEFAULT_SHARD_SPLIT_SIZE        500*MB

/*
===============================================================================================

 ConfigHeartbeatManager

===============================================================================================
*/

class ConfigHeartbeatManager
{
    typedef InSortedList<Heartbeat>     HeartbeatList;

public:
    void                Init(ConfigServer* configServer);
    void                Shutdown();

    void                SetShardSplitSize(uint64_t shardSplitSize);
    uint64_t            GetShardSplitSize();

    void                SetHeartbeatExpireTimeout(uint64_t heartbeatExpireTimeout);
    uint64_t            GetHeartbeatExpireTimeout();

    void                SetShardSplitCooldownTime(uint64_t shardSplitCooldownTime);
    uint64_t            GetShardSplitCooldownTime();

    void                OnHeartbeatMessage(ClusterMessage& message);
    void                OnHeartbeatTimeout();

    bool                HasHeartbeat(uint64_t nodeID);
    unsigned            GetNumHeartbeats();
    HeartbeatList&      GetHeartbeats();

private:
    void                RegisterHeartbeat(uint64_t nodeID);
    void                TrySplitShardActions(ClusterMessage& message);
    bool                IsSplitCreating(ConfigQuorum* configQuorum, uint64_t& newShardID);

    ConfigServer*       configServer;
    HeartbeatList       heartbeats;
    Countdown           heartbeatTimeout;
    uint64_t            shardSplitSize;
    uint64_t            heartbeatExpireTimeout;
    uint64_t            lastSplitTime;
    uint64_t            shardSplitCooldownTime;
};

/*
===============================================================================================

 Heartbeat

===============================================================================================
*/

class Heartbeat
{
public:

    Heartbeat()     { prev = next = this; }

    uint64_t        nodeID;
    uint64_t        expireTime;
    
    Heartbeat*      prev;
    Heartbeat*      next;
};

inline bool LessThan(Heartbeat &a, Heartbeat &b)
{
    return (a.expireTime < b.expireTime);
}

#endif

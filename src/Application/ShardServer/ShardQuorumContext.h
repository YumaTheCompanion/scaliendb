#ifndef SHARDQUORUMCONTEXT_H
#define SHARDQUORUMCONTEXT_H

#include "Framework/Replication/Quorums/QuorumContext.h"
#include "Framework/Replication/Quorums/TotalQuorum.h"
#include "Framework/Replication/ReplicatedLog/ReplicatedLog.h"
#include "Framework/Replication/PaxosLease/PaxosLease.h"
#include "Application/ConfigState/ConfigQuorum.h"
#include "ShardMessage.h"

class ShardQuorumProcessor; // forward

/*
===============================================================================================

 ShardQuorumContext

===============================================================================================
*/

class ShardQuorumContext : public QuorumContext
{
public:
    void                            Init(ConfigQuorum* configQuorum,
                                     ShardQuorumProcessor* quorumProcessor_, StorageTable* table);
    
    void                            SetActiveNodes(ConfigQuorum::NodeList& activeNodes);
    void                            TryReplicationCatchup();
    void                            Append(); // nextValue was filled up using GetNextValue()
    bool                            IsAppending();
    
    // ========================================================================================
    // QuorumContext interface:
    //
    virtual bool                    IsLeaseOwner();
    virtual bool                    IsLeaseKnown();
    virtual uint64_t                GetLeaseOwner();
    // multiPaxos
    virtual bool                    IsLeader();

    virtual void                    OnLearnLease();
    virtual void                    OnLeaseTimeout();

    virtual uint64_t                GetQuorumID();
    virtual void                    SetPaxosID(uint64_t paxosID);
    virtual uint64_t                GetPaxosID();
    virtual uint64_t                GetHighestPaxosID();
    
    virtual Quorum*                 GetQuorum();
    virtual QuorumDatabase*         GetDatabase();
    virtual QuorumTransport*        GetTransport();
    
    virtual void                    OnAppend(ReadBuffer value, bool ownAppend);
    virtual Buffer&                 GetNextValue();
    virtual void                    OnMessage(uint64_t nodeID, ReadBuffer msg);
    virtual void                    OnStartCatchup();
    virtual void                    OnCatchupComplete(uint64_t paxosID);

    virtual void                    StopReplication();
    virtual void                    ContinueReplication();

private:
    void                            OnPaxosMessage(ReadBuffer buffer);
    void                            OnCatchupMessage(ReadBuffer buffer);
    void                            RegisterPaxosID(uint64_t paxosID);

    bool                            isReplicationActive;
    uint64_t                        quorumID;
    uint64_t                        highestPaxosID;
    ShardQuorumProcessor*           quorumProcessor;
    TotalQuorum                     quorum;
    QuorumDatabase                  database;
    QuorumTransport                 transport;
    ReplicatedLog                   replicatedLog;
    Buffer                          nextValue;
};

#endif
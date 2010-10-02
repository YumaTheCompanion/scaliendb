#ifndef CONFIGQUORUM_H
#define CONFIGQUORUM_H

#include "System/Common.h"
#include "System/Containers/List.h"
#include "System/Containers/ArrayList.h"

// TODO: create ConfigConsts.h and define there
#define CONFIG_MAX_NODES                7

#define CONFIG_QUORUM_PRODUCTION        'P'
#define CONFIG_QUORUM_TEST              'T'

/*
===============================================================================================

 ConfigQuorum

===============================================================================================
*/

class ConfigQuorum
{
public:
    typedef ArrayList<uint64_t, CONFIG_MAX_NODES> NodeList;
    ConfigQuorum()      { prev = next = this; hasPrimary = false; }

    uint64_t            quorumID;
    NodeList            activeNodes;
    NodeList            inactiveNodes;
    List<uint64_t>      shards;
    
    // ========================================================================================
    //
    // Not replicated, only stored by the MASTER in-memory
    bool                hasPrimary;
    uint64_t            primaryID;
    //
    // ========================================================================================

    ConfigQuorum*       prev;
    ConfigQuorum*       next;
};

#endif
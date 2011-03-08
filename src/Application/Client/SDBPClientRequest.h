#ifndef SDBPCLIENTREQUEST_H
#define SDBPCLIENTREQUEST_H

#include "System/Containers/List.h"
#include "System/Containers/InTreeMap.h"
#include "Application/Common/ClientRequest.h"
#include "Application/Common/ClientResponse.h"

namespace SDBPClient
{

/*
===============================================================================================

 SDBPClient::Request

===============================================================================================
*/

class Request : public ClientRequest
{
public:
    typedef InTreeNode<Request>     TreeNode;
    typedef List<ClientResponse*>   ResponseList;

    Request();

    TreeNode        treeNode;
    int             status;
    uint64_t        requestTime;
    uint64_t        responseTime;
    unsigned        numTry;
    unsigned        numQuorums;
    unsigned        numShardServers;
    unsigned        numBulkResponses;
    ResponseList    responses;
    bool            async;
};

};  // namespace

#endif

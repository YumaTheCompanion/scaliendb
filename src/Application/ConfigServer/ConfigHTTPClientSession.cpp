#include "ConfigHTTPClientSession.h"
#include "ConfigServer.h"
#include "JSONConfigState.h"
#include "System/Config.h"
#include "Application/Common/ContextTransport.h"
#include "Version.h"
#include "ConfigHeartbeatManager.h"

void ConfigHTTPClientSession::SetConfigServer(ConfigServer* configServer_)
{
    configServer = configServer_;
}

void ConfigHTTPClientSession::SetConnection(HTTPConnection* conn)
{
    session.SetConnection(conn);
    conn->SetOnClose(MFUNC(ConfigHTTPClientSession, OnConnectionClose));
}

bool ConfigHTTPClientSession::HandleRequest(HTTPRequest& request)
{
    ReadBuffer  cmd;
    
    session.ParseRequest(request, cmd, params);
    return ProcessCommand(cmd);
}

void ConfigHTTPClientSession::OnComplete(ClientRequest* request, bool last)
{
    Buffer          tmp;
    ReadBuffer      rb;
    ClientResponse* response;

    response = &request->response;
    switch (response->type)
    {
    case CLIENTRESPONSE_OK:
        session.Print(ReadBuffer("OK"));
        break;
    case CLIENTRESPONSE_NUMBER:
        tmp.Writef("%U", response->number);
        rb.Wrap(tmp);
        session.Print(rb);
        break;
    case CLIENTRESPONSE_VALUE:
        session.Print(response->value);
        break;
    case CLIENTRESPONSE_CONFIG_STATE:
        if (!last)
        {
            response->configState.Write(tmp, true);
            rb.Wrap(tmp);
            session.Print(rb);
            configServer->OnClientClose(this);
        }
        break;
    case CLIENTRESPONSE_NOSERVICE:
        session.Print(ReadBuffer("NOSERVICE"));
        break;
    case CLIENTRESPONSE_FAILED:
        session.Print(ReadBuffer("FAILED"));
        break;
    }
    
    if (last)
    {
        session.Flush();        
        delete request;
    }
}

bool ConfigHTTPClientSession::IsActive()
{
    return true;
}

void ConfigHTTPClientSession::PrintStatus()
{
    Buffer          buf;
    ConfigState*    configState;

    session.PrintPair("ScalienDB", "Controller");
    session.PrintPair("Version", VERSION_STRING);

    buf.Writef("%d", (int) configServer->GetNodeID());
    buf.NullTerminate();
    session.PrintPair("NodeID", buf.GetBuffer());   

    buf.Writef("%d", (int) configServer->GetQuorumProcessor()->GetMaster());
    buf.NullTerminate();
    session.PrintPair("Master", buf.GetBuffer());

    buf.Writef("%d", (int) configServer->GetQuorumProcessor()->GetPaxosID());
    buf.NullTerminate();
    session.PrintPair("Round", buf.GetBuffer());
    
    session.PrintPair("Controllers", configFile.GetValue("controllers", ""));
    
    session.Print("\n--- Configuration State ---\n");
    
    configState = configServer->GetDatabaseManager()->GetConfigState();
    PrintShardServers(configState);
    session.Print("");
    PrintQuorumMatrix(configState);
    session.Print("");
    PrintDatabases(configState);
    session.Print("");
    PrintShardMatrix(configState);
    
    session.Flush();
}

void ConfigHTTPClientSession::PrintShardServers(ConfigState* configState)
{
    ConfigShardServer*      it;
    Buffer                  buffer;
    ReadBuffer              rb;
    uint64_t                ssID;
    
    if (configState->shardServers.GetLength() == 0)
    {
        session.Print("No shard servers configured");
    }
    else
    {
        session.Print("Shard servers:\n");
        ConfigState::ShardServerList& shardServers = configState->shardServers;
        for (it = shardServers.First(); it != NULL; it = shardServers.Next(it))
        {
            if (configServer->GetHeartbeatManager()->HasHeartbeat(it->nodeID))
            {
                if (CONTEXT_TRANSPORT->IsConnected(it->nodeID))
                    buffer.Writef("+ ");
                else
                    buffer.Writef("! ");
            }
            else
            {
                if (CONTEXT_TRANSPORT->IsConnected(it->nodeID))
                    buffer.Writef("* ");
                else
                    buffer.Writef("- ");
            }
            rb = it->endpoint.ToReadBuffer();
            ssID = it->nodeID - CONFIG_MIN_SHARD_NODE_ID;
            buffer.Appendf("ss%U (%R)", ssID, &rb);
            session.Print(buffer);
        }
    }
}

void ConfigHTTPClientSession::PrintQuorumMatrix(ConfigState* configState)
{
    bool                    found;
    ConfigShardServer*      itShardServer;
    ConfigQuorum*           itQuorum;
    uint64_t*               itNodeID;
    Buffer                  buffer;
    uint64_t                ssID;
    
    if (configState->shardServers.GetLength() == 0 || configState->quorums.GetLength() == 0)
        return;
    
    session.Print("Quorum matrix:\n");
    ConfigState::ShardServerList& shardServers = configState->shardServers;
    ConfigState::QuorumList& quorums = configState->quorums;

    buffer.Writef("       ");
    for (itShardServer = shardServers.First(); itShardServer != NULL; itShardServer = shardServers.Next(itShardServer))
    {
        ssID = itShardServer->nodeID - CONFIG_MIN_SHARD_NODE_ID;
        if (ssID < 10)
            buffer.Appendf("   ");
        else if (ssID < 100)
            buffer.Appendf("  ");
        else if (ssID < 1000)
            buffer.Appendf(" ");
        else
            buffer.Appendf("");
        buffer.Appendf("ss%U", ssID);
    }
    session.Print(buffer);

    buffer.Writef("      +");
    for (itShardServer = shardServers.First(); itShardServer != NULL; itShardServer = shardServers.Next(itShardServer))
    {
        buffer.Appendf("------");
    }
    session.Print(buffer);
    
    for (itQuorum = quorums.First(); itQuorum != NULL; itQuorum = quorums.Next(itQuorum))
    {
        if (itQuorum->quorumID < 10)
            buffer.Writef("   ");
        else if (itQuorum->quorumID < 100)
            buffer.Writef("  ");
        else if (itQuorum->quorumID < 1000)
            buffer.Writef(" ");
        else
            buffer.Writef("");
        buffer.Appendf("q%U |", itQuorum->quorumID);
        ConfigQuorum::NodeList& activeNodes = itQuorum->activeNodes;
        ConfigQuorum::NodeList& inactiveNodes = itQuorum->inactiveNodes;
        for (itShardServer = shardServers.First(); itShardServer != NULL; itShardServer = shardServers.Next(itShardServer))
        {
            found = false;
            FOREACH(itNodeID, activeNodes)
            {
                if (itShardServer->nodeID == *itNodeID)
                {
                    found = true;
                    if (itQuorum->hasPrimary && itQuorum->primaryID == *itNodeID)
                        if (configServer->GetHeartbeatManager()->HasHeartbeat(*itNodeID) &&
                         CONTEXT_TRANSPORT->IsConnected(*itNodeID))
                            buffer.Appendf("     P");
                        else
                            buffer.Appendf("     !");
                    else
                    {
                        if (configServer->GetHeartbeatManager()->HasHeartbeat(*itNodeID))
                            buffer.Appendf("     +");
                        else
                            buffer.Appendf("     -");
                    }
                    break;
                }
            }
            FOREACH(itNodeID, inactiveNodes)
            {
                if (itShardServer->nodeID == *itNodeID)
                {
                    found = true;
                    buffer.Appendf("     i");
                    break;
                }
            }
            if (!found)
                buffer.Appendf("      ");

        }
        session.Print(buffer);
    }
}

void ConfigHTTPClientSession::PrintDatabases(ConfigState* configState)
{
    ConfigDatabase*     itDatabase;
    uint64_t*           itTableID;
    uint64_t*           itShardID;
    ConfigTable*        table;
    ConfigShard*        shard;
    Buffer              buffer;
    
    session.Print("Databases, tables and shards:\n");
    
    ConfigState::DatabaseList& databases = configState->databases;
    for (itDatabase = databases.First(); itDatabase != NULL; itDatabase = databases.Next(itDatabase))
    {
        buffer.Writef("- %B(d%u)", &itDatabase->name, itDatabase->databaseID);
        List<uint64_t>& tables = itDatabase->tables;
        if (tables.GetLength() > 0)
            buffer.Appendf(":");
        session.Print(buffer);
        for (itTableID = tables.First(); itTableID != NULL; itTableID = tables.Next(itTableID))
        {
            table = configState->GetTable(*itTableID);
            buffer.Writef("  - %B(t%U): [", &table->name, table->tableID);
            List<uint64_t>& shards = table->shards;
            for (itShardID = shards.First(); itShardID != NULL; itShardID = shards.Next(itShardID))
            {
                shard = configState->GetShard(*itShardID);
                buffer.Appendf("s%U => q%U", *itShardID, shard->quorumID);
                if (shards.Next(itShardID) != NULL)
                    buffer.Appendf(", ");
            }
            buffer.Appendf("]");
            session.Print(buffer);
        }
        session.Print("");
    }
}

void ConfigHTTPClientSession::PrintShardMatrix(ConfigState* configState)
{
    bool                    found;
    ConfigShardServer*      itShardServer;
    ConfigShard*            itShard;
    ConfigQuorum*           quorum;
    uint64_t*               itNodeID;
    Buffer                  buffer;
    uint64_t                ssID;
    
    if (configState->shardServers.GetLength() == 0 ||
     configState->quorums.GetLength() == 0 ||
     configState->shards.GetLength() == 0)
        return;
    
    session.Print("Shard matrix:\n");
    ConfigState::ShardServerList& shardServers = configState->shardServers;
    ConfigState::ShardList& shards = configState->shards;
    
    buffer.Writef("       ");
    for (itShardServer = shardServers.First(); itShardServer != NULL; itShardServer = shardServers.Next(itShardServer))
    {
        ssID = itShardServer->nodeID - CONFIG_MIN_SHARD_NODE_ID;
        if (ssID < 10)
            buffer.Appendf("   ");
        else if (ssID < 100)
            buffer.Appendf("  ");
        else if (ssID < 1000)
            buffer.Appendf(" ");
        else
            buffer.Appendf("");
        buffer.Appendf("ss%U", ssID);
    }
    session.Print(buffer);

    buffer.Writef("      +");
    for (itShardServer = shardServers.First(); itShardServer != NULL; itShardServer = shardServers.Next(itShardServer))
    {
        buffer.Appendf("------");
    }
    session.Print(buffer);
    
    for (itShard = shards.First(); itShard != NULL; itShard = shards.Next(itShard))
    {
        if (itShard->shardID < 10)
            buffer.Writef("   ");
        else if (itShard->shardID < 100)
            buffer.Writef("  ");
        else if (itShard->shardID < 1000)
            buffer.Writef(" ");
        else
            buffer.Writef("");
        buffer.Appendf("s%U |", itShard->shardID);
        quorum = configState->GetQuorum(itShard->quorumID);
        ConfigQuorum::NodeList& activeNodes = quorum->activeNodes;
        for (itShardServer = shardServers.First(); itShardServer != NULL; itShardServer = shardServers.Next(itShardServer))
        {
            found = false;
            for (itNodeID = activeNodes.First(); itNodeID != NULL; itNodeID = activeNodes.Next(itNodeID))
            {
                if (itShardServer->nodeID == *itNodeID)
                {
                    found = true;
                    if (quorum->hasPrimary && quorum->primaryID == *itNodeID)
                        if (CONTEXT_TRANSPORT->IsConnected(*itNodeID))
                            buffer.Appendf("     P");
                        else
                            buffer.Appendf("     !");
                    else
                    {
                        if (CONTEXT_TRANSPORT->IsConnected(*itNodeID))
                            buffer.Appendf("     +");
                        else
                            buffer.Appendf("     -");
                    }
                    break;
                }
            }
            if (!found)
                buffer.Appendf("     .");

        }
        session.Print(buffer);
    }
}

void ConfigHTTPClientSession::PrintConfigState()
{
    JSONConfigState jsonConfigState(*configServer->GetDatabaseManager()->GetConfigState(), session.json);
    jsonConfigState.Write();
    session.Flush();
}

bool ConfigHTTPClientSession::ProcessCommand(ReadBuffer& cmd)
{
    ClientRequest*  request;
    
    if (HTTP_MATCH_COMMAND(cmd, ""))
    {
        PrintStatus();
        return true;
    }
    if (HTTP_MATCH_COMMAND(cmd, "getconfigstate"))
    {
        PrintConfigState();
        return true;
    }

    request = ProcessConfigCommand(cmd);
    if (!request)
        return false;

    request->session = this;
    configServer->OnClientRequest(request);
    
    return true;
}

ClientRequest* ConfigHTTPClientSession::ProcessConfigCommand(ReadBuffer& cmd)
{
    if (HTTP_MATCH_COMMAND(cmd, "getmaster"))
        return ProcessGetMaster();
    if (HTTP_MATCH_COMMAND(cmd, "getstate"))
        return ProcessGetState();
    if (HTTP_MATCH_COMMAND(cmd, "createquorum"))
        return ProcessCreateQuorum();
//  if (HTTP_MATCH_COMMAND(cmd, "increasequorum"))
//      return ProcessIncreaseQuorum();
//  if (HTTP_MATCH_COMMAND(cmd, "decreasequorum"))
//      return ProcessDecreaseQuorum();
    if (HTTP_MATCH_COMMAND(cmd, "createdatabase"))
        return ProcessCreateDatabase();
    if (HTTP_MATCH_COMMAND(cmd, "renamedatabase"))
        return ProcessRenameDatabase();
    if (HTTP_MATCH_COMMAND(cmd, "deletedatabase"))
        return ProcessDeleteDatabase();
    if (HTTP_MATCH_COMMAND(cmd, "createtable"))
        return ProcessCreateTable();
    if (HTTP_MATCH_COMMAND(cmd, "renametable"))
        return ProcessRenameTable();
    if (HTTP_MATCH_COMMAND(cmd, "deletetable"))
        return ProcessDeleteTable();
    
    return NULL;
}

ClientRequest* ConfigHTTPClientSession::ProcessGetMaster()
{
    ClientRequest*  request;
    
    request = new ClientRequest;
    request->GetMaster(0);
    
    return request;
}

ClientRequest* ConfigHTTPClientSession::ProcessGetState()
{
    ClientRequest*  request;
    
    request = new ClientRequest;
    request->GetConfigState(0);
    
    return request;
}

ClientRequest* ConfigHTTPClientSession::ProcessCreateQuorum()
{
    typedef ClientRequest::NodeList NodeList;
    
    ClientRequest*  request;
    NodeList        nodes;
    ReadBuffer      tmp;
    char*           next;
    unsigned        nread;
    uint64_t        nodeID;
    
    // parse comma separated nodeID values
    HTTP_GET_PARAM(params, "nodes", tmp);
    while ((next = FindInBuffer(tmp.GetBuffer(), tmp.GetLength(), ',')) != NULL)
    {
        nodeID = BufferToUInt64(tmp.GetBuffer(), tmp.GetLength(), &nread);
        if (nread != (unsigned) (next - tmp.GetBuffer()))
            return NULL;
        next++;
        tmp.Advance((unsigned) (next - tmp.GetBuffer()));
        nodes.Append(nodeID);
    }
    
    nodeID = BufferToUInt64(tmp.GetBuffer(), tmp.GetLength(), &nread);
    if (nread != tmp.GetLength())
        return NULL;
    nodes.Append(nodeID);

    request = new ClientRequest;
    request->CreateQuorum(0, nodes);
    
    return request;
}

//ClientRequest* ConfigHTTPClientSession::ProcessIncreaseQuorum()
//{
//  ClientRequest*  request;
//  uint64_t        shardID;
//  uint64_t        nodeID;
//  
//  HTTP_GET_U64_PARAM(params, "shardID", shardID);
//  HTTP_GET_U64_PARAM(params, "nodeID", nodeID);
//
//  request = new ClientRequest;
//  request->IncreaseQuorum(0, shardID, nodeID);
//
//  return request;
//}
//
//ClientRequest* ConfigHTTPClientSession::ProcessDecreaseQuorum()
//{
//  ClientRequest*  request;
//  uint64_t        shardID;
//  uint64_t        nodeID;
//  
//  HTTP_GET_U64_PARAM(params, "shardID", shardID);
//  HTTP_GET_U64_PARAM(params, "nodeID", nodeID);
//
//  request = new ClientRequest;
//  request->DecreaseQuorum(0, shardID, nodeID);
//
//  return request;
//}

ClientRequest* ConfigHTTPClientSession::ProcessCreateDatabase()
{
    ClientRequest*  request;
    ReadBuffer      name;
    ReadBuffer      tmp;
    
    HTTP_GET_PARAM(params, "name", name);

    request = new ClientRequest;
    request->CreateDatabase(0, name);

    return request;
}

ClientRequest* ConfigHTTPClientSession::ProcessRenameDatabase()
{
    ClientRequest*  request;
    uint64_t        databaseID;
    ReadBuffer      name;
    
    HTTP_GET_U64_PARAM(params, "databaseID", databaseID);
    HTTP_GET_PARAM(params, "name", name);

    request = new ClientRequest;
    request->RenameDatabase(0, databaseID, name);

    return request;
}

ClientRequest* ConfigHTTPClientSession::ProcessDeleteDatabase()
{
    ClientRequest*  request;
    uint64_t        databaseID;
    
    HTTP_GET_U64_PARAM(params, "databaseID", databaseID);

    request = new ClientRequest;
    request->DeleteDatabase(0, databaseID);

    return request;
}

ClientRequest* ConfigHTTPClientSession::ProcessCreateTable()
{
    ClientRequest*  request;
    uint64_t        databaseID;
    uint64_t        quorumID;
    ReadBuffer      name;
    
    HTTP_GET_U64_PARAM(params, "databaseID", databaseID);
    HTTP_GET_U64_PARAM(params, "quorumID", quorumID);
    HTTP_GET_PARAM(params, "name", name);

    request = new ClientRequest;
    request->CreateTable(0, databaseID, quorumID, name);

    return request;
}

ClientRequest* ConfigHTTPClientSession::ProcessRenameTable()
{
    ClientRequest*  request;
    uint64_t        databaseID;
    uint64_t        tableID;
    ReadBuffer      name;
    
    HTTP_GET_U64_PARAM(params, "databaseID", databaseID);
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "name", name);

    request = new ClientRequest;
    request->RenameTable(0, databaseID, tableID, name);

    return request;
}

ClientRequest* ConfigHTTPClientSession::ProcessDeleteTable()
{
    ClientRequest*  request;
    uint64_t        databaseID;
    uint64_t        tableID;
    
    HTTP_GET_U64_PARAM(params, "databaseID", databaseID);
    HTTP_GET_U64_PARAM(params, "tableID", tableID);

    request = new ClientRequest;
    request->DeleteTable(0, databaseID, tableID);

    return request;
}

void ConfigHTTPClientSession::OnConnectionClose()
{
    configServer->OnClientClose(this);
    session.SetConnection(NULL);
    delete this;
}
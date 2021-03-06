#include "SDBPControllerConnection.h"
#include "SDBPController.h"
#include "SDBPClient.h"
#include "SDBPClientConsts.h"
#include "Application/Common/ClientRequest.h"
#include "Application/Common/ClientResponse.h"
#include "Application/SDBP/SDBPRequestMessage.h"
#include "Application/SDBP/SDBPResponseMessage.h"
#include "Framework/Replication/PaxosLease/PaxosLease.h"
#include "System/Events/EventLoop.h"
#include "System/Stopwatch.h"

#define GETCONFIGSTATE_TIMEOUT  (3*1000)
#define RECONNECT_TIMEOUT       2000

using namespace SDBPClient;

// =============================================================================================
//
// Private & ControllerPool interface
//
// =============================================================================================

ControllerConnection::ControllerConnection(Controller* controller_, uint64_t nodeID_, Endpoint& endpoint_)
{
    Log_Debug("Creating ControllerConnection: %s", endpoint_.ToString());
    controller = controller_;
    nodeID = nodeID_;
    endpoint = endpoint_;
    getConfigStateTime = 0;
    getConfigStateTimeout.SetDelay(GETCONFIGSTATE_TIMEOUT);
    getConfigStateTimeout.SetCallable(MFUNC(ControllerConnection, OnGetConfigStateTimeout));
    SetPriority(true);
    Connect();
}

ControllerConnection::~ControllerConnection()
{
    EventLoop::Remove(&getConfigStateTimeout);
}

// =============================================================================================
//
// Client interface
//    
// =============================================================================================

void ControllerConnection::ClearRequests(Client* client)
{
    Request*    request;
    Request*    next;

    // clear requests belonging to the given client
    for (request = requests.First(); request; request = next)
    {
        if (request->client == client)
            next = requests.Remove(request);
        else
            next = requests.Next(request);
    }
}

void ControllerConnection::SendRequest(Request* request)
{
    Log_Trace("type = %c, nodeID = %u", request->type, (unsigned) nodeID);

    SDBPRequestMessage  msg;

    msg.request = request;
    Write(msg);

    requests.Append(request);
}

uint64_t ControllerConnection::GetNodeID() const
{
    return nodeID;
}

// =============================================================================================
//
// Callback interface
//    
// =============================================================================================

void ControllerConnection::OnGetConfigStateTimeout()
{
    Log_Trace();
        
    if (EventLoop::Now() - getConfigStateTime > PAXOSLEASE_MAX_LEASE_TIME * 3)
    {
        if (!controller->HasMaster())
        {
            Log_Debug("ConfigStateTimeout");
            OnClose();
            // We need to remove connectTimeout, because Connect() will add it back later
            EventLoop::Remove(&connectTimeout);
            Connect();
        }
        return;
    }
    
    SendGetConfigState();
}

bool ControllerConnection::OnMessage(ReadBuffer& rbuf)
{
    SDBPResponseMessage msg;
    ClientResponse*     resp;
    Stopwatch           sw;

    Log_Trace();
    
    resp = new ClientResponse;
    msg.response = resp;
    
    // parse response
    sw.Start();
    if (!msg.Read(rbuf))
    {
        delete resp;
        return false;
    }
    sw.Stop();
    if (sw.Elapsed() > 500)
        Log_Debug("ControllerConnection::OnMessage took %U msecs", sw.Elapsed());
        
    // we don't care about Hello messages
    if (resp->type == CLIENTRESPONSE_HELLO)
    {
        delete resp;
        return false;
    }
    
    // pair the request and response if found, delete the response otherwise
    if (!ProcessResponse(resp))
    {
        delete resp;
        return false;
    }

    return false;
}

void ControllerConnection::OnWrite()
{
    Log_Trace();

    MessageConnection::OnWrite();
}

void ControllerConnection::OnConnect()
{
    MessageConnection::OnConnect();
    SendGetConfigState();

    controller->OnConnected(this);
}

void ControllerConnection::OnClose()
{
    Log_Debug("ControllerConnection::OnClose");
    
    // TODO: resend requests without response
    if (state == CONNECTED)
    {
        requests.Clear();
    }
        
    // close socket
    MessageConnection::OnClose();
    
    // clear timers
    EventLoop::Remove(&getConfigStateTimeout);
    
    // update the controller connectivity status
    controller->OnDisconnected(this);
    
    if (EventLoop::Now() - connectTime > connectTimeout.GetDelay())
    {
        // lot of time has elapsed since last connect, reconnect immediately
        Connect();
    }
    else
    {
        // wait for timeout
        EventLoop::Reset(&connectTimeout);
    }
}

// =============================================================================================
//
// Private implementation
//    
// =============================================================================================

bool ControllerConnection::ProcessResponse(ClientResponse* resp)
{
    if (resp->type == CLIENTRESPONSE_CONFIG_STATE)
        return ProcessGetConfigState(resp);

    return ProcessCommandResponse(resp);
}

bool ControllerConnection::ProcessGetConfigState(ClientResponse* resp)
{
    ASSERT(resp->configState.Get()->masterID == nodeID);

    EventLoop::Remove(&getConfigStateTimeout);
    
    // copy the config state created on stack in OnMessage
    controller->SetConfigState(this, resp->configState.Get());

    return false;
}

bool ControllerConnection::ProcessCommandResponse(ClientResponse* resp)
{
    Log_Trace();

    Request*    req;

    req = RemoveRequest(resp->commandID);
    
    // this happens when the controller loses the mastership
    if (resp->type == CLIENTRESPONSE_NOSERVICE)
    {
        controller->OnNoService(this);
        return false;
    }
    
    // pair commands to results
    if (req && req->client)
        controller->OnRequestResponse(req, resp);
    
    return false;
}

Request* ControllerConnection::RemoveRequest(uint64_t commandID)
{
    Request*    it;

    // find the request by commandID
    FOREACH (it, requests)
    {
        if (it->commandID == commandID)
        {
            requests.Remove(it);
            return it;
        }
    }

    return NULL;
}

void ControllerConnection::Connect()
{
    MessageConnection::Connect(endpoint);
}

void ControllerConnection::SendGetConfigState()
{
    Request*            request;
    SDBPRequestMessage  msg;

    ASSERT(state == CONNECTED);

    request = new Request;
    request->GetConfigState(controller->NextCommandID());
        
    // send request but don't append to the request queue
    msg.request = request;        
    Write(msg);

    delete request;

    EventLoop::Reset(&getConfigStateTimeout);
}


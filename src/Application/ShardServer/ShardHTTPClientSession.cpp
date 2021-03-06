#include "ShardHTTPClientSession.h"
#include "ShardServer.h"
#include "System/Registry.h"
#include "System/Config.h"
#include "System/Common.h"
#include "System/FileSystem.h"
#include "System/CrashReporter.h"
#include "System/IO/IOProcessor.h"
#include "Application/HTTP/HTTPConnection.h"
#include "Application/Common/ClientRequestCache.h"
#include "Application/ShardServer/ShardServerApp.h"
#include "Framework/Replication/ReplicationConfig.h"
#include "Framework/Storage/StoragePageCache.h"
#include "Framework/Storage/StorageListPageCache.h"
#include "Framework/Storage/StorageFileDeleter.h"
#include "Version.h"

#define PARAM_BOOL_VALUE(param)                         \
    ((ReadBuffer::Cmp((param), "yes") == 0 ||           \
    ReadBuffer::Cmp((param), "true") == 0 ||            \
    ReadBuffer::Cmp((param), "on") == 0 ||            \
    ReadBuffer::Cmp((param), "1") == 0) ? true : false)

#define SHARD_MIGRATION_WRITER  (shardServer->GetShardMigrationWriter())
#define LOCK_MANAGER            (shardServer->GetTransactionManager()->GetLockManager())
#define WAITQUEUE_MANAGER       (shardServer->GetTransactionManager()->GetWaitQueueManager())
#define PRINT_BOOL(str, b) { if ((b)) buffer.Appendf("%s: yes\n", str); else buffer.Appendf("%s: no\n", str); }

/*
===============================================================================================

 Debug code

===============================================================================================
*/

static uint64_t     infiniteLoopIntervalEnd;
static YieldTimer   infiniteLoopYieldTimer;

static void TimedInfiniteLoop()
{
    uint64_t    now;

    Log_Message("Async infinite loop started, expire in %U", infiniteLoopIntervalEnd);

    while (true)
    {
        now = ::Now();
        if (now > infiniteLoopIntervalEnd)
            break;
    }

    Log_Message("Async infinite loop finished");
}

static void YieldTimerInfiniteLoop()
{
    uint64_t    now;
    uint64_t    start;

    start = ::Now();
    while (true)
    {
        now = ::Now();
        if (now > infiniteLoopIntervalEnd)
            break;

        TRY_YIELD_RETURN(infiniteLoopYieldTimer, start);
    }

    Log_Message("Yield infinite loop finished");
}


/*
===============================================================================================

 ShardHTTPClientSession

===============================================================================================
*/

void ShardHTTPClientSession::SetShardServer(ShardServer* shardServer_)
{
    shardServer = shardServer_;
}

void ShardHTTPClientSession::SetConnection(HTTPConnection* conn_)
{
    session.SetConnection(conn_);
    conn_->SetOnClose(MFUNC(ShardHTTPClientSession, OnConnectionClose));
}

bool ShardHTTPClientSession::HandleRequest(HTTPRequest& request)
{
    ReadBuffer  cmd;
    ReadBuffer  param;
    
    session.ParseRequest(request, cmd, params);

    // assume printable output by default
    binaryData = false;
    if (HTTP_GET_OPT_PARAM(params, "binary", param))
    {
        binaryData = PARAM_BOOL_VALUE(param);
    }

    return ProcessCommand(cmd);
}

void ShardHTTPClientSession::OnComplete(ClientRequest* request, bool last)
{
    Buffer          tmp;
    ReadBuffer      rb;
    Buffer          key, value;
    ClientResponse* response;
    Buffer          location;
    ReadBuffer      param;

    response = &request->response;
    switch (response->type)
    {
    case CLIENTRESPONSE_OK:
        if (request->type == CLIENTREQUEST_COUNT)
        {
            tmp.Writef("%u", response->numKeys);
            rb.Wrap(tmp);
            session.Print(rb);
        }
        else
        {
            if (HTTP_GET_OPT_PARAM(params, "timing", param) && PARAM_BOOL_VALUE(param) == true)
            {
                // this is not the default case because of compatibility reasons
                // some test programs assume that the result of list commands returns with "OK"
                uint64_t elapsed = EventLoop::Now() - request->lastChangeTime;
                if (elapsed < 1000)
                    tmp.Writef("OK, %U msec", elapsed);
                else if (elapsed < 10*1000)
                    tmp.Writef("OK, %U.%U sec", elapsed / 1000, (elapsed % 1000) / 100);
                else
                    tmp.Writef("OK, %U sec", elapsed / 1000);
            }
            else
                tmp.Write("OK");

            session.Print(tmp);
        }
        break;
    case CLIENTRESPONSE_NUMBER:
        if (request->type == CLIENTREQUEST_COUNT)
        {
            response->numKeys += response->number;
        }
        else
        {
            tmp.Writef("%U", response->number);
            rb.Wrap(tmp);
            session.Print(rb);
        }
        break;
    case CLIENTRESPONSE_SNUMBER:
        tmp.Writef("%I", response->snumber);
        rb.Wrap(tmp);
        session.Print(rb);
        break;
    case CLIENTRESPONSE_VALUE:
        session.Print(response->value);
        break;
    case CLIENTRESPONSE_LIST_KEYS:
        for (unsigned i = 0; i < response->numKeys; i++)
        {
            if (binaryData)
            {
                key.Writef("%#R", &response->keys[i]);
                session.Print(key);
            }
            else
                session.Print(response->keys[i]);
        }
        break;
    case CLIENTRESPONSE_LIST_KEYVALUES:
        for (unsigned i = 0; i < response->numKeys; i++)
        {
            if (binaryData)
            {
                key.Writef("%#R", &response->keys[i]);
                value.Writef("%#R", &response->values[i]);
                session.PrintPair(key, value);
            }
            else
                session.PrintPair(response->keys[i], response->values[i]);
        }
        break;
    case CLIENTRESPONSE_NEXT:
        tmp.Writef("NEXT \"%R\" \"%R\" %U",
         &response->value, &response->endKey, response->number);
        rb.Wrap(tmp);
        session.Print(rb);
        break;
    case CLIENTRESPONSE_NOSERVICE:
        if (GetRedirectedShardServer(request->tableID, request->key, location))
            session.Redirect(location);
        else
            session.Print("NOSERVICE");
        break;
    case CLIENTRESPONSE_BADSCHEMA:
        if (GetRedirectedShardServer(request->tableID, request->key, location))
            session.Redirect(location);
        else
            session.Print("BADSCHEMA");
        break;
    case CLIENTRESPONSE_FAILED:
        if (request->paxosID > 0 && GetRedirectedShardServer(request->tableID, request->key, location))
            session.Redirect(location);
        else
            session.Print("FAILED");
        break;
    }
    
    if (last)
    {
        session.Flush();        
        delete request;
    }
}

bool ShardHTTPClientSession::IsActive()
{
    return true;
}

void ShardHTTPClientSession::PrintStatus()
{
    Buffer                      keybuf;
    Buffer                      valbuf;
    uint64_t                    primaryID;
    uint64_t                    totalSpace, freeSpace;
    ShardQuorumProcessor*       it;
    char                        hexbuf[64 + 1];
    int                         i;
    uint64_t                    elapsed;
    char                        humanBytesSent[5];
    char                        humanBytesTotal[5];
    char                        humanThroughput[5];

    session.PrintPair("ScalienDB", "ShardServer");
    session.PrintPair("Version", VERSION_STRING);

    valbuf.Writef("%U", GetProcessID());
    valbuf.NullTerminate();
    session.PrintPair("ProcessID", valbuf.GetBuffer());

    UInt64ToBufferWithBase(hexbuf, sizeof(hexbuf), REPLICATION_CONFIG->GetClusterID(), 64);
    session.PrintPair("ClusterID", hexbuf);

    valbuf.Writef("%U", MY_NODEID);
    valbuf.NullTerminate();
    session.PrintPair("NodeID", valbuf.GetBuffer());   

    valbuf.Clear();
    for (i = 0; i < configFile.GetListNum("controllers"); i++)
    {
        if (i != 0)
            valbuf.Append(",");
        valbuf.Append(configFile.GetListValue("controllers", i, ""));
    }
    valbuf.NullTerminate();
    session.PrintPair("Controllers", valbuf.GetBuffer());

    totalSpace = FS_DiskSpace(configFile.GetValue("database.dir", "db"));
    freeSpace = FS_FreeDiskSpace(configFile.GetValue("database.dir", "db"));

    // write quorums, shards, and leases
    ShardServer::QuorumProcessorList* quorumProcessors = shardServer->GetQuorumProcessors();
    
    valbuf.Writef("%u", quorumProcessors->GetLength());
    valbuf.NullTerminate();
    session.PrintPair("Number of quorums", valbuf.GetBuffer());
    
    FOREACH (it, *quorumProcessors)
    {
        primaryID = shardServer->GetLeaseOwner(it->GetQuorumID());
        
        keybuf.Writef("Quorum %U", it->GetQuorumID());
        keybuf.NullTerminate();

        uint64_t paxosID = shardServer->GetQuorumProcessor(it->GetQuorumID())->GetPaxosID();
        uint64_t highestPaxosID = shardServer->GetQuorumProcessor(it->GetQuorumID())->GetConfigQuorum()->paxosID;
        if (paxosID > highestPaxosID)
            highestPaxosID = paxosID;
        elapsed = (EventLoop::Now() - it->GetLastLearnChosenTime()) / 1000.0;
        if (it->GetLastLearnChosenTime() > 0)
        {
            valbuf.Writef("primary: %U, paxosID: %U/%U, Seconds since last replication: %U", primaryID, paxosID, highestPaxosID, elapsed);
        }
        else
        {
            valbuf.Writef("primary: %U, paxosID: %U/%U, No replication round seen since start...", primaryID, paxosID, highestPaxosID);
        }

        if (it->NeedCatchup())
            valbuf.Appendf(" *** NEED MANUAL CATCHUP! ***");

        valbuf.NullTerminate();
        
        session.PrintPair(keybuf.GetBuffer(), valbuf.GetBuffer());

        keybuf.Writef("Quorum %U primary", it->GetQuorumID());
        // primaryID is (uint64_t)(-1) when there is no primary, so we print it as signed
        valbuf.Writef("%I", primaryID);
        session.PrintPair(keybuf, valbuf);

        keybuf.Writef("Quorum %U paxosID", it->GetQuorumID());
        valbuf.Writef("%U", paxosID);
        session.PrintPair(keybuf, valbuf);
    }    
    
    keybuf.Writef("Migrating shard (sending)");
    keybuf.NullTerminate();
    if (SHARD_MIGRATION_WRITER->IsActive())
        valbuf.Writef("yes (sent: %s/%s, aggregate throughput: %s/s)",
         HumanBytes(SHARD_MIGRATION_WRITER->GetBytesSent(), humanBytesSent),
         HumanBytes(SHARD_MIGRATION_WRITER->GetBytesTotal(), humanBytesTotal),
         HumanBytes(SHARD_MIGRATION_WRITER->GetThroughput(), humanThroughput));
    else
        valbuf.Writef("no");
    valbuf.NullTerminate();
    session.PrintPair(keybuf.GetBuffer(), valbuf.GetBuffer());
    
    valbuf.Writef("%u", shardServer->GetNumSDBPClients());
    valbuf.NullTerminate();
    session.PrintPair("Number of clients", valbuf.GetBuffer());
    
    valbuf.Writef("%u", LOCK_MANAGER->GetNumLocks());
    valbuf.NullTerminate();
    session.PrintPair("Number of locks", valbuf.GetBuffer());

    session.Flush();
}

void ShardHTTPClientSession::PrintStorage()
{
    Buffer  buffer;
    
    shardServer->GetDatabaseManager()->GetEnvironment()->PrintState(
     QUORUM_DATABASE_DATA_CONTEXT, buffer);

    session.Print(buffer);

    session.Flush();
}

void ShardHTTPClientSession::PrintStatistics()
{
    Buffer                  buffer;
    IOProcessorStat         iostat;
    FS_Stat                 fsStat;
    ShardDatabaseManager*   databaseManager;
    ShardQuorumProcessor*   quorumProcessor;
    ReadBuffer              param;
    char                    formatBuf[100];
    ByteFormatType          formatType;
    RegistryNode*           registryNode;

    formatType = BYTE_FORMAT_HUMAN;
    if (HTTP_GET_OPT_PARAM(params, "humanize", param) && PARAM_BOOL_VALUE(param) == false)
    {
        formatType = BYTE_FORMAT_RAW;
    }

    IOProcessor::GetStats(&iostat);
    
    buffer.Append("IOProcessor stats\n");
    buffer.Appendf("numPolls: %U\n", iostat.numPolls);
    buffer.Appendf("numTCPReads: %U\n", iostat.numTCPReads);
    buffer.Appendf("numTCPWrites: %U\n", iostat.numTCPWrites);
    buffer.Appendf("numTCPBytesReceived: %s\n", FormatBytes(iostat.numTCPBytesReceived, formatBuf, formatType));
    buffer.Appendf("numTCPBytesSent: %s\n", FormatBytes(iostat.numTCPBytesSent, formatBuf, formatType));
    buffer.Appendf("numCompletions: %U\n", iostat.numCompletions);
    buffer.Appendf("numLongCallbacks: %U\n", iostat.numLongCallbacks);
    buffer.Appendf("totalPollTime: %U\n", iostat.totalPollTime);
    buffer.Appendf("totalNumEvents: %U\n", iostat.totalNumEvents);
    buffer.Appendf("numDanglingIods: %d\n", iostat.numDanglingIods);

    FS_GetStats(&fsStat);
    buffer.Append("  Category: FileSystem\n");
    buffer.Appendf("numReads: %U\n", fsStat.numReads);
    buffer.Appendf("numWrites: %U\n", fsStat.numWrites);
    buffer.Appendf("numBytesRead: %s\n", FormatBytes(fsStat.numBytesRead, formatBuf, formatType));
    buffer.Appendf("numBytesWritten: %s\n", FormatBytes(fsStat.numBytesWritten, formatBuf, formatType));
    buffer.Appendf("numFileOpens: %U\n", fsStat.numFileOpens);
    buffer.Appendf("numFileCloses: %U\n", fsStat.numFileCloses);
    buffer.Appendf("numFileDeletes: %U\n", fsStat.numFileDeletes);

    databaseManager = shardServer->GetDatabaseManager();
    buffer.Append("  Category: ShardServer\n");
    buffer.Appendf("uptime: %U sec\n", (Now() - shardServer->GetStartTimestamp()) / 1000);
    buffer.Appendf("totalCpuUsage: %u%%\n", GetTotalCpuUsage());
    buffer.Appendf("diskReadsPerSec: %u\n", GetDiskReadsPerSec());
    buffer.Appendf("diskWritesPerSec: %u\n", GetDiskWritesPerSec());
    buffer.Appendf("numRequests: %U\n", shardServer->GetNumRequests());
    buffer.Appendf("nextGetRequestID: %U\n", databaseManager->GetNextGetRequestID());
    buffer.Appendf("pendingReadRequests: %u\n", databaseManager->GetNumReadRequests());
    buffer.Appendf("pendingBlockingReadRequests: %u\n", databaseManager->GetNumBlockingReadRequests());
    buffer.Appendf("pendingListRequests: %u\n", databaseManager->GetNumListRequests());
    buffer.Appendf("inactiveListThreads: %u\n", databaseManager->GetNumInactiveListThreads());
    buffer.Appendf("numAbortedListRequests: %U\n", databaseManager->GetNumAbortedListRequests());
    buffer.Appendf("nextListRequestID: %U\n", databaseManager->GetNextListRequestID());
    buffer.Appendf("listPageCacheSize: %U\n", StorageListPageCache::GetCacheSize());
    buffer.Appendf("maxListPageCacheSize: %U\n", StorageListPageCache::GetMaxCacheSize());
    buffer.Appendf("maxUsedListPageCacheSize: %U\n", StorageListPageCache::GetMaxUsedSize());
    buffer.Appendf("maxCachedPageSize: %U\n", StorageListPageCache::GetMaxCachedPageSize());
    buffer.Appendf("averageCachedPageSize: %U\n", StorageListPageCache::GetListLength() == 0 ? 0 : (uint64_t)((float)StorageListPageCache::GetCacheSize() / StorageListPageCache::GetListLength()));
    buffer.Appendf("listPageCacheLength: %u\n", StorageListPageCache::GetListLength());
    PRINT_BOOL("isMergeEnabled", databaseManager->GetEnvironment()->IsMergeEnabled());
    buffer.Appendf("mergeCpuThreshold: %u\n", databaseManager->GetEnvironment()->GetMergeCpuThreshold());
    buffer.Appendf("mergeYieldFactor: %u\n", databaseManager->GetEnvironment()->GetConfig().GetMergeYieldFactor());
    PRINT_BOOL("isMergeRunning", databaseManager->GetEnvironment()->IsMergeRunning());
    buffer.Appendf("numFinishedMergeJobs: %u\n", databaseManager->GetEnvironment()->GetNumFinishedMergeJobs());
    buffer.Appendf("chunkFileDiskUsage: %s\n", FormatBytes(databaseManager->GetEnvironment()->GetChunkFileDiskUsage(), formatBuf, formatType));
    buffer.Appendf("logFileDiskUsage: %s\n", FormatBytes(databaseManager->GetEnvironment()->GetLogSegmentDiskUsage(), formatBuf, formatType));
    buffer.Appendf("numShards: %u\n", databaseManager->GetEnvironment()->GetNumShards());
    buffer.Appendf("numFileChunks: %u\n", databaseManager->GetEnvironment()->GetNumFileChunks());

    for (registryNode = Registry::First(); registryNode != NULL; registryNode = Registry::Next(registryNode))
    {
        registryNode->AppendKey(buffer);
        buffer.Append(": ");
        registryNode->AppendValue(buffer);
        buffer.Appendf("\n");
    }

    buffer.Append("  Category: Mutexes\n");
    buffer.Appendf("StorageFileDeleter mutexLockCounter: %U\n", StorageFileDeleter::GetMutex().lockCounter);
    buffer.Appendf("StorageFileDeleter mutexLastLockDate: %U\n", StorageFileDeleter::GetMutex().lastLockTime);
    buffer.Appendf("Endpoint mutexLockCounter: %U\n", Endpoint::GetMutex().lockCounter);
    buffer.Appendf("Endpoint mutexLastLockDate: %U\n", Endpoint::GetMutex().lastLockTime);
    buffer.Appendf("Log mutexLockCounter: %U\n", Log_GetMutex().lockCounter);
    buffer.Appendf("Log mutexLastLockDate: %U\n", Log_GetMutex().lastLockTime);

    buffer.Append("  Category: Locks\n");
    buffer.Appendf("numLocks: %u\n", LOCK_MANAGER->GetNumLocks());
    // user settings
    buffer.Appendf("lockExpireTime: %u\n", LOCK_MANAGER->GetLockExpireTime());
    buffer.Appendf("lockMaxCacheTime: %u\n", LOCK_MANAGER->GetMaxCacheTime());
    buffer.Appendf("lockMaxCacheCount: %u\n", LOCK_MANAGER->GetMaxCacheCount());
    buffer.Appendf("lockMaxPoolCount: %u\n", LOCK_MANAGER->GetMaxPoolCount());
    // internal structures
    buffer.Appendf("lockTreeCount: %u\n", LOCK_MANAGER->GetTreeCount());
    buffer.Appendf("lockCacheListLength: %u\n", LOCK_MANAGER->GetCacheListLength());
    buffer.Appendf("lockPoolListLength: %u\n", LOCK_MANAGER->GetPoolListLength());
    buffer.Appendf("lockExpiryListLength: %u\n", LOCK_MANAGER->GetExpiryListLength());

    buffer.Append("  Category: Lock wait queues\n");
    buffer.Appendf("numWaitQueues: %u\n", WAITQUEUE_MANAGER->GetNumWaitQueues());
    // user settings
    buffer.Appendf("waitQueueExpireTime: %u\n", WAITQUEUE_MANAGER->GetWaitExpireTime());
    buffer.Appendf("waitQueueMaxCacheTime: %u\n", WAITQUEUE_MANAGER->GetMaxCacheTime());
    buffer.Appendf("waitQueueMaxCacheCount: %u\n", WAITQUEUE_MANAGER->GetMaxCacheCount());
    buffer.Appendf("waitQueueMaxPoolCount: %u\n", WAITQUEUE_MANAGER->GetMaxPoolCount());
    // internal structures
    buffer.Appendf("waitQueueCacheListLength: %u\n", WAITQUEUE_MANAGER->GetQueueCacheListLength());
    buffer.Appendf("waitQueuePoolListLength: %u\n", WAITQUEUE_MANAGER->GetQueuePoolListLength());
    buffer.Appendf("waitQueueNodeExpiryListLength: %u\n", WAITQUEUE_MANAGER->GetNodeExpiryListLength());
    buffer.Appendf("waitQueueNodePoolListLength: %u\n", WAITQUEUE_MANAGER->GetNodePoolListLength());

    buffer.Append("  Category: Replication\n");
    FOREACH (quorumProcessor, *shardServer->GetQuorumProcessors())
    {
        buffer.Appendf("quorum[%U].messageListLength: %u\n", quorumProcessor->GetQuorumID(), 
         quorumProcessor->GetMessageListLength());
        buffer.Appendf("quorum[%U].replicationThroughput: %s\n", quorumProcessor->GetQuorumID(), 
            FormatBytes(quorumProcessor->GetReplicationThroughput(), formatBuf, formatType));

    }

    session.Print(buffer);
    session.Flush();
}

void ShardHTTPClientSession::PrintMemoryState()
{
    Buffer                  buffer;
    ByteFormatType          formatType;
    ReadBuffer              param;

    formatType = BYTE_FORMAT_HUMAN;
    if (HTTP_GET_OPT_PARAM(params, "humanize", param) && PARAM_BOOL_VALUE(param) == false)
    {
        formatType = BYTE_FORMAT_RAW;
    }

    shardServer->GetMemoryUsageBuffer(buffer, formatType);

    session.Print(buffer);
    session.Flush();
}

void ShardHTTPClientSession::PrintConfigFile()
{
    ConfigVar*  configVar;
    Buffer      value;

    FOREACH (configVar, configFile)
    {
        // fix terminating zeroes
        value.Write(configVar->value);
        value.Shorten(1);
        // replace zeroes back to commas
        for (unsigned i = 0; i < value.GetLength(); i++)
        {
            if (value.GetCharAt(i) == '\0')
                value.SetCharAt(i, ',');
        }
        session.PrintPair(configVar->name, value);
    }

    session.Flush();
}

bool ShardHTTPClientSession::ProcessCommand(ReadBuffer& cmd)
{
    ClientRequest*  request;
    
    if (HTTP_MATCH_COMMAND(cmd, ""))
    {
        PrintStatus();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "storage"))
    {
        PrintStorage();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "settings"))
    {
        ProcessSettings();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "clearcache"))
    {
        StoragePageCache::Clear();
        session.Print("Cache cleared");
        session.Flush();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "stats"))
    {
        PrintStatistics();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "memory"))
    {
        PrintMemoryState();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "config"))
    {
        PrintConfigFile();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "debug"))
    {
        ProcessDebugCommand();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "startbackup"))
    {
        ProcessStartBackup();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "endbackup"))
    {
        ProcessEndBackup();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "dumpmemochunks"))
    {
        ProcessDumpMemoChunks();
        return true;
    }
    else if (HTTP_MATCH_COMMAND(cmd, "rotatelog"))
    {
        Log_Rotate();
        session.Print("Log rotated");
        session.Flush();
        return true;
    }

    request = ProcessShardServerCommand(cmd);
    if (!request)
        return false;

    request->session = this;
    request->lastChangeTime = EventLoop::Now();
    shardServer->OnClientRequest(request);
    
    return true;
}

void ShardHTTPClientSession::ProcessDebugCommand()
{
    ReadBuffer      param;
    char            buf[100];
    const char*     debugKey;

    debugKey = configFile.GetValue("debug.key", NULL);
    if (debugKey == NULL)
    {
        session.Flush();
        return;
    }

    if (!HTTP_GET_OPT_PARAM(params, "key", param))
    {
        session.Flush();
        return;
    }

    if (!param.Equals(debugKey))
    {
        session.Flush();
        return;
    }

    if (HTTP_GET_OPT_PARAM(params, "crash", param))
    {
        Log_Message("Crashing due to request from HTTP interface");
        Log_Shutdown();
    
        // Access violation
        *((char*) 0) = 0;        
    }

    if (HTTP_GET_OPT_PARAM(params, "timedcrash", param))
    {
        uint64_t crashInterval = 0;
        HTTP_GET_OPT_U64_PARAM(params, "interval", crashInterval);
        CrashReporter::TimedCrash((unsigned int) crashInterval);
        snprintf(buf, sizeof(buf), "Crash scheduled in %u msec", (unsigned int) crashInterval);
        session.Print(buf);
        Log_Message("%s", buf);
    }

    if (HTTP_GET_OPT_PARAM(params, "randomcrash", param))
    {
        uint64_t crashInterval = 0;
        HTTP_GET_OPT_U64_PARAM(params, "interval", crashInterval);
        CrashReporter::RandomCrash((unsigned int) crashInterval);
        snprintf(buf, sizeof(buf), "Crash in %u msec", (unsigned int) crashInterval);
        session.Print(buf);
        Log_Message("%s", buf);
    }

    if (HTTP_GET_OPT_PARAM(params, "sleep", param))
    {
        unsigned sleep = 0;
        param.Readf("%u", &sleep);
        Log_Debug("Sleeping for %u seconds", sleep);
        session.Print("Start sleeping");
        MSleep(sleep * 1000);
        session.Print("Sleep finished");
    }

    if (HTTP_GET_OPT_PARAM(params, "asyncInfiniteLoop", param))
    {
        static ThreadPool* asyncInfiniteLoopThread;
        uint64_t infiniteLoopIntervalSec = 60;
        HTTP_GET_OPT_U64_PARAM(params, "interval", infiniteLoopIntervalSec);
        snprintf(buf, sizeof(buf), "Looping for %u sec", (unsigned int) infiniteLoopIntervalSec);
        session.Print(buf);
        Log_Message("%s", buf);
        if (asyncInfiniteLoopThread == NULL)
        {
            asyncInfiniteLoopThread = ThreadPool::Create(1);
            asyncInfiniteLoopThread->Start();
        }
        infiniteLoopIntervalEnd = NowClock() + infiniteLoopIntervalSec * 1000;
        asyncInfiniteLoopThread->Execute(CFunc(TimedInfiniteLoop));
    }

    if (HTTP_GET_OPT_PARAM(params, "yieldInfiniteLoop", param))
    {
        uint64_t infiniteLoopIntervalSec = 60;
        HTTP_GET_OPT_U64_PARAM(params, "interval", infiniteLoopIntervalSec);
        snprintf(buf, sizeof(buf), "Looping for %u sec", (unsigned int) infiniteLoopIntervalSec);
        session.Print(buf);
        Log_Message("%s", buf);
        if (!infiniteLoopYieldTimer.IsActive())
        {
            infiniteLoopIntervalEnd = NowClock() + infiniteLoopIntervalSec * 1000;
            infiniteLoopYieldTimer.SetCallable(CFunc(YieldTimerInfiniteLoop));
            EventLoop::Add(&infiniteLoopYieldTimer);
        }
    }

    if (HTTP_GET_OPT_PARAM(params, "stop", param))
    {
        STOP("Stopping due to request from HTTP interface");
        // program terminates here
    }

    if (HTTP_GET_OPT_PARAM(params, "fail", param))
    {
        STOP_FAIL(1, "Failing due to request from HTTP interface");
        // program terminates here
    }

    if (HTTP_GET_OPT_PARAM(params, "assert", param))
    {
        ASSERT_FAIL();
        // program terminates here
    }

    session.Flush();
}

void ShardHTTPClientSession::ProcessStartBackup()
{
    uint64_t                tocID;
    Buffer                  output;
    Buffer					configStateBuffer;
    StorageEnvironment*     env;
    ConfigState*			configState;

    env = shardServer->GetDatabaseManager()->GetEnvironment();

    configState = shardServer->GetDatabaseManager()->GetConfigState();
    if (configState)
        configState->Write(configStateBuffer, false);

    // turn off file deletion and write a snapshot of the TOC
    env->SetDeleteEnabled(false);
    tocID = env->WriteSnapshotTOC(configStateBuffer);
    
    output.Writef("%U", tocID);
    output.NullTerminate();
    
    session.Print(output.GetBuffer());
    session.Flush();
}

void ShardHTTPClientSession::ProcessEndBackup()
{
    uint64_t                tocID;
    Buffer                  output;
    StorageEnvironment*     env;

    tocID = 0;
    HTTP_GET_OPT_U64_PARAM(params, "tocID", tocID);
    
    env = shardServer->GetDatabaseManager()->GetEnvironment();
    env->DeleteSnapshotTOC(tocID);
    env->SetDeleteEnabled(true);
    
    session.Print("Done.");
    session.Flush();
}

void ShardHTTPClientSession::ProcessDumpMemoChunks()
{
    shardServer->GetDatabaseManager()->GetEnvironment()->DumpMemoChunks();

    session.Print("Dumping memo chunks.");
    session.Flush();
}

bool ShardHTTPClientSession::ProcessSettings()
{
    ReadBuffer              param;
    bool                    boolValue;
    uint64_t                u64;
    uint64_t                mergeCpuThreshold;
    uint64_t                mergeBufferSize;
    uint64_t                mergeYieldFactor;
    uint64_t                traceBufferSize;
    uint64_t                logFlushInterval;
    uint64_t                logTraceInterval;
    uint64_t                replicationLimit;
    uint64_t				abortWaitingListsNum;
    uint64_t                listDataPageCacheSize;
    ShardQuorumProcessor*   quorumProcessor;
    char                    buf[100];

#define CHECK_AND_SET_REGISTRY_UINT64(pstr)                     \
    if (HTTP_GET_OPT_PARAM(params, pstr, param))                \
    {                                                           \
        u64 = 0;                                                \
        HTTP_GET_OPT_U64_PARAM(params, pstr, u64);              \
        *(Registry::GetUintPtr(pstr)) = u64;                    \
        snprintf(buf, sizeof(buf), "%u", (unsigned) u64);       \
        session.PrintPair(pstr, buf);                           \
    }

#define CHECK_AND_SET_UINT64(pstr, func)                        \
    if (HTTP_GET_OPT_PARAM(params, pstr, param))                \
    {                                                           \
        u64 = 0;                                                \
        HTTP_GET_OPT_U64_PARAM(params, pstr, u64);              \
        func(u64);                                              \
        snprintf(buf, sizeof(buf), "%u", (unsigned) u64);       \
        session.PrintPair(pstr, buf);                           \
    }

#define CHECK_AND_SET_POSITIVE_UINT64(pstr, func)               \
    if (HTTP_GET_OPT_PARAM(params, pstr, param))                \
    {                                                           \
        u64 = 0;                                                \
        HTTP_GET_OPT_U64_PARAM(params, pstr, u64);              \
        if (u64 > 0)                                            \
        {                                                       \
            func(u64);                                          \
            snprintf(buf, sizeof(buf), "%u", (unsigned) u64);   \
            session.PrintPair(pstr, buf);                       \
        }                                                       \
    }       

    if (HTTP_GET_OPT_PARAM(params, "trace", param))
    {
        boolValue = PARAM_BOOL_VALUE(param);
        Log_SetTrace(boolValue);
        Log_Flush();
        session.PrintPair("Trace", boolValue ? "on" : "off");
        // Optional log trace interval in seconds
        logTraceInterval = 0;
        HTTP_GET_OPT_U64_PARAM(params, "interval", logTraceInterval);
        if (logTraceInterval > 0 && !onTraceOffTimeout.IsActive())
        {
            onTraceOffTimeout.SetCallable(MFUNC(ShardHTTPClientSession, OnTraceOffTimeout));
            onTraceOffTimeout.SetDelay(logTraceInterval * 1000);
            EventLoop::Add(&onTraceOffTimeout);
            session.Flush(false);
            return true;
        }
    }

    if (HTTP_GET_OPT_PARAM(params, "traceBufferSize", param))
    {
        // initialize variable, because conversion may fail
        traceBufferSize = 0;
        HTTP_GET_OPT_U64_PARAM(params, "traceBufferSize", traceBufferSize);
        // we expect traceBufferSize is in bytes
        Log_SetTraceBufferSize((unsigned) traceBufferSize);
        snprintf(buf, sizeof(buf), "%u", (unsigned) traceBufferSize);
        session.PrintPair("TraceBufferSize", buf);
    }

    if (HTTP_GET_OPT_PARAM(params, "debug", param))
    {
        boolValue = PARAM_BOOL_VALUE(param);
        Log_SetDebug(boolValue);
        Log_Flush();
        session.PrintPair("Debug", boolValue ? "on" : "off");
    }

    if (HTTP_GET_OPT_PARAM(params, "merge", param))
    {
        boolValue = PARAM_BOOL_VALUE(param);
        if (boolValue ^ shardServer->GetDatabaseManager()->GetEnvironment()->IsMergeEnabled())
            shardServer->GetDatabaseManager()->GetEnvironment()->SetMergeEnabled(boolValue);
        boolValue = shardServer->GetDatabaseManager()->GetEnvironment()->IsMergeEnabled();
        session.PrintPair("Merge", boolValue ? "on" : "off");
    }

    if (HTTP_GET_OPT_PARAM(params, "mergeCpuThreshold", param))
    {
        // initialize variable, because conversion may fail
        mergeCpuThreshold = 100;
        HTTP_GET_OPT_U64_PARAM(params, "mergeCpuThreshold", mergeCpuThreshold);
        shardServer->GetDatabaseManager()->GetEnvironment()->SetMergeCpuThreshold(mergeCpuThreshold);
        snprintf(buf, sizeof(buf), "%u", (unsigned) mergeCpuThreshold);
        session.PrintPair("MergeCpuThreshold", buf);
    }

    if (HTTP_GET_OPT_PARAM(params, "mergeBufferSize", param))
    {
        mergeBufferSize = shardServer->GetDatabaseManager()->GetEnvironment()->GetConfig().GetMergeBufferSize();
        HTTP_GET_OPT_U64_PARAM(params, "mergeBufferSize", mergeBufferSize);
        shardServer->GetDatabaseManager()->GetEnvironment()->GetConfig().SetMergeBufferSize(mergeBufferSize);
        snprintf(buf, sizeof(buf), "%u", (unsigned) mergeBufferSize);
        session.PrintPair("MergeBufferSize", buf);
    }

    if (HTTP_GET_OPT_PARAM(params, "mergeYieldFactor", param))
    {
        mergeYieldFactor = shardServer->GetDatabaseManager()->GetEnvironment()->GetConfig().GetMergeYieldFactor();
        HTTP_GET_OPT_U64_PARAM(params, "mergeYieldFactor", mergeYieldFactor);
        shardServer->GetDatabaseManager()->GetEnvironment()->GetConfig().SetMergeYieldFactor(mergeYieldFactor);
        snprintf(buf, sizeof(buf), "%u", (unsigned) mergeYieldFactor);
        session.PrintPair("MergeYieldFactor", buf);
    }

    if (HTTP_GET_OPT_PARAM(params, "assert", param))
    {
        ASSERT(false);
    }

    if (HTTP_GET_OPT_PARAM(params, "logFlushInterval", param))
    {
        logFlushInterval = 0;
        HTTP_GET_OPT_U64_PARAM(params, "logFlushInterval", logFlushInterval);
        Log_SetFlushInterval((unsigned) logFlushInterval * 1000);
        snprintf(buf, sizeof(buf), "%u", (unsigned) logFlushInterval);
        session.PrintPair("LogFlushInterval", buf);
    }

    if (HTTP_GET_OPT_PARAM(params, "replicationLimit", param))
    {
        replicationLimit = 0;
        HTTP_GET_OPT_U64_PARAM(params, "replicationLimit", replicationLimit);
        FOREACH (quorumProcessor, *shardServer->GetQuorumProcessors())
        {
            quorumProcessor->SetReplicationLimit((unsigned) replicationLimit);
        }
        snprintf(buf, sizeof(buf), "%u", (unsigned) replicationLimit);
        session.PrintPair("ReplicationLimit", buf);
    }

    if (HTTP_GET_OPT_PARAM(params, "abortWaitingListsNum", param))
    {
        // initialize variable, because conversion may fail
        abortWaitingListsNum = 0;
        HTTP_GET_OPT_U64_PARAM(params, "abortWaitingListsNum", abortWaitingListsNum);
        shardServer->GetDatabaseManager()->GetEnvironment()->GetConfig().SetAbortWaitingListsNum(abortWaitingListsNum);
        snprintf(buf, sizeof(buf), "%u", (unsigned) abortWaitingListsNum);
        session.PrintPair("AbortWaitingListsNum", buf);
    }

    if (HTTP_GET_OPT_PARAM(params, "listDataPageCacheSize", param))
    {
        // initialize variable, because conversion may fail
        listDataPageCacheSize = 0;
        HTTP_GET_OPT_U64_PARAM(params, "listDataPageCacheSize", listDataPageCacheSize);
        if (listDataPageCacheSize > 0)
        {
            StorageListPageCache::SetMaxCacheSize(listDataPageCacheSize);
            snprintf(buf, sizeof(buf), "%u", (unsigned) listDataPageCacheSize);
            session.PrintPair("ListDataPageCacheSize", buf);
        }
    }

    CHECK_AND_SET_POSITIVE_UINT64("maxChunkPerShard", shardServer->GetDatabaseManager()->GetEnvironment()->GetConfig().SetMaxChunkPerShard);

    CHECK_AND_SET_POSITIVE_UINT64("lockExpireTime", LOCK_MANAGER->SetLockExpireTime);
    CHECK_AND_SET_UINT64("lockMaxCacheTime",        LOCK_MANAGER->SetMaxCacheTime);
    CHECK_AND_SET_UINT64("lockMaxCacheCount",       LOCK_MANAGER->SetMaxCacheCount);
    CHECK_AND_SET_UINT64("lockMaxPoolCount",        LOCK_MANAGER->SetMaxPoolCount);

    CHECK_AND_SET_POSITIVE_UINT64("waitExpireTime", WAITQUEUE_MANAGER->SetWaitExpireTime);
    CHECK_AND_SET_UINT64("waitQueueMaxCacheTime",   WAITQUEUE_MANAGER->SetMaxCacheTime);
    CHECK_AND_SET_UINT64("waitQueueMaxCacheCount",  WAITQUEUE_MANAGER->SetMaxCacheCount);
    CHECK_AND_SET_UINT64("waitQueueMaxPoolCount",   WAITQUEUE_MANAGER->SetMaxPoolCount);

    CHECK_AND_SET_POSITIVE_UINT64("system.maxFileCacheSize", SetMaxFileCacheSize);

    session.Flush();

    return true;
}

ClientRequest* ShardHTTPClientSession::ProcessShardServerCommand(ReadBuffer& cmd)
{
    if (HTTP_MATCH_COMMAND(cmd, "get"))
        return ProcessGet();
    if (HTTP_MATCH_COMMAND(cmd, "set"))
        return ProcessSet();
//    if (HTTP_MATCH_COMMAND(cmd, "setifnex"))
//        return ProcessSetIfNotExists();
//    if (HTTP_MATCH_COMMAND(cmd, "testandset"))
//        return ProcessTestAndSet();
//    if (HTTP_MATCH_COMMAND(cmd, "testanddelete"))
//        return ProcessTestAndDelete();
//    if (HTTP_MATCH_COMMAND(cmd, "getandset"))
//        return ProcessGetAndSet();
    if (HTTP_MATCH_COMMAND(cmd, "add"))
        return ProcessAdd();
    if (HTTP_MATCH_COMMAND(cmd, "delete"))
        return ProcessDelete();
//    if (HTTP_MATCH_COMMAND(cmd, "remove"))
//        return ProcessRemove();
    if (HTTP_MATCH_COMMAND(cmd, "listKeys"))
        return ProcessListKeys();
    if (HTTP_MATCH_COMMAND(cmd, "listKeyValues"))
        return ProcessListKeyValues();
    if (HTTP_MATCH_COMMAND(cmd, "count"))
        return ProcessCount();
    
    return NULL;
}

ClientRequest* ShardHTTPClientSession::ProcessGet()
{
    ClientRequest*  request;
    uint64_t        tableID;
    ReadBuffer      key;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "key", key);

    request = new ClientRequest;
    request->Get(0, 0, tableID, key);

    HTTP_GET_OPT_U64_PARAM(params, "paxosID", request->paxosID);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessSet()
{
    ClientRequest*  request;
    uint64_t        tableID;
    ReadBuffer      key;
    ReadBuffer      value;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "key", key);
    HTTP_GET_PARAM(params, "value", value);

    request = new ClientRequest;
    request->Set(0, 0, tableID, key, value);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessSetIfNotExists()
{
    ClientRequest*  request;
    uint64_t        tableID;
    ReadBuffer      key;
    ReadBuffer      value;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "key", key);
    HTTP_GET_PARAM(params, "value", value);

    request = new ClientRequest;
    request->SetIfNotExists(0, 0, tableID, key, value);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessTestAndSet()
{
    ClientRequest*  request;
    uint64_t        tableID;
    ReadBuffer      key;
    ReadBuffer      test;
    ReadBuffer      value;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "key", key);
    HTTP_GET_PARAM(params, "test", test);
    HTTP_GET_PARAM(params, "value", value);

    request = new ClientRequest;
    request->TestAndSet(0, 0, tableID, key, test, value);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessTestAndDelete()
{
    ClientRequest*  request;
    uint64_t        tableID;
    ReadBuffer      key;
    ReadBuffer      test;
    ReadBuffer      value;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "key", key);
    HTTP_GET_PARAM(params, "test", test);

    request = new ClientRequest;
    request->TestAndDelete(0, 0, tableID, key, test);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessGetAndSet()
{
    ClientRequest*  request;
    uint64_t        tableID;
    ReadBuffer      key;
    ReadBuffer      value;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "key", key);
    HTTP_GET_PARAM(params, "value", value);

    request = new ClientRequest;
    request->GetAndSet(0, 0, tableID, key, value);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessAdd()
{
    unsigned        nread;
    uint64_t        tableID;
    int64_t         number;
    ClientRequest*  request;
    ReadBuffer      key;
    ReadBuffer      numberBuffer;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "key", key);
    HTTP_GET_PARAM(params, "number", numberBuffer);

    number = BufferToInt64(numberBuffer.GetBuffer(), numberBuffer.GetLength(), &nread);
    if (nread != numberBuffer.GetLength())
        return NULL;

    request = new ClientRequest;
    request->Add(0, 0, tableID, key, number);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessDelete()
{
    ClientRequest*  request;
    uint64_t        tableID;
    ReadBuffer      key;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "key", key);

    request = new ClientRequest;
    request->Delete(0, 0, tableID, key);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessRemove()
{
    ClientRequest*  request;
    uint64_t        tableID;
    ReadBuffer      key;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_PARAM(params, "key", key);

    request = new ClientRequest;
    request->Remove(0, 0, tableID, key);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessListKeys()
{
    bool            forwardDirection;
    uint64_t        tableID;
    uint64_t        count;
    ReadBuffer      startKey;
    ReadBuffer      endKey;
    ReadBuffer      prefix;
    ReadBuffer      dirBuffer;
    ClientRequest*  request;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_OPT_PARAM(params, "startKey", startKey);
    HTTP_GET_OPT_PARAM(params, "endKey", endKey);
    HTTP_GET_OPT_PARAM(params, "prefix", prefix);
    HTTP_GET_OPT_PARAM(params, "direction", dirBuffer);
    count = 0;
    HTTP_GET_OPT_U64_PARAM(params, "count", count);

    if (ReadBuffer::Cmp(dirBuffer, "backward") == 0)
        forwardDirection = false;
    else
        forwardDirection = true;

    request = new ClientRequest;
    request->ListKeys(0, 0, tableID, startKey, endKey, prefix, count, forwardDirection);

    HTTP_GET_OPT_U64_PARAM(params, "paxosID", request->paxosID);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessListKeyValues()
{
    bool            forwardDirection;
    uint64_t        tableID;
    uint64_t        count;
    ReadBuffer      startKey;
    ReadBuffer      endKey;
    ReadBuffer      prefix;
    ReadBuffer      dirBuffer;
    ClientRequest*  request;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_OPT_PARAM(params, "startKey", startKey);
    HTTP_GET_OPT_PARAM(params, "endKey", endKey);
    HTTP_GET_OPT_PARAM(params, "prefix", prefix);
    HTTP_GET_OPT_PARAM(params, "direction", dirBuffer);
    count = 0;
    HTTP_GET_OPT_U64_PARAM(params, "count", count);

    if (ReadBuffer::Cmp(dirBuffer, "backward") == 0)
        forwardDirection = false;
    else
        forwardDirection = true;

    request = new ClientRequest;
    request->ListKeyValues(0, 0, tableID, startKey, endKey, prefix, count, forwardDirection);

    HTTP_GET_OPT_U64_PARAM(params, "paxosID", request->paxosID);

    return request;    
}

ClientRequest* ShardHTTPClientSession::ProcessCount()
{
    bool            forwardDirection;
    ClientRequest*  request;
    uint64_t        tableID;
    ReadBuffer      startKey;
    ReadBuffer      endKey;
    ReadBuffer      prefix;
    ReadBuffer      dirBuffer;
    
    HTTP_GET_U64_PARAM(params, "tableID", tableID);
    HTTP_GET_OPT_PARAM(params, "startKey", startKey);
    HTTP_GET_OPT_PARAM(params, "endKey", endKey);
    HTTP_GET_OPT_PARAM(params, "prefix", prefix);
    HTTP_GET_OPT_PARAM(params, "direction", dirBuffer);

    if (ReadBuffer::Cmp(dirBuffer, "backward") == 0)
        forwardDirection = false;
    else
        forwardDirection = true;

    request = new ClientRequest;
    request->Count(0, 0, tableID, startKey, endKey, prefix, forwardDirection);

    HTTP_GET_OPT_U64_PARAM(params, "paxosID", request->paxosID);

    return request;    
}

void ShardHTTPClientSession::OnConnectionClose()
{
    shardServer->OnClientClose(this);
    session.SetConnection(NULL);
    delete this;
}

bool ShardHTTPClientSession::GetRedirectedShardServer(uint64_t tableID, const ReadBuffer& key, 
 Buffer& location)
{
    ConfigState*        configState;
    ConfigShard*        shard;
    ConfigQuorum*       quorum;
    ConfigShardServer*  server;
    Endpoint            endpoint;
    
    configState = shardServer->GetConfigState();
    shard = configState->GetShard(tableID, key);
    if (!shard)
        return false;
    
    quorum = configState->GetQuorum(shard->quorumID);
    if (!quorum)
        return false;
    
    if (!quorum->hasPrimary)
        return false;
    
    server = configState->GetShardServer(quorum->primaryID);
    if (!server)
        return false;
    
    if (server->nodeID == MY_NODEID)
        return false;
    
    endpoint = server->endpoint;
    endpoint.SetPort(server->httpPort);
    // TODO: change http:// to symbolic const
    location.Writef("http://%s%R", endpoint.ToString(), &session.uri);
    return true;
}

void ShardHTTPClientSession::OnTraceOffTimeout()
{
    Log_SetTrace(false);
    session.PrintPair("Trace", "off");
    session.Flush();
}

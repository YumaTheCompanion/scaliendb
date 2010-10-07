#include "Test.h"
#include "Application/Client/SDBPClient.h"

using namespace SDBPClient;

#define TEST_CLIENT_FAIL() \
    { \
        PRINT_CLIENT_STATUS("Transport", client.TransportStatus()); \
        PRINT_CLIENT_STATUS("Connectivity", client.ConnectivityStatus()); \
        PRINT_CLIENT_STATUS("Timeout", client.TimeoutStatus()); \
        PRINT_CLIENT_STATUS("Command", client.CommandStatus()); \
        TEST_FAIL(); \
    }

#define PRINT_CLIENT_STATUS(which, status) \
    switch (status) \
    { \
    case SDBP_SUCCESS: TEST_LOG("%s status: SDBP_SUCCESS", which); break; \
    case SDBP_API_ERROR: TEST_LOG("%s status: SDBP_API_ERROR", which); break; \
    case SDBP_PARTIAL: TEST_LOG("%s status: SDBP_PARTIAL", which); break; \
    case SDBP_FAILURE: TEST_LOG("%s status: SDBP_FAILURE", which); break; \
    case SDBP_NOMASTER: TEST_LOG("%s status: SDBP_NOMASTER", which); break; \
    case SDBP_NOCONNECTION: TEST_LOG("%s status: SDBP_NOCONNECTION", which); break; \
    case SDBP_NOPRIMARY: TEST_LOG("%s status: SDBP_NOPRIMARY", which); break; \
    case SDBP_MASTER_TIMEOUT: TEST_LOG("%s status: SDBP_MASTER_TIMEOUT", which); break; \
    case SDBP_GLOBAL_TIMEOUT: TEST_LOG("%s status: SDBP_GLOBAL_TIMEOUT", which); break; \
    case SDBP_PRIMARY_TIMEOUT: TEST_LOG("%s status: SDBP_PRIMARY_TIMEOUT", which); break; \
    case SDBP_NOSERVICE: TEST_LOG("%s status: SDBP_NOSERVICE", which); break; \
    case SDBP_FAILED: TEST_LOG("%s status: SDBP_FAILED", which); break; \
    case SDBP_BADSCHEMA: TEST_LOG("%s status: SDBP_BADSCHEMA", which); break; \
    }
    

TEST_DEFINE(TestClientBasic)
{
    Client          client;
    const char*     nodes[] = {"localhost:7080"};
    ReadBuffer      databaseName = "mediafilter";
    ReadBuffer      tableName = "users";
    ReadBuffer      key = "hol";
    ReadBuffer      value = "value";
    uint64_t        databaseID;
    uint64_t        tableID;
    int             ret;
    
    ret = client.Init(SIZE(nodes), nodes);
    if (ret != SDBP_SUCCESS)
        TEST_CLIENT_FAIL();

    client.SetMasterTimeout(1000);
    ret = client.GetDatabaseID(databaseName, databaseID);
    if (ret != SDBP_SUCCESS)
        TEST_CLIENT_FAIL();
    
    if (databaseID != 1)
        TEST_CLIENT_FAIL();
    
    ret = client.GetTableID(tableName, databaseID, tableID);
    if (ret != SDBP_SUCCESS)
        TEST_CLIENT_FAIL();
    
    if (tableID != 1)
        TEST_CLIENT_FAIL();
    
    ret = client.Set(databaseID, tableID, key, value);
    if (ret != SDBP_SUCCESS)
        TEST_CLIENT_FAIL();
    
    return TEST_SUCCESS;
}

TEST_MAIN(TestClientBasic);
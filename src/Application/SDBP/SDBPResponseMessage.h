#ifndef SDBPRESPONSEMESSAGE_H
#define SDBPRESPONSEMESSAGE_H

#include "Framework/Messaging/Message.h"
#include "Application/Common/ClientResponse.h"

class SDBPResponseMessage : public Message
{
public:
    ClientResponse* response;
    
    bool            Read(ReadBuffer& buffer);
    bool            Write(Buffer& buffer);
    
    int             ReadOptionalParts(ReadBuffer buffer, int offset);
    void            WriteOptionalParts(Buffer& buffer);
};

#endif

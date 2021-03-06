#ifndef BUFFER_H
#define BUFFER_H

#include "System/Platform.h"
#include "ReadBuffer.h"

#define ARRAY_SIZE              16
#define ALLOC_GRANURALITY       32

/*
===============================================================================================

 Buffer

===============================================================================================
*/

class Buffer
{
public:
    Buffer();
    Buffer(const Buffer& other);
    ~Buffer();

    Buffer&             operator=(const Buffer& other);

    void                SetPreallocated(char* buffer, unsigned size);
    
    static int          Cmp(const Buffer& a, const Buffer& b);
    int                 Cmp(const char* buffer, unsigned length_);
    int                 Cmp(const char* str);

    void                Lengthen(unsigned k);
    void                Shorten(unsigned k);
    
    void                Allocate(unsigned size, bool keepold = true);

    int                 Readf(const char* format, ...) const;
    unsigned            Writef(const char* fmt, ...);
    unsigned            Appendf(const char* fmt, ...);
    
    void                Write(const char* buffer, unsigned length);
    void                Write(const char* str);
    void                Write(const Buffer& other);
    void                Write(const ReadBuffer& other);
    void                Write(char c, unsigned num);

    void                Append(char c);
    void                Append(const char* buffer, unsigned length);
    void                Append(const char* str);
    void                Append(const Buffer& other);
    void                Append(ReadBuffer other);
    void                Append(char c, unsigned num);

    void                AppendLittle16(uint16_t x);
    void                AppendLittle32(uint32_t x);
    void                AppendLittle64(uint64_t x);

    char                GetCharAt(unsigned i);
    void                SetCharAt(unsigned i, char c);

    bool                IsAsciiPrintable();
    void                ToHexadecimal();

    void                NullTerminate();
    void                Zero();
    void                ZeroRest();

    void                SetLength(unsigned length);

    void                Init();
    unsigned            GetSize() const;
    char*               GetBuffer() const;
    unsigned            GetLength() const;
    unsigned            GetRemaining() const;
    char*               GetPosition() const;
    uint32_t            GetChecksum() const;
    ReadBuffer          GetReadBuffer() const;
    
    void                Clear();
    void                Reset();

    bool                BeginsWith(const char* s);
    bool                BeginsWith(Buffer& other);

    Buffer*             next;
    Buffer*             prev;
        
protected:
    char*               buffer;
    unsigned            size;
    unsigned            length;
    bool                preallocated;
    char                array[ARRAY_SIZE];
};

#endif

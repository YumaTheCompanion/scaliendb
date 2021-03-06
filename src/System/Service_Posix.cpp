#ifndef PLATFORM_WINDOWS
#include "Service.h"
#include "Macros.h"

bool Service::Main(int argc, char **argv, void (*serviceFunc)(), ServiceIdentity& ident)
{
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(ident);

    serviceFunc();
    return true;
}

void Service::SetStatus(unsigned status)
{
    UNUSED(status);
}

#endif

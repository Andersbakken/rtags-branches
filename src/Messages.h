#ifndef MESSAGES_H
#define MESSAGES_H

#include "QueryMessage.h"
#include "ProjectMessage.h"
#include "ResponseMessage.h"
#include "CreateOutputMessage.h"

class Messages
{
public:
    static void init();
    static void cleanup();
    static Message *create(const char *data, int size);
};

#endif // MESSAGES_H

#ifndef FindSymbolsJob_h
#define FindSymbolsJob_h

#include "ByteArray.h"
#include "List.h"
#include "QueryMessage.h"
#include "Job.h"

class FindSymbolsJob : public Job
{
public:
    FindSymbolsJob(Connection *connection, const QueryMessage &query, const shared_ptr<Project> &project);
    virtual void execute();
private:
    const ByteArray string;
};

#endif

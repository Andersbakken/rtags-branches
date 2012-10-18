#ifndef StatusJob_h
#define StatusJob_h

#include "ByteArray.h"
#include "List.h"
#include "Job.h"

class QueryMessage;
class StatusJob : public Job
{
public:
    StatusJob(const QueryMessage &query, const SharedPtr<Project> &project);
    static const char *delimiter;
protected:
    virtual void execute();
private:
    const ByteArray query;
};

#endif

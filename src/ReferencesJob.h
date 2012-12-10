#ifndef ReferencesJob_h
#define ReferencesJob_h

#include "ByteArray.h"
#include "Job.h"
#include "List.h"
#include "Location.h"
#include "RTags.h"

class CursorInfo;
class ReferencesJob : public Job
{
public:
    ReferencesJob(Connection *connection, const Location &location, const QueryMessage &query, const shared_ptr<Project> &project);
    ReferencesJob(Connection *connection, const ByteArray &symbolName, const QueryMessage &query, const shared_ptr<Project> &project);
    virtual void execute();
private:
    Set<Location> locations;
    const ByteArray symbolName;
};

#endif

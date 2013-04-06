#ifndef FollowLocationJob_h
#define FollowLocationJob_h

#include <rct/String.h>
#include <rct/List.h>
#include "RTags.h"
#include "Job.h"
#include "Location.h"

class FollowLocationJob : public Job
{
public:
    FollowLocationJob(const Location &loc, const QueryMessage &query, const shared_ptr<Project> &project);
protected:
    virtual void execute();
    bool process(const SymbolMap &map);
private:
    const Location location;
};

#endif

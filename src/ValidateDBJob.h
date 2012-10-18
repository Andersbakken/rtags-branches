#ifndef ValidateDBJob_h
#define ValidateDBJob_h

#include "Job.h"
#include "Set.h"
#include "Location.h"
#include "SignalSlot.h"

class ValidateDBJob : public Job
{
public:
    ValidateDBJob(const SharedPtr<Project> &proj, const Set<Location> &prev);
    signalslot::Signal1<const Set<Location> &> &errors() { return mErrors; }
protected:
    virtual void execute();
private:
    const Set<Location> mPrevious;
    signalslot::Signal1<const Set<Location> &> mErrors;

};

#endif

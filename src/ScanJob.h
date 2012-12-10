#ifndef ScanJob_h
#define ScanJob_h

#include "ThreadPool.h"
#include "AbortInterface.h"
#include "Path.h"
#include "SignalSlot.h"
#include "Project.h"

class ScanJob : public ThreadPoolJob, public AbortInterface
{
public:
    enum Mode {
        Sources,
        All
    };
    ScanJob(Mode mode, const Path &path, const shared_ptr<Project> &project);
    virtual void run();
    signalslot::Signal1<const Set<Path> &>&finished() { return mFinished; }

    enum FilterResult {
        Filtered,
        File,
        Source,
        Directory
    };

    static FilterResult filter(const Path &path, const List<ByteArray> &filters);
private:
    const Mode mMode;
    static Path::VisitResult visit(const Path &path, void *userData);
    Path mPath;
    const List<ByteArray> &mFilters;
    Set<Path> mPaths;
    signalslot::Signal1<const Set<Path> &> mFinished; // value => true means it's a source file

    weak_ptr<Project> mProject;
};

#endif

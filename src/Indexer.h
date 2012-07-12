#ifndef INDEXER_H
#define INDEXER_H

#include "CursorInfo.h"
#include "FileSystemWatcher.h"
#include "MutexLocker.h"
#include "RTags.h"
#include "ReadWriteLock.h"
#include "ThreadPool.h"
#include "Timer.h"
#include <clang-c/Index.h>
#include <list>

class IndexerJob;
class DirtyThread;
class Indexer
{
public:
    Indexer();
    ~Indexer();

    void init(const Path &srcRoot, const Path &projectRoot, bool validate);

    int index(const Path &input, const List<ByteArray> &arguments, unsigned indexerJobFlags);

    void setPchDependencies(const Path &pchHeader, const Set<uint32_t> &deps);
    void addDependencies(const DependencyMap &hash);
    Set<uint32_t> dependencies(uint32_t fileId) const;
    Set<uint32_t> pchDependencies(const Path &pchHeader) const;
    Map<ByteArray, Location> pchUSRMap(const List<Path> &pchFiles) const;
    void setPchUSRMap(const Path &pch, const PchUSRMap &astMap);
    void abort();
    bool visitFile(uint32_t fileId, const Path &p, bool isPch);
    Set<uint32_t> visitedFiles() const { MutexLocker lock(&mMutex); return mVisitedFiles; }
    ByteArray fixIts(const Path &path) const;
    ByteArray errors(const Path &path) const;
    void setDiagnostics(const Map<uint32_t, List<ByteArray> > &errors,
                        const Map<Location, std::pair<int, ByteArray> > &fixIts);
    void reindex(const ByteArray &pattern);
    signalslot::Signal1<Indexer*> &jobsComplete() { return mJobsComplete; }
    void onDirectoryChanged(const Path &path);
    Path srcRoot() const { return mSrcRoot; } // ~/src/foobar
    Path projectRoot() const { return mProjectRoot; } // ~/.rtags/projects/[_foobar_]
private:
    void onValidateDBJobErrors(const Set<Location> &errors);
    void onJobFinished(IndexerJob *job);
    void onDirtyThreadComplete(DirtyThread *job);
    void commitDependencies(const DependencyMap &deps, bool sync);
    void dirty(const Set<uint32_t> &dirtyFileIds,
               const Map<Path, List<ByteArray> > &dirtyPch,
               const Map<Path, List<ByteArray> > &dirty);

    enum InitMode {
        Normal,
        NoValidate,
        ForceDirty
    };
    void initDB(InitMode forceDirty, const ByteArray &pattern = ByteArray());
    bool needsToWaitForPch(IndexerJob *job) const;
    void startJob(IndexerJob *job);

    Map<Path, PchUSRMap> mPchUSRMaps;

    Set<uint32_t> mVisitedFiles;

    Map<Path, Set<uint32_t> > mPchDependencies;
    int mJobCounter;

    mutable Mutex mMutex;
    WaitCondition mWaitCondition;

    ByteArray mPath;
    Map<int, IndexerJob*> mJobs, mWaitingForPch, mWaitingForAbort;

    bool mTimerRunning;
    Timer mTimer;

    Path mSrcRoot, mProjectRoot;
    FileSystemWatcher mWatcher;
    DependencyMap mDependencies;
    WatchedMap mWatched;

    Map<Location, std::pair<int, ByteArray> > mFixIts;
    Map<uint32_t, ByteArray> mErrors;

    Set<Location> mPreviousErrors;

    signalslot::Signal1<Indexer*> mJobsComplete;

    struct CacheEntry
    {
    public:
        CacheEntry()
            : unit(0), index(0), flags(0), next(0), prev(0)
        {}
        ~CacheEntry()
        {
            assert(!unit == !index);
            if (unit) {
                clang_disposeTranslationUnit(unit);
                clang_disposeIndex(index);
            }
        }
        CXTranslationUnit unit;
        CXIndex index;
        Path path;
        List<ByteArray> args;
        unsigned flags; // this is with prioriy and'ed out
        CacheEntry *next, *prev;
    } *mFirst, *mLast;

    void removeCacheEntry(CacheEntry *entry);
    void insertCacheEntry(CacheEntry *entry);
    int mCacheSize;
};

inline bool Indexer::visitFile(uint32_t fileId, const Path &path, bool isPch)
{
    MutexLocker lock(&mMutex);
    if (!isPch && mVisitedFiles.contains(fileId)) {
        return false;
    }
    mVisitedFiles.insert(fileId);
    return true;
}
#endif

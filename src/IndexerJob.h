#ifndef IndexerJob_h
#define IndexerJob_h

#include "Indexer.h"
#include <RTags.h>
#include "Rdm.h"
#include "Job.h"
#include "Str.h"
#include "ThreadPool.h"
#include "AbortInterface.h"
#include <clang-c/Index.h>

class IndexerJob : public ThreadPool::Job, public AbortInterface
{
public:
    enum Flag {
        DirtyPch = 0x04,
        Dirty = 0x02,
        Makefile = 0x1, // these are used as ThreadPool priorites
        Priorities = DirtyPch|Dirty|Makefile,
        NeedsDirty = 0x010,
        FixIt = 0x020
    };
    IndexerJob(Indexer *indexer, int id, unsigned flags,
               const Path &input, const List<ByteArray> &arguments);
    int priority() const { return mFlags & Priorities; }
    virtual void run();
    void execute();

    const int mId;
    unsigned mFlags;
    bool mIsPch;
    Location createLocation(const CXCursor &cursor, bool *blocked);
    ByteArray addNamePermutations(const CXCursor &cursor, const Location &location, bool addToDb);
    static CXChildVisitResult indexVisitor(CXCursor cursor, CXCursor parent, CXClientData client_data);
    static void inclusionVisitor(CXFile included_file, CXSourceLocation *include_stack,
                                 unsigned include_len, CXClientData client_data);

    struct Cursor {
        CXCursor cursor;
        Location location;
        CXCursorKind kind;
    };

    CXChildVisitResult processCursor(const Cursor &cursor, const Cursor &ref);
    Cursor findByUSR(const CXCursor &cursor, CXCursorKind kind, const Location &loc);

    List<Cursor> mDelayed;
    SymbolMap mSymbols;
    SymbolNameMap mSymbolNames;

    enum PathState {
        Unset,
        Index,
        DontIndex,
        Reference
    };
    Map<uint32_t, PathState> mPaths;
    Map<Str, CXCursor> mHeaderMap;
    bool mDoneFullUSRScan;
    ReferenceMap mReferences;
    const Path mIn;
    const uint32_t mFileId;
    const List<ByteArray> mArgs;
    DependencyMap mDependencies;
    Set<uint32_t> mPchDependencies, mIndexed, mReferenced;
    Indexer *mIndexer;
    Map<ByteArray, Location> mPchUSRMap;

    List<Path> mPchHeaders;
    CXTranslationUnit mUnit;

    ByteArray mMessage;
};

class IndexerJobFinishedEvent : public Event
{
public:
    enum { Type = 1 };
    IndexerJobFinishedEvent(IndexerJob *j)
        : Event(Type), job(j)
    {}

    IndexerJob *job;
};

#endif

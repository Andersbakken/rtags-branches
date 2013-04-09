#include "IndexerJob.h"
#include "IndexerJobClang.h"
#include "IndexerJobEsprima.h"
#include "IndexerJobRParser.h"
#include <rct/StopWatch.h>
#include "Project.h"

// #define TIMINGS_ENABLED
#ifdef TIMINGS_ENABLED
static Mutex mutex;
static Map<const char*, uint64_t> times;
static void addTiming(const char *name, uint64_t usec)
{
    MutexLocker lock(&mutex);
    times[name] += usec;
}

struct TimingNode
{
    const char *key;
    uint64_t usecs;
    bool operator<(const TimingNode &other) const { return usecs > other.usecs; }
};

static void dumpTimings()
{
    MutexLocker lock(&mutex);
    List<TimingNode> nodes;
    uint64_t tot = 0;
    for (Map<const char*, uint64_t>::const_iterator it = times.begin(); it != times.end(); ++it) {
        if (!it->first) {
            TimingNode node = { "Total", it->second };
            nodes.append(node);
            tot = it->second;
        } else {
            TimingNode node = { it->first, it->second };
            nodes.append(node);
        }
    }
    if (tot) {
        std::sort(nodes.begin(), nodes.end());
        error("Timings:\n---------------------------");
        for (int i=0; i<nodes.size(); ++i) {
            error("%s: %llums (%.1f%%)", nodes.at(i).key, static_cast<unsigned long long>(nodes.at(i).usecs) / 1000,
                  (static_cast<double>(nodes.at(i).usecs) / static_cast<double>(tot)) * 100.0);
        }
    }
}
class Timing
{
public:
    Timing(const char *n) : name(n), watch(StopWatch::Microsecond) {}
    ~Timing() { addTiming(name, watch.elapsed()); }
    const char *name;
    StopWatch watch;
};
#define TIMING() Timing timing(__FUNCTION__)
#define NAMED_TIMING(name) addTiming(name, timing.watch.elapsed())
#else
#define TIMING() do {} while (0)
#define NAMED_TIMING(name) do {} while (0)
#endif

enum IndexerJobType {
    Esprima,
    Clang,
    RParser
};

static inline IndexerJobType indexerJobType(const SourceInformation &sourceInfo)
{
    if (sourceInfo.isJS()) {
        return Esprima;
    }
    char line[16];
    FILE *f = fopen(sourceInfo.sourceFile.constData(), "r");
    IndexerJobType ret = Clang;
    if (f) {
        if (Rct::readLine(f, line, sizeof(line) - 1) >= 10 && !strcmp("// RParser", line)) {
            ret = RParser;
        }
        fclose(f);
    }
    return ret;
}

shared_ptr<IndexerJob> IndexerJob::createIndex(const shared_ptr<Project> &project, Type type, const SourceInformation &sourceInformation)
{
    shared_ptr<IndexerJob> ret;
    switch (indexerJobType(sourceInformation)) {
    case Esprima: ret.reset(new IndexerJobEsprima(project, type, sourceInformation)); break;
    case Clang: ret.reset(new IndexerJobClang(project, type, sourceInformation)); break;
    case RParser: ret.reset(new IndexerJobRParser(project, type, sourceInformation)); break;
    }
    return ret;
}

shared_ptr<IndexerJob> IndexerJob::createDump(const QueryMessage &msg, const shared_ptr<Project> &project, const SourceInformation &sourceInformation)
{
    shared_ptr<IndexerJob> ret;
    switch (indexerJobType(sourceInformation)) {
    case Esprima: ret.reset(new IndexerJobEsprima(msg, project, sourceInformation)); break;
    case Clang: ret.reset(new IndexerJobClang(msg, project, sourceInformation)); break;
    case RParser: ret.reset(new IndexerJobRParser(msg, project, sourceInformation)); break;
    }
    return ret;
}

IndexerJob::IndexerJob(const shared_ptr<Project> &project, Type type, const SourceInformation &sourceInformation)
    : Job(0, project), mType(type), mSourceInformation(sourceInformation),
      mFileId(Location::insertFile(sourceInformation.sourceFile)), mTimer(StopWatch::Microsecond), mParseTime(0),
      mStarted(false), mAborted(false)
{}

IndexerJob::IndexerJob(const QueryMessage &msg, const shared_ptr<Project> &project,
                       const SourceInformation &sourceInformation)
    : Job(msg, WriteUnfiltered|WriteBuffered|QuietJob, project), mType(Dump), mSourceInformation(sourceInformation),
      mFileId(Location::insertFile(sourceInformation.sourceFile)), mTimer(StopWatch::Microsecond), mParseTime(0),
      mStarted(false), mAborted(false)
{
}

IndexerJob::~IndexerJob()
{
#ifdef TIMINGS_ENABLED
    addTiming(0, mTimer.elapsed()); // in ms
    dumpTimings();
#endif
}

Location IndexerJob::createLocation(const Path &file, uint32_t offset, bool *blocked)
{
    uint32_t &fileId = mFileIds[file];
    if (!fileId) {
        const Path resolved = file.resolved();
        fileId = mFileIds[resolved] = Location::insertFile(resolved);
    }
    return createLocation(fileId, offset, blocked);
}


Location IndexerJob::createLocation(uint32_t fileId, uint32_t offset, bool *blocked)
{
    TIMING();
    if (blocked)
        *blocked = false;
    if (fileId) {
        if (blocked) {
            if (mVisitedFiles.contains(fileId)) {
                *blocked = false;
            } else if (mBlockedFiles.contains(fileId)) {
                *blocked = true;
            } else {
                shared_ptr<Project> p = project();
                if (!p) {
                    return Location();
                } else if (p->visitFile(fileId)) {
                    if (blocked)
                        *blocked = false;
                    mVisitedFiles.insert(fileId);
                    mData->errors[fileId] = 0;
                    return Location(fileId, offset);
                } else {
                    mBlockedFiles.insert(fileId);
                    if (blocked)
                        *blocked = true;
                }
            }
        }
    }
    return Location();
}

bool IndexerJob::abortIfStarted()
{
    MutexLocker lock(&mutex());
    if (mStarted)
        aborted() = true;
    return mAborted;
}

void IndexerJob::execute()
{
    {
        MutexLocker lock(&mutex());
        mStarted = true;
    }
    mTimer.restart();
    mData.reset(new IndexData);

    index();
    shared_ptr<Project> p = project();
    if (p)
        p->onJobFinished(static_pointer_cast<IndexerJob>(shared_from_this()));
}

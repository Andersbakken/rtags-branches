#include "Project.h"
#include "FileManager.h"
#include "IndexerJob.h"
#include "Log.h"
#include "MemoryMonitor.h"
#include "Path.h"
#include "RTags.h"
#include "ReadLocker.h"
#include "RegExp.h"
#include "Server.h"
#include "Server.h"
#include "ValidateDBJob.h"
#include "WriteLocker.h"
#include <math.h>

static void *ModifiedFiles = &ModifiedFiles;
static void *Save = &Save;
static void *Sync = &Sync;

enum {
    SaveTimeout = 2000,
    ModifiedFilesTimeout = 50,
    SyncTimeout = 2000
};

Project::Project(const Path &path)
    : mPath(path), mJobCounter(0), mTimerRunning(false), mFlags(0),
      mFirstCachedUnit(0), mLastCachedUnit(0), mUnitCacheSize(0)
{
    const unsigned options = Server::instance()->options().options;
    if (options & Server::Validate)
        mFlags |= Project::Validate;
    if (options & Server::IgnorePrintfFixits)
        mFlags |= Project::IgnorePrintfFixits;
    mWatcher.modified().connect(this, &Project::onFileModified);
    mWatcher.removed().connect(this, &Project::onFileModified);
}

void Project::init()
{
    assert(!isValid());
    fileManager.reset(new FileManager);
    fileManager->init(static_pointer_cast<Project>(shared_from_this()));
}

bool Project::restore()
{
    StopWatch timer;
    Path path = mPath;
    RTags::encodePath(path);
    const Path p = ByteArray::format<128>("%s%s", Server::instance()->options().dataDir.constData(), path.constData());
    bool restoreError = false;
    FILE *f = fopen(p.constData(), "r");
    if (!f)
        return false;

    Deserializer in(f);
    int version;
    in >> version;
    if (version != Server::DatabaseVersion) {
        error("Wrong database version. Expected %d, got %d for %s. Removing.", Server::DatabaseVersion, version, p.constData());
        restoreError = true;
        goto end;
    }
    {
        int fs;
        in >> fs;
        if (fs != RTags::fileSize(f)) {
            error("%s seems to be corrupted, refusing to restore %s",
                  p.constData(), mPath.constData());
            restoreError = true;
            goto end;
        }
    }
    {
        {
            Scope<SymbolMap &> scope = lockSymbolsForWrite();
            in >> scope.data();
        }
        {
            Scope<SymbolNameMap &> scope = lockSymbolNamesForWrite();
            in >> scope.data();
        }
        {
            Scope<UsrMap &> scope = lockUsrForWrite();
            in >> scope.data();
        }


        in >> mDependencies >> mSources >> mVisitedFiles;

        DependencyMap reversedDependencies;
        // these dependencies are in the form of:
        // Path.cpp: Path.h, ByteArray.h ...
        // mDependencies are like this:
        // Path.h: Path.cpp, Server.cpp ...

        for (DependencyMap::const_iterator it = mDependencies.begin(); it != mDependencies.end(); ++it) {
            const Path dir = Location::path(it->first).parentDir();
            if (dir.isEmpty()) {
                error() << "File busted" << it->first << Location::path(it->first);
                continue;
            }
            if (mWatchedPaths.insert(dir))
                mWatcher.watch(dir);
            for (Set<uint32_t>::const_iterator s = it->second.begin(); s != it->second.end(); ++s) {
                reversedDependencies[*s].insert(it->first);
            }
        }

        SourceInformationMap::iterator it = mSources.begin();
        while (it != mSources.end()) {
            if (!it->second.sourceFile.isFile()) {
                error() << it->second.sourceFile << "seems to have disappeared";
                mSources.erase(it++);
                mModifiedFiles.insert(it->first);
            } else {
                const time_t parsed = it->second.parsed;
                // error() << "parsed" << ByteArray::timeToString(parsed, ByteArray::DateTime) << parsed;
                assert(mDependencies.value(it->first).contains(it->first));
                assert(mDependencies.contains(it->first));
                const Set<uint32_t> &deps = reversedDependencies[it->first];
                for (Set<uint32_t>::const_iterator d = deps.begin(); d != deps.end(); ++d) {
                    if (!mModifiedFiles.contains(*d) && Location::path(*d).lastModified() > parsed)
                        mModifiedFiles.insert(*d);
                }
                ++it;
            }
        }
        if (!mModifiedFiles.isEmpty())
            startDirtyJobs();
    }
end:
    fclose(f);
    if (restoreError) {
        Path::rm(p);
        return false;
    } else {
        error() << "Restored project" << mPath << "in" << timer.elapsed() << "ms";
    }

    return true;
}

bool Project::isValid() const
{
    return fileManager.get();
}

Scope<const SymbolMap&> Project::lockSymbolsForRead(int maxTime)
{
    Scope<const SymbolMap&> scope;
    if (mSymbolsLock.lockForRead(maxTime))
        scope.mData.reset(new Scope<const SymbolMap&>::Data(mSymbols, &mSymbolsLock));
    return scope;
}

Scope<SymbolMap&> Project::lockSymbolsForWrite()
{
    Scope<SymbolMap&> scope;
    mSymbolsLock.lockForWrite();
    scope.mData.reset(new Scope<SymbolMap&>::Data(mSymbols, &mSymbolsLock));
    return scope;
}

Scope<const SymbolNameMap&> Project::lockSymbolNamesForRead(int maxTime)
{
    Scope<const SymbolNameMap&> scope;
    if (mSymbolNamesLock.lockForRead(maxTime))
        scope.mData.reset(new Scope<const SymbolNameMap&>::Data(mSymbolNames, &mSymbolNamesLock));
    return scope;
}

Scope<SymbolNameMap&> Project::lockSymbolNamesForWrite()
{
    Scope<SymbolNameMap&> scope;
    mSymbolNamesLock.lockForWrite();
    scope.mData.reset(new Scope<SymbolNameMap&>::Data(mSymbolNames, &mSymbolNamesLock));
    return scope;
}

Scope<const UsrMap&> Project::lockUsrForRead(int maxTime)
{
    Scope<const UsrMap&> scope;
    if (mUsrLock.lockForRead(maxTime))
        scope.mData.reset(new Scope<const UsrMap&>::Data(mUsr, &mUsrLock));
    return scope;
}

Scope<UsrMap&> Project::lockUsrForWrite()
{
    Scope<UsrMap&> scope;
    mUsrLock.lockForWrite();
    scope.mData.reset(new Scope<UsrMap&>::Data(mUsr, &mUsrLock));
    return scope;

}
Scope<const FilesMap&> Project::lockFilesForRead(int maxTime)
{
    Scope<const FilesMap&> scope;
    if (mFilesLock.lockForRead(maxTime))
        scope.mData.reset(new Scope<const FilesMap&>::Data(mFiles, &mFilesLock));
    return scope;
}

Scope<FilesMap&> Project::lockFilesForWrite()
{
    Scope<FilesMap&> scope;
    mFilesLock.lockForWrite();
    scope.mData.reset(new Scope<FilesMap&>::Data(mFiles, &mFilesLock));
    return scope;
}

void Project::unload()
{
    MutexLocker lock(&mMutex);
    for (Map<uint32_t, shared_ptr<IndexerJob> >::const_iterator it = mJobs.begin(); it != mJobs.end(); ++it) {
        it->second->abort();
    }
    mJobs.clear();
    fileManager.reset();
}

bool Project::match(const Match &p)
{
    Path paths[] = { p.pattern(), p.pattern() };
    paths[1].resolve();
    const int count = paths[1] != paths[0] ? 2 : 1;
    Scope<const FilesMap&> files = lockFilesForRead();
    for (int i=0; i<count; ++i) {
        const Path &path = paths[i];
        if (files.data().contains(path) || p.match(mPath))
            return true;
        const uint32_t id = Location::fileId(path);
        if (isIndexed(id))
            return true;

    }
    return false;
}

void Project::onJobFinished(const shared_ptr<IndexerJob> &job)
{
    PendingJob pending;
    bool startPending = false;
    {
        MutexLocker lock(&mMutex);

        if (Server::instance()->options().completionCacheSize) {
            CXTranslationUnit unit = job->takeTranslationUnit();
            if (unit)
                addCachedUnit(job->path(), job->arguments(), job->takeIndex(), unit);
        }

        const uint32_t fileId = job->fileId();
        if (job->isAborted()) {
            mVisitedFiles -= job->visitedFiles();
            --mJobCounter;
            pending = mPendingJobs.take(fileId, &startPending);
            if (mJobs.value(fileId) == job)
                mJobs.remove(fileId);
        } else {
            assert(mJobs.value(fileId) == job);
            mJobs.remove(fileId);

            shared_ptr<IndexData> data = job->data();
            mPendingData[fileId] = data;

            const int idx = mJobCounter - mJobs.size();

            mSources[fileId].parsed = job->parseTime();
            error("[%3d%%] %d/%d %s %s. %d mb mem.",
                  static_cast<int>(round((double(idx) / double(mJobCounter)) * 100.0)), idx, mJobCounter,
                  ByteArray::timeToString(time(0), ByteArray::Time).constData(),
                  data->message.constData(), int((MemoryMonitor::usage() / (1024 * 1024))));

            if (mJobs.isEmpty() && job->flags() & IndexerJob::Dirty) {
                const int syncTime = syncDB();
                error() << "Jobs took" << (static_cast<double>(mTimer.elapsed()) / 1000.0) << "secs, syncing took"
                        << (static_cast<double>(syncTime) / 1000.0) << " secs, using"
                        << MemoryMonitor::usage() / (1024.0 * 1024.0) << "mb of memory";
                mTimerRunning = false;
                mSaveTimer.start(shared_from_this(), SaveTimeout, SingleShot, Save);
                mJobCounter = 0;
            } else if (mJobs.isEmpty()) {
                mSyncTimer.start(shared_from_this(), SyncTimeout, SingleShot, Sync);
            }
        }
    }
    if (startPending)
        index(pending.source, pending.jobFlags);
}

bool Project::save()
{
    MutexLocker lock(&mMutex);
    if (!Server::instance()->saveFileIds())
        return false;

    StopWatch timer;
    Path srcPath = mPath;
    RTags::encodePath(srcPath);
    const Server::Options &options = Server::instance()->options();
    const Path p = options.dataDir + srcPath;
    FILE *f = fopen(p.constData(), "w");
    if (!f) {
        error("Can't open file %s", p.constData());
        return false;
    }
    Serializer out(f);
    out << static_cast<int>(Server::DatabaseVersion);
    const int pos = ftell(f);
    out << static_cast<int>(0);
    {
        Scope<const SymbolMap &> scope = lockSymbolsForRead();
        out << scope.data();
    }
    {
        Scope<const SymbolNameMap &> scope = lockSymbolNamesForRead();
        out << scope.data();
    }
    {
        Scope<const UsrMap &> scope = lockUsrForRead();
        out << scope.data();
    }
    out << mDependencies << mSources << mVisitedFiles;

    const int size = ftell(f);
    fseek(f, pos, SEEK_SET);
    out << size;

    error() << "saved project" << path() << "in" << ByteArray::format<12>("%dms", timer.elapsed()).constData();
    fclose(f);
    return true;
}

void Project::index(const SourceInformation &c, unsigned indexerJobFlags)
{
    MutexLocker locker(&mMutex);
    static const char *fileFilter = getenv("RTAGS_FILE_FILTER");
    if (fileFilter && !strstr(c.sourceFile.constData(), fileFilter))
        return;
    mSyncTimer.stop();
    mSaveTimer.stop();
    const uint32_t fileId = Location::insertFile(c.sourceFile);
    shared_ptr<IndexerJob> &job = mJobs[fileId];
    if (job) {
        if (job->abortIfStarted()) {
            const PendingJob pending = { c, indexerJobFlags };
            mPendingJobs[fileId] = pending;
        }
        return;
    }

    mSources[fileId] = c;
    mPendingData.remove(fileId);

    if (mFlags & IgnorePrintfFixits)
        indexerJobFlags |= IndexerJob::IgnorePrintfFixits;

    CXIndex index = 0;
    CXTranslationUnit unit = 0;
    initJobFromCache(c.sourceFile, c.args, index, unit, 0);
    shared_ptr<Project> project = static_pointer_cast<Project>(shared_from_this());
    job.reset(new IndexerJob(project, indexerJobFlags, c.sourceFile, c.args, index, unit));

    ++mJobCounter;
    if (!mTimerRunning) {
        mTimerRunning = true;
        mTimer.start();
    }
    Server::instance()->startIndexerJob(job);
}

void Project::onFileModified(const Path &file)
{
    const uint32_t fileId = Location::fileId(file);
    warning() << file << "was modified" << fileId << mModifiedFiles.contains(fileId);
    if (!fileId || !mModifiedFiles.insert(fileId)) {
        return;
    }
    if (mModifiedFiles.size() == 1 && file.isSource()) {
        startDirtyJobs();
    } else {
        mModifiedFilesTimer.start(shared_from_this(), ModifiedFilesTimeout,
                                  SingleShot, ModifiedFiles);
    }
}


SourceInformation Project::sourceInfo(uint32_t fileId) const
{
    if (fileId) {
        MutexLocker lock(&mMutex);
        return mSources.value(fileId);
    }
    return SourceInformation();
}

void Project::addDependencies(const DependencyMap &deps, Set<uint32_t> &newFiles)
{
    StopWatch timer;

    const DependencyMap::const_iterator end = deps.end();
    for (DependencyMap::const_iterator it = deps.begin(); it != end; ++it) {
        Set<uint32_t> &values = mDependencies[it->first];
        if (values.isEmpty()) {
            values = it->second;
        } else {
            values.unite(it->second);
        }
        if (newFiles.isEmpty()) {
            newFiles = it->second;
        } else {
            newFiles.unite(it->second);
        }
        newFiles.insert(it->first);
    }
}

Set<uint32_t> Project::dependencies(uint32_t fileId) const
{
    MutexLocker lock(&mMutex);
    return mDependencies.value(fileId);
}

ByteArray Project::diagnostics() const
{
    MutexLocker lock(&mMutex);
    List<ByteArray> ret;
    for (DiagnosticsMap::const_iterator it = mDiagnostics.begin(); it != mDiagnostics.end(); ++it) {
        ret += it->second;
    }
    return ByteArray::join(ret, '\n');
}

int Project::reindex(const Match &match)
{
    Set<uint32_t> dirty;
    {
        MutexLocker lock(&mMutex);

        const DependencyMap::const_iterator end = mDependencies.end();
        for (DependencyMap::const_iterator it = mDependencies.begin(); it != end; ++it) {
            if (!mPendingDirtyFiles.contains(it->first)
                && (match.isEmpty() || match.match(Location::path(it->first)))) {
                dirty.insert(it->first);
            }
        }
        if (dirty.isEmpty())
            return 0;
        mModifiedFiles += dirty;
    }
    startDirtyJobs();
    return dirty.size();
}

int Project::remove(const Match &match)
{
    int count = 0;
    {
        MutexLocker lock(&mMutex);
        SourceInformationMap::iterator it = mSources.begin();
        while (it != mSources.end()) {
            if (match.match(it->second.sourceFile)) {
                const uint32_t fileId = Location::insertFile(it->second.sourceFile);
                mSources.erase(it++);
                shared_ptr<IndexerJob> job = mJobs.value(fileId);
                if (job)
                    job->abort();
                mPendingData.remove(fileId);
                mPendingJobs.remove(fileId);
                ++count;
            } else {
                ++it;
            }
        }
    }
    return count;
}


void Project::onValidateDBJobErrors(const Set<Location> &errors)
{
    MutexLocker lock(&mMutex);
    mPreviousErrors = errors;
}

void Project::startDirtyJobs()
{
    Set<uint32_t> dirtyFiles;
    Map<Path, List<ByteArray> > toIndex;
    {
        MutexLocker lock(&mMutex);
        std::swap(dirtyFiles, mModifiedFiles);
        for (Set<uint32_t>::const_iterator it = dirtyFiles.begin(); it != dirtyFiles.end(); ++it) {
            const Set<uint32_t> deps = mDependencies.value(*it);
            dirtyFiles += deps;
            mVisitedFiles.remove(*it);
            mVisitedFiles -= deps;
        }
        mPendingDirtyFiles.unite(dirtyFiles);
    }
    bool indexed = false;
    for (Set<uint32_t>::const_iterator it = dirtyFiles.begin(); it != dirtyFiles.end(); ++it) {
        const SourceInformationMap::const_iterator found = mSources.find(*it);
        if (found != mSources.end()) {
            index(found->second, IndexerJob::Dirty);
            indexed = true;
        }
    }
    if (!indexed && !mPendingDirtyFiles.isEmpty()) {
        {
            Scope<SymbolMap&> symbols = lockSymbolsForWrite();
            RTags::dirtySymbols(symbols.data(), mPendingDirtyFiles);
        }
        {
            Scope<SymbolNameMap&> symbolNames = lockSymbolNamesForWrite();
            RTags::dirtySymbolNames(symbolNames.data(), mPendingDirtyFiles);
        }
        {
            Scope<UsrMap&> usr = lockUsrForWrite();
            RTags::dirtyUsr(usr.data(), mPendingDirtyFiles);
        }
        mPendingDirtyFiles.clear();
    }
}

static inline void writeSymbolNames(const SymbolNameMap &symbolNames, SymbolNameMap &current)
{
    SymbolNameMap::const_iterator it = symbolNames.begin();
    const SymbolNameMap::const_iterator end = symbolNames.end();
    while (it != end) {
        Set<Location> &value = current[it->first];
        value.unite(it->second);
        ++it;
    }
}

static inline void joinCursors(SymbolMap &symbols, const Set<Location> &locations)
{
    for (Set<Location>::const_iterator it = locations.begin(); it != locations.end(); ++it) {
        SymbolMap::iterator c = symbols.find(*it);
        if (c != symbols.end()) {
            CursorInfo &cursorInfo = c->second;
            for (Set<Location>::const_iterator innerIt = locations.begin(); innerIt != locations.end(); ++innerIt) {
                if (innerIt != it)
                    cursorInfo.targets.insert(*innerIt);
            }
            // ### this is filthy, we could likely think of something better
        }
    }
}

static inline void writeUsr(const UsrMap &usr, UsrMap &current, SymbolMap &symbols)
{
    UsrMap::const_iterator it = usr.begin();
    const UsrMap::const_iterator end = usr.end();
    while (it != end) {
        Set<Location> &value = current[it->first];
        int count = 0;
        value.unite(it->second, &count);
        if (count && value.size() > 1)
            joinCursors(symbols, value);
        ++it;
    }
}

static inline void writeCursors(SymbolMap &symbols, SymbolMap &current)
{
    if (!symbols.isEmpty()) {
        SymbolMap::iterator it = symbols.begin();
        const SymbolMap::iterator end = symbols.end();
        while (it != end) {
            // This is kind of a hack but we use these cursors' symbolnames
            // to make the symbolname of references earlier so we can't
            // inject the class/struct/union stuff then and here we're
            // anyway walking the whole lot of them.
            switch (it->second.kind) {
            case CXCursor_ClassDecl:
            case CXCursor_ClassTemplate:
                it->second.symbolName.prepend("class ");
                break;
            case CXCursor_StructDecl:
                it->second.symbolName.prepend("struct ");
                break;
            case CXCursor_UnionDecl:
                it->second.symbolName.prepend("union ");
                break;
            default:
                break;
            }
            if (!current.isEmpty()) {
                SymbolMap::iterator cur = current.find(it->first);
                if (cur == current.end()) {
                    current[it->first] = it->second;
                } else {
                    cur->second.unite(it->second);
                }
            }
            ++it;
        }
        if (current.isEmpty()) {
            current = symbols;
        }
    }
}

static inline void writeReferences(const ReferenceMap &references, SymbolMap &symbols)
{
    if (!references.isEmpty()) {
        const ReferenceMap::const_iterator end = references.end();
        for (ReferenceMap::const_iterator it = references.begin(); it != end; ++it) {
            const Set<Location> &refs = it->second;
            for (Set<Location>::const_iterator rit = refs.begin(); rit != refs.end(); ++rit) {
                CursorInfo &ci = symbols[*rit];
                ci.references.insert(it->first);
            }
        }
    }
}


int Project::syncDB()
{
    if (mPendingDirtyFiles.isEmpty() && mPendingData.isEmpty())
        return -1;
    StopWatch watch;
    Scope<SymbolMap&> symbols = lockSymbolsForWrite();
    Scope<SymbolNameMap&> symbolNames = lockSymbolNamesForWrite();
    Scope<UsrMap&> usr = lockUsrForWrite();
    if (!mPendingDirtyFiles.isEmpty()) {
        RTags::dirtySymbols(symbols.data(), mPendingDirtyFiles);
        RTags::dirtySymbolNames(symbolNames.data(), mPendingDirtyFiles);
        RTags::dirtyUsr(usr.data(), mPendingDirtyFiles);
        mPendingDirtyFiles.clear();
    }

    Set<uint32_t> newFiles;
    for (Map<uint32_t, shared_ptr<IndexData> >::iterator it = mPendingData.begin(); it != mPendingData.end(); ++it) {
        const shared_ptr<IndexData> &data = it->second;
        addDependencies(data->dependencies, newFiles);
        addDiagnostics(data->dependencies, data->diagnostics, data->fixIts);
        writeCursors(data->symbols, symbols.data());
        writeUsr(data->usrMap, usr.data(), symbols.data());
        writeReferences(data->references, symbols.data());
        writeSymbolNames(data->symbolNames, symbolNames.data());
    }
    for (Set<uint32_t>::const_iterator it = newFiles.begin(); it != newFiles.end(); ++it) {
        const Path path = Location::path(*it);
        const Path dir = path.parentDir();
        if (dir.isEmpty()) {
            error() << "Got empty parent dir for" << path << *it;
        } else if (mWatchedPaths.insert(dir)) {
            mWatcher.watch(dir);
        }
    }
    mPendingData.clear();
    return watch.elapsed();
}

bool Project::isIndexed(uint32_t fileId) const
{
    MutexLocker lock(&mMutex);
    return mVisitedFiles.contains(fileId) || mSources.contains(fileId);
}

SourceInformationMap Project::sources() const
{
    MutexLocker lock(&mMutex);
    return mSources;
}
DependencyMap Project::dependencies() const
{
    MutexLocker lock(&mMutex);
    return mDependencies;
}

void Project::addCachedUnit(const Path &path, const List<ByteArray> &args, CXIndex index, CXTranslationUnit unit)
{
    assert(index);
    assert(unit);
    CachedUnit *cachedUnit = new CachedUnit;
    cachedUnit->path = path;
    cachedUnit->index = index;
    cachedUnit->unit = unit;
    cachedUnit->arguments = args;
    if (!mFirstCachedUnit) {
        assert(!mLastCachedUnit);
        assert(!mUnitCacheSize);
        mFirstCachedUnit = mLastCachedUnit = cachedUnit;
        mUnitCacheSize = 1;
        return;
    }

    const int maxCacheSize = Server::instance()->options().completionCacheSize;
    assert(maxCacheSize >= 1);
    if (mUnitCacheSize == maxCacheSize) {
        CachedUnit *tmp = mFirstCachedUnit;
        mFirstCachedUnit = tmp->next;
        if (!mFirstCachedUnit)
            mLastCachedUnit = 0;
        delete tmp;
    } else {
        ++mUnitCacheSize;
    }
    if (!mLastCachedUnit) {
        mLastCachedUnit = mFirstCachedUnit = cachedUnit;
    } else {
        assert(mLastCachedUnit);
        assert(!mLastCachedUnit->next);
        mLastCachedUnit->next = cachedUnit;
        mLastCachedUnit = cachedUnit;
    }
}

bool Project::initJobFromCache(const Path &path, const List<ByteArray> &args,
                               CXIndex &index, CXTranslationUnit &unit, List<ByteArray> *argsOut)
{
    CachedUnit *prev = 0;
    CachedUnit *cachedUnit = mFirstCachedUnit;
    while (cachedUnit) {
        if (cachedUnit->path == path && (args.isEmpty() || args == cachedUnit->arguments)) {
            index = cachedUnit->index;
            unit = cachedUnit->unit;
            cachedUnit->unit = 0;
            cachedUnit->index = 0;
            if (prev) {
                prev->next = cachedUnit->next;
                if (cachedUnit == mLastCachedUnit)
                    mLastCachedUnit = prev;
            } else {
                mFirstCachedUnit = cachedUnit->next;
                if (!mFirstCachedUnit)
                    mLastCachedUnit = 0;
            }
            --mUnitCacheSize;
            assert(mUnitCacheSize >= 0);
            if (argsOut)
                *argsOut = cachedUnit->arguments;
            delete cachedUnit;
            return true;
        }
        prev = cachedUnit;
        cachedUnit = cachedUnit->next;
    }
    index = 0;
    unit = 0;
    return false;
}

bool Project::fetchFromCache(const Path &path, List<ByteArray> &args, CXIndex &index, CXTranslationUnit &unit)
{
    MutexLocker lock(&mMutex);
    return initJobFromCache(path, List<ByteArray>(), index, unit, &args);
}

void Project::addToCache(const Path &path, const List<ByteArray> &args, CXIndex index, CXTranslationUnit unit)
{
    MutexLocker lock(&mMutex);
    addCachedUnit(path, args, index, unit);
}

void Project::addDiagnostics(const DependencyMap &visited, const DiagnosticsMap &diagnostics, const FixItMap &fixIts) // lock always held
{
    for (DependencyMap::const_iterator it = visited.begin(); it != visited.end(); ++it) {
        const FixItMap::const_iterator fit = fixIts.find(it->first);
        if (fit == fixIts.end()) {
            mFixIts.erase(it->first);
        } else {
            mFixIts[it->first] = fit->second;
        }
        const DiagnosticsMap::const_iterator dit = diagnostics.find(it->first);
        if (dit == diagnostics.end()) {
            mDiagnostics.erase(it->first);
        } else {
            mDiagnostics[it->first] = dit->second;
        }

    }
}

ByteArray Project::fixIts(uint32_t fileId) const
{
    MutexLocker lock(&mMutex);
    const FixItMap::const_iterator it = mFixIts.find(fileId);
    ByteArray out;
    if (it != mFixIts.end()) {
        const Set<FixIt> &fixIts = it->second;
        if (!fixIts.isEmpty()) {
            Set<FixIt>::const_iterator f = fixIts.end();
            do {
                --f;
                if (!out.isEmpty())
                    out.append('\n');
                out.append(ByteArray::format<32>("%d-%d %s", f->start, f->end, f->text.constData()));

            } while (f != fixIts.begin());
        }
    }
    return out;
}

void Project::timerEvent(TimerEvent *e)
{
    if (e->userData() == Save) {
        save();
    } else if (e->userData() == Sync) {
        const int syncTime = syncDB();
        error() << "Jobs took" << (static_cast<double>(mTimer.elapsed()) / 1000.0) << "secs, syncing took"
                << (static_cast<double>(syncTime) / 1000.0) << " secs, using"
                << MemoryMonitor::usage() / (1024.0 * 1024.0) << "mb of memory";
        mTimerRunning = false;
        mSaveTimer.start(shared_from_this(), SaveTimeout, SingleShot, Save);
        mJobCounter = 0;
    } else if (e->userData() == ModifiedFiles) {
        startDirtyJobs();
    } else {
        assert(0 && "Unexpected timer event in Project");
        e->stop();
    }
}


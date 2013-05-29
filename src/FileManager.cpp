#include "FileManager.h"
#include "Server.h"
#include "Filter.h"
#include "Project.h"

FileManager::FileManager()
{
    mWatcher.added().connect(this, &FileManager::onFileAdded);
    mWatcher.removed().connect(this, &FileManager::onFileRemoved);
}

void FileManager::init(const shared_ptr<Project> &proj)
{
    mProject = proj;
    recurseDirs();
}

class ScanThread : public Thread
{
public:
    ScanThread(const Path &path)
        : mPath(path), mFilters(Server::options().excludeFilters)
    {
        if (!mPath.endsWith('/'))
            mPath.append('/');
    }

    signalslot::Signal1<Set<Path> > &finished() { return mFinished; }
protected:
    virtual void run()
    {
        mPath.visit(&ScanThread::visit, this);
        mFinished(mPaths);
    }
private:
    static Path::VisitResult visit(const Path &path, void *userData)
    {
        ScanThread *thread = reinterpret_cast<ScanThread*>(userData);
        const Filter::Result result = Filter::filter(path, thread->mFilters);
        switch (result) {
        case Filter::Filtered:
            return Path::Continue;
        case Filter::Directory:
            if (Path::exists(path + "/.rtags-ignore"))
                return Path::Continue;
            return Path::Recurse;
        case Filter::File:
        case Filter::Source:
            thread->mPaths.insert(path);
            break;
        }
        return Path::Continue;
    }

    Path mPath;
    const List<Path> mFilters;
    Set<Path> mPaths;
    signalslot::Signal1<Set<Path> > mFinished;
};

void FileManager::recurseDirs()
{
    shared_ptr<Project> project = mProject.lock();
    assert(project);
    ScanThread *thread = new ScanThread(project->path());
    thread->setAutoDelete(true);
    thread->finished().connectAsync(this, &FileManager::onScanFinished);
    thread->start();
}

void FileManager::onScanFinished(Set<Path> paths)
{
    bool emitJS = false;
    {
        MutexLocker lock(&mMutex); // ### is this needed now?
        Set<Path> old;
        std::swap(mJSFiles, old);

        shared_ptr<Project> project = mProject.lock();
        assert(project);
        FilesMap &map = project->filesMap();
        mWatcher.clear();
        for (Set<Path>::const_iterator it = paths.begin(); it != paths.end(); ++it) {
            if (it->endsWith(".js"))
                mJSFiles.insert(*it);
            const Path parent = it->parentDir();
            if (parent.isEmpty()) {
                error() << "Got empty parent here" << *it;
                continue;
            }
            assert(!parent.isEmpty());
            Set<String> &dir = map[parent];
            if (dir.isEmpty())
                mWatcher.watch(parent);
            dir.insert(it->fileName());
        }
        assert(!map.contains(""));
        emitJS = old != mJSFiles;
    }
    if (emitJS)
        mJSFilesChanged();
}

void FileManager::onFileAdded(const Path &path)
{
    bool emitJS = false;
    {
        MutexLocker lock(&mMutex);
        if (path.isEmpty()) {
            error("Got empty file added here");
            return;
        }
        const Filter::Result res = Filter::filter(path);
        switch (res) {
        case Filter::Directory:
            recurseDirs();
            return;
        case Filter::Filtered:
            return;
        default:
            break;
        }

        shared_ptr<Project> project = mProject.lock();
        assert(project);
        FilesMap &map = project->filesMap();
        const Path parent = path.parentDir();
        if (!parent.isEmpty()) {
            Set<String> &dir = map[parent];
            if (dir.isEmpty())
                mWatcher.watch(parent);
            dir.insert(path.fileName());
            emitJS = path.endsWith(".js");
        } else {
            error() << "Got empty parent here" << path;
        }
        assert(!map.contains(Path()));
    }
    if (emitJS)
        mJSFilesChanged();
}

void FileManager::onFileRemoved(const Path &path)
{
    MutexLocker lock(&mMutex);
    shared_ptr<Project> project = mProject.lock();
    FilesMap &map = project->filesMap();
    if (map.contains(path)) {
        recurseDirs();
        return;
    }
    const Path parent = path.parentDir();
    if (map.contains(parent)) {
        Set<String> &dir = map[parent];
        dir.remove(path.fileName());
        if (dir.isEmpty()) {
            mWatcher.unwatch(parent);
            map.remove(parent);
        }
    }
}

static inline bool startsWith(const Path &left, const Path &right)
{
    assert(!left.isEmpty());
    return !right.isEmpty() && left.startsWith(right);
}

bool FileManager::contains(const Path &path) const
{
    MutexLocker lock(&mMutex);
    shared_ptr<Project> proj = mProject.lock();
    if (!proj)
        return false;
    if (startsWith(path, proj->path()))
        return true;
    const Path p = Path::resolved(path);
    if (p != path && startsWith(path, proj->path()))
        return true;
    return false;
}

void FileManager::reload()
{
    MutexLocker lock(&mMutex);
    shared_ptr<Project> proj = mProject.lock();
    FilesMap &map = proj->filesMap();
    map.clear();
    recurseDirs();
}
Set<Path> FileManager::jsFiles() const
{
    MutexLocker lock(&mMutex);
    return mJSFiles;
}

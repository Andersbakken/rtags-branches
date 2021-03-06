#include "FileManager.h"
#include "GRScanJob.h"
#include "Server.h"
#include "Indexer.h"

FileManager::FileManager()
{
    mWatcher.added().connect(this, &FileManager::onFileAdded);
    mWatcher.removed().connect(this, &FileManager::onFileRemoved);
}

void FileManager::init(const std::shared_ptr<Project> &proj)
{
    mProject = proj;
    recurseDirs();
}

void FileManager::recurseDirs()
{
    std::shared_ptr<Project> project = mProject.lock();
    assert(project);
    std::shared_ptr<GRScanJob> job(new GRScanJob(GRScanJob::All, project->srcRoot, mProject.lock()));
    job->finished().connect(this, &FileManager::onRecurseJobFinished);
    Server::instance()->threadPool()->start(job);
}

void FileManager::onRecurseJobFinished(const Set<Path> &paths)
{
    std::shared_ptr<Project> project = mProject.lock();
    assert(project);
    Scope<FilesMap&> scope = project->lockFilesForWrite();
    FilesMap &map = scope.data();
    mWatcher.clear();
    for (Set<Path>::const_iterator it = paths.begin(); it != paths.end(); ++it) {
        const Path parent = it->parentDir();
        if (parent.isEmpty()) {
            error() << "Got empty parent here" << *it;
            continue;
        }
        Set<ByteArray> &dir = map[parent];
        if (dir.isEmpty())
            mWatcher.watch(parent);
        dir.insert(it->fileName());
    }
}

void FileManager::onFileAdded(const Path &path)
{
    if (path.isEmpty()) {
        error("Got empty file added here");
        return;
    }
    const GRScanJob::FilterResult res = GRScanJob::filter(path, Server::instance()->excludeFilter());
    switch (res) {
    case GRScanJob::Directory:
        recurseDirs();
        return;
    case GRScanJob::Filtered:
        return;
    default:
        break;
    }

    std::shared_ptr<Project> project = mProject.lock();
    assert(project);
    Scope<FilesMap&> scope = project->lockFilesForWrite();
    FilesMap &map = scope.data();
    const Path parent = path.parentDir();
    Set<ByteArray> &dir = map[parent];
    if (dir.isEmpty())
        mWatcher.watch(parent);
    dir.insert(path.fileName());
}

void FileManager::onFileRemoved(const Path &path)
{
    std::shared_ptr<Project> project = mProject.lock();
    Scope<FilesMap&> scope = project->lockFilesForWrite();
    FilesMap &map = scope.data();
    if (map.contains(path)) {
        recurseDirs();
        return;
    }
    const Path parent = path.parentDir();
    Set<ByteArray> &dir = map[parent];
    if (dir.remove(path.fileName()) && dir.isEmpty()) {
        mWatcher.unwatch(parent);
        map.remove(parent);
    }
}

bool FileManager::contains(const Path &path) const
{
    std::shared_ptr<Project> proj = mProject.lock();
    if(!proj)
        return false;
    Path resolvedPath = proj->srcRoot;
    resolvedPath.resolve();
    return (path.size() >= resolvedPath.size()
            && !strncmp(path.constData(), resolvedPath.constData(), resolvedPath.size()));
}

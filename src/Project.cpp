#include "Project.h"
#include "Indexer.h"
#include "FileManager.h"
#include "GRTags.h"
#include "Server.h"

Project::Project(const Path &src)
    : srcRoot(src)
{
    resolvedSrcRoot = src;
    resolvedSrcRoot.resolve();
    if (resolvedSrcRoot == srcRoot)
        resolvedSrcRoot.clear();
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

Scope<const GRMap&> Project::lockGRForRead(int maxTime)
{
    Scope<const GRMap&> scope;
    if (mGRLock.lockForRead(maxTime))
        scope.mData.reset(new Scope<const GRMap&>::Data(mGR, &mGRLock));
    return scope;
}

Scope<GRMap&> Project::lockGRForWrite()
{
    Scope<GRMap&> scope;
    mGRLock.lockForWrite();
    scope.mData.reset(new Scope<GRMap&>::Data(mGR, &mGRLock));
    return scope;
}

Scope<const GRFilesMap&> Project::lockGRFilesForRead(int maxTime)
{
    Scope<const GRFilesMap&> scope;
    if (mGRFilesLock.lockForRead(maxTime))
        scope.mData.reset(new Scope<const GRFilesMap&>::Data(mGRFiles, &mGRFilesLock));
    return scope;
}

Scope<GRFilesMap&> Project::lockGRFilesForWrite()
{
    Scope<GRFilesMap&> scope;
    mGRFilesLock.lockForWrite();
    scope.mData.reset(new Scope<GRFilesMap&>::Data(mGRFiles, &mGRFilesLock));
    return scope;
}

bool Project::isIndexed(uint32_t fileId) const
{
    if (indexer)
        return indexer->isIndexed(fileId);
    if (grtags)
        return grtags->isIndexed(fileId);
    return false;
}

bool Project::save(Serializer &out)
{
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

    return true;
}

bool Project::restore(Deserializer &in)
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

    return true;
}

Location Project::findLocation(const ByteArray &usr)
{
    if (!usr.isEmpty()) {
        Scope<const SymbolMap &> scope = lockSymbolsForRead();
        const SymbolMap &map = scope.data();
        const SymbolMap::const_iterator it = map.find(usr);
        if (it != map.end())
            return it->second.location;
    }
    return Location();
}


ByteArray Project::findUsr(const Location &location)
{
    if (!indexer)
        return ByteArray();
    Scope<const UsrMap &> scope = lockUsrForRead();
    const UsrMap &map = scope.data();

    UsrMap::const_iterator it = map.find(location);
    if (it != map.end())
        return it->second.first;
    it = map.lower_bound(location);
    if (it == map.end()) {
        --it;
    } else {
        const int cmp = it->first.compare(location);
        if (!cmp) {
            assert(0);
            // ### Don't think this should ever happen given the find above,
            // ### we could probably also assume that if (it > location) we've
            // ### already lost
            return it->second.first;
        }
        --it;
    }
    if (location.fileId() != it->first.fileId())
        return map.end();
    const int off = location.offset() - it->first.offset();
    if (it->second.second > off) // ### should this be >=
        return it->second.first;
    return ByteArray();
}

CursorInfo Project::findCursorInfo(const Location &location)
{
    const ByteArray usr = findUsr(location);
    if (!usr.isEmpty()) {
        Scope<const SymbolMap &> scope = lockSymbolsForRead();
        const SymbolMap &map = scope.data();
        const SymbolMap::const_iterator it = map.find(usr);
        if (it != map.end())
            return it->second;
    }
    return CursorInfo();
}

static ThreadLocal<std::weak_ptr<Project> > sCurrentProject;

void Project::makeCurrent()
{
    sCurrentProject.set(shared_from_this());
}

std::shared_ptr<Project> Project::current()
{
    return sCurrentProject.get();
}

#include "Project.h"
#include "Indexer.h"
#include "FileManager.h"
#include "GRTags.h"
#include "Server.h"

Project::Project(unsigned flags, const Path &path)
    : mFlags(flags), mPath(path)
{
}

void Project::init(const Path &src)
{
    initSrcPath(src);
    assert(!isValid());

    if (mFlags & FileManagerEnabled) {
        fileManager.reset(new FileManager);
        fileManager->init(shared_from_this());
    }
    if (mFlags & IndexerEnabled) {
        unsigned flags = Indexer::None;
        const unsigned options = Server::instance()->options().options;
        if (!(options & Server::NoValidate))
            flags |= Indexer::Validate;
        if (options & Server::IgnorePrintfFixits)
            flags |= Indexer::IgnorePrintfFixits;
        indexer.reset(new Indexer(shared_from_this(), flags));
    }

    if (mFlags & GRTagsEnabled) {
        grtags.reset(new GRTags);
        grtags->init(shared_from_this());
    }
}

void Project::initSrcPath(const Path &src)
{
    mResolvedSrcRoot = mSrcRoot = src;
    mResolvedSrcRoot.resolve();
    if (!mSrcRoot.endsWith('/'))
        mSrcRoot.append('/');
    if (mResolvedSrcRoot.ByteArray::operator==(mSrcRoot)) {
        mResolvedSrcRoot.clear();
    } else if (!mResolvedSrcRoot.endsWith('/')) {
        mResolvedSrcRoot.append('/');
    }
}

bool Project::isValid() const
{
    return indexer || grtags;
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

void Project::unload()
{
    if (indexer) {
        indexer->abort();
        indexer.reset();
        fileManager.reset();
        // ### should unload grtags too probably
    }
}

bool Project::match(const Match &p) const
{
    if (!mSrcRoot.isEmpty() && p.match(mSrcRoot))
        return true;
    if (!mResolvedSrcRoot.isEmpty() && p.match(mResolvedSrcRoot))
        return true;
    if (!mPath.isEmpty() && p.match(mPath))
        return true;
    Path paths[] = { p.pattern(), p.pattern() };
    paths[1].resolve();
    const int count = paths[1] != paths[0] ? 2 : 1;
    for (int i=0; i<count; ++i) {
        const Path &path = paths[i];
        // error() << "comparing" << path << mSrcRoot << mResolvedSrcRoot << mPath;
        if (!mSrcRoot.isEmpty() && mSrcRoot.startsWith(path))
            return true;
        if (!mResolvedSrcRoot.isEmpty() && mResolvedSrcRoot.startsWith(path))
            return true;
        if (!mPath.isEmpty()) {
            if (mPath.startsWith(path)) {
                return true;
            } else if (mPath.isFile()) {
                const int lastSlash = mPath.lastIndexOf('/');
                if (lastSlash != -1 && path.size() >= lastSlash && !strncmp(mPath.constData(), path.constData(), lastSlash - 1))
                    return true;
            }
        }

    }
    return false;
}

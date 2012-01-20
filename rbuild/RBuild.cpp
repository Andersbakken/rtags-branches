#include "RBuild.h"
#include <RTags.h>
#include <QCoreApplication>
#include <QtAlgorithms>
#include <sstream>
#include <clang-c/Index.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>
#include "AtomicString.h"
#include "RBuild_p.h"
#include <FileDB.h>
#include <memory>

using namespace RTags;

static QElapsedTimer timer;

class CompileRunnable : public QRunnable
{
public:
    CompileRunnable(RBuild *rb, const Path &p, const QList<QByteArray> &a)
        : rbuild(rb), path(p), args(a)
    {
        setAutoDelete(true);
    }

    virtual void run()
    {
        const qint64 before = timer.elapsed();
        rbuild->compile(args, path);
        const qint64 elapsed = timer.elapsed();
        fprintf(stderr, "parsed %s, (%lld ms)\n", path.constData(), elapsed - before);
    }
private:
    RBuild *rbuild;
    const Path path;
    const QList<QByteArray> args;
};

static inline const Source* findSource(const QByteArray& filename, const QList<Source>& sources)
{
    foreach(const Source& source, sources) {
        if (source.path == filename)
            return &source;
    }
    return 0;
}

static inline bool isSourceDirty(const Source* source, QSet<Path>* dirty = 0)
{
    bool dirtySource = (source->path.lastModified() != source->lastModified);
    for (QHash<Path, quint64>::const_iterator it = source->dependencies.constBegin();
         it != source->dependencies.constEnd(); ++it) {
        if (dirty && dirty->contains(it.key())) {
            dirtySource = true;
        } else if (it.key().lastModified() != it.value()) {
            if (dirty)
                dirty->insert(it.key());
            dirtySource = true;
        }
    }
    return dirtySource;
}

RBuild::RBuild(unsigned flags, QObject *parent)
    : QObject(parent), mData(new RBuildPrivate)
{
    mData->flags = flags;
    mData->index = clang_createIndex(1, 0);
    if (const char *env = getenv("RTAGS_THREAD_COUNT")) {
        const int threads = atoi(env);
        if (threads > 0)
            mData->threadPool.setMaxThreadCount(threads);
    }
    RTags::systemIncludes(); // force creation before any threads are spawned
    connect(this, SIGNAL(compileFinished(bool)), this, SLOT(onCompileFinished()));
    timer.start();
}

RBuild::~RBuild()
{
    clang_disposeIndex(mData->index);
    delete mData;
}

void RBuild::setDBPath(const Path &path)
{
    mData->dbPath = path;
}

bool RBuild::buildDB(const QList<Path> &makefiles,
                     const QList<Path> &sources,
                     const Path &sourceDir)
{
    mData->sourceDir = sourceDir.isEmpty() ? Path(".") : sourceDir;
    mData->sourceDir.resolve();
    if (!mData->sourceDir.isDir()) {
        fprintf(stderr, "%s is not a directory\n", sourceDir.constData());
        return false;
    }
    if (!mData->sourceDir.endsWith('/'))
        mData->sourceDir.append('/');

    bool ok = false;
    foreach(const Path &makefile, makefiles) {
        if (makefile.isFile()) {
            MakefileParser *parser = new MakefileParser(this);
            connect(parser, SIGNAL(fileReady(const GccArguments&)),
                    this, SLOT(processFile(const GccArguments&)));
            connect(parser, SIGNAL(done()), this, SLOT(onMakefileDone()));
            mData->makefileParsers.insert(parser);
            parser->run(makefile);
            ok = true;
        } else {
            qWarning("%s is not a Makefile", makefile.constData());
            return false;
        }
    }
    foreach(const Path &source, sources) {
        if (!source.isFile()) {
            qWarning("%s is not a file", source.constData());
            return false;
        }
            
        const GccArguments::Language lang = GccArguments::guessLanguage(source);
        char buf[1024];
        const int written = snprintf(buf, 1024, "gcc %s%s %s",
                                     lang != GccArguments::LangUndefined ? "-x" : "",
                                     (lang != GccArguments::LangUndefined
                                      ? GccArguments::languageString(lang)
                                      : ""),
                                     source.constData());
        Q_ASSERT(written < 1024);

        GccArguments args;
        if (!args.parse(QByteArray(buf, written))) {
            qWarning("Couldn't parse %s", buf);
            return false;
        } else {
            processFile(args);
            ok = true;
        }
    }

    if (!ok) {
        qWarning("Nothing to do");
        return false;
    }
    QEventLoop loop;
    connect(this, SIGNAL(finishedCompiling()), &loop, SLOT(quit()));
    loop.exec();
    save();
    return true;
}

bool RBuild::updateDB()
{
    if (!openDB(Update))
        return false;
#ifdef QT_DEBUG
    const int beforeCount = mData->db->count();
#endif
    QList<Source> sources = mData->db->read<QList<Source> >("sources");
    mData->filesByName = mData->db->read<QHash<Path, unsigned> >("filesByName");

    QList<Source*> reparse;
    QSet<Path> dirty;
    const int sourceCount = sources.size();
    for (int i=0; i<sourceCount; ++i) {
        const Source &source = sources.at(i);
        if (isSourceDirty(&source, &dirty)) {
            dirty.insert(source.path);
            reparse.append(&sources[i]);
        } else {
            mData->sources.append(source);
        }
    }
    if (reparse.isEmpty()) {
        printf("Nothing has changed (%lld ms)\n", timer.elapsed());
        return true;
    }

    mData->db->invalidateEntries(dirty);
    mData->pendingJobs += reparse.size();
    foreach(Source *source, reparse) {
        mData->threadPool.start(new CompileRunnable(this, source->path, source->args));
    }
    QEventLoop loop;
    connect(this, SIGNAL(finishedCompiling()), &loop, SLOT(quit()));
    loop.exec();
    writeEntities();
    mData->db->write("sources", mData->sources);

    printf("Updated db %lld ms, %d threads\n",
           timer.elapsed(), mData->threadPool.maxThreadCount());
#ifdef QT_DEBUG
    printf("%d => %d entries\n", beforeCount, closeDB());
#else
    closeDB();
#endif

    return true;
}

void RBuild::save()
{
    const qint64 beforeWriting = timer.elapsed();

    // Q_ASSERT(filename.endsWith(".rtags.db"));
    if (!openDB(Create)) {
        return;
    }
    writeData();
    closeDB();
    const qint64 elapsed = timer.elapsed();
    fprintf(stderr, "All done. (total/saving %lld/%lld ms, %d threads\n",
            elapsed, elapsed - beforeWriting, mData->threadPool.maxThreadCount());
}

void RBuild::processFile(const GccArguments& arguments)
{
    switch (arguments.language()) {
    case GccArguments::LangHeader:
    case GccArguments::LangCPlusPlusHeader:
        pch(arguments);
        return;
    default:
        break;
    }
    const Path input = arguments.input();
    QSet<QByteArray> defines;
    foreach(const GccArguments &a, mData->files) {
        if (a.input() == input && arguments.language() == a.language()) {
            if (defines.isEmpty())
                defines = arguments.arguments("-D").toSet();
            if (a.arguments("-D").toSet() == defines) {
                return; // duplicate
            }
        }
    }
    mData->files.append(arguments);

    ++mData->pendingJobs;
    QList<QByteArray> clangArgs = arguments.clangArgs();
    {
        QMutexLocker lock(&mData->mutex);
        foreach(const QByteArray &inc, arguments.arguments("-include")) {
            const Path p = mData->pch.value(inc);
            if (!p.isFile()) {
                mData->pch.remove(inc);
            } else {
                clangArgs << "-include-pch" << p;
            }
        }
    }
    mData->threadPool.start(new CompileRunnable(this, arguments.input(), clangArgs));
}

static void recurseDir(QSet<Path> *allFiles, Path path, int rootDirLen)
{
#if defined(_DIRENT_HAVE_D_TYPE) || defined(Q_OS_BSD4) || defined(Q_OS_SYMBIAN)
    DIR *d = opendir(path.constData());
    char fileBuffer[PATH_MAX];
    if (d) {
        if (!path.endsWith('/'))
            path.append('/');
        dirent *p;
        while ((p=readdir(d))) {
            switch (p->d_type) {
            case DT_DIR:
                if (p->d_name[0] != '.') {
                    recurseDir(allFiles, path + QByteArray::fromRawData(p->d_name, strlen(p->d_name)), rootDirLen);
                }
                break;
            case DT_REG: {
                const int w = snprintf(fileBuffer, PATH_MAX, "%s%s", path.constData() + rootDirLen, p->d_name);
                if (w >= PATH_MAX) {
                    fprintf(stderr, "Path too long: %d, max is %d\n", w, PATH_MAX);
                } else {
                    allFiles->insert(Path(fileBuffer, w));
                }
                break; }
                // case DT_LNK: not following links
            }

        }
        closedir(d);
    }
#else
#warning "Can't use --source-dir on this platform"
#endif
}

void RBuild::writeData()
{
    mData->db->write("filesByName", mData->filesByName);
    writeEntities();
    mData->db->write("sources", mData->sources);
    QSet<Path> allFiles;

    if (!mData->sourceDir.isEmpty()) {
        Q_ASSERT(mData->sourceDir.endsWith('/'));
        if (mData->sourceDir.isDir()) {
            recurseDir(&allFiles, mData->sourceDir, mData->sourceDir.size());
        } else {
            fprintf(stderr, "%s is not a directory\n", mData->sourceDir.constData());
        }
        mData->db->write("sourceDir", mData->sourceDir);
    }
    mData->db->write("files", allFiles);
}

static inline void debugCursor(FILE* out, const CXCursor& cursor)
{
    CXFile file;
    unsigned int line, col, off;
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    clang_getInstantiationLocation(loc, &file, &line, &col, &off);
    CXString name = clang_getCursorDisplayName(cursor);
    CXString filename = clang_getFileName(file);
    CXString kind = clang_getCursorKindSpelling(clang_getCursorKind(cursor));
    fprintf(out, "cursor name %s, kind %s, loc %s:%u:%u\n",
            clang_getCString(name), clang_getCString(kind),
            clang_getCString(filename), line, col);
    clang_disposeString(name);
    clang_disposeString(kind);
    clang_disposeString(filename);
}

// #define COLLECTDEBUG

static inline bool isSource(const AtomicString &str)
{
    const QByteArray b = str.toByteArray();
    const int dot = b.lastIndexOf('.');
    const int len = b.size() - dot - 1;
    return (dot != -1 && len > 0 && Path::isSource(b.constData() + dot + 1, len));
}

struct InclusionUserData {
    InclusionUserData(QHash<Path, quint64> &deps,
                      const QList<QByteArray> &sysInclude,
                      bool ignoreSysIncludes)
        : dependencies(deps), systemIncludes(sysInclude), ignoreSystemIncludes(ignoreSysIncludes)
    {}
    QHash<Path, quint64> &dependencies;
    const QList<QByteArray> &systemIncludes;
    const bool ignoreSystemIncludes;
};

static inline bool isSystemInclude(const Path &header, const QList<QByteArray> &systemIncludes)
{
    foreach(const QByteArray &systemInclude, systemIncludes) {
        if (!strncmp(header.constData(), systemInclude.constData() + 2, systemInclude.size() - 2))
            return true;
    }
    return false;
}

static inline void getInclusions(CXFile includedFile,
                                 CXSourceLocation* /*inclusionStack*/,
                                 unsigned inclusionStackLen,
                                 CXClientData userData)
{
    if (inclusionStackLen) {
        InclusionUserData *u = reinterpret_cast<InclusionUserData*>(userData);
        CXString str = clang_getFileName(includedFile);
        const char *cstr = clang_getCString(str);
        if (!u->ignoreSystemIncludes || strncmp("/usr/", cstr, 5) != 0) {
            const Path p = Path::resolved(cstr);
            bool add = true;
            if (u->ignoreSystemIncludes) {
                foreach(const QByteArray &systemInclude, u->systemIncludes) {
                    if (!strncmp(p.constData(), systemInclude.constData() + 2, systemInclude.size() - 2)) {
                        add = false;
                        break;
                    }
                }
            }
            if (add) {
                u->dependencies[p] = p.lastModified();
            }
        }
        clang_disposeString(str);
    }
}

static inline bool diagnose(CXTranslationUnit unit)
{
    if (!unit)
        return false;
    const bool verbose = (getenv("VERBOSE") != 0);
    bool foundError = false;
    const unsigned int numDiags = clang_getNumDiagnostics(unit);
    for (unsigned int i = 0; i < numDiags; ++i) {
        CXDiagnostic diag = clang_getDiagnostic(unit, i);
        const bool error = clang_getDiagnosticSeverity(diag) >= CXDiagnostic_Error;
        foundError = foundError || error;
        if (verbose || error) {
            CXSourceLocation loc = clang_getDiagnosticLocation(diag);
            CXFile file;
            unsigned int line, col, off;

            clang_getInstantiationLocation(loc, &file, &line, &col, &off);
            CXString fn = clang_getFileName(file);
            CXString txt = clang_getDiagnosticSpelling(diag);
            const char* fnstr = clang_getCString(fn);

            // Suppress diagnostic messages that doesn't have a filename
            if (fnstr && (strcmp(fnstr, "") != 0))
                fprintf(stderr, "%s:%u:%u %s\n", fnstr, line, col, clang_getCString(txt));

            clang_disposeString(txt);
            clang_disposeString(fn);
        }
        clang_disposeDiagnostic(diag);
    }
    return !foundError;
}

static inline Location createLocation(const CXIdxLoc &l, QHash<Path, unsigned> &files)
{
    Location loc;
    CXFile f;
    clang_indexLoc_getFileLocation(l, 0, &f, &loc.line, &loc.column, 0);
    CXString str = clang_getFileName(f);
    const Path fileName = Path::resolved(clang_getCString(str));
    unsigned &file = files[fileName];
    if (!file)
        file = files.size();
    loc.file = file;
    clang_disposeString(str);
    return loc;
}

static inline Location createLocation(const CXCursor &c, QHash<Path, unsigned> &files)
{
    Location loc;
    CXFile f;
    CXSourceLocation sl = clang_getCursorLocation(c);
    clang_getInstantiationLocation(sl, &f, &loc.line, &loc.column, 0);
    CXString str = clang_getFileName(f);
    const Path fileName = Path::resolved(clang_getCString(str));
    unsigned &file = files[fileName];
    if (!file)
        file = files.size();
    loc.file = file;
    clang_disposeString(str);
    return loc;
}

struct UserData {
    RBuildPrivate *p;
    Path file;
};

void RBuild::indexDeclaration(CXClientData userData, const CXIdxDeclInfo *decl)
{
    RBuildPrivate *p = reinterpret_cast<UserData*>(userData)->p;
    if (p->flags & RBuild::DebugAllSymbols) {
        CXFile f;
        unsigned l, c;
        clang_indexLoc_getFileLocation(decl->loc, 0, &f, &l, &c, 0);
        Path p = Path::resolved(eatString(clang_getFileName(f)));
        QByteArray extra;
        if (decl->isDefinition)
            extra += "definition ";
        if (clang_isVirtualBase(decl->cursor))
            extra += "isVirtualBase ";
        CXCursor *overridden = 0;
        unsigned count = 0;
        clang_getOverriddenCursors(decl->cursor, &overridden, &count);
        for (unsigned i=0; i<count; ++i) {
            extra += cursorToString(overridden[i]) + ' ';
        }
        clang_disposeOverriddenCursors(overridden);
        printf("%s:%d:%d: %s %s %s\n",
               p.constData(), l, c, kindToString(decl->entityInfo->kind), decl->entityInfo->name,
               extra.constData());
    }

    QMutexLocker lock(&p->mutex); // ### is this the right place to lock?

    Entity &e = p->entities[decl->entityInfo->USR];
    Location loc = createLocation(decl->loc, p->filesByName);
    if (decl->isDefinition) {
        e.definition = loc;
    } else {
        e.declarations.insert(loc);
    }
    if (e.symbolName.isEmpty()) {
        CXString nm = clang_getCursorDisplayName(decl->cursor); // this one gives us args
        e.symbolName = clang_getCString(nm);
        clang_disposeString(nm);
        e.kind = decl->entityInfo->kind;
        if (e.kind == CXIdxEntity_CXXInstanceMethod || CXIdxEntity_CXXDestructor) {
            CXCursor *overridden = 0;
            unsigned count = 0;
            clang_getOverriddenCursors(decl->cursor, &overridden, &count);
            if (count == 1) { // we don't care about multiple inheritance, we could just take first I guess?
                const Location l = createLocation(overridden[0], p->filesByName);
                e.super = l;
                // qDebug() << "setting super for " << loc << "to" << l;
                const QByteArray usr = eatString(clang_getCursorUSR(overridden[0]));
                QHash<QByteArray, Entity>::iterator it = p->entities.find(usr);
                if (it != p->entities.end()) {
                    // qDebug() << "adding " << loc << "as sub for" << l;
                    it.value().subs.insert(loc);
                }
            }
            clang_disposeOverriddenCursors(overridden);
        }
        CXCursor parent = decl->cursor;
        forever {
            parent = clang_getCursorSemanticParent(parent);
            const CXCursorKind k = clang_getCursorKind(parent);
            if (clang_isInvalid(k))
                break;
            CXString str = clang_getCursorDisplayName(parent);
            const char *cstr = clang_getCString(str);
            if (!cstr || !strlen(cstr)) {
                clang_disposeString(str);
                break;
            }
            switch (k) {
            case CXCursor_StructDecl:
            case CXCursor_ClassDecl:
            case CXCursor_Namespace:
                e.parentNames.prepend(cstr);
                break;
            default:
                break;
            }
            clang_disposeString(str);
        }
    }
    // qDebug() << loc << name << kindToString(kind) << decl->entityInfo->templateKind
    //          << decl->entityInfo->USR;
    switch (e.kind) {
    case CXIdxEntity_CXXConstructor:
    case CXIdxEntity_CXXDestructor: {
        if (e.kind == CXIdxEntity_CXXDestructor) {
            Q_ASSERT(e.symbolName.startsWith('~'));
            ++loc.column; // this is just for renameSymbol purposes
        }
        
        Q_ASSERT(decl->semanticContainer);
        CXString usr = clang_getCursorUSR(decl->semanticContainer->cursor);
        p->entities[clang_getCString(usr)].extraDeclarations.insert(loc);
        break; }
    default:
        break;
    }
    
}

void RBuild::indexReference(CXClientData userData, const CXIdxEntityRefInfo *ref)
{
    RBuildPrivate *p = reinterpret_cast<UserData*>(userData)->p;
    if (p->flags & RBuild::DebugAllSymbols) {
        CXFile f, f2;
        unsigned l, c, l2, c2;
        clang_indexLoc_getFileLocation(ref->loc, 0, &f, &l, &c, 0);
        CXSourceLocation loc = clang_getCursorLocation(ref->referencedEntity->cursor);
        clang_getInstantiationLocation(loc, &f2, &l2, &c2, 0);
        const Path p = Path::resolved(eatString(clang_getFileName(f)));
        const Path p2 = Path::resolved(eatString(clang_getFileName(f2)));
        printf("%s:%d:%d: ref of %s %s %s:%d:%d\n",
               p.constData(), l, c,
               kindToString(ref->referencedEntity->kind),
               ref->referencedEntity->name,
               p2.constData(), l2, c2);
    }
    
    QMutexLocker lock(&p->mutex);
    const Location loc = createLocation(ref->loc, p->filesByName);
    CXCursor spec = clang_getSpecializedCursorTemplate(ref->referencedEntity->cursor);
    PendingReference &r = p->pendingReferences[loc];
    if (r.usr.isEmpty()) {
        r.usr = ref->referencedEntity->USR;
        if (!clang_isInvalid(clang_getCursorKind(spec)))
            r.specialized = eatString(clang_getCursorUSR(spec));
    }
}

void RBuild::diagnostic(CXClientData userdata, CXDiagnosticSet set, void *)
{
    const int count = clang_getNumDiagnosticsInSet(set);
    const static bool verbose = getenv("VERBOSE");
    for (int i=0; i<count; ++i) {
        CXDiagnostic diagnostic = clang_getDiagnosticInSet(set, i);
        if (verbose || clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error) {

            fprintf(stderr, "%s: %s\n",
                    reinterpret_cast<UserData*>(userdata)->file.constData(),
                    eatString(clang_formatDiagnostic(diagnostic, 0xff)).constData());
        }
        clang_disposeDiagnostic(diagnostic);
    }
}

bool RBuild::compile(const QList<QByteArray> &args, const Path &file, const Path &output)
{
    bool ret = true;
    if ((mData->flags & DontClang) != DontClang) {
        const QList<QByteArray> systemIncludes = RTags::systemIncludes();
        QVarLengthArray<const char *, 64> clangArgs(args.size()
                                                    + mData->extraArgs.size()
                                                    + systemIncludes.size());
        int argCount = 0;
        foreach(const QByteArray& arg, args) {
            clangArgs[argCount++] = arg.constData();
        }
        foreach(const QByteArray &systemInclude, systemIncludes) {
            clangArgs[argCount++] = systemInclude.constData();
        }
        foreach(const QByteArray &extraArg, mData->extraArgs) {
            clangArgs[argCount++] = extraArg.constData();
        }

        IndexerCallbacks cb;
        memset(&cb, 0, sizeof(IndexerCallbacks));
        cb.diagnostic = diagnostic;
        if ((mData->flags & DontIndex) != DontIndex) {
            cb.indexDeclaration = indexDeclaration;
            cb.indexEntityReference = indexReference;
        }

        CXIndexAction action = clang_IndexAction_create(mData->index);
        CXTranslationUnit unit = 0;
        const bool verbose = (getenv("VERBOSE") != 0);

        if (verbose) {
            fprintf(stderr, "clang ");
            for (int i=0; i<argCount; ++i) {
                fprintf(stderr, "%s ", clangArgs[i]);
            }
            fprintf(stderr, "%s\n", file.constData());
        }

        UserData userData = { mData, file };
        clang_indexSourceFile(action, &userData, &cb, sizeof(IndexerCallbacks),
                              CXIndexOpt_IndexFunctionLocalSymbols, file.constData(),
                              clangArgs.constData(), argCount,
                              0, 0, &unit, clang_defaultEditingTranslationUnitOptions());
        // ### do we need incomplete for pch?

        if (!unit) {
            qWarning() << "Unable to parse unit for" << file;
            for (int i=0; i<argCount; ++i) {
                fprintf(stderr, "%s", clangArgs[i]);
                fprintf(stderr, "%c", i + 1 < argCount ? ' ' : '\n');
            }

            ret = false;
        } else {
            Source src = { file, args, file.lastModified(), QHash<Path, quint64>() };
            InclusionUserData u(src.dependencies, systemIncludes,
                                !(mData->flags & EnableSystemHeaderDependencies));
            clang_getInclusions(unit, getInclusions, &u);
            {
                QMutexLocker lock(&mData->mutex); // ### is this the right place to lock?
                mData->sources.append(src);
            }
            // qDebug() << input << mData->dependencies.last().dependencies.keys();
            if (!output.isEmpty()) {
                const int c = clang_saveTranslationUnit(unit, output.constData(), clang_defaultSaveOptions(unit));
                if (c) {
                    qWarning("Couldn't save translation unit: %d", c);
                    ret = false;
                }
            }
            clang_disposeTranslationUnit(unit);
        }
    }
    emit compileFinished(ret);
    return ret;
}

void RBuild::onCompileFinished()
{
    if (!--mData->pendingJobs && mData->makefileParsers.isEmpty())
        emit finishedCompiling();
}

bool RBuild::openDB(Mode mode)
{
    Q_ASSERT(!mData->db);
    mData->db = Database::create(mData->dbPath,
                                 mode == Update ? Database::ReadWrite : Database::WriteOnly);
    return mData->db->isOpened();
}

int RBuild::closeDB()
{
    int ret = -1;
    if (mData->db) {
        ret = mData->db->close();
        delete mData->db;
        mData->db = 0;
    }
    return ret;
}
void RBuild::writeEntities()
{
    for (QHash<Location, PendingReference>::const_iterator it = mData->pendingReferences.begin();
         it != mData->pendingReferences.end(); ++it) {
        const PendingReference &ref = it.value();
        QHash<QByteArray, Entity>::iterator i = mData->entities.find(ref.usr);
        if (i == mData->entities.end())
            i = mData->entities.find(ref.specialized);
        if (i == mData->entities.end()) {
            qDebug() << "Can't find this reference anywhere" << ref.usr << ref.specialized << it.key();
        } else {
            i.value().references.insert(it.key());
        }
    }

    foreach(const Entity &entity, mData->entities) {
        mData->db->writeEntity(entity);
    }
}
// ### it's only legal to call these before any compilation has started
void RBuild::addDefines(const QList<QByteArray> &defines)
{
    foreach(const QByteArray &define, defines)
        mData->extraArgs += ("-D" + define);
}

void RBuild::addIncludePaths(const QList<Path> &paths)
{
    foreach(const Path &path, paths)
        mData->extraArgs += ("-I" + path);
}
void RBuild::onMakefileDone()
{
    MakefileParser *parser = qobject_cast<MakefileParser*>(sender());
    mData->makefileParsers.remove(parser);
    parser->deleteLater();
    if (mData->makefileParsers.isEmpty() && !mData->pendingJobs)
        emit finishedCompiling();
}

bool RBuild::pch(const GccArguments &pch)
{
    if (mData->flags & DisablePCH)
        return false;
    const qint64 before = timer.elapsed();
#warning Make output absolute?
    QByteArray output = pch.output();
    int idx = output.indexOf(".gch");
    if (idx != -1) {
        output.remove(idx, output.size() - idx);
    } else {
        return false;
    }
    {
        QMutexLocker lock(&mData->mutex);
        if (mData->pch.contains(output))
            return true;
        const QByteArray existingPch = mData->db->read<QByteArray>(Database::General, "pch-" + output);
        if (!existingPch.isEmpty()) {
            mData->pch[output] = existingPch;
            const Source* pchSource = findSource(existingPch, mData->sources);
            Q_ASSERT(pchSource);
            if (!isSourceDirty(pchSource))
                return true;
        } else {
            mData->pch[output] = QByteArray();
        }
    }

    QList<QByteArray> args = pch.clangArgs();
    args << "-emit-pch";
    char tmp[128];
    strncpy(tmp, "/tmp/rtagspch.XXXXXX", 127);
    int id = mkstemp(tmp);
    if (id == -1) {
        mData->pch.remove(output);
        return false;
    }
    close(id);
    ++mData->pendingJobs;
    const bool ok = compile(args, pch.input(), tmp);
    QMutexLocker lock(&mData->mutex);
    if (ok) {
        mData->pch[output] = tmp;
        const qint64 elapsed = timer.elapsed() - before;
        printf("pch %s %s (%lld ms)\n", pch.input().constData(), output.constData(), elapsed);
        mData->db->write(Database::General, "pch-" + output, QByteArray(tmp));
    } else {
        mData->pch.remove(output);
        mData->db->remove(Database::General, "pch-" + output);
    }
    return ok;
}

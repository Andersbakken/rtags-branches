#include "IndexerJob.h"
#include "Timer.h"
#include "SHA256.h"
#include "MemoryMonitor.h"
#include "Server.h"
#include "EventLoop.h"

static inline List<Path> extractPchFiles(const List<ByteArray> &args)
{
    List<Path> out;
    bool nextIsPch = false;
    const int count = args.size();
    for (int i=0; i<count; ++i) {
        const ByteArray &arg = args.at(i);
        if (arg.isEmpty())
            continue;

        if (nextIsPch) {
            nextIsPch = false;
            out.append(arg);
        } else if (arg == "-include-pch") {
            nextIsPch = true;
        }
    }
    return out;
}

IndexerJob::IndexerJob(Indexer *indexer, int id, unsigned flags,
                       const Path &input, const List<ByteArray> &arguments,
                       const Set<uint32_t> &dirty)

    : mId(id), mFlags(flags), mIsPch(false), mDoneFullUSRScan(false), mIn(input),
      mFileId(Location::insertFile(input)), mArgs(arguments), mIndexer(indexer),
      mDirty(dirty), mPchHeaders(extractPchFiles(arguments)), mUnit(0)
{
    setAutoDelete(false);
    assert(mDirty.isEmpty() || flags & NeedsDirty);
}

static inline uint32_t fileId(CXFile file)
{
    return Location(file, 0).fileId();
}

void IndexerJob::inclusionVisitor(CXFile includedFile,
                                  CXSourceLocation *includeStack,
                                  unsigned includeLen,
                                  CXClientData userData)
{
    IndexerJob *job = static_cast<IndexerJob*>(userData);
    if (job->isAborted())
        return;
    const Location l(includedFile, 0);

    const Path path = l.path();
    job->mSymbolNames[path].insert(l);
    const char *fn = path.fileName();
    job->mSymbolNames[ByteArray(fn, strlen(fn))].insert(l);

    const uint32_t fileId = l.fileId();
    if (!includeLen) {
        job->mDependencies[fileId].insert(fileId);
        if (job->mIsPch)
            job->mPchDependencies.insert(fileId);
    } else if (!path.isSystem()) {
        for (unsigned i=0; i<includeLen; ++i) {
            CXFile originatingFile;
            clang_getSpellingLocation(includeStack[i], &originatingFile, 0, 0, 0);
            Location loc(originatingFile, 0);
            const uint32_t f = loc.fileId();
            if (f)
                job->mDependencies[fileId].insert(f);
        }
        if (job->mIsPch) {
            job->mPchDependencies.insert(fileId);
        }
    }
}

static inline bool mayHaveTemplates(CXCursorKind kind)
{
    switch (kind) {
    case CXCursor_ClassTemplate:
    case CXCursor_Constructor:
    case CXCursor_Destructor:
        return true;
    default:
        return false;
    }
}

static inline void addToSymbolNames(const ByteArray &arg, bool hasTemplates, const Location &location, SymbolNameMap &symbolNames)
{
    symbolNames[arg].insert(location);
    if (hasTemplates) {
        ByteArray copy = arg;
        const int lt = arg.indexOf('<');
        if (lt == -1)
            return;
        const int gt = arg.indexOf('>', lt + 1);
        if (gt == -1)
            return;
        if (gt + 1 == arg.size()) {
            copy.truncate(lt);
        } else {
            copy.remove(lt, gt - lt + 1);
        }

        symbolNames[copy].insert(location);
    }
}

ByteArray IndexerJob::addNamePermutations(const CXCursor &cursor, const Location &location, bool addToDB)
{
    ByteArray ret, qname, qparam, qnoparam;

    CXCursor cur = cursor, null = clang_getNullCursor();
    CXCursorKind kind;
    bool first = true;
    for (;;) {
        if (clang_equalCursors(cur, null))
            break;
        kind = clang_getCursorKind(cur);
        if (!first) {
            bool ok = false;
            switch (kind) {
            case CXCursor_Namespace:
            case CXCursor_ClassDecl:
            case CXCursor_ClassTemplate:
            case CXCursor_StructDecl:
            case CXCursor_CXXMethod:
            case CXCursor_Constructor:
            case CXCursor_FunctionDecl:
                ok = true;
                break;
            default:
                break;
            }
            if (!ok)
                break;
        }

        CXStringScope displayName(clang_getCursorDisplayName(cur));
        const char *name = clang_getCString(displayName.string);
        if (!name || !strlen(name)) {
            break;
        }
        qname = ByteArray(name);
        if (ret.isEmpty()) {
            ret = qname;
            if (!addToDB)
                return ret;
        }
        if (qparam.isEmpty()) {
            qparam = qname;
            const int sp = qparam.indexOf('(');
            if (sp != -1)
                qnoparam = qparam.left(sp);
        } else {
            qparam.prepend(qname + "::");
            if (!qnoparam.isEmpty())
                qnoparam.prepend(qname + "::");
        }

        assert(!qparam.isEmpty());
        const bool hasTemplates = mayHaveTemplates(kind) && qnoparam.contains('<');
        addToSymbolNames(qparam, hasTemplates, location, mSymbolNames);
        if (!qnoparam.isEmpty()) {
            assert(!qnoparam.isEmpty());
            addToSymbolNames(qnoparam, hasTemplates, location, mSymbolNames);
        }

        if (first) {
            first = false;
            switch (kind) {
            case CXCursor_Namespace:
            case CXCursor_ClassDecl:
            case CXCursor_StructDecl:
            case CXCursor_CXXMethod:
            case CXCursor_Constructor:
            case CXCursor_FunctionDecl:
            case CXCursor_VarDecl:
            case CXCursor_ParmDecl:
            case CXCursor_FieldDecl:
            case CXCursor_ClassTemplate:
                break;
            default:
                // these don't need the scope
                return ret;
            }
        }

        cur = clang_getCursorSemanticParent(cur);
    }
    return ret;
}

Location IndexerJob::createLocation(const CXCursor &cursor, bool *blocked)
{
    CXSourceLocation location = clang_getCursorLocation(cursor);
    Location ret;
    if (!clang_equalLocations(location, clang_getNullLocation())) {
        CXFile file;
        unsigned start;
        clang_getSpellingLocation(location, &file, 0, 0, &start);
        if (file) {
            ret = Location(file, start);
            const uint32_t fileId = ret.fileId();
            if (blocked) {
                PathState &state = mPaths[fileId];
                if (state == Unset) {
                    state = mIndexer->visitFile(fileId, mIn) ? Index : DontIndex;
                }
                if (state != Index) {
                    *blocked = true;
                    return Location();
                }
                *blocked = false;
            } else {
                PathState &state = mPaths[fileId];
                if (state == Unset || state == DontIndex) {
                    state = Reference;
                }
            }
        }
    }
    return ret;
}

static inline bool isInteresting(CXCursorKind kind)
{
    if (clang_isInvalid(kind))
        return false;
    switch (kind) {
    case CXCursor_CXXThisExpr:
    case CXCursor_CXXTypeidExpr:
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXNullPtrLiteralExpr:
    case CXCursor_CXXNewExpr: // ### Are these right?
    case CXCursor_CXXDeleteExpr:
    case CXCursor_CompoundAssignOperator: // ### Are these right?
    case CXCursor_CompoundStmt:
    case CXCursor_ParenExpr:
    case CXCursor_StringLiteral:
    case CXCursor_IntegerLiteral:
    case CXCursor_InitListExpr:
    case CXCursor_BreakStmt:
    case CXCursor_DefaultStmt:
    case CXCursor_BinaryOperator:
    case CXCursor_CaseStmt:
    case CXCursor_ConditionalOperator:
    case CXCursor_CStyleCastExpr:
    case CXCursor_ForStmt:
    case CXCursor_WhileStmt:
    case CXCursor_DoStmt:
    case CXCursor_IfStmt:
    case CXCursor_CXXBoolLiteralExpr:
    case CXCursor_CharacterLiteral:
    case CXCursor_UnaryOperator:
    case CXCursor_ReturnStmt:
    case CXCursor_CXXAccessSpecifier:
    case CXCursor_CXXConstCastExpr:
    case CXCursor_CXXDynamicCastExpr:
    case CXCursor_CXXReinterpretCastExpr:
        return false;
    default:
        break;
    }
    return true;
}

CXChildVisitResult IndexerJob::indexVisitor(CXCursor cursor,
                                            CXCursor /*parent*/,
                                            CXClientData client_data)
{
    IndexerJob *job = static_cast<IndexerJob*>(client_data);
    if (job->isAborted())
        return CXChildVisit_Break;

    if (testLog(Debug))
        debug() << "indexVisitor " << cursor << " " << clang_getCursorReferenced(cursor);
    const CXCursorKind kind = clang_getCursorKind(cursor);
    if (!isInteresting(kind))
        return CXChildVisit_Recurse;

    bool blocked = false;
    const Location loc = job->createLocation(cursor, &blocked);
    if (blocked) {
        switch (kind) {
        case CXCursor_FunctionDecl:
        case CXCursor_CXXMethod:
        case CXCursor_Destructor:
        case CXCursor_Constructor:
            job->mHeaderMap[clang_getCursorUSR(cursor)] = cursor;
            break;
        case CXCursor_ClassDecl:
        case CXCursor_StructDecl:
        case CXCursor_Namespace:
        case CXCursor_ClassTemplate:
            return CXChildVisit_Recurse;
        default:
            break;
        }
        return CXChildVisit_Continue;
    } else if (loc.isNull()) {
        return CXChildVisit_Recurse;
    }
    CXCursor ref = clang_getCursorReferenced(cursor);
    const CXCursorKind refKind = clang_getCursorKind(ref);
    // the kind won't change even if the reference is looked up from elsewhere

    /* CXCursor_CallExpr is the right thing to use for invocations of constructors */
    if (kind == CXCursor_CallExpr && (refKind == CXCursor_CXXMethod || refKind == CXCursor_FunctionDecl))
        return CXChildVisit_Recurse;

    const Cursor c = { cursor, loc, kind };
    Location refLoc;
    if (!clang_equalCursors(cursor, ref)) {
        refLoc = job->createLocation(ref, 0);
    } else {
        if (!clang_isCursorDefinition(cursor)) {
            ref = clang_getCursorDefinition(cursor);
            if (!clang_equalCursors(clang_getNullCursor(), ref)) {
                assert(!clang_equalCursors(cursor, ref));
                refLoc = job->createLocation(ref, 0);
                if (testLog(Debug)) {
                    debug() << "Looked up definition for ref " << ref << " " << cursor;
                }
            }
        }

        if (refLoc.isNull()) {
            const Cursor r = job->findByUSR(cursor, kind, loc);
            if (r.kind != CXCursor_FirstInvalid)
                return job->processCursor(c, r);
        }
    }
    const Cursor r = { ref, refLoc, refKind };
    return job->processCursor(c, r);
}

CXChildVisitResult IndexerJob::processCursor(const Cursor &cursor, const Cursor &ref)
{
    if (testLog(VerboseDebug))
        verboseDebug() << "processCursor " << cursor.cursor << " " << ref.cursor;

    if (cursor.kind == CXCursor_InclusionDirective) {
        CXFile includedFile = clang_getIncludedFile(cursor.cursor);
        if (includedFile) {
            const Location refLoc(includedFile, 0);
            if (!refLoc.isNull()) {
                {
                    ByteArray include = "#include ";
                    const Path path = refLoc.path();
                    mSymbolNames[(include + path)].insert(cursor.location);
                    mSymbolNames[(include + path.fileName())].insert(cursor.location);
                }
                CXSourceRange range = clang_getCursorExtent(cursor.cursor);
                unsigned int end;
                clang_getSpellingLocation(clang_getRangeEnd(range), 0, 0, 0, &end);
                unsigned tokenCount = 0;
                CXToken *tokens = 0;
                clang_tokenize(mUnit, range, &tokens, &tokenCount);
                CursorInfo &info = mSymbols[cursor.location];
                info.target = refLoc;
                info.kind = cursor.kind;
                info.isDefinition = false;
                info.symbolLength = end - cursor.location.offset();
                for (unsigned i=0; i<tokenCount; ++i) {
                    if (clang_getTokenKind(tokens[i]) == CXToken_Literal) {
                        CXStringScope scope(clang_getTokenSpelling(mUnit, tokens[i]));
                        info.symbolName = "#include ";
                        info.symbolName += clang_getCString(scope.string);
                        mSymbolNames[info.symbolName].insert(cursor.location);
                        break;
                    }
                }
                if (tokens) {
                    clang_disposeTokens(mUnit, tokens, tokenCount);
                }
            }
        }
        return CXChildVisit_Recurse;
    }

    CursorInfo &info = mSymbols[cursor.location];
    if (!info.symbolLength) {
        if (mIsPch) {
            const ByteArray usr = Rdm::eatString(clang_getCursorUSR(cursor.cursor));
            if (!usr.isEmpty()) {
                mPchUSRMap[usr] = cursor.location;
            }
        }
        info.isDefinition = clang_isCursorDefinition(cursor.cursor);
        info.kind = cursor.kind;
        const bool isReference = Rdm::isReference(info.kind);

        if (!isReference && !info.isDefinition) {
            CXSourceRange range = clang_getCursorExtent(cursor.cursor);
            unsigned end;
            clang_getSpellingLocation(clang_getRangeEnd(range), 0, 0, 0, &end);
            info.symbolLength = end - cursor.location.offset();
        } else {
            CXStringScope name = clang_getCursorSpelling(cursor.cursor);
            const char *cstr = clang_getCString(name.string);
            info.symbolLength = cstr ? strlen(cstr) : 0;
        }
        if (!info.symbolLength) {
            mSymbols.remove(cursor.location);
            return CXChildVisit_Recurse;
        }
        info.symbolName = addNamePermutations(cursor.cursor, cursor.location, !isReference);
    } else if (info.kind == CXCursor_Constructor && cursor.kind == CXCursor_TypeRef) {
        return CXChildVisit_Recurse;
    }

    if (!clang_isInvalid(ref.kind) && !ref.location.isNull() && ref.location != cursor.location) {
        info.target = ref.location;
        Rdm::ReferenceType referenceType = Rdm::NormalReference;
        if (ref.kind == cursor.kind) {
            switch (ref.kind) {
            case CXCursor_Constructor:
            case CXCursor_Destructor:
            case CXCursor_CXXMethod:
                referenceType = Rdm::MemberFunction;
                break;
            case CXCursor_FunctionDecl:
                referenceType = Rdm::GlobalFunction;
                break;
            default:
                break;
            }
        }
        mReferences[cursor.location] = std::pair<Location, Rdm::ReferenceType>(ref.location, referenceType);
    }
    return CXChildVisit_Recurse;
}

static ByteArray pchFileName(const ByteArray &header)
{
    return Server::pchDir() + SHA256::hash(header.constData());
}

struct Scope {
    ~Scope()
    {
        cleanup();
    }
    void cleanup()
    {
        headerMap.clear();
        if (unit) {
            clang_disposeTranslationUnit(unit);
            unit = 0;
        }
        if (index) {
            clang_disposeIndex(index);
            index = 0;
        }
    }

    Map<Str, CXCursor> &headerMap;
    CXTranslationUnit &unit;
    CXIndex &index;
};

void IndexerJob::run()
{
    execute();
    EventLoop::instance()->postEvent(mIndexer, new IndexerJobFinishedEvent(this));
}
void IndexerJob::execute()
{
    Timer timer;
    // while (!Rdm::waitForMemory(10000)) {
    //     error("%s Waiting for rdm to shrink", mIn.constData());
    // }
    if (!mPchHeaders.isEmpty())
        mPchUSRMap = mIndexer->pchUSRMap(mPchHeaders);

    List<const char*> clangArgs(mArgs.size(), 0);
    ByteArray clangLine = "clang ";
    bool nextIsPch = false, nextIsX = false;
    ByteArray pchName;

    List<Path> pchFiles;
    int idx = 0;
    const int count = mArgs.size();
    for (int i=0; i<count; ++i) {
        const ByteArray &arg = mArgs.at(i);
        if (arg.isEmpty())
            continue;

        if (nextIsPch) {
            nextIsPch = false;
            pchFiles.append(pchFileName(arg));
            clangArgs[idx++] = pchFiles.back().constData();
            clangLine += pchFiles.back().constData();
            clangLine += " ";
            continue;
        }

        if (nextIsX) {
            nextIsX = false;
            mIsPch = (arg == "c++-header" || arg == "c-header");
        }
        clangArgs[idx++] = arg.constData();
        clangLine += arg;
        clangLine += " ";
        if (arg == "-include-pch") {
            nextIsPch = true;
        } else if (arg == "-x") {
            nextIsX = true;
        }
    }
    if (mIsPch) {
        pchName = pchFileName(mIn);
    }
    clangLine += mIn;

    if (isAborted()) {
        return;
    }
    CXIndex index = clang_createIndex(1, 0);
    mUnit = clang_parseTranslationUnit(index, mIn.constData(),
                                       clangArgs.data(), idx, 0, 0,
                                       CXTranslationUnit_Incomplete | CXTranslationUnit_DetailedPreprocessingRecord);
    Scope scope = { mHeaderMap, mUnit, index };
    const time_t timeStamp = time(0);
    // fprintf(stdout, "%s => %d\n", clangLine.nullTerminated(), (mUnit != 0));

    warning() << "loading unit " << clangLine << " " << (mUnit != 0);
    if (isAborted()) {
        return;
    }

    mDependencies[mFileId].insert(mFileId);
    bool compileError = false;
    if (!mUnit) {
        compileError = true;
        error() << "got 0 unit for " << clangLine;
        mIndexer->addDependencies(mDependencies);
        FileInformation fi;
        fi.compileArgs = mArgs;
        fi.lastTouched = timeStamp;

        Rdm::writeFileInformation(mFileId, mArgs, timeStamp);
    } else {
        Map<Location, std::pair<int, ByteArray> > fixIts;
        Map<uint32_t, List<ByteArray> > visited;
        const unsigned diagnosticCount = clang_getNumDiagnostics(mUnit);
        // bool hasCompilationErrors = false;
        for (unsigned i=0; i<diagnosticCount; ++i) {
            CXDiagnostic diagnostic = clang_getDiagnostic(mUnit, i);
            int logLevel = INT_MAX;
            const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
            switch (severity) {
            case CXDiagnostic_Fatal:
            case CXDiagnostic_Error:
            case CXDiagnostic_Warning:
                logLevel = CompilationError;
                // hasCompilationErrors = true;
                break;
            case CXDiagnostic_Note:
                logLevel = Debug;
                break;
            case CXDiagnostic_Ignored:
                logLevel = INT_MAX;
                break;
            }

            if (testLog(logLevel)) {
                CXSourceLocation loc = clang_getDiagnosticLocation(diagnostic);
                const ByteArray string = Rdm::eatString(clang_formatDiagnostic(diagnostic,
                                                                               CXDiagnostic_DisplaySourceLocation|
                                                                               CXDiagnostic_DisplayColumn|
                                                                               CXDiagnostic_DisplaySourceRanges|
                                                                               CXDiagnostic_DisplayOption|
                                                                               CXDiagnostic_DisplayCategoryId|
                                                                               CXDiagnostic_DisplayCategoryName));
                CXFile file;
                clang_getSpellingLocation(loc, &file, 0, 0, 0);
                if (file)
                    visited[Location(file, 0).fileId()].append(string);

                log(logLevel, "%s", string.constData());
            }

            const unsigned fixItCount = clang_getDiagnosticNumFixIts(diagnostic);
            for (unsigned f=0; f<fixItCount; ++f) {
                CXSourceRange range;
                CXString string = clang_getDiagnosticFixIt(diagnostic, f, &range);
                const Location start(clang_getRangeStart(range));
                unsigned endOffset = 0;
                clang_getSpellingLocation(clang_getRangeEnd(range), 0, 0, 0, &endOffset);

                error("Fixit (%d/%d) for %s: [%s] %s-%d", f + 1, fixItCount, mIn.constData(),
                      clang_getCString(string), start.key().constData(), endOffset);
                // ### can there be more than one fixit starting at the same location? Probably not.
                fixIts[start] = std::pair<int, ByteArray>(endOffset - start.offset(), Rdm::eatString(string));
            }

            clang_disposeDiagnostic(diagnostic);
        }
        // if (!hasCompilationErrors) {
        //     log(CompilationError, "%s parsed", mIn.constData()); ### this is annoying for rdm, we need something better
        // }

        clang_getInclusions(mUnit, inclusionVisitor, this);

        clang_visitChildren(clang_getTranslationUnitCursor(mUnit), indexVisitor, this);
        if (mIsPch) {
            assert(!pchName.isEmpty());
            if (clang_saveTranslationUnit(mUnit, pchName.constData(), clang_defaultSaveOptions(mUnit)) != CXSaveError_None) {
                error() << "Couldn't save pch file" << mIn << pchName;
            } else {
                mIndexer->setPchUSRMap(mIn, mPchUSRMap);
            }
        }
        const int pchHeaderCount = mPchHeaders.size();
        for (int i=0; i<pchHeaderCount; ++i) {
            const Path &pchHeader = mPchHeaders.at(i);
            const Set<uint32_t> pchDeps = mIndexer->pchDependencies(pchHeader);
            for (Set<uint32_t>::const_iterator it = pchDeps.begin(); it != pchDeps.end(); ++it) {
                mDependencies[*it].insert(mFileId);
            }
        }
        scope.cleanup();

        if (!isAborted()) {
            Set<uint32_t> referenced;
            Set<uint32_t> indexed;
            for (Map<uint32_t, PathState>::const_iterator it = mPaths.begin(); it != mPaths.end(); ++it) {
                switch (it->second) {
                case Reference:
                    if (mFlags & NeedsDirty)
                        referenced.insert(it->first);
                    break;
                case Index:
                    visited[it->first] = List<ByteArray>();
                    if (mFlags & NeedsDirty)
                        indexed.insert(it->first);
                    break;
                case DontIndex:
                    break;
                case Unset:
                    assert(0);
                    break;
                }
            }
            if (mFlags & NeedsDirty) {
                // Map<uint32_t, Set<uint32_t> > dirty = mIndexer->dependencies(fileIds, dirtyAndIndexed);
                // Log log(Error);
                // log << "About to dirty for " << mIn;
                // for (Map<uint32_t, Set<uint32_t> >::iterator it = dirty.begin(); it != dirty.end(); ++it) {
                //     // log << " " << Location::path(it->first);
                //     it->second.unite(dirtyAndIndexed);
                // }

                debug() << "about to dirty for " << mIn << " " << indexed << " " << referenced << " " << mDirty;
                Rdm::dirtySymbols(indexed, referenced, mDirty);
                Rdm::dirtySymbolNames(indexed);
            }
            mIndexer->addDependencies(mDependencies);
            assert(mDependencies[mFileId].contains(mFileId));

            mIndexer->setDiagnostics(visited, fixIts);
            Rdm::writeSymbols(mSymbols, mReferences, mFileId);
            Rdm::writeSymbolNames(mSymbolNames);
            Rdm::writeFileInformation(mFileId, mArgs, timeStamp);
            if (mIsPch)
                mIndexer->setPchDependencies(mIn, mPchDependencies);
        }
    }

    char buf[1024];
    const char *strings[] = { "", " (pch)", " (dirty)", " (pch, dirty)" };
    enum {
        None = 0x0,
        Pch = 0x1,
        Dirty = 0x2
    };
    const int w = snprintf(buf, sizeof(buf), "Visited %s (%s) in %sms. (%d syms, %d refs, %d deps, %d symNames)%s",
                           mIn.constData(), compileError ? "error" : "success", ByteArray::number(timer.elapsed()).constData(),
                           mSymbols.size(), mReferences.size(), mDependencies.size(), mSymbolNames.size(),
                           strings[(mPchHeaders.isEmpty() ? None : Pch) | (mFlags & NeedsDirty ? Dirty : None)]);
    mMessage = ByteArray(buf, w);
    if (testLog(Warning)) {
        warning() << "We're using " << double(MemoryMonitor::usage()) / double(1024 * 1024) << " MB of memory " << timer.elapsed() << "ms";
    }
}

CXChildVisitResult isInlineVisitor(CXCursor, CXCursor, CXClientData u)
{
    *reinterpret_cast<bool*>(u) = true;
    return CXChildVisit_Break;
}

static inline bool isInline(const CXCursor &cursor)
{
    switch (clang_getCursorKind(clang_getCursorLexicalParent(cursor))) {
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_StructDecl:
        return true;
    default:
        return false;
    }
}

IndexerJob::Cursor IndexerJob::findByUSR(const CXCursor &cursor, CXCursorKind kind, const Location &loc)
{
    bool ok = false;
    switch (kind) {
    case CXCursor_FunctionDecl:
        ok = (clang_isCursorDefinition(cursor) && loc.fileId() == mFileId);
        break;
    case CXCursor_CXXMethod:
    case CXCursor_Destructor:
    case CXCursor_Constructor:
        ok = (clang_isCursorDefinition(cursor) && loc.fileId() == mFileId && !isInline(cursor));
        break;
    default:
        break;
    }
    if (!ok) {
        const Cursor ret = { clang_getNullCursor(), Location(), CXCursor_FirstInvalid };
        return ret;
    }

    const Str usr(clang_getCursorUSR(cursor));
    if (!usr.length()) {
        const Cursor ret = { clang_getNullCursor(), Location(), CXCursor_FirstInvalid };
        return ret;
    }

    const ByteArray key(usr.data(), usr.length());
    Location refLoc = mPchUSRMap.value(key);
    if (!refLoc.isNull()) {
        const Cursor ret = { cursor, refLoc, clang_getCursorKind(cursor) };
        // ### even if this isn't the right CXCursor it's good enough for our needs
        return ret;
    }

    Map<Str, CXCursor>::const_iterator it = mHeaderMap.find(usr);
    if (it != mHeaderMap.end()) {
        const CXCursor ref = it->second;
        const Cursor ret = { ref, createLocation(ref, 0), clang_getCursorKind(ref) };
        assert(!clang_equalCursors(ref, cursor)); // ### why is this happening?
        return ret;
    }
    const Cursor ret = { clang_getNullCursor(), Location(), CXCursor_FirstInvalid };
    return ret;
}

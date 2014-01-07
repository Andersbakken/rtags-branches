/* This file is part of RTags.

   RTags is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   RTags is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with RTags.  If not, see <http://www.gnu.org/licenses/>. */

#include "RTagsClang.h"
#include "Server.h"
#include <rct/StopWatch.h>
#include "Cpp.h"
#include "Project.h"
#include <iostream>

#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS

#ifdef HAVE_BACKTRACE
#undef HAVE_BACKTRACE
#endif

#include <clang/Driver/Util.h>
#include <llvm/Config/config.h>
#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/TextDiagnosticBuffer.h>
#if CLANG_VERSION_MINOR < 4
#include <clang/Driver/ArgList.h>
using clang::driver::InputArgList;
#else
#include <llvm/Option/ArgList.h>
using llvm::opt::InputArgList;
#endif
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/ToolChain.h>
#include <clang/Frontend/Utils.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/PreprocessingRecord.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Host.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Lex/HeaderSearchOptions.h>

namespace RTags {
String eatString(CXString str)
{
    const String ret(clang_getCString(str));
    clang_disposeString(str);
    return ret;
}

String cursorToString(CXCursor cursor, unsigned flags)
{
    const CXCursorKind kind = clang_getCursorKind(cursor);
    String ret;
    ret.reserve(256);
    ret += eatString(clang_getCursorKindSpelling(kind));
    if (clang_isInvalid(kind))
        return ret;

    switch (RTags::cursorType(kind)) {
    case Reference:
        ret += " r";
        break;
    case Cursor:
        ret += " c";
        break;
    case Other:
        ret += " o";
        break;
    case Include:
        ret += " i";
        break;
    }

    const String name = eatString(clang_getCursorDisplayName(cursor));
    const String other = eatString(clang_getCursorSpelling(cursor));
    if (!name.isEmpty())
        ret += " " + name;
    if (other != name && !other.isEmpty())
        ret += " " + other;

    if (clang_isCursorDefinition(cursor))
        ret += " def";

    if (flags & IncludeUSR)
        ret += " " + eatString(clang_getCursorUSR(cursor));

    CXString file;
    unsigned line, col;
    CXSourceLocation location = clang_getCursorLocation(cursor);
    clang_getPresumedLocation(location, &file, &line, &col);

    const Str fileName(file);
    if (fileName.data() && *fileName.data()) {
        ret += ' ';
        ret += fileName.data();
        ret += ':';
        ret += String::number(line);
        ret += ':';
        ret += String::number(col);

        // if (flags & IncludeRange) {
        //     ret += " (";
        //     CXSourceRange range = clang_getCursorExtent(cursor);
        //     unsigned start, end;
        //     clang_getSpellingLocation(clang_getRangeStart(range), 0, 0, 0, &start);
        //     clang_getSpellingLocation(clang_getRangeEnd(range), 0, 0, 0, &end);
        //     ret += String::number(start);
        //     ret += '-';
        //     ret += String::number(end);
        //     ret += ')';
        // }

        // if (presumedLine != line || presumedCol != col)
        //     ret += String::snprintf<32>("presumed: %d:%d", presumedLine, presumedCol);
        // if (instantiationLoc != off)
        //     ret += String::snprintf<32>("instantiation: %d", instantiationLoc);
        // if (expansionLoc != off)
        //     ret += String::snprintf<32>("expansion: %d", expansionLoc);

    }
    return ret;
}

SymbolMap::const_iterator findCursorInfo(const SymbolMap &map, const Location &location, const String &context)
{
    if (context.isEmpty()) {
        SymbolMap::const_iterator it = map.lower_bound(location);
        if (it != map.end() && it->first == location) {
            return it;
        } else if (it != map.begin()) {
            --it;
            if (it->first.fileId() == location.fileId() && location.line() == it->first.line()) {
                const int off = location.column() - it->first.column();
                if (it->second.symbolLength > off)
                    return it;
            }
        }
        return map.end();
    }

    SymbolMap::const_iterator f = map.lower_bound(location);
    if (f != map.begin() && (f == map.end() || f->first != location))
        --f;
    SymbolMap::const_iterator b = f;

    enum { Search = 32 };
    for (int j=0; j<Search; ++j) {
        if (f != map.end()) {
            if (location.fileId() != f->first.fileId()) {
                if (b == map.begin())
                    break;
                f = map.end();
            } else if (f->second.symbolName.contains(context)) {
                // error() << "found it forward" << j;
                return f;
            } else {
                ++f;
            }
        }

        if (b != map.begin()) {
            --b;
            if (location.fileId() != b->first.fileId()) {
                if (f == map.end())
                    break;
                b = map.begin();
            } else if (b->second.symbolName.contains(context)) {
                // error() << "found it backward" << j;
                return b;
            }
        }
    }
    return map.end();
}

void parseTranslationUnit(const Path &sourceFile, const List<String> &args,
                          const List<String> &defaultArguments,
                          CXTranslationUnit &unit, CXIndex index,
                          CXUnsavedFile *unsaved, int unsavedCount,
                          unsigned int translationUnitFlags,
                          String *clangLine)

{
    if (clangLine)
        *clangLine = "clang ";

    int idx = 0;
    List<const char*> clangArgs(args.size() + defaultArguments.size(), 0);

    const List<String> *lists[] = { &args, &defaultArguments };
    for (int i=0; i<2; ++i) {
        const int count = lists[i]->size();
        for (int j=0; j<count; ++j) {
            String arg = lists[i]->at(j);
            if (arg.isEmpty())
                continue;

            clangArgs[idx++] = lists[i]->at(j).constData();
            if (clangLine) {
                arg.replace("\"", "\\\"");
                *clangLine += arg;
                *clangLine += ' ';
            }
        }
    }

    if (clangLine)
        *clangLine += sourceFile;

    StopWatch sw;
    unit = clang_parseTranslationUnit(index, sourceFile.constData(),
                                      clangArgs.data(), idx, unsaved, unsavedCount,
                                      translationUnitFlags);
    // error() << sourceFile << sw.elapsed();
}

void reparseTranslationUnit(CXTranslationUnit &unit, CXUnsavedFile *unsaved, int unsavedCount)
{
    assert(unit);
    if (clang_reparseTranslationUnit(unit, 0, unsaved, clang_defaultReparseOptions(unit)) != 0) {
        clang_disposeTranslationUnit(unit);
        unit = 0;
    }
}

class StringOStream : public llvm::raw_ostream
{
public:
    StringOStream(String *str)
        : llvm::raw_ostream(true), mString(str) // non-buffered
    {}
    virtual void write_impl(const char *data, size_t size)
    {
        mString->append(data, size);
    }
    virtual uint64_t current_pos() const
    {
        return mString->size();
    }
private:
    String *mString;
};

static inline void processArgs(clang::HeaderSearchOptions &headerSearchOptions,
                               const clang::driver::ArgStringList &args)
{
    enum Type {
        Pending,
        SystemInclude
    } type = Pending;
    for (const char *arg : args) {
        switch (type) {
        case Pending:
            if (!strcmp(arg, "-internal-isystem") || !strcmp(arg, "-internal-externc-isystem")) {
                type = SystemInclude;
            } else if (!strncmp("-l", arg, 2)) {

            } else {
                error() << "Unknown arg type" << arg;
            }
            break;
        case SystemInclude: {
            const Path path = Path::resolved(arg);
            // error() << "Adding system include" << path;
            headerSearchOptions.AddPath(clang::StringRef(path.constData(), path.size()), clang::frontend::System,
#if CLANG_VERSION_MINOR < 3
                                        false,
#endif
                                        false, false);
            type = Pending;
            break; }
        }
        // printf("got clang cxx arg %s\n", arg);
    }
}

static String toString(const Source::Define &def)
{
    String ret;
    ret += "#define ";
    ret += def.define;
    if (!def.value.isEmpty()) {
        ret += ' ';
        ret += def.value;
    }
    return ret;
}

bool compile(const Path& output, const Source &source, const String& preprocessed)
{
    (void)output;
    (void)source;
    (void)preprocessed;
#if CLANG_VERSION_MINOR < 4
    StopWatch sw;

    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    LLVMInitializeAllAsmParsers();

    clang::CompilerInstance compilerInstance;
    compilerInstance.createFileManager();
    assert(compilerInstance.hasFileManager());

#if CLANG_VERSION_MINOR >= 3
    compilerInstance.createDiagnostics();
#else
    compilerInstance.createDiagnostics(0, 0);
#endif
    assert(compilerInstance.hasDiagnostics());

    clang::FileManager &fm = compilerInstance.getFileManager();
    compilerInstance.createSourceManager(fm);
    assert(compilerInstance.hasSourceManager());
    clang::SourceManager &sm = compilerInstance.getSourceManager();

    const Path& sourceFile = source.sourceFile();
    clang::StringRef src(sourceFile.constData(), sourceFile.size());
    clang::StringRef pre(preprocessed.constData(), preprocessed.size());
    llvm::MemoryBuffer* premem = llvm::MemoryBuffer::getMemBuffer(pre, src);
    const clang::FileEntry* preent = fm.getVirtualFile(src, preprocessed.size(), time(0));
    sm.overrideFileContents(preent, premem);

    clang::TargetOptions &targetOptions = compilerInstance.getTargetOpts();
    targetOptions.Triple = llvm::sys::getDefaultTargetTriple();
    clang::DiagnosticsEngine& diags = compilerInstance.getDiagnostics();
    compilerInstance.setTarget(clang::TargetInfo::CreateTargetInfo(diags,
#if CLANG_VERSION_MINOR < 3
                                                                   targetOptions
#else
                                                                   &targetOptions
#endif
                                   ));
    clang::LangOptions &langOpts = compilerInstance.getLangOpts();

    clang::InputKind ik;

    switch (source.language) {
    case Source::CPlusPlus11:
    case Source::CPlusPlus:
        ik = clang::IK_PreprocessedCXX;
        break;
    default:
        ik = clang::IK_PreprocessedC;
        break;
    }
    compilerInstance.getInvocation().setLangDefaults(langOpts, ik);
#if CLANG_VERSION_MINOR >= 3
    if (source.language == Source::CPlusPlus11)
        langOpts.CPlusPlus11 = true;
#endif

    clang::FrontendInputFile input(src, ik);

    clang::FrontendOptions& feopts = compilerInstance.getFrontendOpts();
    feopts.ProgramAction = clang::frontend::EmitObj;
    feopts.Inputs.push_back(input);

    compilerInstance.setTarget(clang::TargetInfo::CreateTargetInfo(compilerInstance.getDiagnostics(),
#if CLANG_VERSION_MINOR < 3
                                                                   compilerInstance.getTargetOpts()
#else
                                                                   &compilerInstance.getTargetOpts()
#endif
                                                                   ));
    compilerInstance.getTarget().setForcedLangOptions(langOpts);

    // ### ???
    compilerInstance.createPreprocessor();
    clang::InitializePreprocessor(compilerInstance.getPreprocessor(),
                                  compilerInstance.getPreprocessorOpts(),
                                  compilerInstance.getHeaderSearchOpts(),
                                  feopts);

    //compilerInstance.getDiagnosticClient().BeginSourceFile(compilerInstance.getLangOpts(), &compilerInstance.getPreprocessor());

    compilerInstance.clearOutputFiles(false);
    clang::StringRef out(output.constData(), output.size());
    //llvm::raw_fd_ostream* ostrm = compilerInstance.createDefaultOutputFile(true, out);
    llvm::raw_fd_ostream* ostrm = compilerInstance.createOutputFile(out, true, true, "", "o");
    ostrm->SetUnbuffered();

    clang::EmitObjAction emitact;
    emitact.BeginSourceFile(compilerInstance, input);
    emitact.Execute();
    emitact.EndSourceFile();

#if CLANG_VERSION_MINOR >= 3
    LLVMShutdown();
#endif

#endif
    return true;
}

static inline uint32_t visitFile(const char *path,
                                 const std::shared_ptr<Cpp> &cpp,
                                 const std::shared_ptr<Project> &project,
                                 Map<Path, uint32_t> &blockedCache,
                                 bool *blocked)
{
    assert(!strstr(path, ".."));
    assert(blocked);
    *blocked = false;
    Path resolved = path;
    bool changed;
    if (!resolved.resolve(Path::RealPath, Path(), &changed))
        return 0;
    if (!project)
        return Location::insertFile(path);

    {
        const Map<Path, uint32_t>::const_iterator it = cpp->visited.find(resolved);
        if (it != cpp->visited.end()) {
            return it->second;
        }
    }

    {
        const Map<Path, uint32_t>::const_iterator it = blockedCache.find(resolved);
        if (it != blockedCache.end()) {
            return it->second;
        }
    }

    // error() << "trying here" << path;
    assert(!path.isEmpty());
    const uint32_t fileId = Location::insertFile(path);
    if (!project->visitFile(fileId, 0)) {
        blockedCache[path] = fileId;
        *blocked = true;
    } else {
        cpp->visited[path] = fileId;
        // ### We should find something nicer than this
        if (cpp->visited.size() % 10 == 0) {
            usleep(50000);
        }
    }
    return fileId;
}

class Compiler : public clang::CompilerInstance
{
public:
    ~Compiler()
    {
        resetAndLeakSourceManager();
    }
};

std::shared_ptr<Cpp> preprocess(const Source &source, const std::shared_ptr<Project> &project)
{
    Map<Path, uint32_t> blockedCache;
    StopWatch sw;
    Compiler compilerInstance;
    compilerInstance.createFileManager();
    assert(compilerInstance.hasFileManager());
#if CLANG_VERSION_MINOR >= 3
    compilerInstance.createDiagnostics();
#else
    compilerInstance.createDiagnostics(0, 0);
#endif
    assert(compilerInstance.hasDiagnostics());
    clang::DiagnosticsEngine& diags = compilerInstance.getDiagnostics();
    clang::FileManager &fm = compilerInstance.getFileManager();
    clang::SourceManager sm(diags, fm, true);
    compilerInstance.setSourceManager(&sm);
    assert(compilerInstance.hasSourceManager());
    const Path sourceFile = source.sourceFile();
    uint64_t now = Rct::currentTimeMs();
    const clang::FileEntry *file = 0;
    for (int i=0; i<4; ++i) {
        file = fm.getFile(sourceFile.constData(), true); // pass openfile?
        if (file)
            break;
        usleep(100);
    }
    if (!file) {
        return std::shared_ptr<Cpp>();
    }
    sm.createMainFileID(file);
    clang::TargetOptions &targetOptions = compilerInstance.getTargetOpts();
    //targetOptions.Triple = LLVM_HOST_TRIPLE;
    targetOptions.Triple = llvm::sys::getDefaultTargetTriple();
    clang::TextDiagnosticBuffer diagnosticsClient;
    diags.setClient(&diagnosticsClient, false);
    compilerInstance.setTarget(clang::TargetInfo::CreateTargetInfo(diags,
#if CLANG_VERSION_MINOR < 3
                                                                   targetOptions
#else
                                                                   &targetOptions
#endif
                                   ));
    clang::LangOptions &langOpts = compilerInstance.getLangOpts();
    switch (source.language) {
    case Source::CPlusPlus11:
#if CLANG_VERSION_MINOR >= 3
        langOpts.CPlusPlus11 = true;
#endif
        langOpts.CPlusPlus = true;
        break;
    case Source::CPlusPlus:
        langOpts.CPlusPlus = true;
        break;
    default:
        break;
    }
    List<Path> includePaths = source.includePaths;
    List<Source::Define> defines = source.defines;

    const Server::Options &options = Server::instance()->options();
    clang::HeaderSearchOptions &headerSearchOptions = compilerInstance.getHeaderSearchOpts();
    Path sysRoot = source.sysRoot();
    if (sysRoot != "/") {
        assert(sysRoot.endsWith('/'));
        sysRoot.chop(1);
    }

    headerSearchOptions.Sysroot = sysRoot;
    {
        clang::driver::Driver driver("clang", llvm::sys::getDefaultTargetTriple(), "a.out",
#if CLANG_VERSION_MINOR < 3
                                     true, // is_production, no idea what it means
#endif
                                     diags);
        std::vector<String> copies; // not cool
        std::vector<const char*> args;
        const Path compiler = source.compiler();
        args.push_back(compiler.constData());
        args.push_back("-c");
        args.push_back(sourceFile.constData());
        for (const Path &path : source.includePaths)
            copies.push_back("-I" + path);
        for (const Path &path : options.includePaths)
            copies.push_back("-I" + path);
        for (const Source::Define &def : source.defines)
            copies.push_back(def.toString());
        for (const Source::Define &def : options.defines)
            copies.push_back(def.toString());
        for (const String &str : copies)
            args.push_back(str.constData());

        std::unique_ptr<clang::driver::Compilation> compilation(driver.BuildCompilation(llvm::ArrayRef<const char*>(&args[0], args.size())));
        const clang::driver::ToolChain& toolChain = compilation->getDefaultToolChain();
        const InputArgList inputArgs(&args[0], &args[0] + args.size());
        clang::driver::ArgStringList outputArgs;
        toolChain.AddClangCXXStdlibIncludeArgs(inputArgs, outputArgs);
        processArgs(headerSearchOptions, outputArgs);
        toolChain.AddClangSystemIncludeArgs(inputArgs, outputArgs);
        processArgs(headerSearchOptions, outputArgs);
        toolChain.AddCXXStdlibLibArgs(inputArgs, outputArgs);
        processArgs(headerSearchOptions, outputArgs);
    }

    for (List<Path>::const_iterator it = source.includePaths.begin(); it != source.includePaths.end(); ++it) {
        // error() << "Adding -I" << *it;
        headerSearchOptions.AddPath(clang::StringRef(it->constData(), it->size()),
                                    clang::frontend::Angled,
#if CLANG_VERSION_MINOR < 3
                                    false,
#endif
                                    false, true);
    }
    for (List<Path>::const_iterator it = options.includePaths.begin(); it != options.includePaths.end(); ++it) {
        // error() << "Adding -I" << *it;
        headerSearchOptions.AddPath(clang::StringRef(it->constData(), it->size()),
                                    clang::frontend::System,
#if CLANG_VERSION_MINOR < 3
                                    false,
#endif
                                    false, true);
    }

    compilerInstance.createPreprocessor();
    std::string predefines = compilerInstance.getPreprocessor().getPredefines();
    for (List<Source::Define>::const_iterator it = source.defines.begin(); it != source.defines.end(); ++it) {
        predefines += toString(*it);
        predefines += '\n';
        // error() << "Got define" << it->define << it->value;
    }
    for (List<Source::Define>::const_iterator it = options.defines.begin(); it != options.defines.end(); ++it) {
        predefines += toString(*it);
        predefines += '\n';
        // error() << "Got define" << it->define << it->value;
    }
    predefines += "#define __STRICT_ANSI__\n";

    // error() << "predefines" << compilerInstance.getPreprocessor().getPredefines();
    compilerInstance.getPreprocessor().setPredefines(predefines);
    clang::PreprocessorOutputOptions preprocessorOptions;
    preprocessorOptions.ShowCPP = 1;

    compilerInstance.getDiagnosticClient().BeginSourceFile(compilerInstance.getLangOpts(), &compilerInstance.getPreprocessor());

    std::shared_ptr<Cpp> cpp(new Cpp);
    StringOStream out(&cpp->preprocessed);
    cpp->time = now;
    clang::Preprocessor &preprocessor = compilerInstance.getPreprocessor();
    preprocessor.createPreprocessingRecord(
#if CLANG_VERSION_MINOR < 3
    true
#endif
        );
    clang::DoPrintPreprocessedInput(preprocessor, &out, preprocessorOptions);
    struct {
        const Cpp::Diagnostic::Type type;
        const clang::TextDiagnosticBuffer::const_iterator begin, end;
    } const diagnostics[] = {
        { Cpp::Diagnostic::Note, diagnosticsClient.note_begin(), diagnosticsClient.note_end() },
        { Cpp::Diagnostic::Warning, diagnosticsClient.warn_begin(), diagnosticsClient.warn_end() },
        { Cpp::Diagnostic::Error, diagnosticsClient.err_begin(), diagnosticsClient.err_end() }
    };
    for (size_t i=0; i<sizeof(diagnostics) / sizeof(diagnostics[0]); ++i) {
        for (auto it = diagnostics[i].begin; it != diagnostics[i].end; ++it) {
            const clang::PresumedLoc presumedLocation = sm.getPresumedLoc(it->first);
            Path path = presumedLocation.getFilename();
            bool changed;
            if (!path.resolve(Path::RealPath, Path(), &changed))
                continue;
            bool blocked;
            const uint32_t fileId = visitFile(path, cpp, project, blockedCache, &blocked);
            if (blocked)
                continue;

            cpp->diagnostics.append(Cpp::Diagnostic());
            Cpp::Diagnostic &d = cpp->diagnostics.last();
            d.location = Location(fileId, presumedLocation.getLine(), presumedLocation.getColumn());
            d.type = diagnostics[i].type;
            d.text = it->second;
        }
    }
    clang::PreprocessingRecord *record = preprocessor.getPreprocessingRecord();
    assert(record);

    // We need this extra map because macros might be defined, then undef'ed and
    // then redefined. In that case we need to know the "active" macro
    // definition so the set in cpp->macroNames doesn't help us.

    StopWatch watch;
    Hash<String, Location> macroLocations;
    for (clang::PreprocessingRecord::iterator it = record->begin(); it != record->end(); ++it) {
        const clang::PreprocessedEntity *entity = *it;
        switch (entity->getKind()) {
        case clang::PreprocessedEntity::MacroDefinitionKind: {
            const clang::MacroDefinition *def = static_cast<const clang::MacroDefinition*>(entity);
            const clang::SourceRange range = def->getSourceRange();
            if (!range.isValid())
                break;

            const clang::SourceLocation begin = range.getBegin();
            const clang::PresumedLoc presumedLocation = sm.getPresumedLoc(begin);
            const char *path = presumedLocation.getFilename();
            if (!strcmp(path, "<built-in>"))
                break;

            Path resolved = path;
            if (!resolved.resolve()) {
                // printf("Resolved didn't %s/%s\n", path, resolved.constData());
                break;
            }

            bool blocked;
            const uint32_t fileId = visitFile(resolved, cpp, project, blockedCache, &blocked);
            const clang::IdentifierInfo *name = def->getName();
            const String macroName(name->getNameStart(), name->getLength());
            const Location loc(fileId, presumedLocation.getLine(), presumedLocation.getColumn());
            if (!blocked) {
                CursorInfo &cursor = cpp->macroCursors[loc];
                cursor.symbolName = macroName;
                cursor.symbolLength = cursor.symbolName.size();
                cursor.kind = CXCursor_MacroDefinition;
                cpp->macroNames[cursor.symbolName].insert(loc);
            }
            macroLocations[macroName] = loc;
            // error() << "Got definition" << String(name->getNameStart(), name->getLength()) << loc;
            break; }
        case clang::PreprocessedEntity::MacroExpansionKind: {
            const clang::MacroExpansion *exp = static_cast<const clang::MacroExpansion*>(entity);
            if (exp->isBuiltinMacro())
                break;

            const clang::SourceRange range = exp->getSourceRange();
            if (!range.isValid())
                break;

            const clang::PresumedLoc presumedLocation = sm.getPresumedLoc(range.getBegin());
            const char *path = presumedLocation.getFilename();
            if (!strcmp(path, "<built-in>"))
                break;

            Path resolved = path;
            if (!resolved.resolve()) {
                // printf("Resolved didn't %s/%s\n", path, resolved.constData());
                break;
            }

            bool blocked;
            const uint32_t fileId = visitFile(resolved, cpp, project, blockedCache, &blocked);
            if (blocked)
                break;

            const clang::IdentifierInfo *name = exp->getName();
            const String macroName(name->getNameStart(), name->getLength());
            const Location defLocation = macroLocations.value(macroName);
            if (defLocation.isNull()) {
                // error() << "Bailing on" << macroName << exp->getDefinition();
                break;
            }
            const Location loc(fileId, presumedLocation.getLine(), presumedLocation.getColumn());
            CursorInfo &cursor = cpp->macroCursors[loc];
            cursor.symbolName = macroName;
            cursor.symbolLength = cursor.symbolName.size();
            cursor.kind = CXCursor_MacroExpansion;
            CursorInfo &def = cpp->macroCursors[defLocation];
            // ### do I have to fill in def here? Do I need to in ClangIndexer?
            def.references.insert(loc);
            cursor.targets.insert(defLocation);
            // error() << "Got expansion" << String(name->getNameStart(), name->getLength()) << loc;
            break; }
        default:
            break;
        }
    }

    // FILE *f = fopen("/tmp/preprocess.cpp", "w");
    // fwrite(sourceFile.constData(), 1, sourceFile.size(), f);

    // fwrite(str.constData(), 1, str.size(), f);
    // fclose(f);
    warning() << "preprocessing" << sourceFile << "took" << sw.elapsed() << watch.elapsed() << "ms";

    return cpp;
}

static CXChildVisitResult findFirstChildVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    *reinterpret_cast<CXCursor*>(data) = cursor;
    return CXChildVisit_Break;
}

CXCursor findFirstChild(CXCursor parent)
{
    CXCursor ret = clang_getNullCursor();
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findFirstChildVisitor, &ret);
    return ret;
}

struct FindChildVisitor
{
    CXCursorKind kind;
    String name;
    CXCursor cursor;
};

static CXChildVisitResult findChildVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    FindChildVisitor *u = reinterpret_cast<FindChildVisitor*>(data);
    if (u->name.isEmpty()) {
        if (clang_getCursorKind(cursor) == u->kind) {
            u->cursor = cursor;
            return CXChildVisit_Break;
        }
    } else {
        CXStringScope str = clang_getCursorSpelling(cursor);
        if (str.data() && u->name == str.data()) {
            u->cursor = cursor;
            return CXChildVisit_Break;
        }
    }
    return CXChildVisit_Continue;
}

CXCursor findChild(CXCursor parent, CXCursorKind kind)
{
    FindChildVisitor u = { kind, String(), clang_getNullCursor() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findChildVisitor, &u);
    return u.cursor;
}

CXCursor findChild(CXCursor parent, const String &name)
{
    FindChildVisitor u = { CXCursor_FirstInvalid, name, clang_getNullCursor() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findChildVisitor, &u);
    return u.cursor;
}

struct ChildrenVisitor
{
    const Filter &in;
    const Filter &out;
    List<CXCursor> children;
};

static CXChildVisitResult childrenVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    ChildrenVisitor *u = reinterpret_cast<ChildrenVisitor*>(data);
    if ((u->out.isNull() || !u->out.match(cursor)) && (u->in.isNull() || u->in.match(cursor))) {
        u->children.append(cursor);
    }
    return CXChildVisit_Continue;
}

List<CXCursor> children(CXCursor parent, const Filter &in, const Filter &out)
{
    ChildrenVisitor userData = { in, out, List<CXCursor>() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, childrenVisitor, &userData);
    return userData.children;
}

struct FindChainVisitor
{
    const List<CXCursorKind> &kinds;
    List<CXCursor> ret;
};

static CXChildVisitResult findChainVisitor(CXCursor cursor, CXCursor, CXClientData data)
{
    FindChainVisitor *u = reinterpret_cast<FindChainVisitor*>(data);
    if (clang_getCursorKind(cursor) == u->kinds.at(u->ret.size())) {
        u->ret.append(cursor);
        if (u->ret.size() < u->kinds.size())
            return CXChildVisit_Recurse;

        return CXChildVisit_Break;
    }
    return CXChildVisit_Break;
}

List<CXCursor> findChain(CXCursor parent, const List<CXCursorKind> &kinds)
{
    assert(!kinds.isEmpty());
    FindChainVisitor userData = { kinds, List<CXCursor>() };
    if (!clang_isInvalid(clang_getCursorKind(parent)))
        clang_visitChildren(parent, findChainVisitor, &userData);
    if (userData.ret.size() != kinds.size()) {
        userData.ret.clear();
    }
    return userData.ret;
}
}

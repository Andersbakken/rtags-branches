#include "RClient.h"
#include "CreateOutputMessage.h"
#include "ProjectMessage.h"
#include "EventLoop.h"

class RCCommand
{
public:
    virtual ~RCCommand() {}
    virtual void exec(RClient *rc, Client *client) = 0;
    virtual ByteArray description() const = 0;
};

class QueryCommand : public RCCommand
{
public:
    QueryCommand(QueryMessage::Type t, const ByteArray &q)
        : type(t), query(q), extraQueryFlags(0)
    {}

    const QueryMessage::Type type;
    const ByteArray query;
    unsigned extraQueryFlags;

    virtual void exec(RClient *rc, Client *client)
    {
        QueryMessage msg(type);
        msg.init(rc->argc(), rc->argv());
        msg.setQuery(query);
        msg.setFlags(extraQueryFlags | rc->queryFlags());
        msg.setMax(rc->max());
        msg.setUnsavedFiles(rc->unsavedFiles());
        msg.setPathFilters(rc->pathFilters().toList());
        client->message(&msg);
    }

    virtual ByteArray description() const
    {
        return ("QueryMessage " + ByteArray::number(type) + " " + query);
    }
};

class RdmLogCommand : public RCCommand
{
public:
    enum { Default = -2 };
    RdmLogCommand(int level)
        : mLevel(level)
    {
    }
    virtual void exec(RClient *rc, Client *client)
    {
        CreateOutputMessage msg(mLevel == Default ? rc->logLevel() : mLevel);
        msg.init(rc->argc(), rc->argv());
        client->message(&msg);
    }
    virtual ByteArray description() const
    {
        return "RdmLogCommand";
    }
    const int mLevel;
};

class ProjectCommand : public RCCommand
{
public:
    ProjectCommand(ProjectMessage::Type t, const Path &p, const List<ByteArray> &a = List<ByteArray>())
        : type(t), path(p), args(a)
    {}
    const ProjectMessage::Type type;
    const Path path;
    const List<ByteArray> args;
    virtual void exec(RClient *rc, Client *client)
    {
        switch (path.type()) {
        case Path::Directory:
            if (type == ProjectMessage::MakefileType) {
                error() << path << "is not a makefile";
                return;
            }
            break;
        case Path::File:
            if (type != ProjectMessage::MakefileType) {
                error() << path << "is not a directory";
                return;
            }
            break;
        default:
            error() << "Invalid path" << path;
        }
        ProjectMessage msg(type, path);
        msg.init(rc->argc(), rc->argv());
        msg.setFlags(rc->makefileFlags());
        msg.setExtraFlags(rc->extraCompilerFlags());
        msg.setArguments(args);
        client->message(&msg);
    }
    virtual ByteArray description() const
    {
        return ("ProjectMessage " + path);
    }
};

RClient::RClient()
    : mQueryFlags(0), mClientFlags(0), mMakefileFlags(0), mMax(-1),
      mLogLevel(0), mTimeout(0), mArgc(0), mArgv(0)
{
}

RClient::~RClient()
{
    cleanupLogging();
}

QueryCommand *RClient::addQuery(QueryMessage::Type t, const ByteArray &query)
{
    QueryCommand *cmd = new QueryCommand(t, query);
    mCommands.append(cmd);
    return cmd;
}
void RClient::addLog(int level)
{
    mCommands.append(new RdmLogCommand(level));
}

void RClient::addMakeFile(const Path &path, const List<ByteArray> &args)
{
    mCommands.append(new ProjectCommand(ProjectMessage::MakefileType, path, args));
}

void RClient::addGRTag(const Path &path)
{
    mCommands.append(new ProjectCommand(ProjectMessage::GRTagsType, path));
}

void RClient::addSmartProject(const Path &path)
{
    mCommands.append(new ProjectCommand(ProjectMessage::SmartType, path));
}

static void timeout(int timerId, void *userData)
{
    EventLoop *loop = static_cast<EventLoop*>(userData);
    loop->removeTimer(timerId);
    loop->exit();
}

void RClient::exec()
{
    EventLoop loop;

    Client client(mSocketFile, mClientFlags, mRdmArgs);

    const int commandCount = mCommands.size();
    for (int i=0; i<commandCount; ++i) {
        RCCommand *cmd = mCommands.at(i);
        debug() << "running command " << cmd->description();
        const int timeoutId = (mTimeout ? loop.addTimer(mTimeout, ::timeout, &loop) : -1);
        cmd->exec(this, &client);
        if (timeoutId != -1)
            loop.removeTimer(timeoutId);
        delete cmd;
    }
    mCommands.clear();
}

enum {
    None = 0,
    Verbose,
    Help,
    SkipParen,
    AutostartRdm,
    FollowLocation,
    Makefile,
    Max,
    ReferenceName,
    ReferenceLocation,
    ReverseSort,
    ListSymbols,
    FindSymbols,
    CursorInfo,
    // UnsavedFile,
    LogFile,
    NoContext,
    Status,
    RdmLog,
    LineNumbers,
    PathFilter,
    FilterSystemHeaders,
    Includepath,
    Define,
    CompilerFlag,
    IsIndexed,
    QuitRdm,
    RestartRdm,
    AllReferences,
    ElispList,
    Clear,
    Fixits,
    Errors,
    Reindex,
    Diagnostics,
    Project,
    DeleteProject,
    WaitForIndexing,
    FindFile,
    MatchRegexp,
    AbsolutePath,
    GRTag,
    SocketFile,
    AlwaysMake,
    DumpFile,
    Timeout,
    SniffMake,
    SmartProject,
    FindVirtuals,
    HasFileManager,
    PreprocessFile
};

struct Option {
    const int option;
    const char *longOpt;
    const char shortOpt;
    const int argument;
    const char *description;
};

struct Option opts[] = {
    { None, 0, 0, 0, "Options:" },
    { Verbose, "verbose", 'v', no_argument, "Be more verbose." },
    { Help, "help", 'h', no_argument, "Display this help." },

    { None, 0, 0, 0, "" },
    { None, 0, 0, 0, "Rdm:" },
    { QuitRdm, "quit-rdm", 'q', no_argument, "Tell server to shut down." },
    { RestartRdm, "restart-rdm", 'e', optional_argument, "Restart rdm [args] before doing the rest of the commands." },
    { AutostartRdm, "autostart-rdm", 'a', optional_argument, "Output elisp: (list \"one\" \"two\" ...)." },

    { None, 0, 0, 0, "" },
    { None, 0, 0, 0, "Project management:" },
    { Makefile, "makefile", 'm', optional_argument, "Process this makefile." },
    { Clear, "clear", 'C', no_argument, "Clear projects." },
    { Project, "project", 'w', optional_argument, "With arg, select project matching that if unique, otherwise list all projects." },
    { DeleteProject, "delete-project", 'W', required_argument, "Delete all projects matching regexp." },
    { GRTag, "grtag", 't', optional_argument, "Index this directory using grtags." },
    { SmartProject, "smart-project", 'j', optional_argument, "Try to guess the source files and includepaths for a certain path. Often has to be combined with -D." },

    { None, 0, 0, 0, "" },
    { None, 0, 0, 0, "Commands:" },
    { FollowLocation, "follow-location", 'f', required_argument, "Follow this location." },
    { ReferenceName, "references-name", 'R', required_argument, "Find references matching arg." },
    { ReferenceLocation, "references", 'r', required_argument, "Find references matching this location." },
    { ListSymbols, "list-symbols", 'S', optional_argument, "List symbol names matching arg." },
    { FindSymbols, "find-symbols", 'F', required_argument, "Find symbols matching arg." },
    { CursorInfo, "cursor-info", 'U', required_argument, "Get cursor info for this location." },
    { Status, "status", 's', optional_argument, "Dump status of rdm. Arg can be symbols or symbolNames." },
    { IsIndexed, "is-indexed", 'T', required_argument, "Check if rtags knows about, and is ready to return information about, this source file." },
    { HasFileManager, "has-filemanager", 0, optional_argument, "Check if rtags has info about files in this directory." },
    { PreprocessFile, "preprocess", 0, required_argument, "Preprocess file." },
    { Reindex, "reindex", 'V', optional_argument, "Reindex all files or all files matching pattern." },
    { FindFile, "path", 'P', optional_argument, "Print files matching pattern." },
    { DumpFile, "dump-file", 'd', required_argument, "Dump source file." },
    { RdmLog, "rdm-log", 'g', no_argument, "Receive logs from rdm." },

    { None, 0, 0, 0, "" },
    { None, 0, 0, 0, "Command flags:" },
    { SkipParen, "skip-paren", 'p', no_argument, "Skip parens in various contexts." },
    { Max, "max", 'M', required_argument, "Max lines of output for queries." },
    { ReverseSort, "reverse-sort", 'O', no_argument, "Sort output reversed." },
//        { UnsavedFile, "unsaved-file", 'u', required_argument, },
    { LogFile, "log-file", 'L', required_argument, "Log to this file." },
    { NoContext, "no-context", 'N', no_argument, "Don't print context for locations." },
    { LineNumbers, "line-numbers", 'l', no_argument, "Output line numbers instead of offsets." },
    { PathFilter, "path-filter", 'i', required_argument, "Filter out results not matching with arg." },
    { FilterSystemHeaders, "filter-system-headers", 'H', no_argument, "Don't exempt system headers from path filters." },
    { Includepath, "includepath", 'I', required_argument, "Add additional include path." },
    { Define, "define", 'D', required_argument, "Add additional define." },
    { CompilerFlag, "compiler-flag", 'o', required_argument, "Add additional compiler flags." },
    { AllReferences, "all-references", 'E', no_argument, "Include definitions/declarations/constructors/destructors for references. Used for rename symbol." },
    { ElispList, "elisp-list", 'Y', no_argument, "Output elisp: (list \"one\" \"two\" ...)." },
    { Fixits, "fixits", 'x', required_argument, "Get fixits for file." },
    { Errors, "errors", 'Q', required_argument, "Get errors for file." },
    { Diagnostics, "diagnostics", 'G', no_argument, "Receive continual diagnostics from rdm." },
    { WaitForIndexing, "wait-for-indexing", 'X', no_argument, "Wait for indexing to finish before doing query." },
    { MatchRegexp, "match-regexp", 'Z', no_argument, "Treat various text patterns as regexps (-P, -i, -V)." },
    { AbsolutePath, "absolute-path", 'K', no_argument, "Print files with absolute path." },
    { SocketFile, "socket-file", 'n', required_argument, "Use this socket file (default ~/.rdm)." },
    { AlwaysMake, "always-make", 'B', no_argument, "Pass -B to make for rc -m." },
    { Timeout, "timeout", 'y', required_argument, "Max time in ms to wait for job to finish (default no timeout)." },
    { SniffMake, "sniff-make", 'J', no_argument, "No make trickery, only parse the output. Assumes you've run make clean first." },
    { FindVirtuals, "find-virtuals", 'k', no_argument, "Use in combinations with -R or -r to show other implementations of this function." },
    { None, 0, 0, 0, 0 }
};

static void help(FILE *f, const char* app)
{
    List<ByteArray> out;
    int longest = 0;
    for (int i=0; opts[i].description; ++i) {
        if (!opts[i].longOpt && !opts[i].shortOpt) {
            out.append(ByteArray());
        } else {
            out.append(ByteArray::snprintf<64>("  %s%s%s%s",
                                               opts[i].longOpt ? ByteArray::snprintf<4>("--%s", opts[i].longOpt).constData() : "",
                                               opts[i].longOpt && opts[i].shortOpt ? "|" : "",
                                               opts[i].shortOpt ? ByteArray::snprintf<2>("-%c", opts[i].shortOpt).constData() : "",
                                               opts[i].argument == required_argument ? " [arg] "
                                               : opts[i].argument == optional_argument ? " [optional] " : ""));
            longest = std::max<int>(out[i].size(), longest);
        }
    }
    fprintf(f, "%s options...\n", app);
    const int count = out.size();
    for (int i=0; i<count; ++i) {
        if (out.at(i).isEmpty()) {
            fprintf(f, "%s\n", opts[i].description);
        } else {
            fprintf(f, "%s%s%s\n",
                    out.at(i).constData(),
                    ByteArray(longest - out.at(i).size(), ' ').constData(),
                    opts[i].description);
        }
    }
}

bool RClient::parse(int &argc, char **argv)
{
    RTags::findApplicationDirPath(*argv);
    mSocketFile = Path::home() + ".rdm";

    List<option> options;
    options.reserve(sizeof(opts) / sizeof(Option));
    List<QueryCommand*> projectCommands;

    ByteArray shortOptionString;
    Map<int, Option*> shortOptions, longOptions;
    for (int i=0; opts[i].description; ++i) {
        if (opts[i].option != None) {
            const option opt = { opts[i].longOpt, opts[i].argument, 0, opts[i].shortOpt };
            if (opts[i].shortOpt) {
                shortOptionString.append(opts[i].shortOpt);
                switch (opts[i].argument) {
                case no_argument:
                    break;
                case required_argument:
                    shortOptionString.append(':');
                    break;
                case optional_argument:
                    shortOptionString.append("::");
                    break;
                }
                assert(!shortOptions.contains(opts[i].shortOpt));
                shortOptions[opts[i].shortOpt] = &opts[i];
            }
            if (opts[i].longOpt)
                longOptions[options.size()] = &opts[i];
            options.push_back(opt);
        }
    }
    {
        const option opt = { 0, 0, 0, 0 };
        options.push_back(opt);
    }

    Path logFile;
    unsigned logFlags = 0;

    // Unused: Abcz

    while (true) {
        int idx = -1;
        const int c = getopt_long(argc, argv, shortOptionString.constData(), options.data(), &idx);
        bool done = false;
        switch (c) {
        case -1:
        case '?':
        case ':':
            done = true;
            break;
        default:
            break;
        }
        if (done)
            break;

        const Option *opt = (idx == -1 ? shortOptions.value(c) : longOptions.value(idx));
        assert(opt);

        switch (opt->option) {
        case Help:
            help(stdout, argv[0]);
            return 0;
        case SocketFile:
            mSocketFile = optarg;
            break;
        case FindVirtuals:
            mQueryFlags |= QueryMessage::FindVirtuals;
            break;
        case AlwaysMake:
            mMakefileFlags |= ProjectMessage::UseDashB;
            break;
        case SniffMake:
            mMakefileFlags |= ProjectMessage::NoMakeTricks;
            break;
        case AutostartRdm:
            mClientFlags |= Client::AutostartRdm;
            if (optarg)
                mRdmArgs = ByteArray(optarg, strlen(optarg)).split(' ');
            break;
        case RestartRdm:
            mClientFlags |= Client::RestartRdm;
            if (optarg)
                mRdmArgs = ByteArray(optarg, strlen(optarg)).split(' ');
            break;
        case AllReferences:
            mQueryFlags |= QueryMessage::ReferencesForRenameSymbol;
            break;
        case MatchRegexp:
            mQueryFlags |= QueryMessage::MatchRegexp;
            break;
        case AbsolutePath:
            mQueryFlags |= QueryMessage::AbsolutePath;
            break;
        case WaitForIndexing:
            mQueryFlags |= QueryMessage::WaitForIndexing;
            break;
        case ReverseSort:
            mQueryFlags |= QueryMessage::ReverseSort;
            break;
        case ElispList:
            mQueryFlags |= QueryMessage::ElispList;
            break;
        case FilterSystemHeaders:
            mQueryFlags |= QueryMessage::FilterSystemIncludes;
            break;
        case Includepath: {
            ByteArray flag("-I");
            flag += optarg;
            mExtraCompilerFlags.append(flag);
            break; }
        case Define: {
            ByteArray flag("-D");
            flag += optarg;
            mExtraCompilerFlags.append(flag);
            break; }
        case CompilerFlag:
            mExtraCompilerFlags.append(optarg);
            break;
        case NoContext:
            mQueryFlags |= QueryMessage::NoContext;
            break;
        case PathFilter:
            mPathFilters.insert(optarg);
            break;
        case LineNumbers:
            mQueryFlags |= QueryMessage::LineNumbers;
            break;
        case Verbose:
            ++mLogLevel;
            break;
        case LogFile:
            logFile = optarg;
            break;
        case SkipParen:
            mQueryFlags |= QueryMessage::SkipParentheses;
            break;
        case Max:
            mMax = atoi(optarg);
            if (mMax <= 0) {
                fprintf(stderr, "-M [arg] must be positive integer\n");
                return false;
            }
            break;
        case Timeout:
            mTimeout = atoi(optarg);
            if (mTimeout <= 0) {
                fprintf(stderr, "-y [arg] must be positive integer\n");
                return false;
            }
            break;
            // case UnsavedFile: {
            //     const ByteArray arg(optarg);
            //     const int colon = arg.lastIndexOf(':');
            //     if (colon == -1) {
            //         fprintf(stderr, "Can't parse -u [%s]\n", optarg);
            //         return false;
            //     }
            //     const int bytes = atoi(arg.constData() + colon + 1);
            //     if (!bytes) {
            //         fprintf(stderr, "Can't parse -u [%s]\n", optarg);
            //         return false;
            //     }
            //     const Path path = Path::resolved(arg.left(colon));
            //     if (!path.isFile()) {
            //         fprintf(stderr, "Can't open [%s] for reading\n", arg.left(colon).nullTerminated());
            //         return false;
            //     }

            //     ByteArray contents(bytes, '\0');
            //     const int r = fread(contents.data(), 1, bytes, stdin);
            //     if (r != bytes) {
            //         fprintf(stderr, "Read error %d (%s). Got %d, expected %d\n",
            //                 errno, strerror(errno), r, bytes);
            //     }
            //     mUnsavedFiles[path] = contents;
            //     break; }
        case FollowLocation:
        case CursorInfo:
        case ReferenceLocation: {
            const ByteArray encoded = Location::encodeClientLocation(optarg);
            if (encoded.isEmpty()) {
                fprintf(stderr, "Can't resolve argument %s\n", optarg);
                return false;
            }
            QueryMessage::Type type = QueryMessage::Invalid;
            switch (opt->option) {
            case FollowLocation: type = QueryMessage::FollowLocation; break;
            case CursorInfo: type = QueryMessage::CursorInfo; break;
            case ReferenceLocation: type = QueryMessage::ReferencesLocation; break;
            }
            addQuery(type, encoded);
            break; }
        case Clear:
            addQuery(QueryMessage::ClearProjects);
            break;
        case RdmLog:
            addLog(RdmLogCommand::Default);
            break;
        case Diagnostics:
            addLog(CompilationError);
            break;
        case QuitRdm:
            addQuery(QueryMessage::Shutdown);
            break;
        case DeleteProject:
            addQuery(QueryMessage::DeleteProject, optarg);
            break;
        case Reindex:
        case Project:
        case FindFile:
        case ListSymbols:
        case Status: {
            QueryMessage::Type type = QueryMessage::Invalid;
            switch (opt->option) {
            case Reindex: type = QueryMessage::Reindex; break;
            case Project: type = QueryMessage::Project; break;
            case FindFile: type = QueryMessage::FindFile; break;
            case Status: type = QueryMessage::Status; break;
            case ListSymbols: type = QueryMessage::ListSymbols; break;
            }

            QueryCommand *cmd;
            if (optarg) {
                cmd = addQuery(type, optarg);
            } else if (optind < argc && argv[optind][0] != '-') {
                cmd = addQuery(type, argv[optind++]);
            } else {
                cmd = addQuery(type);
            }
            assert(cmd);
            if (type == QueryMessage::Project)
                projectCommands.append(cmd);
            break; }
        case GRTag:
            if (optarg) {
                addGRTag(Path::resolved(optarg));
            } else if (optind < argc && argv[optind][0] != '-') {
                addGRTag(Path::resolved(argv[optind++]));
            } else {
                addGRTag(Path::resolved("."));
            }
            break;
        case SmartProject:
            if (optarg) {
                addSmartProject(Path::resolved(optarg));
            } else if (optind < argc && argv[optind][0] != '-') {
                addSmartProject(Path::resolved(argv[optind++]));
            } else {
                addSmartProject(Path::resolved("."));
            }
            break;
        case HasFileManager: {
            Path p;
            if (optarg) {
                p = Path::resolved(optarg);
            } else if (optind < argc && argv[optind][0] != '-') {
                p = Path::resolved(argv[optind++]);
            } else {
                p = Path::resolved(".");
            }

            if (!p.exists()) {
                fprintf(stderr, "%s does not seem to exist\n", optarg);
                return false;
            }
            if (p.isDir())
                p.append('/');
            addQuery(QueryMessage::HasFileManager, p);
            break; }
        case IsIndexed:
        case Fixits:
        case DumpFile:
        case PreprocessFile:
        case Errors: {
            Path p = Path::resolved(optarg);
            if (!p.exists()) {
                fprintf(stderr, "%s does not exist\n", optarg);
                return false;
            }
            if (p.isDir())
                p.append('/');
            QueryMessage::Type type = QueryMessage::Invalid;
            switch (opt->option) {
            case IsIndexed: type = QueryMessage::IsIndexed; break;
            case Fixits: type = QueryMessage::FixIts; break;
            case Errors: type = QueryMessage::Errors; break;
            case DumpFile: type = QueryMessage::DumpFile; break;
            case PreprocessFile: type = QueryMessage::PreprocessFile; break;
            }

            addQuery(type, p);
            break; }
        case Makefile: {
            Path makefile;
            if (optarg) {
                makefile = Path::resolved(optarg);
                if (!makefile.isFile()) {
                    fprintf(stderr, "%s is not a file\n", optarg);
                    return false;
                }
            } else {
                if (optind < argc) {
                    makefile = Path::resolved(argv[optind]);
                    if (!makefile.isFile()) {
                        makefile = Path::resolved("Makefile");
                        if (!makefile.isFile()) {
                            fprintf(stderr, "%s is not a file", argv[optind]);
                            return false;
                        }
                    } else {
                        ++optind;
                    }
                } else {
                    makefile = Path::resolved("Makefile");
                    if (!makefile.isFile()) {
                        fprintf(stderr, "Can't find a Makefile here\n");
                        return false;
                    }
                }
            }

            List<ByteArray> makefileArgs;
            while (optind < argc && argv[optind][0] != '-')
                makefileArgs.append(argv[optind++]);
            addMakeFile(makefile, makefileArgs);
            break; }
        case ReferenceName:
            addQuery(QueryMessage::ReferencesName, optarg);
            break;
        case FindSymbols:
            addQuery(QueryMessage::FindSymbols, optarg);
            break;
        default:
            assert(0);
            break;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "rc: unexpected option -- '%s'\n", argv[optind]);
        return false;
    }

    if (!initLogging(mLogLevel, logFile, logFlags)) {
        fprintf(stderr, "Can't initialize logging with %d %s 0x%0x\n",
                mLogLevel, logFile.constData(), logFlags);
        return false;
    }


    if (mCommands.isEmpty() && !(mClientFlags & (Client::RestartRdm|Client::AutostartRdm))) {
        help(stderr, argv[0]);
        return false;
    }
    if (mCommands.size() > projectCommands.size()) {
        // If there's more than one command one likely does not want output from
        // the queryCommand (unless there's no arg specified for it). This is so
        // we don't have to pass a different flag for auto-updating project
        // using the current buffer but rather piggy-back on --project
        const int count = projectCommands.size();
        for (int i=0; i<count; ++i) {
            QueryCommand *cmd = projectCommands[i];
            if (!cmd->query.isEmpty()) {
                cmd->extraQueryFlags |= QueryMessage::Silent;
            }
        }
    }

    if (!logFile.isEmpty() || mLogLevel > 0) {
        Log l(1);
        l << argc;
        for (int i = 0; i < argc; ++i)
            l << " " << argv[i];
    }
    mArgc = argc;
    mArgv = argv;

    return true;
}

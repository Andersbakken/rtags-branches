#include "Daemon.h"
#include "GccArguments.h"
#include <QCoreApplication>
#include "Utils.h"
#include "PreCompile.h"
#include "Node.h"
#include "FileManager.h"
#include <magic.h>
#include <fcntl.h>

// const unsigned defaultFlags = (CXTranslationUnit_PrecompiledPreamble
//                                |CXTranslationUnit_CXXPrecompiledPreamble
//                                |CXTranslationUnit_CXXChainedPCH);

struct MatchBase : public Match
{
    enum Flags {
        MatchSymbolName = 0x01,
        MatchFileNames = 0x02,
        MatchRegExp = 0x04,
        SymbolOnly = 0x08,
        OneMatch = 0x10
    };

    MatchBase(uint nodeTypes, uint f)
        : Match(nodeTypes), flags(f)
    {}
    virtual MatchResult match(const QByteArray &path, const Node *node)
    {
        if (accept(path, node)) {
            int len;
            if (flags & SymbolOnly) {
                len = snprintf(buffer, BufferLength, "%s\n", node->symbolName.constData());
            } else {
                len = snprintf(buffer, BufferLength, "%s %s%s \"%s:%d:%d\"\n",
                               nodeTypeToName(node->type, Abbreviated), path.constData(), node->symbolName.constData(),
                               node->location.path.constData(), node->location.line, node->location.column);
            }
            output.append(buffer); // ### use len and QByteArray::fromRawData
            return (flags & OneMatch ? Finish : Recurse);
        }
        return Skip;
    }

    virtual bool accept(const QByteArray &path, const Node *node) = 0;

    const uint flags;
    enum { BufferLength = 1024 };
    char buffer[BufferLength];
    QByteArray output;
};

struct FollowSymbolMatch : public Match
{
    FollowSymbolMatch(const Location &loc)
        : Match(All), location(loc), found(0)
    {}
    virtual MatchResult match(const QByteArray &, const Node *node)
    {
        if (node->location == location) {
            // qDebug() << "found our location" << node->location << node->symbolName
            //          << nodeTypeToName(node->type);
            switch (node->type) {
            case All:
            case Invalid:
            case Root:
                break;
            case MethodDeclaration:
                found = node->methodDefinition();
                break;
            case Reference:
                found = node->parent;
                break;
            case MethodDefinition:
                found = node->methodDeclaration();
                break;
            case Class:
            case Struct:
            case Namespace:
            case Variable:
            case Enum:
            case Typedef:
            case MacroDefinition:
                break;
            case EnumValue:
                node = node->parent; // parent is Enum
                break;
            }
            return Finish;
        }

        return Recurse;
    }
    const Location &location;
    Node *found;
};

struct GenericMatch : public MatchBase
{
    GenericMatch(uint nodeTypes, uint flags, const QRegExp &r, const QByteArray &m)
        : MatchBase(nodeTypes, flags), regexp(r), match(m)
    {
    }
    virtual bool accept(const QByteArray &path, const Node *node)
    {
        if (flags & MatchFileNames) {
            if (!match.isEmpty() && node->location.path.contains(match))
                return true;
            if (flags & MatchRegExp && QString::fromLocal8Bit(node->location.path).contains(regexp))
                return true;
        }
        if (flags & MatchSymbolName) {
            const QByteArray full = path + node->symbolName;
            if (!match.isEmpty() && full.contains(match))
                return true;
            if (flags & MatchRegExp && QString::fromLocal8Bit(full).contains(regexp))
                return true;
        }
        return false;
    }
    const QRegExp &regexp;
    const QByteArray &match;
};

template <typename T>
static inline QByteArray joined(const T &container, const char joinCharacter = '\n')
{
    QByteArray joined;
    joined.reserve(container.size() * 100);
    foreach(const QByteArray &f, container) {
        joined += f + joinCharacter;
    }
    if (!joined.isEmpty())
        joined.chop(1);
    return joined;
}

const unsigned defaultFlags = 0;

static QHash<QByteArray, QVariant> createResultMap(const QByteArray& result)
{
    QHash<QByteArray, QVariant> ret;
    ret.insert("result", result);
    return ret;
}

#warning should be able to get signature of current function were on and we could display it in the modeline or something (or some popup while typing)
Daemon::Daemon(QObject *parent)
    : QObject(parent), mParseThread(&mFileManager, &mVisitThread)
{
    qRegisterMetaType<Path>("Path");
    qRegisterMetaType<QSet<Path> >("QSet<Path>");
    qRegisterMetaType<CXTranslationUnit>("CXTranslationUnit");
    connect(&mParseThread, SIGNAL(fileParsed(Path, void*)), &mVisitThread, SLOT(onFileParsed(Path, void*)));
    connect(&mParseThread, SIGNAL(dependenciesAdded(QSet<Path>)), this, SLOT(onDependenciesAdded(QSet<Path>)));
    connect(&mFileManager, SIGNAL(done()), this, SLOT(quit()));
    connect(&mVisitThread, SIGNAL(done()), this, SLOT(quit()));
    mParseThread.start();
    mVisitThread.start();
    mFileManager.start();
}

Daemon::~Daemon()
{
    mParseThread.abort();
    mVisitThread.quit();
    mFileManager.quit();
    QThread *threads[] = { &mParseThread, &mVisitThread, &mFileManager, 0 };
    for (int i=0; threads[i]; ++i)
        threads[i]->wait();
}

static QHash<QByteArray, QVariant> syntax()
{
    return createResultMap("Syntax: rtags --command=command [--argument1, --argument2=foo, ...]\n"
                           "commands: syntax|quit|add|remove|lookupline|makefile|daemonize|files|lookup\n");
}

void Daemon::quit()
{
    mParseThread.abort();
    mVisitThread.abort();
    mFileManager.quit();
    mParseThread.wait();
    mVisitThread.wait();
    mFileManager.wait();
    QTimer::singleShot(100, QCoreApplication::instance(), SLOT(quit()));
    // hack to make the quit command properly respond before the server goes down
}

QHash<QByteArray, QVariant> Daemon::runCommand(const QHash<QByteArray, QVariant> &dashArgs,
                                               QList<QByteArray> freeArgs)
{
    qDebug() << "runCommand" << dashArgs << freeArgs;
    QByteArray cmd = freeArgs.value(0);
    if (cmd.isEmpty())
        return createResultMap("No command or path specified");

    const Path cwd = dashArgs.value("cwd").toByteArray();
    bool removeFirst = true;
    if (cmd != "makefile") { // makefile will resolve to Makefile on Mac so in that case we skip this
        Path p = Path::resolved(cmd, cwd);
        if (p.isDir()) {
            p = p + "/Makefile";
            if (p.magicType() == Path::Makefile) {
                removeFirst = false;
                cmd = "makefile";
                freeArgs[0] = p;
            }
        } else if (p.isFile()) {
            switch (p.magicType()) {
            case Path::Source:
            case Path::Header:
                removeFirst = false;
                cmd = "load";
                break;
            case Path::Makefile:
                removeFirst = false;
                cmd = "makefile";
                break;
            case Path::Other:
                break;
            }
        }
    }
    if (removeFirst)
        freeArgs.removeFirst();
    const int size = freeArgs.size();
    for (int i=0; i<size; ++i) {
        bool ok;
        Path p = Path::resolved(freeArgs.at(i), cwd, &ok);
        if (ok)
            freeArgs[i] = p;
    }

    if (cmd == "syntax") {
        quit();
        return syntax();
    } else if (cmd == "quit") {
        quit();
        return createResultMap("quitting");
    } else if (cmd == "printtree") {
        return printTree(dashArgs, freeArgs);
    } else if (cmd == "followsymbol") {
        return followSymbol(dashArgs, freeArgs);
    } else if (cmd == "makefile") {
        return addMakefile(dashArgs, freeArgs);
    } else if (cmd == "lookup") {
        return lookup(dashArgs, freeArgs);
    } else if (cmd == "load") {
        return load(dashArgs, freeArgs);
    } else if (cmd == "dependencies") {
        return createResultMap(mFileManager.dependencyMap());
    } else if (cmd == "gccarguments") {
        const Path cwd = dashArgs.value("cwd").toByteArray();
        QByteArray ret;
        for (int i=1; i<freeArgs.size(); ++i) {
            const Path p = Path::resolved(freeArgs.at(i), cwd);
            ret.append(p + ": " + mFileManager.arguments(p).raw() + '\n');
        }
        if (!ret.isEmpty())
            ret.chop(1);
        return createResultMap(ret);
    }
    return createResultMap("Unknown command");
}

QHash<QByteArray, QVariant> Daemon::addMakefile(const QHash<QByteArray, QVariant>& dashArgs,
                                                const QList<QByteArray>& freeArgs)
{
    Q_UNUSED(dashArgs);

    if (freeArgs.isEmpty())
        return createResultMap("No Makefile passed");

    Path makefile = freeArgs.first();
    if (!makefile.isResolved())
        makefile.resolve();
    if (makefile.isDir())
        makefile = makefile + "/Makefile";
    if (!makefile.isFile()) {
        return createResultMap("Makefile does not exist: " + makefile);
    }
    mFileManager.addMakefile(makefile);
    return createResultMap("Added makefile");
}

QHash<QByteArray, QVariant> Daemon::lookup(const QHash<QByteArray, QVariant> &args, const QList<QByteArray> &freeArgs)
{
    uint nodeTypes = 0;
    foreach(const QByteArray &type, args.value("types").toByteArray().split(',')) {
        if (type.isEmpty())
            continue;
        const NodeType t = stringToNodeType(type);
        if (t) {
            nodeTypes |= t;
        } else {
            return createResultMap("Can't parse type " + type);
        }
    }
    if (!nodeTypes)
        nodeTypes = (All & ~Root);

    QRegExp rx;
    QByteArray ba;
    uint flags = 0;
    if (args.contains("regexp")) {
        rx = QRegExp(QString::fromLocal8Bit(freeArgs.value(0)));
        if (!rx.isEmpty() && rx.isValid())
            flags |= MatchBase::MatchRegExp;
    } else {
        ba = freeArgs.value(0);
    }
    if (args.contains("symbolonly"))
        flags |= MatchBase::SymbolOnly;

    if (args.contains("filename"))
        flags |= MatchBase::MatchFileNames;
    if (args.contains("symbolname") || !(flags & (MatchBase::MatchFileNames)))
        flags |= MatchBase::MatchSymbolName;

    GenericMatch match(nodeTypes, flags, rx, ba);
    mVisitThread.lookup(&match);

    return createResultMap(match.output);
}

QHash<QByteArray, QVariant> Daemon::load(const QHash<QByteArray, QVariant>&dashArgs,
                                         const QList<QByteArray> &freeArgs)
{
    Path cwd = dashArgs.value("cwd").toByteArray();
    int count = 0;
    foreach(const QByteArray &arg, freeArgs) {
        bool ok;
        const Path p = Path::resolved(arg, cwd, &ok);
        if (!ok) {
            qWarning() << p << arg << "doesn't seem to exist";
        } else {
            ++count;
            mParseThread.load(p);
        }
    }

    return createResultMap("Loading " + QByteArray::number(count) + " files");
}

QHash<QByteArray, QVariant> Daemon::followSymbol(const QHash<QByteArray, QVariant>& args,
                                                 const QList<QByteArray> &freeArgs)
{
    if (freeArgs.size() != 1)
        return createResultMap("Invalid args");
    Path path = freeArgs.first();
    if (!path.resolve())
        return createResultMap("Invalid file " + freeArgs.first());
    bool ok;
    const int line = args.value("line").toUInt(&ok);
    if (!ok)
        return createResultMap("Invalid line arg");
    const int col = args.value("column").toUInt(&ok);
    if (!ok)
        return createResultMap("Invalid column arg");
    const Location loc(path, line, col);
    mVisitThread.lockMutex();
    Node *node = mVisitThread.nodeForLocation(loc);
    QHash<QByteArray, QVariant> ret;
    if (node) {
        switch (node->type) {
        case MethodDeclaration:
            node = node->methodDefinition();
            break;
        case Reference:
            node = node->parent;
            break;
        case MethodDefinition:
            node = node->methodDeclaration();
            break;
        case EnumValue:
            node = node->parent; // parent is Enum
            break;
        default:
            node = 0;
        }
    }
    if (node) {
        ret = createResultMap(node->location.toString());
    } else {
        ret = createResultMap("Can't follow symbol");
    }
    mVisitThread.unlockMutex();
    return ret;
}

QDebug operator<<(QDebug dbg, CXCursor cursor)
{
    QString text = "";
    if (clang_isInvalid(clang_getCursorKind(cursor))) {
        text += "";
        dbg << text;
        return dbg;
    }

    QByteArray name = eatString(clang_getCursorDisplayName(cursor));
    if (name.isEmpty())
        name = eatString(clang_getCursorSpelling(cursor));
    if (!name.isEmpty()) {
        text += name + ", ";
    }
    if (clang_isCursorDefinition(cursor))
        text += "(def), ";
    text += kindToString(clang_getCursorKind(cursor));
    CXSourceLocation location = clang_getCursorLocation(cursor);
    unsigned int line, column, offset;
    CXFile file;
    clang_getInstantiationLocation(location, &file, &line, &column, &offset);
    Path path = eatString(clang_getFileName(file));
    if (path.resolve()) {
        text += QString(", %1:%2:%3").arg(QString::fromLocal8Bit(path)).arg(line).arg(column);
    }
    if (clang_isCursorDefinition(cursor))
        text += ", def";
    dbg << text;
    return dbg;
}


QHash<QByteArray, QVariant> Daemon::printTree(const QHash<QByteArray, QVariant>&, const QList<QByteArray> &)
{
    struct TreeMatch : public Match
    {
        TreeMatch()
            : Match(All)
        {}
        QByteArray out;
        virtual MatchResult match(const QByteArray &, const Node *node)
        {
            out += node->toString() + '\n';
            return Recurse;
        }
    } match;
    mVisitThread.lookup(&match);
    return createResultMap(match.out);
}

void Daemon::onDependenciesAdded(const QSet<Path> &paths)
{
#warning do this
    // qWarning() << "Not adding dependencies right now" << paths;
    return;
    QList<QByteArray> sources;
    foreach(const Path &p, paths) {
        QSet<Path> extraSources;
        mFileManager.getInfo(p, 0, &extraSources, 0);
        foreach(const Path &extraSource, extraSources)
            sources += extraSource;
    }
    load(QHash<QByteArray, QVariant>(), sources);
}

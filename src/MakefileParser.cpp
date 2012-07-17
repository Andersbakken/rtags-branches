#include "MakefileParser.h"

#include "List.h"
#include "Log.h"
#include "Process.h"
#include "RTags.h"
#include "RegExp.h"
#include <stdio.h>

#ifndef MAKE
#define MAKE "make"
#endif

class DirectoryTracker
{
public:
    DirectoryTracker();

    void init(const Path &path);
    void track(const ByteArray &line);

    const Path &path() const { return mPaths.back(); }

private:
    void enterDirectory(const ByteArray &dir);
    void leaveDirectory(const ByteArray &dir);

private:
    List<Path> mPaths;
};

DirectoryTracker::DirectoryTracker()
{
}

void DirectoryTracker::init(const Path &path)
{
    mPaths.push_back(path);
}

void DirectoryTracker::track(const ByteArray &line)
{
    // printf("Tracking %s\n", line.constData());
    static RegExp rx("make[^:]*: ([^ ]+) directory `([^']+)'");
    List<RegExp::Capture> captures;
    if (rx.indexIn(line.constData(), 0, &captures) != -1) {
        assert(captures.size() >= 3);
        if (captures.at(1).capture() == "Entering") {
            enterDirectory(captures.at(2).capture());
        } else if (captures.at(1).capture() == "Leaving") {
            leaveDirectory(captures.at(2).capture());
        } else {
            error("Invalid directory track: %s %s",
                  captures.at(1).capture().constData(),
                  captures.at(2).capture().constData());
        }
    }
}

void DirectoryTracker::enterDirectory(const ByteArray &dir)
{
    bool ok;
    Path newPath = Path::resolved(dir, path(), &ok);
    if (ok) {
        mPaths.push_back(newPath);
        debug("New directory resolved: %s", newPath.constData());
    } else {
        error("Unable to resolve path %s (%s)", dir.constData(), path().constData());
    }
}

void DirectoryTracker::leaveDirectory(const ByteArray &dir)
{
    verboseDebug() << "leaveDirectory" << dir;
    // enter and leave share the same code for now
    mPaths.pop_back();
    // enterDirectory(dir);
}

MakefileParser::MakefileParser(const List<ByteArray> &extraFlags, Connection *conn)
    : mProc(0), mTracker(new DirectoryTracker), mExtraFlags(extraFlags),
      mConnection(conn), mHasProject(false)
{
}

MakefileParser::~MakefileParser()
{
    if (mProc) {
        mProc->stop();
        delete mProc;
    }
    delete mTracker;
}

void MakefileParser::run(const Path &makefile, const List<ByteArray> &args)
{
    mMakefile = makefile;
    assert(!mProc);
    mProc = new Process;

    List<ByteArray> environment = Process::environment();
    if (!args.contains("-B")) {
        Path p = RTags::applicationDirPath();
#ifdef OS_Darwin
        p += "/../makelib/libmakelib.so";
        p.resolve();
        environment.push_back("DYLD_INSERT_LIBRARIES=" + p);
#else
        p += "/../makelib/libmakelib.so";
        p.resolve();
        environment.push_back("LD_PRELOAD=" + p);
#endif
    }

    mProc->readyReadStdOut().connect(this, &MakefileParser::processMakeOutput);
    mProc->readyReadStdErr().connect(this, &MakefileParser::processMakeError);
    mProc->finished().connect(this, &MakefileParser::onDone);

    mTracker->init(makefile.parentDir());
    warning(MAKE " -j1 -w -f %s -C %s\n",
            makefile.constData(), mTracker->path().constData());

    List<ByteArray> a;
    a.push_back("-j1");
    a.push_back("-w");
    a.push_back("-f");
    a.push_back(makefile);
    a.push_back("-C");
    a.push_back(mTracker->path());
    a.push_back("AM_DEFAULT_VERBOSITY=1");
    a.push_back("VERBOSE=1");

    const int c = args.size();
    for (int i=0; i<c; ++i) {
        a.push_back(args.at(i));
    }

    unlink("/tmp/makelib.log");
    if (!mProc->start(MAKE, a, environment))
        error() << "Process failed" << mProc->errorString();
}

bool MakefileParser::isDone() const
{
    return mProc && mProc->isFinished();
}

void MakefileParser::processMakeOutput()
{
    assert(mProc);
    mData += mProc->readAllStdOut();

    // ### this could be more efficient
    int nextNewline = mData.indexOf('\n');
    while (nextNewline != -1) {
        processMakeLine(mData.left(nextNewline));
        mData = mData.mid(nextNewline + 1);
        nextNewline = mData.indexOf('\n');
    }
}

void MakefileParser::processMakeError()
{
    assert(mProc);
    error("got stderr from make: '%s'", mProc->readAllStdErr().nullTerminated());
}

void MakefileParser::processMakeLine(const ByteArray &line)
{
    //printf("processMakeLine '%s'\n", line.nullTerminated());
    if (testLog(VerboseDebug))
        verboseDebug("%s", line.constData());
    GccArguments args;
    if (args.parse(line, mTracker->path())) {
        args.addFlags(mExtraFlags);
        List<Path> inputFiles = args.inputFiles();
        List<Path> unresolvedInputFiles = args.unresolvedInputFiles();
        const int c = inputFiles.size();
        assert(c == unresolvedInputFiles.size());
        ByteArray output;
        if (args.type() == GccArguments::Pch) {
            output = args.outputFile();
            assert(!output.isEmpty());
            const int ext = output.lastIndexOf(".gch/c");
            if (ext != -1) {
                output = output.left(ext + 4);
            } else if (!output.endsWith(".gch")) {
                error("couldn't find .gch in pch output");
                return;
            }
        }

        for (int i=0; i<c; ++i) {
            const Path p = inputFiles.at(i);
            args.mInputFiles.clear();
            args.mInputFiles.append(p);
            const Path unresolved = unresolvedInputFiles.at(i);
            args.mUnresolvedInputFiles.clear();
            args.mUnresolvedInputFiles.append(unresolved);
            if (args.type() == GccArguments::Pch) {
                setPch(output, p);
                mPchFiles[p] = args;
            } else {
                mSourceFiles[p] = args;
            }
        }
    } else {
        mTracker->track(line);
    }
}

List<ByteArray> MakefileParser::mapPchToInput(const List<ByteArray> &input) const
{
    List<ByteArray> output;
    Map<ByteArray, ByteArray>::const_iterator pchit;
    const Map<ByteArray, ByteArray>::const_iterator pchend = mPchs.end();
    const int count = input.size();
    for (int i=0; i<count; ++i) {
        const ByteArray &in = input.at(i);
        pchit = mPchs.find(in);
        if (pchit != pchend)
            output.append(pchit->second);
    }
    return output;
}

void MakefileParser::setPch(const ByteArray &output, const ByteArray &input)
{
    mPchs[output] = input;
}

void MakefileParser::onDone()
{
    done()(this);
}

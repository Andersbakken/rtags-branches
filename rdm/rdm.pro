######################################################################
# Automatically generated by qmake (2.01a) Mon Feb 6 20:18:57 2012
######################################################################

TEMPLATE = app
TARGET =
DEPENDPATH += .
INCLUDEPATH += . ../3rdparty/leveldb/src/include ../3rdparty/kyoto/src
#DEFINES += BUILDING_RDM USE_LEVELDB
DEFINES += BUILDING_RDM USE_KYOTO
include(../shared/shared.pri)
include(../shared/clang.pri)

# Input
SOURCES += \
    main.cpp \
    Indexer.cpp \
    Server.cpp \
    SHA256.cpp \
    DumpJob.cpp \
    FollowLocationJob.cpp \
    CursorInfoJob.cpp \
    MatchJob.cpp \
    ReferencesJob.cpp \
    Rdm.cpp \
    StatusJob.cpp \
    IndexerJob.cpp \
    DirtyJob.cpp \
    Job.cpp \
    TestJob.cpp \
    MemoryMonitor.cpp \
    Database.cpp \
    Location.cpp

HEADERS += \
    Indexer.h \
    Server.h \
    SHA256.h \
    DumpJob.h \
    FollowLocationJob.h \
    CursorInfoJob.h \
    MatchJob.h \
    ReferencesJob.h \
    Rdm.h \
    StatusJob.h \
    IndexerJob.h \
    Source.h \
    DirtyJob.h \
    Job.h \
    TestJob.h \
    CursorInfo.h \
    AbortInterface.h \
    MemoryMonitor.h \
    Database.h \
    Location.h

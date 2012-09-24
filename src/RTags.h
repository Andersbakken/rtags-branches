#ifndef RTags_h
#define RTags_h

#include "ByteArray.h"
#include "Location.h"
#include "Log.h"
#include "Path.h"
#include <assert.h>
#include <getopt.h>
#include <memory>
#include <stdio.h>
#include <typeinfo>

using namespace std::tr1;

class Database;
class Project;
namespace RTags {

enum DatabaseLockType {
    Read = ReadWriteLock::Read,
    Write = ReadWriteLock::Write,
    Erase
};

enum ReferenceType {
    NoReference,
    NormalReference,
    LinkedReference
};

enum UnitType {
    CompileC,
    CompileCPlusPlus
};
enum CursorType {
    Include,
    Cursor,
    Reference,
    Other
};
}

class CursorInfo;
typedef Map<Location, CursorInfo> SymbolMap;
typedef Map<ByteArray, Set<Location> > SymbolNameMap;
typedef Map<uint32_t, Set<uint32_t> > DependencyMap;
typedef Map<uint32_t, List<ByteArray> > CompileArgumentsMap;
typedef Map<Path, Set<ByteArray> > FilesMap;
typedef Map<Location, std::pair<int, ByteArray> > FixitMap;
typedef Map<uint32_t, List<ByteArray> > DiagnosticsMap;
typedef Map<uint32_t, time_t> GRFilesMap;
// file id to last modified, time_t means currently parsing
typedef Map<ByteArray, Map<Location, bool> > GRMap;
// symbolName to Map<location, bool> bool == false means cursor, true means reference

namespace RTags {
void dirtySymbolNames(SymbolNameMap &map, const Set<uint32_t> &dirty);
void dirtySymbols(SymbolMap &map, const Set<uint32_t> &dirty);

ByteArray backtrace(int maxFrames = -1);

static inline const char *kindToString(CXIdxEntityKind kind)
{
    switch (kind) {
    case CXIdxEntity_Unexposed: return "Unexposed";
    case CXIdxEntity_Typedef: return "Typedef";
    case CXIdxEntity_Function: return "Function";
    case CXIdxEntity_Variable: return "Variable";
    case CXIdxEntity_Field: return "Field";
    case CXIdxEntity_EnumConstant: return "EnumConstant";
    case CXIdxEntity_ObjCClass: return "ObjCClass";
    case CXIdxEntity_ObjCProtocol: return "ObjCProtocol";
    case CXIdxEntity_ObjCCategory: return "ObjCCategory";
    case CXIdxEntity_ObjCInstanceMethod: return "ObjCInstanceMethod";
    case CXIdxEntity_ObjCClassMethod: return "ObjCClassMethod";
    case CXIdxEntity_ObjCProperty: return "ObjCProperty";
    case CXIdxEntity_ObjCIvar: return "ObjCIvar";
    case CXIdxEntity_Enum: return "Enum";
    case CXIdxEntity_Struct: return "Struct";
    case CXIdxEntity_Union: return "Union";
    case CXIdxEntity_CXXClass: return "CXXClass";
    case CXIdxEntity_CXXNamespace: return "CXXNamespace";
    case CXIdxEntity_CXXNamespaceAlias: return "CXXNamespaceAlias";
    case CXIdxEntity_CXXStaticVariable: return "CXXStaticVariable";
    case CXIdxEntity_CXXStaticMethod: return "CXXStaticMethod";
    case CXIdxEntity_CXXInstanceMethod: return "CXXInstanceMethod";
    case CXIdxEntity_CXXConstructor: return "CXXConstructor";
    case CXIdxEntity_CXXDestructor: return "CXXDestructor";
    case CXIdxEntity_CXXConversionFunction: return "CXXConversionFunction";
    case CXIdxEntity_CXXTypeAlias: return "CXXTypeAlias";
    }
    return "";
}

ByteArray eatString(CXString str);
enum CursorToStringFlags {
    NoCursorToStringFlags = 0x0,
    IncludeUSR = 0x1,
    IncludeRange = 0x2,
    DefaultCursorToStringFlags = IncludeRange,
    AllCursorToStringFlags = IncludeUSR|IncludeRange
};
ByteArray cursorToString(CXCursor cursor, unsigned = DefaultCursorToStringFlags);
template <typename T>
inline bool startsWith(const List<T> &list, const T &str)
{
    if (!list.isEmpty()) {
        typename List<T>::const_iterator it = std::upper_bound(list.begin(), list.end(), str);
        if (it != list.end()) {
            const int cmp = strncmp(str.constData(), (*it).constData(), (*it).size());
            if (cmp == 0) {
                return true;
            } else if (cmp < 0 && it != list.begin() && str.startsWith(*(it - 1))) {
                return true;
            }
        } else if (str.startsWith(*(it - 1))) {
            return true;
        }
    }
    return false;
}

template <typename Container, typename Value>
inline bool addTo(Container &container, const Value &value)
{
    const int oldSize = container.size();
    container += value;
    return container.size() != oldSize;
}

SymbolMap::const_iterator findCursorInfo(const SymbolMap &map, const Location &location);
CursorInfo findCursorInfo(const SymbolMap &map, const Location &location, Location *key);

enum TimeFormat {
    DateTime,
    Time,
    Date
};
inline ByteArray timeToString(time_t t, TimeFormat fmt)
{
    const char *format = 0;
    switch (fmt) {
    case DateTime:
        format = "%Y-%m-%d %H:%M:%S";
        break;
    case Date:
        format = "%Y-%m-%d";
        break;
    case Time:
        format = "%H:%M:%S";
        break;
    }

    char buf[32];
    tm tm;
    localtime_r(&t, &tm);
    const int w = strftime(buf, sizeof(buf), format, &tm);
    return ByteArray(buf, w);
}

inline bool encodePath(Path &path)
{
    int size = path.size();
    enum { EncodedUnderscoreLength = 12 };
    for (int i=0; i<size; ++i) {
        char &ch = path[i];
        switch (ch) {
        case '/':
            ch = '_';
            break;
        case '_':
            path.replace(i, 1, "<underscore>");
            size += EncodedUnderscoreLength - 1;
            i += EncodedUnderscoreLength - 1;
            break;
        case '<':
            if (i + EncodedUnderscoreLength <= size && !strncmp(&ch + 1, "underscore>", EncodedUnderscoreLength - 1)) {
                error("Invalid folder name %s", path.constData());
                return false;
            }
            break;
        }
    }
    return true;
}

inline void decodePath(Path &path)
{
    int size = path.size();
    enum { EncodedUnderscoreLength = 12 };
    for (int i=0; i<size; ++i) {
        char &ch = path[i];
        switch (ch) {
        case '_':
            ch = '/';
            break;
        case '<':
            if (i + EncodedUnderscoreLength <= size && !strncmp(&ch + 1, "underscore>", EncodedUnderscoreLength - 1)) {
                path.replace(i, EncodedUnderscoreLength, "_");
                size -= EncodedUnderscoreLength - 1;
            }
            break;
        }
    }
}


inline int digits(int len)
{
    int ret = 1;
    while (len >= 10) {
        len /= 10;
        ++ret;
    }
    return ret;
}

ByteArray shortOptions(const option *longOptions);
int readLine(FILE *f, char *buf = 0, int max = -1);
void removeDirectory(const Path &path);
int canonicalizePath(char *path, int len);
ByteArray unescape(ByteArray command);

template <typename T> class Ptr : public shared_ptr<T>
{
public:
    Ptr(T *t = 0)
        : shared_ptr<T>(t)
    {}
    operator T*() const { return shared_ptr<T>::get(); }
};
bool startProcess(const Path &dotexe, const List<ByteArray> &dollarArgs);

void findApplicationDirPath(const char *argv0);
Path applicationDirPath();
}

#define eintrwrap(VAR, BLOCK)                   \
    do {                                        \
        VAR = BLOCK;                            \
    } while (VAR == -1 && errno == EINTR)

class CursorInfo;
class CXStringScope
{
public:
    CXStringScope(CXString str)
        : string(str)
    {
    }

    ~CXStringScope()
    {
        clang_disposeString(string);
    }
    const char *data() const
    {
        return clang_getCString(string);
    }
    CXString string;
};

inline bool match(uint32_t fileId, const Location &loc)
{
    return loc.fileId() == fileId;
}

inline bool match(const Set<uint32_t> &fileIds, const Location &loc)
{
    return fileIds.contains(loc.fileId());
}

inline Log operator<<(Log dbg, CXCursor cursor)
{
    dbg << RTags::cursorToString(cursor);
    return dbg;
}

inline Log operator<<(Log dbg, CXCursorKind kind)
{
    dbg << RTags::eatString(clang_getCursorKindSpelling(kind));
    return dbg;
}

#endif

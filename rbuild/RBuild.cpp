#include "RBuild.h"
#include "Shared.h"
#include <QCoreApplication>
#include <QtAlgorithms>
#include <sstream>
#include <clang-c/Index.h>
#include <leveldb/db.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/types.h>
#include <dirent.h>
#include <Utils.h>

//#define REENTRANT_ATOMICSTRING

static inline bool cursorDefinition(const CXCursor& c)
{
    switch (clang_getCursorKind(c)) {
    case CXCursor_MacroDefinition:
        return true;
    case CXCursor_VarDecl:
        //return false;
    default:
        break;
    }

    return (clang_isCursorDefinition(c) != 0);
}

static inline bool cursorDefinitionFor(const CXCursor& d, const CXCursor c)
{
    switch (clang_getCursorKind(c)) {
    case CXCursor_CallExpr:
        return false;
    default:
        break;
    }
    return cursorDefinition(d);
}

class AtomicString
{
public:
    AtomicString() : mData(0) {}
    AtomicString(const CXString& string);
    AtomicString(const QByteArray& string);
    AtomicString(const QString& string);
    AtomicString(const AtomicString& other);
    AtomicString(const char* string, int size);
    ~AtomicString();

    AtomicString& operator=(const AtomicString& other);
    QByteArray operator*() const { return mData ? mData->data : QByteArray(); }
    bool operator==(const AtomicString& other) const { return mData == other.mData; }
    bool operator==(const QByteArray& string) const { return mData ? mData->data == string : false; }
    bool operator!=(const QByteArray& string) const { return mData ? mData->data != string : false; }
    bool operator<(const QByteArray& string) const { return mData ? mData->data < string : false; }
    bool operator>(const QByteArray& string) const { return mData ? mData->data > string : false; }
    bool operator<=(const QByteArray& string) const { return mData ? mData->data <= string : false; }
    bool operator>=(const QByteArray& string) const { return mData ? mData->data >= string : false; }

    bool isEmpty() const { return mData ? mData->data.isEmpty() : true; }
    int strcmp(const AtomicString& other) const;

    QByteArray toByteArray() const { return mData ? mData->data : QByteArray(); }
    QString toString() const { return QString::fromUtf8(mData ? mData->data.constData() : 0); }
    const char* constData() const { return mData ? mData->data.constData() : 0; }

private:
    void init(const QByteArray& str);

private:
    struct Data
    {
        QByteArray data;
        int ref;
    };

    Data* mData;

    static QHash<QByteArray, Data*> sData;
#ifdef REENTRANT_ATOMICSTRING
    static QMutex sMutex;
#endif
};

QHash<QByteArray, AtomicString::Data*> AtomicString::sData;
#ifdef REENTRANT_ATOMICSTRING
QMutex AtomicString::sMutex;
#endif

inline void AtomicString::init(const QByteArray& str)
{
    QHash<QByteArray, Data*>::iterator it = sData.find(str);
    if (it != sData.end()) {
        mData = it.value();
        ++mData->ref;
    } else {
        mData = new Data;
        mData->data = str;
        mData->ref = 1;
        sData[str] = mData;
    }
}

AtomicString::AtomicString(const CXString& string)
{
#ifdef REENTRANT_ATOMICSTRING
    QMutexLocker locker(&sMutex);
#endif
    QByteArray ba(clang_getCString(string));
    init(ba);
}

AtomicString::AtomicString(const QByteArray& string)
{
#ifdef REENTRANT_ATOMICSTRING
    QMutexLocker locker(&sMutex);
#endif
    init(string);
}

AtomicString::AtomicString(const QString& string)
{
#ifdef REENTRANT_ATOMICSTRING
    QMutexLocker locker(&sMutex);
#endif
    init(string.toUtf8());
}

AtomicString::AtomicString(const char* string, int size)
{
#ifdef REENTRANT_ATOMICSTRING
    QMutexLocker locker(&sMutex);
#endif
    init(QByteArray(string, size));
}

AtomicString::AtomicString(const AtomicString& other)
{
#ifdef REENTRANT_ATOMICSTRING
    QMutexLocker locker(&sMutex);
#endif
    mData = other.mData;
    if (mData)
        ++mData->ref;
}

AtomicString::~AtomicString()
{
#ifdef REENTRANT_ATOMICSTRING
    QMutexLocker locker(&sMutex);
#endif
    if (mData && !--mData->ref) {
        sData.remove(mData->data);
        delete mData;
        mData = 0;
    }
}

int AtomicString::strcmp(const AtomicString& other) const
{
#ifdef REENTRANT_ATOMICSTRING
    QMutexLocker locker(&sMutex);
#endif
    if (!mData)
        return !other.mData ? 0 : -1;
    if (!other.mData)
        return 1;
    return qstrcmp(mData->data, other.mData->data);
}

AtomicString& AtomicString::operator=(const AtomicString& other)
{
#ifdef REENTRANT_ATOMICSTRING
    QMutexLocker locker(&sMutex);
#endif
    if (mData && !--mData->ref) {
        sData.remove(mData->data);
        delete mData;
    }
    mData = other.mData;
    if (mData)
        ++mData->ref;
    return *this;
}

static inline uint qHash(const AtomicString& string)
{
    return qHash(string.toByteArray());
}

class CursorKey
{
public:
    CursorKey()
        : kind(CXCursor_FirstInvalid), line(0), col(0), off(0), def(false)
    {}
    CursorKey(const CXCursor &cursor)
        : kind(clang_getCursorKind(cursor)), line(0), col(0), off(0), def(false)
    {
        if (!clang_isInvalid(kind)) {
            CXSourceLocation loc = clang_getCursorLocation(cursor);
            CXFile file;
            clang_getInstantiationLocation(loc, &file, &line, &col, &off);
            fileName = Path::resolved(eatString(clang_getFileName(file)));
            symbolName = eatString(clang_getCursorDisplayName(cursor));
            def = cursorDefinition(cursor);
        }
    }

    bool isValid() const
    {
        return !fileName.isEmpty() && !symbolName.isEmpty();
    }

    bool isNull() const
    {
        return !isValid();
    }

    bool isDefinition() const
    {
        return def;
    }

    bool operator<(const CursorKey &other) const
    {
        if (!isValid())
            return true;
        if (!other.isValid())
            return false;
        int ret = fileName.strcmp(other.fileName);
        if (ret < 0)
            return true;
        if (ret > 0)
            return false;
        if (off < other.off)
            return true;
        if (off > other.off)
            return false;
        ret = symbolName.strcmp(other.symbolName);
        if (ret < 0)
            return true;
        if (ret > 0)
            return false;
        return kind < other.kind;
    }

    bool operator==(const CursorKey &other) const
    {
        if (isNull())
            return other.isNull();
        return (kind == other.kind
                && off == other.off
                && fileName == other.fileName
                && symbolName == other.symbolName);
    }
    bool operator!=(const CursorKey &other) const
    {
        return !operator==(other);
    }

    QByteArray locationKey() const
    {
        QByteArray key(fileName.toByteArray());
        key += ":" + QByteArray::number(off);
        return key;
    }

    CXCursorKind kind;
    AtomicString fileName;
    AtomicString symbolName;
    unsigned line, col, off;
    bool def;
};

QDebug operator<<(QDebug d, const CursorKey& key)
{
    d.nospace() << eatString(clang_getCursorKindSpelling(key.kind)).constData() << ", "
                << (key.symbolName.isEmpty() ? "(no symbol)" : key.symbolName.toByteArray().constData()) << ", "
                << key.fileName.toByteArray().constData() << ':' << key.line << ':' << key.col;
    return d.space();
}

static inline uint qHash(const CursorKey &key)
{
    uint h = 0;
    if (!key.isNull()) {
#define HASHCHAR(ch)                            \
        h = (h << 4) + ch;                      \
        h ^= (h & 0xf0000000) >> 23;            \
        h &= 0x0fffffff;                        \
        ++h;

        QByteArray name = key.fileName.toByteArray();
        const char *ch = name.constData();
        Q_ASSERT(ch);
        while (*ch) {
            HASHCHAR(*ch);
            ++ch;
        }
        name = key.symbolName.toByteArray();
        ch = name.constData();
        Q_ASSERT(ch);
        while (*ch) {
            HASHCHAR(*ch);
            ++ch;
        }
        const uint16_t uints[] = { key.kind, key.off };
        for (int i=0; i<2; ++i) {
            ch = reinterpret_cast<const char*>(&uints[i]);
            for (int j=0; j<2; ++j) {
                HASHCHAR(*ch);
                ++ch;
            }
        }
    }
    return h;
}

struct CollectData
{
    CollectData() {}
    ~CollectData() { qDeleteAll(data); }

    struct Data
    {
        CursorKey cursor;
        QList<AtomicString> parentNames;
    };

    struct DataEntry
    {
        DataEntry() : hasDefinition(false) {}

        bool hasDefinition;
        Data cursor;
        Data reference;
        QSet<QByteArray> references;
    };

    QHash<QByteArray, DataEntry*> seen;
    QList<DataEntry*> data;
    struct Dependencies {
        Path file;
        GccArguments arguments;
        time_t lastModified;
        QHash<Path, time_t> dependencies;
    };
    QList<Dependencies> dependencies;
};

RBuild::RBuild(QObject *parent)
    : QObject(parent), mData(0)
{
}

RBuild::~RBuild()
{
    delete mData;
}

void RBuild::init(const Path& makefile)
{
    connect(&mSysInfo, SIGNAL(done()), this, SLOT(startParse()));
    mSysInfo.init();
    mMakefile = makefile;
}

void RBuild::startParse()
{
    connect(&mParser, SIGNAL(fileReady(const MakefileItem&)),
            this, SLOT(makefileFileReady(const MakefileItem&)));
    connect(&mParser, SIGNAL(done()), this, SLOT(makefileDone()));
    mParser.run(mMakefile);
}

void RBuild::makefileDone()
{
    fprintf(stderr, "Done parsing, now writing.\n");
    writeData(".rtags.db");
    fprintf(stderr, "All done.\n");

    qApp->quit();
}

void RBuild::makefileFileReady(const MakefileItem& file)
{
    compile(file.arguments);
}

static inline void writeDependencies(leveldb::DB* db, const leveldb::WriteOptions& opt,
                                     const Path &path, const GccArguments &args,
                                     time_t lastModified, const QHash<Path, time_t> &dependencies)
{
    QByteArray out;
    {
        QDataStream ds(&out, QIODevice::WriteOnly);
        ds << args << lastModified << dependencies;
    }
    const QByteArray p = "f:" + path;
    db->Put(opt, leveldb::Slice(p.constData(), p.size()),
            leveldb::Slice(out.constData(), out.size()));
}

static inline QByteArray cursorKeyToString(const CursorKey& key)
{
    // ### this should probably use snprintf
    QByteArray out = key.fileName.toByteArray();
    out.reserve(out.size() + 32);
    out.append(':');
    out.append(QByteArray::number(key.line));
    out.append(':');
    out.append(QByteArray::number(key.col));
    return out;
}

static inline QByteArray makeRefValue(const CollectData::DataEntry& entry)
{
    QByteArray out;
    {
        QDataStream ds(&out, QIODevice::WriteOnly);
        ds << cursorKeyToString(entry.reference.cursor) << entry.references;
        // qDebug() << "writing out value for" << cursorKeyToString(entry.cursor.cursor)
        //          << cursorKeyToString(entry.reference.cursor) << entry.references;
        // const QByteArray v =
        // ds << QByteArray::fromRawData(&v[0], v.size()) << convertRefs(entry.references);
    }
    return out;
}

static inline void writeDict(leveldb::DB* db, const leveldb::WriteOptions& opt, const QHash<AtomicString, QSet<AtomicString> >& dict)
{
    QHash<AtomicString, QSet<AtomicString> >::const_iterator it = dict.begin();
    const QHash<AtomicString, QSet<AtomicString> >::const_iterator end = dict.end();
    while (it != end) {
        // qDebug() << it.key().toByteArray();
        std::string locs;
        const QSet<AtomicString>& set = it.value();
        QSet<AtomicString>::const_iterator dit = set.begin();
        const QSet<AtomicString>::const_iterator dend = set.end();
        while (dit != dend) {
            locs += (*dit).toByteArray().constData();
            locs += '\0';
            ++dit;
        }
        db->Put(opt, ("d:" + it.key().toByteArray()).constData(), locs);
        ++it;
    }
}

static inline void collectDict(const CollectData::DataEntry& entry, QHash<AtomicString, QSet<AtomicString> >& dict)
{
    const CollectData::Data* datas[] = { &entry.cursor, &entry.reference };
    for (int i = 0; i < 2; ++i) {
        const CursorKey& key = datas[i]->cursor;
        if (!key.isValid())
            continue;

        // qDebug() << "dict" << key;

        const int& kind = key.kind;
        if ((kind >= CXCursor_FirstRef && kind <= CXCursor_LastRef)
            || (kind >= CXCursor_FirstExpr && kind <= CXCursor_LastExpr))
            continue;

        const QList<AtomicString>& parents = datas[i]->parentNames;

        const QByteArray name = key.symbolName.toByteArray();
        const QByteArray loc = cursorKeyToString(key);
        const AtomicString location(loc.constData(), loc.size());

        // add symbolname -> location
        dict[name].insert(location);

        switch (kind) {
        case CXCursor_Namespace:
        case CXCursor_ClassDecl:
        case CXCursor_StructDecl:
        case CXCursor_FieldDecl:
        case CXCursor_CXXMethod:
        case CXCursor_Constructor:
        case CXCursor_Destructor:
            break;
        default:
            continue;
        }

        // write namespace/class/struct::symbolname -> location
        QByteArray current;
        QListIterator<AtomicString> it(parents);
        it.toBack();
        while (it.hasPrevious()) {
            current += it.previous().toByteArray() + "::";
            dict[AtomicString(current + name)].insert(location);
        }
    }
}

static inline void writeEntry(leveldb::DB* db, const leveldb::WriteOptions& opt,
                              const CollectData::DataEntry& entry)
{
    const CursorKey& key = entry.cursor.cursor;
    if (!key.isValid()) {
        return;
    }

    QByteArray k = cursorKeyToString(key);
    QByteArray v = makeRefValue(entry);
    db->Put(opt, std::string(k.constData(), k.size()), std::string(v.constData(), v.size()));
    // qDebug() << "writing" << k << kindToString(key.kind) << entry.references.size()
    //          << v.size() << std::string(v.constData(), v.size()).size()
    //          << cursorKeyToString(val);
}

int remove_directory(const char *path)
{
    DIR *d = opendir(path);
    size_t path_len = strlen(path);
    int r = -1;

    if (d) {
        struct dirent *p;

        r = 0;

        while (!r && (p=readdir(d))) {
            int r2 = -1;
            char *buf;
            size_t len;

            /* Skip the names "." and ".." as we don't want to recurse on them. */
            if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..")) {
                continue;
            }

            len = path_len + strlen(p->d_name) + 2;
            buf = static_cast<char*>(malloc(len));

            if (buf) {
                struct stat statbuf;
                snprintf(buf, len, "%s/%s", path, p->d_name);
                if (!stat(buf, &statbuf)) {
                    if (S_ISDIR(statbuf.st_mode)) {
                        r2 = remove_directory(buf);
                    } else {
                        r2 = unlink(buf);
                    }
                }

                free(buf);
            }

            r = r2;
        }

        closedir(d);
    }

    if (!r) {
        r = rmdir(path);
    }

    return r;
}

void RBuild::writeData(const QByteArray& filename)
{
    if (!mData)
        return;

    leveldb::DB* db = 0;
    leveldb::Options dbOptions;
    leveldb::WriteOptions writeOptions;
    dbOptions.create_if_missing = true;

    Q_ASSERT(filename.endsWith(".rtags.db"));
    remove_directory(filename.constData());
    if (!leveldb::DB::Open(dbOptions, filename.constData(), &db).ok()) {
        return;
    }
    Q_ASSERT(db);

    QHash<AtomicString, QSet<AtomicString> > dict;
    foreach(CollectData::DataEntry* entry, mData->data) {
        const CursorKey key = entry->cursor.cursor;
        const CursorKey ref = entry->reference.cursor;
        if (key.kind == CXCursor_CXXMethod
            || key.kind == CXCursor_Constructor
            || key.kind == CXCursor_Destructor) {
            if (key != ref && !key.isDefinition()) {
                CollectData::DataEntry *def = mData->seen.value(ref.locationKey());
                Q_ASSERT(def && def != entry);
                def->reference = entry->cursor;
            }
            continue;
        }

        CollectData::DataEntry *r = mData->seen.value(entry->reference.cursor.locationKey());
        if (r == entry)
            continue;
        if (r) {
            r->references.insert(cursorKeyToString(entry->cursor.cursor));
            // qDebug() << "adding reference" << cursorKeyToString(entry->cursor.cursor)
            //          << "refers to" << cursorKeyToString(entry->reference.cursor);
        // } else {
        //     qDebug() << "can't find r" << cursorKeyToString(entry->reference.cursor)
        //              << "for" << cursorKeyToString(entry->cursor.cursor)
        //              << entry->cursor.cursor.isValid();
        }
    }

    foreach(const CollectData::DataEntry* entry, mData->data) {
        writeEntry(db, writeOptions, *entry);
        collectDict(*entry, dict);
    }
    writeDict(db, writeOptions, dict);

    foreach(const CollectData::Dependencies &dep, mData->dependencies) {
        writeDependencies(db, writeOptions, dep.file, dep.arguments,
                          dep.lastModified, dep.dependencies);
    }

    delete db;
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
    fprintf(out, "cursor name %s, kind %s%s, loc %s:%u:%u\n",
            clang_getCString(name), clang_getCString(kind),
            cursorDefinition(cursor) ? " def" : "",
            clang_getCString(filename), line, col);
    clang_disposeString(name);
    clang_disposeString(kind);
    clang_disposeString(filename);
}

static inline void addCursor(const CXCursor& cursor, const CursorKey& key, CollectData::Data* data)
{
    Q_ASSERT(key.isValid());
    data->cursor = key;
    CXCursor parent = cursor;
    for (;;) {
        parent = clang_getCursorSemanticParent(parent);
        CursorKey parentKey(parent);
        if (!parentKey.isValid())
            break;
        switch (parentKey.kind) {
        case CXCursor_StructDecl:
        case CXCursor_ClassDecl:
        case CXCursor_Namespace:
            Q_ASSERT(!parentKey.symbolName.isEmpty());
            data->parentNames.append(parentKey.symbolName);
            break;
        default:
            break;
        }
    }
}

//#define REFERENCEDEBUG

static inline bool useCursor(CXCursorKind kind)
{
    switch (kind) {
    case CXCursor_CallExpr:
        return false;
    default:
        break;
    }
    return true;
}

static inline CXCursor referencedCursor(const CXCursor& cursor)
{
#ifdef REFERENCEDEBUG
    CursorKey key(cursor);
    const bool dodebug = (key.fileName.toByteArray().endsWith("GccArguments.cpp") && key.line == 74);
#endif

    CXCursor ret;
    const CXCursorKind kind = clang_getCursorKind(cursor);

    if (!useCursor(kind)) {
#ifdef REFERENCEDEBUG
        if (dodebug) {
            printf("making ref, throwing out\n");
            debugCursor(stdout, cursor);
        }
#endif
        return clang_getNullCursor();
    }

    if (kind >= CXCursor_FirstRef && kind <= CXCursor_LastRef) {
#ifdef REFERENCEDEBUG
        if (dodebug) {
            printf("making ref, ref\n");
            debugCursor(stdout, cursor);
        }
#endif
        const CXType type = clang_getCursorType(cursor);
        if (type.kind == CXType_Invalid)
            ret = clang_getCursorReferenced(cursor);
        else
            ret = clang_getTypeDeclaration(type);
        if (isValidCursor(ret)) {
#ifdef REFERENCEDEBUG
            if (dodebug)
                debugCursor(stdout, ret);
#endif
        } else
            ret = cursor;
    } else if (kind >= CXCursor_FirstExpr && kind <= CXCursor_LastExpr) {
#ifdef REFERENCEDEBUG
        if (dodebug) {
            printf("making ref, expr\n");
            debugCursor(stdout, cursor);
        }
#endif
        ret = clang_getCursorReferenced(cursor);
#ifdef REFERENCEDEBUG
        if (dodebug)
            debugCursor(stdout, ret);
#endif
    } else if (kind >= CXCursor_FirstStmt && kind <= CXCursor_LastStmt) {
#ifdef REFERENCEDEBUG
        if (dodebug) {
            printf("making ref, stmt\n");
            debugCursor(stdout, cursor);
        }
#endif
        ret = clang_getCursorReferenced(cursor);
        if (isValidCursor(ret)) {
#ifdef REFERENCEDEBUG
            if (dodebug)
                debugCursor(stdout, ret);
#endif
        } else
            ret = cursor;
    } else if (kind >= CXCursor_FirstDecl && kind <= CXCursor_LastDecl) {
#ifdef REFERENCEDEBUG
        if (dodebug) {
            printf("making ref, decl\n");
            debugCursor(stdout, cursor);
        }
#endif
        ret = clang_getCursorReferenced(cursor);
#ifdef REFERENCEDEBUG
        if (dodebug)
            debugCursor(stdout, ret);
#endif
    } else if (kind == CXCursor_MacroDefinition || kind == CXCursor_MacroExpansion) {
#ifdef REFERENCEDEBUG
        if (dodebug) {
            printf("making ref, macro\n");
            debugCursor(stdout, cursor);
        }
#endif
        if (kind == CXCursor_MacroExpansion) {
            ret = clang_getCursorReferenced(cursor);
#ifdef REFERENCEDEBUG
            if (dodebug)
                debugCursor(stdout, ret);
#endif
        } else
            ret = cursor;
    } else {
#ifdef REFERENCEDEBUG
        if (!key.symbolName.isEmpty()) {
            if (kind != CXCursor_InclusionDirective) {
                fprintf(stderr, "unhandled reference %s\n", eatString(clang_getCursorKindSpelling(clang_getCursorKind(cursor))).constData());
                debugCursor(stderr, cursor);
            }
        }
#endif
        ret = clang_getNullCursor();
    }
    return ret;
}

static inline bool equalLocation(const CursorKey& key1, const CursorKey& key2)
{
    return (key1.off == key2.off && key1.fileName == key2.fileName);
}

// #define COLLECTDEBUG

static CXChildVisitResult collectSymbols(CXCursor cursor, CXCursor, CXClientData client_data)
{
    const CursorKey key(cursor);
    if (!key.isValid())
        return CXChildVisit_Recurse;
    // qDebug() << key << kindToString(key.kind);

    CollectData* data = reinterpret_cast<CollectData*>(client_data);

    CollectData::DataEntry* entry = 0;
    const QHash<QByteArray, CollectData::DataEntry*>::const_iterator it = data->seen.find(key.locationKey());
    static const bool verbose = getenv("VERBOSE");
    if (verbose) {
        debugCursor(stderr, cursor);
    }

#ifdef COLLECTDEBUG
    const bool dodebug = (key.fileName.toByteArray().endsWith("main.cpp") && key.line == 10 && key.col == 6);
#endif
    if (it != data->seen.end()) {
        entry = it.value();
        if (entry->hasDefinition) {
#ifdef COLLECTDEBUG
            if (dodebug) {
                fprintf(stdout, "already got a def\n");
                qDebug() << key;
            }
#endif
            return CXChildVisit_Recurse; // ### Continue?
        }
    } else {
        entry = new CollectData::DataEntry;
        data->seen[key.locationKey()] = entry;
        data->data.append(entry);
    }
    // if (!entry->refData)
    //     entry->refData = findRefData(data, cursor);

    if (key.kind == CXCursor_InclusionDirective) {
        CursorKey inclusion;
        inclusion.fileName = eatString(clang_getFileName(clang_getIncludedFile(cursor)));
        inclusion.symbolName = inclusion.fileName;
        inclusion.line = inclusion.col = 1;
        inclusion.off = 0;
        addCursor(cursor, key, &entry->cursor);
        addCursor(clang_getNullCursor(), inclusion, &entry->reference);
        entry->hasDefinition = true;
        return CXChildVisit_Continue;
    }

    const CXCursor definition = clang_getCursorDefinition(cursor);
#ifdef COLLECTDEBUG
    if (dodebug) {
        debugCursor(stdout, cursor);
        debugCursor(stdout, definition);
        fprintf(stdout, "(%d %d)\n", !isValidCursor(definition), equalLocation(key, CursorKey(definition)));
    }
#endif
    if (!cursorDefinition(definition) || equalLocation(key, CursorKey(definition))) {
        if (entry->reference.cursor.isNull() || entry->reference.cursor == entry->cursor.cursor) {
            const CXCursor reference = clang_getCursorReferenced(cursor);
            const CursorKey referenceKey(reference);
            if (referenceKey.isValid()/* && referenceKey != key*/) {
#ifdef COLLECTDEBUG
                if (dodebug) {
                    debugCursor(stdout, reference);
                    fprintf(stdout, "ref %p\n", entry);
                }
#endif
                addCursor(cursor, key, &entry->cursor);
                addCursor(reference, referenceKey, &entry->reference);
            }
        }
    } else {
        if (cursorDefinitionFor(definition, cursor))
            entry->hasDefinition = true;
        addCursor(cursor, key, &entry->cursor);
        const CursorKey definitionKey(definition);
        if (definitionKey.isValid()) {
            addCursor(definition, definitionKey, &entry->reference);
        }
#ifdef COLLECTDEBUG
        if (dodebug) {
            debugCursor(stdout, definition);
            fprintf(stdout, "def %p\n", entry);
            qDebug() << entry->reference.cursor;
        }
#endif
    }

    return CXChildVisit_Recurse;
}

static inline void getInclusions(CXFile includedFile,
                                 CXSourceLocation* inclusionStack,
                                 unsigned includeLen,
                                 CXClientData userData)
{
    CollectData::Dependencies *deps = reinterpret_cast<CollectData::Dependencies*>(userData);
    CXString str = clang_getFileName(includedFile);
    Path p = Path::resolved(clang_getCString(str));
    deps->dependencies[p] = p.lastModified();
    clang_disposeString(str);
    // printf("Included file %s\n", eatString(clang_getFileName(includedFile)).constData());
    for (unsigned i=0; i<includeLen - 1; ++i) {
        CXFile f;
        unsigned l, c, o;
        clang_getSpellingLocation(inclusionStack[i], &f, &l, &c, &o);
        continue;
        str = clang_getFileName(f);
        p = Path::resolved(clang_getCString(str));
        deps->dependencies[p] = p.lastModified();
        clang_disposeString(str);
        printf("    %d %s\n", i, eatString(clang_getFileName(f)).constData());
    }
}


void RBuild::compile(const GccArguments& arguments)
{
    CXIndex idx = clang_createIndex(0, 0);
    foreach(const Path& input, arguments.input()) {
        /*if (!input.endsWith("/RBuild.cpp")
          && !input.endsWith("/main.cpp")) {
          printf("skipping %s\n", input.constData());
          continue;
          }*/
        fprintf(stderr, "parsing %s\n", input.constData());

        const bool verbose = (getenv("VERBOSE") != 0);

        QList<QByteArray> arglist;
        arglist += arguments.arguments("-I");
        arglist += arguments.arguments("-D");
        arglist += mSysInfo.systemIncludes();
        // ### not very efficient
        QVector<const char*> argvector;
        foreach(const QByteArray& arg, arglist) {
            argvector.append(arg.constData());
            if (verbose)
                fprintf(stderr, "%s ", arg.constData());
        }
        if (verbose)
            fprintf(stderr, "\n");

        CXTranslationUnit unit = clang_parseTranslationUnit(idx, input.constData(),
                                                            argvector.constData(), argvector.size(),
                                                            0, 0,
                                                            CXTranslationUnit_DetailedPreprocessingRecord);
        if (!unit) {
            fprintf(stderr, "Unable to parse unit for %s\n", input.constData());
            continue;
        }

        const unsigned int numDiags = clang_getNumDiagnostics(unit);
        for (unsigned int i = 0; i < numDiags; ++i) {
            CXDiagnostic diag = clang_getDiagnostic(unit, i);
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
            clang_disposeDiagnostic(diag);
        }

        if (!mData)
            mData = new CollectData;

        CXCursor unitCursor = clang_getTranslationUnitCursor(unit);
        clang_visitChildren(unitCursor, collectSymbols, mData);
        CollectData::Dependencies deps = { input, arguments, input.lastModified(),
                                           QHash<Path, time_t>() };
        mData->dependencies.append(deps);
        clang_getInclusions(unit, getInclusions, &mData->dependencies.last());
        clang_disposeTranslationUnit(unit);
    }
    clang_disposeIndex(idx);
}

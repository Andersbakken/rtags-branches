#ifndef Database_h
#define Database_h

#ifdef USE_KYOTO
#include <kcdb.h>
namespace kyotocabinet {
class IndexDB;
}
#endif

#ifdef USE_LEVELDB
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#endif

#include <QtCore>
#include <string>

struct Slice {
    Slice(const std::string &str);
    Slice(const QByteArray &d);
    Slice(const char *d = 0, int s = -1);
    const char *data() const;
    int size() const;
    void clear();
    bool operator==(const Slice &other) const;
    bool operator!=(const Slice &other) const;
private:
#ifdef USE_LEVELDB
    Slice(const leveldb::Slice &slice);
    leveldb::Slice mSlice;
#elif USE_KYOTO
    const char* mSlicePtr;
    int mSliceSize;
#endif
    friend class Database;
    friend class Iterator;
    friend class Batch;
};

static inline QDebug operator<<(QDebug dbg, const Slice &slice)
{
    dbg << "Slice(" << std::string(slice.data(), slice.size()).c_str() << ")";
    return dbg;
}

template <typename T> QByteArray encode(const T &t)
{
    QByteArray out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds << t;
    return out;
}

template <typename T> T decode(const Slice &slice)
{
    const QByteArray ba = QByteArray::fromRawData(slice.data(), slice.size());
    QDataStream ds(ba);
    T t;
    ds >> t;
    return t;
}

class Iterator
{
#ifdef USE_LEVELDB
    Iterator(leveldb::Iterator *iterator);
#elif USE_KYOTO
    Iterator(kyotocabinet::BasicDB::Cursor *cursor);
#endif
public:
    ~Iterator();
    void seekToFirst();
    void seekToLast();
    bool isValid() const;
    void next();
    void previous();
    Slice key() const;
    Slice value() const;
    void seek(const Slice &slice);
    template <typename T> T value() const { return decode<T>(value()); }
private:
#ifdef USE_LEVELDB
    leveldb::Iterator *mIterator;
#elif USE_KYOTO
    kyotocabinet::BasicDB::Cursor *mCursor;
    bool mIsValid;
#endif
    friend class Database;
};

class LocationComparator;
class Database
{
public:
    Database(const char *path, int cacheSizeMB, bool locationKeys);
    ~Database();
    void lockForRead() { mLock.lockForRead(); }
    void lockForWrite() { mLock.lockForWrite(); }
    void unlock() { mLock.unlock(); }
    bool isOpened() const;
    void close();
    QByteArray openError() const;
    std::string rawValue(const Slice &key, bool *ok = 0) const;
    template <typename T> T value(const Slice &key, bool *ok = 0) {
        const std::string val = rawValue(key, ok);
        if (!val.empty())
            return decode<T>(val);
        return T();
    }
    int setRawValue(const Slice &key, const Slice &value);
    template <typename T> int setValue(const Slice &key, const T &t) { return setRawValue(key, encode(t)); }
    bool contains(const Slice &key) const;
    void remove(const Slice &key);
    Iterator *createIterator() const;
    void flush();
private:
    QReadWriteLock mLock;
#ifdef USE_LEVELDB
    leveldb::DB *mDB;
    const leveldb::WriteOptions mWriteOptions;
    LocationComparator *mLocationComparator;
#elif USE_KYOTO
    kyotocabinet::IndexDB* mDB;
#endif
    QByteArray mOpenError;
    friend class Batch;
};

class ScopedDB
{
public:
    enum LockType {
        Read,
        Write
    };
    ScopedDB(Database *db, LockType lockType)
        : mData(new Data(db, lockType))
    {
    }
    Database *operator->() { return mData->db; }
    operator Database *() { return mData->db; }
private:
    class Data : public QSharedData
    {
    public:
        Data(Database *database, LockType lockType)
            : db(database)
        {
            if (db) {
                (lockType == Read ? db->lockForRead() : db->lockForWrite());
            }
        }

        ~Data()
        {
            if (db)
                db->unlock();
        }

        Database *db;
    };
    QSharedDataPointer<Data> mData;
};

struct Batch {
    enum { BatchThreshold = 1024 * 1024 };
    Batch(Database *d);
    ~Batch();
    int flush();
    template <typename T> int add(const Slice &key, const T &t)
    {
        const QByteArray encoded = encode<T>(t);
        return writeEncoded(key, Slice(encoded));
    }

    void remove(const Slice &key);
    int size() const { return mSize; }
    int total() const { return mTotal; }
private:
    int writeEncoded(const Slice &key, const Slice &data);
    Database *mDB;
#ifdef USE_LEVELDB
    leveldb::WriteBatch mBatch;
#endif
    int mSize, mTotal;
};


#endif

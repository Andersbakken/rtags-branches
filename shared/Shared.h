#ifndef Shared_h
#define Shared_h

#include <QByteArray>
#include <clang-c/Index.h>
#include <Path.h>
#include <Log.h>

static inline int digits(int len)
{
    int ret = 1;
    while (len >= 10) {
        len /= 10;
        ++ret;
    }
    return ret;
}
static inline QByteArray makeLocation(const Path &path, int line, int col)
{
    QByteArray ret(path.size() + 2 + digits(line) + digits(col), 0);
    snprintf(ret.data(), ret.size(), "%s:%d:%d", path.constData(), line, col);
    return ret;
}

static inline QByteArray eatString(CXString str)
{
    const QByteArray ret(clang_getCString(str));
    clang_disposeString(str);
    return ret;
}

static inline QByteArray join(const QList<QByteArray> &list, const QByteArray &sep = QByteArray())
{
    QByteArray ret;
    int size = qMax(0, list.size() - 1) * sep.size();
    foreach(const QByteArray &l, list) {
        size += l.size();
    }
    ret.reserve(size);
    foreach(const QByteArray &l, list) {
        ret.append(l);
    }
    return ret;
}

static inline QByteArray cursorToString(CXCursor cursor)
{
    QByteArray ret = eatString(clang_getCursorKindSpelling(clang_getCursorKind(cursor)));
    const QByteArray name = eatString(clang_getCursorSpelling(cursor));
    if (!name.isEmpty())
        ret += " " + name;

    CXFile file;
    unsigned line, col;
    clang_getInstantiationLocation(clang_getCursorLocation(cursor), &file, &line, &col, 0);
    const QByteArray fileName = eatString(clang_getFileName(file));
    if (!fileName.isEmpty()) {
        ret += " " + fileName + ':' + QByteArray::number(line) + ':' +  QByteArray::number(col);
    }
    return ret;
}

struct Location {
    Location() : line(0), column(0) {}

    Path path;
    int line, column;
};

static inline bool makeLocation(const QByteArray &arg, Location *loc,
                                QByteArray *resolvedLocation = 0, const Path &cwd = Path())
{
    Q_ASSERT(!arg.isEmpty());
    int colon = arg.lastIndexOf(':');
    if (colon == arg.size() - 1)
        colon = arg.lastIndexOf(':', colon - 1);
    if (colon == -1) {
        return false;
    }
    const unsigned column = atoi(arg.constData() + colon + 1);
    if (!column)
        return false;
    colon = arg.lastIndexOf(':', colon - 1);
    if (colon == -1)
        return false;
    const unsigned line = atoi(arg.constData() + colon + 1);
    if (!line)
        return false;
    const Path path = Path::resolved(arg.left(colon), cwd);
    if (path.isEmpty())
        return false;
    if (resolvedLocation)
        *resolvedLocation = path + arg.mid(colon);
    if (loc) {
        loc->line = line;
        loc->column = column;
        loc->path = path;
    }
    return true;
}


#endif

#ifndef VisitThread_h
#define VisitThread_h

#include <QtCore>
#include "Path.h"
#include "Location.h"

struct Node;
struct Match
{
    Match(uint nodeTypes)
        : nodeTypes(nodeTypes)
    {}
    virtual ~Match() {}

    enum MatchResult {
        Finish,
        Recurse,
        Skip
    };
    // path means e.g. namespace::class:: (including trailing double colons)
    virtual MatchResult match(const QByteArray &path, const Node *node) = 0;

    const uint nodeTypes;
};

struct CursorNode;
class VisitThread : public QThread
{
    Q_OBJECT
public:
    VisitThread();
    ~VisitThread();
    void lookup(Match *match);
    void printTree();
    QSet<Path> files() const;
    void abort();
    void lockMutex() { mMutex.lock(); }
    void unlockMutex() { mMutex.unlock(); }
    Node *nodeForLocation(const Location &loc) const;
    bool save(const QByteArray &file);
    void timerEvent(QTimerEvent *e)
    {
        if (e->timerId() == timer.timerId()) {
            save("/tmp/balle2");
            timer.stop();
            emit done();
        }
    }
signals:
    void done();
public slots:
    void invalidate(const QSet<Path> &paths);
    void onFileParsed(const Path &path, void *unit);
private:
    struct PendingReference {
        CursorNode *node;
        Location location;
    };


    void buildTree(Node *node, CursorNode *c, QHash<QByteArray, PendingReference> &references);
    void addReference(CursorNode *c, const QByteArray &id, const Location &location);

    Node *mRoot;
    mutable QMutex mMutex;
    QMap<QByteArray, Node*> mNodes;
    bool mQuitting;
    qint32 mLongestId;
    QBasicTimer timer;
};

#endif

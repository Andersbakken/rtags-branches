#include "ClangCompletionJob.h"
#include <rct/Connection.h>

static inline String eatString(CXString string)
{
    const String ret = clang_getCString(string);
    clang_disposeString(string);
    return ret;
}

ClangCompletionJob::ClangCompletionJob(const shared_ptr<UnitCache::Unit> &unit,
                                       const Location &location,
                                       const String &unsaved, Connection *conn)
    : mUnit(unit), mLocation(location), mUnsaved(unsaved), mConnection(conn)
{
    assert(conn);
    conn->destroyed().connect(this, &ClangCompletionJob::onConnectionDestroyed);
}

void ClangCompletionJob::onConnectionDestroyed(Connection*)
{
    MutexLocker lock(&mMutex);
    mConnection = 0;
}

static inline bool isPartOfSymbol(char ch)
{
    return isalnum(ch) || ch == '_';
}

struct CompletionNode
{
    String completion, signature;
    int priority, distance;
};

static int compareCompletionNode(const void *left, const void *right)
{
    const CompletionNode *l = reinterpret_cast<const CompletionNode*>(left);
    const CompletionNode *r = reinterpret_cast<const CompletionNode*>(right);
    if (l->priority != r->priority)
        return l->priority < r->priority ? -1 : 1;
    if ((l->distance != -1) != (r->distance != -1))
        return l->distance != -1 ? -1 : 1;
    if (l->distance != r->distance)
        return l->distance > r->distance ? -1 : 1;
    return strcmp(l->completion.constData(), r->completion.constData());
}

struct Token
{
    Token(const char *bytes = 0, int size = 0)
        : data(bytes), length(size)
    {}

    inline bool operator==(const Token &other) const
    {
        return length == other.length && !strncmp(data, other.data, length);
    }
    inline bool operator<(const Token &other) const
    {
        const int minLength = std::min(length, other.length);
        if (minLength) {
            const int cmp = strncmp(data, other.data, minLength);
            if (cmp < 0)
                return true;
            if (cmp > 0)
                return false;
        }

        return length > other.length;
    }

    const char *data;
    int length;
};

static inline bool symbolChar(char ch)
{
    switch (ch) {
    case '_':
    case '~':
        return true;
    default:
        break;
    }
    return isalnum(ch);
}

static inline void addToken(const char *data, int pos, int len, Map<Token, int> &tokens)
{
    int &val = tokens[Token(data + pos, len)];
    if (!val)
        val = pos;
}

static inline void tokenize(const char *data, int size, Map<Token, int> &tokens)
{
    int tokenEnd = -1;
    for (int i=size - 1; i>=0; --i) {
        if (symbolChar(data[i])) {
            if (tokenEnd == -1)
                tokenEnd = i;
        } else if (tokenEnd != -1) {
            addToken(data, i + 1, tokenEnd - i, tokens);
            tokenEnd = -1;
        }
    }
    if (tokenEnd != -1)
        addToken(data, 0, tokenEnd + 1, tokens);
}


void ClangCompletionJob::run()
{
    const Path path = mLocation.path();
    error() << "Running up some completions here" << path << mUnsaved;
    CXUnsavedFile unsavedFile = { mUnsaved.isEmpty() ? 0 : path.constData(),
                                  mUnsaved.isEmpty() ? 0 : mUnsaved.constData(),
                                  static_cast<unsigned long>(mUnsaved.size()) };

    CXCodeCompleteResults *results = clang_codeCompleteAt(mUnit->unit, path.constData(), mLocation.line(), mLocation.column(),
                                                          &unsavedFile, mUnsaved.isEmpty() ? 0 : 1,
                                                          CXCodeComplete_IncludeMacros | CXCodeComplete_IncludeCodePatterns);

    if (results) {
        CompletionNode *nodes = new CompletionNode[results->NumResults];
        int nodeCount = 0;
        Map<Token, int> tokens;
        if (!mUnsaved.isEmpty()) {
            tokenize(mUnsaved.constData(), mUnsaved.size(), tokens);
            // for (Map<Token, int>::const_iterator it = tokens.begin(); it != tokens.end(); ++it) {
            //     error() << String(it->first.data, it->first.length) << it->second;
            // }
        }
        for (unsigned i = 0; i < results->NumResults; ++i) {
            const CXCursorKind kind = results->Results[i].CursorKind;
            if (kind == CXCursor_Destructor)
                continue;

            const CXCompletionString &string = results->Results[i].CompletionString;
            const CXAvailabilityKind availabilityKind = clang_getCompletionAvailability(string);
            if (availabilityKind != CXAvailability_Available)
                continue;

            const int priority = clang_getCompletionPriority(string);
            if (priority >= 75)
                continue;

            CompletionNode &node = nodes[nodeCount];
            node.priority = priority;
            node.signature.reserve(256);
            const int chunkCount = clang_getNumCompletionChunks(string);
            bool ok = true;
            for (int j=0; j<chunkCount; ++j) {
                const CXCompletionChunkKind chunkKind = clang_getCompletionChunkKind(string, j);
                if (chunkKind == CXCompletionChunk_TypedText) {
                    node.completion = eatString(clang_getCompletionChunkText(string, j));
                    if (node.completion.size() > 8 && node.completion.startsWith("operator") && !isPartOfSymbol(node.completion.at(8))) {
                        ok = false;
                        break;
                    }
                    node.signature.append(node.completion);
                } else {
                    node.signature.append(eatString(clang_getCompletionChunkText(string, j)));
                    if (chunkKind == CXCompletionChunk_ResultType)
                        node.signature.append(' ');
                }
            }

            if (ok) {
                int ws = node.completion.size() - 1;
                while (ws >= 0 && isspace(node.completion.at(ws)))
                    --ws;
                if (ws >= 0) {
                    node.completion.truncate(ws + 1);
                    node.signature.replace("\n", "");
                    node.distance = tokens.value(Token(node.completion.constData(), node.completion.size()), -1);
                    ++nodeCount;
                    continue;
                }
            }
            node.completion.clear();
            node.signature.clear();
        }
        if (nodeCount) {
            qsort(nodes, nodeCount, sizeof(CompletionNode), compareCompletionNode);
            MutexLocker lock(&mMutex);
            if (mConnection) {
                mConnection->write<128>("`%s %s", nodes[0].completion.constData(), nodes[0].signature.constData());
                for (int i=1; i<nodeCount; ++i) {
                    mConnection->write<128>("%s %s", nodes[i].completion.constData(), nodes[i].signature.constData());
                }
            }
        }

        delete[] nodes;

        //processDiagnostics(results);

        clang_disposeCodeCompleteResults(results);
    } else {
        error() << "No results";
    }
    UnitCache::put(path, mUnit);
}

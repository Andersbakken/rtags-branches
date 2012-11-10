#ifndef CursorInfo_h
#define CursorInfo_h

#include "ByteArray.h"
#include "Location.h"
#include "Path.h"
#include "Log.h"
#include "List.h"
#include <clang-c/Index.h>

class CursorInfo;
typedef Map<Location, CursorInfo> SymbolMap;
class CursorInfo
{
public:
    CursorInfo()
        : symbolLength(0), kind(CXCursor_FirstInvalid), type(CXType_Invalid), isDefinition(false), start(-1), end(-1)
    {}

    static int cursorRank(CXCursorKind kind);
    void clear()
    {
        start = end = -1;
        symbolLength = 0;
        kind = CXCursor_FirstInvalid;
        type = CXType_Invalid;
        isDefinition = false;
        targets.clear();
        references.clear();
        symbolName.clear();
        container.clear();
    }

    bool dirty(const Set<uint32_t> &dirty)
    {
        bool changed = false;
        Set<Location> *locations[] = { &targets, &references };
        for (int i=0; i<2; ++i) {
            Set<Location> &l = *locations[i];
            Set<Location>::iterator it = l.begin();
            while (it != l.end()) {
                if (dirty.contains(it->fileId())) {
                    changed = true;
                    l.erase(it++);
                } else {
                    ++it;
                }
            }
        }
        return changed;
    }

    bool isValid() const
    {
        return !isEmpty();
    }

    bool isNull() const
    {
        return isEmpty();
    }

    CursorInfo bestTarget(const SymbolMap &map, Location *loc = 0) const;
    SymbolMap targetInfos(const SymbolMap &map) const;
    SymbolMap referenceInfos(const SymbolMap &map) const;
    SymbolMap callers(const Location &loc, const SymbolMap &map) const;
    SymbolMap allReferences(const Location &loc, const SymbolMap &map) const;
    SymbolMap virtuals(const Location &loc, const SymbolMap &map) const;
    SymbolMap declarationAndDefinition(const Location &loc, const SymbolMap &map) const;

    bool isClass() const
    {
        switch (kind) {
        case CXCursor_ClassDecl:
        case CXCursor_ClassTemplate:
        case CXCursor_StructDecl:
            return true;
        default:
            break;
        }
        return false;
    }

    bool isEmpty() const
    {
        return !symbolLength && targets.isEmpty() && references.isEmpty() && start == -1 && end == -1 && !container.isValid();
    }

    bool unite(const CursorInfo &other)
    {
        bool changed = false;
        if (targets.isEmpty() && !other.targets.isEmpty()) {
            targets = other.targets;
            changed = true;
        } else if (!other.targets.isEmpty()) {
            int count = 0;
            targets.unite(other.targets, &count);
            if (count)
                changed = true;
        }

        if (end == -1 && start == -1 && other.start != -1 && other.end != -1) {
            start = other.start;
            end = other.end;
            changed = true;
        }

        if (!symbolLength && other.symbolLength) {
            symbolLength = other.symbolLength;
            kind = other.kind;
            isDefinition = other.isDefinition;
            symbolName = other.symbolName;
            changed = true;
        }
        const int oldSize = references.size();
        if (!oldSize) {
            references = other.references;
            if (!references.isEmpty())
                changed = true;
        } else {
            int inserted = 0;
            references.unite(other.references, &inserted);
            if (inserted)
                changed = true;
        }
        if (container.isNull() && other.container.isValid()) {
            container = other.container;
            changed = true;
        }

        return changed;
    }

    ByteArray toString(unsigned keyFlags = 0) const;

    uint16_t symbolLength; // this is just the symbol name length e.g. foo => 3
    ByteArray symbolName; // this is fully qualified Foobar::Barfoo::foo
    CXCursorKind kind;
    CXTypeKind type;
    bool isDefinition;
    Set<Location> targets, references;
    int start, end;
    Location container;
};


template <> inline Serializer &operator<<(Serializer &s, const CursorInfo &t)
{
    s << t.symbolLength << t.symbolName << static_cast<int>(t.kind)
      << static_cast<int>(t.type) << t.isDefinition << t.targets << t.references << t.start << t.end
      << t.container;
    return s;
}

template <> inline Deserializer &operator>>(Deserializer &s, CursorInfo &t)
{
    int kind, type;
    s >> t.symbolLength >> t.symbolName >> kind >> type
      >> t.isDefinition >> t.targets >> t.references >> t.start >> t.end
      >> t.container;
    t.kind = static_cast<CXCursorKind>(kind);
    t.type = static_cast<CXTypeKind>(type);
    return s;
}

inline Log operator<<(Log log, const CursorInfo &info)
{
    log << info.toString();
    return log;
}

#endif

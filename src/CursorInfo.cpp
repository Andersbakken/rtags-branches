/* This file is part of RTags.

RTags is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

RTags is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with RTags.  If not, see <http://www.gnu.org/licenses/>. */


#include "CursorInfo.h"
#include "RTagsClang.h"

String CursorInfo::kindSpelling(uint16_t kind)
{
    return RTags::eatString(clang_getCursorKindSpelling(static_cast<CXCursorKind>(kind)));
}

String CursorInfo::toString(unsigned cursorInfoFlags, unsigned keyFlags) const
{
    String ret = String::format<1024>("SymbolName: %s\n"
                                      "Kind: %s\n"
                                      "Type: %s\n" // type
                                      "SymbolLength: %u\n"
                                      "%s" // range
                                      "%s" // enumValue
                                      "%s", // definition
                                      symbolName.constData(),
                                      kindSpelling().constData(),
                                      RTags::eatString(clang_getTypeKindSpelling(type)).constData(),
                                      symbolLength,
                                      startLine != -1 ? String::format<32>("Range: %d:%d-%d:%d\n", startLine, startColumn, endLine, endColumn).constData() : "",
#if CINDEX_VERSION_MINOR > 1
                                      kind == CXCursor_EnumConstantDecl ? String::format<32>("Enum Value: %lld\n", enumValue).constData() :
#endif
                                      "",
                                      isDefinition() ? "Definition\n" : "");

    if (!targets.isEmpty() && !(cursorInfoFlags & IgnoreTargets)) {
        ret.append("Targets:\n");
        for (auto tit = targets.begin(); tit != targets.end(); ++tit) {
            const Location &l = *tit;
            ret.append(String::format<128>("    %s\n", l.key(keyFlags).constData()));
        }
    }

    if (!references.isEmpty() && !(cursorInfoFlags & IgnoreReferences)) {
        ret.append("References:\n");
        for (auto rit = references.begin(); rit != references.end(); ++rit) {
            const Location &l = *rit;
            ret.append(String::format<128>("    %s\n", l.key(keyFlags).constData()));
        }
    }
    return ret;
}

int CursorInfo::targetRank(const std::shared_ptr<CursorInfo> &target) const
{
    switch (target->kind) {
    case CXCursor_Constructor: // this one should be more than class/struct decl
        return 1;
    case CXCursor_ClassDecl:
    case CXCursor_StructDecl:
    case CXCursor_ClassTemplate:
        return 0;
    case CXCursor_FieldDecl:
    case CXCursor_VarDecl:
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
        // functiondecl and cxx method must be more than cxx
        // CXCursor_FunctionTemplate. Since constructors for templatatized
        // objects seem to come out as function templates
        return 3;
    case CXCursor_MacroDefinition:
        return 4;
    default:
        return 2;
    }
}

String CursorInfo::displayName() const
{
    switch (kind) {
    case CXCursor_FunctionTemplate:
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Destructor:
    case CXCursor_Constructor: {
        const int end = symbolName.indexOf('(');
        if (end != -1)
            return symbolName.left(end);
        break; }
    case CXCursor_FieldDecl: {
        int colon = symbolName.indexOf(':');
        if (colon != -1) {
            const int end = colon + 2;
            while (colon > 0 && RTags::isSymbol(symbolName.at(colon - 1)))
                --colon;
            return symbolName.left(colon + 1) + symbolName.mid(end);
        }
        break; }
    default:
        break;
    }
    return symbolName;
}

std::shared_ptr<CursorInfo> CursorInfo::copy() const
{
    return std::shared_ptr<CursorInfo>(std::make_shared<CursorInfo>(*this));
}

bool CursorInfo::isReference(unsigned int kind)
{
    return RTags::isReference(kind);
}

std::unique_ptr<SymbolMap::Iterator> CursorInfo::findCursorInfo(const std::shared_ptr<SymbolMap> &map, const Location &location)
{
    std::unique_ptr<DB<Location, std::shared_ptr<CursorInfo> >::Iterator> it = map->lower_bound(location);
    if (it->isValid()) {
        if (it->key() == location) {
            return it;
        }
        error() << "Found a thing for" << location << it->key();
        it->prev();
    } else {
        it->seekToEnd();
        error() << "Seeking to end";
    }

    if (!it->isValid()) {
        return it;
    }

    error() << "Now looking at" << it->key();

    if (it->key().fileId() == location.fileId() && location.line() == it->key().line()) {
        const int off = location.column() - it->key().column();
        if (it->value()->symbolLength > off) {
            return it;
        }
    }
    return map->createIterator(map->Invalid);
}

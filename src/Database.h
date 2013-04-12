#ifndef Database_h
#define Database_h

#include <Location.h>
#include <rct/Serializer.h>
#include <rct/String.h>
#include <rct/List.h>

class Connection;
class SourceInformation;
class Database
{
public:
    Database() {}
    virtual ~Database() {}

    virtual bool save(Serializer &) const { return false; }
    virtual bool load(Deserializer &) const { return false; }

    class Cursor
    {
    public:
        Cursor() : kind(Invalid) {}
        enum Kind {
            Invalid,
            MemberFunctionDefinition,
            MemberFunctionDeclaration,
            MethodDefinition,
            MethodDeclaration,
            Class,
            Namespace,
            Struct,
            Variable,
            Parameter,
            Field,
            Enum,
            EnumValue,
            Macro,
            Reference
        };

        static const char *kindToString(Kind kind);
        enum CursorInfoFlag { // these are combined with the keyflags from Location
            IncludeTargets = 0x10,
            IncludeReferences = 0x20
        };
        String toString(unsigned flags) const;
        bool isDefinition() const;

        Location location;
        String symbolName;
        Location target;
        Set<Location> references;
        Kind kind;
    };
    virtual Cursor cursor(const Location &location) const = 0;
    virtual void status(const String &query, Connection *conn) const = 0;
    virtual void dump(const SourceInformation &sourceInformation, Connection *conn) const = 0;
    virtual int index(const SourceInformation &sourceInformation) = 0;
    virtual Set<uint32_t> dependencies(uint32_t fileId) const = 0;
    virtual void sync() = 0;
    virtual List<String> matchSymbolNames(const String &string) const = 0;
    virtual Set<Cursor> findCursors(const String &string) const = 0;
};
#endif
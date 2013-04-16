#ifndef ClientMessage_h
#define ClientMessage_h

#include <rct/Message.h>
#include <rct/String.h>

class ClientMessage : public Message
{
public:
    enum {
        CompletionId = ResponseId + 1,
        QueryId,
        ProjectId,
        CreateOutputId
    };

    ClientMessage(int id) : Message(id) {}

    inline void init(int argc, const char **argv)
    {
        mRaw.reserve(256);
        for (int i=0; i<argc; ++i) {
            if (i > 0)
                mRaw.append(' ');
            const bool space = strchr(argv[i], ' ');
            if (space)
                mRaw.append('"');
            mRaw.append(argv[i]);
            if (space)
                mRaw.append('"');
        }
    }
    inline void init(int argc, char **argv)
    {
        init(argc, const_cast<const char**>(argv));
    }

    inline String raw() const
    {
        return mRaw;
    }

    void setProjects(const List<String> &projects) { mProjects = projects; }
    List<String> projects() const { return mProjects; }
protected:
    String mRaw;
    List<String> mProjects;
};

#endif

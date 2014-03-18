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

#include "CompileMessage.h"
#include <rct/Serializer.h>

CompileMessage::CompileMessage(const Path &cwd, const String &args, bool escape)
    : RTagsMessage(MessageId), mWorkingDirectory(cwd), mArgs(args), mEscape(escape)
{
}

void CompileMessage::encode(Serializer &serializer) const
{
    serializer << mRaw << mWorkingDirectory << mArgs << mProjects << mEscape;
}

void CompileMessage::decode(Deserializer &deserializer)
{
    deserializer >> mRaw >> mWorkingDirectory >> mArgs >> mProjects >> mEscape;
}

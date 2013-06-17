#include "JSParser.h"
#include <v8.h>
#include <rct/Mutex.h>
#include <rct/MutexLocker.h>

#define toCString(str) *v8::String::Utf8Value(str)

static v8::Handle<v8::String> toJSON(v8::Handle<v8::Value> obj, bool pretty)
{
    v8::HandleScope scope;

    v8::Handle<v8::Context> context = v8::Context::GetCurrent();
    v8::Handle<v8::Object> global = context->Global();

    v8::Handle<v8::Object> JSON = global->Get(v8::String::New("JSON"))->ToObject();
    v8::Handle<v8::Function> JSON_stringify = v8::Handle<v8::Function>::Cast(JSON->Get(v8::String::New("stringify")));

    v8::Handle<v8::Value> args[3];
    args[0] = obj;
    args[1] = v8::Null();
    args[2] = v8::String::New("    ");

    v8::Handle<v8::Value> ret = JSON_stringify->Call(JSON, pretty ? 3 : 2, args);
    if (!ret.IsEmpty())
        return scope.Close(ret)->ToString();
    return v8::String::New("can't json this");
}

Log operator<<(Log log, v8::Handle<v8::String> string)
{
    if (!string.IsEmpty())
        log << toCString(string);
    return log;
}

Log operator<<(Log log, v8::Handle<v8::Value> value)
{
    if (!value.IsEmpty())
        log << toCString(toJSON(value, true));
    return log;
}

template <typename T>
static v8::Handle<T> get(v8::Handle<v8::Object> object, v8::Handle<v8::String> property)
{
    if (object.IsEmpty() || !object->IsObject())
        return v8::Handle<T>();
    v8::HandleScope scope;
    v8::Handle<v8::Value> prop(object->Get(property));
    if (prop.IsEmpty() || prop->IsNull() || prop->IsUndefined()) {
        return scope.Close(v8::Handle<T>());
    } else {
        return scope.Close(v8::Handle<T>::Cast(prop));
    }
}

template <typename T>
static v8::Handle<T> get(v8::Handle<v8::Object> object, const char *property)
{
    return get<T>(object, v8::String::New(property));
}

template <typename T>
static v8::Persistent<T> getPersistent(v8::Handle<v8::Object> object, v8::Handle<v8::String> property)
{
    v8::Persistent<T> prop(get<T>(object, property));
    return prop;
}

template <typename T>
static v8::Persistent<T> getPersistent(v8::Handle<v8::Object> object, const char *property)
{
    return getPersistent<T>(object, v8::String::New(property));
}

template <typename T>
static v8::Handle<T> get(v8::Handle<v8::Array> object, int index)
{
    if (object.IsEmpty() || !object->IsArray())
        return v8::Handle<T>();
    v8::HandleScope scope;
    v8::Handle<v8::Value> prop = object->Get(index);
    if (prop.IsEmpty() || prop->IsNull() || prop->IsUndefined()) {
        return scope.Close(v8::Handle<T>());
    } else {
        return scope.Close(v8::Handle<T>::Cast(prop));
    }
}

static inline bool operator==(v8::Handle<v8::String> l, const char *r)
{
    // error() << "comparing" << (l.IsEmpty() ? "empty" : toCString(l)) << r;
    return l.IsEmpty() ? (!r || !strlen(r)) : !strcmp(toCString(l), r);
}

static inline bool operator==(const char *l, v8::Handle<v8::String> r)
{
    return operator==(r, l);
}

static inline bool operator!=(const char *l, v8::Handle<v8::String> r)
{
    return !operator==(r, l);
}

static inline bool operator!=(v8::Handle<v8::String> l, const char *r)
{
    return !operator==(l, r);
}

JSParser::JSParser()
    : mIsolate(0)
{}

JSParser::~JSParser()
{
    {
        const v8::Isolate::Scope isolateScope(mIsolate);
#ifdef V8_DISPOSE_HAS_ISOLATE
        if (!mParse.IsEmpty())
            mParse.Dispose(mIsolate);
        if (!mContext.IsEmpty())
            mContext.Dispose(mIsolate);
#else
        if (!mParse.IsEmpty())
            mParse.Dispose();
        if (!mContext.IsEmpty())
            mContext.Dispose();
#endif
    }
    mIsolate->Dispose();
}

// v8::Handle<v8::Value> Print(const v8::Arguments& args);
v8::Handle<v8::Value> log(const v8::Arguments &args)
{
    Log out(Error);
    const int length = args.Length();
    for (int i=0; i<length; ++i) {
        out << args[i];
    }
    return v8::Undefined();
}

// v8::Handle<v8::Value> Print(const v8::Arguments& args);
v8::Handle<v8::Value> jsDefine(const v8::Arguments &args)
{
    error() << "got define called" << args[0];
    Log out(Error);
    const int length = args.Length();
    for (int i=0; i<length; ++i) {
        out << args[i];
    }
    return v8::Undefined();
}


bool JSParser::init()
{
    mIsolate = v8::Isolate::New();
    const v8::Isolate::Scope isolateScope(mIsolate);
    v8::HandleScope handleScope;
    v8::Handle<v8::ObjectTemplate> globalObjectTemplate = v8::ObjectTemplate::New();
    globalObjectTemplate->Set(v8::String::New("log"), v8::FunctionTemplate::New(log));

    mContext = v8::Context::New(0, globalObjectTemplate);
    v8::Context::Scope scope(mContext);
    assert(!mContext.IsEmpty());

    const String esprimaSrcString = Path(ESPRIMA_JS).readAll();
    v8::Handle<v8::String> esprimaSrc = v8::String::New(esprimaSrcString.constData(), esprimaSrcString.size());

    v8::TryCatch tryCatch;
    v8::Handle<v8::Script> script = v8::Script::Compile(esprimaSrc);
    if (tryCatch.HasCaught() || script.IsEmpty() || !tryCatch.Message().IsEmpty()) {
        v8::Handle<v8::Message> message = tryCatch.Message();
        v8::String::Utf8Value msg(message->Get());
        printf("%s:%d:%d: esprima error: %s {%d-%d}\n", ESPRIMA_JS, message->GetLineNumber(),
               message->GetStartColumn(), *msg, message->GetStartPosition(), message->GetEndPosition());
        printf("[%s:%d]: return false;\n", __func__, __LINE__); fflush(stdout);
        return false;
    }
    script->Run();

    v8::Handle<v8::Object> global = mContext->Global();
    mParse = getPersistent<v8::Function>(global, "indexFile");

    return !mParse.IsEmpty() && mParse->IsFunction();
}

bool JSParser::parse(const Path &path, const String &contents, SymbolMap *symbols, SymbolNameMap *symbolNames,
                     String *errors, String *json)
{
    const v8::Isolate::Scope isolateScope(mIsolate);
    // mFileId = Location::insertFile(path);
    v8::HandleScope handleScope;
    v8::Context::Scope scope(mContext);
    String tmp;
    if (contents.isEmpty())
        tmp = path.readAll();
    const String &c = contents.isEmpty() ? tmp : contents;
    if (c.isEmpty()) {
        printf("[%s] %s:%d: if (c.isEmpty()) { [after]\n", __func__, __FILE__, __LINE__);
        return false;
    }
    v8::Handle<v8::Value> args[2];
    args[0] = v8::String::New(c.constData(), c.size());
    v8::Handle<v8::Object> options = v8::Object::New();
    options->Set(v8::String::New("range"), v8::Boolean::New(true));
    options->Set(v8::String::New("tolerant"), v8::Boolean::New(true));
    args[1] = v8::String::New(path.constData(), path.size());
    assert(!mParse.IsEmpty() && mParse->IsFunction());
    assert(!args[0].IsEmpty() && args[0]->IsString());
    assert(!args[1].IsEmpty() && args[1]->IsString());
    v8::Handle<v8::Value> result = mParse->Call(mContext->Global(), 2, args);
    error() << result;
    return true;
}


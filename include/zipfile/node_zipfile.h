#ifndef INCLUDE_ZIPFILE_NODE_ZIPFILE_H_
#define INCLUDE_ZIPFILE_NODE_ZIPFILE_H_

extern "C" {
#include <zlib.h>
#include <errno.h>
#include <unzip.h>
}

#include <v8.h>
#include <node.h>
#include <node_object_wrap.h>

using namespace v8;
using namespace node;

class ZipFile: public node::ObjectWrap {
 public:
    static Persistent<FunctionTemplate> constructor;
    static void Initialize(Handle<Object> target);
    static Handle<Value> New(const Arguments &args);
    static Handle<Value> destroy(const Arguments &args);

    static Handle<Value> get_prop(Local<String> property,
                                  const AccessorInfo& info);

    // Sync
    static Handle<Value> readFileSync(const Arguments& args);

    // Async
    static Handle<Value> readFile(const Arguments& args);

    explicit ZipFile(const char *file_name);

 private:
    ~ZipFile();
    char *file_name_;
    int count_;
    unzFile archive_;
    Persistent<Array> names_;

    static void Work_ReadFile(uv_work_t* req);
    static void Work_AfterReadFile(uv_work_t* req);
};

#endif  // INCLUDE_ZIPFILE_NODE_ZIPFILE_H_

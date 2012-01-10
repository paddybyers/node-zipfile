#include "zipfile/node_zipfile.h"

#include <node_buffer.h>
#include <string.h>

#define TOSTR(obj) (*String::Utf8Value((obj)->ToString()))

void ZipFile::Initialize(Handle<Object> target) {
    HandleScope scope;
    Local<FunctionTemplate> constructor = FunctionTemplate::New(ZipFile::New);
    constructor->InstanceTemplate()->SetInternalFieldCount(1);
    constructor->SetClassName(String::NewSymbol("ZipFile"));

    // functions
    NODE_SET_PROTOTYPE_METHOD(constructor, "readFileSync", readFileSync);
    NODE_SET_PROTOTYPE_METHOD(constructor, "readFile", readFile);
    NODE_SET_PROTOTYPE_METHOD(constructor, "destroy", destroy);

    // properties
    constructor->InstanceTemplate()->SetAccessor(String::NewSymbol("count"), get_prop);
    constructor->InstanceTemplate()->SetAccessor(String::NewSymbol("names"), get_prop);

    target->Set(String::NewSymbol("ZipFile"), constructor->GetFunction());
}

ZipFile::ZipFile(const char *file_name)
    : ObjectWrap(),
      archive_() {
  size_t len = strlen(file_name) + 1;
  file_name_ = new char[len];
  memcpy(file_name_, file_name, len);
}

ZipFile::~ZipFile() {
    if(archive_)
      unzClose(archive_);
    delete[] file_name_;
}

Handle<Value> ZipFile::New(const Arguments& args) {
    HandleScope scope;

    if (!args.IsConstructCall())
        return ThrowException(String::New("Cannot call constructor as function, you need to use 'new' keyword"));

    if (args.Length() != 1 || !args[0]->IsString())
        return ThrowException(Exception::TypeError(
                                  String::New("first argument must be a path to a zipfile")));

    const char *input_file = TOSTR(args[0]);
    unzFile za;
    if ((za = unzOpen(input_file)) == NULL) {
        char errBuf[1024];
        snprintf(errBuf, 1024, "Unable to open file: %s", input_file);
        return ThrowException(Exception::Error(String::New(errBuf)));
    }

    ZipFile* zf = new ZipFile(input_file);
  
    unz_global_info zInfo;
    if (unzGetGlobalInfo(za, &zInfo) != UNZ_OK) {
      char errBuf[1024];
      snprintf(errBuf, 1024, "Unable to process file: %s", input_file);
      return ThrowException(Exception::Error(String::New(errBuf)));
    }

    zf->count_ = zInfo.number_entry;
    zf->names_ = Persistent<Array>::New(Array::New(zf->count_));
    int i = 0;
    if(zf->count_ > 0) {
      unz_file_info st;
      char entryName[1024];
      int unzRes = unzGoToFirstFile(za);
      while (unzRes == UNZ_OK) {
          unzGetCurrentFileInfo(za, &st, entryName, 1024, 0, 0, 0, 0);
          if(st.size_filename > 1023) {
            entryName[1023] = 0;
            char errBuf[1024];
            snprintf(errBuf, 1024, "Unsupported path length for entry: %s", entryName);
            return ThrowException(Exception::Error(String::New(errBuf)));
          }
          zf->names_->Set(i++, String::New(entryName));
          unzRes = unzGoToNextFile(za);
      }
      if(unzRes != UNZ_END_OF_LIST_OF_FILE) {
        char errBuf[1024];
        snprintf(errBuf, 1024, "Unable to process file: %s", input_file);
        return ThrowException(Exception::Error(String::New(errBuf)));
      }
    }

    zf->archive_ = za;
    zf->Wrap(args.This());
    return args.This();
}

Handle<Value> ZipFile::destroy(const Arguments& args) {
  ZipFile* zf = ObjectWrap::Unwrap<ZipFile>(args.This());
  if(zf->archive_) {
    unzClose(zf->archive_);
    zf->archive_ = 0;
  }
  return Undefined();
}

Handle<Value> ZipFile::get_prop(Local<String> property,
                                const AccessorInfo& info) {
    HandleScope scope;
    ZipFile* zf = ObjectWrap::Unwrap<ZipFile>(info.This());
    const char *a = TOSTR(property);
    if (!strcmp(a, "count")) {
        return scope.Close(Integer::New(zf->count_));
    }
    if (!strcmp(a, "names")) {
        return scope.Close(zf->names_);
    }
    return Undefined();
}

Handle<Value> ZipFile::readFileSync(const Arguments& args) {
    HandleScope scope;

    if (args.Length() != 1 || !args[0]->IsString())
        return ThrowException(Exception::TypeError(
                                  String::New("first argument must be a file name inside the zip")));

    const char *name = TOSTR(args[0]);

    ZipFile* zf = ObjectWrap::Unwrap<ZipFile>(args.This());
    if(!zf->archive_)
      return ThrowException(Exception::Error(String::New("Zip archive has been destroyed")));

    int unzRes = unzLocateFile(zf->archive_, name, 1);
    if (unzRes == UNZ_END_OF_LIST_OF_FILE) {
        char errBuf[1024];
        snprintf(errBuf, 1024, "No file found by the name of: %s", name);
        return ThrowException(Exception::Error(String::New(errBuf)));
    }

    if ((unzRes=unzOpenCurrentFile(zf->archive_)) != UNZ_OK) {
        char errBuf[1024];
        snprintf(errBuf, 1024, "Unable to open file: %s: archive error: %d", name, unzRes);
        return ThrowException(Exception::Error(String::New(errBuf)));
    }

    unz_file_info st;
    unzGetCurrentFileInfo(zf->archive_, &st, 0, 0, 0, 0, 0, 0);

    char *data = new char[st.uncompressed_size];
    if(!data) {
      char errBuf[1024];
      snprintf(errBuf, 1024, "Unable to allocate buffer to read file: %s", name);
      return ThrowException(Exception::Error(String::New(errBuf)));
    }
    
    unzRes = unzReadCurrentFile(zf->archive_, data, st.uncompressed_size);
    unzCloseCurrentFile(zf->archive_);
    if (unzRes < 0) {
      char errBuf[1024];
      snprintf(errBuf, 1024, "Unable to read file: %s: archive error: %d", name, unzRes);
      return ThrowException(Exception::Error(String::New(errBuf)));
    }

    node::Buffer *retbuf = Buffer::New(data, st.uncompressed_size);
    delete[] data;
    return scope.Close(retbuf->handle_);
}

typedef struct {
    uv_work_t request;
    ZipFile* zf;
    unzFile za;
    char *name;
    bool error;
    char *error_name;
    char *data;
    long length;
    Persistent<Function> cb;
} closure_t;


Handle<Value> ZipFile::readFile(const Arguments& args) {
    HandleScope scope;

    if (args.Length() < 2)
        return ThrowException(Exception::TypeError(
                                  String::New("requires two arguments, the name of a file and a callback")));

    // first arg must be name
    if (!args[0]->IsString())
        return ThrowException(Exception::TypeError(
                                  String::New("first argument must be a file name inside the zip")));

    // last arg must be function callback
    if (!args[args.Length()-1]->IsFunction())
        return ThrowException(Exception::TypeError(
                                  String::New("last argument must be a callback function")));

    const char *name = TOSTR(args[0]);

    ZipFile* zf = ObjectWrap::Unwrap<ZipFile>(args.This());
    if(!zf->archive_)
      return ThrowException(Exception::Error(String::New("Zip archive has been destroyed")));

    closure_t *closure = new closure_t();
    closure->request.data = closure;

    // minizip is not threadsafe so we cannot use the zf->archive_
    // instead we open a new zip archive for each thread
    unzFile za;
    int err;
    if ((za = unzOpen(zf->file_name_)) == NULL) {
      char errBuf[1024];
      snprintf(errBuf, 1024, "Unable to open file: %s", zf->file_name_);
      return ThrowException(Exception::Error(String::New(errBuf)));
    }

    closure->zf = zf;
    closure->za = za;
    closure->error = false;
    closure->error_name = 0;
    closure->data = 0;

    size_t nameLen = strlen(name) + 1;
    closure->name = new char[nameLen];
    if(closure->name) {
      return ThrowException(Exception::Error(String::New("Out of memory")));
    }
    closure->cb = Persistent<Function>::New(Handle<Function>::Cast(args[args.Length()-1]));
    uv_queue_work(uv_default_loop(), &closure->request, Work_ReadFile, Work_AfterReadFile);
    uv_ref(uv_default_loop());
    zf->Ref();
    return Undefined();
}


void ZipFile::Work_ReadFile(uv_work_t* req) {
    closure_t *closure = static_cast<closure_t *>(req->data);

    int unzRes = unzLocateFile(closure->za, closure->name, 1);
    if (unzRes == UNZ_END_OF_LIST_OF_FILE) {
      char errBuf[1024];
      snprintf(errBuf, 1024, "No file found by the name of: %s", closure->name);
      size_t errLen = strlen(errBuf) + 1;
      closure->error_name = new char[errLen];
      memcpy(closure->error_name, errBuf, errLen);
      closure->error = true;
      return;
    }
  
    if ((unzRes=unzOpenCurrentFile(closure->za)) != UNZ_OK) {
      char errBuf[1024];
      snprintf(errBuf, 1024, "Unable to read file: %s: archive error: %d", closure->name, unzRes);
      size_t errLen = strlen(errBuf) + 1;
      closure->error_name = new char[errLen];
      memcpy(closure->error_name, errBuf, errLen);
      closure->error = true;
      return;
    }
  
    unz_file_info st;
    unzGetCurrentFileInfo(closure->za, &st, 0, 0, 0, 0, 0, 0);

    closure->length = st.uncompressed_size;
    closure->data = new char[closure->length];
    if(!closure->data) {
      char errBuf[1024];
      snprintf(errBuf, 1024, "Unable to allocate buffer to read file: %s", closure->name);
      size_t errLen = strlen(errBuf) + 1;
      closure->error_name = new char[errLen];
      memcpy(closure->error_name, errBuf, errLen);
      closure->error = true;
      return;
    }
  
    unzRes = unzReadCurrentFile(closure->za, closure->data, closure->length);
    unzCloseCurrentFile(closure->za);
    if (unzRes < 0) {
      char errBuf[1024];
      snprintf(errBuf, 1024, "Unable to read file: %s: archive error: %d", closure->name, unzRes);
      size_t errLen = strlen(errBuf) + 1;
      closure->error_name = new char[errLen];
      memcpy(closure->error_name, errBuf, errLen);
      closure->error = true;
      return;
    }
}

void ZipFile::Work_AfterReadFile(uv_work_t* req) {
    HandleScope scope;

    closure_t *closure = static_cast<closure_t *>(req->data);

    TryCatch try_catch;

    if (closure->error) {
        Local<Value> argv[1] = { Exception::Error(String::New(closure->error_name)) };
        closure->cb->Call(Context::GetCurrent()->Global(), 1, argv);
        delete closure->error_name;
    } else {
        node::Buffer *retbuf = Buffer::New(closure->data, closure->length);
        Local<Value> argv[2] = { Local<Value>::New(Null()), Local<Value>::New(retbuf->handle_) };
        closure->cb->Call(Context::GetCurrent()->Global(), 2, argv);
    }

    if (try_catch.HasCaught()) {
        FatalException(try_catch);
    }

    closure->zf->Unref();
    uv_unref(uv_default_loop());
    closure->cb.Dispose();
    delete[] closure->name;
    unzClose(closure->za);
    delete closure;
}

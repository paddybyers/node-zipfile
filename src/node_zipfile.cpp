#include "zipfile/node_zipfile.h"

#include <node_buffer.h>

// std
#include <sstream>
#include <string>
#include <algorithm>

#define TOSTR(obj) (*String::Utf8Value((obj)->ToString()))

Persistent<FunctionTemplate> ZipFile::constructor;

void ZipFile::Initialize(Handle<Object> target) {
    HandleScope scope;
    constructor = Persistent<FunctionTemplate>::New(FunctionTemplate::New(ZipFile::New));
    constructor->InstanceTemplate()->SetInternalFieldCount(1);
    constructor->SetClassName(String::NewSymbol("ZipFile"));

    // functions
    NODE_SET_PROTOTYPE_METHOD(constructor, "readFileSync", readFileSync);
    NODE_SET_PROTOTYPE_METHOD(constructor, "readFile", readFile);

    // properties
    constructor->InstanceTemplate()->SetAccessor(String::NewSymbol("count"), get_prop);
    constructor->InstanceTemplate()->SetAccessor(String::NewSymbol("names"), get_prop);

    target->Set(String::NewSymbol("ZipFile"), constructor->GetFunction());
}

ZipFile::ZipFile(std::string const& file_name)
    : ObjectWrap(),
      file_name_(file_name),
      archive_() {}

ZipFile::~ZipFile() {
    if(archive_)
      unzClose(archive_);
}

Handle<Value> ZipFile::New(const Arguments& args) {
    HandleScope scope;

    if (!args.IsConstructCall())
        return ThrowException(String::New("Cannot call constructor as function, you need to use 'new' keyword"));

    if (args.Length() != 1 || !args[0]->IsString())
        return ThrowException(Exception::TypeError(
                                  String::New("first argument must be a path to a zipfile")));

    std::string input_file = TOSTR(args[0]);
    unzFile za;
    if ((za = unzOpen(input_file.c_str())) == NULL) {
        std::stringstream s;
        s << "Unable to open file: " << input_file << "\n";
        return ThrowException(Exception::Error(
                                  String::New(s.str().c_str())));
    }

    ZipFile* zf = new ZipFile(input_file);
  
    unz_global_info zInfo;
    if (unzGetGlobalInfo(za, &zInfo) != UNZ_OK) {
      std::stringstream s;
      s << "Unable to process file: " << input_file << "\n";
      return ThrowException(Exception::Error(
                                           String::New(s.str().c_str())));
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
            std::stringstream s;
            s << "Unsupported path length for entry: " << entryName << "\n";
            return ThrowException(Exception::Error(
                                                   String::New(s.str().c_str())));
          }
          zf->names_->Set(i++, String::New(entryName));
          unzRes = unzGoToNextFile(za);
      }
      if(unzRes != UNZ_END_OF_LIST_OF_FILE) {
        std::stringstream s;
        s << "Unable to process file: " << input_file << "\n";
        return ThrowException(Exception::Error(
                                               String::New(s.str().c_str())));
      }
    }

    zf->archive_ = za;
    zf->Wrap(args.This());
    return args.This();
}

Handle<Value> ZipFile::Destroy(const Arguments& args) {
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
    std::string a = TOSTR(property);
    if (a == "count") {
        return scope.Close(Integer::New(zf->count_));
    }
    if (a == "names") {
        return scope.Close(zf->names_);
    }
    return Undefined();
}

Handle<Value> ZipFile::readFileSync(const Arguments& args) {
    HandleScope scope;

    if (args.Length() != 1 || !args[0]->IsString())
        return ThrowException(Exception::TypeError(
                                  String::New("first argument must be a file name inside the zip")));

    std::string name = TOSTR(args[0]);

    ZipFile* zf = ObjectWrap::Unwrap<ZipFile>(args.This());
    if(!zf->archive_)
      return ThrowException(Exception::Error(String::New("Zip archive has been destroyed")));

    int unzRes = unzLocateFile(zf->archive_, name.c_str(), 1);
    if (unzRes == UNZ_END_OF_LIST_OF_FILE) {
        std::stringstream s;
        s << "No file found by the name of: '" << name << "\n";
        return ThrowException(Exception::Error(String::New(s.str().c_str())));
    }

    if ((unzRes=unzOpenCurrentFile(zf->archive_)) != UNZ_OK) {
        std::stringstream s;
        s << "cannot open file " << name << ": archive error: " << unzRes << "\n";
        return ThrowException(Exception::Error(String::New(s.str().c_str())));
    }

    unz_file_info st;
    unzGetCurrentFileInfo(zf->archive_, &st, 0, 0, 0, 0, 0, 0);

    char *data = new char[st.uncompressed_size];
    if(!data) {
      std::stringstream s;
      s << "Unable to allocate buffer to read file: " << name << "\n";
      return ThrowException(Exception::Error(String::New(s.str().c_str())));
    }
    
    unzRes = unzReadCurrentFile(zf->archive_, data, st.uncompressed_size);
    unzCloseCurrentFile(zf->archive_);
    if (unzRes < 0) {
      std::stringstream s;
      s << "error reading file " << name << ": archive error: " << unzRes << "\n";
      return ThrowException(Exception::Error(String::New(s.str().c_str())));
    }

    node::Buffer *retbuf = Buffer::New(data, st.uncompressed_size);
    delete[] data;
    return scope.Close(retbuf->handle_);
}

typedef struct {
    uv_work_t request;
    ZipFile* zf;
    unzFile za;
    std::string name;
    bool error;
    std::string error_name;
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

    std::string name = TOSTR(args[0]);

    ZipFile* zf = ObjectWrap::Unwrap<ZipFile>(args.This());
    if(!zf->archive_)
      return ThrowException(Exception::Error(String::New("Zip archive has been destroyed")));

    closure_t *closure = new closure_t();
    closure->request.data = closure;

    // minizip is not threadsafe so we cannot use the zf->archive_
    // instead we open a new zip archive for each thread
    unzFile za;
    int err;
    if ((za = unzOpen(zf->file_name_.c_str())) == NULL) {
        std::stringstream s;
        s << "cannot open file: " << zf->file_name_ << "\n";
        return ThrowException(Exception::Error(
                                  String::New(s.str().c_str())));
    }

    closure->zf = zf;
    closure->za = za;
    closure->error = false;
    closure->name = name;
    closure->data = 0;
    closure->cb = Persistent<Function>::New(Handle<Function>::Cast(args[args.Length()-1]));
    uv_queue_work(uv_default_loop(), &closure->request, Work_ReadFile, Work_AfterReadFile);
    uv_ref(uv_default_loop());
    zf->Ref();
    return Undefined();
}


void ZipFile::Work_ReadFile(uv_work_t* req) {
    closure_t *closure = static_cast<closure_t *>(req->data);

    int unzRes = unzLocateFile(closure->za, closure->name.c_str(), 1);
    if (unzRes == UNZ_END_OF_LIST_OF_FILE) {
      std::stringstream s;
      s << "No file found by the name of: '" << closure->name << "\n";
      closure->error = true;
      closure->error_name = s.str();
      return;
    }
  
    if ((unzRes=unzOpenCurrentFile(closure->za)) != UNZ_OK) {
      std::stringstream s;
      s << "cannot open file " << closure->name << ": archive error: " << unzRes << "\n";
      closure->error = true;
      closure->error_name = s.str();
      return;
    }
  
    unz_file_info st;
    unzGetCurrentFileInfo(closure->za, &st, 0, 0, 0, 0, 0, 0);

    closure->length = st.uncompressed_size;
    closure->data = new char[closure->length];
    if(!closure->data) {
      std::stringstream s;
      s << "Unable to allocate buffer to read file: " << closure->name << "\n";
      closure->error = true;
      closure->error_name = s.str();
      return;
    }
  
    unzRes = unzReadCurrentFile(closure->za, closure->data, closure->length);
    unzCloseCurrentFile(closure->za);
    if (unzRes < 0) {
      std::stringstream s;
      s << "error reading file " << closure->name << ": archive error: " << unzRes << "\n";
      closure->error = true;
      closure->error_name = s.str();
      return;
    }
}

void ZipFile::Work_AfterReadFile(uv_work_t* req) {
    HandleScope scope;

    closure_t *closure = static_cast<closure_t *>(req->data);

    TryCatch try_catch;

    if (closure->error) {
        Local<Value> argv[1] = { Exception::Error(String::New(closure->error_name.c_str())) };
        closure->cb->Call(Context::GetCurrent()->Global(), 1, argv);
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
    unzClose(closure->za);
    delete closure;
}

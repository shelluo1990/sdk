// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/isolate_reload.h"

#include "vm/code_generator.h"
#include "vm/dart_api_impl.h"
#include "vm/isolate.h"
#include "vm/log.h"
#include "vm/object.h"
#include "vm/object_store.h"
#include "vm/safepoint.h"
#include "vm/service_event.h"
#include "vm/stack_frame.h"
#include "vm/thread.h"

namespace dart {

DEFINE_FLAG(bool, trace_reload, true, "Trace isolate reloading");

#define I (isolate())
#define Z (thread->zone())

IsolateReloadContext::IsolateReloadContext(Isolate* isolate, bool test_mode)
    : isolate_(isolate),
      test_mode_(test_mode),
      has_error_(false),
      saved_num_cids_(-1),
      script_uri_(String::null()),
      error_(Error::null()),
      saved_root_library_(Library::null()),
      saved_libraries_(GrowableObjectArray::null()) {
}


IsolateReloadContext::~IsolateReloadContext() {
}


void IsolateReloadContext::ReportError(const Error& error) {
  has_error_ = true;
  error_ = error.raw();
  if (FLAG_trace_reload) {
    THR_Print("ISO-RELOAD: Error: %s\n", error.ToErrorCString());
  }
  ServiceEvent service_event(Isolate::Current(), ServiceEvent::kIsolateReload);
  service_event.set_reload_error(&error);
  Service::HandleEvent(&service_event);
}


void IsolateReloadContext::ReportSuccess() {
  ServiceEvent service_event(Isolate::Current(), ServiceEvent::kIsolateReload);
  Service::HandleEvent(&service_event);
}

void IsolateReloadContext::StartReload() {
  Thread* thread = Thread::Current();
  const Library& root_lib = Library::Handle(object_store()->root_library());
  const String& root_lib_url = String::Handle(root_lib.url());

  CheckpointClassTable();

  // Clear the compile time constants cache.
  // TODO(turnidge): Can this be moved into Commit?
  I->object_store()->set_compile_time_constants(Object::null_array());

  // Block class finalization attempts when calling into the library
  // tag handler.
  I->BlockClassFinalization();
  Object& result = Object::Handle(thread->zone());
  {
    TransitionVMToNative transition(thread);
    Api::Scope api_scope(thread);
    Dart_Handle retval =
        (I->library_tag_handler())(Dart_kScriptTag,
                                Api::NewHandle(thread, Library::null()),
                                Api::NewHandle(thread, root_lib_url.raw()));
    result = Api::UnwrapHandle(retval);
  }
  I->UnblockClassFinalization();
  if (result.IsError()) {
    ReportError(Error::Cast(result));
  }
}


void IsolateReloadContext::FinishReload() {
  BuildClassIdMap();
  BuildLibraryIdMap();
  fprintf(stderr, "---- DONE FINALIZING\n");
  I->class_table()->PrintNonDartClasses();

  if (ValidateReload()) {
    CommitClassTable();
  } else {
    RollbackClassTable();
  }
}


void IsolateReloadContext::CheckpointClassTable() {
  fprintf(stderr, "---- CHECKPOINTING CLASS TABLE\n");
  I->class_table()->PrintNonDartClasses();

  saved_num_cids_ = I->class_table()->NumCids();

  // Build a new libraries array which only has the dart-scheme libs.
  const GrowableObjectArray& libs =
      GrowableObjectArray::Handle(object_store()->libraries());
  const GrowableObjectArray& new_libs = GrowableObjectArray::Handle(
      GrowableObjectArray::New(Heap::kOld));
  Library& tmp_lib = Library::Handle();
  String& tmp_url = String::Handle();
  for (intptr_t i = 0; i < libs.Length(); i++) {
    tmp_lib ^= libs.At(i);
    tmp_url ^= tmp_lib.url();
    if (tmp_lib.is_dart_scheme()) {
      new_libs.Add(tmp_lib, Heap::kOld);
    }
  }
  set_saved_libraries(libs);
  object_store()->set_libraries(new_libs);

  // Reset the root library to null.
  const Library& root_lib =
      Library::Handle(object_store()->root_library());
  set_saved_root_library(root_lib);
  object_store()->set_root_library(Library::Handle());
}


void IsolateReloadContext::RollbackClassTable() {
  fprintf(stderr, "---- ROLLING BACK CLASS TABLE\n");
  Thread* thread = Thread::Current();
  ASSERT(saved_num_cids_ > 0);
  I->class_table()->DropNewClasses(saved_num_cids_);
  I->class_table()->PrintNonDartClasses();

  GrowableObjectArray& saved_libs = GrowableObjectArray::Handle(
      Z, saved_libraries());
  if (!saved_libs.IsNull()) {
    object_store()->set_libraries(saved_libs);
  }
  set_saved_libraries(GrowableObjectArray::Handle());

  Library& saved_root_lib = Library::Handle(Z, saved_root_library());
  if (!saved_root_lib.IsNull()) {
    object_store()->set_root_library(saved_root_lib);
  }
  set_saved_root_library(Library::Handle());
}


void IsolateReloadContext::CommitClassTable() {
  Thread* thread = Thread::Current();
  fprintf(stderr, "---- COMMITTING CLASS TABLE\n");

  Class& cls = Class::Handle();
  Class& new_cls = Class::Handle();
  for (intptr_t i = 0; i < class_mappings_.length(); i++) {
    const Remapping& mapping = class_mappings_.At(i);
    cls = I->class_table()->At(mapping.old_id);
    new_cls = I->class_table()->At(mapping.new_id);
    cls.Reload(new_cls);
  }

  // Remove unneeded classes from the class table.
  for (intptr_t i = 0; i < class_mappings_.length(); i++) {
    // Remove new_cls from the class table.
    const Remapping& mapping = class_mappings_.At(i);
    I->class_table()->ClearClassAt(mapping.new_id);
  }

  GrowableObjectArray& libs = GrowableObjectArray::Handle(
      Z, saved_libraries());
  GrowableObjectArray& new_libs = GrowableObjectArray::Handle(
      Z, object_store()->libraries());

  Library& lib = Library::Handle();
  Library& new_lib = Library::Handle();
  for (intptr_t i = 0; i < lib_mappings_.length(); i++) {
    const Remapping& mapping = lib_mappings_.At(i);
    lib = Library::RawCast(libs.At(mapping.old_id));
    new_lib = Library::RawCast(new_libs.At(mapping.new_id));
    lib.Reload(new_lib);
  }

  I->class_table()->CompactNewClasses(saved_num_cids_);

  // NO TWO.
  if (!libs.IsNull()) {
    object_store()->set_libraries(libs);
  }
  set_saved_libraries(GrowableObjectArray::Handle());

  Library& saved_root_lib = Library::Handle(Z, saved_root_library());
  if (!saved_root_lib.IsNull()) {
    object_store()->set_root_library(saved_root_lib);
  }
  set_saved_root_library(Library::Handle());

  InvalidateWorld();
}


bool IsolateReloadContext::ValidateReload() {
  Class& cls = Class::Handle();
  Class& new_cls = Class::Handle();
  for (intptr_t i = 0; i < class_mappings_.length(); i++) {
    const Remapping& mapping = class_mappings_.At(i);
    cls = I->class_table()->At(mapping.old_id);
    new_cls = I->class_table()->At(mapping.new_id);
    if (!cls.CanReload(new_cls)) {
      return false;
    }
  }

  return true;
}


intptr_t IsolateReloadContext::FindReplacementClassId(const Class& cls) {
  const intptr_t upper_cid_bound = I->class_table()->NumCids();

  const Library& lib = Library::Handle(cls.library());
  const String& url = String::Handle(lib.IsNull() ? String::null() : lib.url());
  const String& name = String::Handle(cls.Name());

  Library& new_lib = Library::Handle();
  String& new_url = String::Handle();
  String& new_name = String::Handle();
  Class& new_class = Class::Handle();

  for (intptr_t i = saved_num_cids_; i < upper_cid_bound; i++) {
    if (!I->class_table()->HasValidClassAt(i)) {
      continue;
    }
    new_class = I->class_table()->At(i);
    new_name = new_class.Name();
    if (!name.Equals(new_name)) {
      continue;
    }
    new_lib = new_class.library();
    new_url = new_lib.IsNull() ? String::null() : new_lib.url();
    if (!new_url.Equals(url)) {
      continue;
    }

    return i;
  }

  return -1;
}


RawClass* IsolateReloadContext::FindOriginalClass(const Class& cls) {
  for (intptr_t i = 0; i < class_mappings_.length(); i++) {
    const Remapping& mapping = class_mappings_.At(i);
    if (mapping.new_id == cls.id()) {
      return I->class_table()->At(mapping.old_id);
    }
  }
  return Class::null();
}


void IsolateReloadContext::BuildClassIdMap() {
  const intptr_t lower_cid_bound =
      Dart::vm_isolate()->class_table()->NumCids();

  Class& cls = Class::Handle();
  for (intptr_t i = lower_cid_bound; i < saved_num_cids_; i++) {
    if (!I->class_table()->HasValidClassAt(i)) {
      continue;
    }
    cls ^= I->class_table()->At(i);
    Remapping mapping;
    mapping.new_id = FindReplacementClassId(cls);
    if (mapping.new_id == -1) {
      continue;
    }
    mapping.old_id = i;
    class_mappings_.Add(mapping);
  }

  fprintf(stderr, "---- CLASS ID MAPPING\n");
  for (intptr_t i = 0; i < class_mappings_.length(); i++) {
    const Remapping& mapping = class_mappings_[i];
    ASSERT(mapping.new_id > 0);
    ASSERT(mapping.old_id > 0);
    fprintf(stderr, "%" Pd " -> %" Pd "\n", mapping.old_id, mapping.new_id);
  }
}




void IsolateReloadContext::BuildLibraryIdMap() {
  const GrowableObjectArray& saved_libs =
      GrowableObjectArray::Handle(saved_libraries());

  Library& lib = Library::Handle();
  for (intptr_t i = 0; i < saved_libs.Length(); i++) {
    lib = Library::RawCast(saved_libs.At(i));
    if (lib.is_dart_scheme()) {
      continue;
    }
    Remapping mapping;
    mapping.new_id = FindReplacementLibrary(lib);
    if (mapping.new_id == -1) {
      continue;
    }
    mapping.old_id = i;
    lib_mappings_.Add(mapping);
  }

  const GrowableObjectArray& libs =
      GrowableObjectArray::Handle(object_store()->libraries());

  fprintf(stderr, "---- LIBRARY ID MAPPING\n");
  String& url = String::Handle();
  for (intptr_t i = 0; i < lib_mappings_.length(); i++) {
    const Remapping& mapping = lib_mappings_[i];
    ASSERT(mapping.new_id > 0);
    ASSERT(mapping.old_id > 0);

    // Lookup old library.
    lib = Library::RawCast(saved_libs.At(mapping.old_id));
    url = lib.url();

    fprintf(stderr, "%" Pd " %s ->", mapping.old_id, url.ToCString());

    lib = Library::RawCast(libs.At(mapping.new_id));
    url = lib.url();

    fprintf(stderr, "%" Pd " %s\n", mapping.new_id, url.ToCString());

  }
}


intptr_t IsolateReloadContext::FindReplacementLibrary(const Library& lib) {
  const GrowableObjectArray& libs =
      GrowableObjectArray::Handle(object_store()->libraries());

  const String& url = String::Handle(lib.url());

  Library& new_lib = Library::Handle();
  String& new_url = String::Handle();

  for (intptr_t i = 0; i < libs.Length(); i++) {
    new_lib = Library::RawCast(libs.At(i));
    if (new_lib.IsNull()) {
      continue;
    }
    new_url = new_lib.url();
    if (url.Equals(new_url)) {
      return i;
    }
  }

  return -1;
}


RawLibrary* IsolateReloadContext::saved_root_library() const {
  return saved_root_library_;
}


void IsolateReloadContext::set_saved_root_library(const Library& value) {
  saved_root_library_ = value.raw();
}


RawGrowableObjectArray* IsolateReloadContext::saved_libraries() const {
  return saved_libraries_;
}


void IsolateReloadContext::set_saved_libraries(
    const GrowableObjectArray& value) {
  saved_libraries_ = value.raw();
}


void IsolateReloadContext::VisitObjectPointers(ObjectPointerVisitor* visitor) {
  visitor->VisitPointers(from(), to());
}


ObjectStore* IsolateReloadContext::object_store() {
  return isolate_->object_store();
}


static void ResetICs(const Function& function, const Code& code) {
  if (function.ic_data_array() == Array::null()) {
    return;  // Already reset in an earlier round.
  }

  Thread* thread = Thread::Current();
  Zone* zone = thread->zone();

  ZoneGrowableArray<const ICData*>* ic_data_array =
      new(zone) ZoneGrowableArray<const ICData*>();
  function.RestoreICDataMap(ic_data_array, false /* clone ic-data */);
  const PcDescriptors& descriptors =
      PcDescriptors::Handle(code.pc_descriptors());
  PcDescriptors::Iterator iter(descriptors, RawPcDescriptors::kIcCall |
                                            RawPcDescriptors::kUnoptStaticCall);
  while (iter.MoveNext()) {
    const ICData* ic_data = (*ic_data_array)[iter.DeoptId()];
    bool is_static_call = iter.Kind() == RawPcDescriptors::kUnoptStaticCall;
    ic_data->Reset(is_static_call);
  }
}


void IsolateReloadContext::ResetUnoptimizedICsOnStack() {
  Code& code = Code::Handle();
  Function& function = Function::Handle();
  ObjectPool& object_table = ObjectPool::Handle();
  Object& object_table_entry = Object::Handle();
  DartFrameIterator iterator;
  StackFrame* frame = iterator.NextFrame();
  while (frame != NULL) {
    code = frame->LookupDartCode();
    if (code.is_optimized()) {
      // If this code is optimized, we need to reset the ICs in the
      // corresponding unoptimized code, which will be executed when the stack
      // unwinds to the the optimized code. We must use the unoptimized code
      // referenced from the optimized code's deopt object table, because this
      // is the code that will be used to finish the activation after deopt. It
      // can be different from the function's current unoptimized code, which
      // may be null if we've already done an atomic install or different code
      // if the function has already been recompiled.
      function = code.function();
      object_table = code.object_pool();
      intptr_t reset_count = 0;
      for (intptr_t i = 0; i < object_table.Length(); i++) {
        object_table_entry = object_table.ObjectAt(i);
        if (object_table_entry.IsCode()) {
          code ^= object_table_entry.raw();
          if (code.function() == function.raw()) {
            reset_count++;
            ResetICs(function, code);
          }
          // Why are other code objects in this table? Allocation stubs?
        }
      }
      // ASSERT(reset_count == 1);
      // vm shot itself in the foot: no reference to unopt code.
    } else {
      function = code.function();
      ResetICs(function, code);
    }
    frame = iterator.NextFrame();
  }
}


void IsolateReloadContext::ResetMegamorphicCaches() {
  object_store()->set_megamorphic_cache_table(GrowableObjectArray::Handle());
  // Since any current optimized code will not make any more calls, it may be
  // better to clear the table instead of clearing each of the caches, allow
  // the current megamorphic caches get GC'd and any new optimized code allocate
  // new ones.
}


class MarkFunctionsForRecompilation : public ObjectVisitor {
 public:
  explicit MarkFunctionsForRecompilation(Isolate* isolate)
    : ObjectVisitor(isolate),
      handle_(Object::Handle()),
      code_(Code::Handle()) {
  }

  virtual void VisitObject(RawObject* obj) {
    // Free-list elements cannot even be wrapped in handles.
    if (obj->IsFreeListElement()) {
      return;
    }
    handle_ = obj;
    if (handle_.IsFunction()) {
      const Function& func = Function::Cast(handle_);

      // Replace the instructions of most functions with the compilation stub so
      // unqualified invocations will be recompiled to the correct kind. But
      // leave the stub to patch the function for extracted properties so they
      // will still be patched if more than one modification happens before they
      // are next called.
      code_ = func.CurrentCode();
      if (!code_.IsStubCode()) {
        func.ClearICDataArray();  // Don't reuse IC data in next compilation.
        func.ClearCode();

        // Type feedback data is gone, don't trigger optimization again too
        // soon.
        func.set_usage_counter(0);
        func.set_deoptimization_counter(0);
      }
    }
  }

 private:
  Object& handle_;
  Code& code_;
};


void IsolateReloadContext::MarkAllFunctionsForRecompilation() {
  MarkFunctionsForRecompilation visitor(isolate_);
  NoSafepointScope no_safepoint;
  isolate_->heap()->VisitObjects(&visitor);
}


void IsolateReloadContext::InvalidateWorld() {
  // Discard all types of cached lookup, which are all potentially invalid.
  // - ICs and MegamorphicCaches
  // - Optimized code (inlining)
  // - Unoptimized code (unqualifed invocations were early bound to static
  //   or instance invocations)

  DeoptimizeFunctionsOnStack();
  ResetUnoptimizedICsOnStack();
  ResetMegamorphicCaches();
  MarkAllFunctionsForRecompilation();
}


}  // namespace dart

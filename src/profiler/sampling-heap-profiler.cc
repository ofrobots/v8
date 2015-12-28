// Copyright 2009-2010 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/api.h"
#include "src/frames-inl.h"
#include "src/heap/heap.h"
#include "src/isolate.h"
#include "src/profiler/sampling-heap-profiler.h"
#include "src/profiler/strings-storage.h"

namespace v8 {
namespace internal {

SamplingHeapProfiler::SamplingHeapProfiler(Heap* heap, StringsStorage* names,
                                           uint64_t rate)
    : InlineAllocationObserver(GetNextSampleInterval(
          heap->isolate()->random_number_generator(), rate)),
      isolate_(heap->isolate()),
      heap_(heap),
      random_(isolate_->random_number_generator()),
      names_(names),
      samples_(),
      rate_(rate) {
  heap->new_space()->AddInlineAllocationObserver(this);
}


SamplingHeapProfiler::~SamplingHeapProfiler() {
  heap_->new_space()->RemoveInlineAllocationObserver(this);
}

void SamplingHeapProfiler::Step(int bytes_allocated, Address soon_object,
                                size_t size) {
  DCHECK(heap_->gc_state() == Heap::NOT_IN_GC);

  if (!soon_object) {
    // TODO(ofrobots): Sometimes we get a step at a point where don't have an
    // actual object being allocated (e.g. ResetAllocationInfo, PauseInlineAllo-
    // cationObservers). These ought to be avoidable. Need to investigate.
    return;
  }
  SampleObject(soon_object, size);
}


void SamplingHeapProfiler::SampleObject(Address soon_object, size_t size) {
  DisallowHeapAllocation no_allocation;

  HandleScope scope(isolate_);
  HeapObject* heap_object = HeapObject::FromAddress(soon_object);
  Handle<Object> obj(heap_object, isolate_);

  // Mark the new block as FreeSpace to make sure the heap is iterable while we
  // are taking the sample.
  heap()->CreateFillerObjectAt(soon_object, static_cast<int>(size));

  Local<v8::Value> loc = v8::Utils::ToLocal(obj);

  Sample* sample = new Sample(this, isolate_, loc, size);
  samples_.insert(sample);
}


// We sample with a Poisson process, with constant average sampling interval.
// This follows the exponential probability distribution with parameter
// λ = 1/rate where rate is the average number of bytes between samples.
//
// Let u be a uniformly distributed random number between 0 and 1, then
// next_sample = (- ln u) / λ
intptr_t SamplingHeapProfiler::GetNextSampleInterval(
    base::RandomNumberGenerator* random, uint64_t rate) {
  double u = random->NextDouble();
  double next = (-std::log(u)) * rate;
  return std::max(static_cast<intptr_t>(kPointerSize),
                  static_cast<intptr_t>(next));
}


void SamplingHeapProfiler::GetHeapSample(OutputStream* stream) {
  OutputStreamWriter writer(stream);
  writer.AddString("[\n");
  std::set<Sample*>::iterator it;
  for (it = samples_.begin(); it != samples_.end(); ++it) {
    auto sample = *it;
    if (it != samples_.begin()) {
      writer.AddString(",");
    }
    writer.AddString(" {\"size\": ");
    writer.AddNumber(static_cast<unsigned>(sample->get_size()));
    writer.AddString(", \"stack\": [\n");
    List<FunctionInfo*>& stack = sample->get_stack();
    for (int i = 0; i < stack.length(); ++i) {
      FunctionInfo* info = stack[i];
      writer.AddString("\t{\"name\": \"");
      writer.AddString(info->name());
      writer.AddString("\", \"scriptName\": \"");
      writer.AddString(info->script_name());
      if (i < (stack.length() - 1)) {
        writer.AddString("\"},\n");
      } else {
        writer.AddString("\"}\n");
      }
    }
    writer.AddString(" ]}\n");
  }
  writer.AddString("]\n");
  writer.Finalize();
}

void SamplingHeapProfiler::Sample::OnWeakCallback(
    const WeakCallbackInfo<Sample>& data) {
  Sample* sample = data.GetParameter();
  sample->shp_->samples_.erase(sample);
  delete sample;
}


SamplingHeapProfiler::FunctionInfo::FunctionInfo(SharedFunctionInfo* shared,
                                                 StringsStorage* names)
    : name_(names->GetFunctionName(shared->DebugName())), script_name_("") {
  if (shared->script()->IsScript()) {
    Script* script = Script::cast(shared->script());
    if (script->name()->IsName()) {
      Name* name = Name::cast(script->name());
      script_name_ = names->GetName(name);
    }

    // TODO(ofrobots): resolve line/column numbers.
  }
}

SamplingHeapProfiler::Sample::Sample(SamplingHeapProfiler* shp,
                                     Isolate* isolate, Local<Value> local,
                                     size_t size)
    : shp_(shp),
      global_(reinterpret_cast<v8::Isolate*>(isolate), local),
      size_(size) {
  global_.SetWeak(this, OnWeakCallback, WeakCallbackType::kParameter);


  StackTraceFrameIterator it(isolate);
  const int limit = 10;
  int frames_captured = 0;
  while (!it.done() && frames_captured < limit) {
    JavaScriptFrame* frame = it.frame();
    SharedFunctionInfo* shared = frame->function()->shared();
    stack_.Add(new FunctionInfo(shared, shp->names()));

    frames_captured++;
    it.Advance();
  }
}


}  // namespace internal
}  // namespace v8

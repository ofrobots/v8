// Copyright 2009-2010 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_PROFILER_SAMPLING_HEAP_PROFILER_H_
#define V8_PROFILER_SAMPLING_HEAP_PROFILER_H_

#include <set>
#include "src/base/utils/random-number-generator.h"
#include "src/heap/spaces.h"
#include "src/profiler/allocation-tracker.h"

namespace v8 {
namespace internal {

class SamplingHeapProfiler : public InlineAllocationObserver {
 public:
  static const uint64_t kDefaultSampleInterval = (512 * 1024);  // 512KiB.

  SamplingHeapProfiler(Heap* heap, StringsStorage* names,
                       uint64_t rate = kDefaultSampleInterval);
  ~SamplingHeapProfiler();

  void GetHeapSample(OutputStream* stream);

  void Step(int bytes_allocated, Address soon_object, size_t size) override;
  intptr_t GetNextStepSize() override {
    return GetNextSampleInterval(random_, rate_);
  }

  StringsStorage* names() const { return names_; }

  class FunctionInfo {
   public:
    FunctionInfo(SharedFunctionInfo* shared, StringsStorage* names);
    const char* name() const { return name_; }
    const char* script_name() const { return script_name_; }

   private:
    const char* const name_;
    const char* script_name_;
  };

  class Sample {
   public:
    Sample(SamplingHeapProfiler* shp, Isolate* isolate, Local<Value> local,
           size_t size);
    ~Sample() {
      stack_.Iterate(&DeleteFunctionInfo);
      global_.Reset();  // drop the reference.
    }
    size_t get_size() const { return size_; }
    List<FunctionInfo*>& get_stack() { return stack_; }

   private:
    static void OnWeakCallback(const WeakCallbackInfo<Sample>& data);
    static void DeleteFunctionInfo(FunctionInfo** infop) { delete *infop; }

    SamplingHeapProfiler* const shp_;
    Global<Value> global_;
    List<FunctionInfo*> stack_;
    const size_t size_;

    DISALLOW_COPY_AND_ASSIGN(Sample);
  };

 private:
  Heap* heap() const { return heap_; }

  void SampleObject(Address soon_object, size_t size);

  static intptr_t GetNextSampleInterval(base::RandomNumberGenerator* random,
                                        uint64_t rate);

  Isolate* const isolate_;
  Heap* const heap_;
  base::RandomNumberGenerator* const random_;
  StringsStorage* const names_;
  std::set<Sample*> samples_;
  const uint64_t rate_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_PROFILER_HEAP_PROFILER_H_

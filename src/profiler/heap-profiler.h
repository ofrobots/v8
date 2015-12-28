// Copyright 2009-2010 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_PROFILER_HEAP_PROFILER_H_
#define V8_PROFILER_HEAP_PROFILER_H_

#include "include/v8-util.h"
#include "src/base/smart-pointers.h"
#include "src/isolate.h"
#include "src/list.h"

namespace v8 {
namespace internal {

// Forward declarations.
class AllocationTracker;
class HeapObjectsMap;
class HeapSnapshot;
class SamplingHeapProfiler;
class StringsStorage;

class HeapProfiler {
 public:
  explicit HeapProfiler(Heap* heap);
  ~HeapProfiler();

  size_t GetMemorySizeUsedByProfiler();

  HeapSnapshot* TakeSnapshot(
      v8::ActivityControl* control,
      v8::HeapProfiler::ObjectNameResolver* resolver);

  void StartSamplingHeapProfiler();
  void StopSamplingHeapProfiler();
  void GetHeapSample(OutputStream* stream);

  void StartHeapObjectsTracking(bool track_allocations);
  void StopHeapObjectsTracking();
  AllocationTracker* allocation_tracker() const {
    return allocation_tracker_.get();
  }
  HeapObjectsMap* heap_object_map() const { return ids_.get(); }
  StringsStorage* names() const { return names_.get(); }

  SnapshotObjectId PushHeapObjectsStats(OutputStream* stream,
                                        int64_t* timestamp_us);
  int GetSnapshotsCount();
  HeapSnapshot* GetSnapshot(int index);
  SnapshotObjectId GetSnapshotObjectId(Handle<Object> obj);
  void DeleteAllSnapshots();
  void RemoveSnapshot(HeapSnapshot* snapshot);

  void ObjectMoveEvent(Address from, Address to, int size);

  void AllocationEvent(Address addr, int size);

  void UpdateObjectSizeEvent(Address addr, int size);

  void DefineWrapperClass(
      uint16_t class_id, v8::HeapProfiler::WrapperInfoCallback callback);

  v8::RetainedObjectInfo* ExecuteWrapperClassCallback(uint16_t class_id,
                                                      Object** wrapper);
  void SetRetainedObjectInfo(UniqueId id, RetainedObjectInfo* info);

  bool is_tracking_object_moves() const { return is_tracking_object_moves_; }
  bool is_tracking_allocations() const {
    return !allocation_tracker_.is_empty();
  }

  Handle<HeapObject> FindHeapObjectById(SnapshotObjectId id);
  void ClearHeapObjectMap();

  Isolate* isolate() const { return heap()->isolate(); }

 private:
  Heap* heap() const;

  // Mapping from HeapObject addresses to objects' uids.
  base::SmartPointer<HeapObjectsMap> ids_;
  List<HeapSnapshot*> snapshots_;
  base::SmartPointer<StringsStorage> names_;
  List<v8::HeapProfiler::WrapperInfoCallback> wrapper_callbacks_;
  base::SmartPointer<AllocationTracker> allocation_tracker_;
  bool is_tracking_object_moves_;
  base::Mutex profiler_mutex_;
  base::SmartPointer<SamplingHeapProfiler> sampling_heap_profiler_;
};

template <int bytes>
struct MaxDecimalDigitsIn;
template <>
struct MaxDecimalDigitsIn<4> {
  static const int kSigned = 11;
  static const int kUnsigned = 10;
};
template <>
struct MaxDecimalDigitsIn<8> {
  static const int kSigned = 20;
  static const int kUnsigned = 20;
};

class OutputStreamWriter {
 public:
  explicit OutputStreamWriter(v8::OutputStream* stream)
      : stream_(stream),
        chunk_size_(stream->GetChunkSize()),
        chunk_(chunk_size_),
        chunk_pos_(0),
        aborted_(false) {
    DCHECK(chunk_size_ > 0);
  }
  bool aborted() { return aborted_; }
  void AddCharacter(char c) {
    DCHECK(c != '\0');
    DCHECK(chunk_pos_ < chunk_size_);
    chunk_[chunk_pos_++] = c;
    MaybeWriteChunk();
  }
  void AddString(const char* s) { AddSubstring(s, StrLength(s)); }
  void AddSubstring(const char* s, int n) {
    if (n <= 0) return;
    DCHECK(static_cast<size_t>(n) <= strlen(s));
    const char* s_end = s + n;
    while (s < s_end) {
      int s_chunk_size =
          Min(chunk_size_ - chunk_pos_, static_cast<int>(s_end - s));
      DCHECK(s_chunk_size > 0);
      MemCopy(chunk_.start() + chunk_pos_, s, s_chunk_size);
      s += s_chunk_size;
      chunk_pos_ += s_chunk_size;
      MaybeWriteChunk();
    }
  }
  void AddNumber(unsigned n) { AddNumberImpl<unsigned>(n, "%u"); }
  void Finalize() {
    if (aborted_) return;
    DCHECK(chunk_pos_ < chunk_size_);
    if (chunk_pos_ != 0) {
      WriteChunk();
    }
    stream_->EndOfStream();
  }

 private:
  template <typename T>
  void AddNumberImpl(T n, const char* format) {
    // Buffer for the longest value plus trailing \0
    static const int kMaxNumberSize =
        MaxDecimalDigitsIn<sizeof(T)>::kUnsigned + 1;
    if (chunk_size_ - chunk_pos_ >= kMaxNumberSize) {
      int result =
          SNPrintF(chunk_.SubVector(chunk_pos_, chunk_size_), format, n);
      DCHECK(result != -1);
      chunk_pos_ += result;
      MaybeWriteChunk();
    } else {
      EmbeddedVector<char, kMaxNumberSize> buffer;
      int result = SNPrintF(buffer, format, n);
      USE(result);
      DCHECK(result != -1);
      AddString(buffer.start());
    }
  }
  void MaybeWriteChunk() {
    DCHECK(chunk_pos_ <= chunk_size_);
    if (chunk_pos_ == chunk_size_) {
      WriteChunk();
    }
  }
  void WriteChunk() {
    if (aborted_) return;
    if (stream_->WriteAsciiChunk(chunk_.start(), chunk_pos_) ==
        v8::OutputStream::kAbort)
      aborted_ = true;
    chunk_pos_ = 0;
  }

  v8::OutputStream* stream_;
  int chunk_size_;
  ScopedVector<char> chunk_;
  int chunk_pos_;
  bool aborted_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_PROFILER_HEAP_PROFILER_H_

// Local additions to the RocksDB C API. See c_api_extensions.h for the
// rationale and the list of extensions; this file just defines the
// declarations from that header.
//
// Each extension is the smallest practical delta over the existing C API:
// either an option setter/getter pair or a thin wrapper over an upstream C++
// callback surface that has not reached rocksdb/c.h yet.

#include "c_api_extensions.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "rocksdb/iterator.h"
#include "rocksdb/listener.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/table.h"

using ROCKSDB_NAMESPACE::BackgroundErrorRecoveryInfo;
using ROCKSDB_NAMESPACE::BlockBasedTableOptions;
using ROCKSDB_NAMESPACE::CompactRangeOptions;
using ROCKSDB_NAMESPACE::CompactionJobInfo;
using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::EventListener;
using ROCKSDB_NAMESPACE::ExternalFileIngestionInfo;
using ROCKSDB_NAMESPACE::FlushJobInfo;
using ROCKSDB_NAMESPACE::IngestExternalFileOptions;
using ROCKSDB_NAMESPACE::Options;
using ROCKSDB_NAMESPACE::ReadOptions;
using ROCKSDB_NAMESPACE::Status;
using ROCKSDB_NAMESPACE::SubcompactionJobInfo;
using ROCKSDB_NAMESPACE::WriteStallInfo;
using ROCKSDB_NAMESPACE::MemTableInfo;
using ROCKSDB_NAMESPACE::Iterator;
using ROCKSDB_NAMESPACE::Slice;
using ROCKSDB_NAMESPACE::SstFileReader;

struct rust_rocksdb_status_t {
  Status* rep;
};

struct rust_rocksdb_background_error_recovery_info_t {
  const BackgroundErrorRecoveryInfo* rep;
};

static bool RustSaveError(char** errptr, const Status& s) {
  assert(errptr != nullptr);
  if (s.ok()) {
    return false;
  }

  std::string message = s.ToString();
  char* copy = static_cast<char*>(std::malloc(message.size() + 1));
  if (copy != nullptr) {
    std::memcpy(copy, message.c_str(), message.size() + 1);
  }

  if (*errptr != nullptr) {
    std::free(*errptr);
  }
  *errptr = copy;
  return true;
}

extern "C" void rust_rocksdb_status_get_error(rust_rocksdb_status_t* status,
                                               char** errptr) {
  RustSaveError(errptr, *(status->rep));
}

extern "C" unsigned char rust_rocksdb_status_get_severity(
    rust_rocksdb_status_t* status) {
  return static_cast<unsigned char>(status->rep->severity());
}

extern "C" void rust_rocksdb_status_reset(rust_rocksdb_status_t* status) {
  *(status->rep) = Status::OK();
}

extern "C" void rust_rocksdb_background_error_recovery_info_old_bg_error(
    const rust_rocksdb_background_error_recovery_info_t* info, char** errptr) {
  RustSaveError(errptr, info->rep->old_bg_error);
}

extern "C" unsigned char
rust_rocksdb_background_error_recovery_info_old_bg_error_severity(
    const rust_rocksdb_background_error_recovery_info_t* info) {
  return static_cast<unsigned char>(info->rep->old_bg_error.severity());
}

extern "C" void rust_rocksdb_background_error_recovery_info_new_bg_error(
    const rust_rocksdb_background_error_recovery_info_t* info, char** errptr) {
  RustSaveError(errptr, info->rep->new_bg_error);
}

extern "C" unsigned char
rust_rocksdb_background_error_recovery_info_new_bg_error_severity(
    const rust_rocksdb_background_error_recovery_info_t* info) {
  return static_cast<unsigned char>(info->rep->new_bg_error.severity());
}

struct rust_rocksdb_eventlistener_t : public EventListener {
  void* state{};
  void (*destructor)(void*){};
  rust_rocksdb_on_flush_begin_cb on_flush_begin{};
  rust_rocksdb_on_flush_completed_cb on_flush_completed{};
  rust_rocksdb_on_compaction_begin_cb on_compaction_begin{};
  rust_rocksdb_on_compaction_completed_cb on_compaction_completed{};
  rust_rocksdb_on_subcompaction_begin_cb on_subcompaction_begin{};
  rust_rocksdb_on_subcompaction_completed_cb on_subcompaction_completed{};
  rust_rocksdb_on_external_file_ingested_cb on_external_file_ingested{};
  rust_rocksdb_on_background_error_cb on_background_error{};
  rust_rocksdb_on_error_recovery_begin_cb on_error_recovery_begin{};
  rust_rocksdb_on_error_recovery_end_cb on_error_recovery_end{};
  rust_rocksdb_on_stall_conditions_changed_cb on_stall_conditions_changed{};
  rust_rocksdb_on_memtable_sealed_cb on_memtable_sealed{};

  rust_rocksdb_eventlistener_t() = default;

  rust_rocksdb_eventlistener_t(const rust_rocksdb_eventlistener_t&) = delete;
  rust_rocksdb_eventlistener_t& operator=(
      const rust_rocksdb_eventlistener_t&) = delete;
  rust_rocksdb_eventlistener_t(rust_rocksdb_eventlistener_t&&) = delete;
  rust_rocksdb_eventlistener_t& operator=(rust_rocksdb_eventlistener_t&&) =
      delete;

  void OnFlushBegin(DB* /*db*/, const FlushJobInfo& info) override {
    if (on_flush_begin != nullptr) {
      on_flush_begin(state,
                     reinterpret_cast<const rocksdb_flushjobinfo_t*>(&info));
    }
  }

  void OnFlushCompleted(DB* /*db*/, const FlushJobInfo& info) override {
    if (on_flush_completed != nullptr) {
      on_flush_completed(
          state, reinterpret_cast<const rocksdb_flushjobinfo_t*>(&info));
    }
  }

  void OnCompactionBegin(DB* /*db*/, const CompactionJobInfo& info) override {
    if (on_compaction_begin != nullptr) {
      on_compaction_begin(
          state, reinterpret_cast<const rocksdb_compactionjobinfo_t*>(&info));
    }
  }

  void OnCompactionCompleted(DB* /*db*/, const CompactionJobInfo& info)
      override {
    if (on_compaction_completed != nullptr) {
      on_compaction_completed(
          state, reinterpret_cast<const rocksdb_compactionjobinfo_t*>(&info));
    }
  }

  void OnSubcompactionBegin(const SubcompactionJobInfo& info) override {
    if (on_subcompaction_begin != nullptr) {
      on_subcompaction_begin(
          state,
          reinterpret_cast<const rocksdb_subcompactionjobinfo_t*>(&info));
    }
  }

  void OnSubcompactionCompleted(const SubcompactionJobInfo& info) override {
    if (on_subcompaction_completed != nullptr) {
      on_subcompaction_completed(
          state,
          reinterpret_cast<const rocksdb_subcompactionjobinfo_t*>(&info));
    }
  }

  void OnExternalFileIngested(DB* /*db*/,
                              const ExternalFileIngestionInfo& info) override {
    if (on_external_file_ingested != nullptr) {
      on_external_file_ingested(
          state,
          reinterpret_cast<const rocksdb_externalfileingestioninfo_t*>(&info));
    }
  }

  void OnBackgroundError(ROCKSDB_NAMESPACE::BackgroundErrorReason reason,
                         Status* status) override {
    if (on_background_error != nullptr) {
      rust_rocksdb_status_t s = {status};
      on_background_error(state, static_cast<uint32_t>(reason), &s);
    }
  }

  void OnErrorRecoveryBegin(ROCKSDB_NAMESPACE::BackgroundErrorReason reason,
                            Status bg_error,
                            bool* auto_recovery) override {
    if (on_error_recovery_begin != nullptr) {
      rust_rocksdb_status_t s = {&bg_error};
      unsigned char auto_recovery_value =
          auto_recovery != nullptr && *auto_recovery;
      on_error_recovery_begin(state, static_cast<uint32_t>(reason), &s,
                              &auto_recovery_value);
      if (auto_recovery != nullptr) {
        *auto_recovery = auto_recovery_value != 0;
      }
    }
    bg_error.PermitUncheckedError();
  }

  void OnErrorRecoveryEnd(const BackgroundErrorRecoveryInfo& info) override {
    if (on_error_recovery_end != nullptr) {
      rust_rocksdb_background_error_recovery_info_t c_info = {&info};
      on_error_recovery_end(state, &c_info);
    }
    info.old_bg_error.PermitUncheckedError();
    info.new_bg_error.PermitUncheckedError();
  }

  void OnStallConditionsChanged(const WriteStallInfo& info) override {
    if (on_stall_conditions_changed != nullptr) {
      on_stall_conditions_changed(
          state, reinterpret_cast<const rocksdb_writestallinfo_t*>(&info));
    }
  }

  void OnMemTableSealed(const MemTableInfo& info) override {
    if (on_memtable_sealed != nullptr) {
      on_memtable_sealed(
          state, reinterpret_cast<const rocksdb_memtableinfo_t*>(&info));
    }
  }

  ~rust_rocksdb_eventlistener_t() override {
    if (destructor != nullptr) {
      destructor(state);
    }
  }
};

extern "C" rust_rocksdb_eventlistener_t* rust_rocksdb_eventlistener_create(
    void* state, void (*destructor)(void*),
    rust_rocksdb_on_flush_begin_cb on_flush_begin,
    rust_rocksdb_on_flush_completed_cb on_flush_completed,
    rust_rocksdb_on_compaction_begin_cb on_compaction_begin,
    rust_rocksdb_on_compaction_completed_cb on_compaction_completed,
    rust_rocksdb_on_subcompaction_begin_cb on_subcompaction_begin,
    rust_rocksdb_on_subcompaction_completed_cb on_subcompaction_completed,
    rust_rocksdb_on_external_file_ingested_cb on_external_file_ingested,
    rust_rocksdb_on_background_error_cb on_background_error,
    rust_rocksdb_on_error_recovery_begin_cb on_error_recovery_begin,
    rust_rocksdb_on_error_recovery_end_cb on_error_recovery_end,
    rust_rocksdb_on_stall_conditions_changed_cb on_stall_conditions_changed,
    rust_rocksdb_on_memtable_sealed_cb on_memtable_sealed) {
  rust_rocksdb_eventlistener_t* listener = new rust_rocksdb_eventlistener_t;
  listener->state = state;
  listener->destructor = destructor;
  listener->on_flush_begin = on_flush_begin;
  listener->on_flush_completed = on_flush_completed;
  listener->on_compaction_begin = on_compaction_begin;
  listener->on_compaction_completed = on_compaction_completed;
  listener->on_subcompaction_begin = on_subcompaction_begin;
  listener->on_subcompaction_completed = on_subcompaction_completed;
  listener->on_external_file_ingested = on_external_file_ingested;
  listener->on_background_error = on_background_error;
  listener->on_error_recovery_begin = on_error_recovery_begin;
  listener->on_error_recovery_end = on_error_recovery_end;
  listener->on_stall_conditions_changed = on_stall_conditions_changed;
  listener->on_memtable_sealed = on_memtable_sealed;
  return listener;
}

extern "C" void rust_rocksdb_eventlistener_destroy(
    rust_rocksdb_eventlistener_t* listener) {
  delete listener;
}

extern "C" void rust_rocksdb_options_add_eventlistener(
    rocksdb_options_t* opt, rust_rocksdb_eventlistener_t* listener) {
  reinterpret_cast<Options*>(opt)->listeners.emplace_back(
      std::shared_ptr<EventListener>(listener));
}

// The opaque-handle types the C API hands out are defined at file scope in
// `rocksdb/db/c.cc` as POD wrappers around a single C++ class:
//
//   struct rocksdb_readoptions_t { ReadOptions rep; /* trailing Slices */ };
//   struct rocksdb_options_t { Options rep; };
//   struct rocksdb_block_based_table_options_t { BlockBasedTableOptions rep; };
//
// In every case the `rep` field is the FIRST member, so a pointer to the
// opaque C type also points at the start of its embedded C++ `rep` field.
// We exploit that here with a direct `reinterpret_cast` instead of
// replicating the struct definitions — replication would either drift
// silently if upstream ever adds a field before `rep` (the very change that
// would also break this cast), or trip C++'s one-definition rule against
// c.h's `typedef struct rocksdb_readoptions_t rocksdb_readoptions_t;`.
//
// If upstream ever adds a field BEFORE `rep` in any of these wrappers,
// every test that round-trips a value through one of our setters will
// fail loudly: the setter would write to one offset and rocksdb's
// internal code would read from another. The integration tests in
// `tests/test_rocksdb_options.rs` cover all three options that this file
// exposes, so a layout regression is detectable.

// -----------------------------------------------------------------------------
// ReadOptions::optimize_multiget_for_io
// -----------------------------------------------------------------------------

extern "C" void rocksdb_readoptions_set_optimize_multiget_for_io(
    rocksdb_readoptions_t* opt, unsigned char v) {
  reinterpret_cast<ReadOptions*>(opt)->optimize_multiget_for_io = v;
}

extern "C" unsigned char rocksdb_readoptions_get_optimize_multiget_for_io(
    rocksdb_readoptions_t* opt) {
  return reinterpret_cast<ReadOptions*>(opt)->optimize_multiget_for_io;
}

// -----------------------------------------------------------------------------
// BlockBasedTableOptions::uniform_cv_threshold
//
// The corresponding `kAuto` enum value is declared in c_api_extensions.h
// — no C-side definition is needed because the existing
// `rocksdb_block_based_options_set_index_block_search_type` setter in
// upstream `c.cc` already does `static_cast<BlockSearchType>(int)` and
// accepts any value the caller passes.
// -----------------------------------------------------------------------------

extern "C" void rocksdb_block_based_options_set_uniform_cv_threshold(
    rocksdb_block_based_table_options_t* opt, double v) {
  reinterpret_cast<BlockBasedTableOptions*>(opt)->uniform_cv_threshold = v;
}

// -----------------------------------------------------------------------------
// AdvancedColumnFamilyOptions::memtable_batch_lookup_optimization
// -----------------------------------------------------------------------------

extern "C" void rocksdb_options_set_memtable_batch_lookup_optimization(
    rocksdb_options_t* opt, unsigned char v) {
  reinterpret_cast<Options*>(opt)->memtable_batch_lookup_optimization = v;
}

extern "C" unsigned char rocksdb_options_get_memtable_batch_lookup_optimization(
    rocksdb_options_t* opt) {
  return reinterpret_cast<Options*>(opt)->memtable_batch_lookup_optimization;
}

// -----------------------------------------------------------------------------
// CompactOptions::blob_garbage_collection_age_cutoff
// -----------------------------------------------------------------------------

extern "C" void rocksdb_compactoptions_set_blob_garbage_collection_age_cutoff(
    rocksdb_compactoptions_t* opt, double v) {
  reinterpret_cast<CompactRangeOptions*>(opt)->blob_garbage_collection_age_cutoff = v;
}

extern "C" double rocksdb_compactoptions_get_blob_garbage_collection_age_cutoff(
    rocksdb_compactoptions_t* opt) {
  return reinterpret_cast<CompactRangeOptions*>(opt)->blob_garbage_collection_age_cutoff;
}

// -----------------------------------------------------------------------------
// IngestExternalFileOptions::link_files
// -----------------------------------------------------------------------------

extern "C" void rocksdb_ingestexternalfileoptions_set_link_files(
    rocksdb_ingestexternalfileoptions_t* opt, unsigned char v) {
  reinterpret_cast<IngestExternalFileOptions*>(opt)->link_files = v;
}

extern "C" unsigned char rocksdb_ingestexternalfileoptions_get_link_files(
    rocksdb_ingestexternalfileoptions_t* opt) {
  return reinterpret_cast<IngestExternalFileOptions*>(opt)->link_files;
}

// -----------------------------------------------------------------------------
// SstFileReader
//
// `rocksdb_options_t` / `rocksdb_readoptions_t` wrap their C++ counterparts as
// the first struct member, so a `reinterpret_cast` recovers the C++ object (the
// same technique upstream `c.cc` uses internally). The reader and iterator get
// their own opaque handle structs, owned entirely by this translation unit.
// -----------------------------------------------------------------------------

struct rust_rocksdb_sst_file_reader_t {
  SstFileReader* rep;
};
struct rust_rocksdb_sst_file_reader_iterator_t {
  Iterator* rep;
};

extern "C" rust_rocksdb_sst_file_reader_t* rust_rocksdb_sst_file_reader_create(
    const rocksdb_options_t* options) {
  auto* reader = new rust_rocksdb_sst_file_reader_t;
  reader->rep = new SstFileReader(*reinterpret_cast<const Options*>(options));
  return reader;
}

extern "C" void rust_rocksdb_sst_file_reader_open(
    rust_rocksdb_sst_file_reader_t* reader, const char* name, char** errptr) {
  RustSaveError(errptr, reader->rep->Open(std::string(name)));
}

extern "C" rust_rocksdb_sst_file_reader_iterator_t*
rust_rocksdb_sst_file_reader_new_iterator(
    rust_rocksdb_sst_file_reader_t* reader,
    const rocksdb_readoptions_t* options) {
  auto* iter = new rust_rocksdb_sst_file_reader_iterator_t;
  iter->rep =
      reader->rep->NewIterator(*reinterpret_cast<const ReadOptions*>(options));
  return iter;
}

extern "C" void rust_rocksdb_sst_file_reader_destroy(
    rust_rocksdb_sst_file_reader_t* reader) {
  delete reader->rep;
  delete reader;
}

extern "C" unsigned char rust_rocksdb_sst_file_reader_iter_valid(
    const rust_rocksdb_sst_file_reader_iterator_t* iter) {
  return iter->rep->Valid();
}

extern "C" void rust_rocksdb_sst_file_reader_iter_seek_to_first(
    rust_rocksdb_sst_file_reader_iterator_t* iter) {
  iter->rep->SeekToFirst();
}

extern "C" void rust_rocksdb_sst_file_reader_iter_seek_to_last(
    rust_rocksdb_sst_file_reader_iterator_t* iter) {
  iter->rep->SeekToLast();
}

extern "C" void rust_rocksdb_sst_file_reader_iter_seek(
    rust_rocksdb_sst_file_reader_iterator_t* iter, const char* k, size_t klen) {
  iter->rep->Seek(Slice(k, klen));
}

extern "C" void rust_rocksdb_sst_file_reader_iter_seek_for_prev(
    rust_rocksdb_sst_file_reader_iterator_t* iter, const char* k, size_t klen) {
  iter->rep->SeekForPrev(Slice(k, klen));
}

extern "C" void rust_rocksdb_sst_file_reader_iter_next(
    rust_rocksdb_sst_file_reader_iterator_t* iter) {
  iter->rep->Next();
}

extern "C" void rust_rocksdb_sst_file_reader_iter_prev(
    rust_rocksdb_sst_file_reader_iterator_t* iter) {
  iter->rep->Prev();
}

extern "C" const char* rust_rocksdb_sst_file_reader_iter_key(
    const rust_rocksdb_sst_file_reader_iterator_t* iter, size_t* klen) {
  Slice key = iter->rep->key();
  *klen = key.size();
  return key.data();
}

extern "C" const char* rust_rocksdb_sst_file_reader_iter_value(
    const rust_rocksdb_sst_file_reader_iterator_t* iter, size_t* vlen) {
  Slice value = iter->rep->value();
  *vlen = value.size();
  return value.data();
}

extern "C" void rust_rocksdb_sst_file_reader_iter_get_error(
    const rust_rocksdb_sst_file_reader_iterator_t* iter, char** errptr) {
  RustSaveError(errptr, iter->rep->status());
}

extern "C" void rust_rocksdb_sst_file_reader_iter_destroy(
    rust_rocksdb_sst_file_reader_iterator_t* iter) {
  delete iter->rep;
  delete iter;
}

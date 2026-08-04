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
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

#include "rocksdb/cleanable.h"
#include "rocksdb/db.h"
#include "rocksdb/iterator.h"
#include "rocksdb/listener.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/sst_file_reader.h"
#include "rocksdb/sst_partitioner.h"
#include "rocksdb/statistics.h"
#include "rocksdb/table.h"
#include "rocksdb/version.h"
#include "rocksdb/write_batch.h"

using ROCKSDB_NAMESPACE::BackgroundErrorRecoveryInfo;
using ROCKSDB_NAMESPACE::CompactionJobInfo;
using ROCKSDB_NAMESPACE::ColumnFamilyHandle;
using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::EventListener;
using ROCKSDB_NAMESPACE::ExternalFileIngestionInfo;
using ROCKSDB_NAMESPACE::FlushJobInfo;
using ROCKSDB_NAMESPACE::IngestExternalFileOptions;
using ROCKSDB_NAMESPACE::Options;
using ROCKSDB_NAMESPACE::PinnableSlice;
using ROCKSDB_NAMESPACE::ReadOptions;
using ROCKSDB_NAMESPACE::Slice;
using ROCKSDB_NAMESPACE::Snapshot;
using ROCKSDB_NAMESPACE::SliceParts;
using ROCKSDB_NAMESPACE::Status;
using ROCKSDB_NAMESPACE::SubcompactionJobInfo;
using ROCKSDB_NAMESPACE::WriteStallInfo;
using ROCKSDB_NAMESPACE::WriteBatch;
using ROCKSDB_NAMESPACE::MemTableInfo;
using ROCKSDB_NAMESPACE::Iterator;
using ROCKSDB_NAMESPACE::SstFileReader;

struct rust_rocksdb_status_t {
  Status* rep;
};

struct rust_rocksdb_background_error_recovery_info_t {
  const BackgroundErrorRecoveryInfo* rep;
};

// Copy a message into a `malloc` block, which is what the Rust side releases
// it with (`rocksdb_free` is plain `free`).
//
// Falls back to a short fixed string if the copy does not fit. Rust reads a
// null `errptr` as success, so losing the allocation must not be allowed to
// turn a failed operation into a silent one. A truncated reason is recoverable;
// a missing error is not.
static char* RustDupMessage(const char* message, size_t len) {
  char* copy = static_cast<char*>(std::malloc(len + 1));
  if (copy != nullptr) {
    std::memcpy(copy, message, len);
    copy[len] = '\0';
    return copy;
  }

  static const char kFallback[] = "out of memory while formatting error";
  copy = static_cast<char*>(std::malloc(sizeof(kFallback)));
  if (copy != nullptr) {
    std::memcpy(copy, kFallback, sizeof(kFallback));
  }
  return copy;
}

static bool RustSaveError(char** errptr, const Status& s) {
  assert(errptr != nullptr);
  if (s.ok()) {
    return false;
  }

  const std::string message = s.ToString();
  char* copy = RustDupMessage(message.data(), message.size());

  if (*errptr != nullptr) {
    std::free(*errptr);
  }
  *errptr = copy;
  return true;
}

static void RustSaveMessage(char** errptr, const char* message) {
  assert(errptr != nullptr);
  char* copy =
      message == nullptr ? nullptr : RustDupMessage(message, std::strlen(message));
  if (*errptr != nullptr) {
    std::free(*errptr);
  }
  *errptr = copy;
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
//   struct rocksdb_writebatch_t { WriteBatch rep; };
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
// every path through one of these casts writes to one offset while
// rocksdb's internal code reads from another, so the value never makes it
// across. `tests/test_event_listener.rs`, `tests/test_multiget_pinned.rs`,
// `tests/test_batched_pinned_multiget.rs` and, for the `rocksdb_snapshot_t`
// cast below, `transaction_snapshot_sequence_number_without_set_snapshot`
// in `tests/test_transaction_db.rs` each exercise one of these casts, so a
// layout regression is detectable.

// -----------------------------------------------------------------------------
// Options::open_files_async
// -----------------------------------------------------------------------------

#if ROCKSDB_MAJOR > 11 || (ROCKSDB_MAJOR == 11 && ROCKSDB_MINOR >= 1)
#define RUST_ROCKSDB_HAS_OPEN_FILES_ASYNC 1
#else
#define RUST_ROCKSDB_HAS_OPEN_FILES_ASYNC 0
#endif

extern "C" unsigned char rust_rocksdb_options_open_files_async_supported() {
  return RUST_ROCKSDB_HAS_OPEN_FILES_ASYNC;
}

extern "C" unsigned char rust_rocksdb_options_set_open_files_async(
    rocksdb_options_t* opt, unsigned char enabled) {
#if RUST_ROCKSDB_HAS_OPEN_FILES_ASYNC
  rocksdb_options_set_open_files_async(opt, enabled);
  return 1;
#else
  (void)opt;
  (void)enabled;
  return 0;
#endif
}

extern "C" unsigned char rust_rocksdb_options_get_open_files_async(
    rocksdb_options_t* opt) {
#if RUST_ROCKSDB_HAS_OPEN_FILES_ASYNC
  return rocksdb_options_get_open_files_async(opt);
#else
  (void)opt;
  return 0;
#endif
}

// -----------------------------------------------------------------------------
// Batch-owned pinned MultiGet results
// -----------------------------------------------------------------------------

struct rust_rocksdb_pinnable_batch_t {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
  std::vector<rocksdb_pinnableslice_t*> system_values;
#else
  std::vector<PinnableSlice> values;
  std::vector<size_t> result_indexes;
#endif
  std::unordered_map<size_t, std::string> errors;

#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
  ~rust_rocksdb_pinnable_batch_t() {
    for (auto* value : system_values) {
      if (value != nullptr) {
        rocksdb_pinnableslice_destroy(value);
      }
    }
  }
#endif
};

#ifndef RUST_ROCKSDB_SYSTEM_BACKEND
static constexpr size_t kRustRocksDbBatchNotFound =
    std::numeric_limits<size_t>::max();
static constexpr size_t kRustRocksDbBatchError =
    std::numeric_limits<size_t>::max() - 1;
#endif

#ifndef RUST_ROCKSDB_SYSTEM_BACKEND
static DB* RustRocksDbRep(rocksdb_t* db) {
  return *reinterpret_cast<DB**>(db);
}

static ColumnFamilyHandle* RustRocksDbColumnFamilyRep(
    rocksdb_column_family_handle_t* column_family) {
  return *reinterpret_cast<ColumnFamilyHandle**>(column_family);
}
#endif

extern "C" rust_rocksdb_pinnable_batch_t*
rust_rocksdb_batched_multi_get_pinned(
    rocksdb_t* db, const rocksdb_readoptions_t* options,
    rocksdb_column_family_handle_t* column_family, size_t num_keys,
    const rocksdb_slice_t* keys, unsigned char sorted_input, char** errptr) {
  try {
    auto batch = std::make_unique<rust_rocksdb_pinnable_batch_t>();
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
    batch->system_values.resize(num_keys, nullptr);
    std::vector<char*> errors(num_keys, nullptr);
    // Releases every per-key error string still held in `errors`, including
    // when the drain loop below throws partway through. Destroying the vector
    // only reclaims the array, not the strings it points at. The loop clears
    // each slot as it takes ownership, so nothing is freed twice.
    struct ErrorArrayGuard {
      std::vector<char*>& errors;
      ~ErrorArrayGuard() {
        for (auto*& error : errors) {
          if (error != nullptr) {
            rocksdb_free(error);
            error = nullptr;
          }
        }
      }
    } error_guard{errors};

    using ColumnFamilyHandlePtr =
        std::unique_ptr<rocksdb_column_family_handle_t,
                        decltype(&rocksdb_column_family_handle_destroy)>;
    ColumnFamilyHandlePtr owned_default(nullptr,
                                        &rocksdb_column_family_handle_destroy);
    if (column_family == nullptr) {
      owned_default.reset(rocksdb_get_default_column_family_handle(db));
      column_family = owned_default.get();
    }
    rocksdb_batched_multi_get_cf_slice(
        db, options, column_family, num_keys, keys,
        batch->system_values.data(), errors.data(), sorted_input != 0);
    for (size_t i = 0; i < num_keys; ++i) {
      if (errors[i] != nullptr) {
        batch->errors.emplace(i, errors[i]);
        rocksdb_free(errors[i]);
        errors[i] = nullptr;
      }
    }
#else
    batch->result_indexes.resize(num_keys, kRustRocksDbBatchNotFound);
    if (num_keys == 0) {
      return batch.release();
    }

    DB* db_rep = RustRocksDbRep(db);
    ColumnFamilyHandle* column_family_rep =
        column_family == nullptr ? db_rep->DefaultColumnFamily()
                                 : RustRocksDbColumnFamilyRep(column_family);
    const auto* read_options = reinterpret_cast<const ReadOptions*>(options);
    const auto* key_slices = reinterpret_cast<const Slice*>(keys);
    std::vector<PinnableSlice> values(num_keys);
    std::vector<Status> statuses(num_keys);

    db_rep->MultiGet(*read_options, column_family_rep, num_keys, key_slices,
                     values.data(), statuses.data(), sorted_input != 0);

    size_t hit_count = 0;
    for (const auto& status : statuses) {
      if (status.ok()) {
        ++hit_count;
      }
    }
    batch->values.reserve(hit_count);
    for (size_t i = 0; i < num_keys; ++i) {
      if (statuses[i].ok()) {
        batch->result_indexes[i] = batch->values.size();
        batch->values.emplace_back(std::move(values[i]));
      } else if (!statuses[i].IsNotFound()) {
        batch->result_indexes[i] = kRustRocksDbBatchError;
        batch->errors.emplace(i, statuses[i].ToString());
      }
    }
#endif
    return batch.release();
  } catch (const std::exception& error) {
    RustSaveMessage(errptr, error.what());
    return nullptr;
  } catch (...) {
    RustSaveMessage(errptr, "unknown C++ exception in pinned MultiGet");
    return nullptr;
  }
}

extern "C" size_t rust_rocksdb_pinnable_batch_len(
    const rust_rocksdb_pinnable_batch_t* batch) {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
  return batch->system_values.size();
#else
  return batch->result_indexes.size();
#endif
}

extern "C" unsigned char rust_rocksdb_pinnable_batch_get(
    const rust_rocksdb_pinnable_batch_t* batch, size_t index,
    const char** value, size_t* value_len, const char** error,
    size_t* error_len) {
  *value = nullptr;
  *value_len = 0;
  *error = nullptr;
  *error_len = 0;

  // Bounds are checked here rather than asserted. The vendored build defines
  // NDEBUG, so an assert would compile away and leave an unchecked
  // `std::vector::operator[]` feeding a pointer and length straight into
  // `slice::from_raw_parts` on the Rust side.
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
  if (index >= batch->system_values.size()) {
    return rust_rocksdb_pinnable_batch_out_of_range;
  }
  const auto error_iter = batch->errors.find(index);
  if (error_iter != batch->errors.end()) {
    *error = error_iter->second.data();
    *error_len = error_iter->second.size();
    return rust_rocksdb_pinnable_batch_error;
  }
  if (batch->system_values[index] == nullptr) {
    return rust_rocksdb_pinnable_batch_not_found;
  }
  *value =
      rocksdb_pinnableslice_value(batch->system_values[index], value_len);
  return rust_rocksdb_pinnable_batch_found;
#else
  if (index >= batch->result_indexes.size()) {
    return rust_rocksdb_pinnable_batch_out_of_range;
  }
  const size_t result_index = batch->result_indexes[index];
  if (result_index == kRustRocksDbBatchNotFound) {
    return rust_rocksdb_pinnable_batch_not_found;
  }
  if (result_index == kRustRocksDbBatchError) {
    const auto error_iter = batch->errors.find(index);
    if (error_iter == batch->errors.end()) {
      return rust_rocksdb_pinnable_batch_out_of_range;
    }
    *error = error_iter->second.data();
    *error_len = error_iter->second.size();
    return rust_rocksdb_pinnable_batch_error;
  }
  if (result_index >= batch->values.size()) {
    return rust_rocksdb_pinnable_batch_out_of_range;
  }

  *value = batch->values[result_index].data();
  *value_len = batch->values[result_index].size();
  return rust_rocksdb_pinnable_batch_found;
#endif
}

extern "C" void rust_rocksdb_pinnable_batch_destroy(
    rust_rocksdb_pinnable_batch_t* batch) {
  delete batch;
}

extern "C" void rust_rocksdb_batched_multi_get_cf_slice_safe(
    rocksdb_t* db, const rocksdb_readoptions_t* options,
    rocksdb_column_family_handle_t* column_family, size_t num_keys,
    const rocksdb_slice_t* keys, rocksdb_pinnableslice_t** values, char** errors,
    unsigned char sorted_input, char** errptr) {
  for (size_t i = 0; i < num_keys; ++i) {
    values[i] = nullptr;
    errors[i] = nullptr;
  }
  try {
    rocksdb_batched_multi_get_cf_slice(
        db, options, column_family, num_keys, keys, values, errors,
        sorted_input != 0);
  } catch (const std::exception& error) {
    for (size_t i = 0; i < num_keys; ++i) {
      if (values[i] != nullptr) {
        rocksdb_pinnableslice_destroy(values[i]);
        values[i] = nullptr;
      }
      if (errors[i] != nullptr) {
        rocksdb_free(errors[i]);
        errors[i] = nullptr;
      }
    }
    RustSaveMessage(errptr, error.what());
  } catch (...) {
    for (size_t i = 0; i < num_keys; ++i) {
      if (values[i] != nullptr) {
        rocksdb_pinnableslice_destroy(values[i]);
        values[i] = nullptr;
      }
      if (errors[i] != nullptr) {
        rocksdb_free(errors[i]);
        errors[i] = nullptr;
      }
    }
    RustSaveMessage(errptr, "unknown C++ exception in batched MultiGet");
  }
}

extern "C" void rust_rocksdb_create_iterators_safe(
    rocksdb_t* db, rocksdb_readoptions_t* options,
    rocksdb_column_family_handle_t** column_families,
    rocksdb_iterator_t** iterators, size_t size, char** errptr) {
  for (size_t i = 0; i < size; ++i) {
    iterators[i] = nullptr;
  }
  try {
    rocksdb_create_iterators(db, options, column_families, iterators, size,
                             errptr);
  } catch (const std::exception& error) {
    for (size_t i = 0; i < size; ++i) {
      if (iterators[i] != nullptr) {
        rocksdb_iter_destroy(iterators[i]);
        iterators[i] = nullptr;
      }
    }
    RustSaveMessage(errptr, error.what());
  } catch (...) {
    for (size_t i = 0; i < size; ++i) {
      if (iterators[i] != nullptr) {
        rocksdb_iter_destroy(iterators[i]);
        iterators[i] = nullptr;
      }
    }
    RustSaveMessage(errptr, "unknown C++ exception while creating iterators");
  }
}

// -----------------------------------------------------------------------------
// Slice-based vectored WriteBatch operations
// -----------------------------------------------------------------------------

#ifndef RUST_ROCKSDB_SYSTEM_BACKEND
static WriteBatch* RustRocksDbWriteBatchRep(rocksdb_writebatch_t* batch) {
  return reinterpret_cast<WriteBatch*>(batch);
}

static SliceParts RustRocksDbSliceParts(int count,
                                       const rocksdb_slice_t* parts) {
  return SliceParts(reinterpret_cast<const Slice*>(parts), count);
}
#endif

template <typename Operation>
static void RustRocksDbWriteBatchCall(char** errptr, Operation operation) {
  try {
    RustSaveError(errptr, operation());
  } catch (const std::exception& error) {
    RustSaveMessage(errptr, error.what());
  } catch (...) {
    RustSaveMessage(errptr, "unknown C++ exception in vectored WriteBatch");
  }
}

#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
struct RustRocksDbSliceArrays {
  std::vector<const char*> pointers;
  std::vector<size_t> lengths;

  RustRocksDbSliceArrays(int count, const rocksdb_slice_t* slices) {
    pointers.reserve(count);
    lengths.reserve(count);
    for (int i = 0; i < count; ++i) {
      pointers.push_back(slices[i].data);
      lengths.push_back(slices[i].size);
    }
  }
};

static void RustRocksDbCheckWriteBatchCount(rocksdb_writebatch_t* batch,
                                            int before, char** errptr) {
  if (rocksdb_writebatch_count(batch) != before + 1) {
    RustSaveMessage(errptr, "RocksDB rejected the vectored WriteBatch operation");
  }
}
#endif

extern "C" void rust_rocksdb_writebatch_put_slices(
    rocksdb_writebatch_t* batch, int key_count, const rocksdb_slice_t* keys,
    int value_count, const rocksdb_slice_t* values, char** errptr) {
  RustRocksDbWriteBatchCall(errptr, [&] {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
    const int before = rocksdb_writebatch_count(batch);
    RustRocksDbSliceArrays key_parts(key_count, keys);
    RustRocksDbSliceArrays value_parts(value_count, values);
    rocksdb_writebatch_putv(batch, key_count, key_parts.pointers.data(),
                            key_parts.lengths.data(), value_count,
                            value_parts.pointers.data(),
                            value_parts.lengths.data());
    RustRocksDbCheckWriteBatchCount(batch, before, errptr);
    return Status::OK();
#else
    return RustRocksDbWriteBatchRep(batch)->Put(
        RustRocksDbSliceParts(key_count, keys),
        RustRocksDbSliceParts(value_count, values));
#endif
  });
}

extern "C" void rust_rocksdb_writebatch_put_slices_cf(
    rocksdb_writebatch_t* batch,
    rocksdb_column_family_handle_t* column_family, int key_count,
    const rocksdb_slice_t* keys, int value_count,
    const rocksdb_slice_t* values, char** errptr) {
  RustRocksDbWriteBatchCall(errptr, [&] {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
    const int before = rocksdb_writebatch_count(batch);
    RustRocksDbSliceArrays key_parts(key_count, keys);
    RustRocksDbSliceArrays value_parts(value_count, values);
    rocksdb_writebatch_putv_cf(
        batch, column_family, key_count, key_parts.pointers.data(),
        key_parts.lengths.data(), value_count, value_parts.pointers.data(),
        value_parts.lengths.data());
    RustRocksDbCheckWriteBatchCount(batch, before, errptr);
    return Status::OK();
#else
    return RustRocksDbWriteBatchRep(batch)->Put(
        RustRocksDbColumnFamilyRep(column_family),
        RustRocksDbSliceParts(key_count, keys),
        RustRocksDbSliceParts(value_count, values));
#endif
  });
}

extern "C" void rust_rocksdb_writebatch_merge_slices(
    rocksdb_writebatch_t* batch, int key_count, const rocksdb_slice_t* keys,
    int value_count, const rocksdb_slice_t* values, char** errptr) {
  RustRocksDbWriteBatchCall(errptr, [&] {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
    const int before = rocksdb_writebatch_count(batch);
    RustRocksDbSliceArrays key_parts(key_count, keys);
    RustRocksDbSliceArrays value_parts(value_count, values);
    rocksdb_writebatch_mergev(batch, key_count, key_parts.pointers.data(),
                              key_parts.lengths.data(), value_count,
                              value_parts.pointers.data(),
                              value_parts.lengths.data());
    RustRocksDbCheckWriteBatchCount(batch, before, errptr);
    return Status::OK();
#else
    return RustRocksDbWriteBatchRep(batch)->Merge(
        RustRocksDbSliceParts(key_count, keys),
        RustRocksDbSliceParts(value_count, values));
#endif
  });
}

extern "C" void rust_rocksdb_writebatch_merge_slices_cf(
    rocksdb_writebatch_t* batch,
    rocksdb_column_family_handle_t* column_family, int key_count,
    const rocksdb_slice_t* keys, int value_count,
    const rocksdb_slice_t* values, char** errptr) {
  RustRocksDbWriteBatchCall(errptr, [&] {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
    const int before = rocksdb_writebatch_count(batch);
    RustRocksDbSliceArrays key_parts(key_count, keys);
    RustRocksDbSliceArrays value_parts(value_count, values);
    rocksdb_writebatch_mergev_cf(
        batch, column_family, key_count, key_parts.pointers.data(),
        key_parts.lengths.data(), value_count, value_parts.pointers.data(),
        value_parts.lengths.data());
    RustRocksDbCheckWriteBatchCount(batch, before, errptr);
    return Status::OK();
#else
    return RustRocksDbWriteBatchRep(batch)->Merge(
        RustRocksDbColumnFamilyRep(column_family),
        RustRocksDbSliceParts(key_count, keys),
        RustRocksDbSliceParts(value_count, values));
#endif
  });
}

extern "C" void rust_rocksdb_writebatch_delete_slices(
    rocksdb_writebatch_t* batch, int key_count, const rocksdb_slice_t* keys,
    char** errptr) {
  RustRocksDbWriteBatchCall(errptr, [&] {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
    const int before = rocksdb_writebatch_count(batch);
    RustRocksDbSliceArrays key_parts(key_count, keys);
    rocksdb_writebatch_deletev(batch, key_count, key_parts.pointers.data(),
                               key_parts.lengths.data());
    RustRocksDbCheckWriteBatchCount(batch, before, errptr);
    return Status::OK();
#else
    return RustRocksDbWriteBatchRep(batch)->Delete(
        RustRocksDbSliceParts(key_count, keys));
#endif
  });
}

extern "C" void rust_rocksdb_writebatch_delete_slices_cf(
    rocksdb_writebatch_t* batch,
    rocksdb_column_family_handle_t* column_family, int key_count,
    const rocksdb_slice_t* keys, char** errptr) {
  RustRocksDbWriteBatchCall(errptr, [&] {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
    const int before = rocksdb_writebatch_count(batch);
    RustRocksDbSliceArrays key_parts(key_count, keys);
    rocksdb_writebatch_deletev_cf(batch, column_family, key_count,
                                  key_parts.pointers.data(),
                                  key_parts.lengths.data());
    RustRocksDbCheckWriteBatchCount(batch, before, errptr);
    return Status::OK();
#else
    return RustRocksDbWriteBatchRep(batch)->Delete(
        RustRocksDbColumnFamilyRep(column_family),
        RustRocksDbSliceParts(key_count, keys));
#endif
  });
}

extern "C" void rust_rocksdb_writebatch_delete_range_slices(
    rocksdb_writebatch_t* batch, int begin_count,
    const rocksdb_slice_t* begin, int end_count, const rocksdb_slice_t* end,
    char** errptr) {
  RustRocksDbWriteBatchCall(errptr, [&] {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
    if (begin_count != end_count) {
      RustSaveMessage(errptr,
                      "system RocksDB requires equal range-bound part counts");
      return Status::OK();
    }
    const int before = rocksdb_writebatch_count(batch);
    RustRocksDbSliceArrays begin_parts(begin_count, begin);
    RustRocksDbSliceArrays end_parts(end_count, end);
    rocksdb_writebatch_delete_rangev(
        batch, begin_count, begin_parts.pointers.data(),
        begin_parts.lengths.data(), end_parts.pointers.data(),
        end_parts.lengths.data());
    RustRocksDbCheckWriteBatchCount(batch, before, errptr);
    return Status::OK();
#else
    return RustRocksDbWriteBatchRep(batch)->DeleteRange(
        RustRocksDbSliceParts(begin_count, begin),
        RustRocksDbSliceParts(end_count, end));
#endif
  });
}

extern "C" void rust_rocksdb_writebatch_delete_range_slices_cf(
    rocksdb_writebatch_t* batch,
    rocksdb_column_family_handle_t* column_family, int begin_count,
    const rocksdb_slice_t* begin, int end_count, const rocksdb_slice_t* end,
    char** errptr) {
  RustRocksDbWriteBatchCall(errptr, [&] {
#ifdef RUST_ROCKSDB_SYSTEM_BACKEND
    if (begin_count != end_count) {
      RustSaveMessage(errptr,
                      "system RocksDB requires equal range-bound part counts");
      return Status::OK();
    }
    const int before = rocksdb_writebatch_count(batch);
    RustRocksDbSliceArrays begin_parts(begin_count, begin);
    RustRocksDbSliceArrays end_parts(end_count, end);
    rocksdb_writebatch_delete_rangev_cf(
        batch, column_family, begin_count, begin_parts.pointers.data(),
        begin_parts.lengths.data(), end_parts.pointers.data(),
        end_parts.lengths.data());
    RustRocksDbCheckWriteBatchCount(batch, before, errptr);
    return Status::OK();
#else
    return RustRocksDbWriteBatchRep(batch)->DeleteRange(
        RustRocksDbColumnFamilyRep(column_family),
        RustRocksDbSliceParts(begin_count, begin),
        RustRocksDbSliceParts(end_count, end));
#endif
  });
}

// -----------------------------------------------------------------------------
// Null-safe Snapshot::GetSequenceNumber
//
// `rocksdb_snapshot_t` is `struct { const Snapshot* rep; }` (db/c.cc), so a
// pointer to the opaque handle is also a pointer to its `rep` field. See the
// layout note above for why this file uses a reinterpret_cast rather than
// re-declaring the struct.
// -----------------------------------------------------------------------------

extern "C" unsigned char rust_rocksdb_snapshot_try_get_sequence_number(
    const rocksdb_snapshot_t* snapshot, uint64_t* seqno) {
  if (snapshot == nullptr) {
    return 0;
  }
  const Snapshot* rep = *reinterpret_cast<const Snapshot* const*>(snapshot);
  if (rep == nullptr) {
    // A transaction started without `set_snapshot(true)` yields a wrapper whose
    // `rep` is null; upstream's getter would dereference it.
    return 0;
  }
  *seqno = rep->GetSequenceNumber();
  return 1;
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

// -----------------------------------------------------------------------------
// Statistics reset
//
// `rocksdb_options_t` wraps `Options` as its first member, so a
// `reinterpret_cast` recovers the C++ object (see the SstFileReader block).
// -----------------------------------------------------------------------------

extern "C" void rust_rocksdb_options_statistics_reset(rocksdb_options_t* opt,
                                                      char** errptr) {
  auto* options = reinterpret_cast<Options*>(opt);
  if (options->statistics == nullptr) {
    return;
  }
  RustSaveError(errptr, options->statistics->Reset());
}

// -----------------------------------------------------------------------------
// Top-bits SST partitioner
//
// Buckets a key by the top `bits` bits of its first 4 bytes (zero-padded,
// big-endian) and cuts compaction output files whenever the bucket changes.
// The bucket function must stay byte-for-byte equivalent to the archival
// backfill's `PartitionedSstWriter::partition_id`, so live-compacted SSTs
// never straddle a backfill partition boundary.
// -----------------------------------------------------------------------------

namespace {

class TopBitsSstPartitioner : public ROCKSDB_NAMESPACE::SstPartitioner {
 public:
  explicit TopBitsSstPartitioner(uint32_t bits) : bits_(bits) {}

  const char* Name() const override { return "TopBitsSstPartitioner"; }

  ROCKSDB_NAMESPACE::PartitionerResult ShouldPartition(
      const ROCKSDB_NAMESPACE::PartitionerRequest& request) override {
    return Bucket(*request.prev_user_key) != Bucket(*request.current_user_key)
               ? ROCKSDB_NAMESPACE::kRequired
               : ROCKSDB_NAMESPACE::kNotRequired;
  }

  bool CanDoTrivialMove(const Slice& smallest_user_key,
                        const Slice& largest_user_key) override {
    return Bucket(smallest_user_key) == Bucket(largest_user_key);
  }

 private:
  uint32_t Bucket(const Slice& key) const {
    unsigned char buf[4] = {0, 0, 0, 0};
    size_t len = key.size() < 4 ? key.size() : 4;
    std::memcpy(buf, key.data(), len);
    uint32_t prefix = (uint32_t{buf[0]} << 24) | (uint32_t{buf[1]} << 16) |
                      (uint32_t{buf[2]} << 8) | uint32_t{buf[3]};
    return prefix >> (32 - bits_);
  }

  uint32_t bits_;
};

class TopBitsSstPartitionerFactory
    : public ROCKSDB_NAMESPACE::SstPartitionerFactory {
 public:
  explicit TopBitsSstPartitionerFactory(uint32_t bits) : bits_(bits) {}

  const char* Name() const override { return "TopBitsSstPartitionerFactory"; }

  std::unique_ptr<ROCKSDB_NAMESPACE::SstPartitioner> CreatePartitioner(
      const ROCKSDB_NAMESPACE::SstPartitioner::Context& /* context */)
      const override {
    return std::make_unique<TopBitsSstPartitioner>(bits_);
  }

 private:
  uint32_t bits_;
};

}  // namespace

extern "C" void rust_rocksdb_options_set_top_bits_sst_partitioner(
    rocksdb_options_t* opt, uint32_t bits) {
  if (bits < 1) {
    bits = 1;
  } else if (bits > 32) {
    bits = 32;
  }
  auto* options = reinterpret_cast<Options*>(opt);
  options->sst_partitioner_factory =
      std::make_shared<TopBitsSstPartitionerFactory>(bits);
}

// -----------------------------------------------------------------------------
// Two-phase external file ingestion
//
// `rocksdb_t` and `rocksdb_column_family_handle_t` hold their C++ object
// pointer as the first struct member, and
// `rocksdb_ingestexternalfileoptions_t` wraps `IngestExternalFileOptions` by
// value as its first member, so a `reinterpret_cast` recovers each C++
// object (see the SstFileReader block). The prepared-ingestion handle gets
// its own opaque struct, owned entirely by this translation unit.
// -----------------------------------------------------------------------------

struct rust_rocksdb_file_ingestion_handle_t {
  std::unique_ptr<ROCKSDB_NAMESPACE::FileIngestionHandle> rep;
};

extern "C" rust_rocksdb_file_ingestion_handle_t*
rust_rocksdb_prepare_file_ingestion_cf(
    rocksdb_t* db, rocksdb_column_family_handle_t* handle,
    const rocksdb_ingestexternalfileoptions_t* opt,
    const char* const* file_list, size_t list_len, char** errptr) {
  ROCKSDB_NAMESPACE::IngestExternalFileArg arg;
  arg.column_family =
      *reinterpret_cast<ROCKSDB_NAMESPACE::ColumnFamilyHandle* const*>(handle);
  arg.external_files.reserve(list_len);
  for (size_t i = 0; i < list_len; ++i) {
    arg.external_files.emplace_back(file_list[i]);
  }
  arg.options = *reinterpret_cast<
      const ROCKSDB_NAMESPACE::IngestExternalFileOptions*>(opt);

  std::unique_ptr<ROCKSDB_NAMESPACE::FileIngestionHandle> prepared;
  ROCKSDB_NAMESPACE::Status status =
      (*reinterpret_cast<DB* const*>(db))->PrepareFileIngestion({arg},
                                                                &prepared);
  if (RustSaveError(errptr, status)) {
    return nullptr;
  }
  return new rust_rocksdb_file_ingestion_handle_t{std::move(prepared)};
}

extern "C" void rust_rocksdb_commit_file_ingestion_handles(
    rocksdb_t* db, rust_rocksdb_file_ingestion_handle_t* const* handles,
    size_t handles_len, char** errptr) {
  std::vector<std::unique_ptr<ROCKSDB_NAMESPACE::FileIngestionHandle>>
      to_commit;
  to_commit.reserve(handles_len);
  for (size_t i = 0; i < handles_len; ++i) {
    to_commit.push_back(std::move(handles[i]->rep));
    delete handles[i];
  }
  RustSaveError(errptr, (*reinterpret_cast<DB* const*>(db))
                            ->CommitFileIngestionHandles(std::move(to_commit)));
}

extern "C" void rust_rocksdb_file_ingestion_handle_destroy(
    rust_rocksdb_file_ingestion_handle_t* handle) {
  // The unique_ptr destructor rolls the prepared ingestion back.
  delete handle;
}

// -----------------------------------------------------------------------------
// ReadScopedBlockBufferProvider
//
// Adapts the C callback triple into the C++ `ReadScopedBlockBufferProvider`
// interface (rocksdb/table.h). Each successful `Allocate()` registers the
// release callback on a fresh `SharedCleanablePtr`, which RocksDB attaches to
// the blocks and pinned slices backed by the lease; the callback runs once,
// on whichever thread drops the last reference. `rocksdb_readoptions_t`
// wraps `ReadOptions` as its first member, so a `reinterpret_cast` recovers
// it (see the SstFileReader block).
// -----------------------------------------------------------------------------

namespace {

class CBlockBufferProvider
    : public ROCKSDB_NAMESPACE::ReadScopedBlockBufferProvider {
 public:
  CBlockBufferProvider(void* state,
                       rust_rocksdb_block_buffer_allocate_cb allocate,
                       rust_rocksdb_block_buffer_release_cb release,
                       rust_rocksdb_block_buffer_provider_destroy_cb destroy)
      : state_(state),
        allocate_(allocate),
        release_(release),
        destroy_(destroy) {}

  ~CBlockBufferProvider() override {
    if (destroy_ != nullptr) {
      destroy_(state_);
    }
  }

  Status Allocate(size_t size, size_t alignment, Lease* out) override {
    char* data = nullptr;
    size_t data_size = 0;
    void* lease_state = nullptr;
    if (allocate_(state_, size, alignment, &data, &data_size, &lease_state) ==
        0) {
      return Status::MemoryLimit(
          "read-scoped block buffer provider failed to allocate");
    }
    out->data = data;
    out->size = data_size;
    out->cleanup.Allocate();
    out->cleanup->RegisterCleanup(&CBlockBufferProvider::ReleaseLease, this,
                                  lease_state);
    return Status::OK();
  }

 private:
  static void ReleaseLease(void* arg1, void* arg2) {
    auto* provider = static_cast<CBlockBufferProvider*>(arg1);
    provider->release_(provider->state_, arg2);
  }

  void* state_;
  rust_rocksdb_block_buffer_allocate_cb allocate_;
  rust_rocksdb_block_buffer_release_cb release_;
  rust_rocksdb_block_buffer_provider_destroy_cb destroy_;
};

}  // namespace

struct rust_rocksdb_read_scoped_block_buffer_provider_t {
  CBlockBufferProvider* rep;
};

extern "C" rust_rocksdb_read_scoped_block_buffer_provider_t*
rust_rocksdb_read_scoped_block_buffer_provider_create(
    void* state, rust_rocksdb_block_buffer_allocate_cb allocate,
    rust_rocksdb_block_buffer_release_cb release,
    rust_rocksdb_block_buffer_provider_destroy_cb destroy) {
  auto* provider = new rust_rocksdb_read_scoped_block_buffer_provider_t;
  provider->rep = new CBlockBufferProvider(state, allocate, release, destroy);
  return provider;
}

extern "C" void rust_rocksdb_read_scoped_block_buffer_provider_destroy(
    rust_rocksdb_read_scoped_block_buffer_provider_t* provider) {
  delete provider->rep;
  delete provider;
}

extern "C" void rust_rocksdb_readoptions_set_read_scoped_block_buffer_provider(
    rocksdb_readoptions_t* options,
    rust_rocksdb_read_scoped_block_buffer_provider_t* provider) {
  reinterpret_cast<ReadOptions*>(options)->read_scoped_block_buffer_provider =
      provider != nullptr ? provider->rep : nullptr;
}

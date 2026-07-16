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
#include "rocksdb/sst_partitioner.h"
#include "rocksdb/statistics.h"

using ROCKSDB_NAMESPACE::BackgroundErrorRecoveryInfo;
using ROCKSDB_NAMESPACE::CompactionJobInfo;
using ROCKSDB_NAMESPACE::DB;
using ROCKSDB_NAMESPACE::EventListener;
using ROCKSDB_NAMESPACE::ExternalFileIngestionInfo;
using ROCKSDB_NAMESPACE::FlushJobInfo;
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

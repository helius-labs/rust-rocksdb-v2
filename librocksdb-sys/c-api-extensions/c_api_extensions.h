/*
 * Local additions to the RocksDB C API.
 *
 * RocksDB's upstream C wrapper (`include/rocksdb/c.h`, `db/c.cc`) is
 * maintained reactively: new C++ options land in the C++ headers first, and
 * a matching C wrapper is added only when someone requests it. This crate
 * exposes the C++ feature surface through that C API, so any feature without
 * a C wrapper is unreachable from Rust.
 *
 * This header declares C wrappers for C++ options that don't have one yet
 * upstream. The matching definitions live in `c_api_extensions.cc`. Both
 * are compiled and linked alongside the vendored RocksDB sources (or
 * alongside a system-installed librocksdb, on the System backend); the
 * submodule is NEVER modified. Bindgen reads this header as its primary
 * input, so the new symbols flow into `rust-librocksdb-sys`'s generated
 * bindings.rs without any special-casing.
 *
 * Each declaration here mirrors an upstream PR against
 * facebook/rocksdb (see this file's comments and the project CHANGELOG).
 * When upstream lands the matching PR and we bump the submodule to a
 * release containing it, the local entry here can be deleted and the
 * binding falls through to the upstream symbol automatically.
 */

#ifndef RUST_LIBROCKSDB_SYS_C_API_EXTENSIONS_H_
#define RUST_LIBROCKSDB_SYS_C_API_EXTENSIONS_H_

/* Pull in every C-API type the extension functions reference. By including
 * c.h here (instead of forward-declaring), this header is a clean superset
 * of c.h: bindgen scanning this file generates declarations for everything
 * in the upstream C API plus our local additions, with no risk of missed
 * types. */
#include "rocksdb/c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Options::open_files_async compatibility wrapper. */
extern ROCKSDB_LIBRARY_API unsigned char
rust_rocksdb_options_open_files_async_supported(void);
extern ROCKSDB_LIBRARY_API unsigned char
rust_rocksdb_options_set_open_files_async(rocksdb_options_t*, unsigned char);
extern ROCKSDB_LIBRARY_API unsigned char
rust_rocksdb_options_get_open_files_async(rocksdb_options_t*);

/* -------------------------------------------------------------------------
 * Batch-owned pinned MultiGet results
 *
 * The upstream batched C API allocates one rocksdb_pinnableslice_t wrapper
 * per successful key. This additive API keeps the PinnableSlice values in one
 * owner so Rust can borrow every result without per-key wrapper allocation.
 * ------------------------------------------------------------------------- */
typedef struct rust_rocksdb_pinnable_batch_t rust_rocksdb_pinnable_batch_t;

enum {
  rust_rocksdb_pinnable_batch_not_found = 0,
  rust_rocksdb_pinnable_batch_found = 1,
  rust_rocksdb_pinnable_batch_error = 2,
  /* index outside the batch, or a batch whose internal state does not agree
     with itself. No out-params are written. */
  rust_rocksdb_pinnable_batch_out_of_range = 3,
};

/* A null `column_family` selects the default column family. */
extern ROCKSDB_LIBRARY_API rust_rocksdb_pinnable_batch_t*
rust_rocksdb_batched_multi_get_pinned(
    rocksdb_t* db, const rocksdb_readoptions_t* options,
    rocksdb_column_family_handle_t* column_family, size_t num_keys,
    const rocksdb_slice_t* keys, unsigned char sorted_input, char** errptr);
/* Creates an empty batch for `rust_rocksdb_batched_multi_get_pinned_into`. */
extern ROCKSDB_LIBRARY_API rust_rocksdb_pinnable_batch_t*
rust_rocksdb_pinnable_batch_create(void);
/* Releases the batch's values (unpinning any block-cache handles they held)
   while retaining its internal buffers for the next fill; the length becomes
   0. */
extern ROCKSDB_LIBRARY_API void rust_rocksdb_pinnable_batch_reset(
    rust_rocksdb_pinnable_batch_t* batch);
/* Refills `batch` in place with the results for `keys`, mirroring
   `rust_rocksdb_batched_multi_get_pinned` but reusing the batch's internal
   buffers across calls on the vendored backend (the System backend refills
   without reuse — the public C API cannot reset a rocksdb_pinnableslice_t).
   Borrows handed out by `rust_rocksdb_pinnable_batch_get` are invalidated.
   On error a message is stored in `*errptr` and the batch is left reset
   (length 0). A null `column_family` selects the default column family. */
extern ROCKSDB_LIBRARY_API void rust_rocksdb_batched_multi_get_pinned_into(
    rust_rocksdb_pinnable_batch_t* batch, rocksdb_t* db,
    const rocksdb_readoptions_t* options,
    rocksdb_column_family_handle_t* column_family, size_t num_keys,
    const rocksdb_slice_t* keys, unsigned char sorted_input, char** errptr);
extern ROCKSDB_LIBRARY_API size_t rust_rocksdb_pinnable_batch_len(
    const rust_rocksdb_pinnable_batch_t* batch);
/* `value` and `error` are borrowed from `batch` and dangle once it is
   destroyed. Do not free them. */
extern ROCKSDB_LIBRARY_API unsigned char rust_rocksdb_pinnable_batch_get(
    const rust_rocksdb_pinnable_batch_t* batch, size_t index,
    const char** value, size_t* value_len, const char** error,
    size_t* error_len);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_pinnable_batch_destroy(
    rust_rocksdb_pinnable_batch_t* batch);
/* `column_family` must not be null, unlike
   `rust_rocksdb_batched_multi_get_pinned` above.
   The caller owns and must release: each non-null `values[i]` with
   `rocksdb_pinnableslice_destroy`, each non-null per-key `errors[i]` with
   `rocksdb_free`, and `*errptr` with `rocksdb_free`. */
extern ROCKSDB_LIBRARY_API void rust_rocksdb_batched_multi_get_cf_slice_safe(
    rocksdb_t* db, const rocksdb_readoptions_t* options,
    rocksdb_column_family_handle_t* column_family, size_t num_keys,
    const rocksdb_slice_t* keys, rocksdb_pinnableslice_t** values,
    char** errors, unsigned char sorted_input, char** errptr);
/* On success the caller owns every `iterators[i]` and must release it with
   `rocksdb_iter_destroy`. On failure no iterator is returned and `*errptr` is
   set. `options` must outlive the returned iterators: RocksDB keeps raw
   pointers to its iterate bounds and timestamps. */
extern ROCKSDB_LIBRARY_API void rust_rocksdb_create_iterators_safe(
    rocksdb_t* db, rocksdb_readoptions_t* options,
    rocksdb_column_family_handle_t** column_families,
    rocksdb_iterator_t** iterators, size_t size, char** errptr);

/* -------------------------------------------------------------------------
 * Slice-based vectored WriteBatch operations
 *
 * The upstream putv/mergev/deletev wrappers rebuild Slice arrays from
 * separate pointer and length arrays. These variants accept ABI-compatible
 * rocksdb_slice_t arrays directly.
 * ------------------------------------------------------------------------- */
extern ROCKSDB_LIBRARY_API void rust_rocksdb_writebatch_put_slices(
    rocksdb_writebatch_t*, int, const rocksdb_slice_t*, int,
    const rocksdb_slice_t*, char**);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_writebatch_put_slices_cf(
    rocksdb_writebatch_t*, rocksdb_column_family_handle_t*, int,
    const rocksdb_slice_t*, int, const rocksdb_slice_t*, char**);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_writebatch_merge_slices(
    rocksdb_writebatch_t*, int, const rocksdb_slice_t*, int,
    const rocksdb_slice_t*, char**);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_writebatch_merge_slices_cf(
    rocksdb_writebatch_t*, rocksdb_column_family_handle_t*, int,
    const rocksdb_slice_t*, int, const rocksdb_slice_t*, char**);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_writebatch_delete_slices(
    rocksdb_writebatch_t*, int, const rocksdb_slice_t*, char**);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_writebatch_delete_slices_cf(
    rocksdb_writebatch_t*, rocksdb_column_family_handle_t*, int,
    const rocksdb_slice_t*, char**);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_writebatch_delete_range_slices(
    rocksdb_writebatch_t*, int, const rocksdb_slice_t*, int,
    const rocksdb_slice_t*, char**);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_writebatch_delete_range_slices_cf(
    rocksdb_writebatch_t*, rocksdb_column_family_handle_t*, int,
    const rocksdb_slice_t*, int, const rocksdb_slice_t*, char**);

/* -------------------------------------------------------------------------
 * EventListener background error status severity and recovery callbacks
 *
 * RocksDB's C++ EventListener exposes Status::Severity on background errors
 * and has callbacks for the automatic error recovery lifecycle. The upstream
 * C listener wrapper available to this crate only forwards OnBackgroundError,
 * so the Rust listener uses this local additive wrapper instead of changing
 * the upstream rocksdb_eventlistener_create ABI.
 * ------------------------------------------------------------------------- */
typedef struct rust_rocksdb_status_t rust_rocksdb_status_t;
typedef struct rust_rocksdb_eventlistener_t rust_rocksdb_eventlistener_t;
typedef struct rust_rocksdb_background_error_recovery_info_t
    rust_rocksdb_background_error_recovery_info_t;

extern ROCKSDB_LIBRARY_API void rust_rocksdb_status_get_error(
    rust_rocksdb_status_t*, char**);
extern ROCKSDB_LIBRARY_API unsigned char rust_rocksdb_status_get_severity(
    rust_rocksdb_status_t*);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_status_reset(
    rust_rocksdb_status_t*);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_background_error_recovery_info_old_bg_error(
    const rust_rocksdb_background_error_recovery_info_t*, char**);
extern ROCKSDB_LIBRARY_API unsigned char
rust_rocksdb_background_error_recovery_info_old_bg_error_severity(
    const rust_rocksdb_background_error_recovery_info_t*);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_background_error_recovery_info_new_bg_error(
    const rust_rocksdb_background_error_recovery_info_t*, char**);
extern ROCKSDB_LIBRARY_API unsigned char
rust_rocksdb_background_error_recovery_info_new_bg_error_severity(
    const rust_rocksdb_background_error_recovery_info_t*);

typedef void (*rust_rocksdb_on_flush_begin_cb)(
    void*, const rocksdb_flushjobinfo_t*);
typedef void (*rust_rocksdb_on_flush_completed_cb)(
    void*, const rocksdb_flushjobinfo_t*);
typedef void (*rust_rocksdb_on_compaction_begin_cb)(
    void*, const rocksdb_compactionjobinfo_t*);
typedef void (*rust_rocksdb_on_compaction_completed_cb)(
    void*, const rocksdb_compactionjobinfo_t*);
typedef void (*rust_rocksdb_on_subcompaction_begin_cb)(
    void*, const rocksdb_subcompactionjobinfo_t*);
typedef void (*rust_rocksdb_on_subcompaction_completed_cb)(
    void*, const rocksdb_subcompactionjobinfo_t*);
typedef void (*rust_rocksdb_on_external_file_ingested_cb)(
    void*, const rocksdb_externalfileingestioninfo_t*);
typedef void (*rust_rocksdb_on_background_error_cb)(
    void*, uint32_t, rust_rocksdb_status_t*);
typedef void (*rust_rocksdb_on_error_recovery_begin_cb)(
    void*, uint32_t, rust_rocksdb_status_t*, unsigned char*);
typedef void (*rust_rocksdb_on_error_recovery_end_cb)(
    void*, const rust_rocksdb_background_error_recovery_info_t*);
typedef void (*rust_rocksdb_on_stall_conditions_changed_cb)(
    void*, const rocksdb_writestallinfo_t*);
typedef void (*rust_rocksdb_on_memtable_sealed_cb)(
    void*, const rocksdb_memtableinfo_t*);

extern ROCKSDB_LIBRARY_API rust_rocksdb_eventlistener_t*
rust_rocksdb_eventlistener_create(
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
    rust_rocksdb_on_memtable_sealed_cb on_memtable_sealed);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_eventlistener_destroy(
    rust_rocksdb_eventlistener_t*);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_options_add_eventlistener(
    rocksdb_options_t*, rust_rocksdb_eventlistener_t*);

/* -------------------------------------------------------------------------
 * Null-safe Snapshot::GetSequenceNumber
 *
 * `rocksdb_transaction_get_snapshot` always mallocs a `rocksdb_snapshot_t`
 * wrapper, but its `rep` is `Transaction::GetSnapshot()`, which is nullptr
 * unless the transaction was started with `set_snapshot(true)`. Upstream's
 * `rocksdb_snapshot_get_sequence_number` dereferences `rep`
 * unconditionally, so calling it on such a snapshot is a null dereference
 * reachable from safe Rust. `rep` is not visible from Rust because
 * `rocksdb_snapshot_t` is opaque, hence this accessor.
 *
 * Returns 1 and writes the sequence number through `seqno` when the
 * snapshot is present; returns 0 and leaves `seqno` untouched otherwise.
 * ------------------------------------------------------------------------- */
extern ROCKSDB_LIBRARY_API unsigned char
rust_rocksdb_snapshot_try_get_sequence_number(const rocksdb_snapshot_t*,
                                             uint64_t* seqno);

/* -------------------------------------------------------------------------
 * SstFileReader
 *
 * RocksDB's C++ `SstFileReader` (read keys/values directly from a standalone
 * .sst file, without a live DB) has no wrapper in upstream `c.h`. This block
 * exposes a reader handle plus a self-contained iterator over it.
 *
 * The iterator is its own type rather than the upstream `rocksdb_iterator_t`:
 * that struct's layout is private to `db/c.cc`, so it cannot be constructed
 * from this separate translation unit. The iterator functions below mirror
 * the upstream `rocksdb_iter_*` surface one-for-one.
 *
 * Mirrors the long-standing SstFileReader C API carried in downstream forks;
 * delete this block if/when an equivalent lands in upstream `c.h`.
 * ------------------------------------------------------------------------- */
typedef struct rust_rocksdb_sst_file_reader_t rust_rocksdb_sst_file_reader_t;
typedef struct rust_rocksdb_sst_file_reader_iterator_t
    rust_rocksdb_sst_file_reader_iterator_t;

extern ROCKSDB_LIBRARY_API rust_rocksdb_sst_file_reader_t*
rust_rocksdb_sst_file_reader_create(const rocksdb_options_t* options);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_open(
    rust_rocksdb_sst_file_reader_t* reader, const char* name, char** errptr);
extern ROCKSDB_LIBRARY_API rust_rocksdb_sst_file_reader_iterator_t*
rust_rocksdb_sst_file_reader_new_iterator(
    rust_rocksdb_sst_file_reader_t* reader,
    const rocksdb_readoptions_t* options);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_destroy(
    rust_rocksdb_sst_file_reader_t* reader);

extern ROCKSDB_LIBRARY_API unsigned char rust_rocksdb_sst_file_reader_iter_valid(
    const rust_rocksdb_sst_file_reader_iterator_t* iter);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_iter_seek_to_first(
    rust_rocksdb_sst_file_reader_iterator_t* iter);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_iter_seek_to_last(
    rust_rocksdb_sst_file_reader_iterator_t* iter);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_iter_seek(
    rust_rocksdb_sst_file_reader_iterator_t* iter, const char* k, size_t klen);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_iter_seek_for_prev(
    rust_rocksdb_sst_file_reader_iterator_t* iter, const char* k, size_t klen);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_iter_next(
    rust_rocksdb_sst_file_reader_iterator_t* iter);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_iter_prev(
    rust_rocksdb_sst_file_reader_iterator_t* iter);
extern ROCKSDB_LIBRARY_API const char* rust_rocksdb_sst_file_reader_iter_key(
    const rust_rocksdb_sst_file_reader_iterator_t* iter, size_t* klen);
extern ROCKSDB_LIBRARY_API const char* rust_rocksdb_sst_file_reader_iter_value(
    const rust_rocksdb_sst_file_reader_iterator_t* iter, size_t* vlen);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_iter_get_error(
    const rust_rocksdb_sst_file_reader_iterator_t* iter, char** errptr);
extern ROCKSDB_LIBRARY_API void rust_rocksdb_sst_file_reader_iter_destroy(
    rust_rocksdb_sst_file_reader_iterator_t* iter);

/* -------------------------------------------------------------------------
 * Statistics reset
 *
 * C++ `Statistics::Reset()` (rocksdb/statistics.h) zeroes all ticker and
 * histogram stats, but upstream `c.h` has no wrapper for it. This is a thin
 * additive wrapper over the statistics object owned by the options. A no-op
 * when statistics were never enabled.
 * ------------------------------------------------------------------------- */
extern ROCKSDB_LIBRARY_API void rust_rocksdb_options_statistics_reset(
    rocksdb_options_t* opt, char** errptr);

/* -------------------------------------------------------------------------
 * Top-bits SST partitioner
 *
 * C++ `ColumnFamilyOptions::sst_partitioner_factory`
 * (rocksdb/sst_partitioner.h) cuts compaction output files at caller-chosen
 * key boundaries. Upstream `c.h` only wraps the fixed-prefix factory, which
 * cannot express sub-byte boundaries. This wrapper installs a partitioner
 * that buckets keys by the top `bits` bits of their first 4 bytes
 * (zero-padded, big-endian) and cuts whenever the bucket changes — 2^bits
 * equal hash-space partitions. Compaction outputs only; flushes (L0) and
 * SstFileWriter files are unaffected.
 *
 * The factory is assigned directly onto the options instead of returning a
 * `rocksdb_sst_partitioner_factory_t*`: that struct's layout is private to
 * `db/c.cc`, so it cannot be constructed from this translation unit (same
 * constraint as the SstFileReader iterator above). `bits` outside 1..=32 is
 * clamped into range.
 * ------------------------------------------------------------------------- */
extern ROCKSDB_LIBRARY_API void
rust_rocksdb_options_set_top_bits_sst_partitioner(rocksdb_options_t* opt,
                                                  uint32_t bits);

/* -------------------------------------------------------------------------
 * Two-phase external file ingestion
 *
 * C++ `DB::PrepareFileIngestion()` / `DB::CommitFileIngestionHandles()`
 * (rocksdb/db.h) split external SST ingestion into a mutex-free prepare
 * phase and a short atomic commit that can cover handles for many column
 * families in a single MANIFEST write. Upstream `c.h` only wraps the
 * single-shot `IngestExternalFile(s)` entry points.
 *
 * `rust_rocksdb_prepare_file_ingestion_cf` prepares one column family's
 * files and returns an opaque handle (NULL on error). Handles targeting the
 * same column family must be prepared with the same options and are
 * committed in argument order (later handles win for overlapping keys).
 * `rust_rocksdb_commit_file_ingestion_handles` consumes every passed handle
 * whether it succeeds or fails (on failure RocksDB rolls all of them back);
 * the pointers must not be used or destroyed afterwards.
 * `rust_rocksdb_file_ingestion_handle_destroy` rolls back and frees a
 * handle that will not be committed. Handles must not outlive the DB.
 * ------------------------------------------------------------------------- */
typedef struct rust_rocksdb_file_ingestion_handle_t
    rust_rocksdb_file_ingestion_handle_t;

extern ROCKSDB_LIBRARY_API rust_rocksdb_file_ingestion_handle_t*
rust_rocksdb_prepare_file_ingestion_cf(
    rocksdb_t* db, rocksdb_column_family_handle_t* handle,
    const rocksdb_ingestexternalfileoptions_t* opt,
    const char* const* file_list, size_t list_len, char** errptr);

extern ROCKSDB_LIBRARY_API void rust_rocksdb_commit_file_ingestion_handles(
    rocksdb_t* db, rust_rocksdb_file_ingestion_handle_t* const* handles,
    size_t handles_len, char** errptr);

extern ROCKSDB_LIBRARY_API void rust_rocksdb_file_ingestion_handle_destroy(
    rust_rocksdb_file_ingestion_handle_t* handle);

/* -------------------------------------------------------------------------
 * ReadScopedBlockBufferProvider
 *
 * C++ `ReadOptions::read_scoped_block_buffer_provider` (rocksdb/options.h,
 * EXPERIMENTAL) lets scans place final data-block contents in caller-provided
 * buffers instead of RocksDB-owned block memory, bypassing the data-block
 * cache. Support is limited to block-based table iterators and MultiScan
 * reads; mmap reads ignore the provider. Upstream calls this a C++-only
 * option, so no `c.h` wrapper exists.
 *
 * `allocate` is called on whichever thread performs the read. On success it
 * returns non-zero and fills `data` (writable memory for the block),
 * `data_size` (usable bytes; must be >= `size`, and a multiple of `alignment`
 * when `alignment` > 1) and `lease_state` (opaque per-lease handle). `data`
 * must be aligned to `alignment` bytes (a power of two; 1 means unaligned).
 * Returning zero fails the read with a memory-limit status.
 *
 * `release` is called exactly once per successful allocation, on whichever
 * thread drops the last RocksDB reference to the block — including on later
 * I/O or decompression failure. The memory behind `data` must stay valid
 * until then.
 *
 * The provider must outlive every ReadOptions that references it and every
 * lease it has handed out. `destroy` is called when the provider wrapper is
 * destroyed.
 * ------------------------------------------------------------------------- */
typedef struct rust_rocksdb_read_scoped_block_buffer_provider_t
    rust_rocksdb_read_scoped_block_buffer_provider_t;

typedef unsigned char (*rust_rocksdb_block_buffer_allocate_cb)(
    void* state, size_t size, size_t alignment, char** data, size_t* data_size,
    void** lease_state);
typedef void (*rust_rocksdb_block_buffer_release_cb)(void* state,
                                                     void* lease_state);
typedef void (*rust_rocksdb_block_buffer_provider_destroy_cb)(void* state);

extern ROCKSDB_LIBRARY_API rust_rocksdb_read_scoped_block_buffer_provider_t*
rust_rocksdb_read_scoped_block_buffer_provider_create(
    void* state, rust_rocksdb_block_buffer_allocate_cb allocate,
    rust_rocksdb_block_buffer_release_cb release,
    rust_rocksdb_block_buffer_provider_destroy_cb destroy);

extern ROCKSDB_LIBRARY_API void
rust_rocksdb_read_scoped_block_buffer_provider_destroy(
    rust_rocksdb_read_scoped_block_buffer_provider_t* provider);

/* Passing NULL clears the provider. The provider is not owned by the read
 * options and must outlive them. */
extern ROCKSDB_LIBRARY_API void
rust_rocksdb_readoptions_set_read_scoped_block_buffer_provider(
    rocksdb_readoptions_t* options,
    rust_rocksdb_read_scoped_block_buffer_provider_t* provider);

/* -------------------------------------------------------------------------
 * Extended perf-context metrics
 *
 * C++ `PerfContext` (rocksdb/perf_context.h) has grown well past the metric
 * enum in upstream `c.h`: 36 fields — CPU-time nanos, iterator op counts,
 * block-cache hit/read breakdowns by block type, secondary-cache stats,
 * write-path scheduling/wait nanos, file-ingestion nanos and the MultiScan
 * counters — have no `rocksdb_perfcontext_metric` value. This enum covers
 * exactly those fields, in `PerfContextBase` declaration order.
 *
 * Values start at 1024 so upstream can keep appending to its own enum
 * (`rocksdb_total_metric_count` is 86 today) without ever colliding; when a
 * field gains an upstream value, delete its entry here and the binding
 * falls through to `rocksdb_perfcontext_metric` automatically.
 *
 * `rust_rocksdb_perfcontext_metric_ext` reads the CALLING THREAD's perf
 * context (`rocksdb::get_perf_context()`), not a context handle: the layout
 * of `rocksdb_perfcontext_t` is private to `db/c.cc` (same constraint as the
 * SstFileReader iterator above), and upstream's `rocksdb_perfcontext_create`
 * merely wraps the calling thread's `get_perf_context()` pointer — so for
 * any handle used on the thread that created it, the two are the same
 * object. Returns 0 for unknown metric values, mirroring upstream.
 * ------------------------------------------------------------------------- */
enum {
  rust_rocksdb_block_cache_index_hit_count = 1024,
  rust_rocksdb_block_cache_standalone_handle_count,
  rust_rocksdb_block_cache_real_handle_count,
  rust_rocksdb_index_block_read_count,
  rust_rocksdb_block_cache_filter_hit_count,
  rust_rocksdb_filter_block_read_count,
  rust_rocksdb_compression_dict_block_read_count,
  rust_rocksdb_block_cache_index_read_byte,
  rust_rocksdb_block_cache_filter_read_byte,
  rust_rocksdb_block_cache_compression_dict_read_byte,
  rust_rocksdb_block_cache_read_byte,
  rust_rocksdb_secondary_cache_hit_count,
  rust_rocksdb_compressed_sec_cache_insert_real_count,
  rust_rocksdb_compressed_sec_cache_insert_dummy_count,
  rust_rocksdb_compressed_sec_cache_uncompressed_bytes,
  rust_rocksdb_compressed_sec_cache_compressed_bytes,
  rust_rocksdb_block_decompress_count,
  rust_rocksdb_write_scheduling_flushes_compactions_time,
  rust_rocksdb_write_thread_wait_nanos,
  rust_rocksdb_get_cpu_nanos,
  rust_rocksdb_iter_next_cpu_nanos,
  rust_rocksdb_iter_prev_cpu_nanos,
  rust_rocksdb_iter_seek_cpu_nanos,
  rust_rocksdb_iter_next_count,
  rust_rocksdb_iter_prev_count,
  rust_rocksdb_iter_seek_count,
  rust_rocksdb_encrypt_data_nanos,
  rust_rocksdb_decrypt_data_nanos,
  rust_rocksdb_file_ingestion_nanos,
  rust_rocksdb_file_ingestion_blocking_live_writes_nanos,
  rust_rocksdb_multiscan_prepare_count,
  rust_rocksdb_multiscan_blocks_prefetched,
  rust_rocksdb_multiscan_blocks_from_cache,
  rust_rocksdb_multiscan_prefetch_bytes,
  rust_rocksdb_multiscan_io_requests,
  rust_rocksdb_multiscan_io_coalesced_nonadjacent,
};

extern ROCKSDB_LIBRARY_API uint64_t
rust_rocksdb_perfcontext_metric_ext(uint32_t metric);

#ifdef __cplusplus
}
#endif

#endif /* RUST_LIBROCKSDB_SYS_C_API_EXTENSIONS_H_ */

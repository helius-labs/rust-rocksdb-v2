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

#ifdef __cplusplus
}
#endif

#endif /* RUST_LIBROCKSDB_SYS_C_API_EXTENSIONS_H_ */

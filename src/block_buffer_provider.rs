// Copyright 2026 Helius Labs
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! Read-scoped data-block buffer provisioning for iterator scans.
//!
//! Wraps the EXPERIMENTAL C++ `ReadOptions::read_scoped_block_buffer_provider`
//! option: when set, block-based table iterator scans (and MultiScan reads)
//! bypass the data-block cache and place final data-block contents in
//! caller-provided buffers, so applications control where that memory comes
//! from (e.g. a pooled or arena allocator instead of per-read heap
//! allocations). Index and filter blocks keep their normal block-cache
//! behavior, and mmap reads ignore the provider.

use std::any::Any;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::sync::Arc;

use libc::{c_char, c_void, size_t};

use crate::ffi;

/// Provides the backing memory for data blocks loaded by iterator scans.
///
/// Set on read options with
/// [`ReadOptions::set_read_scoped_block_buffer_provider`]
/// (crate::ReadOptions::set_read_scoped_block_buffer_provider). RocksDB calls
/// [`allocate`](Self::allocate) on whichever thread performs the read and
/// drops the returned [`BufferLease`] on whichever thread releases the last
/// reference to the block, so implementations must be `Send + Sync` and
/// tolerate concurrent calls.
pub trait ReadScopedBlockBufferProvider: Send + Sync {
    /// Allocate a writable buffer of at least `size` bytes aligned to
    /// `alignment` bytes.
    ///
    /// `alignment` is a power of two; `1` means no alignment requirement.
    /// When `alignment > 1` (direct-I/O backing) the lease's usable length
    /// must also be a multiple of `alignment`. Returning `None` fails the
    /// read with a memory-limit status. Leases violating the size or
    /// alignment contract are dropped and treated as allocation failure.
    fn allocate(&self, size: usize, alignment: usize) -> Option<BufferLease>;
}

/// One buffer handed to RocksDB by a [`ReadScopedBlockBufferProvider`].
///
/// The pointed-to memory must stay valid and writable until the lease is
/// dropped. RocksDB drops it exactly once per successful allocation, after
/// every pinned key/value slice derived from the block is released —
/// including on later I/O or decompression failure — possibly from another
/// thread.
pub struct BufferLease {
    ptr: *mut u8,
    len: usize,
    /// Reclaims the memory when the lease is dropped.
    _owner: Box<dyn Any + Send>,
}

// The raw pointer aliases memory owned by `_owner`, which is `Send`; RocksDB
// releases leases from arbitrary threads.
unsafe impl Send for BufferLease {}

impl BufferLease {
    /// Build a lease from raw parts.
    ///
    /// # Safety
    ///
    /// `ptr` must point to `len` bytes of writable contiguous memory that
    /// remains valid until `owner` is dropped, and must not be aliased
    /// mutably elsewhere while the lease is live.
    pub unsafe fn from_raw_parts(ptr: *mut u8, len: usize, owner: Box<dyn Any + Send>) -> Self {
        Self {
            ptr,
            len,
            _owner: owner,
        }
    }

    /// Build a lease backed by a plain `Vec<u8>`, using its full length as
    /// the usable size. Only suitable when the requested `alignment` is 1
    /// (buffered, non-direct reads): `Vec<u8>` makes no alignment guarantee.
    pub fn from_vec(mut vec: Vec<u8>) -> Self {
        let ptr = vec.as_mut_ptr();
        let len = vec.len();
        Self {
            ptr,
            len,
            _owner: Box::new(vec),
        }
    }

    /// Usable length of the leased buffer in bytes.
    pub fn len(&self) -> usize {
        self.len
    }

    /// Whether the leased buffer is zero length.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }
}

type DynProvider = Arc<dyn ReadScopedBlockBufferProvider>;

/// Owns the C wrapper for one registered provider. Held by `ReadOptions` so
/// the provider outlives every iterator (and pinned block) created from it:
/// iterators store their `ReadOptions` by value, and the C++ option is a
/// non-owning raw pointer.
pub(crate) struct BlockBufferProviderHandle {
    pub(crate) inner: *mut ffi::rust_rocksdb_read_scoped_block_buffer_provider_t,
}

unsafe impl Send for BlockBufferProviderHandle {}
unsafe impl Sync for BlockBufferProviderHandle {}

impl BlockBufferProviderHandle {
    pub(crate) fn new(provider: DynProvider) -> Self {
        let state = Box::into_raw(Box::new(provider));
        let inner = unsafe {
            ffi::rust_rocksdb_read_scoped_block_buffer_provider_create(
                state.cast::<c_void>(),
                Some(allocate_callback),
                Some(release_callback),
                Some(destroy_callback),
            )
        };
        Self { inner }
    }
}

impl Drop for BlockBufferProviderHandle {
    fn drop(&mut self) {
        unsafe {
            ffi::rust_rocksdb_read_scoped_block_buffer_provider_destroy(self.inner);
        }
    }
}

unsafe extern "C" fn allocate_callback(
    state: *mut c_void,
    size: size_t,
    alignment: size_t,
    data: *mut *mut c_char,
    data_size: *mut size_t,
    lease_state: *mut *mut c_void,
) -> u8 {
    let provider = unsafe { &*state.cast::<DynProvider>() };
    // A panicking provider must not unwind across the FFI boundary; treat it
    // as a failed allocation.
    let lease = catch_unwind(AssertUnwindSafe(|| provider.allocate(size, alignment)));
    let Ok(Some(lease)) = lease else {
        return 0;
    };

    // Enforce the C++ contract here so a misbehaving provider surfaces as a
    // failed read instead of undefined behavior inside RocksDB. Dropping the
    // lease reclaims its memory.
    let misaligned = alignment > 1
        && (!(lease.ptr as usize).is_multiple_of(alignment)
            || !lease.len.is_multiple_of(alignment));
    if lease.len < size || misaligned {
        return 0;
    }

    unsafe {
        *data = lease.ptr.cast::<c_char>();
        *data_size = lease.len;
        *lease_state = Box::into_raw(Box::new(lease)).cast::<c_void>();
    }
    1
}

unsafe extern "C" fn release_callback(_state: *mut c_void, lease_state: *mut c_void) {
    drop(unsafe { Box::from_raw(lease_state.cast::<BufferLease>()) });
}

unsafe extern "C" fn destroy_callback(state: *mut c_void) {
    drop(unsafe { Box::from_raw(state.cast::<DynProvider>()) });
}

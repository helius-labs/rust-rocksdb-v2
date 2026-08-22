// Copyright 2020 Tyler Neely
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

use crate::ffi;
use libc::size_t;
use std::{ptr::NonNull, slice};

/// A caller-owned `PinnableSlice` that survives across point lookups.
///
/// [`DBPinnableSlice`](crate::DBPinnableSlice) allocates one native pinnable
/// slice per `get_pinned` call. This type owns one native slice for its whole
/// lifetime instead: each [`get_pinned_into_cf_opt`] fill resets it and reads
/// into it, so a lookup loop performs no per-call slice allocation, and the
/// internal buffer capacity used for copied (non-pinned) values is retained
/// between fills.
///
/// Filling takes `&mut self` and reading borrows `&self`, so the borrow
/// checker guarantees the previous value is released before the next fill.
///
/// # Lifetime contract
///
/// The slice carries no DB lifetime, so it can live in long-lived state next
/// to an owning handle (e.g. `Arc<DB>`). In exchange the caller must uphold:
/// a filled slice may pin a block-cache handle of the DB it was last filled
/// from, so it must be [`reset`](Self::reset) or dropped before that DB is
/// closed, and holding a filled slice keeps that cache memory pinned until
/// then.
///
/// [`get_pinned_into_cf_opt`]: crate::DB::get_pinned_into_cf_opt
pub struct ReusablePinnableSlice {
    ptr: NonNull<ffi::rocksdb_pinnableslice_t>,
}

// SAFETY: the native slice is only mutated through `&mut self` (fills and
// resets); `&self` access only reads the stable data pointer and length.
unsafe impl Send for ReusablePinnableSlice {}
unsafe impl Sync for ReusablePinnableSlice {}

impl ReusablePinnableSlice {
    /// Creates an empty reusable slice.
    pub fn new() -> Self {
        // SAFETY: `rocksdb_pinnableslice_create` allocates a fresh, empty
        // pinnable slice; it only returns null on allocation failure, which
        // aborts under the default allocator.
        let ptr = unsafe { ffi::rocksdb_pinnableslice_create() };
        Self {
            ptr: NonNull::new(ptr).expect("RocksDB returned a null pinnable slice"),
        }
    }

    /// Releases the current value, unpinning any block-cache handle it held.
    /// Internal buffer capacity from copied values is retained for reuse.
    pub fn reset(&mut self) {
        unsafe { ffi::rocksdb_pinnableslice_reset(self.ptr.as_ptr()) }
    }

    /// The current value; empty when unset (never filled, reset, or last
    /// lookup missed).
    pub fn value(&self) -> &[u8] {
        let mut len: size_t = 0;
        // SAFETY: `ptr` is a live pinnable slice owned by `self`.
        let data = unsafe { ffi::rocksdb_pinnableslice_value(self.ptr.as_ptr(), &raw mut len) };
        if len == 0 {
            // An empty-but-present value can carry a null data pointer, and
            // `slice::from_raw_parts(null, 0)` is undefined behaviour.
            return &[];
        }
        // SAFETY: `data`/`len` describe memory kept alive by the pin until
        // the next `&mut self` fill/reset or drop, which the returned
        // borrow's lifetime excludes.
        unsafe { slice::from_raw_parts(data.cast::<u8>(), len) }
    }

    pub(crate) fn as_mut_ptr(&mut self) -> *mut ffi::rocksdb_pinnableslice_t {
        self.ptr.as_ptr()
    }
}

impl Default for ReusablePinnableSlice {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for ReusablePinnableSlice {
    fn drop(&mut self) {
        unsafe {
            ffi::rocksdb_pinnableslice_destroy(self.ptr.as_ptr());
        }
    }
}

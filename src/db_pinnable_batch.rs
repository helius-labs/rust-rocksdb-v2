use crate::{Error, ffi};
use std::{
    marker::PhantomData,
    ptr::{self, NonNull},
    slice,
};

/// Owns all values returned by one native pinned MultiGet operation.
///
/// Values are borrowed directly from RocksDB and remain valid until this batch
/// is dropped. Vendored builds store successful values in one native owner
/// instead of allocating one wrapper per key. System builds use upstream C API
/// handles internally to avoid depending on RocksDB's private C++ ABI.
pub struct DBPinnableBatch<'db> {
    inner: NonNull<ffi::rust_rocksdb_pinnable_batch_t>,
    len: usize,
    db: PhantomData<&'db ()>,
}

/// Iterator over a [`DBPinnableBatch`].
pub struct DBPinnableBatchIter<'batch, 'db> {
    batch: &'batch DBPinnableBatch<'db>,
    index: usize,
}

unsafe impl Send for DBPinnableBatch<'_> {}
unsafe impl Sync for DBPinnableBatch<'_> {}

impl<'db> DBPinnableBatch<'db> {
    /// Returns the number of results in the batch.
    #[inline]
    pub fn len(&self) -> usize {
        self.len
    }

    /// Returns whether the batch contains no results.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Returns one result by input index.
    pub fn get(&self, index: usize) -> Option<Result<Option<&[u8]>, Error>> {
        if index >= self.len() {
            return None;
        }
        // SAFETY: `index` is bounds-checked above, and the returned borrows
        // are tied to `&self`, which owns the batch and never refills it.
        Some(unsafe { get_entry(self.inner.as_ptr(), index) })
    }

    /// Iterates over results in input order.
    pub fn iter(&self) -> DBPinnableBatchIter<'_, 'db> {
        DBPinnableBatchIter {
            batch: self,
            index: 0,
        }
    }

    pub(crate) unsafe fn from_c(inner: *mut ffi::rust_rocksdb_pinnable_batch_t) -> Self {
        let inner = NonNull::new(inner).expect("RocksDB returned a null pinned batch");
        Self {
            len: unsafe { ffi::rust_rocksdb_pinnable_batch_len(inner.as_ptr()) },
            inner,
            db: PhantomData,
        }
    }
}

impl Drop for DBPinnableBatch<'_> {
    fn drop(&mut self) {
        unsafe {
            ffi::rust_rocksdb_pinnable_batch_destroy(self.inner.as_ptr());
        }
    }
}

impl<'batch> IntoIterator for &'batch DBPinnableBatch<'_> {
    type Item = Result<Option<&'batch [u8]>, Error>;
    type IntoIter = DBPinnableBatchIter<'batch, 'batch>;

    fn into_iter(self) -> Self::IntoIter {
        self.iter()
    }
}

impl<'batch> Iterator for DBPinnableBatchIter<'batch, '_> {
    type Item = Result<Option<&'batch [u8]>, Error>;

    fn next(&mut self) -> Option<Self::Item> {
        let result = self.batch.get(self.index)?;
        self.index += 1;
        Some(result)
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        let remaining = self.batch.len() - self.index;
        (remaining, Some(remaining))
    }
}

impl ExactSizeIterator for DBPinnableBatchIter<'_, '_> {}
impl std::iter::FusedIterator for DBPinnableBatchIter<'_, '_> {}

/// Decodes one `rust_rocksdb_pinnable_batch_get` result.
///
/// # Safety
///
/// `inner` must be a live batch, `index` must be within its current length,
/// and the returned borrows must not outlive the batch's next refill, reset,
/// or destruction — the caller ties `'b` to whatever guards that.
unsafe fn get_entry<'b>(
    inner: *const ffi::rust_rocksdb_pinnable_batch_t,
    index: usize,
) -> Result<Option<&'b [u8]>, Error> {
    let mut value = ptr::null();
    let mut value_len = 0;
    let mut error = ptr::null();
    let mut error_len = 0;
    let state = unsafe {
        ffi::rust_rocksdb_pinnable_batch_get(
            inner,
            index,
            &raw mut value,
            &raw mut value_len,
            &raw mut error,
            &raw mut error_len,
        )
    };

    match state {
        state if state == ffi::rust_rocksdb_pinnable_batch_not_found as u8 => Ok(None),
        state if state == ffi::rust_rocksdb_pinnable_batch_found as u8 => {
            let value = if value_len == 0 {
                &[]
            } else {
                // SAFETY: RocksDB owns `value` for as long as the caller's
                // `'b` guarantees the batch is neither refilled nor dropped.
                unsafe { slice::from_raw_parts(value.cast::<u8>(), value_len) }
            };
            Ok(Some(value))
        }
        state if state == ffi::rust_rocksdb_pinnable_batch_error as u8 => {
            let message = if error_len == 0 {
                String::new()
            } else {
                // SAFETY: The batch owns the error bytes; they are copied out
                // before this call returns.
                let bytes = unsafe { slice::from_raw_parts(error.cast::<u8>(), error_len) };
                String::from_utf8_lossy(bytes).into_owned()
            };
            Err(Error::new(message))
        }
        // Callers bounds-check `index`, so neither the out-of-range state nor
        // an unknown one is reachable against a matching `librocksdb-sys`.
        // Report them rather than panicking: a System backend built against a
        // skewed extension should not be able to abort the process from a
        // safe method.
        unexpected => Err(Error::new(format!(
            "unexpected pinned batch result state {unexpected} at index {index}"
        ))),
    }
}

/// A caller-owned pinned MultiGet batch that survives across calls.
///
/// [`DBPinnableBatch`] is allocated fresh by every batched lookup. This type
/// owns one native batch for its whole lifetime instead: each
/// [`batched_multi_get_pinned_into_cf_opt`] fill resets and refills it in
/// place, reusing the native value/status buffers (including the internal
/// buffer capacity copied, non-pinned values allocated) and the Rust-side
/// key-slice scratch — so a steady-state lookup loop performs no per-call
/// allocation on either side of the FFI boundary.
///
/// Filling takes `&mut self` and reading borrows `&self`, so the borrow
/// checker guarantees the previous fill's values are released before the
/// next fill.
///
/// # Lifetime contract
///
/// The batch carries no DB lifetime, so it can live in long-lived state next
/// to an owning handle (e.g. `Arc<DB>`). In exchange the caller must uphold:
/// a filled batch may pin block-cache handles of the DB it was last filled
/// from, so it must be [`reset`](Self::reset) or dropped before that DB is
/// closed, and holding a filled batch keeps that cache memory pinned until
/// then.
///
/// [`batched_multi_get_pinned_into_cf_opt`]: crate::DB::batched_multi_get_pinned_into_cf_opt
pub struct ReusablePinnableBatch {
    pub(crate) inner: NonNull<ffi::rust_rocksdb_pinnable_batch_t>,
    /// Key-slice scratch reused by each fill, kept here so the fill call
    /// allocates nothing once its capacity has grown to the batch size.
    pub(crate) key_slices: Vec<ffi::rocksdb_slice_t>,
    pub(crate) len: usize,
}

// SAFETY: the native batch is only mutated through `&mut self` (fills and
// resets); `&self` access only reads stable pointers and lengths.
unsafe impl Send for ReusablePinnableBatch {}
unsafe impl Sync for ReusablePinnableBatch {}

impl ReusablePinnableBatch {
    /// Creates an empty reusable batch.
    pub fn new() -> Self {
        // SAFETY: `rust_rocksdb_pinnable_batch_create` allocates a fresh,
        // empty batch.
        let inner = unsafe { ffi::rust_rocksdb_pinnable_batch_create() };
        Self {
            inner: NonNull::new(inner).expect("RocksDB returned a null pinned batch"),
            key_slices: Vec::new(),
            len: 0,
        }
    }

    /// Releases the current values, unpinning any block-cache handles they
    /// held. Internal buffers are retained for the next fill.
    pub fn reset(&mut self) {
        self.len = 0;
        unsafe { ffi::rust_rocksdb_pinnable_batch_reset(self.inner.as_ptr()) }
    }

    /// Returns the number of results from the most recent fill.
    #[inline]
    pub fn len(&self) -> usize {
        self.len
    }

    /// Returns whether the batch holds no results.
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Returns one result by input index of the most recent fill.
    pub fn get(&self, index: usize) -> Option<Result<Option<&[u8]>, Error>> {
        if index >= self.len {
            return None;
        }
        // SAFETY: `index` is bounds-checked against the most recent fill, and
        // the returned borrows are tied to `&self` — the next fill or reset
        // requires `&mut self`, ending them first.
        Some(unsafe { get_entry(self.inner.as_ptr(), index) })
    }

    /// Iterates over the most recent fill's results in input order.
    pub fn iter(&self) -> impl ExactSizeIterator<Item = Result<Option<&[u8]>, Error>> + '_ {
        // SAFETY: as in `get`; every index below `self.len` is in bounds.
        (0..self.len).map(move |index| unsafe { get_entry(self.inner.as_ptr(), index) })
    }
}

impl Default for ReusablePinnableBatch {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for ReusablePinnableBatch {
    fn drop(&mut self) {
        unsafe {
            ffi::rust_rocksdb_pinnable_batch_destroy(self.inner.as_ptr());
        }
    }
}

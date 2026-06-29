// Copyright 2021 Lucjan Suski
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
//

use crate::{Error, Options, ReadOptions, ffi, ffi_util::to_cpath};

use libc::{c_char, size_t};
use std::ptr::NonNull;
use std::{ffi::CString, marker::PhantomData, path::Path};

/// SstFileReader is used to read sst files that are created by SstFileWriter
/// (or by a database flush/compaction) directly, without opening a DB.
pub struct SstFileReader {
    pub(crate) inner: *mut ffi::rust_rocksdb_sst_file_reader_t,
    // Options must outlive the reader: keep them alive for its lifetime.
    _options: Options,
}

unsafe impl Send for SstFileReader {}
unsafe impl Sync for SstFileReader {}

impl SstFileReader {
    /// Initializes SstFileReader with given DB options.
    pub fn create(opts: Options) -> Self {
        let inner = unsafe { ffi::rust_rocksdb_sst_file_reader_create(opts.inner) };
        Self {
            inner,
            _options: opts,
        }
    }

    /// Prepare SstFileReader to read from the file located at `path`.
    pub fn open<P: AsRef<Path>>(&self, path: P) -> Result<(), Error> {
        let cpath = to_cpath(&path)?;
        self.open_raw(&cpath)
    }

    fn open_raw(&self, cpath: &CString) -> Result<(), Error> {
        unsafe {
            ffi_try!(ffi::rust_rocksdb_sst_file_reader_open(
                self.inner,
                cpath.as_ptr() as *const _
            ));
        }
        Ok(())
    }

    /// Creates a new iterator over the SST file.
    pub fn iterator(&self, options: ReadOptions) -> SstFileReaderIterator<'_> {
        unsafe {
            let inner = ffi::rust_rocksdb_sst_file_reader_new_iterator(self.inner, options.inner);
            SstFileReaderIterator {
                inner: NonNull::new(inner)
                    .expect("rust_rocksdb_sst_file_reader_new_iterator returned null"),
                _options: options,
                phantom: PhantomData,
            }
        }
    }
}

impl Drop for SstFileReader {
    fn drop(&mut self) {
        unsafe {
            ffi::rust_rocksdb_sst_file_reader_destroy(self.inner);
        }
    }
}

pub struct SstFileReaderIterator<'a> {
    pub(crate) inner: NonNull<ffi::rust_rocksdb_sst_file_reader_iterator_t>,
    // ReadOptions must outlive the iterator (it may reference iterate bounds).
    _options: ReadOptions,
    phantom: PhantomData<&'a SstFileReader>,
}

impl SstFileReaderIterator<'_> {
    /// Returns `true` if the iterator is positioned at a valid key/value pair.
    pub fn valid(&self) -> bool {
        unsafe { ffi::rust_rocksdb_sst_file_reader_iter_valid(self.inner.as_ptr()) != 0 }
    }

    /// Returns an error if the iterator encountered one during iteration.
    pub fn status(&self) -> Result<(), Error> {
        unsafe {
            ffi_try!(ffi::rust_rocksdb_sst_file_reader_iter_get_error(
                self.inner.as_ptr()
            ));
        }
        Ok(())
    }

    /// Seeks to the first key in the SST file.
    pub fn seek_to_first(&mut self) {
        unsafe {
            ffi::rust_rocksdb_sst_file_reader_iter_seek_to_first(self.inner.as_ptr());
        }
    }

    /// Seeks to the last key in the SST file.
    pub fn seek_to_last(&mut self) {
        unsafe {
            ffi::rust_rocksdb_sst_file_reader_iter_seek_to_last(self.inner.as_ptr());
        }
    }

    /// Seeks to the given key, or the first key that lexicographically follows it.
    pub fn seek<K: AsRef<[u8]>>(&mut self, key: K) {
        let key = key.as_ref();
        unsafe {
            ffi::rust_rocksdb_sst_file_reader_iter_seek(
                self.inner.as_ptr(),
                key.as_ptr() as *const c_char,
                key.len() as size_t,
            );
        }
    }

    /// Seeks to the given key, or the first key that lexicographically precedes it.
    pub fn seek_for_prev<K: AsRef<[u8]>>(&mut self, key: K) {
        let key = key.as_ref();
        unsafe {
            ffi::rust_rocksdb_sst_file_reader_iter_seek_for_prev(
                self.inner.as_ptr(),
                key.as_ptr() as *const c_char,
                key.len() as size_t,
            );
        }
    }

    /// Advances to the next key.
    pub fn next(&mut self) {
        if self.valid() {
            unsafe {
                ffi::rust_rocksdb_sst_file_reader_iter_next(self.inner.as_ptr());
            }
        }
    }

    /// Moves back to the previous key.
    pub fn prev(&mut self) {
        if self.valid() {
            unsafe {
                ffi::rust_rocksdb_sst_file_reader_iter_prev(self.inner.as_ptr());
            }
        }
    }

    /// Returns the current key, or `None` if the iterator is not valid.
    pub fn key(&self) -> Option<&[u8]> {
        if self.valid() {
            Some(self.key_impl())
        } else {
            None
        }
    }

    /// Returns the current value, or `None` if the iterator is not valid.
    pub fn value(&self) -> Option<&[u8]> {
        if self.valid() {
            Some(self.value_impl())
        } else {
            None
        }
    }

    /// Returns the current key/value pair, or `None` if the iterator is not valid.
    pub fn item(&self) -> Option<(&[u8], &[u8])> {
        if self.valid() {
            Some((self.key_impl(), self.value_impl()))
        } else {
            None
        }
    }

    fn key_impl(&self) -> &[u8] {
        unsafe {
            let mut key_len: size_t = 0;
            let key_ptr =
                ffi::rust_rocksdb_sst_file_reader_iter_key(self.inner.as_ptr(), &raw mut key_len);
            std::slice::from_raw_parts(key_ptr as *const u8, key_len)
        }
    }

    fn value_impl(&self) -> &[u8] {
        unsafe {
            let mut val_len: size_t = 0;
            let val_ptr =
                ffi::rust_rocksdb_sst_file_reader_iter_value(self.inner.as_ptr(), &raw mut val_len);
            std::slice::from_raw_parts(val_ptr as *const u8, val_len)
        }
    }
}

impl Iterator for SstFileReaderIterator<'_> {
    type Item = Result<(Box<[u8]>, Box<[u8]>), Error>;

    fn next(&mut self) -> Option<Self::Item> {
        if let Some((key, value)) = self.item() {
            let item = (Box::from(key), Box::from(value));
            SstFileReaderIterator::next(self);
            Some(Ok(item))
        } else {
            self.status().err().map(Result::Err)
        }
    }
}

impl Drop for SstFileReaderIterator<'_> {
    fn drop(&mut self) {
        unsafe { ffi::rust_rocksdb_sst_file_reader_iter_destroy(self.inner.as_ptr()) }
    }
}

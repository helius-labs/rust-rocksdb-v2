// Copyright 2026 Helius
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

mod util;

use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering};

use pretty_assertions::assert_eq;

use rust_rocksdb::{
    BlockBasedOptions, BufferLease, DB, Options, ReadOptions, ReadScopedBlockBufferProvider,
};
use util::DBPath;

/// Hands out `Vec`-backed leases and counts allocations and live leases.
struct CountingProvider {
    allocations: AtomicUsize,
    live_leases: Arc<AtomicUsize>,
}

/// Decrements the live-lease counter when RocksDB releases the lease.
struct LeaseGuard {
    buffer: Vec<u8>,
    live_leases: Arc<AtomicUsize>,
}

impl Drop for LeaseGuard {
    fn drop(&mut self) {
        self.live_leases.fetch_sub(1, Ordering::SeqCst);
    }
}

impl ReadScopedBlockBufferProvider for CountingProvider {
    fn allocate(&self, size: usize, alignment: usize) -> Option<BufferLease> {
        // Buffered (non-direct) reads request no special alignment.
        assert_eq!(alignment, 1, "unexpected alignment request");
        self.allocations.fetch_add(1, Ordering::SeqCst);
        self.live_leases.fetch_add(1, Ordering::SeqCst);

        let mut guard = LeaseGuard {
            buffer: vec![0u8; size],
            live_leases: Arc::clone(&self.live_leases),
        };
        let ptr = guard.buffer.as_mut_ptr();
        let len = guard.buffer.len();
        Some(unsafe { BufferLease::from_raw_parts(ptr, len, Box::new(guard)) })
    }
}

/// Fails every allocation, so provider-backed reads must error out.
struct FailingProvider;

impl ReadScopedBlockBufferProvider for FailingProvider {
    fn allocate(&self, _size: usize, _alignment: usize) -> Option<BufferLease> {
        None
    }
}

/// Open a DB with small blocks and write enough flushed data that a full
/// scan crosses several data blocks.
fn build_db(path: &DBPath) -> (DB, Vec<(String, String)>) {
    let mut block_opts = BlockBasedOptions::default();
    block_opts.set_block_size(1024);

    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.set_block_based_table_factory(&block_opts);

    let db = DB::open(&opts, path).unwrap();
    let pairs: Vec<(String, String)> = (0..500)
        .map(|i| (format!("key{i:05}"), format!("value{i:05}").repeat(8)))
        .collect();
    for (key, value) in &pairs {
        db.put(key, value).unwrap();
    }
    // The provider only backs data blocks read from SST files; memtable
    // reads never call it.
    db.flush().unwrap();
    (db, pairs)
}

#[test]
fn scan_uses_provider_buffers_and_releases_them() {
    let path = DBPath::new("_rust_rocksdb_block_buffer_provider_scan");
    let (db, mut pairs) = build_db(&path);

    let provider = Arc::new(CountingProvider {
        allocations: AtomicUsize::new(0),
        live_leases: Arc::new(AtomicUsize::new(0)),
    });

    let mut read_opts = ReadOptions::default();
    read_opts.set_read_scoped_block_buffer_provider(
        Arc::clone(&provider) as Arc<dyn ReadScopedBlockBufferProvider>
    );

    let mut scanned = Vec::new();
    {
        let mut iter = db.raw_iterator_opt(read_opts);
        iter.seek_to_first();
        while iter.valid() {
            scanned.push((
                String::from_utf8(iter.key().unwrap().to_vec()).unwrap(),
                String::from_utf8(iter.value().unwrap().to_vec()).unwrap(),
            ));
            iter.next();
        }
        iter.status().unwrap();

        assert!(
            provider.allocations.load(Ordering::SeqCst) > 1,
            "a multi-block scan should draw several buffers from the provider"
        );
    }

    pairs.sort();
    assert_eq!(scanned, pairs);
    assert_eq!(
        provider.live_leases.load(Ordering::SeqCst),
        0,
        "every lease must be released once the iterator is dropped"
    );
}

#[test]
fn failed_allocation_fails_the_read() {
    let path = DBPath::new("_rust_rocksdb_block_buffer_provider_alloc_failure");
    let (db, _pairs) = build_db(&path);

    let mut read_opts = ReadOptions::default();
    read_opts.set_read_scoped_block_buffer_provider(Arc::new(FailingProvider));

    let mut iter = db.raw_iterator_opt(read_opts);
    iter.seek_to_first();
    assert!(!iter.valid(), "reads cannot proceed without buffers");
    let err = iter.status().unwrap_err();
    assert!(
        err.to_string().contains("failed to allocate"),
        "unexpected error: {err}"
    );
}

#[test]
fn scans_without_provider_are_unaffected() {
    let path = DBPath::new("_rust_rocksdb_block_buffer_provider_unset");
    let (db, mut pairs) = build_db(&path);

    let mut scanned = Vec::new();
    let mut iter = db.raw_iterator_opt(ReadOptions::default());
    iter.seek_to_first();
    while iter.valid() {
        scanned.push((
            String::from_utf8(iter.key().unwrap().to_vec()).unwrap(),
            String::from_utf8(iter.value().unwrap().to_vec()).unwrap(),
        ));
        iter.next();
    }
    iter.status().unwrap();

    pairs.sort();
    assert_eq!(scanned, pairs);
}

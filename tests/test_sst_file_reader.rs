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

use rust_rocksdb::{IngestExternalFileOptions, Options, ReadOptions, SstFileReader, SstFileWriter};

fn write_sst(path: &std::path::Path, pairs: &[(&[u8], &[u8])]) {
    let opts = Options::default();
    let mut writer = SstFileWriter::create(&opts);
    writer.open(path).unwrap();
    for (k, v) in pairs {
        writer.put(k, v).unwrap();
    }
    writer.finish().unwrap();
}

#[test]
fn sst_file_reader_iterates_in_order() {
    let dir = tempfile::Builder::new()
        .prefix("_rust_rocksdb_sst_reader")
        .tempdir()
        .unwrap();
    let path = dir.path().join("data.sst");
    write_sst(
        &path,
        &[(b"k1", b"v1"), (b"k2", b"v2"), (b"k3", b"v3")],
    );

    let reader = SstFileReader::create(Options::default());
    reader.open(&path).unwrap();

    let mut iter = reader.iterator(ReadOptions::default());
    iter.seek_to_first();
    assert!(iter.valid());
    assert_eq!(iter.key(), Some(b"k1".as_ref()));
    assert_eq!(iter.value(), Some(b"v1".as_ref()));

    iter.next();
    assert_eq!(iter.item(), Some((b"k2".as_ref(), b"v2".as_ref())));

    iter.next();
    assert_eq!(iter.key(), Some(b"k3".as_ref()));

    iter.next();
    assert!(!iter.valid());
    iter.status().unwrap();
}

#[test]
fn sst_file_reader_seek_and_prev() {
    let dir = tempfile::Builder::new()
        .prefix("_rust_rocksdb_sst_reader_seek")
        .tempdir()
        .unwrap();
    let path = dir.path().join("data.sst");
    write_sst(
        &path,
        &[(b"k1", b"v1"), (b"k3", b"v3"), (b"k5", b"v5")],
    );

    let reader = SstFileReader::create(Options::default());
    reader.open(&path).unwrap();
    let mut iter = reader.iterator(ReadOptions::default());

    // seek lands on the first key >= target
    iter.seek(b"k2");
    assert_eq!(iter.key(), Some(b"k3".as_ref()));

    // seek_for_prev lands on the last key <= target
    iter.seek_for_prev(b"k4");
    assert_eq!(iter.key(), Some(b"k3".as_ref()));

    iter.prev();
    assert_eq!(iter.key(), Some(b"k1".as_ref()));

    iter.seek_to_last();
    assert_eq!(iter.key(), Some(b"k5".as_ref()));
}

#[test]
fn sst_file_reader_iterator_trait_collects_all() {
    let dir = tempfile::Builder::new()
        .prefix("_rust_rocksdb_sst_reader_trait")
        .tempdir()
        .unwrap();
    let path = dir.path().join("data.sst");
    write_sst(&path, &[(b"a", b"1"), (b"b", b"2")]);

    let reader = SstFileReader::create(Options::default());
    reader.open(&path).unwrap();
    let mut iter = reader.iterator(ReadOptions::default());
    iter.seek_to_first();

    let collected: Vec<_> = iter.map(|r| r.unwrap()).collect();
    assert_eq!(collected.len(), 2);
    assert_eq!(&*collected[0].0, b"a");
    assert_eq!(&*collected[0].1, b"1");
    assert_eq!(&*collected[1].0, b"b");
}

#[test]
fn ingest_external_file_options_fail_if_not_bottommost_level_is_settable() {
    // Compile + runtime smoke test for the added setter.
    let mut opts = IngestExternalFileOptions::default();
    opts.fail_if_not_bottommost_level(true);
    opts.fail_if_not_bottommost_level(false);
}

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

mod util;

use pretty_assertions::assert_eq;

use rust_rocksdb::{DB, Options, ReadOptions, ReusablePinnableSlice};
use util::DBPath;

#[test]
fn reuse_across_lookups() {
    let path = DBPath::new("_rust_rocksdb_reusable_pinnable_slice_reuse");

    let mut opts = Options::default();
    opts.create_if_missing(true);
    let db = DB::open_cf(&opts, &path, ["default"]).unwrap();
    let cf = db.cf_handle("default").unwrap();

    db.put(b"k1", b"value1").unwrap();
    db.put(b"k2", b"a-longer-second-value").unwrap();
    // Cover both sources: k1/k2 from the memtable (copy path), k3 from an SST
    // after flush (pinned path).
    db.flush().unwrap();
    db.put(b"k3", b"value3").unwrap();

    let readopts = ReadOptions::default();
    let mut slice = ReusablePinnableSlice::new();

    let value = db
        .get_pinned_into_cf_opt(&cf, b"k1", &readopts, &mut slice)
        .unwrap();
    assert_eq!(value, Some(b"value1".as_ref()));
    assert_eq!(slice.value(), b"value1");

    // Refill with a longer value, then a shorter one.
    let value = db
        .get_pinned_into_cf_opt(&cf, b"k2", &readopts, &mut slice)
        .unwrap();
    assert_eq!(value, Some(b"a-longer-second-value".as_ref()));

    let value = db
        .get_pinned_into_cf_opt(&cf, b"k3", &readopts, &mut slice)
        .unwrap();
    assert_eq!(value, Some(b"value3".as_ref()));
}

#[test]
fn miss_clears_previous_value() {
    let path = DBPath::new("_rust_rocksdb_reusable_pinnable_slice_miss");

    let mut opts = Options::default();
    opts.create_if_missing(true);
    let db = DB::open_cf(&opts, &path, ["default"]).unwrap();
    let cf = db.cf_handle("default").unwrap();

    db.put(b"present", b"value").unwrap();

    let readopts = ReadOptions::default();
    let mut slice = ReusablePinnableSlice::new();

    let value = db
        .get_pinned_into_cf_opt(&cf, b"present", &readopts, &mut slice)
        .unwrap();
    assert_eq!(value, Some(b"value".as_ref()));

    // A miss returns None and must not leave the previous value readable.
    let value = db
        .get_pinned_into_cf_opt(&cf, b"absent", &readopts, &mut slice)
        .unwrap();
    assert_eq!(value, None);
    assert_eq!(slice.value(), b"");
}

#[test]
fn reset_and_empty_states() {
    let path = DBPath::new("_rust_rocksdb_reusable_pinnable_slice_reset");

    let mut opts = Options::default();
    opts.create_if_missing(true);
    let db = DB::open_cf(&opts, &path, ["default"]).unwrap();
    let cf = db.cf_handle("default").unwrap();

    db.put(b"key", b"value").unwrap();

    // Never-filled slices read as empty.
    let mut slice = ReusablePinnableSlice::new();
    assert_eq!(slice.value(), b"");

    let readopts = ReadOptions::default();
    db.get_pinned_into_cf_opt(&cf, b"key", &readopts, &mut slice)
        .unwrap();
    assert_eq!(slice.value(), b"value");

    // Reset releases the value; the slice stays usable.
    slice.reset();
    assert_eq!(slice.value(), b"");

    let value = db
        .get_pinned_into_cf_opt(&cf, b"key", &readopts, &mut slice)
        .unwrap();
    assert_eq!(value, Some(b"value".as_ref()));
}

#[test]
fn empty_value_roundtrips() {
    let path = DBPath::new("_rust_rocksdb_reusable_pinnable_slice_empty");

    let mut opts = Options::default();
    opts.create_if_missing(true);
    let db = DB::open_cf(&opts, &path, ["default"]).unwrap();
    let cf = db.cf_handle("default").unwrap();

    db.put(b"empty", b"").unwrap();

    let readopts = ReadOptions::default();
    let mut slice = ReusablePinnableSlice::new();

    // An empty stored value is found (Some), distinct from a miss (None).
    let value = db
        .get_pinned_into_cf_opt(&cf, b"empty", &readopts, &mut slice)
        .unwrap();
    assert_eq!(value, Some(b"".as_ref()));
}

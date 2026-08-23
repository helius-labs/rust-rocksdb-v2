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

use rust_rocksdb::{DB, Options, ReadOptions, ReusablePinnableBatch};
use util::DBPath;

fn open_db(path: &DBPath) -> DB {
    let mut opts = Options::default();
    opts.create_if_missing(true);
    DB::open_cf(&opts, path, ["default"]).unwrap()
}

fn fill(db: &DB, keys: &[&[u8]], batch: &mut ReusablePinnableBatch) -> Vec<Option<Vec<u8>>> {
    let cf = db.cf_handle("default").unwrap();
    let readopts = ReadOptions::default();
    // SAFETY: every test owns both `db` and the batch, and drops the batch
    // before the DB.
    unsafe { db.batched_multi_get_pinned_into_cf_opt(&cf, keys, false, &readopts, batch) }.unwrap();
    assert_eq!(batch.len(), keys.len());
    batch
        .iter()
        .map(|entry| entry.unwrap().map(<[u8]>::to_vec))
        .collect()
}

#[test]
fn refill_with_more_then_fewer_keys() {
    let path = DBPath::new("_rust_rocksdb_reusable_batch_refill");
    let db = open_db(&path);

    // Mix memtable (copy path) and SST (pinned path) sources.
    db.put(b"a", b"va").unwrap();
    db.put(b"b", b"vb-longer-value").unwrap();
    db.flush().unwrap();
    db.put(b"c", b"vc").unwrap();

    let mut batch = ReusablePinnableBatch::new();
    assert!(batch.is_empty());

    let values = fill(&db, &[b"a", b"missing", b"c"], &mut batch);
    assert_eq!(
        values,
        vec![Some(b"va".to_vec()), None, Some(b"vc".to_vec())]
    );

    // Grow: more keys than the previous fill.
    let values = fill(&db, &[b"c", b"b", b"a", b"also-missing"], &mut batch);
    assert_eq!(
        values,
        vec![
            Some(b"vc".to_vec()),
            Some(b"vb-longer-value".to_vec()),
            Some(b"va".to_vec()),
            None,
        ]
    );

    // Shrink: fewer keys; indexes past the new length must be out of range
    // even though the internal buffers are still larger.
    let values = fill(&db, &[b"b"], &mut batch);
    assert_eq!(values, vec![Some(b"vb-longer-value".to_vec())]);
    assert!(batch.get(1).is_none());

    // Empty fill.
    let values = fill(&db, &[], &mut batch);
    assert_eq!(values, Vec::<Option<Vec<u8>>>::new());
    assert!(batch.is_empty());

    // The batch stays usable after an empty fill.
    let values = fill(&db, &[b"a"], &mut batch);
    assert_eq!(values, vec![Some(b"va".to_vec())]);
}

#[test]
fn reset_releases_values() {
    let path = DBPath::new("_rust_rocksdb_reusable_batch_reset");
    let db = open_db(&path);

    db.put(b"k", b"v").unwrap();

    let mut batch = ReusablePinnableBatch::new();
    fill(&db, &[b"k"], &mut batch);
    assert_eq!(batch.len(), 1);

    batch.reset();
    assert!(batch.is_empty());
    assert!(batch.get(0).is_none());

    // Still refillable after a reset.
    let values = fill(&db, &[b"k"], &mut batch);
    assert_eq!(values, vec![Some(b"v".to_vec())]);
}

#[test]
fn batches_do_not_alias() {
    let path = DBPath::new("_rust_rocksdb_reusable_batch_alias");
    let db = open_db(&path);

    db.put(b"x", b"value-x").unwrap();
    db.put(b"y", b"value-y").unwrap();

    let mut first = ReusablePinnableBatch::new();
    let mut second = ReusablePinnableBatch::new();
    fill(&db, &[b"x"], &mut first);
    fill(&db, &[b"y"], &mut second);

    // Refilling one batch must not disturb the other's values.
    fill(&db, &[b"y"], &mut first);
    let entry = second.get(0).unwrap().unwrap().unwrap();
    assert_eq!(entry, b"value-y");
}

#[test]
fn duplicate_keys_come_back_per_position() {
    let path = DBPath::new("_rust_rocksdb_reusable_batch_dup");
    let db = open_db(&path);

    db.put(b"dup", b"value").unwrap();

    let mut batch = ReusablePinnableBatch::new();
    let values = fill(&db, &[b"dup", b"dup", b"nope"], &mut batch);
    assert_eq!(
        values,
        vec![Some(b"value".to_vec()), Some(b"value".to_vec()), None]
    );
}

#[test]
fn one_shot_batch_still_works() {
    // The one-shot DBPinnableBatch path is now implemented on top of the
    // refillable batch; pin its behavior.
    let path = DBPath::new("_rust_rocksdb_reusable_batch_one_shot");
    let db = open_db(&path);

    db.put(b"k1", b"v1").unwrap();

    let cf = db.cf_handle("default").unwrap();
    let batch = db
        .batched_multi_get_pinned_batch_cf(&cf, [b"k1".as_ref(), b"k2".as_ref()], false)
        .unwrap();
    assert_eq!(batch.len(), 2);
    assert_eq!(batch.get(0).unwrap().unwrap(), Some(b"v1".as_ref()));
    assert_eq!(batch.get(1).unwrap().unwrap(), None);
}

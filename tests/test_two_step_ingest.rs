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

use pretty_assertions::assert_eq;

use rust_rocksdb::{IngestExternalFileOptions, Options, SstFileWriter, DB};
use std::path::Path;
use util::DBPath;

fn write_sst(dir: &Path, name: &str, pairs: &[(&[u8], &[u8])]) -> std::path::PathBuf {
    let path = dir.join(name);
    let opts = Options::default();
    let mut writer = SstFileWriter::create(&opts);
    writer.open(&path).unwrap();
    for (key, value) in pairs {
        writer.put(key, value).unwrap();
    }
    writer.finish().unwrap();
    path
}

#[test]
fn two_step_ingest_commits_multiple_cfs_atomically() {
    let db_path = DBPath::new("_rust_rocksdb_two_step_ingest_multi_cf");
    let dir = tempfile::Builder::new()
        .prefix("_rust_rocksdb_two_step_ingest_multi_cf")
        .tempdir()
        .unwrap();

    let mut opts = Options::default();
    opts.create_if_missing(true);
    opts.create_missing_column_families(true);
    let db = DB::open_cf(&opts, &db_path, ["a", "b"]).unwrap();
    let cf_a = db.cf_handle("a").unwrap();
    let cf_b = db.cf_handle("b").unwrap();

    let default_sst = write_sst(dir.path(), "default.sst", &[(b"dk", b"dv")]);
    let a_sst = write_sst(dir.path(), "a.sst", &[(b"ak", b"av")]);
    let b_sst = write_sst(dir.path(), "b.sst", &[(b"bk", b"bv")]);

    let ingest_opts = IngestExternalFileOptions::default();
    let handles = vec![
        db.prepare_file_ingestion_cf_opts(
            &db.cf_handle("default").unwrap(),
            &ingest_opts,
            vec![&default_sst],
        )
        .unwrap(),
        db.prepare_file_ingestion_cf_opts(&cf_a, &ingest_opts, vec![&a_sst])
            .unwrap(),
        db.prepare_file_ingestion_cf_opts(&cf_b, &ingest_opts, vec![&b_sst])
            .unwrap(),
    ];

    // Prepared data is not visible until the commit.
    assert!(db.get(b"dk").unwrap().is_none());
    assert!(db.get_cf(&cf_a, b"ak").unwrap().is_none());
    assert!(db.get_cf(&cf_b, b"bk").unwrap().is_none());

    db.commit_file_ingestion_handles(handles).unwrap();

    assert_eq!(db.get(b"dk").unwrap().unwrap(), b"dv");
    assert_eq!(db.get_cf(&cf_a, b"ak").unwrap().unwrap(), b"av");
    assert_eq!(db.get_cf(&cf_b, b"bk").unwrap().unwrap(), b"bv");
}

#[test]
fn dropping_uncommitted_handle_rolls_back() {
    let db_path = DBPath::new("_rust_rocksdb_two_step_ingest_rollback");
    let dir = tempfile::Builder::new()
        .prefix("_rust_rocksdb_two_step_ingest_rollback")
        .tempdir()
        .unwrap();

    let mut opts = Options::default();
    opts.create_if_missing(true);
    let db = DB::open_cf(&opts, &db_path, ["default"]).unwrap();
    let sst = write_sst(dir.path(), "data.sst", &[(b"k1", b"v1")]);

    let ingest_opts = IngestExternalFileOptions::default();
    let handle = db
        .prepare_file_ingestion_cf_opts(&db.cf_handle("default").unwrap(), &ingest_opts, vec![&sst])
        .unwrap();
    drop(handle);

    assert!(db.get(b"k1").unwrap().is_none());

    // The rollback released the staged state, so the same file ingests fine.
    let handle = db
        .prepare_file_ingestion_cf_opts(&db.cf_handle("default").unwrap(), &ingest_opts, vec![&sst])
        .unwrap();
    db.commit_file_ingestion_handles(vec![handle]).unwrap();
    assert_eq!(db.get(b"k1").unwrap().unwrap(), b"v1");
}

#[test]
fn same_cf_handles_commit_in_order_so_later_data_wins() {
    let db_path = DBPath::new("_rust_rocksdb_two_step_ingest_order");
    let dir = tempfile::Builder::new()
        .prefix("_rust_rocksdb_two_step_ingest_order")
        .tempdir()
        .unwrap();

    let mut opts = Options::default();
    opts.create_if_missing(true);
    let db = DB::open_cf(&opts, &db_path, ["default"]).unwrap();
    let old = write_sst(dir.path(), "old.sst", &[(b"k", b"old")]);
    let new = write_sst(dir.path(), "new.sst", &[(b"k", b"new")]);

    let ingest_opts = IngestExternalFileOptions::default();
    let cf = db.cf_handle("default").unwrap();
    let handles = vec![
        db.prepare_file_ingestion_cf_opts(&cf, &ingest_opts, vec![&old])
            .unwrap(),
        db.prepare_file_ingestion_cf_opts(&cf, &ingest_opts, vec![&new])
            .unwrap(),
    ];
    db.commit_file_ingestion_handles(handles).unwrap();

    assert_eq!(db.get(b"k").unwrap().unwrap(), b"new");
}

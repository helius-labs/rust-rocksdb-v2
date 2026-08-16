use rust_rocksdb::{
    DB,
    perf::{
        MemoryUsageBuilder, PerfContext, PerfMetric, PerfStatsLevel, get_memory_usage_stats,
        set_perf_stats,
    },
};

#[test]
fn test_memory_usage_builder() {
    let tempdir = tempfile::tempdir().unwrap();

    let db = DB::open_default(tempdir.path()).unwrap();
    let mut builder = MemoryUsageBuilder::new().unwrap();
    builder.add_db(&db);
    let memory_usage = builder.build().unwrap();
    assert!(memory_usage.approximate_mem_table_total() > 0);

    // alternative non-builder approach
    let memory_usage = get_memory_usage_stats(Some(&[&db]), None).unwrap();
    assert!(memory_usage.mem_table_total > 0);
}

/// Metrics past `rocksdb_total_metric_count` are served by the local C API
/// extension (`rust_rocksdb_perfcontext_metric_ext`) instead of upstream
/// `rocksdb_perfcontext_metric`; this exercises that dispatch end to end.
#[test]
fn test_perf_context_extension_metrics() {
    const EXTENSION_METRICS: &[PerfMetric] = &[
        PerfMetric::BlockCacheIndexHitCount,
        PerfMetric::BlockCacheStandaloneHandleCount,
        PerfMetric::BlockCacheRealHandleCount,
        PerfMetric::IndexBlockReadCount,
        PerfMetric::BlockCacheFilterHitCount,
        PerfMetric::FilterBlockReadCount,
        PerfMetric::CompressionDictBlockReadCount,
        PerfMetric::BlockCacheIndexReadByte,
        PerfMetric::BlockCacheFilterReadByte,
        PerfMetric::BlockCacheCompressionDictReadByte,
        PerfMetric::BlockCacheReadByte,
        PerfMetric::SecondaryCacheHitCount,
        PerfMetric::CompressedSecCacheInsertRealCount,
        PerfMetric::CompressedSecCacheInsertDummyCount,
        PerfMetric::CompressedSecCacheUncompressedBytes,
        PerfMetric::CompressedSecCacheCompressedBytes,
        PerfMetric::BlockDecompressCount,
        PerfMetric::WriteSchedulingFlushesCompactionsTime,
        PerfMetric::WriteThreadWaitNanos,
        PerfMetric::GetCpuNanos,
        PerfMetric::IterNextCpuNanos,
        PerfMetric::IterPrevCpuNanos,
        PerfMetric::IterSeekCpuNanos,
        PerfMetric::IterNextCount,
        PerfMetric::IterPrevCount,
        PerfMetric::IterSeekCount,
        PerfMetric::EncryptDataNanos,
        PerfMetric::DecryptDataNanos,
        PerfMetric::FileIngestionNanos,
        PerfMetric::FileIngestionBlockingLiveWritesNanos,
        PerfMetric::MultiscanPrepareCount,
        PerfMetric::MultiscanBlocksPrefetched,
        PerfMetric::MultiscanBlocksFromCache,
        PerfMetric::MultiscanPrefetchBytes,
        PerfMetric::MultiscanIoRequests,
        PerfMetric::MultiscanIoCoalescedNonadjacent,
    ];

    let tempdir = tempfile::tempdir().unwrap();
    let db = DB::open_default(tempdir.path()).unwrap();
    for i in 0..100u32 {
        db.put(i.to_be_bytes(), [b'v'; 64]).unwrap();
    }
    // Land the keys in an SST so gets go through the block cache.
    db.flush().unwrap();

    set_perf_stats(PerfStatsLevel::EnableTimeAndCPUTimeExceptForMutex);
    let mut ctx = PerfContext::default();
    ctx.reset();

    let mut iter = db.raw_iterator();
    iter.seek_to_first();
    let mut found = 0;
    while iter.valid() {
        found += 1;
        iter.next();
    }
    assert_eq!(found, 100);
    for i in 0..100u32 {
        assert!(db.get(i.to_be_bytes()).unwrap().is_some());
    }

    assert!(ctx.metric(PerfMetric::IterSeekCount) >= 1);
    assert!(ctx.metric(PerfMetric::IterNextCount) >= 100);
    // 100 gets accumulate measurable CPU time at this perf level.
    assert!(ctx.metric(PerfMetric::GetCpuNanos) > 0);
    // Repeated gets over the same handful of data blocks hit the block cache.
    assert!(ctx.metric(PerfMetric::BlockCacheReadByte) > 0);

    // Every extension metric round-trips through the C shim, and name() maps
    // it back to the C++ field.
    for metric in EXTENSION_METRICS {
        let _ = ctx.metric(*metric);
        assert!(!metric.name().is_empty());
    }
    assert_eq!(PerfMetric::GetCpuNanos.name(), "get_cpu_nanos");

    set_perf_stats(PerfStatsLevel::Disable);
}

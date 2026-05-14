//! Bucket planner: derives per-bucket expected rows / bytes / target file
//! counts from the pinned `funbuns.boundaries` table plus locked calibration
//! constants. Pure arithmetic apart from one manifest-aggregate sanity check
//! against `funbuns.primes` (`total-records`).

use std::sync::Arc;

use anyhow::{Context, Result, anyhow, bail};
use arrow_array::{Int32Array, Int64Array, RecordBatch};
use futures::TryStreamExt;
use iceberg::{Catalog, NamespaceIdent, TableIdent};

pub const BPR_PRIMES: f64 = 1.114;
pub const BPR_PARTITIONS: f64 = 4.0;
pub const K_MEAN: f64 = 1.882;
pub const RPB: i64 = 3_855_446_405;
pub const TARGET_FILE_BYTES: i64 = 1 << 30;
pub const EXPECTED_N_PRIMES: i64 = 21_699_850_257;
pub const EXPECTED_P_MIN_0: i64 = 3;

#[derive(Debug, Clone, serde::Serialize)]
pub struct BucketPlanRow {
    pub p_bucket: i32,
    pub p_min: i64,
    pub p_max_excl: Option<i64>,
    pub primes_rows: i64,
    pub partitions_rows: i64,
    pub primes_bytes: i64,
    pub partitions_bytes: i64,
    pub target_files_primes: i32,
    pub target_files_partitions: i32,
}

#[derive(Debug, Clone, serde::Serialize)]
pub struct BucketPlan {
    pub p_bucket_version: i32,
    pub buckets: Vec<BucketPlanRow>,
}

const NAMESPACE: &str = "funbuns";

pub async fn plan_buckets(catalog: &dyn Catalog, version: i32) -> Result<BucketPlan> {
    let namespace = NamespaceIdent::new(NAMESPACE.to_string());

    let boundaries = load_boundaries(catalog, &namespace, version).await?;
    validate_boundaries_shape(&boundaries, version)?;

    let n_primes = load_total_records(catalog, &namespace, "primes").await?;
    if n_primes != EXPECTED_N_PRIMES {
        bail!(
            "funbuns.primes total-records {n_primes} does not match expected {EXPECTED_N_PRIMES}; \
             refusing to plan against a source the calibration constants don't describe"
        );
    }

    let b = boundaries.len();
    let mut buckets = Vec::with_capacity(b);
    let mut primes_rows_sum: i64 = 0;
    for (i, row) in boundaries.iter().enumerate() {
        let p_max_excl = if i + 1 < b {
            Some(boundaries[i + 1].p_min)
        } else {
            None
        };
        let primes_rows = if i + 1 < b {
            RPB
        } else {
            n_primes - (b as i64 - 1) * RPB
        };
        primes_rows_sum += primes_rows;

        let partitions_rows = (K_MEAN * primes_rows as f64).ceil() as i64;
        let primes_bytes = (primes_rows as f64 * BPR_PRIMES).round() as i64;
        let partitions_bytes = (partitions_rows as f64 * BPR_PARTITIONS).round() as i64;
        let target_files_primes =
            ((primes_bytes as f64 / TARGET_FILE_BYTES as f64).round() as i32).max(1);
        let target_files_partitions =
            ((partitions_bytes as f64 / TARGET_FILE_BYTES as f64).round() as i32).max(1);

        buckets.push(BucketPlanRow {
            p_bucket: row.p_bucket,
            p_min: row.p_min,
            p_max_excl,
            primes_rows,
            partitions_rows,
            primes_bytes,
            partitions_bytes,
            target_files_primes,
            target_files_partitions,
        });
    }

    if primes_rows_sum != n_primes {
        bail!(
            "internal: sum(primes_rows)={primes_rows_sum} != total-records {n_primes}; \
             rpb/N constants are inconsistent"
        );
    }

    Ok(BucketPlan {
        p_bucket_version: version,
        buckets,
    })
}

#[derive(Debug, Clone)]
struct BoundaryRow {
    p_bucket: i32,
    p_min: i64,
}

async fn load_boundaries(
    catalog: &dyn Catalog,
    namespace: &NamespaceIdent,
    version: i32,
) -> Result<Vec<BoundaryRow>> {
    let ident = TableIdent::new(namespace.clone(), "boundaries".to_string());
    let table = catalog
        .load_table(&ident)
        .await
        .with_context(|| format!("loading {ident:?}"))?;

    let scan = table
        .scan()
        .select(["p_bucket_version", "p_bucket", "p_min"])
        .build()
        .context("building boundaries scan")?;
    let batches: Vec<RecordBatch> = scan
        .to_arrow()
        .await
        .context("starting boundaries arrow stream")?
        .try_collect()
        .await
        .context("collecting boundaries arrow stream")?;

    let mut rows = Vec::new();
    for batch in &batches {
        let ver = batch
            .column_by_name("p_bucket_version")
            .ok_or_else(|| anyhow!("boundaries batch missing p_bucket_version"))?
            .as_any()
            .downcast_ref::<Int32Array>()
            .ok_or_else(|| anyhow!("p_bucket_version is not int32"))?;
        let pb = batch
            .column_by_name("p_bucket")
            .ok_or_else(|| anyhow!("boundaries batch missing p_bucket"))?
            .as_any()
            .downcast_ref::<Int32Array>()
            .ok_or_else(|| anyhow!("p_bucket is not int32"))?;
        let pm = batch
            .column_by_name("p_min")
            .ok_or_else(|| anyhow!("boundaries batch missing p_min"))?
            .as_any()
            .downcast_ref::<Int64Array>()
            .ok_or_else(|| anyhow!("p_min is not int64"))?;
        for i in 0..batch.num_rows() {
            if ver.value(i) != version {
                continue;
            }
            rows.push(BoundaryRow {
                p_bucket: pb.value(i),
                p_min: pm.value(i),
            });
        }
    }
    rows.sort_by_key(|r| r.p_bucket);
    Ok(rows)
}

fn validate_boundaries_shape(rows: &[BoundaryRow], version: i32) -> Result<()> {
    if rows.is_empty() {
        bail!("no boundary rows for p_bucket_version={version}");
    }
    let expected_b = ((EXPECTED_N_PRIMES + RPB - 1) / RPB) as usize;
    if rows.len() != expected_b {
        bail!(
            "boundaries.len()={} != ceil(N/RPB)={}",
            rows.len(),
            expected_b
        );
    }
    if rows[0].p_min != EXPECTED_P_MIN_0 {
        bail!(
            "boundaries[0].p_min={} != expected {EXPECTED_P_MIN_0}",
            rows[0].p_min
        );
    }
    for (i, row) in rows.iter().enumerate() {
        if row.p_bucket != i as i32 {
            bail!(
                "boundaries[{i}].p_bucket={} (expected contiguous 0..{})",
                row.p_bucket,
                rows.len()
            );
        }
    }
    for w in rows.windows(2) {
        if w[1].p_min <= w[0].p_min {
            bail!(
                "boundaries.p_min not strictly increasing: {} -> {}",
                w[0].p_min,
                w[1].p_min
            );
        }
    }
    Ok(())
}

async fn load_total_records(
    catalog: &dyn Catalog,
    namespace: &NamespaceIdent,
    table_name: &str,
) -> Result<i64> {
    let ident = TableIdent::new(namespace.clone(), table_name.to_string());
    let table = catalog
        .load_table(&ident)
        .await
        .with_context(|| format!("loading {ident:?}"))?;
    let metadata = table.metadata();
    let snapshot = metadata
        .current_snapshot()
        .ok_or_else(|| anyhow!("{table_name} has no current snapshot"))?;
    if let Some(raw) = snapshot.summary().additional_properties.get("total-records") {
        return raw
            .parse::<i64>()
            .with_context(|| format!("parsing {table_name}.total-records={raw}"));
    }
    // Fallback: sum live row counts from the manifest list.
    let file_io = table.file_io();
    let manifest_list = Arc::clone(snapshot)
        .load_manifest_list(file_io, metadata)
        .await
        .with_context(|| format!("loading {table_name} manifest list"))?;
    let mut total: i64 = 0;
    for entry in manifest_list.entries() {
        let added = entry.added_rows_count.unwrap_or(0) as i64;
        let existing = entry.existing_rows_count.unwrap_or(0) as i64;
        let deleted = entry.deleted_rows_count.unwrap_or(0) as i64;
        total += added + existing - deleted;
    }
    Ok(total)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Pure-math core, factored out of plan_buckets so we can exercise it
    /// without a live catalog.
    fn derive(boundaries: &[BoundaryRow], n_primes: i64, version: i32) -> BucketPlan {
        let b = boundaries.len();
        let mut buckets = Vec::with_capacity(b);
        for (i, row) in boundaries.iter().enumerate() {
            let p_max_excl = if i + 1 < b {
                Some(boundaries[i + 1].p_min)
            } else {
                None
            };
            let primes_rows = if i + 1 < b {
                RPB
            } else {
                n_primes - (b as i64 - 1) * RPB
            };
            let partitions_rows = (K_MEAN * primes_rows as f64).ceil() as i64;
            let primes_bytes = (primes_rows as f64 * BPR_PRIMES).round() as i64;
            let partitions_bytes = (partitions_rows as f64 * BPR_PARTITIONS).round() as i64;
            let target_files_primes =
                ((primes_bytes as f64 / TARGET_FILE_BYTES as f64).round() as i32).max(1);
            let target_files_partitions =
                ((partitions_bytes as f64 / TARGET_FILE_BYTES as f64).round() as i32).max(1);
            buckets.push(BucketPlanRow {
                p_bucket: row.p_bucket,
                p_min: row.p_min,
                p_max_excl,
                primes_rows,
                partitions_rows,
                primes_bytes,
                partitions_bytes,
                target_files_primes,
                target_files_partitions,
            });
        }
        BucketPlan {
            p_bucket_version: version,
            buckets,
        }
    }

    fn v1_boundaries() -> Vec<BoundaryRow> {
        [
            (0, 3i64),
            (1, 93_357_498_629),
            (2, 192_298_134_389),
            (3, 293_340_633_197),
            (4, 395_747_140_337),
            (5, 499_167_448_849),
        ]
        .into_iter()
        .map(|(b, p)| BoundaryRow {
            p_bucket: b,
            p_min: p,
        })
        .collect()
    }

    #[test]
    fn validate_v1_boundaries_passes() {
        validate_boundaries_shape(&v1_boundaries(), 1).unwrap();
    }

    #[test]
    fn validate_rejects_non_increasing() {
        let mut rows = v1_boundaries();
        rows[3].p_min = rows[2].p_min; // duplicate
        let err = validate_boundaries_shape(&rows, 1).unwrap_err();
        assert!(format!("{err}").contains("strictly increasing"));
    }

    #[test]
    fn validate_rejects_wrong_first_pmin() {
        let mut rows = v1_boundaries();
        rows[0].p_min = 5;
        let err = validate_boundaries_shape(&rows, 1).unwrap_err();
        assert!(format!("{err}").contains("p_min"));
    }

    #[test]
    fn v1_full_buckets_target_four_primes_files() {
        let plan = derive(&v1_boundaries(), EXPECTED_N_PRIMES, 1);
        for row in &plan.buckets[..5] {
            assert_eq!(row.primes_rows, RPB);
            assert_eq!(row.target_files_primes, 4);
        }
    }

    #[test]
    fn v1_frontier_bucket_open_ended() {
        let plan = derive(&v1_boundaries(), EXPECTED_N_PRIMES, 1);
        let frontier = plan.buckets.last().unwrap();
        assert_eq!(frontier.p_bucket, 5);
        assert_eq!(frontier.p_max_excl, None);
        assert_eq!(frontier.primes_rows, EXPECTED_N_PRIMES - 5 * RPB);
        assert_eq!(frontier.primes_rows, 2_422_618_232);
    }

    #[test]
    fn v1_primes_rows_sum_matches_n() {
        let plan = derive(&v1_boundaries(), EXPECTED_N_PRIMES, 1);
        let s: i64 = plan.buckets.iter().map(|b| b.primes_rows).sum();
        assert_eq!(s, EXPECTED_N_PRIMES);
    }

    #[test]
    fn v1_partitions_row_estimate_within_one_percent_of_truth() {
        let plan = derive(&v1_boundaries(), EXPECTED_N_PRIMES, 1);
        let estimated: i64 = plan.buckets.iter().map(|b| b.partitions_rows).sum();
        let truth: i64 = 40_842_554_340;
        let rel = (estimated - truth).abs() as f64 / truth as f64;
        assert!(rel < 0.01, "rel error {rel} too high");
    }

    #[test]
    fn v1_expected_file_counts_table() {
        let plan = derive(&v1_boundaries(), EXPECTED_N_PRIMES, 1);
        let primes_total: i32 = plan.buckets.iter().map(|b| b.target_files_primes).sum();
        let partitions_total: i32 = plan.buckets.iter().map(|b| b.target_files_partitions).sum();
        assert_eq!(primes_total, 23);
        assert_eq!(partitions_total, 152);
    }
}

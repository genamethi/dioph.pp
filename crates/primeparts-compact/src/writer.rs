use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::Arc;

use anyhow::{Context, Result, anyhow};
use arrow_array::{Array, Int64Array, RecordBatch};
use arrow_schema::{Field, Schema};
use parquet::arrow::ArrowWriter;
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;
use parquet::basic::{Compression, ZstdLevel};
use parquet::file::metadata::KeyValue;
use parquet::file::properties::WriterProperties;
use serde::Serialize;
use std::collections::HashMap;
use tracing::{debug, info};

use crate::walker::SourceFile;

// One JSONL row, in the same shape primeparts-commit's manifest.rs deserializes.
// We emit `bytes` as the on-disk size after writer.close() finalizes the footer.
#[derive(Debug, Serialize)]
pub struct ManifestRow {
    pub table: String,
    pub path: PathBuf,
    pub commit_seq: i64,
    pub rows: i64,
    pub p_min: i64,
    pub p_max: i64,
    pub bytes: i64,
}

pub struct CompactConfig {
    pub table: String,
    pub out_root: PathBuf,
    pub partition_w: i64,
    pub target_row_group_bytes: usize,
    pub max_row_groups_per_file: usize,
}

struct OpenFile {
    writer: ArrowWriter<File>,
    path: PathBuf,
    partition: i64,
    rows: i64,
    p_min: i64,
    p_max: i64,
    max_commit_seq: i64,
    row_groups: usize,
    file_seq: usize,
}

pub fn compact(sources: &[SourceFile], cfg: &CompactConfig) -> Result<Vec<ManifestRow>> {
    if sources.is_empty() {
        return Ok(vec![]);
    }
    let arrow_schema = clean_schema_metadata(&read_schema(&sources[0].path)?);
    let p_col_idx = arrow_schema
        .index_of("p")
        .context("source schema missing column `p`")?;
    // The C writer emits the field as `batch_id`; the iceberg schema renames
    // it to `commit_seq`. Field ids differ by table: primes has commit_seq=3,
    // decompositions has commit_seq=5.
    let commit_seq_field_id = match cfg.table.as_str() {
        "primes" => 3,
        "decompositions" => 5,
        other => return Err(anyhow!("unknown table {other:?}")),
    };
    let commit_seq_col_idx = field_index_by_id(&arrow_schema, commit_seq_field_id)
        .or_else(|| arrow_schema.index_of("commit_seq").ok())
        .or_else(|| arrow_schema.index_of("batch_id").ok());

    let mut manifest = Vec::new();
    let mut open: Option<OpenFile> = None;
    let mut next_seq_per_partition: std::collections::HashMap<i64, usize> =
        std::collections::HashMap::new();

    let total_files = sources.len();
    for (file_idx, src) in sources.iter().enumerate() {
        if file_idx % 200 == 0 {
            info!(
                "compact[{}]: reading source {}/{} ({})",
                cfg.table,
                file_idx + 1,
                total_files,
                src.path.display()
            );
        }
        let reader = ParquetRecordBatchReaderBuilder::try_new(File::open(&src.path)?)
            .with_context(|| format!("opening source {}", src.path.display()))?
            .build()
            .context("building parquet record batch reader")?;
        for batch in reader {
            let batch = batch.context("reading record batch")?;
            for run in split_by_partition(&batch, p_col_idx, cfg.partition_w)? {
                ensure_open_for(
                    &mut open,
                    &mut manifest,
                    &mut next_seq_per_partition,
                    cfg,
                    &arrow_schema,
                    run.partition,
                )?;
                let of = open.as_mut().expect("open after ensure_open_for");
                let slice = batch.slice(run.start, run.len);
                of.writer.write(&slice).context("writer.write")?;

                update_open_stats(of, &slice, p_col_idx, commit_seq_col_idx)?;

                if of.writer.in_progress_size() >= cfg.target_row_group_bytes {
                    debug!(
                        "compact[{}]: flush row group at {} bytes (file {})",
                        cfg.table,
                        of.writer.in_progress_size(),
                        of.path.display()
                    );
                    of.writer.flush().context("writer.flush")?;
                    of.row_groups += 1;
                    if of.row_groups >= cfg.max_row_groups_per_file {
                        close_file(open.take().unwrap(), &mut manifest)?;
                    }
                }
            }
        }
    }
    if let Some(of) = open.take() {
        close_file(of, &mut manifest)?;
    }
    info!(
        "compact[{}]: wrote {} files from {} sources",
        cfg.table,
        manifest.len(),
        sources.len()
    );
    Ok(manifest)
}

struct Run {
    start: usize,
    len: usize,
    partition: i64,
}

fn split_by_partition(batch: &RecordBatch, p_col_idx: usize, w: i64) -> Result<Vec<Run>> {
    let n = batch.num_rows();
    if n == 0 {
        return Ok(vec![]);
    }
    let p = batch
        .column(p_col_idx)
        .as_any()
        .downcast_ref::<Int64Array>()
        .ok_or_else(|| anyhow!("column `p` is not Int64"))?;

    let mut runs = Vec::new();
    let mut start = 0usize;
    let mut current = trunc(p.value(0), w);
    for i in 1..n {
        let part = trunc(p.value(i), w);
        if part != current {
            runs.push(Run {
                start,
                len: i - start,
                partition: current,
            });
            start = i;
            current = part;
        }
    }
    runs.push(Run {
        start,
        len: n - start,
        partition: current,
    });
    Ok(runs)
}

#[inline]
fn trunc(v: i64, w: i64) -> i64 {
    // Iceberg's TruncateTransform on integers is `v - (v %% w)` where %% is
    // Python-style mod (always non-negative). For our data p > 0, plain
    // remainder is identical, but we use rem_euclid to stay correct if the
    // schema ever permits negatives.
    v - v.rem_euclid(w)
}

fn ensure_open_for(
    open: &mut Option<OpenFile>,
    manifest: &mut Vec<ManifestRow>,
    next_seq_per_partition: &mut std::collections::HashMap<i64, usize>,
    cfg: &CompactConfig,
    arrow_schema: &Arc<Schema>,
    partition: i64,
) -> Result<()> {
    if let Some(of) = open {
        if of.partition == partition {
            return Ok(());
        }
        let prev = open.take().unwrap();
        close_file(prev, manifest)?;
    }
    let seq = next_seq_per_partition.entry(partition).or_insert(0);
    let part_dir = cfg.out_root.join(format!("p_trunc={partition}"));
    std::fs::create_dir_all(&part_dir).with_context(|| format!("mkdir {}", part_dir.display()))?;
    let file_seq = *seq;
    *seq += 1;
    let path = part_dir.join(format!("{}_compact_{:06}.parquet", cfg.table, file_seq));
    let file =
        File::create(&path).with_context(|| format!("creating output {}", path.display()))?;
    let props = WriterProperties::builder()
        .set_compression(Compression::ZSTD(
            ZstdLevel::try_new(3).context("invalid zstd compression level")?,
        ))
        .set_max_row_group_row_count(Some(usize::MAX)) // we drive row-group flush manually via in_progress_size
        .build();
    let writer = ArrowWriter::try_new(file, arrow_schema.clone(), Some(props))
        .with_context(|| format!("opening ArrowWriter for {}", path.display()))?;
    *open = Some(OpenFile {
        writer,
        path,
        partition,
        rows: 0,
        p_min: i64::MAX,
        p_max: i64::MIN,
        max_commit_seq: i64::MIN,
        row_groups: 0,
        file_seq,
    });
    Ok(())
}

fn update_open_stats(
    of: &mut OpenFile,
    slice: &RecordBatch,
    p_col_idx: usize,
    commit_seq_col_idx: Option<usize>,
) -> Result<()> {
    let p = slice
        .column(p_col_idx)
        .as_any()
        .downcast_ref::<Int64Array>()
        .ok_or_else(|| anyhow!("p not Int64 in slice"))?;
    of.rows += slice.num_rows() as i64;
    for i in 0..p.len() {
        let v = p.value(i);
        if v < of.p_min {
            of.p_min = v;
        }
        if v > of.p_max {
            of.p_max = v;
        }
    }
    if let Some(cs_idx) = commit_seq_col_idx {
        let arr = slice.column(cs_idx);
        if let Some(a) = arr.as_any().downcast_ref::<arrow_array::Int32Array>() {
            for i in 0..a.len() {
                let v = a.value(i) as i64;
                if v > of.max_commit_seq {
                    of.max_commit_seq = v;
                }
            }
        } else if let Some(a) = arr.as_any().downcast_ref::<Int64Array>() {
            for i in 0..a.len() {
                let v = a.value(i);
                if v > of.max_commit_seq {
                    of.max_commit_seq = v;
                }
            }
        }
    }
    Ok(())
}

fn close_file(mut of: OpenFile, manifest: &mut Vec<ManifestRow>) -> Result<()> {
    // funbuns.* footer KVs (option (a) in the compaction plan):
    //   commit_seq = max(input.commit_seq) over rows actually written here.
    //   p_min/p_max/n_rows recomputed from the rows in this output file.
    let table = infer_table_from_path(&of.path);
    of.writer.append_key_value_metadata(KeyValue::new(
        "funbuns.table".to_string(),
        Some(table.clone()),
    ));
    of.writer.append_key_value_metadata(KeyValue::new(
        "funbuns.commit_seq".to_string(),
        Some(of.max_commit_seq.to_string()),
    ));
    of.writer.append_key_value_metadata(KeyValue::new(
        "funbuns.p_min".to_string(),
        Some(of.p_min.to_string()),
    ));
    of.writer.append_key_value_metadata(KeyValue::new(
        "funbuns.p_max".to_string(),
        Some(of.p_max.to_string()),
    ));
    of.writer.append_key_value_metadata(KeyValue::new(
        "funbuns.n_rows".to_string(),
        Some(of.rows.to_string()),
    ));
    of.writer.close().context("closing ArrowWriter")?;
    let bytes = std::fs::metadata(&of.path)
        .with_context(|| format!("stat {}", of.path.display()))?
        .len() as i64;
    info!(
        "closed {} (partition={} seq={} rows={} p=[{}..{}] commit_seq={} bytes={})",
        of.path.display(),
        of.partition,
        of.file_seq,
        of.rows,
        of.p_min,
        of.p_max,
        of.max_commit_seq,
        bytes,
    );
    manifest.push(ManifestRow {
        table,
        path: of.path,
        commit_seq: of.max_commit_seq,
        rows: of.rows,
        p_min: of.p_min,
        p_max: of.p_max,
        bytes,
    });
    Ok(())
}

fn infer_table_from_path(p: &Path) -> String {
    // Output layout: {out_root}/p_trunc={N}/{table}_compact_*.parquet
    // The table name is the leading token of the filename before "_compact".
    let name = p.file_name().unwrap_or_default().to_string_lossy();
    name.split("_compact_")
        .next()
        .unwrap_or("unknown")
        .to_string()
}

// Strip schema-level funbuns.* metadata copied from the input file. Those KVs
// describe the source batch and would be stale on the compacted output (e.g.
// "funbuns.batch_id: '0'" from the first input). Field-level metadata
// (PARQUET:field_id) is kept untouched so iceberg field-id mapping survives.
// The compacted file's authoritative funbuns.* lives in the parquet footer KV
// stamped at close().
fn clean_schema_metadata(schema: &Arc<Schema>) -> Arc<Schema> {
    let kept: HashMap<String, String> = schema
        .metadata()
        .iter()
        .filter(|(k, _)| !k.starts_with("funbuns."))
        .map(|(k, v)| (k.clone(), v.clone()))
        .collect();
    let fields: Vec<Field> = schema.fields().iter().map(|f| f.as_ref().clone()).collect();
    Arc::new(Schema::new_with_metadata(fields, kept))
}

fn field_index_by_id(schema: &Schema, field_id: i32) -> Option<usize> {
    let needle = field_id.to_string();
    for (i, f) in schema.fields().iter().enumerate() {
        if f.metadata().get("PARQUET:field_id").map(|s| s.as_str()) == Some(needle.as_str()) {
            return Some(i);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::trunc;

    #[test]
    fn trunc_matches_iceberg_positive_ranges() {
        assert_eq!(trunc(3, 10_000_000_000), 0);
        assert_eq!(trunc(9_999_999_967, 10_000_000_000), 0);
        assert_eq!(trunc(10_000_000_019, 10_000_000_000), 10_000_000_000);
    }

    #[test]
    fn trunc_uses_euclidean_remainder() {
        assert_eq!(trunc(-1, 10), -10);
        assert_eq!(trunc(-10, 10), -10);
    }
}

fn read_schema(path: &Path) -> Result<Arc<Schema>> {
    let f = File::open(path).with_context(|| format!("opening {}", path.display()))?;
    let builder = ParquetRecordBatchReaderBuilder::try_new(f)
        .context("ParquetRecordBatchReaderBuilder::try_new")?;
    Ok(builder.schema().clone())
}

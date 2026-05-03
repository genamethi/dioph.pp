use std::collections::HashMap;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use primeparts_core::{extract_funbuns_kv, read_parquet_metadata};
use serde::Deserialize;

#[derive(Debug, Deserialize, Clone)]
pub struct ManifestRow {
    pub table: String,
    pub path: PathBuf,
    pub commit_seq: i64,
    pub rows: i64,
    pub p_min: i64,
    pub p_max: i64,
    pub bytes: i64,
}

pub fn parse_jsonl(path: &Path) -> Result<Vec<ManifestRow>> {
    let content = std::fs::read_to_string(path)
        .with_context(|| format!("reading manifest {}", path.display()))?;
    let mut rows = Vec::new();
    for (i, line) in content.lines().enumerate() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let row: ManifestRow = serde_json::from_str(line)
            .with_context(|| format!("manifest {} line {}", path.display(), i + 1))?;
        rows.push(row);
    }
    Ok(rows)
}

// Mirrors Python's verify_native_files (src/primeparts/native_iceberg.py:135-138).
// We intentionally do not compare manifest `bytes` against std::fs::metadata().len():
// (a) Python doesn't, so adding it makes the Rust path stricter than production —
//     a cutover failure mode that gives no real signal;
// (b) the parquet footer lives at end-of-file, so any truncation that would shift
//     the byte count also breaks read_parquet_metadata, which fails earlier with
//     a clearer parquet-level error.
pub fn verify_against_footer(row: &ManifestRow) -> Result<()> {
    let metadata = read_parquet_metadata(&row.path)?;
    let kv = extract_funbuns_kv(&metadata)?;
    let path = row.path.display();
    if kv.table != row.table {
        bail!(
            "table mismatch in {path}: footer={} manifest={}",
            kv.table,
            row.table
        );
    }
    if kv.commit_seq != row.commit_seq {
        bail!(
            "commit_seq mismatch in {path}: footer={} manifest={}",
            kv.commit_seq,
            row.commit_seq
        );
    }
    if kv.n_rows != row.rows {
        bail!(
            "n_rows mismatch in {path}: footer={} manifest={}",
            kv.n_rows,
            row.rows
        );
    }
    if kv.p_min != row.p_min {
        bail!(
            "p_min mismatch in {path}: footer={} manifest={}",
            kv.p_min,
            row.p_min
        );
    }
    if kv.p_max != row.p_max {
        bail!(
            "p_max mismatch in {path}: footer={} manifest={}",
            kv.p_max,
            row.p_max
        );
    }
    Ok(())
}

pub fn group_by_table(rows: Vec<ManifestRow>) -> HashMap<String, Vec<ManifestRow>> {
    let mut by_table: HashMap<String, Vec<ManifestRow>> = HashMap::new();
    for row in rows {
        by_table.entry(row.table.clone()).or_default().push(row);
    }
    by_table
}

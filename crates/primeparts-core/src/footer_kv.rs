use std::collections::HashMap;
use std::fs::File;
use std::path::Path;
use std::sync::Arc;

use anyhow::{anyhow, Context, Result};
use parquet::file::metadata::{ParquetMetaData, ParquetMetaDataReader};

#[derive(Debug, Clone)]
pub struct FooterKv {
    pub table: String,
    pub commit_seq: i64,
    pub p_min: i64,
    pub p_max: i64,
    pub n_rows: i64,
    pub n_primes: Option<i64>,
    pub raw: HashMap<String, String>,
}

pub fn read_parquet_metadata(path: &Path) -> Result<Arc<ParquetMetaData>> {
    let file = File::open(path)
        .with_context(|| format!("opening parquet {}", path.display()))?;
    let metadata = ParquetMetaDataReader::new()
        .parse_and_finish(&file)
        .with_context(|| format!("reading parquet metadata from {}", path.display()))?;
    Ok(Arc::new(metadata))
}

pub fn extract_funbuns_kv(metadata: &ParquetMetaData) -> Result<FooterKv> {
    let kv = metadata
        .file_metadata()
        .key_value_metadata()
        .context("parquet footer has no key-value metadata")?;

    let mut raw = HashMap::with_capacity(kv.len());
    for entry in kv {
        if let Some(v) = &entry.value {
            raw.insert(entry.key.clone(), v.clone());
        }
    }

    let get_str = |k: &str| -> Result<String> {
        raw.get(k)
            .cloned()
            .ok_or_else(|| anyhow!("missing footer KV: {k}"))
    };
    let get_i64 = |k: &str| -> Result<i64> {
        get_str(k)?
            .parse()
            .with_context(|| format!("parsing footer KV {k} as i64"))
    };

    Ok(FooterKv {
        table: get_str("funbuns.table")?,
        commit_seq: get_i64("funbuns.commit_seq")?,
        p_min: get_i64("funbuns.p_min")?,
        p_max: get_i64("funbuns.p_max")?,
        n_rows: get_i64("funbuns.n_rows")?,
        n_primes: raw.get("funbuns.n_primes").and_then(|v| v.parse().ok()),
        raw,
    })
}

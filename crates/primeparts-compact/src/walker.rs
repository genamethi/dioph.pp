use std::path::{Path, PathBuf};

use anyhow::{Context, Result, anyhow};
use walkdir::WalkDir;

#[derive(Debug, Clone)]
pub struct SourceFile {
    pub path: PathBuf,
    pub batch_id: i64,
    pub file_seq: i64,
}

// Filenames are of the form "{table}_b{NNNNNN}_{NNN}.parquet". The first
// numeric block is the monotonic batch id, and the second is the chunk index
// within that batch. We sort on both, ignoring whether the parent directory
// uses the older `batch_id=N` or newer `commit_seq=N` naming — both schemes
// coexist in the live warehouse.
pub fn walk(table_data_root: &Path) -> Result<Vec<SourceFile>> {
    let mut files = Vec::new();
    for entry in WalkDir::new(table_data_root)
        .min_depth(2)
        .max_depth(2)
        .into_iter()
        .filter_map(|e| e.ok())
    {
        if !entry.file_type().is_file() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_string();
        if !name.ends_with(".parquet") {
            continue;
        }
        let (batch_id, file_seq) =
            parse_file_ids(&name).with_context(|| format!("parsing file ids from {name}"))?;
        files.push(SourceFile {
            path: entry.path().to_path_buf(),
            batch_id,
            file_seq,
        });
    }
    files.sort_by_key(|f| (f.batch_id, f.file_seq));
    Ok(files)
}

fn parse_file_ids(name: &str) -> Result<(i64, i64)> {
    // "primes_b000123_000.parquet" → 123
    let after_b = name
        .split_once("_b")
        .map(|(_, rest)| rest)
        .ok_or_else(|| anyhow!("filename has no _b token"))?;
    let digits: String = after_b.chars().take_while(|c| c.is_ascii_digit()).collect();
    if digits.is_empty() {
        return Err(anyhow!("no digits after _b"));
    }
    let batch_id = digits.parse().context("parsing batch id digits")?;
    let after_batch = after_b
        .strip_prefix(&digits)
        .and_then(|rest| rest.strip_prefix('_'))
        .ok_or_else(|| anyhow!("filename has no chunk suffix"))?;
    let seq_digits: String = after_batch
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect();
    if seq_digits.is_empty() {
        return Err(anyhow!("no digits after chunk separator"));
    }
    let file_seq = seq_digits.parse().context("parsing chunk suffix digits")?;
    Ok((batch_id, file_seq))
}

#[cfg(test)]
mod tests {
    use super::parse_file_ids;

    #[test]
    fn parses_batch_and_chunk_ids() {
        assert_eq!(
            parse_file_ids("primes_b000123_004.parquet").unwrap(),
            (123, 4)
        );
        assert_eq!(
            parse_file_ids("decompositions_b987654_012.parquet").unwrap(),
            (987654, 12)
        );
    }

    #[test]
    fn rejects_names_without_chunk_suffix() {
        assert!(parse_file_ids("primes_b000123.parquet").is_err());
    }
}

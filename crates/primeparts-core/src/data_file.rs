// Per-file parquet → DataFile conversion that stamps the table's
// default_sort_order_id on every builder before .build(). Mirrors the
// pyiceberg `_patch_pyiceberg_sort_order_id` fix on the Rust side: without
// this, files registered through any of iceberg-rust's add_files paths land
// in the manifest with sort_order_id=None, and downstream SQL engines plan
// redundant sort steps on scans.

use std::collections::HashMap;
use std::path::Path;
use std::sync::Arc;

use anyhow::{Context, Result};
use iceberg::spec::{DataFile, TableMetadata};
use iceberg::writer::file_writer::ParquetWriter;
use parquet::file::metadata::ParquetMetaData;

use crate::footer_kv::read_parquet_metadata;

pub fn parquet_file_to_data_file(
    file_path: &Path,
    table_metadata: &TableMetadata,
) -> Result<DataFile> {
    let metadata = read_parquet_metadata(file_path)?;
    let file_size_in_bytes = std::fs::metadata(file_path)
        .with_context(|| format!("stat {}", file_path.display()))?
        .len() as usize;
    parquet_metadata_to_data_file(
        file_path.to_string_lossy().into_owned(),
        metadata,
        file_size_in_bytes,
        table_metadata,
    )
}

pub fn parquet_metadata_to_data_file(
    file_path: String,
    metadata: Arc<ParquetMetaData>,
    file_size_in_bytes: usize,
    table_metadata: &TableMetadata,
) -> Result<DataFile> {
    let mut builder = ParquetWriter::parquet_to_data_file_builder(
        table_metadata.current_schema().clone(),
        metadata,
        file_size_in_bytes,
        file_path.clone(),
        HashMap::new(),
    )
    .with_context(|| format!("building DataFile for {file_path}"))?;

    builder
        .partition_spec_id(table_metadata.default_partition_spec_id())
        .sort_order_id(table_metadata.default_sort_order_id() as i32);

    builder
        .build()
        .with_context(|| format!("finalizing DataFile for {file_path}"))
}

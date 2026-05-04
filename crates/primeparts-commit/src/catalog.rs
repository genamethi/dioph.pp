use std::collections::HashMap;
use std::str::FromStr;
use std::sync::Arc;

use anyhow::{Context, Result};
use iceberg::io::LocalFsStorageFactory;
use iceberg::table::Table;
use iceberg::transaction::{ApplyTransactionAction, Transaction};
use iceberg::{Catalog, CatalogBuilder, NamespaceIdent, TableIdent};
use iceberg_catalog_sql::{SqlBindStyle, SqlCatalog, SqlCatalogBuilder};
use primeparts_core::parquet_file_to_data_file;
use primeparts_core::table_paths::NAMESPACE;
use sqlx::Row;
use sqlx::sqlite::{SqliteConnectOptions, SqlitePoolOptions};

use crate::manifest::ManifestRow;

async fn ensure_catalog_iceberg_type_column(uri: &str) -> Result<()> {
    let opts = SqliteConnectOptions::from_str(uri)
        .map(|opts| opts.create_if_missing(true))
        .with_context(|| format!("parsing sqlite uri: {uri}"))?;
    let pool = SqlitePoolOptions::new()
        .max_connections(1)
        .connect_with(opts)
        .await
        .context("opening sqlite for catalog schema check")?;

    let cols = sqlx::query("PRAGMA table_info('iceberg_tables')")
        .fetch_all(&pool)
        .await
        .context("reading iceberg_tables schema")?;

    if cols.is_empty() {
        // Catalog table does not exist yet; SqlCatalogBuilder::load will create it.
        return Ok(());
    }

    let has_iceberg_type = cols.iter().any(|row| {
        row.try_get::<String, _>("name")
            .map(|name| name == "iceberg_type")
            .unwrap_or(false)
    });
    if has_iceberg_type {
        return Ok(());
    }

    sqlx::query("ALTER TABLE iceberg_tables ADD COLUMN iceberg_type VARCHAR(5)")
        .execute(&pool)
        .await
        .context("adding iceberg_tables.iceberg_type")?;
    sqlx::query("UPDATE iceberg_tables SET iceberg_type = 'TABLE' WHERE iceberg_type IS NULL")
        .execute(&pool)
        .await
        .context("backfilling iceberg_tables.iceberg_type")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use sqlx::SqlitePool;
    use std::time::{SystemTime, UNIX_EPOCH};

    async fn test_db_uri(name: &str) -> Result<(String, std::path::PathBuf)> {
        let ts = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .context("clock before unix epoch")?
            .as_nanos();
        let base = std::env::temp_dir().join(format!("primeparts-commit-{name}-{ts}"));
        std::fs::create_dir_all(&base).context("creating temp test dir")?;
        let db_path = base.join("catalog.db");
        let uri = format!("sqlite://{}", db_path.display());
        Ok((uri, base))
    }

    async fn fetch_columns(uri: &str) -> Result<Vec<String>> {
        let opts = SqliteConnectOptions::from_str(uri)
            .map(|opts| opts.create_if_missing(true))
            .with_context(|| format!("parsing sqlite uri: {uri}"))?;
        let pool = SqlitePoolOptions::new()
            .max_connections(1)
            .connect_with(opts)
            .await
            .context("opening sqlite for test read")?;
        let rows = sqlx::query("PRAGMA table_info('iceberg_tables')")
            .fetch_all(&pool)
            .await
            .context("reading iceberg_tables schema")?;
        let cols = rows
            .iter()
            .filter_map(|row| row.try_get::<String, _>("name").ok())
            .collect();
        Ok(cols)
    }

    async fn insert_table_row_without_iceberg_type(pool: &SqlitePool) -> Result<()> {
        sqlx::query(
            "CREATE TABLE iceberg_tables (
                catalog_name TEXT NOT NULL,
                table_namespace TEXT NOT NULL,
                table_name TEXT NOT NULL,
                metadata_location TEXT NOT NULL,
                previous_metadata_location TEXT,
                PRIMARY KEY (catalog_name, table_namespace, table_name)
            )",
        )
        .execute(pool)
        .await
        .context("creating legacy iceberg_tables schema")?;

        sqlx::query(
            "INSERT INTO iceberg_tables
            (catalog_name, table_namespace, table_name, metadata_location, previous_metadata_location)
            VALUES (?, ?, ?, ?, ?)",
        )
        .bind("funbuns")
        .bind("funbuns")
        .bind("primes")
        .bind("metadata/v1.json")
        .bind(Option::<String>::None)
        .execute(pool)
        .await
        .context("inserting legacy row")?;
        Ok(())
    }

    #[tokio::test]
    async fn ensure_adds_iceberg_type_for_legacy_catalog() -> Result<()> {
        let (uri, dir) = test_db_uri("ensure-adds-column").await?;
        let opts = SqliteConnectOptions::from_str(&uri)
            .map(|opts| opts.create_if_missing(true))
            .with_context(|| format!("parsing sqlite uri: {uri}"))?;
        let pool = SqlitePoolOptions::new()
            .max_connections(1)
            .connect_with(opts)
            .await
            .context("opening sqlite for legacy setup")?;
        insert_table_row_without_iceberg_type(&pool).await?;
        drop(pool);

        ensure_catalog_iceberg_type_column(&uri).await?;

        let cols = fetch_columns(&uri).await?;
        assert!(cols.iter().any(|c| c == "iceberg_type"));

        let opts = SqliteConnectOptions::from_str(&uri)
            .map(|opts| opts.create_if_missing(true))
            .with_context(|| format!("parsing sqlite uri: {uri}"))?;
        let pool = SqlitePoolOptions::new()
            .max_connections(1)
            .connect_with(opts)
            .await
            .context("reopening sqlite for backfill check")?;
        let row = sqlx::query(
            "SELECT iceberg_type FROM iceberg_tables
             WHERE catalog_name = ? AND table_namespace = ? AND table_name = ?",
        )
        .bind("funbuns")
        .bind("funbuns")
        .bind("primes")
        .fetch_one(&pool)
        .await
        .context("reading backfilled iceberg_type")?;
        let value: String = row.try_get("iceberg_type").context("extracting iceberg_type")?;
        assert_eq!(value, "TABLE");

        std::fs::remove_dir_all(dir).ok();
        Ok(())
    }

    #[tokio::test]
    async fn ensure_is_idempotent_when_column_already_exists() -> Result<()> {
        let (uri, dir) = test_db_uri("ensure-idempotent").await?;
        let opts = SqliteConnectOptions::from_str(&uri)
            .map(|opts| opts.create_if_missing(true))
            .with_context(|| format!("parsing sqlite uri: {uri}"))?;
        let pool = SqlitePoolOptions::new()
            .max_connections(1)
            .connect_with(opts)
            .await
            .context("opening sqlite for setup")?;
        sqlx::query(
            "CREATE TABLE iceberg_tables (
                catalog_name TEXT NOT NULL,
                table_namespace TEXT NOT NULL,
                table_name TEXT NOT NULL,
                metadata_location TEXT NOT NULL,
                previous_metadata_location TEXT,
                iceberg_type VARCHAR(5),
                PRIMARY KEY (catalog_name, table_namespace, table_name)
            )",
        )
        .execute(&pool)
        .await
        .context("creating schema with iceberg_type")?;
        drop(pool);

        ensure_catalog_iceberg_type_column(&uri).await?;
        ensure_catalog_iceberg_type_column(&uri).await?;

        let cols = fetch_columns(&uri).await?;
        assert_eq!(cols.iter().filter(|c| *c == "iceberg_type").count(), 1);

        std::fs::remove_dir_all(dir).ok();
        Ok(())
    }
}

pub async fn open_sql_catalog(uri: &str, warehouse: &str) -> Result<SqlCatalog> {
    ensure_catalog_iceberg_type_column(uri).await?;
    SqlCatalogBuilder::default()
        .uri(uri)
        .warehouse_location(warehouse)
        .sql_bind_style(SqlBindStyle::QMark)
        .with_storage_factory(Arc::new(LocalFsStorageFactory))
        .load("funbuns".to_string(), HashMap::new())
        .await
        .context("opening SqlCatalog")
}

pub async fn append_files(
    catalog: &dyn Catalog,
    table_name: &str,
    rows: &[ManifestRow],
    snapshot_properties: HashMap<String, String>,
) -> Result<Table> {
    let ident = TableIdent::new(
        NamespaceIdent::new(NAMESPACE.to_string()),
        table_name.to_string(),
    );
    let table = catalog
        .load_table(&ident)
        .await
        .with_context(|| format!("loading table {ident:?}"))?;

    let mut data_files = Vec::with_capacity(rows.len());
    for row in rows {
        let df = parquet_file_to_data_file(&row.path, table.metadata())
            .with_context(|| format!("DataFile build for {}", row.path.display()))?;
        data_files.push(df);
    }

    let tx = Transaction::new(&table);
    let action = tx
        .fast_append()
        .add_data_files(data_files)
        .set_snapshot_properties(snapshot_properties);
    let tx = action.apply(tx).context("apply fast_append")?;
    tx.commit(catalog).await.context("commit transaction")
}

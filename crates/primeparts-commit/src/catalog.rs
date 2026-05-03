use std::collections::HashMap;
use std::sync::Arc;

use anyhow::{Context, Result};
use iceberg::io::LocalFsStorageFactory;
use iceberg::table::Table;
use iceberg::transaction::{ApplyTransactionAction, Transaction};
use iceberg::{Catalog, CatalogBuilder, NamespaceIdent, TableIdent};
use iceberg_catalog_sql::{SqlBindStyle, SqlCatalog, SqlCatalogBuilder};
use primeparts_core::parquet_file_to_data_file;
use primeparts_core::table_paths::NAMESPACE;

use crate::manifest::ManifestRow;

pub async fn open_sql_catalog(uri: &str, warehouse: &str) -> Result<SqlCatalog> {
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

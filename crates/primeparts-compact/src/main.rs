use std::collections::HashMap;
use std::process::ExitCode;
use std::str::FromStr;
use std::sync::Arc;

use anyhow::{Context, Result};
use clap::{Parser, Subcommand};
use iceberg::io::LocalFsStorageFactory;
use iceberg::{Catalog, CatalogBuilder, NamespaceIdent, TableIdent};
use iceberg_catalog_sql::{SqlBindStyle, SqlCatalog, SqlCatalogBuilder};
use serde::Serialize;
use sqlx::Row;
use sqlx::sqlite::{SqliteConnectOptions, SqlitePoolOptions};
use tracing_subscriber::EnvFilter;

mod planner;

const DEFAULT_WAREHOUSE: &str = "file:///media/extssd/research/dioph.pp/data/iceberg/warehouse";
const DEFAULT_SQLITE: &str = "sqlite:////media/extssd/research/dioph.pp/data/iceberg/catalog.db";
const NAMESPACE: &str = "funbuns";

#[derive(Parser, Debug)]
#[command(name = "primeparts-compact")]
struct Args {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Verify that iceberg-rust can load the source tables after void normalization.
    CheckSource {
        #[arg(long, default_value = DEFAULT_WAREHOUSE)]
        warehouse: String,

        #[arg(long, env = "FUNBUNS_CATALOG_URI", default_value = DEFAULT_SQLITE)]
        sqlite: String,

        #[arg(
            long,
            value_delimiter = ',',
            default_value = "primes,decompositions,boundaries"
        )]
        tables: Vec<String>,
    },
    /// Derive the per-bucket plan from funbuns.boundaries + locked calibration
    /// constants and print it as JSON. Used by the rewriter in-process; this
    /// subcommand exists for inspection and handoff recording.
    Plan {
        #[arg(long, default_value = DEFAULT_WAREHOUSE)]
        warehouse: String,

        #[arg(long, env = "FUNBUNS_CATALOG_URI", default_value = DEFAULT_SQLITE)]
        sqlite: String,

        #[arg(long, default_value_t = 1)]
        version: i32,
    },
}

#[derive(Serialize)]
struct SourceTableSummary {
    table: String,
    metadata_location: String,
    current_snapshot_id: Option<i64>,
    default_partition_spec_id: i32,
    default_sort_order_id: i64,
    partition_spec: String,
}

#[tokio::main]
async fn main() -> ExitCode {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .with_writer(std::io::stderr)
        .init();

    match run().await {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("primeparts-compact: {e:#}");
            ExitCode::from(1)
        }
    }
}

async fn run() -> Result<()> {
    let args = Args::parse();
    match args.command {
        Command::CheckSource {
            warehouse,
            sqlite,
            tables,
        } => check_source(&warehouse, &sqlite, &tables).await,
        Command::Plan {
            warehouse,
            sqlite,
            version,
        } => plan(&warehouse, &sqlite, version).await,
    }
}

async fn plan(warehouse: &str, sqlite: &str, version: i32) -> Result<()> {
    let catalog = open_sql_catalog(sqlite, warehouse).await?;
    let plan = planner::plan_buckets(&catalog, version).await?;
    println!("{}", serde_json::to_string_pretty(&plan)?);
    Ok(())
}

async fn check_source(warehouse: &str, sqlite: &str, tables: &[String]) -> Result<()> {
    let catalog = open_sql_catalog(sqlite, warehouse).await?;
    let namespace = NamespaceIdent::new(NAMESPACE.to_string());
    let mut summaries = Vec::with_capacity(tables.len());
    for table_name in tables {
        let ident = TableIdent::new(namespace.clone(), table_name.to_string());
        let table = catalog
            .load_table(&ident)
            .await
            .with_context(|| format!("loading table {ident:?}"))?;
        let metadata = table.metadata();
        summaries.push(SourceTableSummary {
            table: table_name.clone(),
            metadata_location: table
                .metadata_location()
                .map(|location| location.to_string())
                .unwrap_or_default(),
            current_snapshot_id: metadata.current_snapshot_id().map(|id| id as i64),
            default_partition_spec_id: metadata.default_partition_spec_id(),
            default_sort_order_id: metadata.default_sort_order_id() as i64,
            partition_spec: format!("{:?}", metadata.default_partition_spec()),
        });
    }
    println!("{}", serde_json::to_string_pretty(&summaries)?);
    Ok(())
}

async fn open_sql_catalog(uri: &str, warehouse: &str) -> Result<SqlCatalog> {
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

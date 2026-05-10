mod catalog;
mod footer_kv;
mod hms_sync;
mod manifest;
mod prompt;
mod settings;
mod snapshot_props;

use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{Context, Result};
use clap::Parser;
use iceberg::{Catalog, NamespaceIdent, TableIdent};
use serde::Serialize;
use tracing::info;
use tracing_subscriber::EnvFilter;

use crate::catalog::{append_files, open_sql_catalog};
use crate::manifest::{group_by_table, parse_jsonl, verify_against_footer};
use crate::settings::{WarehouseStandingCheck, load as load_settings, resolve_check};

#[derive(Parser, Debug)]
#[command(name = "primeparts-commit")]
struct Args {
    #[arg(long)]
    manifest: Option<PathBuf>,

    #[arg(long)]
    warehouse: String,

    #[arg(
        long,
        env = "FUNBUNS_CATALOG_URI",
        default_value = "sqlite:////media/extssd/research/dioph.pp/data/iceberg/catalog.db"
    )]
    sqlite: String,

    #[arg(
        long,
        env = "FUNBUNS_HMS_URI",
        default_value = "thrift://localhost:9083"
    )]
    hms: String,

    #[arg(long)]
    skip_hms: bool,

    /// Override warehouse-standing pre-check mode: `ask`, `run`, or `skip`.
    #[arg(long, value_name = "MODE")]
    warehouse_standing: Option<String>,

    #[arg(long)]
    dry_run: bool,

    /// Sync HMS metadata pointers from the current SqlCatalog state only.
    #[arg(long)]
    sync_hms_only: bool,
}

// Exit codes match the contract in crates/PLAN.md:
//   0 ok · 2 verify fail · 3 warehouse-standing fail
//   4 catalog conflict · 5 HMS sync fail (sqlite committed; rerun retries HMS only)
const EXIT_VERIFY_FAIL: u8 = 2;
const EXIT_WAREHOUSE_STANDING_FAIL: u8 = 3;
const EXIT_CATALOG_CONFLICT: u8 = 4;
const EXIT_HMS_SYNC_FAIL: u8 = 5;

#[tokio::main]
async fn main() -> ExitCode {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .with_writer(std::io::stderr)
        .init();
    match run().await {
        Ok(code) => ExitCode::from(code),
        Err(e) => {
            eprintln!("primeparts-commit: {e:#}");
            ExitCode::from(1)
        }
    }
}

async fn run() -> Result<u8> {
    let args = Args::parse();
    if args.sync_hms_only {
        return run_sync_hms_only(&args).await;
    }

    let manifest = args
        .manifest
        .as_ref()
        .ok_or_else(|| anyhow::anyhow!("--manifest is required unless --sync-hms-only is set"))?;

    let rows = parse_jsonl(manifest)?;
    info!(
        "loaded {} manifest rows from {}",
        rows.len(),
        manifest.display()
    );

    for row in &rows {
        if let Err(e) = verify_against_footer(row) {
            eprintln!("verify failed for {}: {e:#}", row.path.display());
            return Ok(EXIT_VERIFY_FAIL);
        }
    }
    info!("manifest verified against parquet footers");

    let settings = load_settings().unwrap_or_default();
    let env_var = std::env::var("PRIMEPARTS_WAREHOUSE_STANDING").ok();
    let mode = resolve_check(
        &settings,
        env_var.as_deref(),
        args.warehouse_standing.as_deref(),
    )?;
    let should_check = match mode {
        WarehouseStandingCheck::AlwaysAsk => prompt::confirm("Run warehouse-standing pre-check?")?,
        WarehouseStandingCheck::AlwaysRun => true,
        WarehouseStandingCheck::NeverAsk => false,
    };
    if should_check {
        if let Err(e) = prompt::run_warehouse_standing_check() {
            eprintln!("{e:#}");
            return Ok(EXIT_WAREHOUSE_STANDING_FAIL);
        }
    }

    if args.dry_run {
        info!("dry-run: stopping before catalog mutation");
        return Ok(0);
    }

    let by_table = group_by_table(rows);
    let catalog = open_sql_catalog(&args.sqlite, &args.warehouse).await?;

    let mut sync_targets = Vec::new();
    let mut summary = Summary::default();
    summary.warehouse = args.warehouse.clone();
    summary.sqlite = args.sqlite.clone();
    for (table, table_rows) in &by_table {
        let props = snapshot_props::aggregate(table_rows);
        let new_table = match append_files(&catalog, table, table_rows, props).await {
            Ok(t) => t,
            Err(e) => {
                eprintln!("catalog append failed for {table}: {e:#}");
                return Ok(EXIT_CATALOG_CONFLICT);
            }
        };
        let metadata_location = new_table
            .metadata_location()
            .map(|m| m.to_string())
            .context("table missing metadata_location after commit")?;
        info!(
            "appended {} files to {table} → {metadata_location}",
            table_rows.len()
        );
        sync_targets.push(hms_sync::SyncTarget {
            table: table.clone(),
            new_metadata_location: metadata_location.clone(),
        });
        summary.absorb(table, table_rows, &metadata_location);
    }

    if !args.skip_hms {
        let address = match hms_sync::parse_address(&args.hms) {
            Ok(a) => a,
            Err(e) => {
                summary.hms_synced = false;
                eprintln!("HMS sync failed (address): {e:#}");
                println!("{}", serde_json::to_string(&summary).unwrap());
                return Ok(EXIT_HMS_SYNC_FAIL);
            }
        };
        let client = hms_sync::build_client(address);
        if let Err(e) = hms_sync::sync(&client, &sync_targets).await {
            summary.hms_synced = false;
            eprintln!("HMS sync failed: {e:#}");
            eprintln!("hint: sqlite was committed; rerun with the same args to retry HMS only.");
            println!("{}", serde_json::to_string(&summary).unwrap());
            return Ok(EXIT_HMS_SYNC_FAIL);
        }
        summary.hms_synced = true;
        info!("HMS sync complete");
    } else {
        info!("--skip-hms: skipping HMS sync");
    }

    println!("{}", serde_json::to_string(&summary).unwrap());
    Ok(0)
}

async fn run_sync_hms_only(args: &Args) -> Result<u8> {
    let catalog = open_sql_catalog(&args.sqlite, &args.warehouse).await?;
    let ns = NamespaceIdent::new(primeparts_core::table_paths::NAMESPACE.to_string());
    let mut targets = Vec::with_capacity(2);
    let mut metadata_locations = std::collections::BTreeMap::new();
    for table in ["primes", "decompositions"] {
        let ident = TableIdent::new(ns.clone(), table.to_string());
        let loaded = catalog
            .load_table(&ident)
            .await
            .with_context(|| format!("loading table {ident:?}"))?;
        let metadata_location = loaded
            .metadata_location()
            .map(|m| m.to_string())
            .context("table missing metadata_location")?;
        metadata_locations.insert(table.to_string(), metadata_location.clone());
        targets.push(hms_sync::SyncTarget {
            table: table.to_string(),
            new_metadata_location: metadata_location,
        });
    }

    let address = hms_sync::parse_address(&args.hms)?;
    let client = hms_sync::build_client(address);
    if args.dry_run {
        let preview_rows = hms_sync::preview(&client, &targets).await?;
        let updates_needed = preview_rows.iter().filter(|p| p.needs_update).count();
        let preview = preview_rows
            .into_iter()
            .map(SyncPreviewRow::from)
            .collect::<Vec<_>>();
        let summary = SyncOnlySummary {
            warehouse: args.warehouse.clone(),
            sqlite: args.sqlite.clone(),
            hms: args.hms.clone(),
            dry_run: true,
            updates_needed,
            metadata_locations,
            preview,
            hms_synced: false,
        };
        println!("{}", serde_json::to_string(&summary).unwrap());
        return Ok(0);
    }
    hms_sync::sync(&client, &targets).await?;
    let summary = SyncOnlySummary {
        warehouse: args.warehouse.clone(),
        sqlite: args.sqlite.clone(),
        hms: args.hms.clone(),
        dry_run: false,
        updates_needed: 0,
        metadata_locations,
        preview: Vec::new(),
        hms_synced: true,
    };
    println!("{}", serde_json::to_string(&summary).unwrap());
    Ok(0)
}

#[derive(Debug, Default, Serialize)]
struct Summary {
    warehouse: String,
    sqlite: String,
    files: usize,
    prime_files: usize,
    decomposition_files: usize,
    prime_rows: i64,
    decomposition_rows: i64,
    bytes: i64,
    max_p: i64,
    max_commit_seq: i64,
    metadata_locations: std::collections::BTreeMap<String, String>,
    hms_synced: bool,
}

impl Summary {
    fn absorb(&mut self, table: &str, rows: &[manifest::ManifestRow], meta_loc: &str) {
        self.files += rows.len();
        for r in rows {
            self.bytes += r.bytes;
            if r.p_max > self.max_p {
                self.max_p = r.p_max;
            }
            if r.commit_seq > self.max_commit_seq {
                self.max_commit_seq = r.commit_seq;
            }
        }
        match table {
            "primes" => {
                self.prime_files += rows.len();
                self.prime_rows += rows.iter().map(|r| r.rows).sum::<i64>();
            }
            "decompositions" => {
                self.decomposition_files += rows.len();
                self.decomposition_rows += rows.iter().map(|r| r.rows).sum::<i64>();
            }
            _ => {}
        }
        self.metadata_locations
            .insert(table.to_string(), meta_loc.to_string());
    }
}

#[derive(Debug, Serialize)]
struct SyncOnlySummary {
    warehouse: String,
    sqlite: String,
    hms: String,
    dry_run: bool,
    updates_needed: usize,
    metadata_locations: std::collections::BTreeMap<String, String>,
    preview: Vec<SyncPreviewRow>,
    hms_synced: bool,
}

#[derive(Debug, Serialize)]
struct SyncPreviewRow {
    table: String,
    current_metadata_location: Option<String>,
    new_metadata_location: String,
    needs_update: bool,
}

impl From<hms_sync::SyncPreview> for SyncPreviewRow {
    fn from(value: hms_sync::SyncPreview) -> Self {
        Self {
            table: value.table,
            current_metadata_location: value.current_metadata_location,
            new_metadata_location: value.new_metadata_location,
            needs_update: value.needs_update,
        }
    }
}

mod walker;
mod writer;

use std::fs::File;
use std::io::Write;
use std::path::PathBuf;
use std::process::ExitCode;

use anyhow::{Context, Result, anyhow};
use clap::Parser;
use tracing::info;
use tracing_subscriber::EnvFilter;

use crate::walker::walk;
use crate::writer::{CompactConfig, compact};

#[derive(Parser, Debug)]
#[command(name = "primeparts-compact")]
struct Args {
    /// Source warehouse data root, e.g. /…/iceberg/warehouse/funbuns
    #[arg(long)]
    src_root: PathBuf,

    /// Destination data root for compacted output, e.g. /…/iceberg-staging/warehouse/funbuns
    #[arg(long)]
    dst_root: PathBuf,

    /// Tables to compact (sub-dirs of src_root). Defaults to both.
    #[arg(long, value_delimiter = ',', default_value = "primes,decompositions")]
    tables: Vec<String>,

    /// W in TruncateTransform(W) on `p`. Output files are routed into
    /// dst_root/{table}/data/p_trunc={N}/.
    #[arg(long, default_value_t = 10_000_000_000)]
    partition_w: i64,

    /// Flush a row group when the in-progress write buffer reaches this size.
    #[arg(long, default_value_t = 256 * 1024 * 1024)]
    target_row_group_bytes: usize,

    /// Close an output file after this many flushed row groups.
    #[arg(long, default_value_t = 4)]
    max_row_groups_per_file: usize,

    /// Where to write the JSONL manifest (consumed by primeparts-commit).
    #[arg(long)]
    manifest_out: PathBuf,

    /// Compact only the first N source files per table (smoke test).
    #[arg(long)]
    limit: Option<usize>,
}

fn main() -> ExitCode {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        .with_writer(std::io::stderr)
        .init();
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("primeparts-compact: {e:#}");
            ExitCode::from(1)
        }
    }
}

fn run() -> Result<()> {
    let args = Args::parse();
    if args.partition_w <= 0 {
        return Err(anyhow!("--partition-w must be positive"));
    }
    if args.target_row_group_bytes == 0 {
        return Err(anyhow!("--target-row-group-bytes must be positive"));
    }
    if args.max_row_groups_per_file == 0 {
        return Err(anyhow!("--max-row-groups-per-file must be positive"));
    }
    if let Some(parent) = args.manifest_out.parent() {
        std::fs::create_dir_all(parent).with_context(|| format!("mkdir {}", parent.display()))?;
    }
    let mut manifest_file = File::create(&args.manifest_out)
        .with_context(|| format!("creating manifest {}", args.manifest_out.display()))?;

    for table in &args.tables {
        let src_data = args.src_root.join(table).join("data");
        let dst_data = args.dst_root.join(table).join("data");
        if !src_data.is_dir() {
            return Err(anyhow!(
                "source data dir does not exist: {}",
                src_data.display()
            ));
        }
        info!("walking {}", src_data.display());
        let mut sources = walk(&src_data)?;
        if let Some(n) = args.limit {
            sources.truncate(n);
        }
        info!(
            "compact[{}]: {} source files (batch ids {}..{})",
            table,
            sources.len(),
            sources.first().map(|f| f.batch_id).unwrap_or(-1),
            sources.last().map(|f| f.batch_id).unwrap_or(-1),
        );

        let cfg = CompactConfig {
            table: table.clone(),
            out_root: dst_data,
            partition_w: args.partition_w,
            target_row_group_bytes: args.target_row_group_bytes,
            max_row_groups_per_file: args.max_row_groups_per_file,
        };
        let manifest_rows = compact(&sources, &cfg)?;
        for row in &manifest_rows {
            let line = serde_json::to_string(row).context("serializing manifest row")?;
            manifest_file
                .write_all(line.as_bytes())
                .context("writing manifest line")?;
            manifest_file.write_all(b"\n").context("writing newline")?;
        }
        info!(
            "compact[{}]: wrote {} manifest rows",
            table,
            manifest_rows.len()
        );
    }
    info!("manifest written to {}", args.manifest_out.display());
    Ok(())
}

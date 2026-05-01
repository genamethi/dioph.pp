use std::io::{self, BufRead, Write};
use std::process::Command;

use anyhow::{bail, Context, Result};

pub fn confirm(message: &str) -> Result<bool> {
    let stderr = io::stderr();
    let mut stderr = stderr.lock();
    write!(stderr, "{message} [y/N]: ").context("prompt write")?;
    stderr.flush().context("prompt flush")?;

    let stdin = io::stdin();
    let mut line = String::new();
    stdin.lock().read_line(&mut line).context("prompt read")?;
    Ok(matches!(line.trim().to_lowercase().as_str(), "y" | "yes"))
}

pub fn run_warehouse_standing_check() -> Result<()> {
    let status = Command::new("python")
        .args(["-m", "primeparts.native_iceberg", "--check-warehouse"])
        .status()
        .context("spawning python -m primeparts.native_iceberg --check-warehouse")?;
    if !status.success() {
        bail!(
            "warehouse-standing check failed (exit {})",
            status.code().unwrap_or(-1)
        );
    }
    Ok(())
}

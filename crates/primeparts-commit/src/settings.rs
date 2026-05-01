use std::path::PathBuf;
use std::str::FromStr;

use anyhow::{anyhow, Context, Result};
use serde::Deserialize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum WarehouseStandingCheck {
    #[default]
    AlwaysAsk,
    AlwaysRun,
    NeverAsk,
}

impl FromStr for WarehouseStandingCheck {
    type Err = anyhow::Error;
    fn from_str(s: &str) -> std::result::Result<Self, Self::Err> {
        match s {
            "always-ask" | "ask" => Ok(Self::AlwaysAsk),
            "always-run" | "run" => Ok(Self::AlwaysRun),
            "never-ask" | "skip" => Ok(Self::NeverAsk),
            other => Err(anyhow!("unknown warehouse_standing_check value: {other}")),
        }
    }
}

#[derive(Debug, Default, Deserialize)]
pub struct Settings {
    #[serde(default)]
    pub warehouse_standing_check: Option<String>,
}

pub fn config_path() -> Option<PathBuf> {
    dirs::config_dir().map(|p| p.join("primeparts/config.toml"))
}

pub fn load() -> Result<Settings> {
    let Some(path) = config_path() else {
        return Ok(Settings::default());
    };
    if !path.exists() {
        return Ok(Settings::default());
    }
    let s = std::fs::read_to_string(&path)
        .with_context(|| format!("reading {}", path.display()))?;
    toml::from_str(&s).with_context(|| format!("parsing {}", path.display()))
}

pub fn resolve_check(
    file: &Settings,
    env: Option<&str>,
    cli: Option<&str>,
) -> Result<WarehouseStandingCheck> {
    if let Some(c) = cli {
        return c.parse();
    }
    if let Some(e) = env {
        return e.parse();
    }
    if let Some(f) = &file.warehouse_standing_check {
        return f.parse();
    }
    Ok(WarehouseStandingCheck::default())
}

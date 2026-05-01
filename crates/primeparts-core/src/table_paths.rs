// Warehouse path conventions shared between commit and future compact
// binaries. Mirrors native/src/generate.cc:685-691:
//   <warehouse>/funbuns/<table>/data/commit_seq=N/<table>_b00000N_NNN.parquet

use std::path::{Path, PathBuf};

pub const NAMESPACE: &str = "funbuns";
pub const TABLE_PRIMES: &str = "primes";
pub const TABLE_DECOMPOSITIONS: &str = "decompositions";

pub fn data_dir(warehouse: &Path, table: &str, commit_seq: i64) -> PathBuf {
    warehouse
        .join(NAMESPACE)
        .join(table)
        .join("data")
        .join(format!("commit_seq={commit_seq}"))
}

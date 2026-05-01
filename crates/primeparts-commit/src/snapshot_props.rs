use std::collections::HashMap;

use crate::manifest::ManifestRow;

pub fn aggregate(rows: &[ManifestRow]) -> HashMap<String, String> {
    let max_p = rows.iter().map(|r| r.p_max).max().unwrap_or(0);
    let max_commit_seq = rows.iter().map(|r| r.commit_seq).max().unwrap_or(0);
    HashMap::from([
        ("funbuns.max_p".to_string(), max_p.to_string()),
        ("funbuns.max_commit_seq".to_string(), max_commit_seq.to_string()),
    ])
}

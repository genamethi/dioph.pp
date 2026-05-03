// Lazy HMS sync — port of scripts/sync_hms.py.
//
// After the SqlCatalog commit, HMS still points at the previous
// metadata_location. This walks each touched table, fetches the current HMS
// row, swaps `metadata_location` (and stashes the prior value into
// `previous_metadata_location`), and alters via
// alter_table_with_environment_context. Best-effort by design: a failure
// here does not roll back sqlite, and re-running is idempotent.

use std::net::{SocketAddr, ToSocketAddrs};

use anyhow::{Context, Result, anyhow, bail};
use hive_metastore::{
    EnvironmentContext, GetTableRequest, ThriftHiveMetastoreClient,
    ThriftHiveMetastoreClientBuilder,
};
use pilota::FastStr;
use primeparts_core::table_paths::NAMESPACE;
use volo_thrift::MaybeException;

pub struct SyncTarget {
    pub table: String,
    pub new_metadata_location: String,
}

pub fn parse_address(uri: &str) -> Result<SocketAddr> {
    let host_port = uri.strip_prefix("thrift://").unwrap_or(uri);
    host_port
        .to_socket_addrs()
        .with_context(|| format!("resolving HMS address {uri}"))?
        .next()
        .ok_or_else(|| anyhow!("no addresses for {uri}"))
}

pub fn build_client(address: SocketAddr) -> ThriftHiveMetastoreClient {
    ThriftHiveMetastoreClientBuilder::new("hms")
        .address(address)
        .make_codec(volo_thrift::codec::default::DefaultMakeCodec::buffered())
        .build()
}

fn strip_scheme(p: &str) -> &str {
    p.strip_prefix("file://").unwrap_or(p)
}

pub async fn sync(client: &ThriftHiveMetastoreClient, targets: &[SyncTarget]) -> Result<()> {
    for target in targets {
        let db = FastStr::from(NAMESPACE);
        let tbl = FastStr::from(target.table.clone());

        let resp = client
            .get_table_req(GetTableRequest {
                db_name: db.clone(),
                tbl_name: tbl.clone(),
                capabilities: None,
            })
            .await
            .with_context(|| format!("HMS get_table_req {NAMESPACE}.{}", target.table))?;

        let mut hms_table = match resp {
            MaybeException::Ok(r) => r.table,
            MaybeException::Exception(e) => bail!(
                "HMS get_table_req exception for {NAMESPACE}.{}: {e:?}",
                target.table
            ),
        };

        let mut params = hms_table.parameters.take().unwrap_or_default();
        let key_meta = FastStr::from("metadata_location");
        let key_prev = FastStr::from("previous_metadata_location");
        let prev = params.get(&key_meta).cloned();
        params.insert(
            key_meta,
            FastStr::from(strip_scheme(&target.new_metadata_location).to_string()),
        );
        if let Some(prev) = prev {
            params.insert(key_prev, prev);
        }
        hms_table.parameters = Some(params);

        let env_ctx = EnvironmentContext { properties: None };
        let resp = client
            .alter_table_with_environment_context(db, tbl, hms_table, env_ctx)
            .await
            .with_context(|| format!("HMS alter_table {NAMESPACE}.{}", target.table))?;
        match resp {
            MaybeException::Ok(()) => {}
            MaybeException::Exception(e) => bail!(
                "HMS alter_table exception {NAMESPACE}.{}: {e:?}",
                target.table
            ),
        }
    }
    Ok(())
}

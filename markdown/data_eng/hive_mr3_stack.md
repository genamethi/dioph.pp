# Hive + MR3 + HMS + k3s stack

Single-node k3s deployment of Hive 4.2 on MR3 (DataMonad's execution
backend) serving the `primeparts` Iceberg warehouse at
`/media/extssd/research/dioph.pp/data/ib-staging/`. Node: `promix`
(Ryzen 9 5900X, 12 physical / 24 logical cores, 16 GiB RAM).

Tables are committed natively via iceberg-cpp → HMS-backed Iceberg REST
Catalog. The previous Python/SqlCatalog/sync_hms.py path is **retired**
for `primeparts.*`; it is documented at the bottom of this file only as
historical context for the funbuns-era data still on disk.

## Who does what

| Pod / Process | Role | Speaks | Listens on | Used by |
|---|---|---|---|---|
| **metastore** (HMS) | Hive metastore + Iceberg REST Catalog server | Thrift, HTTP/REST, JDBC (out) | `:9850` Thrift, `:9090` REST `/iceberg/v1` | HS2, MR3 workers, pyiceberg (Thrift); iceberg-cpp, any IRC client (REST) |
| **hiveserver2** (HS2) | SQL planner + driver, submits DAGs | Thrift, HTTP, IPC (out) | `:9852` Thrift, `:10001` HTTP | beeline, pyhive (host via NodePort 31140 / 31680) |
| **mr3master** | DAG application master — schedules tasks, spawns workers | IPC | `:80`, `:9890` | HS2 (submits DAGs here) |
| **mr3worker** (ephemeral) | Holds Tez task slots, runs DAG operators, reads warehouse | Thrift (out to HMS) | none | mr3master spawns them on demand |
| **mysql** | Catalog metadata DB | MySQL wire | `:3306` (cluster-only) | HMS only — never touched by clients |

### Where does Tez fit?

Tez (specifically Tez-MR3, the fork) provides the **DAG runtime** — shuffle, grouping, vertex management — used by Hive 4's physical operators. It's only on the SQL execution path:

`beeline → HS2 → mr3master → mr3workers` (all using Tez-MR3 jars)

It is **not** involved when:
- a client speaks IRC (`iceberg-cpp` → `:9090`)
- a client speaks Thrift HMS (`pyiceberg` → `:9850`)
- native C++ tools (`covering_sieve`, `rewrite`, etc.) write parquet to the warehouse directly
- HMS or pyiceberg commit Iceberg snapshots

In other words: if no SQL DAG is being executed, the only "live" pods are HMS + mysql. HS2 + mr3master are spun up for SQL but otherwise idle. mr3workers don't exist until a DAG needs them.

## Topology

```
   pyhive / polars (host)        iceberg-cpp (host)
              │                          │
   NodePort 31140 ──►            NodePort 9090 (LoadBalancer)
              │                          │
              ▼                          ▼
      hiveserver2:9852           hivemr3-metastore-0
              │                  ┌────────┴───────────────┐
              │ DAG submit       │ Thrift :9850 (HMS)     │
              ▼                  │ HTTP  :9090 (Iceberg   │
   client-am-config ──►          │        REST Catalog,   │
   mr3master-<ts>-0              │        /iceberg/v1/…)  │
              │                  └────────┬───────────────┘
              │ spawns                    │ JDBC
              ▼                           ▼
   mr3worker-<hash>-<seq> ───► mysql-server namespace (MySQL 8)
                  reads metadata via Thrift

   warehouse: /media/extssd/research/dioph.pp/data/ib-staging
   (hostPath mounted into every pod that needs it)
   (legacy /iceberg warehouse is also mounted read-only for residual
    funbuns-era tables; will be archived in a future cutover)
```

## Config locations

All MR3 configs live in the upstream MR3 distribution at `mr3/` (an inner
git repo; ignored by the outer funbuns repo — see `.gitignore`).

| File                                       | Purpose                                                      |
|--------------------------------------------|--------------------------------------------------------------|
| `mr3/kubernetes/env.sh`                    | Pod environment: heap sizes, warehouse path, HMS creds       |
| `mr3/kubernetes/conf/hive-site.xml`        | Hive + MR3 task sizing, authz settings, Iceberg catalog URI  |
| `mr3/kubernetes/conf/mr3-site.xml`         | MR3 master/worker pod sizing, mounts, hostpaths              |
| `mr3/kubernetes/conf/core-site.xml`        | Hadoop FS defaults                                           |
| `mr3/kubernetes/conf/tez-site.xml`         | Tez split / grouping parameters                              |
| `mr3/kubernetes/yaml/metastore.yaml`       | StatefulSet for HMS pod (pod-level request/limit)            |
| `mr3/kubernetes/yaml/hive.yaml`            | Deployment for HS2 pod                                       |
| `mr3/kubernetes/yaml/workdir-pvc.yaml`     | PVC for /opt/mr3-run/work-dir (ReadWriteOnce, local-path)    |
| `mr3/kubernetes/yaml/metastore-rest.yaml`  | LoadBalancer Service exposing HMS IRC server on host :9090   |
| `mr3/kubernetes/yaml/schema-upgrade.yaml`  | One-shot Pod for HMS schema init-or-upgrade                  |
| `mr3/kubernetes/upgrade-schema.sh`         | Driver script that scales sts → applies pod → scales sts back |

Kubernetes objects in the `hivemr3` namespace:

```
configmap/hivemr3-conf-configmap      # contents of conf/
configmap/client-am-config            # mr3 session key + app-id timestamp
configmap/mr3conf-configmap-master    # auto-generated by HS2 on first boot
configmap/mr3conf-configmap-worker    # auto-generated by mr3master
secret/env-secret                     # env.sh
secret/hivemr3-keytab-secret          # (empty placeholder, no kerberos)
service/hiveserver2                   # LoadBalancer; NodePort 31140 → 9852
service/metastore                     # headless ClusterIP :9850
service/service-master-<ts>-0         # ClusterIP for mr3master IPC
statefulset.apps/hivemr3-metastore
deployment.apps/hivemr3-hiveserver2
deployment.apps/mr3master-<ts>-0      # regenerated each HS2 boot
```

## Common commands

### State / watchers
```
kubectl -n hivemr3 get pods -o wide
kubectl -n hivemr3 logs <pod> [--tail=N] [-f]
kubectl -n hivemr3 describe pod <pod> | grep -A5 Events
kubectl top pod -n hivemr3                     # live CPU/mem per pod
kubectl top node promix                        # node headroom

# Live MR3 scheduler / DAG progress
kubectl -n hivemr3 logs -l mr3-pod-role=master-role -f \
  | grep -E "ContainerViewGroup|Tickets|ContainerGroup|insufficient|DAG"
```

### Stack control
```
pixi run kube-down   # scale HS2, HMS, mr3master to 0; evict workers
pixi run kube-up     # scale back to 1; waits for Ready

# Harder reset (drops every pod + the k3s cgroup):
sudo systemctl stop k3s
sudo systemctl start k3s
```

`pixi run kube-up` / `scripts/kube-up.sh` assumes the Kubernetes API is already
running. If it reports a connection failure on `127.0.0.1:6443`, start k3s
first and then rerun the script:

```
sudo systemctl start k3s
kubectl get nodes
pixi run kube-up
```

### Client access
```
# SQL via pyhive, NodePort 31140
from pyhive import hive
conn = hive.Connection(host='localhost', port=31140, username='hive',
                       database='funbuns', auth='NONE')

# pyiceberg against live HMS (port-forward required from host)
kubectl -n hivemr3 port-forward hivemr3-metastore-0 9850:9850 &
FUNBUNS_HMS_URI=thrift://localhost:9850 pixi run python ...
# The `get_table` vs `get_table_req` shim is applied via
# `primeparts._patches`. Import it explicitly in scripts that use HMS directly.
```

### HMS as Iceberg REST Catalog server

HMS now serves the Iceberg REST Catalog spec directly on port 9090. The server
ships with Hive 4.2.0 (`hive-standalone-metastore-rest-catalog-4.2.0.jar`,
class `org.apache.iceberg.rest.HMSCatalogFactory`). Endpoint:

```
http://192.168.1.202:9090/iceberg/v1/...
```

Full OpenAPI surface (config, namespaces, tables, views, register, rename,
transactions/commit, metrics) — same one served by iceberg-rest-fixture but
backed by HMS+MySQL instead of a separate JDBC catalog.

Enabled via two keys in `mr3/kubernetes/conf/hive-site.xml`:

```xml
<property><name>metastore.catalog.servlet.port</name><value>9090</value></property>
<property><name>metastore.catalog.servlet.auth</name><value>none</value></property>
```

Defaults that we rely on (don't set unless changing):
- `metastore.catalog.servlet.factory` = `org.apache.iceberg.rest.HMSCatalogFactory`
- `metastore.iceberg.catalog.servlet.path` = `iceberg`

The pod exposes the port via `containerPort: 9090` in `yaml/metastore.yaml`,
and `yaml/metastore-rest.yaml` is a LoadBalancer Service that forwards the
node IP's 9090 to the metastore pod.

#### HS2 as IRC client (separate switch, currently unused)

`env.sh` still has the `ICEBERG_CATALOG_TYPE` knob for pointing HS2's
`HiveIcebergStorageHandler` at an external REST catalog instead of Thrift
HMS. Leave at default `hive` unless you're running a second catalog
implementation.

### Custom Hive 4.2 image

The IRC server is a Hive 4.2 feature; mr3project's published `mr3project/hive`
tag prefix `4.0.0.mr3.2.x` ships Hive 4.0.0 jars (no IRC). We run a locally
built overlay image:

```
mr3-hive-4.2.0-local/hive:4.2.0
```

Built from `~/fluid/byo/repos/hive-mr3` master4.2.0 branch:

1. Run the upstream packaging build to produce `apache-hive-4.2.0-bin/lib/`
   (includes `hive-standalone-metastore-rest-catalog-4.2.0.jar`).
2. `docker build -f Dockerfile.mr3-overlay -t mr3-hive-4.2.0-local/hive:4.2.0 .`
   in `~/fluid/byo/repos/hive-mr3/packaging/target/apache-hive-4.2.0-bin/`.
   The Dockerfile starts `FROM mr3project/hive:4.0.0.mr3.2.5` and replaces
   `/opt/mr3-run/hive/apache-hive/{lib,bin,scripts}` with the 4.2 build.
3. Pod yamls reference the local tag with `imagePullPolicy: Never`. The
   mr3master pod is spawned by HS2 at runtime, so its pull policy is set
   in `mr3-site.xml` via `mr3.k8s.pod.image.pull.policy=Never`.

### k3s runtime: docker, not containerd

k3s is configured to use the host's docker daemon as its CRI so that
locally-built images appear immediately in the cluster (no `ctr import`,
no local registry). Set once at install in `/etc/rancher/k3s/config.yaml`:

```yaml
docker: true
```

Then `sudo systemctl restart k3s`. Verify with
`kubectl get node -o jsonpath='{.items[0].status.nodeInfo.containerRuntimeVersion}'`
— it should print `docker://...` not `containerd://...`.

Caveats: restarting dockerd takes the CRI out from under k3s for ~30s; if
that's unacceptable, switch to a local registry instead. The deprecation
of `--docker` upstream is handled by k3s shipping the cri-dockerd shim;
for a single-node dev cluster it's not a practical concern.

### Schema init / upgrade after image bumps

When the image's Hive version differs from the schema currently in MySQL,
the metastore pod refuses to start with:

```
Hive Schema version X.Y.Z does not match metastore's schema version A.B.C
```

mr3project's `metastore-service.sh` only handles initial schema creation
(`schematool -initSchema`), not upgrades. Run the upgrade driver:

```
bash mr3/kubernetes/upgrade-schema.sh
```

This scales the StatefulSet to 0, applies a one-shot Pod that runs
`schematool -initOrUpgradeSchema` (idempotent — init on empty DB, upgrade
on drift, no-op when current), waits for success, then scales the
StatefulSet back. Safe to run on every bring-up.

### Config changes → pod propagation
MR3 caches many settings at HS2 startup (the auto-generated
`mr3conf-configmap-master` + the mr3master Deployment). After editing
`mr3/kubernetes/**`, re-roll through:

```
# refresh configmap + env secret
kubectl -n hivemr3 create configmap hivemr3-conf-configmap \
  --from-file=mr3/kubernetes/conf/ --dry-run=client -o yaml | kubectl apply -f -
kubectl -n hivemr3 create secret generic env-secret \
  --from-file=env.sh=mr3/kubernetes/env.sh --dry-run=client -o yaml | kubectl apply -f -

# reapply pod-spec YAMLs if hive.yaml/metastore.yaml changed
kubectl apply -f mr3/kubernetes/yaml/metastore.yaml \
               -f mr3/kubernetes/yaml/hive.yaml

# drop mr3master so it rereads pod sizes (see "Gotchas" below)
kubectl -n hivemr3 delete deployment -l mr3-pod-role=master-role
kubectl -n hivemr3 delete configmap mr3conf-configmap-master client-am-config
kubectl -n hivemr3 delete service -l mr3-pod-role=master-role
NEW_TS=$(date +%s | tail -c 5)
kubectl -n hivemr3 create configmap client-am-config \
  --from-literal=key=$(openssl rand -hex 16) \
  --from-literal=timestamp=$NEW_TS \
  --from-literal=mr3sessionid=$(openssl rand -hex 8)

# bounce HS2 so it regenerates mr3conf-configmap-master and launches
# a fresh mr3master Deployment
kubectl -n hivemr3 rollout restart deploy/hivemr3-hiveserver2
```

### Database location — one-time cutover step

When a namespace's iceberg tables are moved to a new warehouse path,
the HMS *database* row also needs updating. IRC's `RegisterTable` only
touches the iceberg table row; the database `location_uri` is a
separate Hive-catalog concept that Hive uses as the *default parent*
for any tables Hive itself creates (MVs especially).

For `primeparts`, the cutover step is:

```sql
ALTER DATABASE primeparts SET LOCATION
    'file:/media/extssd/research/dioph.pp/data/ib-staging/primeparts.db';
```

Run via `pixi run python scripts/hive_sql.py -e '...'`. Verify with
`DESCRIBE DATABASE primeparts`. Tables that already exist keep their
absolute paths; only future Hive-created tables inherit the new
default. See memory `[[feedback-irc-vs-hive-db-location]]`.

## Current resource settings (Config A, 2026-05-27)

| Lever                                           | File                | Current        | Notes                                    |
|-------------------------------------------------|---------------------|----------------|------------------------------------------|
| HS2 pod request/limit                           | `hive.yaml`         | 1280Mi / 1 cpu req, 4 cpu limit | heap 1024MB; Burstable QoS    |
| Metastore pod request/limit                     | `metastore.yaml`    | 1536Mi / 1 cpu req, 4 cpu limit | heap 1024MB; hosts IRC servlet |
| mr3master pod request/limit                     | `mr3-site.xml`      | 2Gi / 1 vcore  | `mr3.am.resource.{memory.mb,cpu.cores}`; baseline RSS ~1.9Gi (tight by design) |
| ContainerGroup per-worker envelope              | `hive-site.xml`     | 4096MB / 4 vcores | `hive.mr3.all-in-one.containergroup.*` |
| Per-Tez-task memory                             | `hive-site.xml`     | 2048MB         | `hive.mr3.map.task.memory.mb`, `.reduce.task.memory.mb` |
| Per-Tez-task vcores                             | `hive-site.xml`     | 1              | slot density inside a ContainerGroup     |
| Container JVM heap fraction of memory.mb        | `hive-site.xml`     | 0.7            | `hive.mr3.container.max.java.heap.fraction` |
| Worker count (initial / cap)                    | `mr3-site.xml`      | 2 / 8 GB total | `mr3.enable.auto.scaling=true`, `mr3.auto.scale.out.num.initial.containers=2`, `mr3.k8s.worker.total.max.memory.gb=8`, `mr3.k8s.worker.total.max.cpu.cores=8` |
| Tez sort buffer                                 | `tez-site.xml`      | 384MB          | `tez.runtime.io.sort.mb` — must be < (task.memory.mb × heap.fraction) |
| Tez unordered output buffer                     | `tez-site.xml`      | 128MB          | `tez.runtime.unordered.output.buffer.size-mb` |
| Pipelining threshold                            | `hive-site.xml`     | 80             | `hive.mr3.am.task.concurrent.run.threshold.percent` — reducer starts at 80% map completion |

Derived:
- Concurrent Tez tasks per worker = `containergroup.memory.mb / map.task.memory.mb` = **2**
- Concurrent tasks total = 2 workers × 2 = **4**
- Always-on pod memory = HS2 1.25Gi + HMS 1.5Gi + master 2Gi = **4.75Gi**
- Peak with 2 workers active = 4.75Gi + 8Gi = **12.75Gi / 16Gi physical** (≈3.25Gi host headroom)

Why this config: tuned 2026-05-27 for MV builds against the 40 B-row partitions table. Earlier attempts with `task.memory.mb=1024` (Config B, 4 concurrent tasks/worker) OOMed at task startup because the per-task floor (Tez sort buffer 384 + parquet decode + vectorized batches) exceeded the 1024 MB slot. Workers under Config A still peak at ~4095 Mi (cgroup ceiling) regardless of aggregate size — the floor is the dominant memory consumer, not the GROUP BY hash. See memory `[[feedback-per-task-memory-floor]]`.

Why CPU limit > request: pods are mostly idle. Setting limit > request makes the pod Burstable QoS so query planning can use spare cycles on the 24-logical-core node. Memory request = limit keeps memory accounting strict (no overcommit).

## Known gotchas

1. **`kubectl rollout restart` does not re-read YAML.** You must
   `kubectl apply -f yaml/*.yaml` first if pod specs changed.
2. **mr3master caches master-pod sizes at its startup.** After shrinking
   HS2 or metastore you must delete the mr3master pod so it re-samples,
   otherwise `Tickets=0` persists with `insufficient resource: [0MB, 0]`.
3. **Metastore schema:** `datanucleus.schema.autoCreateAll=false` +
   `schematool -initSchema` on a fresh MySQL DB. `autoCreateAll=true`
   misses txn and COMPACTION_QUEUE tables → `LockException` on query.
4. **Authorization:** empty `hive.metastore.pre.event.listeners`; set
   `hive.security.metastore.authorization.manager` to
   `MetaStoreAuthzAPIAuthorizerEmbedOnly` (permissive).
5. **Warehouse mount:** ContainerWorker pods need a hostPath mount for
   the warehouse dir. Set `mr3.k8s.pod.worker.additional.hostpaths`.
   If you blank `mr3.k8s.pod.worker.hostpaths` you must also uncomment
   `mr3.k8s.pod.worker.emptydirs=/opt/mr3-run/work-local-dir`.
6. **`kubectl port-forward` → HS2 connects via the pod's loopback;** HS2
   binds to the pod IP, so forwards yield "connection refused." Use
   NodePort 31140 from the host for SQL.
7. **MySQL 8** requires `allowPublicKeyRetrieval=true` in the JDBC URL
   for `caching_sha2_password` auth.
8. **pyiceberg 0.11.1 vs HMS 4** — pyiceberg calls the deprecated Thrift
   `get_table`; HMS 4 only exposes `get_table_req`. Patched at runtime
   via `primeparts._patches._patch_hive_metastore_get_table`. Only
   relevant for legacy funbuns-era scripts; new code uses iceberg-cpp
   via IRC and avoids this entirely.
9. **busybox init-container gets GC'd** *(workaround in use, not resolved)*.
   Worker pods use a tiny busybox init container; kubelet has evicted
   it 3+ times during this session.
   - **Symptom**: workers stuck in `Init:ErrImageNeverPull`.
   - **Manual workaround currently in use**: `docker pull busybox`
     then `kubectl delete pod mr3worker-...` to force respawn.
   - **What we know about the cause**:
     - Dockerd's `data-root` is `/home/erpage159/docker` (on the
       118 GB `/home` partition, currently 54 GB free / ~54 % used).
       Not the small `/tmp` (16 GB) partition.
     - Image inventory is small: 10 images totalling ~5.7 GB on disk;
       ~4.5 GB is reclaimable via `docker image prune -f`. The hive
       overlay rebuilds add ~20 MB per rebuild (shared base layers).
       Build cache is 0 (BuildKit disabled for overlay bakes).
     - Heavy data (warehouse parquet, iceberg metadata, mr3 work-dir,
       MySQL) is on hostPath / PVC mounts, **not** in container
       writable layers. Containers contribute < 10 MB total.
     - So image-filesystem disk pressure does *not* explain the GC.
       Earlier `ImageGCFailed: wanted to free 7 GB` events likely
       came from a different filesystem accounting or a transient
       state; current state doesn't reproduce the pressure
       conditions.
   - **Durable fix options (none applied yet)**, ordered by cost:
     1. **Replace busybox init with the hive image** *(recommended)* —
        set
        `mr3.k8s.pod.worker.init.container.image=mr3-hive-4.2.0-local/hive:4.2.0`
        with `command: ["/bin/true"]`. The worker container already
        references this image, so kubelet can never evict its layers
        while a worker pod is alive. Doesn't depend on disk-space
        reasoning at all. One config-rotation cycle.
     2. **Pin via DaemonSet** — a 5 MB `busybox sleep infinity` pod
        keeps kubelet from evicting it. One YAML.
     3. **Move dockerd's data-root to extssd** — `data-root` is
        already on `/home` (118 GB, 54 GB free) which is plenty for
        the current image footprint. Moving to the 445 GB extssd
        partition is overkill for image storage but would be
        appropriate if BuildKit ever gets enabled (cache can balloon).
        Requires dockerd downtime + image re-import.
   - **Maintenance**: regardless of which durable fix is chosen,
     `docker image prune -f` after each overlay rebuild keeps data-root
     small.
10. **MV stub-zombie on failed CREATE.** A failed `CREATE MATERIALIZED
    VIEW ... STORED BY ICEBERG ...` (OOM, DAG NPE, anything past the
    iceberg-handler registration) leaves a registered MV stub with no
    data. Retry CREATE fails with `Table already exists`. Always
    prepend `DROP MATERIALIZED VIEW IF EXISTS ...` when iterating.
11. **`tez.runtime.io.sort.mb` is pre-allocated** at task startup. If
    larger than the per-task heap (`task.memory.mb × heap.fraction`),
    tasks fail at startup with `IllegalArgumentException`. When
    shrinking `task.memory.mb`, shrink sort.mb proportionally
    (rule of thumb: ~25-30 % of per-task heap).

## Catalog architecture (current)

Native (iceberg-cpp) is the canonical commit path for `primeparts.*`:

```
native binary  ──►  IRC (HMS REST servlet :9090)  ──►  HMS (Thrift + MySQL)
   (writes parquet     RegisterTable / commit              metadata_location
    files first)       writes the table row                stamped here
                                                            │
                                                            ▼
                                                   HS2 + MR3 readers
```

When a C++ tool finishes writing parquet and metadata.json, it calls
`Catalog::RegisterTable(ident, metadata_location)` (or `CreateTable +
FastAppend`) against IRC. IRC writes the HMS row with
`table_type=ICEBERG` and `metadata_location=...`.

> **OPEN / corrected:** an earlier version of this doc claimed "Hive
> sees the new state on its next read. No separate sync step." That is
> **verified false** for snapshot-advancing commits: a native IRC /
> on-disk commit does not by itself make the new snapshot visible to the
> Hive engine for read or MV refresh — a separate HMS sync that sets
> `metadata_location` is required. The register-once case (first
> `RegisterTable` of a fresh table) *may* be visible without the extra
> step; the snapshot-advance case is not. Mechanism (beeline vs raw
> Thrift) and the exact boundary are under investigation — see "HMS sync
> for native-committed snapshots (open)" below and `HANDOFF.md`.

Working pattern, with the gotchas we hit during the 2026-05-26 cutover,
is implemented in `native/src/drop_bucket_cols_main.cc::PublishTable`:
- If on-disk `metadata.json` already exists and the catalog knows the
  table at the same location → no-op.
- Otherwise: `DropTable(purge=false)` (tolerates not-found) then
  `RegisterTable(metadata_location)` to point the catalog at the
  on-disk metadata. Preserves any existing parquet.

### Legacy funbuns / SqlCatalog (retired)

The pre-cutover stack used a SqlCatalog at `/iceberg/warehouse/catalog.db`
plus a `scripts/sync_hms.py` bridge that pushed `metadata_location`
into HMS after each pyiceberg-driven ingest. That setup remains in tree
for two reasons:

1. The legacy `funbuns.primes` / `funbuns.decompositions` tables still
   exist on disk and are still pointed at by HMS rows. Cutover task #15
   will archive these.
2. `scripts/sync_hms.py` is referenced by older docs and demonstrates
   the pre-IRC bridge pattern. Don't extend it; new code uses IRC.

The `scripts/sync_hms.py` Thrift bridge is dead code for the SqlCatalog
path (it targets the retired sqlite catalog), **but its mechanism is not
obsolete**: the `get_table` → swap `metadata_location` /
`previous_metadata_location` → `alter_table_with_environment_context`
sequence is exactly the HMS sync the native stack still needs for
snapshot-advancing commits (see "HMS sync for native-committed snapshots
(open)" below). Don't revive `sync_hms.py` itself; port its three-line
core if we land on the raw-Thrift route.

## Next steps / open questions

### MV builds (in progress)

See [`MV_list.md`](MV_list.md) for the running inventory of
materialized views, what's built vs planned, what was rejected and why,
and the session-level settings used for each build.

The MV-as-index philosophy: these are precomputed indexes for the
native covering-sieve / modular-filter consumers
(`markdown/math/modular_filter_more_ideas.md`), not analytical
end-products. See memory `[[feedback-mv-as-index-for-native]]`.

### Resource tuning — known headroom

Config A (above) is the validated baseline. Push targets that remain
unmeasured:

1. **CPU saturation** — `containergroup.vcores: 4 → 12 or 20` (k8s CPU
   limits are cgroup shares, not pins; 24 logical cores available,
   current requests are ~17% of the machine). Cheap.
2. **Heap fraction** — `hive.mr3.container.max.java.heap.fraction:
   0.7 → 0.8`. Gives ~20% more heap in the same container envelope.
   Risk: parquet uses off-heap DirectByteBuffers; squeeze may cause
   off-heap pressure.
3. **Larger worker pods** — `containergroup.memory.mb: 4096 → 8192`
   (1 worker × 8 GB) for cardinality-hardest aggregates that don't fit
   in Config A. Trades parallelism for capacity. Useful for the
   deferred `q_k_freq_top`-style work if Hive is the right tool.
4. **Split tuning** — `tez.grouping.min-size` / `max-size` to change
   #tasks per DAG. For LIMIT queries, current 257-mapper DAG is
   overkill; bigger min-size could amortize per-task overhead.

Empirical cadence:
```
# during a real scan query
watch -n2 "kubectl top pod -n hivemr3; echo ---; \
  kubectl -n hivemr3 logs -l mr3-pod-role=master-role --tail=3 \
  | grep -E 'ContainerViewGroup|Tickets'"

# JVM-level heap/GC for a worker (workers are ephemeral; grab a name
# while a DAG is running)
WK=$(kubectl -n hivemr3 get pod -l mr3-container-worker=true -o name | head -1)
kubectl -n hivemr3 exec "$WK" -- bash -c 'jstat -gc $(pgrep java) 1000 30'
```

### Polars vs Hive

Polars over iceberg (`pl.scan_iceberg`) is faster for one-off
exploratory scans on `primeparts.*`. Hive's role is reserved for
queries that need transactional semantics, MV refresh, or multi-table
joins with bucket-aligned shuffle elimination. For any ad hoc count
or filter, prefer polars.

### MV REBUILD semantics (open)

Hive's incremental MV refresh expects a transactional source. We
commit `primeparts.*` natively via IRC. Whether `ALTER MATERIALIZED
VIEW ... REBUILD` picks up snapshots committed outside Hive is
**untested**. Resolve before binding any MV to an automated refresh
loop. The current MVs are one-shot snapshot views — they don't
auto-update when source tables advance.

### HMS sync for native-committed snapshots (open)

A native IRC / on-disk commit is **verified insufficient** on its own to
make a snapshot-advancing change visible to the Hive engine (read / MV
refresh). A separate HMS sync that updates the table's `metadata_location`
(+ `previous_metadata_location`) is required. Two candidate mechanisms,
undecided:

- **beeline (leading):** in-container `ALTER TABLE primeparts.<tbl> SET
  TBLPROPERTIES('metadata_location'=…)` via `run-beeline.sh` against HS2.
  Routes through `HiveIcebergStorageHandler`, so if honored, visibility is
  guaranteed and no `get_table_req` shim is needed.
- **raw Thrift (proven elsewhere):** the `sync_hms.py` core — `get_table`
  → swap params → `alter_table_with_environment_context`. Bypasses the
  handler; needs the HMS-4 `get_table_req` patch (`primeparts._patches`).
  A Rust port on `iceberg-catalog-hms` is extend-and-verify (its
  `update_table`/`register_table` are stubs).

> **OPEN — probe not yet run:** does the beeline `SET TBLPROPERTIES`
> path actually make Hive adopt an externally-committed snapshot, or does
> the storage handler reject/reinterpret a manual pointer swap?
> `sync_hms.py` chose raw Thrift precisely *to avoid* that handler logic,
> which hints the SQL path may differ. One throwaway test (register →
> advance metadata.json → beeline ALTER → fresh SELECT) decides
> beeline-vs-Thrift. Also unresolved: whether the initial `RegisterTable`
> already yields an engine-readable table, or that too needs
> handler-aware DDL.

### Bucket-map-join verification (open)

`primes ⨝ partitions ON p` is layout-compatible with a zero-shuffle
bucket-map-join (both tables identity-partitioned on
`(p_bucket_version, p_bucket)`). Confirm via `EXPLAIN PLAN` before
building any join-heavy MV. Relevant settings:
`hive.optimize.bucketmapjoin=true`, `hive.optimize.bucketmapjoin.sortedmerge=true`.

### GPU polars engine

`cudf-polars-cu12` backend for `.collect(engine="gpu")`. Streams through
row groups; VRAM is not a bound for aggregations with small outputs.
Unsupported ops silently fall back to CPU per-node; run with
`POLARS_VERBOSE=1` to see the plan. Candidate targets: ad hoc
q-histogram exploration, sample-based covering-system sweeps.

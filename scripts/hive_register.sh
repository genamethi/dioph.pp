#!/usr/bin/env bash
#
# hive_register.sh — register / sync Iceberg tables into the Hive catalog using
# a REMOTE beeline (client -> HiveServer2 over JDBC). Permanent counterpart to
# the retired, raw-Thrift scripts/sync_hms.py.
#
# Why remote beeline (not kubectl-exec-into-pod): the MR3 docs treat beeline as
# an ordinary remote client ("run Beeline or any client program to connect to
# HiveServer2 at a given address"). Routing through HS2 means the SQL is
# compiled by the same HiveIcebergStorageHandler the engine reads with, so if a
# catalog mutation is honored at all, engine visibility follows by construction.
#
# Working invocation (verified end-to-end 2026-05-31 against the live cluster;
# each fact below was an actual failure mode hit while bringing this up):
#   * Java 21 is REQUIRED. The host default is Java 25, under which the bundled
#     jline's FFM terminal provider throws UnsupportedClassVersionError and
#     beeline can't start. The cluster itself runs zulu21; we match it.
#   * -Dorg.jline.terminal.provider=dumb is REQUIRED. Non-interactively jline
#     tries FFM (needs --enable-preview) then jna (needs jna.jar), fails both,
#     and aborts with "Unable to create a terminal" unless forced to dumb.
#   * HTTP transport on :10001 (transportMode=http;httpPath=cliservice). HS2 is
#     transport.mode=all; the binary :9852 SASL path was not used. auth=NONE so
#     -n/-p are nominal.
#   * Classpath = dist lib/* (beeline + its deps) + hive-jdbc-*-standalone.jar
#     (the JDBC driver) + a full hadoop-common jar. The standalone jar alone
#     lacks org.apache.hadoop.util.concurrent.HadoopExecutors (used by beeline's
#     shutdown hook), so a real hadoop-common is appended.
#
# Native tooling commits Iceberg snapshots via IRC (iceberg-cpp -> HMS REST
# servlet). An IRC commit writes the HMS table row, but a *snapshot-advancing*
# commit is not by itself adopted by the running Hive engine (HANDOFF.md
# "HMS sync for native-committed snapshots"). `sync` issues the HMS-side pointer
# update that makes the new snapshot visible to the engine.
#
# Operations:
#   exec     <SQL>                      run arbitrary SQL via remote beeline
#   describe <db.table>                 DESCRIBE FORMATTED (metadata_location etc.)
#   sync     <db.table> <metadata.json> point an EXISTING registered table at a
#                                       new metadata.json (snapshot advance)
#   register <db.table> <metadata.json> create the Hive catalog entry for an
#                                       on-disk Iceberg table not yet in HMS
#
# Usage:
#   scripts/hive_register.sh exec "SELECT COUNT(*) FROM primeparts.primes"
#   scripts/hive_register.sh sync primeparts.primes \
#       file:/media/extssd/.../primes/metadata/00008-....metadata.json
#
# Env overrides:
#   HS2_HOST   HiveServer2 host             (default: 192.168.1.202)
#   HS2_HTTP_PORT  HiveServer2 http port    (default: 10001)
#   HS2_PATH   http path                    (default: cliservice)
#   HS2_USER / HS2_PASS  beeline -n / -p    (default: hive / hive)  [auth=NONE]
#   JAVA21     path to a Java 21 binary     (default: /usr/lib/jvm/java-21-openjdk-amd64/bin/java)
#   HIVE_DIST  unpacked Hive 4.2 dist root  (default: locally-built apache-hive-4.2.0-bin)
#   HADOOP_COMMON_JAR  full hadoop-common   (default: ~/.m2 hadoop-common-3.4.1.jar)
#
# NOTE: whether `ALTER TABLE ... SET TBLPROPERTIES('metadata_location'=...)` is
# actually honored by Hive 4.2/MR3 for an externally-advanced snapshot is the
# open probe (HANDOFF.md). This script is the mechanism; validate `sync` against
# a throwaway table before wiring it into any automated refresh.

set -uo pipefail

HS2_HOST="${HS2_HOST:-192.168.1.202}"
HS2_HTTP_PORT="${HS2_HTTP_PORT:-10001}"
HS2_PATH="${HS2_PATH:-cliservice}"
HS2_USER="${HS2_USER:-hive}"
HS2_PASS="${HS2_PASS:-hive}"
JAVA21="${JAVA21:-/usr/lib/jvm/java-21-openjdk-amd64/bin/java}"
HIVE_DIST="${HIVE_DIST:-/home/erpage159/fluid/byo/repos/hive-mr3/packaging/target/apache-hive-4.2.0-bin/apache-hive-4.2.0-bin}"
HADOOP_COMMON_JAR="${HADOOP_COMMON_JAR:-/home/erpage159/.m2/repository/org/apache/hadoop/hadoop-common/3.4.1/hadoop-common-3.4.1.jar}"
BEELINE_MAIN="org.apache.hive.beeline.BeeLine"

die() { echo "hive_register: $*" >&2; exit 1; }

usage() {
  sed -n '2,55p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

pick_jar() {
  local m
  for m in $1; do [ -e "$m" ] && { echo "$m"; return; }; done
  die "no jar matching '$1'"
}

build_classpath() {
  local L="$HIVE_DIST/lib" J="$HIVE_DIST/jdbc"
  [ -d "$L" ] || die "HIVE_DIST lib not found: $L (set HIVE_DIST)"
  [ -e "$HADOOP_COMMON_JAR" ] || die "hadoop-common jar not found: $HADOOP_COMMON_JAR (set HADOOP_COMMON_JAR)"
  printf '%s/*:%s:%s' "$L" "$(pick_jar "$J/hive-jdbc-*-standalone.jar")" "$HADOOP_COMMON_JAR"
}

# Run SQL; echo cleaned beeline output; return nonzero on a real error.
run_sql() {
  local sql="$1" cp out
  [ -x "$JAVA21" ] || die "Java 21 not found at $JAVA21 (set JAVA21)"
  cp="$(build_classpath)" || exit 1
  local url="jdbc:hive2://$HS2_HOST:$HS2_HTTP_PORT/;transportMode=http;httpPath=$HS2_PATH"
  echo ">>> [$url] $sql" >&2
  out="$("$JAVA21" -Dorg.jline.terminal.provider=dumb -cp "$cp" "$BEELINE_MAIN" \
          -u "$url" -n "$HS2_USER" -p "$HS2_PASS" \
          --silent=true --showHeader=false --outputformat=tsv2 -e "$sql" 2>&1 \
        | grep -avE '^(SLF4J|Picked up |log4j:|WARNING: |[A-Z][a-z]{2} [0-9]+, [0-9]{4})')"
  printf '%s\n' "$out"
  if printf '%s' "$out" | grep -qiE 'Error:|Exception|Could not open|Read timed out|FAILED|No current connection'; then
    return 1
  fi
  return 0
}

require_loc() {
  # Iceberg metadata_location must be a URI (scheme:/...) ending in .metadata.json.
  case "$1" in
    *:/*.metadata.json) : ;;
    *.metadata.json) die "metadata_location must be a URI (e.g. file:/...), got: $1" ;;
    *) die "metadata_location must end in .metadata.json, got: $1" ;;
  esac
}

cmd="${1:-}"; shift || true
case "$cmd" in
  exec)     [ $# -ge 1 ] || usage 1; run_sql "$*" ;;
  describe) [ $# -eq 1 ] || usage 1; run_sql "DESCRIBE FORMATTED $1" ;;
  sync)
    [ $# -eq 2 ] || usage 1; require_loc "$2"
    run_sql "ALTER TABLE $1 SET TBLPROPERTIES ('metadata_location'='$2')"
    ;;
  register)
    [ $# -eq 2 ] || usage 1; require_loc "$2"
    run_sql "CREATE TABLE $1 STORED BY ICEBERG STORED AS PARQUET TBLPROPERTIES ('metadata_location'='$2')"
    ;;
  ""|-h|--help|help) usage 0 ;;
  *) die "unknown command '$cmd' (try: exec|sync|register|describe)";;
esac

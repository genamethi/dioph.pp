#!/usr/bin/env bash
# Scale the Hive + MR3 + HMS stack in the hivemr3 namespace to zero replicas
# and evict any lingering ContainerWorker pods.
#
# Use before a heavy core.py run to reclaim the node's memory + CPU.
# Restore with: scripts/kube-up.sh
#
# For a harder reset, run `sudo systemctl stop k3s` instead — that drops the
# whole k3s cgroup, not just this namespace.

set -euo pipefail

NS=hivemr3

echo "Scaling HiveServer2, Metastore, and all mr3master-* deployments to 0..."
kubectl -n "$NS" scale deploy/hivemr3-hiveserver2 --replicas=0 >/dev/null || true
# The mr3master deployment carries no labels (the master-role label is on the
# pod template only), so match by name prefix instead.
for d in $(kubectl -n "$NS" get deploy -o name 2>/dev/null | grep '^deployment.apps/mr3master-' || true); do
  kubectl -n "$NS" scale "$d" --replicas=0 >/dev/null || true
done
kubectl -n "$NS" scale statefulset/hivemr3-metastore --replicas=0 >/dev/null || true

echo "Force-deleting any ContainerWorker pods..."
kubectl -n "$NS" delete pod -l mr3-container-worker=true --force --grace-period=0 2>/dev/null || true
# Also sweep up completed/leftover worker pods (the mr3-container-worker label
# is only set on Running workers; Completed pods can linger without it).
kubectl -n "$NS" delete pod --field-selector=status.phase=Succeeded --force --grace-period=0 2>/dev/null || true

echo "Current state:"
kubectl -n "$NS" get pods 2>/dev/null || echo "  (namespace empty)"

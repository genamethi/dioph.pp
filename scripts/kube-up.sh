#!/usr/bin/env bash
# Bring the Hive + MR3 + HMS stack back up after a kube-down.
#
# Scales Metastore + HiveServer2 back to 1 replica. HS2 will spawn a fresh
# mr3master deployment from client-am-config on first DAG submission, so we
# don't need to scale mr3master-* manually.

set -euo pipefail

NS=hivemr3

if ! kubectl version --client=false --request-timeout=5s >/dev/null 2>&1; then
  echo "Kubernetes API is not reachable via the current kubeconfig." >&2
  if systemctl is-active --quiet k3s 2>/dev/null; then
    echo "k3s is active, but kubectl cannot reach 127.0.0.1:6443." >&2
    echo "Check: sudo journalctl -u k3s -n 120 --no-pager" >&2
  else
    echo "k3s is inactive. Start it first:" >&2
    echo "  sudo systemctl start k3s" >&2
    echo "Then rerun: scripts/kube-up.sh" >&2
  fi
  exit 2
fi

# If the node was cordoned (kubectl cordon / drain), un-cordon first so the
# scaled-up pods can actually schedule.
for n in $(kubectl get nodes -o jsonpath='{.items[?(@.spec.unschedulable==true)].metadata.name}'); do
  echo "Un-cordoning node $n..."
  kubectl uncordon "$n" >/dev/null
done

echo "Scaling Metastore and HiveServer2 back up..."
kubectl -n "$NS" scale statefulset/hivemr3-metastore --replicas=1 >/dev/null
kubectl -n "$NS" scale deploy/hivemr3-hiveserver2 --replicas=1 >/dev/null

# If an mr3master deployment still exists from a prior run, bring it back;
# otherwise the next DAG submission will create a new one. The deployment
# carries no labels, so match by name prefix (see kube-down.sh for context).
for d in $(kubectl -n "$NS" get deploy -o name 2>/dev/null | grep '^deployment.apps/mr3master-' || true); do
  kubectl -n "$NS" scale "$d" --replicas=1 >/dev/null || true
done

echo "Waiting for Metastore and HiveServer2 to be Ready..."
kubectl -n "$NS" wait --for=condition=ready pod -l hivemr3_app=metastore --timeout=180s
kubectl -n "$NS" wait --for=condition=ready pod -l hivemr3_app=hiveserver2 --timeout=180s

kubectl -n "$NS" get pods

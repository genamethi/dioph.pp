#!/usr/bin/env bash
# Refresh the cached Iceberg references in this directory from upstream.
# See README.md for what each file is. Prints a short change summary.
set -euo pipefail
cd "$(dirname "$0")"

SPEC_URL="https://raw.githubusercontent.com/apache/iceberg/refs/heads/main/open-api/rest-catalog-open-api.yaml"
SPEC=rest-catalog-open-api.yaml
ISSUE_JSON=iceberg-cpp-issue-273.json
ISSUE_MD=iceberg-cpp-issue-273.md

echo ">> OpenAPI spec ($SPEC_URL)"
curl -fsSL -o "$SPEC.new" "$SPEC_URL"
if [ -f "$SPEC" ] && cmp -s "$SPEC" "$SPEC.new"; then
  echo "   unchanged"; rm -f "$SPEC.new"
else
  [ -f "$SPEC" ] && echo "   CHANGED ($(diff <(wc -l <"$SPEC") <(wc -l <"$SPEC.new") >/dev/null 2>&1 && echo 'same line count' || echo 'line count differs'))"
  mv "$SPEC.new" "$SPEC"
fi

echo ">> iceberg-cpp issue #273 (conformance tracker)"
old_upd=$(python3 -c "import json,sys; print(json.load(open('$ISSUE_JSON')).get('updatedAt',''))" 2>/dev/null || echo "")
gh issue view 273 --repo apache/iceberg-cpp \
  --json title,state,url,updatedAt,body,comments > "$ISSUE_JSON"
python3 - "$ISSUE_JSON" "$ISSUE_MD" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
open(sys.argv[2], "w").write(
    f"# iceberg-cpp #273 — {d['title']}\n\nstate: {d['state']}  |  url: {d['url']}"
    f"  |  upstream updatedAt: {d.get('updatedAt','?')}\n\n{d['body']}"
)
new_upd = d.get("updatedAt", "")
print(f"   updatedAt: {new_upd}")
PY
[ -n "$old_upd" ] && [ "$old_upd" != "$(python3 -c "import json;print(json.load(open('$ISSUE_JSON')).get('updatedAt',''))")" ] \
  && echo "   (changed since last cache: was $old_upd)" || true

echo ">> done. Review with: git diff native/vendor/iceberg-refs/"

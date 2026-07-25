import argparse, json, os, sys, time, resource
from collections import defaultdict
import multiprocessing as mp

from sage.all import prime_range, Integer, PolynomialRing, ZZ

R = PolynomialRing(ZZ, "x")
X = R.gen()

_HE = [R(1), X]
def he(n):
    while len(_HE) <= n:
        k = len(_HE) - 1
        _HE.append(X * _HE[k] - k * _HE[k - 1])
    return _HE[n]

def to_hermite(P):
    out = {}
    P = R(P)
    while P != 0:
        d = P.degree()
        c = int(P[d])
        out[d] = c
        P = P - c * he(d)
    return out

SEED = (0, ())

def word_push(word, n, c):
    a0, segs = word
    if n == 1:
        if segs:
            segs = segs[:-1] + ((segs[-1][0], segs[-1][1] + c),)
        else:
            a0 = a0 + c
    else:
        segs = segs + ((n, c),)
    return (a0, segs)

def word_poly(word):
    a0, segs = word
    p = X + a0
    for (n, c) in segs:
        p = p ** n + c
    return p

def word_features(word):
    a0, segs = word
    p = word_poly(word)
    hv = to_hermite(p)
    return dict(
        degree=int(p.degree()),
        skeleton=json.dumps([n for (n, _) in segs]),
        translations=json.dumps([a0] + [c for (_, c) in segs]),
        hermite=json.dumps({str(k): v for k, v in sorted(hv.items())}, separators=(",", ":")),
        symbolic=str(p),
    )

def build_graph(B):
    rank = {}
    for i, p in enumerate(prime_range(2, B), start=1):
        rank[int(p)] = i
    primes = sorted(p for p in rank if p >= 3)
    children = defaultdict(list)
    parents = defaultdict(list)
    for p in primes:
        m = 1
        while (1 << m) < p:
            r = p - (1 << m)
            if r >= 3 and (r & 1):
                q, n = Integer(r).is_prime_power(get_data=True)
                if n >= 1:
                    children[int(q)].append((m, int(n), p))
                    parents[p].append((m, int(n), int(q)))
            m += 1
    sinks = [p for p in primes if not children.get(p)]
    return rank, primes, children, parents, sinks

PARENTS = None

def paths_down(v, memo):
    r = memo.get(v)
    if r is not None:
        return r
    ps = PARENTS.get(v)
    if not ps:
        res = {(v, SEED): 1}
    else:
        acc = defaultdict(int)
        for (m, n, u) in ps:
            for (root, word), cnt in paths_down(u, memo).items():
                acc[(root, word_push(word, n, 1 << m))] += cnt
        res = dict(acc)
    memo[v] = res
    return res

def worker(sink_batch):
    sys.setrecursionlimit(1000000)
    memo = {}
    out = []
    for s in sink_batch:
        for (root, word), cnt in paths_down(s, memo).items():
            if word != SEED:
                out.append((root, s, word, cnt))
    return out

def brute_maximal(children, parents, primes, sinks):
    roots = [p for p in primes if not parents.get(p)]
    sinkset = set(sinks)
    total = 0
    per = defaultdict(int)
    for r in roots:
        stack = [(r, r)]
        while stack:
            root, node = stack.pop()
            if node in sinkset and node != root:
                total += 1
                per[(root, node)] += 1
            for (m, n, child) in children.get(node, ()):
                stack.append((root, child))
    return total, per

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bound", type=int, default=1000)
    ap.add_argument("--workers", type=int, default=12)
    ap.add_argument("--out", default="scripts/chain_dag_out")
    ap.add_argument("--validate", action="store_true")
    args = ap.parse_args()

    global PARENTS
    import polars as pl
    t0 = time.time()
    rank, primes, children, parents, sinks = build_graph(args.bound)
    PARENTS = dict(parents)
    nedges = sum(len(v) for v in children.values())
    t_graph = time.time() - t0
    print(f"[graph] B={args.bound} primes={len(primes)} edges={nedges} "
          f"sinks={len(sinks)} in {t_graph:.2f}s", flush=True)

    ctx = mp.get_context("fork")
    t1 = time.time()
    batches = [sinks[i::args.workers] for i in range(args.workers)]
    raw = []
    with ctx.Pool(args.workers) as pool:
        for part in pool.imap_unordered(worker, batches):
            raw.extend(part)
    t_dp = time.time() - t1

    words = {}
    for (root, s, word, cnt) in raw:
        if word not in words:
            words[word] = len(words)
    inv = [None] * len(words)
    for w, i in words.items():
        inv[i] = w
    t2 = time.time()
    with ctx.Pool(args.workers) as pool:
        feats = pool.map(word_features, inv, chunksize=256)
    t_feat = time.time() - t2

    outdir = os.path.join(args.out, f"B{args.bound}")
    os.makedirs(outdir, exist_ok=True)

    pl.DataFrame([(p, rk) for p, rk in rank.items() if p >= 3],
                 schema=["prime", "id"], orient="row").write_parquet(
        os.path.join(outdir, "nodes.parquet"))

    edge_rows = [(rank[p], rank[q], m, n) for q, lst in children.items() for (m, n, p) in lst]
    pl.DataFrame(edge_rows, schema=["child_id", "parent_id", "m", "n"],
                 orient="row").write_parquet(os.path.join(outdir, "dag_edges.parquet"))

    hv_rows = [(i, f["degree"], f["skeleton"], f["translations"], f["hermite"], f["symbolic"])
               for i, f in enumerate(feats)]
    pl.DataFrame(hv_rows, schema=["hevec_id", "degree", "skeleton", "translations",
                                  "hermite", "symbolic"], orient="row").write_parquet(
        os.path.join(outdir, "hevecs.parquet"))

    cls_rows = [(rank[root], rank[s], words[word], cnt) for (root, s, word, cnt) in raw]
    pl.DataFrame(cls_rows, schema=["root_id", "sink_id", "hevec_id", "mult"],
                 orient="row").write_parquet(os.path.join(outdir, "maximal_classes.parquet"))

    nchains = sum(cnt for *_, cnt in raw)
    summary = dict(bound=args.bound, primes=len(primes), edges=nedges, sinks=len(sinks),
                   maximal_classes=len(raw), distinct_words=len(words), maximal_chains=nchains,
                   max_degree=max((f["degree"] for f in feats), default=0),
                   t_graph=round(t_graph, 2), t_dp=round(t_dp, 2), t_feat=round(t_feat, 2),
                   peak_rss_mb=round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024, 1),
                   peak_child_rss_mb=round(resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss / 1024, 1))

    if args.validate:
        bt, bper = brute_maximal(children, parents, primes, sinks)
        dp_per = defaultdict(int)
        for (root, s, word, cnt) in raw:
            dp_per[(root, s)] += cnt
        ok = (bt == nchains) and all(bper[k] == dp_per.get(k, 0) for k in bper)
        summary["validate_brute_maximal_chains"] = bt
        summary["validate_ok"] = bool(ok)

    with open(os.path.join(outdir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=2)
    print("[summary] " + json.dumps(summary), flush=True)

if __name__ == "__main__":
    main()

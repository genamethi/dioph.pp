# Regeneration of the deleted tests/grade_covering.cc: grade cov_emit's two
# covers (divisibility vs order) for k=0 coverage and k>0 bleed.
# pixi run python scripts/coverings/grade_covering_regen.py
from sympy import primerange, factorint, n_order
B = 2_000_000
spf = list(range(B+1))
for i in range(2, int(B**0.5)+1):
    if spf[i]==i:
        for j in range(i*i, B+1, i):
            if spf[j]==j: spf[j]=i
def factors(n):
    s=set()
    while n>1:
        q=spf[n]; s.add(q)
        while n%q==0: n//=q
    return s
cert_bound = int(B**0.5); order_cap = 63
order_ok = {}
for l in primerange(3, cert_bound+1):
    try: order_ok[l] = (n_order(2, l) <= order_cap)
    except: order_ok[l] = False
def positions(p): return range(1, p.bit_length())
def kcount(p):
    c=0
    for m in positions(p):
        x=p-(1<<m)
        if x>=2 and len(factorint(x))==1: c+=1
    return c
def full_primary(p):
    for m in positions(p):
        x=p-(1<<m)
        if x>=2 and spf[x] > cert_bound: return False
    return True
def full_order(p):
    for m in positions(p):
        x=p-(1<<m)
        if x>=2 and not any(order_ok.get(q,False) for q in factors(x)): return False
    return True
k0=k0p=k0o=kp=kpp=kpo=0
for p in primerange(3, B+1):
    k=kcount(p); fp=full_primary(p); fo=full_order(p)
    if k==0: k0+=1; k0p+=fp; k0o+=fo
    else: kp+=1; kpp+=fp; kpo+=fo
print(f"k=0 {k0}  k>0 {kp}")
print(f"primary   k0cov {k0p/k0:.4f}  k>0bleed {kpp/kp:.5f}")
print(f"bleed_min k0cov {k0o/k0:.4f}  k>0bleed {kpo/kp:.5f}")

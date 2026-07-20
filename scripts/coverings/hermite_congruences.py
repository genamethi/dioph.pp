# He_p = x^p mod p, and He_3 factorization mod l <-> (3/l). pixi run python <this>
from sympy import symbols, legendre_symbol, primerange, hermite_prob, Poly, n_order
x=symbols('x')
def He(n): return Poly(hermite_prob(n,x),x)
def roots_modl(Hn,l):
    cs=[int(c) for c in Hn.all_coeffs()]; d=Hn.degree()
    return [a for a in range(l) if sum(c*pow(a,d-i,l) for i,c in enumerate(cs))%l==0]
data=[(l, len(roots_modl(He(3),l))>1, int(legendre_symbol(3,l))==1) for l in primerange(5,500)]
print("He_3 nonzero roots <-> (3/l)=1 :", sum(1 for _,h,q in data if h==q)/len(data)*100, "%")
for l in [3,5,7,11,13]:
    cs=[int(c)%l for c in He(l).all_coeffs()]
    print(f"He_{l} == x^{l} mod {l}? {cs[0]==1 and all(c==0 for c in cs[1:])}")

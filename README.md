## A mostly C/C++ implementation exploring the equation p = 2^m + q^n for p, q prime, with m, n positive integers.

### This is an open ended project, but I have some paths I'd like to explore.

* It's a bit of a dead end, maybe:

For a given p, we have k(p) (usually just written as k) solutions fitting the restricted partition (length two, prime powers).
For k = 0, we consider that p - 2^m_i = s for some composition number. Then we consider that the differences between p - 2^m_i and p - 2^m_j are going to be Mersenne factors. That is, they're prime factors of numbers of the form 2^m - 1. Since we have a bound on m from log2(p), we can easily keep a table of all these factors. Considering each position as an opportunity for s | t where t is a prime factor of M_i, for some Mersenne number, we can ask questions about p mod C, and potentially develop somethign along the lines of an Erdos-SElfridge covering system. 

This also carries over to k > 0. For instance, with k = 1 we have one prime power solution 2^m + q^n, then if q^n' in p - 2^m' = q^n'*s, q^n' ... I'll pick this up later


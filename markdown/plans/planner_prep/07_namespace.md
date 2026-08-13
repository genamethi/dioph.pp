# 07_namespace

status: done (merged 0bb87bd)

`iceberg::Namespace` formed at composition roots (`catalog::ResolveNamespace`: explicit → `PRIMEPARTS_NAMESPACE` env → "primeparts") and threaded as an argument everywhere; namespace literals deleted. Distinction kept: `kCatalogName` ("primeparts", pp_iceberg_rest.h) is the SqlCatalog/LMDB catalog name, not a namespace. Multi-level ns joins with %1F in REST paths per IRC.

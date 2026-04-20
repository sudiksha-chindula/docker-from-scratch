#ifndef CACHE_H
#define CACHE_H

/* Returns malloc'd layer digest (hex, no sha256: prefix) on hit, else NULL. */
char *cache_lookup(const char *key);

/* Stores key -> layer digest mapping in ~/.docksmith/cache/. Returns 0 on success. */
int cache_store(const char *key, const char *layer_digest);

#endif /* CACHE_H */

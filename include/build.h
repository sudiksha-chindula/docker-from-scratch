#ifndef BUILD_H
#define BUILD_H

// Parses Docksmithfile, manages cache, and executes build steps[cite: 8, 17, 88].
int execute_build(const char *tag, const char *context_dir, int use_cache);

#endif // BUILD_H

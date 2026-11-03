// Replace only /proc maps access on hosts without /proc (not manager logic).
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <string>
#include <vector>
#include "native_hook_manager.h"
#include "art_elf_resolver.h"
#include "root_helper_client.h"
#include <dobby.h>

FILE *native_test_fopen(const char *, const char *);
#define fopen native_test_fopen
#include "native_hook_manager.cpp"
#undef fopen

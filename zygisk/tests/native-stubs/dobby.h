#pragma once

#include <cstddef>

using dobby_hook_prepare_t = void (*)(void *, size_t, void *);
extern "C" int DobbyHook(void *, void *, void **);
extern "C" int DobbyHookWithPrepare(void *, void *, dobby_hook_prepare_t, void *);
extern "C" int DobbyDestroy(void *);
extern "C" int DobbyIsHooked(void *);

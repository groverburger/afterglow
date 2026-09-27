// Single translation unit holding the sokol + sokol_imgui implementations.
#define SOKOL_IMPL
#if defined(__APPLE__)
#define SOKOL_METAL
#elif defined(_WIN32)
#define SOKOL_D3D11
#else
#define SOKOL_GLCORE
#endif
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "sokol_audio.h"
#include "sokol_time.h"
#include "imgui.h"
#include "sokol_imgui.h"

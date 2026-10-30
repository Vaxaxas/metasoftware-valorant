#pragma once
#include <cstdint>

typedef void* NDIlib_recv_instance_t;
typedef void* NDIlib_find_instance_t;

inline void NDIlib_recv_destroy(void*) {}

namespace g_NDI {
    NDIlib_recv_instance_t connect_to_default_device();
    void cleanup_device();
}

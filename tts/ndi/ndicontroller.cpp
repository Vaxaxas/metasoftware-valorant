#include "ndicontroller.h"
#include <iostream>

NDIlib_recv_instance_t g_NDI::connect_to_default_device()
{
    if (!NDIlib_initialize()) {
        return nullptr;
    }

    NDIlib_find_create_t find_create_desc = {};
    find_create_desc.show_local_sources = true;
    NDIlib_find_instance_t pNDI_find = NDIlib_find_create_v2(&find_create_desc);
    if (!pNDI_find) {
        return nullptr;
    }

    NDIlib_find_wait_for_sources(pNDI_find, 5000);

    uint32_t sources_cnt = 0;
    const NDIlib_source_t* p_sources = NDIlib_find_get_current_sources(pNDI_find, &sources_cnt);

    if (sources_cnt == 0) {
        NDIlib_find_destroy(pNDI_find);
        return nullptr;
    }

    NDIlib_recv_create_v3_t recv_desc = {};
    recv_desc.source_to_connect_to = p_sources[0];
    recv_desc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
    recv_desc.bandwidth = NDIlib_recv_bandwidth_highest;
    recv_desc.allow_video_fields = false;

    NDIlib_recv_instance_t receiver = NDIlib_recv_create_v3(&recv_desc);

    g_Index::g_NDIFind = pNDI_find;

    NDIlib_find_destroy(pNDI_find);

    if (!receiver) {
        return nullptr;
    }

    return receiver;
}

void g_NDI::cleanup_device() {
    NDIlib_recv_destroy(g_Index::g_NDIReceiver);
    //NDIlib_find_destroy(g_Index::g_NDIFind);
    NDIlib_destroy();
}
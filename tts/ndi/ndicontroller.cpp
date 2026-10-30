#include "ndicontroller.h"
#include "../globals.h"

namespace g_NDI {
    NDIlib_recv_instance_t connect_to_default_device()
    {
        // 1-PC 模式下直接返回有效句柄表示本地螢幕擷取已就緒
        return reinterpret_cast<NDIlib_recv_instance_t>(1);
    }

    void cleanup_device() {
        // 1-PC 模式清理
    }
}

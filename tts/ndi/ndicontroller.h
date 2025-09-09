#pragma once
#include <Processing.NDI.Lib.h>

#include "../globals.h"

namespace g_NDI {
    NDIlib_recv_instance_t connect_to_default_device();

    void cleanup_device();
}
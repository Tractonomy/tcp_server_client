#pragma once

#include <cstdio>

#define MAX_PACKET_SIZE 2048

namespace fd_wait {
    enum Result {
        FAILURE,
        TIMEOUT,
        SUCCESS
    };

    Result waitFor(const FileDescriptor &fileDescriptor, uint32_t timeoutSeconds = 1);
};





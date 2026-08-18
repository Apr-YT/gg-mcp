/* posix_stub/sys/uio.h — MinGW 缺 sys/uio.h（仅测试编译期用）。 */
#pragma once
#include <stddef.h>

struct iovec {
    void *iov_base;
    size_t iov_len;
};

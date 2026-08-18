#pragma once
struct pollfd { int fd; short events; short revents; };
#define POLLOUT 0x0010
int poll(struct pollfd*, unsigned int, int);

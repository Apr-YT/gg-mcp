#pragma once
int fcntl(int, int, int);
#define F_GETFL 3
#define F_SETFL 4
#define O_NONBLOCK 04000

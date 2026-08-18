#pragma once
#include <cstddef>
typedef int socklen_t;
struct sockaddr { unsigned short sa_family; char sa_data[14]; };
#define AF_INET 2
#define SOCK_STREAM 1
#define SOL_SOCKET 0xffff
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define SO_ERROR 0x1007
int socket(int, int, int);
int connect(int, const sockaddr*, socklen_t);
int recv(int, void*, int, int);
int send(int, const void*, int, int);
int setsockopt(int, int, int, const void*, socklen_t);
int getsockopt(int, int, int, void*, socklen_t*);

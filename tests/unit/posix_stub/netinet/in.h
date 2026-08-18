#pragma once
#include <cstdint>
struct in_addr { unsigned int s_addr; };
struct sockaddr_in { unsigned short sin_family; unsigned short sin_port; in_addr sin_addr; char sin_zero[8]; };
unsigned short htons(unsigned short);

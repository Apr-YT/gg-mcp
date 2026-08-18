#include "../../agentd/driver.h"
#include <stdio.h>
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int pid = qx_find_hidden_pid();
    printf("qx_find_hidden_pid -> %d\n", pid);
    return pid > 0 ? 0 : 1;
}

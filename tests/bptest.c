/* bptest.c — hwBreakpointProc_module 最小直连测试（绕过 agentd/MCP）
 *
 * 用法: bptest <mode> [addr]
 *   mode 0: 裸读节点（read 33 字节空头, 触发 proc_read 间接调用）
 *   mode 1: CMD_OPEN_PROCESS（自身 pid）
 *   mode 2: OPEN + CMD_INST_PROCESS_HWBP（对 addr 装 W 断点, 不触发命中）
 *   mode 3: 发任意命令码（addr 参数当作 cmd; p1=自身pid, p2/p3=0）
 *
 * 每步后打印结果并 sleep，观察设备在哪一步挂死。
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define NODE "/proc/dce3771681d4c7a143d5d06b7d32548e/dce3771681d4c7a143d5d06b7d32548e"
#define CMD_OPEN_PROCESS 1
#define CMD_CLOSE_PROCESS 2
#define CMD_INST_PROCESS_HWBP 5

#pragma pack(push,1)
struct req { char cmd; uint64_t p1; uint64_t p2; uint64_t p3; uint64_t bs; };
#pragma pack(pop)

int main(int argc, char **argv) {
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    uint64_t addr = argc > 2 ? strtoull(argv[2], NULL, 0) : 0;
    uint8_t buf[512];
    struct req *r = (struct req *)buf;
    int fd, n;
    uint64_t handle = 0, bp = 0;

    printf("bptest mode=%d addr=0x%llx pid=%d\n", mode, (unsigned long long)addr, (int)getpid());
    fflush(stdout);

    fd = open(NODE, O_RDWR);
    if (fd < 0) { printf("open failed: %s\n", strerror(errno)); return 1; }
    printf("node opened fd=%d\n", fd); fflush(stdout);

    if (mode == 3) {
        /* 发任意命令码（p1=自身pid, p2/p3=0） */
        memset(buf, 0, sizeof(buf));
        r->cmd = (char)(addr & 0xff);
        r->p1 = (uint64_t)getpid();
        printf("sending cmd=%d\n", r->cmd); fflush(stdout);
        n = (int)read(fd, buf, 33);
        printf("cmd=%d read rc=%d\n", r->cmd, n); fflush(stdout);
        sleep(3);
        printf("bptest done\n");
        return 0;
    }

    if (mode == 0) {
        /* 裸读: 全零头 → cmd=0 → default -EINVAL, 但触发 proc_read 间接调用 */
        memset(buf, 0, sizeof(buf));
        n = (int)read(fd, buf, 33);
        printf("bare read rc=%d\n", n); fflush(stdout);
        sleep(3);
        printf("bptest done\n");
        return 0;
    }

    /* OPEN */
    memset(buf, 0, sizeof(buf));
    r->cmd = CMD_OPEN_PROCESS;
    r->p1 = (uint64_t)getpid();
    n = (int)read(fd, buf, 33);
    printf("OPEN rc=%d\n", n); fflush(stdout);
    if (n < 0) { printf("OPEN failed: %s\n", strerror(errno)); return 1; }
    handle = *(uint64_t *)(buf + 33);
    printf("OPEN handle=0x%llx\n", (unsigned long long)handle); fflush(stdout);
    sleep(2);

    if (mode == 1) {
        printf("bptest done (open only)\n");
        return 0;
    }

    /* INST */
    memset(buf, 0, sizeof(buf));
    r->cmd = CMD_INST_PROCESS_HWBP;
    r->p1 = handle;
    r->p2 = addr;
    r->p3 = 4 | ((uint64_t)2 << 8);   /* len=4, type=W(2) */
    printf("INST(addr=0x%llx)...\n", (unsigned long long)addr); fflush(stdout);
    n = (int)read(fd, buf, 33);
    printf("INST rc=%d\n", n); fflush(stdout);
    if (n < 0) { printf("INST failed: %s\n", strerror(errno)); return 1; }
    bp = *(uint64_t *)(buf + 33);
    printf("INST bp=0x%llx\n", (unsigned long long)bp); fflush(stdout);

    sleep(3);
    printf("bptest done (installed)\n");
    return 0;
}

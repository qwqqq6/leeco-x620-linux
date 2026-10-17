/* Request recovery through the Linux kernel's standard RESTART2 interface. */
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

int main(void)
{
    sync();
    syscall(SYS_reboot, 0xfee1deadUL, 0x28121969UL, 0xa1b2c3d4UL, "recovery");
    perror("reboot recovery");
    return 1;
}

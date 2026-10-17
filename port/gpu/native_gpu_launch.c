/* Freestanding AArch64 Linux launcher: set the private root before the
 * original linker can load Mali or its constructors. No libc dependency. */
static long syscall3(long number, long a, long b, long c) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x8 __asm__("x8") = number;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory", "cc");
    return x0;
}
#ifndef NATIVE_GPU_ROOT
#define NATIVE_GPU_ROOT "/tmp/native-gpu-probe"
#endif
void _start(void) {
    static char *const args[] = {"/native-gpu-probe", 0};
    static char *const env[] = {"LD_PRELOAD=/system/lib64/libnativegpuprops.so",
        "LD_LIBRARY_PATH=/system/lib64:/system/lib64/egl", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", 0};
    /* Linux asm-generic syscall numbers: chroot=51, chdir=49, execve=221. */
    if (!syscall3(51, (long)NATIVE_GPU_ROOT, 0, 0) &&
        !syscall3(49, (long)"/", 0, 0)) syscall3(221, (long)args[0], (long)args, (long)env);
    static const char error[] = "GPU_LAUNCH_FAILED\n";
    syscall3(64, 2, (long)error, sizeof error-1);
    syscall3(94, 126, 0, 0);
    for (;;) {}
}

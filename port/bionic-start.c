/*
 * ARM64 entry pattern adapted from Android 6 bionic crtbegin.c.
 * Copyright (C) 2013 The Android Open Source Project. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * Source and SHA256: docs/sources/aosp-arm64-crtbegin.{txt,json}.
 * A narrow hardware helper uses the original bionic ABI. Linux PID 1,
 * desktop, network services and applications continue to use musl.
 */
typedef void (*init_fn)(void);
typedef struct {
    init_fn *preinit_array;
    init_fn *init_array;
    init_fn *fini_array;
} structors_array_t;

static init_fn preinit[] __attribute__((section(".preinit_array"), used)) = {(init_fn)-1, 0};
static init_fn init[] __attribute__((section(".init_array"), used)) = {(init_fn)-1, 0};
static init_fn fini[] __attribute__((section(".fini_array"), used)) = {(init_fn)-1, 0};

extern int main(int, char **, char **);
extern void __libc_init(void *, init_fn, int (*)(int, char **, char **), const structors_array_t *) __attribute__((noreturn));

void do_arm64_start(void *raw_args) __attribute__((visibility("hidden")));
void do_arm64_start(void *raw_args) {
    const structors_array_t arrays = {preinit, init, fini};
    __libc_init(raw_args, 0, main, &arrays);
}

__asm__(".text\n.align 2\n.global _start\n.hidden _start\n.type _start, %function\n"
        "_start:\nmov x0, sp\nb do_arm64_start\n.size _start, .-_start\n");

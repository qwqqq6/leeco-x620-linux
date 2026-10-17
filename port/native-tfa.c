/* Narrow bionic helper for the original TFA9890 library.
 * The power check keeps the amplifier muted and never calls OTC/calibration.
 */
#include "native-tfa-settings.h"
extern int printf(const char *, ...);
extern int fflush(void *);
extern int strcmp(const char *, const char *);
extern int memcmp(const void *, const void *, unsigned long);
extern void *fopen(const char *, const char *);
extern unsigned long fread(void *, unsigned long, unsigned long, void *);
extern int fseek(void *, long, int);
extern long ftell(void *);
extern int fclose(void *);
extern int usleep(unsigned int);
extern int fork(void);
extern int waitpid(int, int *, int);
extern int kill(int, int);
extern unsigned int alarm(unsigned int);
extern int *__errno(void);
extern int execve(const char *, char *const [], char *const []);
extern void _exit(int) __attribute__((noreturn));
typedef void (*handler_t)(int);
extern handler_t signal(int, handler_t);
extern int Tfa98xx_Open(unsigned char address, int *handle);
extern int Tfa98xx_Close(int handle);
extern int Tfa98xx_ReadRegister16(int handle, unsigned char reg, unsigned short *value);
extern int Tfa98xx_Init(int handle);
extern int Tfa98xx_SetMute(int handle, int state);
extern int Tfa98xx_SetVolume(int handle, float db);
extern int Tfa98xx_SetSampleRate(int handle, int rate);
extern int Tfa98xx_DspReset(int handle, int state);
extern int Tfa98xx_Powerdown(int handle, int state);
extern int Tfa98xx_DspSystemStable(int handle, int *ready);
extern int Tfa98xx_WriteRegister16(int handle, unsigned char reg, unsigned short value);
extern int Tfa98xx_DspPatch(int handle, int length, const unsigned char *data);
extern int Tfa98xx_DspWriteSpeakerParameters(int handle, int length, const unsigned char *data);
extern int Tfa98xx_DspWriteConfig(int handle, int length, const unsigned char *data);
extern int Tfa98xx_DspWritePreset(int handle, int length, const unsigned char *data);
extern int Tfa98xx_DspBiquad_SetCoeff(int handle, int index, float b0, float b1, float b2, float a1, float a2);
extern int Tfa98xx_DspBiquad_Disable(int handle, int index);
extern int Tfa98xx_DspReadMem(int handle, unsigned short start, int count, int *values);
extern int Tfa98xx_DspGetCalibrationImpedance(int handle, float *ohms);
extern int Tfa98xx_SetConfigured(int handle);
extern int Tfa98xx_DspConfigParameterCount(int handle, int *count);

static volatile int interrupted = 0;
static volatile int player_pid = 0;
static void stop(int number) {
    interrupted = number;
    if (player_pid > 0) kill(player_pid, 15);
}

static unsigned int le32(const unsigned char *p) {
    return p[0] | (unsigned int)p[1] << 8 | (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

static int validate_wav(const char *path) {
    void *file = fopen(path, "rb");
    if (!file) return 1;
    unsigned char head[16];
    int failed = 1, format_ok = 0;
    if (fseek(file, 0, 2)) goto out;
    long length = ftell(file);
    if (length < 44 || length > 16 * 1024 * 1024 || fseek(file, 0, 0)) goto out;
    if (fread(head, 1, 12, file) != 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) goto out;
    long limit = (long)le32(head + 4) + 8;
    if (limit > length || limit < 44) goto out;
    for (int chunks = 0; chunks < 64 && ftell(file) + 8 <= limit; chunks++) {
        if (fread(head, 1, 8, file) != 8) goto out;
        unsigned int size = le32(head + 4);
        long next = ftell(file) + size + (size & 1);
        if (next > limit) goto out;
        if (!memcmp(head, "fmt ", 4)) {
            if (size < 16 || fread(head, 1, 16, file) != 16) goto out;
            format_ok = head[0] == 1 && head[1] == 0 && head[2] == 2 && head[3] == 0
                && le32(head + 4) == 48000 && le32(head + 8) == 192000
                && head[12] == 4 && head[13] == 0 && head[14] == 16 && head[15] == 0;
        } else if (!memcmp(head, "data", 4)) {
            if (format_ok && size > 0 && size <= 60 * 192000 && size % 4 == 0) failed = 0;
            goto out;
        }
        if (fseek(file, next, 0)) goto out;
    }
out:
    fclose(file);
    if (failed) printf("Expected PCM WAV: 48000 Hz, stereo, S16_LE, 1 sample to 60 seconds\n");
    return failed;
}

static int registers(int handle) {
    const unsigned char regs[] = {0x03, 0x00, 0x01, 0x02, 0x04, 0x06, 0x07,
                                  0x09, 0x0a, 0x70, 0x80, 0x83, 0x84};
    int failed = 0;
    for (unsigned int i = 0; i < sizeof(regs); i++) {
        unsigned short value = 0;
        int result = Tfa98xx_ReadRegister16(handle, regs[i], &value);
        printf("reg=0x%02x result=%d value=0x%04x\n", regs[i], result, value);
        if (result) failed = 1;
    }
    fflush((void *)0);
    return failed;
}

static int check(const char *name, int result) {
    printf("%s result=%d\n", name, result);
    fflush((void *)0);
    return result;
}

#define STEP(expression) do { if (interrupted || check(#expression, (expression))) { failed = 1; goto cleanup; } } while (0)

int main(int argc, char **argv, char **env) {
    (void)env;
    int play = argc == 3 && !strcmp(argv[1], "--play");
    int off = argc == 2 && !strcmp(argv[1], "--off");
    int dsp = play || (argc == 2 && !strcmp(argv[1], "--dsp-check"));
    int power = dsp || (argc == 2 && !strcmp(argv[1], "--power-check"));
    if (!play && !off && (argc != 2 || (!power && strcmp(argv[1], "--check")))) {
        printf("Usage: native-tfa --check | --off | --power-check | --dsp-check | --play FILE.wav\n");
        return 2;
    }
    signal(1, stop);
    signal(2, stop);
    signal(15, stop);
    signal(14, stop);
    alarm(65);
    if (play && validate_wav(argv[2])) return 2;
    int handle = -1;
    int result = Tfa98xx_Open(0x68, &handle);
    printf("Tfa98xx_Open result=%d handle=%d\n", result, handle);
    if (result) return 1;
    int failed = 0;
    if (off) goto cleanup;
    failed = registers(handle);
    if (failed || !power) goto close;
    STEP(Tfa98xx_Init(handle));
    STEP(Tfa98xx_SetMute(handle, 2));
    STEP(Tfa98xx_SetVolume(handle, -30.0f));
    STEP(Tfa98xx_SetSampleRate(handle, 48000));
    STEP(Tfa98xx_DspReset(handle, 1));
    STEP(Tfa98xx_Powerdown(handle, 0));
    for (int i = 0; i < 300; i++) {
        int ready = 0;
        STEP(Tfa98xx_DspSystemStable(handle, &ready));
        if (ready) {
            printf("DSP hardware ready after %d polls (DSP held in reset)\n", i + 1);
            goto powered;
        }
        usleep(2000);
    }
    printf("DSP hardware readiness timed out\n");
    failed = 1;
powered:
    failed |= registers(handle);
    if (failed || !dsp) goto cleanup;
    unsigned short mtp = 0, re0 = 0;
    STEP(Tfa98xx_ReadRegister16(handle, 0x80, &mtp));
    STEP(Tfa98xx_ReadRegister16(handle, 0x83, &re0));
    if ((mtp & 3) != 3 || !re0) {
        printf("Refusing DSP start without existing once-only calibration\n");
        failed = 1;
        goto cleanup;
    }
    /* Match this handset coldStartup boost-current setting (volatile 0x07). */
    unsigned short boost = 0;
    STEP(Tfa98xx_ReadRegister16(handle, 0x07, &boost));
    STEP(Tfa98xx_WriteRegister16(handle, 0x07, (boost & 0xffc0) | 0x1f));
    STEP(Tfa98xx_DspPatch(handle, sizeof(cold_patch), cold_patch));
    STEP(Tfa98xx_DspPatch(handle, sizeof(dsp_patch), dsp_patch));
    int config_words = 0;
    STEP(Tfa98xx_DspConfigParameterCount(handle, &config_words));
    printf("DSP config words=%d; original HAL transfers 55 words\n", config_words);
    if (config_words != 55 && config_words != 67) {
        failed = 1;
        goto cleanup;
    }
    STEP(Tfa98xx_DspWriteSpeakerParameters(handle, sizeof(speaker_model), speaker_model));
    STEP(Tfa98xx_DspWriteConfig(handle, config_words * 3, speaker_config));
    STEP(Tfa98xx_DspWritePreset(handle, sizeof(speaker_preset), speaker_preset));
    for (int i = 0; i < 10; i++) {
        const float *p = speaker_eq[i];
        if (p[0] == 1 && p[1] == 0 && p[2] == 0 && p[3] == 0 && p[4] == 0) {
            STEP(Tfa98xx_DspBiquad_Disable(handle, i + 1));
        } else {
            STEP(Tfa98xx_DspBiquad_SetCoeff(handle, i + 1, p[0], p[1], p[2], p[3], p[4]));
        }
    }
    STEP(Tfa98xx_SetConfigured(handle));
    int done = 0;
    for (int i = 0; i < 100; i++) {
        STEP(Tfa98xx_DspReadMem(handle, 231, 1, &done));
        if (done) break;
        usleep(10000);
    }
    float impedance = 0;
    STEP(Tfa98xx_DspGetCalibrationImpedance(handle, &impedance));
    printf("DSP calibration ready=%d existing impedance=%.3f ohms\n", done, (double)impedance);
    if (!done || impedance < 3.0f || impedance > 16.0f) failed = 1;
    unsigned short after_mtp = 0, after_re0 = 0;
    STEP(Tfa98xx_ReadRegister16(handle, 0x80, &after_mtp));
    STEP(Tfa98xx_ReadRegister16(handle, 0x83, &after_re0));
    printf("Calibration unchanged=%d\n", mtp == after_mtp && re0 == after_re0);
    if (mtp != after_mtp || re0 != after_re0) failed = 1;
    if (failed || !play || interrupted) goto cleanup;
    STEP(Tfa98xx_SetVolume(handle, -20.0f));
    STEP(Tfa98xx_SetMute(handle, 0));
    int child = fork();
    if (child == 0) {
        char *arguments[] = {"aplay", "-D", "hw:0,8", "--", argv[2], (char *)0};
        char *environment[] = {"PATH=/usr/bin:/bin", "LANG=C", (char *)0};
        execve("/usr/bin/aplay", arguments, environment);
        _exit(127);
    }
    if (child < 0) { failed = 1; goto cleanup; }
    player_pid = child;
    if (interrupted) kill(child, 15);
    int status = 0, waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && *__errno() == 4);
    player_pid = 0;
    printf("aplay wait=%d status=%d interrupted=%d\n", waited, status, interrupted);
    if (waited < 0 || status != 0 || interrupted) failed = 1;
cleanup:
    failed |= check("cleanup mute", Tfa98xx_SetMute(handle, 2)) != 0;
    failed |= check("cleanup powerdown", Tfa98xx_Powerdown(handle, 1)) != 0;
    failed |= registers(handle);
close:
    Tfa98xx_Close(handle);
    return failed;
}

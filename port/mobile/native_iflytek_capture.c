/* PortAudio input ABI adapter for the official iFlytek recognizer.
 *
 * The original MT6797 PCM driver works with native arecord's read/write path.
 * A bounded route helper owns arecord; samples cross a private FIFO only.
 * This library does not access hardware, save audio, recognize speech or send
 * network traffic. The unmodified vendor engine receives 16 kHz mono S16.
 * All runtime functions are resolved from the target glibc process.
 */
extern void *dlsym(void *, const char *);
#define NEXT(name, type) ((type)dlsym((void *)-1L, name))
typedef struct {
    int device, channels;
    unsigned long format;
    double latency;
    void *specific;
} pa_params;
typedef struct {
    int version;
    const char *name;
    int api, inputs, outputs;
    double low_input, low_output, high_input, high_output, rate;
} pa_device;
typedef int (*pa_callback)(const void *, void *, unsigned long,
                           const void *, unsigned long, void *);
struct pollfd { int fd; short events, revents; };
struct timespec { long seconds, nanoseconds; };
static struct {
    int used, joined, fd, position, phase;
    volatile int running;
    unsigned long thread, output_frames, delivered_frames;
    void *user;
    pa_callback callback;
    int history[63];
    short output[1024];
} capture;
static const pa_device device = {1, "Le620 native microphone", 0, 1, 0,
                                  0.20, 0.0, 0.50, 0.0, 16000.0};
static unsigned int converted;
static void state(const char *text)
{
    int (*getpid_fn)(void) = NEXT("getpid", int (*)(void));
    int (*format_fn)(char *, unsigned long, const char *, ...) =
        NEXT("snprintf", int (*)(char *, unsigned long, const char *, ...));
    int (*open_fn)(const char *, int, ...) = NEXT("open", int (*)(const char *, int, ...));
    long (*write_fn)(int, const void *, unsigned long) = NEXT("write", long (*)(int, const void *, unsigned long));
    int (*close_fn)(int) = NEXT("close", int (*)(int));
    int (*rename_fn)(const char *, const char *) = NEXT("rename", int (*)(const char *, const char *));
    char path[100], temporary[104];
    unsigned long length = 0;
    int fd;
    if (!getpid_fn || !format_fn || !open_fn || !write_fn || !close_fn || !rename_fn) return;
    format_fn(path, sizeof(path), "/run/user/1000/iflytek-capture.%d.state", getpid_fn());
    format_fn(temporary, sizeof(temporary), "%s.tmp", path);
    while (text[length]) length++;
    fd = open_fn(temporary, 1 | 0100 | 01000 | 00400000 | 02000000, 0600);
    if (fd < 0) return;
    if (write_fn(fd, text, length) == (long)length) {
        close_fn(fd);
        rename_fn(temporary, path);
    } else close_fn(fd);
}
static const int fir[63] = { 17, 29, 20, -8, -40, -50, -18, 49, 101, 82, -23, -152, -195, -76, 157, 334, 276, -47, -440, -577, -252, 408, 939, 829, -69, -1288, -1895, -1019, 1542, 5045, 8073, 9264, 8073, 5045, 1542, -1019, -1895, -1288, -69, 829, 939, 408, -252, -577, -440, -47, 276, 334, 157, -76, -195, -152, -23, 82, 101, 49, -18, -50, -40, -8, 20, 29, 17 };

static double now(void)
{
    int (*clock_gettime_fn)(int, struct timespec *) =
        NEXT("clock_gettime", int (*)(int, struct timespec *));
    struct timespec value;
    if (!clock_gettime_fn || clock_gettime_fn(1, &value)) return 0.0;
    return (double)value.seconds + (double)value.nanoseconds / 1000000000.0;
}

static int deliver(const short *samples)
{
    unsigned long frame;
    for (frame = 0; frame < 3072; frame++) {
        int value = ((int)samples[frame * 2] + samples[frame * 2 + 1]) / 2;
        capture.history[capture.position] = value;
        capture.position = (capture.position + 1) % 63;
        if (++capture.phase == 3) {
            long long total = 0;
            int tap, pos = capture.position;
            capture.phase = 0;
            for (tap = 0; tap < 63; tap++) {
                pos = pos ? pos - 1 : 62;
                total += (long long)capture.history[pos] * fir[tap];
            }
            value = (int)(total / 32768);
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            capture.output[capture.output_frames++] = (short)value;
            if (capture.output_frames == 1024) {
                double time_info[3];
                int result;
                time_info[1] = now();
                time_info[0] = time_info[1] - 0.064;
                time_info[2] = 0.0;
                result = capture.callback(capture.output, (void *)0, 1024,
                                          time_info, 0, capture.user);
                capture.delivered_frames += 1024;
                capture.output_frames = 0;
                if (result) return result;
            }
        }
    }
    return 0;
}

static void *reader(void *unused)
{
    int (*poll_fn)(struct pollfd *, unsigned long, int) =
        NEXT("poll", int (*)(struct pollfd *, unsigned long, int));
    long (*read_fn)(int, void *, unsigned long) = NEXT("read", long (*)(int, void *, unsigned long));
    int (*sleep_fn)(unsigned int) = NEXT("usleep", int (*)(unsigned int));
    short block[6144];
    unsigned long filled = 0;
    double deadline = now() + 65.0;
    int seen = 0;
    (void)unused;
    if (!poll_fn || !read_fn || !sleep_fn) {
        capture.running = 0;
        return (void *)0;
    }
    while (capture.running && now() < deadline) {
        struct pollfd descriptor = {capture.fd, 1, 0};
        int ready = poll_fn(&descriptor, 1, 100);
        if (ready > 0 && (descriptor.revents & 1)) {
            long size = read_fn(capture.fd, (char *)block + filled, sizeof(block) - filled);
            if (size > 0) {
                filled += (unsigned long)size;
                seen = 1;
                if (filled == sizeof(block)) {
                    filled = 0;
                    if (!capture.delivered_frames) state("streaming");
                    if (deliver(block)) break;
                }
            }
        } else if (ready > 0 && (descriptor.revents & (8 | 16 | 32))) {
            if (seen) break;
            sleep_fn(10000);
        } else if (ready < 0) {
            break;
        }
    }
    capture.running = 0;
    state("stopped");
    return (void *)0;
}

int Pa_CloseStream(void *stream);
int Pa_Initialize(void) { if (!capture.used) state("closed"); return 0; }
int Pa_Terminate(void) { return capture.used ? Pa_CloseStream(&capture) : 0; }
int Pa_GetDeviceCount(void) { return 1; }
int Pa_GetDefaultInputDevice(void) { return 0; }
int Pa_GetDefaultOutputDevice(void) { return -1; }
const pa_device *Pa_GetDeviceInfo(int index) { return index == 0 ? &device : (const pa_device *)0; }
int Pa_IsFormatSupported(const pa_params *input, const pa_params *output, double rate)
{
    return input && !output && input->device == 0 && input->channels == 1 &&
           input->format == 8 && rate == 16000.0 ? 0 : -9996;
}

int Pa_OpenStream(void **stream, const pa_params *input, const pa_params *output,
                  double rate, unsigned long frames, unsigned long flags,
                  pa_callback callback, void *user)
{
    int i;
    (void)flags;
    if (capture.used) return -9985;
    if (Pa_IsFormatSupported(input, output, rate) || frames != 1024 || !callback) return -9996;
    capture.used = 1;
    capture.joined = 1;
    capture.fd = -1;
    capture.position = capture.phase = capture.running = 0;
    capture.output_frames = capture.delivered_frames = 0;
    capture.callback = callback;
    capture.user = user;
    for (i = 0; i < 63; i++) capture.history[i] = 0;
    *stream = &capture;
    converted++;
    state("opened");
    return 0;
}

int Pa_StartStream(void *stream)
{
    int (*open_fn)(const char *, int, ...) = NEXT("open", int (*)(const char *, int, ...));
    int (*close_fn)(int) = NEXT("close", int (*)(int));
    int (*create_fn)(unsigned long *, const void *, void *(*)(void *), void *) =
        NEXT("pthread_create", int (*)(unsigned long *, const void *, void *(*)(void *), void *));
    if (stream != &capture || !capture.used || capture.running || !open_fn || !close_fn || !create_fn)
        return -9985;
    /* Linux O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW. The FIFO is user-owned 0600. */
    capture.fd = open_fn("/run/user/1000/iflytek-capture.pcm", 04000 | 02000000 | 00400000);
    if (capture.fd < 0) return -9985;
    capture.running = 1;
    state("recording");
    if (create_fn(&capture.thread, (void *)0, reader, (void *)0)) {
        capture.running = 0;
        close_fn(capture.fd);
        capture.fd = -1;
        state("opened");
        return -9999;
    }
    capture.joined = 0;
    return 0;
}

int Pa_AbortStream(void *stream)
{
    int (*join_fn)(unsigned long, void **) = NEXT("pthread_join", int (*)(unsigned long, void **));
    int (*close_fn)(int) = NEXT("close", int (*)(int));
    if (stream != &capture || !capture.used || !join_fn || !close_fn) return -9988;
    capture.running = 0;
    if (!capture.joined) {
        if (join_fn(capture.thread, (void *)0)) return -9999;
        capture.joined = 1;
    }
    if (capture.fd >= 0) {
        close_fn(capture.fd);
        capture.fd = -1;
    }
    state("stopped");
    return 0;
}
int Pa_StopStream(void *stream) { return Pa_AbortStream(stream); }
int Pa_CloseStream(void *stream)
{
    int result = Pa_AbortStream(stream);
    if (!result) {
        capture.used = 0;
        state("closed");
    }
    return result;
}
int Pa_IsStreamActive(void *stream) { return stream == &capture && capture.used ? capture.running : -9988; }
int Pa_IsStreamStopped(void *stream) { return stream == &capture && capture.used ? !capture.running : -9988; }
double Pa_GetStreamTime(void *stream) { (void)stream; return now(); }
double Pa_GetStreamCpuLoad(void *stream) { (void)stream; return 0.0; }
unsigned int native_iflytek_access_denials(void) { return 0; }
unsigned int native_iflytek_converted_streams(void) { return converted; }
unsigned long native_iflytek_delivered_frames(void) { return capture.delivered_frames; }

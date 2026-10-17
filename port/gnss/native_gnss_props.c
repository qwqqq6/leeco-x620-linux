/* Original X620 GNSS daemon compatibility, ARM32, private process only.
 * The caller must first expose the original /nvdata read-only in its root.
 * No Android property service is started; writes are never forwarded.
 */
typedef __builtin_va_list va_list;
extern int vsnprintf(char *, unsigned, const char *, va_list);
extern int write(int, const void *, unsigned);
extern int open(const char *, int, ...);
extern int close(int);

/* The original caller uses LID 0x34, 52 bytes x 1 record and IsRead=1.
 * ARM32 returns this 12-byte F_ID through the hidden structure pointer.
 * Reading this exact already-mounted record avoids Android NVRAM service
 * initialization, partition discovery and any automatic restore/write path.
 */
struct native_fid { int descriptor, opaque, is_read; };
struct native_fid NVM_GetFileDesc(int lid, int *size, int *number, int is_read) {
    struct native_fid result = {-1, 0, 1};
    if (lid != 0x34 || !is_read || !size || !number) return result;
    int file = open("/data/nvram/APCFG/APRDEB/GPS", 0x80000);
    if (file < 0) return result;
    *size = 52; *number = 1;
    result.descriptor = file;
    const char message[] = "GNSS_FACTORY_RECORD_OPENED bytes=52 records=1 readonly=1\n";
    write(2, message, sizeof message - 1);
    return result;
}
int NVM_CloseFileDesc(struct native_fid file) {
    return file.descriptor >= 0 && close(file.descriptor) == 0;
}

static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static unsigned length(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static int contains(const char *s, const char *needle) {
    if (!s) return 0;
    for (; *s; s++) {
        unsigned n = 0;
        while (needle[n] && s[n] == needle[n]) n++;
        if (!needle[n]) return 1;
    }
    return 0;
}
int property_get(const char *key, char *out, const char *fallback) {
    const char *value = fallback;
    if (equal(key, "service.nvram_init")) value = "Ready";
    else if (equal(key, "persist.mtk.wcn.combo.chipid")) value = "0x6797";
    else if (equal(key, "service.wcn.driver.ready")) value = "yes";
    unsigned n = 0;
    while (value && value[n] && n < 91) { out[n] = value[n]; n++; }
    out[n] = 0;
    return (int)n;
}
int property_set(const char *key, const char *value) { (void)key; (void)value; return 0; }
static int log_message(const char *tag, const char *format, va_list args) {
    /* Test logs retain status and calibration results, not locations/NMEA. */
    if (contains(format, "nmea") || contains(format, "NMEA") || contains(format, "location") ||
        contains(format, "latitude") || contains(format, "longitude") || contains(format, "position")) return 0;
    char buffer[1600];
    int n = vsnprintf(buffer, sizeof buffer, format, args);
    if (n < 0) return n;
    if (n >= (int)sizeof buffer) n = sizeof buffer - 1;
    if (tag) { write(2, tag, length(tag)); write(2, ": ", 2); }
    write(2, buffer, (unsigned)n);
    write(2, "\n", 1);
    return n;
}
int __android_log_print(int priority, const char *tag, const char *format, ...) {
    (void)priority;
    va_list args; __builtin_va_start(args, format);
    int n = log_message(tag, format, args);
    __builtin_va_end(args); return n;
}
int __android_log_buf_print(int buffer_id, int priority, const char *tag, const char *format, ...) {
    (void)buffer_id; (void)priority;
    va_list args; __builtin_va_start(args, format);
    int n = log_message(tag, format, args);
    __builtin_va_end(args); return n;
}
/* Audited original liblog.so uses {tag, format, priority}, not four fixed args. */
struct xlog_record { const char *tag, *format; int priority; };
int __xlog_buf_printf(int buffer_id, const struct xlog_record *record, ...) {
    (void)buffer_id;
    if (!record) return 0;
    va_list args; __builtin_va_start(args, record);
    int n = log_message(record->tag, record->format, args);
    __builtin_va_end(args); return n;
}

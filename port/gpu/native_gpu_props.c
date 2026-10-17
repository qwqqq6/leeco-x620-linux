/* Process-private hardware selection; never start Android properties. */
static int equal(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
int property_get(const char *key, char *out, const char *fallback) {
    const char *value = fallback;
    if (equal(key, "ro.hardware") || equal(key, "ro.board.platform")) value = "mt6797";
    if (equal(key, "ro.hardware.gralloc")) value = "mt6797";
    if (equal(key, "ro.kernel.qemu")) value = "0";
    int n = 0;
    while (value && value[n] && n < 91) { out[n] = value[n]; n++; }
    out[n] = 0;
    return n;
}
int property_set(const char *key, const char *value) { (void)key; (void)value; return 0; }

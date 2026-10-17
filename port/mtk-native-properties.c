/* X620 connectivity constants for the original Bionic MTK helper only.
 * Build without libc or other dependencies; preload only for 6620_launcher.
 * There is no Android init/property service in the native Linux system.
 */
static int equal(const char *left, const char *right)
{
    while (*left && *left == *right) { ++left; ++right; }
    return *left == *right;
}

static int copy_value(char *destination, const char *source)
{
    int length = 0;
    if (source)
        while (length < 91 && source[length]) {
            destination[length] = source[length];
            ++length;
        }
    destination[length] = 0;
    return length;
}

int property_get(const char *key, char *value, const char *default_value)
{
    if (equal(key, "persist.mtk.wcn.combo.chipid"))
        return copy_value(value, "0x6797");
    if (equal(key, "service.wcn.driver.ready"))
        return copy_value(value, "yes");
    return copy_value(value, default_value);
}

int property_set(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return 0;
}

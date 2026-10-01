/*
 * meta.c — reads every metadata dictionary ImageIO exposes for a photo
 * (file attributes, image properties, TIFF, Exif, ExifAux, GPS, IPTC,
 * format-specific dictionaries, XMP and maker notes) and turns it into
 * a flat list of human-friendly rows plus a short summary.
 */
#include "meta.h"
#include "objc.h"

#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
#include <ctype.h>
#include <libgen.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define VALUE_MAX 4096

/* ------------------------------------------------------------------ */
/* small string utilities                                              */
/* ------------------------------------------------------------------ */

static void append(char *buf, size_t n, const char *s)
{
    size_t l = strlen(buf);
    if (l + 1 < n)
        snprintf(buf + l, n - l, "%s", s);
}

static void appendf(char *buf, size_t n, const char *fmt, ...)
{
    char tmp[VALUE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    append(buf, n, tmp);
}

static void cfstr(CFStringRef s, char *buf, size_t n)
{
    buf[0] = 0;
    if (!s || CFGetTypeID(s) != CFStringGetTypeID())
        return;
    if (CFStringGetCString(s, buf, (CFIndex)n, kCFStringEncodingUTF8))
        return;
    /* Too long: take as many whole characters as fit. */
    CFIndex used = 0;
    CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)), kCFStringEncodingUTF8, '?', false,
                     (UInt8 *)buf, (CFIndex)n - 1, &used);
    buf[used] = 0;
}

/* Number with at most `dec` decimals, trailing zeros removed. */
static void fmtnum(double v, int dec, char *buf, size_t n)
{
    snprintf(buf, n, "%.*f", dec, v);
    if (strchr(buf, '.')) {
        char *e = buf + strlen(buf) - 1;
        while (*e == '0') *e-- = 0;
        if (*e == '.') *e = 0;
    }
    if (strcmp(buf, "-0") == 0) strcpy(buf, "0");
}

static void thousands(long long v, char *buf, size_t n)
{
    char raw[32], out[48];
    snprintf(raw, sizeof raw, "%lld", v < 0 ? -v : v);
    size_t len = strlen(raw), j = 0;
    if (v < 0) out[j++] = '-';
    for (size_t i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) out[j++] = ',';
        out[j++] = raw[i];
    }
    out[j] = 0;
    snprintf(buf, n, "%s", out);
}

static void human_size(long long bytes, char *buf, size_t n)
{
    static const char *units[] = { "bytes", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int u = 0;
    while (v >= 1000.0 && u < 4) { v /= 1000.0; u++; }
    if (u == 0) snprintf(buf, n, "%lld bytes", bytes);
    else snprintf(buf, n, "%.1f %s", v, units[u]);
}

static void fmt_tm(const struct tm *tm, char *buf, size_t n)
{
    char wd[16], mon[16];
    strftime(wd, sizeof wd, "%a", tm);
    strftime(mon, sizeof mon, "%b", tm);
    snprintf(buf, n, "%s %d %s %d, %02d:%02d:%02d", wd, tm->tm_mday, mon, tm->tm_year + 1900,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
}

static void fmt_time(time_t t, char *buf, size_t n)
{
    struct tm tm;
    localtime_r(&t, &tm);
    fmt_tm(&tm, buf, n);
}

/* Exif style "YYYY:MM:DD HH:MM:SS" -> "Wed 12 Jun 2024, 18:22:31" */
static int fmt_exif_date(const char *s, char *buf, size_t n)
{
    struct tm tm = { 0 };
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6 || y < 1800)
        return 0;
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se; tm.tm_isdst = -1;
    timegm(&tm); /* normalise + fill in weekday without applying a time zone */
    fmt_tm(&tm, buf, n);
    return 1;
}

/* "ISOSpeedRatings" -> "ISO Speed Ratings", "LensModel" -> "Lens Model" */
static void split_camel(const char *s, char *buf, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; s[i] && j + 2 < n; i++) {
        char c = s[i], p = i ? s[i - 1] : 0, nx = s[i + 1];
        if (c == '_') { buf[j++] = ' '; continue; }
        if (i && p != ' ' && p != '_' &&
            ((isupper((unsigned char)c) && (islower((unsigned char)p) || isdigit((unsigned char)p) ||
                                            (isupper((unsigned char)p) && islower((unsigned char)nx)))) ||
             (isdigit((unsigned char)c) && isalpha((unsigned char)p))))
            buf[j++] = ' ';
        buf[j++] = c;
    }
    buf[j] = 0;
    if (buf[0]) buf[0] = (char)toupper((unsigned char)buf[0]);
}

/* ------------------------------------------------------------------ */
/* row list                                                            */
/* ------------------------------------------------------------------ */

static void push_row(Meta *m, RowKind kind, const char *key, const char *value)
{
    if (m->count == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 128;
        m->rows = realloc(m->rows, m->cap * sizeof *m->rows);
    }
    MetaRow *r = &m->rows[m->count++];
    r->kind = kind;
    r->key = strdup(key ? key : "");
    r->value = strdup(value ? value : "");
    if (kind == ROW_ITEM) {
        /* single line: turn control characters into spaces */
        for (char *p = r->value; *p; p++)
            if ((unsigned char)*p < 0x20) *p = ' ';
        m->items++;
    }
}

static size_t g_section_start;

static void begin_section(Meta *m, const char *title, const char *icon)
{
    g_section_start = m->count;
    push_row(m, ROW_HEADER, title, icon);
}

/* Drop the header again if nothing was added under it. */
static void end_section(Meta *m)
{
    if (m->count == g_section_start + 1) {
        m->count--;
        free(m->rows[m->count].key);
        free(m->rows[m->count].value);
    }
}

static void add_item(Meta *m, const char *key, const char *fmt, ...)
{
    char v[VALUE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(v, sizeof v, fmt, ap);
    va_end(ap);
    push_row(m, ROW_ITEM, key, v[0] ? v : "—");
}

void meta_free(Meta *m)
{
    for (size_t i = 0; i < m->count; i++) {
        free(m->rows[i].key);
        free(m->rows[i].value);
    }
    free(m->rows);
    if (m->thumbnail) CGImageRelease(m->thumbnail);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------ */
/* CoreFoundation access                                               */
/* ------------------------------------------------------------------ */

static CFTypeRef dget(CFDictionaryRef d, const char *key)
{
    if (!d) return NULL;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFDictionaryGetValue(d, k);
    CFRelease(k);
    return v;
}

static CFDictionaryRef ddict(CFDictionaryRef d, const char *key)
{
    CFTypeRef v = dget(d, key);
    return (v && CFGetTypeID(v) == CFDictionaryGetTypeID()) ? (CFDictionaryRef)v : NULL;
}

static int as_num(CFTypeRef v, double *out)
{
    if (!v) return 0;
    if (CFGetTypeID(v) == CFArrayGetTypeID() && CFArrayGetCount(v) > 0)
        v = CFArrayGetValueAtIndex(v, 0);
    if (CFGetTypeID(v) == CFNumberGetTypeID())
        return CFNumberGetValue(v, kCFNumberDoubleType, out);
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) {
        *out = CFBooleanGetValue(v);
        return 1;
    }
    return 0;
}

static int dnum(CFDictionaryRef d, const char *key, double *out) { return as_num(dget(d, key), out); }

static int dstr(CFDictionaryRef d, const char *key, char *buf, size_t n)
{
    CFTypeRef v = dget(d, key);
    buf[0] = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID()) cfstr(v, buf, n);
    /* trim whitespace */
    size_t l = strlen(buf);
    while (l && isspace((unsigned char)buf[l - 1])) buf[--l] = 0;
    return buf[0] != 0;
}

/* Generic, recursive value -> text. */
static void fmt_value(CFTypeRef v, char *buf, size_t n)
{
    if (!v) return;
    CFTypeID t = CFGetTypeID(v);
    char tmp[VALUE_MAX];

    if (t == CFStringGetTypeID()) {
        cfstr(v, tmp, sizeof tmp);
        append(buf, n, tmp);
    } else if (t == CFNumberGetTypeID()) {
        if (CFNumberIsFloatType(v)) {
            double d;
            CFNumberGetValue(v, kCFNumberDoubleType, &d);
            fmtnum(d, 4, tmp, sizeof tmp);
        } else {
            long long l;
            CFNumberGetValue(v, kCFNumberLongLongType, &l);
            snprintf(tmp, sizeof tmp, "%lld", l);
        }
        append(buf, n, tmp);
    } else if (t == CFBooleanGetTypeID()) {
        append(buf, n, CFBooleanGetValue(v) ? "Yes" : "No");
    } else if (t == CFArrayGetTypeID()) {
        CFIndex c = CFArrayGetCount(v);
        for (CFIndex i = 0; i < c; i++) {
            if (i) append(buf, n, ", ");
            fmt_value(CFArrayGetValueAtIndex(v, i), buf, n);
        }
    } else if (t == CFDictionaryGetTypeID()) {
        CFIndex c = CFDictionaryGetCount(v);
        const void **keys = malloc(sizeof(void *) * (size_t)c);
        const void **vals = malloc(sizeof(void *) * (size_t)c);
        CFDictionaryGetKeysAndValues(v, keys, vals);
        for (CFIndex i = 0; i < c; i++) {
            if (i) append(buf, n, "; ");
            fmt_value(keys[i], buf, n);
            append(buf, n, ": ");
            fmt_value(vals[i], buf, n);
        }
        free(keys);
        free(vals);
    } else if (t == CFDataGetTypeID()) {
        appendf(buf, n, "%ld bytes of binary data", (long)CFDataGetLength(v));
    } else if (t == CFDateGetTypeID()) {
        fmt_time((time_t)(CFDateGetAbsoluteTime(v) + kCFAbsoluteTimeIntervalSince1970), tmp, sizeof tmp);
        append(buf, n, tmp);
    } else if (t == CGImageMetadataTagGetTypeID()) {
        CFTypeRef inner = CGImageMetadataTagCopyValue((CGImageMetadataTagRef)v);
        if (inner) {
            fmt_value(inner, buf, n);
            CFRelease(inner);
        }
    } else {
        CFStringRef d = CFCopyDescription(v);
        cfstr(d, tmp, sizeof tmp);
        CFRelease(d);
        append(buf, n, tmp);
    }
}

/* ------------------------------------------------------------------ */
/* naming and value formatting for well-known keys                     */
/* ------------------------------------------------------------------ */

static const char *FRIENDLY[][2] = {
    /* image */
    { "PixelWidth", "Width" }, { "PixelHeight", "Height" }, { "DPIWidth", "Horizontal Resolution" },
    { "DPIHeight", "Vertical Resolution" }, { "Depth", "Bit Depth" }, { "ColorModel", "Color Model" },
    { "ProfileName", "Color Profile" }, { "HasAlpha", "Alpha Channel" }, { "IsFloat", "Floating Point" },
    { "PrimaryImage", "Primary Image" }, { "IsIndexed", "Indexed Color" },
    /* exif */
    { "FNumber", "Aperture" }, { "ExposureTime", "Shutter Speed" }, { "ISOSpeedRatings", "ISO" },
    { "FocalLenIn35mmFilm", "Focal Length (35 mm equiv.)" }, { "ExposureBiasValue", "Exposure Compensation" },
    { "ApertureValue", "Aperture Value" }, { "MaxApertureValue", "Max Aperture" },
    { "ShutterSpeedValue", "Shutter Speed Value" }, { "BrightnessValue", "Brightness Value" },
    { "DateTimeOriginal", "Date Taken" }, { "DateTimeDigitized", "Date Digitized" },
    { "OffsetTimeOriginal", "Time Zone (Taken)" }, { "OffsetTimeDigitized", "Time Zone (Digitized)" },
    { "OffsetTime", "Time Zone" }, { "SubsecTimeOriginal", "Subseconds (Taken)" },
    { "SubsecTimeDigitized", "Subseconds (Digitized)" }, { "SubsecTime", "Subseconds" },
    { "PixelXDimension", "Pixel Width" }, { "PixelYDimension", "Pixel Height" },
    { "ExifVersion", "Exif Version" }, { "FlashPixVersion", "FlashPix Version" },
    { "ComponentsConfiguration", "Components" }, { "LensSpecification", "Lens Specification" },
    { "SubjectDistance", "Subject Distance" }, { "SubjectDistRange", "Subject Distance Range" },
    /* tiff */
    { "DateTime", "Date Modified" }, { "XResolution", "Horizontal Resolution" },
    { "YResolution", "Vertical Resolution" }, { "HostComputer", "Host Computer" },
    /* exif aux */
    { "SerialNumber", "Serial Number" }, { "LensID", "Lens ID" }, { "LensInfo", "Lens Info" },
    /* gps */
    { "ImgDirection", "Image Direction" }, { "HPositioningError", "Horizontal Accuracy" },
    { "DestBearing", "Destination Bearing" }, { "TimeStamp", "Time (UTC)" }, { "DateStamp", "Date (UTC)" },
    { "GPSVersion", "GPS Version" },
    /* iptc */
    { "ObjectName", "Title" }, { "Caption/Abstract", "Caption" }, { "Byline", "Author" },
    { "BylineTitle", "Author Title" }, { "CopyrightNotice", "Copyright" },
    { "Province/State", "State / Province" }, { "Country/PrimaryLocationName", "Country" },
    { "Country/PrimaryLocationCode", "Country Code" }, { "Writer/Editor", "Writer / Editor" },
    { "Sub-location", "Sub-location" },
    /* png */
    { "sRGBIntent", "sRGB Rendering Intent" },
    { NULL, NULL }
};

static void friendly_name(const char *key, char *buf, size_t n)
{
    char bare[160];
    size_t kl = strlen(key);
    if (kl > 2 && key[0] == '{' && key[kl - 1] == '}') {
        snprintf(bare, sizeof bare, "%.*s", (int)(kl - 2), key + 1);
        key = bare;
    }
    for (int i = 0; FRIENDLY[i][0]; i++)
        if (strcmp(FRIENDLY[i][0], key) == 0) {
            snprintf(buf, n, "%s", FRIENDLY[i][1]);
            return;
        }
    int numeric = key[0] != 0;
    for (const char *p = key; *p; p++)
        if (!isdigit((unsigned char)*p)) numeric = 0;
    if (numeric) snprintf(buf, n, "Tag %s", key);
    else split_camel(key, buf, n);
}

typedef struct { const char *key; int val; const char *label; } EnumName;

static const EnumName ENUMS[] = {
    { "Orientation", 1, "Normal" }, { "Orientation", 2, "Mirrored horizontally" },
    { "Orientation", 3, "Rotated 180°" }, { "Orientation", 4, "Mirrored vertically" },
    { "Orientation", 5, "Mirrored, rotated 270° CW" }, { "Orientation", 6, "Rotated 90° CW" },
    { "Orientation", 7, "Mirrored, rotated 90° CW" }, { "Orientation", 8, "Rotated 90° CCW" },
    { "MeteringMode", 0, "Unknown" }, { "MeteringMode", 1, "Average" },
    { "MeteringMode", 2, "Center-weighted average" }, { "MeteringMode", 3, "Spot" },
    { "MeteringMode", 4, "Multi-spot" }, { "MeteringMode", 5, "Pattern (matrix)" },
    { "MeteringMode", 6, "Partial" }, { "MeteringMode", 255, "Other" },
    { "ExposureProgram", 0, "Not defined" }, { "ExposureProgram", 1, "Manual" },
    { "ExposureProgram", 2, "Normal program" }, { "ExposureProgram", 3, "Aperture priority" },
    { "ExposureProgram", 4, "Shutter priority" }, { "ExposureProgram", 5, "Creative (depth of field)" },
    { "ExposureProgram", 6, "Action (fast shutter)" }, { "ExposureProgram", 7, "Portrait" },
    { "ExposureProgram", 8, "Landscape" },
    { "WhiteBalance", 0, "Auto" }, { "WhiteBalance", 1, "Manual" },
    { "ExposureMode", 0, "Auto" }, { "ExposureMode", 1, "Manual" }, { "ExposureMode", 2, "Auto bracket" },
    { "SceneCaptureType", 0, "Standard" }, { "SceneCaptureType", 1, "Landscape" },
    { "SceneCaptureType", 2, "Portrait" }, { "SceneCaptureType", 3, "Night scene" },
    { "SensingMethod", 1, "Not defined" }, { "SensingMethod", 2, "One-chip color area" },
    { "SensingMethod", 3, "Two-chip color area" }, { "SensingMethod", 4, "Three-chip color area" },
    { "SensingMethod", 5, "Color sequential area" }, { "SensingMethod", 7, "Trilinear" },
    { "SensingMethod", 8, "Color sequential linear" },
    { "ColorSpace", 1, "sRGB" }, { "ColorSpace", 65535, "Uncalibrated" },
    { "ResolutionUnit", 1, "None" }, { "ResolutionUnit", 2, "Inches" }, { "ResolutionUnit", 3, "Centimeters" },
    { "FocalPlaneResolutionUnit", 2, "Inches" }, { "FocalPlaneResolutionUnit", 3, "Centimeters" },
    { "Contrast", 0, "Normal" }, { "Contrast", 1, "Low" }, { "Contrast", 2, "High" },
    { "Saturation", 0, "Normal" }, { "Saturation", 1, "Low" }, { "Saturation", 2, "High" },
    { "Sharpness", 0, "Normal" }, { "Sharpness", 1, "Soft" }, { "Sharpness", 2, "Hard" },
    { "CustomRendered", 0, "Normal" }, { "CustomRendered", 1, "Custom" },
    { "GainControl", 0, "None" }, { "GainControl", 1, "Low gain up" }, { "GainControl", 2, "High gain up" },
    { "GainControl", 3, "Low gain down" }, { "GainControl", 4, "High gain down" },
    { "SubjectDistRange", 0, "Unknown" }, { "SubjectDistRange", 1, "Macro" },
    { "SubjectDistRange", 2, "Close" }, { "SubjectDistRange", 3, "Distant" },
    { "LightSource", 0, "Unknown" }, { "LightSource", 1, "Daylight" }, { "LightSource", 2, "Fluorescent" },
    { "LightSource", 3, "Tungsten" }, { "LightSource", 4, "Flash" }, { "LightSource", 9, "Fine weather" },
    { "LightSource", 10, "Cloudy" }, { "LightSource", 11, "Shade" }, { "LightSource", 255, "Other" },
    { "SceneType", 1, "Directly photographed" },
    { "sRGBIntent", 0, "Perceptual" }, { "sRGBIntent", 1, "Relative colorimetric" },
    { "sRGBIntent", 2, "Saturation" }, { "sRGBIntent", 3, "Absolute colorimetric" },
    { "InterlaceType", 0, "None" }, { "InterlaceType", 1, "Adam7" },
    { "PhotometricInterpretation", 0, "White is zero" }, { "PhotometricInterpretation", 1, "Black is zero" },
    { "PhotometricInterpretation", 2, "RGB" }, { "PhotometricInterpretation", 3, "Palette" },
    { "PhotometricInterpretation", 5, "CMYK" }, { "PhotometricInterpretation", 6, "YCbCr" },
    { "PhotometricInterpretation", 32803, "Color filter array" },
    { "PhotometricInterpretation", 34892, "Linear raw" },
    { "DensityUnit", 0, "None (aspect ratio only)" }, { "DensityUnit", 1, "Pixels per inch" },
    { "DensityUnit", 2, "Pixels per centimeter" },
    { "FileSource", 3, "Digital camera" },
    { "CompositeImage", 0, "Unknown" }, { "CompositeImage", 1, "Not composite" },
    { "CompositeImage", 2, "Composite" }, { "CompositeImage", 3, "Composite (captured while shooting)" },
    { NULL, 0, NULL }
};

static void fmt_shutter(double t, char *buf, size_t n)
{
    char num[32];
    if (t > 0 && t < 0.5) {
        snprintf(buf, n, "1/%.0f s", 1.0 / t);
    } else {
        fmtnum(t, 1, num, sizeof num);
        snprintf(buf, n, "%s s", num);
    }
}

static void fmt_fnum(double f, char *buf, size_t n)
{
    char num[32];
    fmtnum(f, 1, num, sizeof num);
    snprintf(buf, n, "ƒ/%s", num);
}

/* Standard-tag decoding does not apply to vendor maker notes, which reuse names freely. */
static int is_standard_section(const char *section)
{
    return strstr(section, "Maker") == NULL;
}

/* Format keys we understand. Returns 1 if handled. d is the containing dict. */
static int fmt_known(const char *section, const char *key, CFDictionaryRef d, CFTypeRef v, char *buf, size_t n)
{
    double x;
    char num[64], s[VALUE_MAX];
    int gps = !strcmp(section, "{GPS}");
    size_t klen = strlen(key);

    /* any "...Version" stored as [1, 0, 1] -> "1.0.1" */
    if (klen > 7 && !strcmp(key + klen - 7, "Version") && CFGetTypeID(v) == CFArrayGetTypeID()) {
        buf[0] = 0;
        for (CFIndex i = 0; i < CFArrayGetCount(v); i++) {
            if (i) append(buf, n, ".");
            fmt_value(CFArrayGetValueAtIndex(v, i), buf, n);
        }
        return 1;
    }
    if ((!strcmp(key, "StarRating") || !strcmp(key, "Rating")) && as_num(v, &x) && x >= 0 && x <= 5) {
        buf[0] = 0;
        for (int i = 0; i < 5; i++) append(buf, n, i < (int)x ? "★" : "☆");
        return 1;
    }

    if (!strcmp(section, "{IPTC}") && CFGetTypeID(v) == CFStringGetTypeID()) {
        int y, mo, d, h, mi, se;
        cfstr(v, s, sizeof s);
        if (strstr(key, "Date") && strlen(s) == 8 &&
            sscanf(s, "%4d%2d%2d", &y, &mo, &d) == 3) {
            snprintf(buf, n, "%04d-%02d-%02d", y, mo, d);
            return 1;
        }
        if (strstr(key, "Time") && strlen(s) >= 6 &&
            sscanf(s, "%2d%2d%2d", &h, &mi, &se) == 3) {
            snprintf(buf, n, "%02d:%02d:%02d%s%s", h, mi, se, s[6] ? " " : "", s + 6);
            return 1;
        }
        return 0;
    }

    if (gps) {
        if ((!strcmp(key, "Latitude") || !strcmp(key, "Longitude")) && as_num(v, &x)) {
            char ref[8];
            dstr(d, !strcmp(key, "Latitude") ? "LatitudeRef" : "LongitudeRef", ref, sizeof ref);
            snprintf(buf, n, "%.6f°%s%s", x, ref[0] ? " " : "", ref);
            return 1;
        }
        if (!strcmp(key, "Altitude") && as_num(v, &x)) {
            double ref = 0;
            dnum(d, "AltitudeRef", &ref);
            fmtnum(x, 1, num, sizeof num);
            snprintf(buf, n, "%s m %s", num, ref == 1 ? "below sea level" : "above sea level");
            return 1;
        }
        if (!strcmp(key, "HPositioningError") && as_num(v, &x)) {
            fmtnum(x, 1, num, sizeof num);
            snprintf(buf, n, "± %s m", num);
            return 1;
        }
        if ((!strcmp(key, "ImgDirection") || !strcmp(key, "DestBearing") || !strcmp(key, "Track")) &&
            as_num(v, &x)) {
            char ref[8], refkey[32];
            snprintf(refkey, sizeof refkey, "%sRef", key);
            dstr(d, refkey, ref, sizeof ref);
            fmtnum(x, 1, num, sizeof num);
            snprintf(buf, n, "%s°%s", num, !strcmp(ref, "T") ? " (true north)" : !strcmp(ref, "M") ? " (magnetic north)" : "");
            return 1;
        }
        if (!strcmp(key, "Speed") && as_num(v, &x)) {
            char ref[8];
            dstr(d, "SpeedRef", ref, sizeof ref);
            fmtnum(x, 2, num, sizeof num);
            snprintf(buf, n, "%s %s", num, !strcmp(ref, "M") ? "mph" : !strcmp(ref, "N") ? "knots" : "km/h");
            return 1;
        }
        return 0;
    }

    if (!is_standard_section(section))
        return 0;

    for (int i = 0; ENUMS[i].key; i++)
        if (!strcmp(ENUMS[i].key, key) && as_num(v, &x)) {
            for (int j = i; ENUMS[j].key && !strcmp(ENUMS[j].key, key); j++)
                if (ENUMS[j].val == (int)x) {
                    snprintf(buf, n, "%s", ENUMS[j].label);
                    return 1;
                }
            return 0;
        }

    if (!strcmp(key, "ExposureTime") && as_num(v, &x)) { fmt_shutter(x, buf, n); return 1; }
    if (!strcmp(key, "ShutterSpeedValue") && as_num(v, &x)) {
        fmt_shutter(pow(2.0, -x), s, sizeof s);
        fmtnum(x, 2, num, sizeof num);
        snprintf(buf, n, "%s (APEX %s)", s, num);
        return 1;
    }
    if (!strcmp(key, "FNumber") && as_num(v, &x)) { fmt_fnum(x, buf, n); return 1; }
    if ((!strcmp(key, "ApertureValue") || !strcmp(key, "MaxApertureValue")) && as_num(v, &x)) {
        fmt_fnum(pow(2.0, x / 2.0), s, sizeof s);
        fmtnum(x, 2, num, sizeof num);
        snprintf(buf, n, "%s (APEX %s)", s, num);
        return 1;
    }
    if (!strcmp(key, "FocalLength") && as_num(v, &x)) {
        fmtnum(x, 2, num, sizeof num);
        snprintf(buf, n, "%s mm", num);
        return 1;
    }
    if (!strcmp(key, "FocalLenIn35mmFilm") && as_num(v, &x)) { snprintf(buf, n, "%.0f mm", x); return 1; }
    if (!strcmp(key, "SubjectDistance") && as_num(v, &x)) {
        fmtnum(x, 2, num, sizeof num);
        snprintf(buf, n, "%s m", num);
        return 1;
    }
    if (!strcmp(key, "ExposureBiasValue") && as_num(v, &x)) {
        fmtnum(fabs(x), 2, num, sizeof num);
        snprintf(buf, n, "%s%s EV", x > 0 ? "+" : x < 0 ? "−" : "", num);
        return 1;
    }
    if (!strcmp(key, "ISOSpeedRatings")) {
        buf[0] = 0;
        append(buf, n, "ISO ");
        fmt_value(v, buf, n);
        return 1;
    }
    if (!strcmp(key, "Flash") && as_num(v, &x)) {
        int f = (int)x, mode = (f >> 3) & 3;
        if (f & 0x20) { snprintf(buf, n, "No flash function"); return 1; }
        snprintf(buf, n, "%s", (f & 1) ? "Fired" : "Did not fire");
        if (mode == 1) append(buf, n, ", compulsory");
        if (mode == 2) append(buf, n, ", flash off");
        if (mode == 3) append(buf, n, ", auto");
        if (f & 0x40) append(buf, n, ", red-eye reduction");
        return 1;
    }
    if ((!strcmp(key, "PixelWidth") || !strcmp(key, "PixelHeight") || !strcmp(key, "PixelXDimension") ||
         !strcmp(key, "PixelYDimension")) && as_num(v, &x)) {
        thousands((long long)x, num, sizeof num);
        snprintf(buf, n, "%s px", num);
        return 1;
    }
    if ((!strcmp(key, "DPIWidth") || !strcmp(key, "DPIHeight")) && as_num(v, &x)) {
        fmtnum(x, 1, num, sizeof num);
        snprintf(buf, n, "%s dpi", num);
        return 1;
    }
    if ((!strcmp(key, "XResolution") || !strcmp(key, "YResolution")) && as_num(v, &x)) {
        double unit = 2;
        dnum(d, "ResolutionUnit", &unit);
        fmtnum(x, 1, num, sizeof num);
        snprintf(buf, n, "%s%s", num, unit == 2 ? " dpi" : unit == 3 ? " dpcm" : "");
        return 1;
    }
    if (!strcmp(key, "Depth") && as_num(v, &x)) { snprintf(buf, n, "%.0f bits per channel", x); return 1; }
    if ((!strcmp(key, "DateTimeOriginal") || !strcmp(key, "DateTimeDigitized") || !strcmp(key, "DateTime")) &&
        CFGetTypeID(v) == CFStringGetTypeID()) {
        cfstr(v, s, sizeof s);
        return fmt_exif_date(s, buf, n);
    }
    if (!strcmp(key, "LensSpecification") && CFGetTypeID(v) == CFArrayGetTypeID() && CFArrayGetCount(v) == 4) {
        double a[4];
        for (int i = 0; i < 4; i++)
            if (!as_num(CFArrayGetValueAtIndex(v, i), &a[i])) return 0;
        char f1[32], f2[32], a1[32], a2[32];
        fmtnum(a[0], 2, f1, sizeof f1); fmtnum(a[1], 2, f2, sizeof f2);
        fmtnum(a[2], 1, a1, sizeof a1); fmtnum(a[3], 1, a2, sizeof a2);
        buf[0] = 0;
        if (a[0] == a[1]) appendf(buf, n, "%s mm", f1);
        else appendf(buf, n, "%s–%s mm", f1, f2);
        if (a[2] > 0) {
            if (a[2] == a[3] || a[3] <= 0) appendf(buf, n, " ƒ/%s", a1);
            else appendf(buf, n, " ƒ/%s–%s", a1, a2);
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* sections                                                            */
/* ------------------------------------------------------------------ */

static const char *PRIORITY[] = {
    "PixelWidth", "PixelHeight", "Orientation", "ColorModel", "ProfileName", "Depth", "DPIWidth", "DPIHeight",
    "HasAlpha",
    "Make", "Model", "Software", "HostComputer", "DateTime", "Artist", "Copyright",
    "DateTimeOriginal", "OffsetTimeOriginal", "DateTimeDigitized", "ExposureTime", "FNumber", "ISOSpeedRatings",
    "FocalLength", "FocalLenIn35mmFilm", "ExposureBiasValue", "ExposureProgram", "ExposureMode", "MeteringMode",
    "WhiteBalance", "Flash", "LensMake", "LensModel", "LensSpecification",
    "Latitude", "Longitude", "Altitude", "HPositioningError", "Speed", "ImgDirection", "DestBearing",
    "DateStamp", "TimeStamp",
    "ObjectName", "Caption/Abstract", "Keywords", "Byline", "CopyrightNotice", "City", "Province/State",
    "Country/PrimaryLocationName",
    NULL
};

/* GPS reference keys are merged into the value they qualify. */
static const char *GPS_MERGED[] = { "LatitudeRef", "LongitudeRef", "AltitudeRef", "SpeedRef", "ImgDirectionRef",
                                    "DestBearingRef", "TrackRef", NULL };

typedef struct {
    CFStringRef cfkey;
    char key[128];
    char name[160];
    int prio;
} Entry;

static int priority_of(const char *key)
{
    for (int i = 0; PRIORITY[i]; i++)
        if (!strcmp(PRIORITY[i], key)) return i;
    return 1000;
}

static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    if (x->prio != y->prio) return x->prio - y->prio;
    if (isdigit((unsigned char)x->key[0]) && isdigit((unsigned char)y->key[0]))
        return atoi(x->key) - atoi(y->key);
    return strcasecmp(x->name, y->name);
}

static int in_list(const char **list, const char *s)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}

/*
 * Emit every entry of d as rows. Nested dictionaries are flattened with a
 * "Parent › Child" prefix. When skip_dicts is set, dictionary values are
 * left for the caller (top-level property dict).
 */
static void emit_dict(Meta *m, const char *section, CFDictionaryRef d, const char *prefix, int skip_dicts,
                      const char **skip_keys)
{
    CFIndex c = CFDictionaryGetCount(d);
    if (c == 0) return;
    const void **keys = malloc(sizeof(void *) * (size_t)c);
    Entry *e = calloc((size_t)c, sizeof *e);
    CFDictionaryGetKeysAndValues(d, keys, NULL);

    CFIndex ne = 0;
    for (CFIndex i = 0; i < c; i++) {
        if (CFGetTypeID(keys[i]) != CFStringGetTypeID()) continue;
        e[ne].cfkey = keys[i];
        cfstr(keys[i], e[ne].key, sizeof e[ne].key);
        friendly_name(e[ne].key, e[ne].name, sizeof e[ne].name);
        e[ne].prio = priority_of(e[ne].key);
        ne++;
    }
    qsort(e, (size_t)ne, sizeof *e, cmp_entry);

    for (CFIndex i = 0; i < ne; i++) {
        if (skip_keys && in_list(skip_keys, e[i].key)) continue;
        if (!strcmp(section, "{GPS}") && in_list(GPS_MERGED, e[i].key)) continue;

        CFTypeRef v = CFDictionaryGetValue(d, e[i].cfkey);
        char label[320];
        snprintf(label, sizeof label, "%s%s", prefix, e[i].name);

        if (CFGetTypeID(v) == CFDictionaryGetTypeID()) {
            if (skip_dicts) continue;
            char sub[340];
            snprintf(sub, sizeof sub, "%s › ", label);
            emit_dict(m, section, v, sub, 0, NULL);
            continue;
        }

        char val[VALUE_MAX] = "";
        if (!fmt_known(section, e[i].key, d, v, val, sizeof val)) {
            val[0] = 0;
            fmt_value(v, val, sizeof val);
        }
        add_item(m, label, "%s", val);
    }
    free(e);
    free(keys);
}

typedef struct { const char *key; const char *title; const char *icon; int order; } SectionInfo;

static const SectionInfo SECTIONS[] = {
    { "{TIFF}", "Camera & Software", "camera", 10 },
    { "{Exif}", "Exposure & Capture", "camera.aperture", 20 },
    { "{ExifAux}", "Lens & Body", "scope", 30 },
    { "{GPS}", "Location", "location", 40 },
    { "{IPTC}", "Description (IPTC)", "text.quote", 50 },
    { "{8BIM}", "Photoshop", "paintbrush", 60 },
    { "{DNG}", "DNG", "camera.filters", 60 },
    { "{CIFF}", "Canon CIFF", "camera.filters", 60 },
    { "{Raw}", "RAW", "camera.filters", 60 },
    { "{HEIF}", "HEIF", "photo.stack", 60 },
    { "{PNG}", "PNG", "photo", 60 },
    { "{JFIF}", "JFIF", "photo", 60 },
    { "{GIF}", "GIF", "photo.stack", 60 },
    { "{WebP}", "WebP", "photo", 60 },
    { "{TGA}", "TGA", "photo", 60 },
    { "{OpenEXR}", "OpenEXR", "photo", 60 },
    { NULL, NULL, NULL, 0 }
};

typedef struct {
    CFStringRef cfkey;
    char key[64];
    char title[128];
    const char *icon;
    int order;
} SubDict;

static int cmp_subdict(const void *a, const void *b)
{
    const SubDict *x = a, *y = b;
    if (x->order != y->order) return x->order - y->order;
    return strcasecmp(x->title, y->title);
}

static void describe_subdict(SubDict *s)
{
    for (int i = 0; SECTIONS[i].key; i++)
        if (!strcmp(SECTIONS[i].key, s->key)) {
            snprintf(s->title, sizeof s->title, "%s", SECTIONS[i].title);
            s->icon = SECTIONS[i].icon;
            s->order = SECTIONS[i].order;
            return;
        }
    /* strip braces */
    char bare[64];
    snprintf(bare, sizeof bare, "%s", s->key + (s->key[0] == '{'));
    size_t l = strlen(bare);
    if (l && bare[l - 1] == '}') bare[l - 1] = 0;

    if (!strncmp(bare, "Maker", 5) && bare[5]) {
        snprintf(s->title, sizeof s->title, "%s Maker Notes", bare + 5);
        s->icon = "wrench.and.screwdriver";
        s->order = 90;
    } else {
        snprintf(s->title, sizeof s->title, "%s", bare);
        s->icon = "square.stack.3d.up";
        s->order = 70;
    }
}

static void emit_subdicts(Meta *m, CFDictionaryRef props, int max_order)
{
    CFIndex c = CFDictionaryGetCount(props);
    const void **keys = malloc(sizeof(void *) * (size_t)c);
    const void **vals = malloc(sizeof(void *) * (size_t)c);
    SubDict *subs = calloc((size_t)c, sizeof *subs);
    CFDictionaryGetKeysAndValues(props, keys, vals);

    size_t ns = 0;
    for (CFIndex i = 0; i < c; i++) {
        if (CFGetTypeID(vals[i]) != CFDictionaryGetTypeID() || CFGetTypeID(keys[i]) != CFStringGetTypeID())
            continue;
        subs[ns].cfkey = keys[i];
        cfstr(keys[i], subs[ns].key, sizeof subs[ns].key);
        describe_subdict(&subs[ns]);
        ns++;
    }
    qsort(subs, ns, sizeof *subs, cmp_subdict);

    for (size_t i = 0; i < ns; i++) {
        if (subs[i].order > max_order) continue;
        begin_section(m, subs[i].title, subs[i].icon);
        emit_dict(m, subs[i].key, CFDictionaryGetValue(props, subs[i].cfkey), "", 0, NULL);
        end_section(m);
    }
    free(subs);
    free(vals);
    free(keys);
}

/* XMP namespaces that duplicate the Exif/TIFF dictionaries, or are ImageIO-internal flags. */
static const char *XMP_SKIPPED_PREFIXES[] = { "exif", "exifEX", "tiff", "aux", "iio", NULL };

static void emit_xmp(Meta *m, CGImageSourceRef src)
{
    CGImageMetadataRef md = CGImageSourceCopyMetadataAtIndex(src, 0, NULL);
    if (!md) return;

    begin_section(m, "XMP", "tag");
    CGImageMetadataEnumerateTagsUsingBlock(md, NULL, NULL, ^bool(CFStringRef path, CGImageMetadataTagRef tag) {
        (void)path;
        char prefix[64] = "", name[160] = "", friendly[200], label[280], val[VALUE_MAX] = "";
        CFStringRef p = CGImageMetadataTagCopyPrefix(tag);
        CFStringRef nm = CGImageMetadataTagCopyName(tag);
        if (p) { cfstr(p, prefix, sizeof prefix); CFRelease(p); }
        if (nm) { cfstr(nm, name, sizeof name); CFRelease(nm); }
        if (in_list(XMP_SKIPPED_PREFIXES, prefix)) return true;

        friendly_name(name, friendly, sizeof friendly);
        if (prefix[0]) snprintf(label, sizeof label, "%s (%s)", friendly, prefix);
        else snprintf(label, sizeof label, "%s", friendly);
        fmt_value(tag, val, sizeof val);
        add_item(m, label, "%s", val);
        return true;
    });
    end_section(m);
    CFRelease(md);
}

/* ------------------------------------------------------------------ */
/* summary                                                             */
/* ------------------------------------------------------------------ */

static void build_summary(Meta *m, CFDictionaryRef props)
{
    MetaSummary *s = &m->summary;
    CFDictionaryRef exif = ddict(props, "{Exif}");
    CFDictionaryRef tiff = ddict(props, "{TIFF}");
    CFDictionaryRef aux = ddict(props, "{ExifAux}");
    CFDictionaryRef gps = ddict(props, "{GPS}");
    char a[256], b[256];
    double x;

    /* dimensions, as displayed (respecting orientation) */
    double w = 0, h = 0, orient = 1;
    if (dnum(props, "PixelWidth", &w) && dnum(props, "PixelHeight", &h)) {
        dnum(props, "Orientation", &orient);
        if (orient >= 5) { double t = w; w = h; h = t; }
        char mp[32];
        fmtnum(w * h / 1e6, 1, mp, sizeof mp);
        snprintf(s->dims, sizeof s->dims, "%.0f × %.0f  ·  %s MP", w, h, mp);
    }

    /* camera: avoid "Canon Canon EOS R5" */
    if (dstr(tiff, "Model", b, sizeof b)) {
        if (dstr(tiff, "Make", a, sizeof a) && strncasecmp(b, a, strlen(a)) != 0)
            snprintf(s->camera, sizeof s->camera, "%s %s", a, b);
        else
            snprintf(s->camera, sizeof s->camera, "%s", b);
    }

    if (!dstr(exif, "LensModel", s->lens, sizeof s->lens))
        dstr(aux, "LensModel", s->lens, sizeof s->lens);

    /* exposure triangle */
    char tmp[64];
    if (dnum(exif, "FNumber", &x)) {
        fmt_fnum(x, tmp, sizeof tmp);
        appendf(s->exposure, sizeof s->exposure, "%s", tmp);
    }
    if (dnum(exif, "ExposureTime", &x)) {
        fmt_shutter(x, tmp, sizeof tmp);
        appendf(s->exposure, sizeof s->exposure, "%s%s", s->exposure[0] ? "  ·  " : "", tmp);
    }
    if (dnum(exif, "ISOSpeedRatings", &x))
        appendf(s->exposure, sizeof s->exposure, "%sISO %.0f", s->exposure[0] ? "  ·  " : "", x);
    if (dnum(exif, "FocalLenIn35mmFilm", &x) || dnum(exif, "FocalLength", &x)) {
        fmtnum(x, 1, tmp, sizeof tmp);
        appendf(s->exposure, sizeof s->exposure, "%s%s mm", s->exposure[0] ? "  ·  " : "", tmp);
    }

    if ((dstr(exif, "DateTimeOriginal", a, sizeof a) || dstr(tiff, "DateTime", a, sizeof a)) &&
        !fmt_exif_date(a, s->date, sizeof s->date))
        snprintf(s->date, sizeof s->date, "%s", a);

    double lat, lon;
    if (dnum(gps, "Latitude", &lat) && dnum(gps, "Longitude", &lon)) {
        char latref[8] = "N", lonref[8] = "E";
        dstr(gps, "LatitudeRef", latref, sizeof latref);
        dstr(gps, "LongitudeRef", lonref, sizeof lonref);
        s->has_gps = 1;
        s->lat = (latref[0] == 'S') ? -lat : lat;
        s->lon = (lonref[0] == 'W') ? -lon : lon;
        snprintf(s->location, sizeof s->location, "%.5f° %s,  %.5f° %s", lat, latref[0] ? latref : "N", lon,
                 lonref[0] ? lonref : "E");
        double alt;
        if (dnum(gps, "Altitude", &alt))
            appendf(s->location, sizeof s->location, "  ·  %.0f m", alt);
    }
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

static void type_description(CFStringRef uti, char *buf, size_t n)
{
    buf[0] = 0;
    id type = MSG(id, id)(cls("UTType"), sel("typeWithIdentifier:"), (id)uti);
    id desc = type ? msg(type, "localizedDescription") : NULL;
    if (desc) cfstr((CFStringRef)desc, buf, n);
    if (buf[0]) buf[0] = (char)toupper((unsigned char)buf[0]);
}

static void emit_file_section(Meta *m, const char *path, const struct stat *st, CGImageSourceRef src)
{
    char tmp[1024], tmp2[512];
    MetaSummary *s = &m->summary;

    begin_section(m, "File", "doc");

    char pathcopy[PATH_MAX];
    snprintf(pathcopy, sizeof pathcopy, "%s", path);
    snprintf(s->name, sizeof s->name, "%s", basename(pathcopy));
    add_item(m, "Name", "%s", s->name);

    snprintf(pathcopy, sizeof pathcopy, "%s", path);
    const char *dir = dirname(pathcopy), *home = getenv("HOME");
    if (home && home[0] && !strncmp(dir, home, strlen(home)) &&
        (dir[strlen(home)] == '/' || dir[strlen(home)] == 0))
        add_item(m, "Folder", "~%s", dir + strlen(home));
    else
        add_item(m, "Folder", "%s", dir);

    CFStringRef uti = CGImageSourceGetType(src);
    if (uti) {
        type_description(uti, s->kind, sizeof s->kind);
        cfstr(uti, tmp, sizeof tmp);
        add_item(m, "Kind", "%s", s->kind[0] ? s->kind : tmp);
        add_item(m, "Type Identifier", "%s", tmp);
    }

    human_size((long long)st->st_size, s->size, sizeof s->size);
    thousands((long long)st->st_size, tmp2, sizeof tmp2);
    add_item(m, "Size", "%s (%s bytes)", s->size, tmp2);

    fmt_time(st->st_birthtimespec.tv_sec, tmp, sizeof tmp);
    add_item(m, "Created", "%s", tmp);
    fmt_time(st->st_mtimespec.tv_sec, tmp, sizeof tmp);
    add_item(m, "Modified", "%s", tmp);

    char perms[11];
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        perms[i] = (st->st_mode & (1 << (8 - i))) ? rwx[i] : '-';
    perms[9] = 0;
    add_item(m, "Permissions", "%s", perms);

    size_t count = CGImageSourceGetCount(src);
    if (count > 1)
        add_item(m, "Images in File", "%zu", count);

    end_section(m);
}

static void emit_image_section(Meta *m, CFDictionaryRef props)
{
    static const char *skip[] = { "PixelWidth", "PixelHeight", NULL };
    double w, h;

    begin_section(m, "Image", "photo");
    if (dnum(props, "PixelWidth", &w) && dnum(props, "PixelHeight", &h)) {
        char a[32], b[32], mp[32];
        thousands((long long)w, a, sizeof a);
        thousands((long long)h, b, sizeof b);
        fmtnum(w * h / 1e6, 1, mp, sizeof mp);
        add_item(m, "Dimensions", "%s × %s px (%s megapixels)", a, b, mp);

        long long x = (long long)w, y = (long long)h;
        while (y) { long long t = x % y; x = y; y = t; }
        if (x > 0 && w / x <= 32 && h / x <= 32)
            add_item(m, "Aspect Ratio", "%.0f:%.0f", w / x, h / x);
        else if (h > 0) {
            fmtnum(w / h, 2, a, sizeof a);
            add_item(m, "Aspect Ratio", "%s:1", a);
        }
    }
    emit_dict(m, "", props, "", 1, skip);
    end_section(m);
}

static CGImageRef make_thumbnail(CGImageSourceRef src)
{
    int max = 900;
    CFNumberRef maxn = CFNumberCreate(NULL, kCFNumberIntType, &max);
    const void *keys[] = { kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceCreateThumbnailWithTransform,
                           kCGImageSourceThumbnailMaxPixelSize };
    const void *vals[] = { kCFBooleanTrue, kCFBooleanTrue, maxn };
    CFDictionaryRef opts = CFDictionaryCreate(NULL, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CGImageRef img = CGImageSourceCreateThumbnailAtIndex(src, 0, opts);
    CFRelease(opts);
    CFRelease(maxn);
    return img;
}

int meta_load(const char *path, Meta *m, char *err, size_t errlen)
{
    memset(m, 0, sizeof *m);

    char abspath[PATH_MAX];
    if (realpath(path, abspath)) path = abspath;

    struct stat st;
    if (stat(path, &st) != 0) {
        snprintf(err, errlen, "The file could not be opened.");
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        snprintf(err, errlen, "This is not a regular file.");
        return -1;
    }

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageSourceRef src = url ? CGImageSourceCreateWithURL(url, NULL) : NULL;
    if (url) CFRelease(url);
    if (!src || CGImageSourceGetStatus(src) != kCGImageStatusComplete || CGImageSourceGetCount(src) == 0) {
        if (src) CFRelease(src);
        snprintf(err, errlen, "The file isn’t a photo in a format macOS can read.");
        return -1;
    }

    CFDictionaryRef props = CGImageSourceCopyPropertiesAtIndex(src, 0, NULL);
    CFDictionaryRef container = CGImageSourceCopyProperties(src, NULL);
    if (!props) props = CFDictionaryCreate(NULL, NULL, NULL, 0, NULL, NULL);

    emit_file_section(m, path, &st, src);
    emit_image_section(m, props);
    emit_subdicts(m, props, 80);               /* standard dictionaries; maker notes come last */

    if (container) {                            /* file-level (container) properties */
        static const char *skip[] = { "FileSize", NULL };
        begin_section(m, "Container", "shippingbox");
        emit_dict(m, "container", container, "", 0, skip);
        end_section(m);
    }

    emit_xmp(m, src);
    {
        /* maker notes last */
        CFIndex c = CFDictionaryGetCount(props);
        const void **keys = malloc(sizeof(void *) * (size_t)c);
        CFDictionaryGetKeysAndValues(props, keys, NULL);
        for (CFIndex i = 0; i < c; i++) {
            SubDict sd = { 0 };
            if (CFGetTypeID(keys[i]) != CFStringGetTypeID()) continue;
            cfstr(keys[i], sd.key, sizeof sd.key);
            CFTypeRef v = CFDictionaryGetValue(props, keys[i]);
            if (CFGetTypeID(v) != CFDictionaryGetTypeID()) continue;
            describe_subdict(&sd);
            if (sd.order <= 80) continue;
            begin_section(m, sd.title, sd.icon);
            emit_dict(m, sd.key, v, "", 0, NULL);
            end_section(m);
        }
        free(keys);
    }

    build_summary(m, props);
    m->thumbnail = make_thumbnail(src);

    CFRelease(props);
    if (container) CFRelease(container);
    CFRelease(src);
    return 0;
}

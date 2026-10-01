/*
 * meta.h — photo metadata extraction (ImageIO / CoreFoundation, no UI).
 */
#ifndef PHOTO_META_META_H
#define PHOTO_META_META_H

#include <stddef.h>
#include <CoreGraphics/CoreGraphics.h>

typedef enum { ROW_HEADER, ROW_ITEM } RowKind;

/* A flat, display-ready list: section headers followed by key/value items. */
typedef struct {
    RowKind kind;
    char *key;    /* item label, or section title for headers */
    char *value;  /* item value, or SF Symbol name for headers */
} MetaRow;

/* Highlights shown in the sidebar. Empty string = not available. */
typedef struct {
    char name[256];
    char kind[128];
    char size[64];
    char dims[128];
    char camera[256];
    char lens[256];
    char exposure[256];
    char date[128];
    char location[160];
    int has_gps;
    double lat, lon;
} MetaSummary;

typedef struct {
    MetaRow *rows;
    size_t count, cap;
    size_t items;          /* number of ROW_ITEM rows */
    MetaSummary summary;
    CGImageRef thumbnail;  /* oriented preview, may be NULL */
} Meta;

/* Returns 0 on success; on failure writes a human-readable reason to err. */
int meta_load(const char *path, Meta *m, char *err, size_t errlen);
void meta_free(Meta *m);

#endif

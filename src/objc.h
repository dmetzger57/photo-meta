/*
 * objc.h — tiny helpers for driving Cocoa from plain C via the Objective-C runtime.
 *
 * Every message send goes through objc_msgSend cast to the exact function
 * prototype of the method being called (required for correctness on arm64).
 * No method used by this program returns a struct, so objc_msgSend_stret is
 * never needed and the same code works on x86_64.
 *
 * Copyright (c) 2026 Dennis Metzger
 * SPDX-License-Identifier: MIT
 */
#ifndef PHOTO_META_OBJC_H
#define PHOTO_META_OBJC_H

#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreGraphics/CoreGraphics.h>

/* Cast objc_msgSend to a typed function pointer: MSG(ret, argtypes...)(obj, sel, args...) */
#define MSG(RET, ...) ((RET (*)(id, SEL, ##__VA_ARGS__))objc_msgSend)

#define cls(name) ((id)objc_getClass(name))
#define sel(name) sel_registerName(name)

/* Not in public headers, but exported by libobjc. */
extern void *objc_autoreleasePoolPush(void);
extern void objc_autoreleasePoolPop(void *pool);

static inline id msg(id o, const char *s) { return MSG(id)(o, sel(s)); }
static inline id msg_id(id o, const char *s, id a) { return MSG(id, id)(o, sel(s), a); }
static inline id msg_id2(id o, const char *s, id a, id b) { return MSG(id, id, id)(o, sel(s), a, b); }
static inline void msg_bool(id o, const char *s, BOOL v) { MSG(void, BOOL)(o, sel(s), v); }
static inline void msg_long(id o, const char *s, long v) { MSG(void, long)(o, sel(s), v); }
static inline void msg_dbl(id o, const char *s, double v) { MSG(void, double)(o, sel(s), v); }
static inline void msg_sel(id o, const char *s, SEL v) { MSG(void, SEL)(o, sel(s), v); }

static inline id new_obj(const char *klass) { return msg(cls(klass), "new"); }

static inline id nsstr(const char *s)
{
    return MSG(id, const char *)(cls("NSString"), sel("stringWithUTF8String:"), s ? s : "");
}

static inline const char *cstr(id nsstring)
{
    return nsstring ? MSG(const char *)(nsstring, sel("UTF8String")) : "";
}

#endif

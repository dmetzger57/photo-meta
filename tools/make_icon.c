/*
 * make_icon.c — renders the photo-meta app icon with CoreGraphics.
 *
 * Motif: a photo print (with a second print behind it) and a blue "i"
 * information badge over its corner, on a macOS-style rounded square.
 * Everything is drawn on a 1024-unit canvas and scaled to each size.
 *
 *   make_icon <out.iconset>    writes icon_*.png files for iconutil
 *
 * Copyright (c) 2026 Dennis Metzger
 * SPDX-License-Identifier: MIT
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define RGB(r, g, b) (r) / 255.0, (g) / 255.0, (b) / 255.0

static CGColorSpaceRef g_rgb;
static CGFloat g_scale; /* device pixels per canvas unit; shadows are not affected by the CTM */

static void rounded_rect(CGContextRef c, CGRect r, CGFloat radius)
{
    CGPathRef p = CGPathCreateWithRoundedRect(r, radius, radius, NULL);
    CGContextAddPath(c, p);
    CGPathRelease(p);
}

static void drop_shadow(CGContextRef c, CGFloat dy, CGFloat blur, CGFloat alpha)
{
    CGColorRef col = CGColorCreateGenericRGB(0, 0, 0.08, alpha);
    CGContextSetShadowWithColor(c, CGSizeMake(0, dy * g_scale), blur * g_scale, col);
    CGColorRelease(col);
}

/* vertical gradient over the current clip, from top colour to bottom colour */
static void vgradient(CGContextRef c, CGFloat top, CGFloat bottom, const CGFloat t[3], const CGFloat b[3])
{
    CGFloat comps[] = { t[0], t[1], t[2], 1, b[0], b[1], b[2], 1 };
    CGFloat locs[] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(g_rgb, comps, locs, 2);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, top), CGPointMake(0, bottom),
                                kCGGradientDrawsBeforeStartLocation | kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(g);
}

static void background(CGContextRef c)
{
    CGRect body = CGRectMake(100, 100, 824, 824);
    CGFloat radius = 185;

    CGContextSaveGState(c);
    drop_shadow(c, -12, 28, 0.35);
    rounded_rect(c, body, radius);
    CGContextSetRGBFillColor(c, RGB(30, 38, 76), 1);
    CGContextFillPath(c);
    CGContextRestoreGState(c);

    CGContextSaveGState(c);
    rounded_rect(c, body, radius);
    CGContextClip(c);
    vgradient(c, 924, 100, (CGFloat[]){ RGB(56, 74, 140) }, (CGFloat[]){ RGB(20, 26, 56) });

    /* soft sheen across the top */
    CGFloat sheen[] = { 1, 1, 1, 0.10, 1, 1, 1, 0 };
    CGFloat locs[] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(g_rgb, sheen, locs, 2);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, 924), CGPointMake(0, 560), 0);
    CGGradientRelease(g);

    /* hairline inner edge */
    rounded_rect(c, CGRectInset(body, 2, 2), radius - 2);
    CGContextSetRGBStrokeColor(c, 1, 1, 1, 0.12);
    CGContextSetLineWidth(c, 4);
    CGContextStrokePath(c);
    CGContextRestoreGState(c);
}

/* A white photo print centred at (cx, cy), rotated by deg, with a landscape inside. */
static void photo_print(CGContextRef c, CGFloat cx, CGFloat cy, CGFloat deg, int detailed)
{
    CGFloat w = 560, h = 420, border = 26;
    CGRect card = CGRectMake(-w / 2, -h / 2, w, h);

    CGContextSaveGState(c);
    CGContextTranslateCTM(c, cx, cy);
    CGContextRotateCTM(c, deg * M_PI / 180.0);

    CGContextSaveGState(c);
    drop_shadow(c, -14, 34, detailed ? 0.45 : 0.30);
    rounded_rect(c, card, 30);
    CGContextSetRGBFillColor(c, RGB(250, 250, 252), 1);
    CGContextFillPath(c);
    CGContextRestoreGState(c);

    CGRect photo = CGRectInset(card, border, border);
    CGContextSaveGState(c);
    rounded_rect(c, photo, 12);
    CGContextClip(c);

    CGFloat x = CGRectGetMinX(photo), y = CGRectGetMinY(photo);
    CGFloat pw = photo.size.width, ph = photo.size.height;

    if (!detailed) {
        vgradient(c, y + ph, y, (CGFloat[]){ RGB(150, 170, 214) }, (CGFloat[]){ RGB(112, 128, 178) });
        CGContextRestoreGState(c);
        CGContextRestoreGState(c);
        return;
    }

    /* dusk sky */
    CGFloat sky[] = { RGB(64, 132, 230), 1, RGB(150, 160, 235), 1, RGB(255, 186, 140), 1 };
    CGFloat locs[] = { 0, 0.55, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(g_rgb, sky, locs, 3);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, y + ph), CGPointMake(0, y + ph * 0.25), 0);
    CGGradientRelease(g);

    /* sun with glow */
    CGPoint sun = CGPointMake(x + pw * 0.70, y + ph * 0.60);
    CGFloat glow[] = { 1, 0.93, 0.75, 0.55, 1, 0.93, 0.75, 0 };
    CGFloat glocs[] = { 0, 1 };
    g = CGGradientCreateWithColorComponents(g_rgb, glow, glocs, 2);
    CGContextDrawRadialGradient(c, g, sun, 0, sun, 120, 0);
    CGGradientRelease(g);
    CGContextSetRGBFillColor(c, RGB(255, 241, 200), 1);
    CGContextFillEllipseInRect(c, CGRectMake(sun.x - 42, sun.y - 42, 84, 84));

    /* far range */
    CGContextSetRGBFillColor(c, RGB(104, 112, 186), 1);
    CGContextMoveToPoint(c, x, y);
    CGContextAddLineToPoint(c, x, y + ph * 0.42);
    CGContextAddLineToPoint(c, x + pw * 0.22, y + ph * 0.62);
    CGContextAddLineToPoint(c, x + pw * 0.40, y + ph * 0.45);
    CGContextAddLineToPoint(c, x + pw * 0.56, y + ph * 0.58);
    CGContextAddLineToPoint(c, x + pw * 0.80, y + ph * 0.36);
    CGContextAddLineToPoint(c, x + pw, y + ph * 0.48);
    CGContextAddLineToPoint(c, x + pw, y);
    CGContextClosePath(c);
    CGContextFillPath(c);

    /* near range */
    CGContextSetRGBFillColor(c, RGB(44, 52, 110), 1);
    CGContextMoveToPoint(c, x, y);
    CGContextAddLineToPoint(c, x, y + ph * 0.22);
    CGContextAddLineToPoint(c, x + pw * 0.30, y + ph * 0.50);
    CGContextAddLineToPoint(c, x + pw * 0.48, y + ph * 0.30);
    CGContextAddLineToPoint(c, x + pw * 0.62, y + ph * 0.40);
    CGContextAddLineToPoint(c, x + pw, y + ph * 0.08);
    CGContextAddLineToPoint(c, x + pw, y);
    CGContextClosePath(c);
    CGContextFillPath(c);

    /* snow caps on the near peak */
    CGContextSetRGBFillColor(c, 1, 1, 1, 0.85);
    CGContextMoveToPoint(c, x + pw * 0.30, y + ph * 0.50);
    CGContextAddLineToPoint(c, x + pw * 0.245, y + ph * 0.445);
    CGContextAddLineToPoint(c, x + pw * 0.29, y + ph * 0.455);
    CGContextAddLineToPoint(c, x + pw * 0.32, y + ph * 0.43);
    CGContextAddLineToPoint(c, x + pw * 0.345, y + ph * 0.455);
    CGContextClosePath(c);
    CGContextFillPath(c);

    CGContextRestoreGState(c);
    CGContextRestoreGState(c);
}

/* Blue circular "i" badge with a white ring. */
static void info_badge(CGContextRef c, CGFloat cx, CGFloat cy, CGFloat r)
{
    CGFloat ring = 16;

    CGContextSaveGState(c);
    drop_shadow(c, -12, 30, 0.45);
    CGContextSetRGBFillColor(c, 1, 1, 1, 1);
    CGContextFillEllipseInRect(c, CGRectMake(cx - r - ring, cy - r - ring, 2 * (r + ring), 2 * (r + ring)));
    CGContextRestoreGState(c);

    CGContextSaveGState(c);
    CGContextAddEllipseInRect(c, CGRectMake(cx - r, cy - r, 2 * r, 2 * r));
    CGContextClip(c);
    vgradient(c, cy + r, cy - r, (CGFloat[]){ RGB(72, 166, 255) }, (CGFloat[]){ RGB(20, 92, 236) });
    /* glossy upper highlight */
    CGFloat hl[] = { 1, 1, 1, 0.28, 1, 1, 1, 0 };
    CGFloat locs[] = { 0, 1 };
    CGGradientRef g = CGGradientCreateWithColorComponents(g_rgb, hl, locs, 2);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, cy + r), CGPointMake(0, cy), 0);
    CGGradientRelease(g);
    CGContextRestoreGState(c);

    /* the "i" */
    CGContextSaveGState(c);
    drop_shadow(c, -3, 6, 0.25);
    CGContextSetRGBFillColor(c, 1, 1, 1, 1);
    CGFloat dot = r * 0.17, stem_w = r * 0.25, stem_h = r * 0.80;
    CGContextFillEllipseInRect(c, CGRectMake(cx - dot, cy + r * 0.40 - dot, 2 * dot, 2 * dot));
    rounded_rect(c, CGRectMake(cx - stem_w / 2, cy - r * 0.58, stem_w, stem_h), stem_w / 2);
    CGContextFillPath(c);
    CGContextRestoreGState(c);
}

static void draw(CGContextRef c, CGFloat size)
{
    g_scale = size / 1024.0;
    CGContextScaleCTM(c, g_scale, g_scale);
    background(c);
    photo_print(c, 548, 600, 9, 0);   /* print behind */
    photo_print(c, 474, 578, -4, 1);  /* main print */
    info_badge(c, 704, 330, 148);
}

static int write_png(const char *path, int px)
{
    CGContextRef c = CGBitmapContextCreate(NULL, px, px, 8, 0, g_rgb, kCGImageAlphaPremultipliedLast);
    CGContextSetShouldAntialias(c, true);
    CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
    draw(c, px);
    CGImageRef img = CGBitmapContextCreateImage(c);

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    CGImageDestinationAddImage(dst, img, NULL);
    int ok = CGImageDestinationFinalize(dst);

    CFRelease(dst);
    CFRelease(url);
    CGImageRelease(img);
    CGContextRelease(c);
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: make_icon <out.iconset>\n");
        return 2;
    }
    g_rgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    static const int sizes[] = { 16, 32, 128, 256, 512 };
    char path[1024];
    int rc = 0;
    for (int i = 0; i < 5; i++) {
        snprintf(path, sizeof path, "%s/icon_%dx%d.png", argv[1], sizes[i], sizes[i]);
        rc |= write_png(path, sizes[i]);
        snprintf(path, sizeof path, "%s/icon_%dx%d@2x.png", argv[1], sizes[i], sizes[i]);
        rc |= write_png(path, sizes[i] * 2);
    }
    CGColorSpaceRelease(g_rgb);
    return rc;
}

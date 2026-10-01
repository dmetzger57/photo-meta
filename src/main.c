/*
 * photo-meta — a small macOS photo metadata viewer written in C.
 *
 * The UI is native AppKit, driven from C through the Objective-C runtime.
 * Metadata extraction lives in meta.c (ImageIO).
 *
 *   photo-meta                 launch the app
 *   photo-meta <file>          launch and open a photo
 *   photo-meta --dump <file>   print all metadata to stdout (no UI)
 *
 * Copyright (c) 2026 Dennis Metzger
 * SPDX-License-Identifier: MIT
 */
#include "meta.h"
#include "objc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AppKit constants used below */
enum {
    WindowStyleTitled = 1 << 0,
    WindowStyleClosable = 1 << 1,
    WindowStyleMiniaturizable = 1 << 2,
    WindowStyleResizable = 1 << 3,
    WindowStyleFullSizeContentView = 1 << 15,
    BackingStoreBuffered = 2,
    WindowTitleHidden = 1,
    LayoutAttributeLeading = 5,
    LayoutAttributeCenterX = 9,
    LayoutAttributeCenterY = 10,
    UserInterfaceLayoutOrientationVertical = 1,
    ImageScaleProportionallyUpOrDown = 3,
    VisualEffectMaterialSidebar = 7,
    BoxSeparator = 2,
    TableViewStyleInset = 2,
    TableColumnAutoresizeLastColumnOnly = 4,
    LineBreakByWordWrapping = 0,
    LineBreakByTruncatingTail = 4,
    LineBreakByTruncatingMiddle = 5,
    BezelStyleRounded = 1,
    ControlSizeLarge = 3,
    DragOperationNone = 0,
    DragOperationCopy = 1,
    AlertStyleWarning = 0,
    EventModifierCommand = 1 << 20,
    EventModifierShift = 1 << 17,
};

static const double FontWeightRegular = 0.0, FontWeightMedium = 0.23, FontWeightSemibold = 0.3,
                    FontWeightBold = 0.4, FontWeightLight = -0.4;

#define SIDEBAR_WIDTH 300.0
#define SIDEBAR_INSET 20.0
#define HIGHLIGHTS 6

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

static id g_controller;
static id g_window, g_table, g_keyColumn, g_search, g_count, g_emptyState, g_scroll;
static id g_thumb, g_name, g_subtitle, g_mapsButton;
static id g_highlightRow[HIGHLIGHTS], g_highlightLabel[HIGHLIGHTS];
static Meta g_meta;
static int g_loaded;
static size_t *g_visible, g_nvisible;
static char *g_pendingPath; /* file given on the command line */

/* ------------------------------------------------------------------ */
/* view helpers                                                        */
/* ------------------------------------------------------------------ */

static id font(double size, double weight)
{
    return MSG(id, double, double)(cls("NSFont"), sel("systemFontOfSize:weight:"), size, weight);
}

static id digits_font(double size, double weight)
{
    return MSG(id, double, double)(cls("NSFont"), sel("monospacedDigitSystemFontOfSize:weight:"), size, weight);
}

static id color(const char *name) { return msg(cls("NSColor"), name); }

static id symbol(const char *name, double size, double weight)
{
    id img = MSG(id, id, id)(cls("NSImage"), sel("imageWithSystemSymbolName:accessibilityDescription:"),
                             nsstr(name), NULL);
    if (!img) return NULL;
    id cfg = MSG(id, double, double)(cls("NSImageSymbolConfiguration"), sel("configurationWithPointSize:weight:"),
                                     size, weight);
    return msg_id(img, "imageWithSymbolConfiguration:", cfg);
}

static id label(const char *text, id fnt, id clr)
{
    id l = msg_id(cls("NSTextField"), "labelWithString:", nsstr(text));
    msg_id(l, "setFont:", fnt);
    if (clr) msg_id(l, "setTextColor:", clr);
    return l;
}

static void wrap_label(id l, double width, long lines)
{
    msg_long(l, "setLineBreakMode:", LineBreakByWordWrapping);
    msg_long(l, "setMaximumNumberOfLines:", lines);
    msg_dbl(l, "setPreferredMaxLayoutWidth:", width);
    MSG(void, float, long)(l, sel("setContentCompressionResistancePriority:forOrientation:"), 250.0f, 0);
}

static id image_view(id image)
{
    id iv = msg_id(cls("NSImageView"), "imageViewWithImage:", image);
    msg_bool(iv, "setEditable:", NO);
    return iv;
}

static id button(const char *title, const char *action)
{
    id b = MSG(id, id, id, SEL)(cls("NSButton"), sel("buttonWithTitle:target:action:"), nsstr(title), g_controller,
                                sel(action));
    return b;
}

static void no_autoresize(id v) { msg_bool(v, "setTranslatesAutoresizingMaskIntoConstraints:", NO); }

static void activate(id constraint) { msg_bool(constraint, "setActive:", YES); }

/* a.<anchorA> == b.<anchorB> + c */
static void pin(id a, const char *anchorA, id b, const char *anchorB, double c)
{
    activate(MSG(id, id, double)(msg(a, anchorA), sel("constraintEqualToAnchor:constant:"), msg(b, anchorB), c));
}

static void fix(id v, const char *dimension, double c)
{
    activate(MSG(id, double)(msg(v, dimension), sel("constraintEqualToConstant:"), c));
}

static id stack(int vertical, double spacing)
{
    id s = new_obj("NSStackView");
    if (vertical) msg_long(s, "setOrientation:", UserInterfaceLayoutOrientationVertical);
    msg_dbl(s, "setSpacing:", spacing);
    return s;
}

static void add_arranged(id s, id v) { msg_id(s, "addArrangedSubview:", v); }

static void set_text(id field, const char *text) { msg_id(field, "setStringValue:", nsstr(text)); }

/* ------------------------------------------------------------------ */
/* filtering                                                           */
/* ------------------------------------------------------------------ */

static void rebuild_visible(const char *query)
{
    free(g_visible);
    g_visible = malloc(sizeof(size_t) * (g_meta.count + 1));
    g_nvisible = 0;

    int has_query = query && query[0];
    size_t header = (size_t)-1, shown_items = 0;
    int header_added = 0, header_matches = 0;

    for (size_t i = 0; i < g_meta.count; i++) {
        MetaRow *r = &g_meta.rows[i];
        if (r->kind == ROW_HEADER) {
            header = i;
            header_added = 0;
            header_matches = has_query && strcasestr(r->key, query) != NULL;
            continue;
        }
        if (has_query && !header_matches && !strcasestr(r->key, query) && !strcasestr(r->value, query))
            continue;
        if (!header_added && header != (size_t)-1) {
            g_visible[g_nvisible++] = header;
            header_added = 1;
        }
        g_visible[g_nvisible++] = i;
        shown_items++;
    }

    char text[96];
    if (!g_loaded) text[0] = 0;
    else if (has_query) snprintf(text, sizeof text, "%zu of %zu fields", shown_items, g_meta.items);
    else snprintf(text, sizeof text, "%zu fields", g_meta.items);
    set_text(g_count, text);
}

/* ------------------------------------------------------------------ */
/* loading a photo                                                     */
/* ------------------------------------------------------------------ */

static void show_alert(const char *title, const char *text)
{
    id alert = new_obj("NSAlert");
    msg_id(alert, "setMessageText:", nsstr(title));
    msg_id(alert, "setInformativeText:", nsstr(text));
    msg_long(alert, "setAlertStyle:", AlertStyleWarning);
    MSG(void, id, id)(alert, sel("beginSheetModalForWindow:completionHandler:"), g_window, NULL);
    msg(alert, "release");
}

static void set_highlight(int i, const char *text)
{
    set_text(g_highlightLabel[i], text);
    msg_id(g_highlightLabel[i], "setToolTip:", nsstr(text));
    msg_bool(g_highlightRow[i], "setHidden:", text[0] == 0);
}

static void update_sidebar(void)
{
    MetaSummary *s = &g_meta.summary;

    if (g_meta.thumbnail) {
        CGSize zero = { 0, 0 };
        id img = MSG(id, CGImageRef, CGSize)(msg(cls("NSImage"), "alloc"), sel("initWithCGImage:size:"),
                                             g_meta.thumbnail, zero);
        msg_id(g_thumb, "setImage:", img);
        msg(img, "release");
    } else {
        msg_id(g_thumb, "setImage:", symbol("photo", 64, FontWeightLight));
    }

    set_text(g_name, s->name);
    char sub[256];
    snprintf(sub, sizeof sub, "%s%s%s", s->kind, s->kind[0] ? "  ·  " : "", s->size);
    set_text(g_subtitle, sub);

    set_highlight(0, s->dims);
    set_highlight(1, s->camera);
    set_highlight(2, s->lens);
    set_highlight(3, s->exposure);
    set_highlight(4, s->date);
    set_highlight(5, s->location);
    msg_bool(g_mapsButton, "setHidden:", !s->has_gps);
}

static void open_path(const char *path)
{
    Meta fresh;
    char err[256];
    if (meta_load(path, &fresh, err, sizeof err) != 0) {
        const char *slash = strrchr(path, '/');
        char title[512];
        snprintf(title, sizeof title, "Can’t read “%s”", slash ? slash + 1 : path);
        show_alert(title, err);
        return;
    }

    meta_free(&g_meta);
    g_meta = fresh;
    g_loaded = 1;

    update_sidebar();
    set_text(g_search, "");
    rebuild_visible(NULL);
    msg(g_table, "reloadData");
    msg_long(g_table, "scrollRowToVisible:", 0);
    msg_bool(g_emptyState, "setHidden:", YES);
    msg_bool(g_scroll, "setHidden:", NO);
    msg_id(g_window, "makeFirstResponder:", g_table);

    msg_id(g_window, "setTitle:", nsstr(g_meta.summary.name));
    id url = msg_id(cls("NSURL"), "fileURLWithPath:", nsstr(path));
    msg_id(g_window, "setRepresentedURL:", url);
    msg_id(msg(cls("NSDocumentController"), "sharedDocumentController"), "noteNewRecentDocumentURL:", url);
}

static void open_url(id url)
{
    if (!url || !MSG(BOOL)(url, sel("isFileURL"))) return;
    open_path(MSG(const char *)(url, sel("fileSystemRepresentation")));
}

/* ------------------------------------------------------------------ */
/* controller methods (NSApplicationDelegate, table data source, …)    */
/* ------------------------------------------------------------------ */

static void open_document(id self, SEL _cmd, id sender)
{
    (void)self; (void)_cmd; (void)sender;
    id panel = msg(cls("NSOpenPanel"), "openPanel");
    msg_bool(panel, "setCanChooseFiles:", YES);
    msg_bool(panel, "setCanChooseDirectories:", NO);
    msg_bool(panel, "setAllowsMultipleSelection:", NO);
    msg_id(panel, "setMessage:", nsstr("Choose a photo to inspect"));
    msg_id(panel, "setPrompt:", nsstr("Inspect"));
    id imageType = msg_id(cls("UTType"), "typeWithIdentifier:", nsstr("public.image"));
    msg_id(panel, "setAllowedContentTypes:", msg_id(cls("NSArray"), "arrayWithObject:", imageType));

    MSG(void, id, void (^)(long))(panel, sel("beginSheetModalForWindow:completionHandler:"), g_window,
                                  ^(long response) {
                                      if (response == 1 /* NSModalResponseOK */)
                                          open_url(msg(msg(panel, "URLs"), "firstObject"));
                                  });
}

static void filter_changed(id self, SEL _cmd, id sender)
{
    (void)self; (void)_cmd;
    rebuild_visible(cstr(msg(sender, "stringValue")));
    msg(g_table, "reloadData");
}

static void focus_search(id self, SEL _cmd, id sender)
{
    (void)self; (void)_cmd; (void)sender;
    msg_id(g_window, "makeFirstResponder:", g_search);
}

static void show_in_maps(id self, SEL _cmd, id sender)
{
    (void)self; (void)_cmd; (void)sender;
    char url[256];
    snprintf(url, sizeof url, "https://maps.apple.com/?ll=%.6f,%.6f&q=%s", g_meta.summary.lat,
             g_meta.summary.lon, "Photo%20Location");
    id nsurl = msg_id(cls("NSURL"), "URLWithString:", nsstr(url));
    msg_id(msg(cls("NSWorkspace"), "sharedWorkspace"), "openURL:", nsurl);
}

static void reveal_in_finder(id self, SEL _cmd, id sender)
{
    (void)self; (void)_cmd; (void)sender;
    id url = msg(g_window, "representedURL");
    if (!url) return;
    msg_id(msg(cls("NSWorkspace"), "sharedWorkspace"), "activateFileViewerSelectingURLs:",
           msg_id(cls("NSArray"), "arrayWithObject:", url));
}

/* Copy selected rows as "Key: Value" lines (or everything when nothing is selected). */
static void copy_rows(id self, SEL _cmd, id sender)
{
    (void)self; (void)_cmd; (void)sender;
    if (!g_loaded) return;
    id selection = msg(g_table, "selectedRowIndexes");
    long selected = MSG(long)(selection, sel("count"));

    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    out[0] = 0;
    MetaRow *pending_header = NULL;
    for (size_t v = 0; v < g_nvisible; v++) {
        MetaRow *r = &g_meta.rows[g_visible[v]];
        if (r->kind == ROW_HEADER) {
            pending_header = r; /* printed only if one of its rows is copied */
            continue;
        }
        if (selected && !MSG(BOOL, unsigned long)(selection, sel("containsIndex:"), v)) continue;
        size_t need = strlen(r->key) + strlen(r->value) + (pending_header ? strlen(pending_header->key) : 0) + 8;
        if (len + need >= cap) { cap = (cap + need) * 2; out = realloc(out, cap); }
        if (pending_header) {
            len += (size_t)snprintf(out + len, cap - len, "%s%s\n", len ? "\n" : "", pending_header->key);
            pending_header = NULL;
        }
        len += (size_t)snprintf(out + len, cap - len, "%s: %s\n", r->key, r->value);
    }

    id pb = msg(cls("NSPasteboard"), "generalPasteboard");
    MSG(long)(pb, sel("clearContents"));
    MSG(BOOL, id, id)(pb, sel("setString:forType:"), nsstr(out), nsstr("public.utf8-plain-text"));
    free(out);
}

static BOOL validate_menu_item(id self, SEL _cmd, id item)
{
    (void)self; (void)_cmd;
    SEL action = MSG(SEL)(item, sel("action"));
    if (action == sel("copy:") || action == sel("revealInFinder:") || action == sel("performFindPanelAction:"))
        return g_loaded;
    return YES;
}

static long number_of_rows(id self, SEL _cmd, id table)
{
    (void)self; (void)_cmd; (void)table;
    return (long)g_nvisible;
}

static BOOL is_group_row(id self, SEL _cmd, id table, long row)
{
    (void)self; (void)_cmd; (void)table;
    return row >= 0 && (size_t)row < g_nvisible && g_meta.rows[g_visible[row]].kind == ROW_HEADER;
}

static BOOL should_select_row(id self, SEL _cmd, id table, long row)
{
    return !is_group_row(self, _cmd, table, row);
}

static double height_of_row(id self, SEL _cmd, id table, long row)
{
    return is_group_row(self, _cmd, table, row) ? 34.0 : 24.0;
}

static id cell_container(id content, double leading)
{
    id cell = new_obj("NSTableCellView");
    no_autoresize(content);
    msg_id(cell, "addSubview:", content);
    pin(content, "leadingAnchor", cell, "leadingAnchor", leading);
    pin(content, "trailingAnchor", cell, "trailingAnchor", -6);
    pin(content, "centerYAnchor", cell, "centerYAnchor", 0);
    return msg(cell, "autorelease");
}

static id view_for_row(id self, SEL _cmd, id table, id column, long row)
{
    (void)self; (void)_cmd; (void)table;
    if (row < 0 || (size_t)row >= g_nvisible) return NULL;
    MetaRow *r = &g_meta.rows[g_visible[row]];

    if (r->kind == ROW_HEADER) {
        id s = stack(0, 7);
        msg_long(s, "setAlignment:", LayoutAttributeCenterY);
        id img = symbol(r->value, 12, FontWeightSemibold);
        if (img) {
            id iv = image_view(img);
            msg_id(iv, "setContentTintColor:", color("controlAccentColor"));
            add_arranged(s, iv);
        }
        add_arranged(s, label(r->key, font(12, FontWeightBold), color("labelColor")));
        id cell = cell_container(s, 4);
        msg(s, "release");
        return cell;
    }

    id l;
    if (column == g_keyColumn) {
        l = label(r->key, font(12.5, FontWeightRegular), color("secondaryLabelColor"));
        msg_long(l, "setLineBreakMode:", LineBreakByTruncatingTail);
    } else {
        l = label(r->value, digits_font(12.5, FontWeightMedium), color("labelColor"));
        msg_long(l, "setLineBreakMode:", LineBreakByTruncatingMiddle);
    }
    msg_id(l, "setToolTip:", nsstr(column == g_keyColumn ? r->key : r->value));
    MSG(void, float, long)(l, sel("setContentCompressionResistancePriority:forOrientation:"), 250.0f, 0);
    return cell_container(l, 6);
}

static BOOL should_terminate_after_last_window_closed(id self, SEL _cmd, id app)
{
    (void)self; (void)_cmd; (void)app;
    return YES;
}

static void open_urls(id self, SEL _cmd, id app, id urls)
{
    (void)self; (void)_cmd; (void)app;
    open_url(msg(urls, "firstObject"));
}

/* drag & drop onto the window */
static unsigned long dragging_entered(id self, SEL _cmd, id info)
{
    (void)self; (void)_cmd;
    id pb = msg(info, "draggingPasteboard");
    id types = msg_id(cls("NSArray"), "arrayWithObject:", nsstr("public.file-url"));
    return MSG(id, id)(pb, sel("availableTypeFromArray:"), types) ? DragOperationCopy : DragOperationNone;
}

static BOOL perform_drag(id self, SEL _cmd, id info)
{
    (void)self; (void)_cmd;
    id pb = msg(info, "draggingPasteboard");
    id classes = msg_id(cls("NSArray"), "arrayWithObject:", cls("NSURL"));
    id urls = msg_id2(pb, "readObjectsForClasses:options:", classes, NULL);
    id url = msg(urls, "firstObject");
    if (!url) return NO;
    open_url(url);
    return YES;
}

/* ------------------------------------------------------------------ */
/* building the UI                                                     */
/* ------------------------------------------------------------------ */

static id menu_item(id menu, const char *title, const char *action, const char *key)
{
    id item = MSG(id, id, SEL, id)(msg(cls("NSMenuItem"), "alloc"), sel("initWithTitle:action:keyEquivalent:"),
                                   nsstr(title), action ? sel(action) : NULL, nsstr(key));
    msg_id(menu, "addItem:", item);
    msg(item, "release");
    return item;
}

static id submenu(id menubar, const char *title)
{
    id holder = menu_item(menubar, title, NULL, "");
    id menu = msg_id(msg(cls("NSMenu"), "alloc"), "initWithTitle:", nsstr(title));
    msg_id(holder, "setSubmenu:", menu);
    msg(menu, "release");
    return menu;
}

static void separator(id menu) { msg_id(menu, "addItem:", msg(cls("NSMenuItem"), "separatorItem")); }

static void build_menu(id app)
{
    id menubar = new_obj("NSMenu");

    id appMenu = submenu(menubar, "photo-meta");
    menu_item(appMenu, "About photo-meta", "orderFrontStandardAboutPanel:", "");
    separator(appMenu);
    menu_item(appMenu, "Hide photo-meta", "hide:", "h");
    id hideOthers = menu_item(appMenu, "Hide Others", "hideOtherApplications:", "h");
    msg_long(hideOthers, "setKeyEquivalentModifierMask:", EventModifierCommand | (1 << 19) /* option */);
    menu_item(appMenu, "Show All", "unhideAllApplications:", "");
    separator(appMenu);
    menu_item(appMenu, "Quit photo-meta", "terminate:", "q");

    id fileMenu = submenu(menubar, "File");
    menu_item(fileMenu, "Open…", "openDocument:", "o");
    id reveal = menu_item(fileMenu, "Show in Finder", "revealInFinder:", "r");
    msg_long(reveal, "setKeyEquivalentModifierMask:", EventModifierCommand | EventModifierShift);
    separator(fileMenu);
    menu_item(fileMenu, "Close", "performClose:", "w");

    id editMenu = submenu(menubar, "Edit");
    menu_item(editMenu, "Copy", "copy:", "c");
    menu_item(editMenu, "Select All", "selectAll:", "a");
    separator(editMenu);
    menu_item(editMenu, "Find…", "focusSearch:", "f");

    id windowMenu = submenu(menubar, "Window");
    menu_item(windowMenu, "Minimize", "performMiniaturize:", "m");
    menu_item(windowMenu, "Zoom", "performZoom:", "");
    msg_id(app, "setWindowsMenu:", windowMenu);

    msg_id(app, "setMainMenu:", menubar);
    msg(menubar, "release");
}

static id build_sidebar(void)
{
    id sidebar = new_obj("NSVisualEffectView");
    msg_long(sidebar, "setMaterial:", VisualEffectMaterialSidebar);
    msg_long(sidebar, "setBlendingMode:", 0 /* behind window */);
    no_autoresize(sidebar);

    double inner = SIDEBAR_WIDTH - 2 * SIDEBAR_INSET;
    id col = stack(1, 6);
    msg_long(col, "setAlignment:", LayoutAttributeLeading);
    no_autoresize(col);
    msg_id(sidebar, "addSubview:", col);
    pin(col, "topAnchor", sidebar, "topAnchor", 52);
    pin(col, "leadingAnchor", sidebar, "leadingAnchor", SIDEBAR_INSET);
    pin(col, "trailingAnchor", sidebar, "trailingAnchor", -SIDEBAR_INSET);

    /* preview */
    g_thumb = image_view(symbol("photo.on.rectangle.angled", 64, FontWeightLight));
    msg_long(g_thumb, "setImageScaling:", ImageScaleProportionallyUpOrDown);
    msg_id(g_thumb, "setContentTintColor:", color("tertiaryLabelColor"));
    fix(g_thumb, "widthAnchor", inner);
    fix(g_thumb, "heightAnchor", 210);
    add_arranged(col, g_thumb);
    MSG(void, double, id)(col, sel("setCustomSpacing:afterView:"), 16, g_thumb);

    /* name + kind */
    g_name = label("No Photo Selected", font(15, FontWeightSemibold), color("labelColor"));
    wrap_label(g_name, inner, 2);
    msg_bool(g_name, "setSelectable:", YES);
    add_arranged(col, g_name);
    g_subtitle = label("Open a photo to see its metadata", font(12, FontWeightRegular),
                       color("secondaryLabelColor"));
    wrap_label(g_subtitle, inner, 2);
    add_arranged(col, g_subtitle);

    id rule = new_obj("NSBox");
    msg_long(rule, "setBoxType:", BoxSeparator);
    fix(rule, "widthAnchor", inner);
    add_arranged(col, rule);
    MSG(void, double, id)(col, sel("setCustomSpacing:afterView:"), 14, g_subtitle);
    MSG(void, double, id)(col, sel("setCustomSpacing:afterView:"), 14, rule);
    msg(rule, "release");

    /* highlights */
    static const char *icons[HIGHLIGHTS] = { "aspectratio", "camera", "scope", "sun.max", "calendar", "location" };
    for (int i = 0; i < HIGHLIGHTS; i++) {
        id row = stack(0, 10);
        msg_long(row, "setAlignment:", LayoutAttributeCenterY);
        id iv = image_view(symbol(icons[i], 13, FontWeightMedium));
        msg_id(iv, "setContentTintColor:", color("secondaryLabelColor"));
        fix(iv, "widthAnchor", 18);
        add_arranged(row, iv);
        g_highlightLabel[i] = label("", digits_font(12.5, FontWeightRegular), color("labelColor"));
        wrap_label(g_highlightLabel[i], inner - 28, 3);
        add_arranged(row, g_highlightLabel[i]);
        g_highlightRow[i] = row;
        msg_bool(row, "setHidden:", YES);
        add_arranged(col, row);
        msg(row, "release");
    }
    MSG(void, double, id)(col, sel("setCustomSpacing:afterView:"), 8, g_highlightRow[HIGHLIGHTS - 1]);

    g_mapsButton = button("Show in Maps", "showInMaps:");
    msg_long(g_mapsButton, "setControlSize:", 1 /* small */);
    msg_bool(g_mapsButton, "setHidden:", YES);
    add_arranged(col, g_mapsButton);

    /* open button pinned to the bottom */
    id open = button("Open Photo…", "openDocument:");
    msg_long(open, "setControlSize:", ControlSizeLarge);
    no_autoresize(open);
    msg_id(sidebar, "addSubview:", open);
    pin(open, "leadingAnchor", sidebar, "leadingAnchor", SIDEBAR_INSET);
    pin(open, "trailingAnchor", sidebar, "trailingAnchor", -SIDEBAR_INSET);
    pin(open, "bottomAnchor", sidebar, "bottomAnchor", -SIDEBAR_INSET);

    msg(col, "release");
    return sidebar;
}

static id build_table(void)
{
    g_scroll = new_obj("NSScrollView");
    no_autoresize(g_scroll);
    msg_bool(g_scroll, "setHasVerticalScroller:", YES);
    msg_bool(g_scroll, "setAutohidesScrollers:", YES);
    msg_bool(g_scroll, "setDrawsBackground:", NO);
    msg_bool(g_scroll, "setAutomaticallyAdjustsContentInsets:", NO);

    g_table = new_obj("NSTableView");
    msg_long(g_table, "setStyle:", TableViewStyleInset);
    msg_id(g_table, "setHeaderView:", NULL);
    msg_bool(g_table, "setUsesAlternatingRowBackgroundColors:", YES);
    msg_bool(g_table, "setFloatsGroupRows:", YES);
    msg_bool(g_table, "setAllowsMultipleSelection:", YES);
    msg_long(g_table, "setColumnAutoresizingStyle:", TableColumnAutoresizeLastColumnOnly);

    g_keyColumn = msg_id(msg(cls("NSTableColumn"), "alloc"), "initWithIdentifier:", nsstr("key"));
    msg_dbl(g_keyColumn, "setWidth:", 210);
    msg_dbl(g_keyColumn, "setMinWidth:", 120);
    msg_id(g_table, "addTableColumn:", g_keyColumn);

    id valueColumn = msg_id(msg(cls("NSTableColumn"), "alloc"), "initWithIdentifier:", nsstr("value"));
    msg_dbl(valueColumn, "setMinWidth:", 160);
    msg_id(g_table, "addTableColumn:", valueColumn);
    msg(valueColumn, "release");

    msg_id(g_table, "setDataSource:", g_controller);
    msg_id(g_table, "setDelegate:", g_controller);
    msg_id(g_scroll, "setDocumentView:", g_table);
    msg_bool(g_scroll, "setHidden:", YES);
    return g_scroll;
}

static id build_empty_state(void)
{
    id s = stack(1, 8);
    msg_long(s, "setAlignment:", LayoutAttributeCenterX);
    no_autoresize(s);

    id iv = image_view(symbol("photo.on.rectangle.angled", 56, FontWeightLight));
    msg_id(iv, "setContentTintColor:", color("tertiaryLabelColor"));
    add_arranged(s, iv);
    MSG(void, double, id)(s, sel("setCustomSpacing:afterView:"), 14, iv);

    add_arranged(s, label("No Photo Selected", font(17, FontWeightSemibold), color("labelColor")));
    id hint = label("Choose a photo, or drop one anywhere in this window.", font(13, FontWeightRegular),
                    color("secondaryLabelColor"));
    add_arranged(s, hint);
    MSG(void, double, id)(s, sel("setCustomSpacing:afterView:"), 18, hint);

    id b = button("Choose Photo…", "openDocument:");
    msg_long(b, "setBezelStyle:", BezelStyleRounded);
    msg_long(b, "setControlSize:", ControlSizeLarge);
    msg_id(b, "setKeyEquivalent:", nsstr("\r"));
    add_arranged(s, b);
    return s;
}

static void build_window(void)
{
    CGRect frame = { { 0, 0 }, { 980, 680 } };
    g_window = MSG(id, CGRect, unsigned long, unsigned long, BOOL)(
        msg(cls("NSWindow"), "alloc"), sel("initWithContentRect:styleMask:backing:defer:"), frame,
        WindowStyleTitled | WindowStyleClosable | WindowStyleMiniaturizable | WindowStyleResizable |
            WindowStyleFullSizeContentView,
        BackingStoreBuffered, NO);
    msg_id(g_window, "setTitle:", nsstr("photo-meta"));
    msg_long(g_window, "setTitleVisibility:", WindowTitleHidden);
    msg_bool(g_window, "setTitlebarAppearsTransparent:", YES);
    msg_bool(g_window, "setReleasedWhenClosed:", NO);
    MSG(void, CGSize)(g_window, sel("setMinSize:"), (CGSize){ 760, 480 });
    msg_id(g_window, "setFrameAutosaveName:", nsstr("MainWindow"));

    /* content view that accepts dropped files */
    id content = new_obj("PMDropView");
    msg_id(g_window, "setContentView:", content);
    msg_id(content, "registerForDraggedTypes:",
           msg_id(cls("NSArray"), "arrayWithObject:", nsstr("public.file-url")));

    id sidebar = build_sidebar();
    msg_id(content, "addSubview:", sidebar);
    pin(sidebar, "topAnchor", content, "topAnchor", 0);
    pin(sidebar, "bottomAnchor", content, "bottomAnchor", 0);
    pin(sidebar, "leadingAnchor", content, "leadingAnchor", 0);
    fix(sidebar, "widthAnchor", SIDEBAR_WIDTH);

    id divider = new_obj("NSBox");
    msg_long(divider, "setBoxType:", BoxSeparator);
    no_autoresize(divider);
    msg_id(content, "addSubview:", divider);
    pin(divider, "topAnchor", content, "topAnchor", 0);
    pin(divider, "bottomAnchor", content, "bottomAnchor", 0);
    pin(divider, "leadingAnchor", sidebar, "trailingAnchor", 0);
    fix(divider, "widthAnchor", 1);

    /* header: title, count, search */
    id title = label("Metadata", font(15, FontWeightBold), color("labelColor"));
    no_autoresize(title);
    msg_id(content, "addSubview:", title);
    pin(title, "leadingAnchor", divider, "trailingAnchor", 24);
    pin(title, "topAnchor", content, "topAnchor", 16);

    g_count = label("", digits_font(12, FontWeightRegular), color("secondaryLabelColor"));
    no_autoresize(g_count);
    msg_id(content, "addSubview:", g_count);
    pin(g_count, "leadingAnchor", title, "trailingAnchor", 10);
    pin(g_count, "firstBaselineAnchor", title, "firstBaselineAnchor", 0);

    g_search = new_obj("NSSearchField");
    no_autoresize(g_search);
    msg_id(g_search, "setPlaceholderString:", nsstr("Filter fields"));
    msg_id(g_search, "setTarget:", g_controller);
    msg_sel(g_search, "setAction:", sel("filterChanged:"));
    msg_id(content, "addSubview:", g_search);
    pin(g_search, "trailingAnchor", content, "trailingAnchor", -20);
    pin(g_search, "centerYAnchor", title, "centerYAnchor", 0);
    fix(g_search, "widthAnchor", 230);

    id rule = new_obj("NSBox");
    msg_long(rule, "setBoxType:", BoxSeparator);
    no_autoresize(rule);
    msg_id(content, "addSubview:", rule);
    pin(rule, "topAnchor", content, "topAnchor", 52);
    pin(rule, "leadingAnchor", divider, "trailingAnchor", 0);
    pin(rule, "trailingAnchor", content, "trailingAnchor", 0);

    id scroll = build_table();
    msg_id(content, "addSubview:", scroll);
    pin(scroll, "topAnchor", rule, "bottomAnchor", 0);
    pin(scroll, "bottomAnchor", content, "bottomAnchor", 0);
    pin(scroll, "leadingAnchor", divider, "trailingAnchor", 0);
    pin(scroll, "trailingAnchor", content, "trailingAnchor", 0);

    g_emptyState = build_empty_state();
    msg_id(content, "addSubview:", g_emptyState);
    pin(g_emptyState, "centerXAnchor", scroll, "centerXAnchor", 0);
    pin(g_emptyState, "centerYAnchor", scroll, "centerYAnchor", -20);

    msg(sidebar, "release");
    msg(divider, "release");
    msg(rule, "release");
    msg(content, "release");

    msg_id(g_window, "setInitialFirstResponder:", g_table);
    rebuild_visible(NULL);
    if (!MSG(BOOL, id)(g_window, sel("setFrameUsingName:"), nsstr("MainWindow")))
        msg(g_window, "center");
    msg_id(g_window, "makeKeyAndOrderFront:", NULL);
}

static void ensure_ui(void)
{
    if (!g_window) build_window();
}

static void will_finish_launching(id self, SEL _cmd, id note)
{
    (void)self; (void)_cmd; (void)note;
    ensure_ui();
}

static void did_finish_launching(id self, SEL _cmd, id note)
{
    (void)self; (void)_cmd; (void)note;
    ensure_ui();
    id app = msg(cls("NSApplication"), "sharedApplication");
    msg_bool(app, "activateIgnoringOtherApps:", YES);
    if (g_pendingPath) {
        open_path(g_pendingPath);
        free(g_pendingPath);
        g_pendingPath = NULL;
    }
}

static void register_classes(void)
{
    Class c = objc_allocateClassPair(objc_getClass("NSObject"), "PMController", 0);
    class_addMethod(c, sel("applicationWillFinishLaunching:"), (IMP)will_finish_launching, "v@:@");
    class_addMethod(c, sel("applicationDidFinishLaunching:"), (IMP)did_finish_launching, "v@:@");
    class_addMethod(c, sel("applicationShouldTerminateAfterLastWindowClosed:"),
                    (IMP)should_terminate_after_last_window_closed, "c@:@");
    class_addMethod(c, sel("application:openURLs:"), (IMP)open_urls, "v@:@@");
    class_addMethod(c, sel("openDocument:"), (IMP)open_document, "v@:@");
    class_addMethod(c, sel("filterChanged:"), (IMP)filter_changed, "v@:@");
    class_addMethod(c, sel("focusSearch:"), (IMP)focus_search, "v@:@");
    class_addMethod(c, sel("showInMaps:"), (IMP)show_in_maps, "v@:@");
    class_addMethod(c, sel("revealInFinder:"), (IMP)reveal_in_finder, "v@:@");
    class_addMethod(c, sel("copy:"), (IMP)copy_rows, "v@:@");
    class_addMethod(c, sel("validateMenuItem:"), (IMP)validate_menu_item, "c@:@");
    class_addMethod(c, sel("numberOfRowsInTableView:"), (IMP)number_of_rows, "q@:@");
    class_addMethod(c, sel("tableView:viewForTableColumn:row:"), (IMP)view_for_row, "@@:@@q");
    class_addMethod(c, sel("tableView:isGroupRow:"), (IMP)is_group_row, "c@:@q");
    class_addMethod(c, sel("tableView:shouldSelectRow:"), (IMP)should_select_row, "c@:@q");
    class_addMethod(c, sel("tableView:heightOfRow:"), (IMP)height_of_row, "d@:@q");
    const char *protocols[] = { "NSApplicationDelegate", "NSTableViewDataSource", "NSTableViewDelegate" };
    for (int i = 0; i < 3; i++) {
        Protocol *p = objc_getProtocol(protocols[i]);
        if (p) class_addProtocol(c, p);
    }
    objc_registerClassPair(c);

    Class d = objc_allocateClassPair(objc_getClass("NSView"), "PMDropView", 0);
    class_addMethod(d, sel("draggingEntered:"), (IMP)dragging_entered, "Q@:@");
    class_addMethod(d, sel("performDragOperation:"), (IMP)perform_drag, "c@:@");
    objc_registerClassPair(d);
}

/* ------------------------------------------------------------------ */
/* command line dump                                                   */
/* ------------------------------------------------------------------ */

static int dump(const char *path)
{
    void *pool = objc_autoreleasePoolPush();
    Meta m;
    char err[256];
    int rc = 0;
    if (meta_load(path, &m, err, sizeof err) != 0) {
        fprintf(stderr, "photo-meta: %s: %s\n", path, err);
        rc = 1;
    } else {
        for (size_t i = 0; i < m.count; i++) {
            if (m.rows[i].kind == ROW_HEADER) printf("%s%s\n", i ? "\n" : "", m.rows[i].key);
            else printf("  %-32s %s\n", m.rows[i].key, m.rows[i].value);
        }
        meta_free(&m);
    }
    objc_autoreleasePoolPop(pool);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("usage: photo-meta [photo]\n       photo-meta --dump <photo>\n");
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--dump")) {
        if (argc < 3) {
            fprintf(stderr, "usage: photo-meta --dump <photo>\n");
            return 2;
        }
        int rc = 0;
        for (int i = 2; i < argc; i++) rc |= dump(argv[i]);
        return rc;
    }

    void *pool = objc_autoreleasePoolPush();
    if (argc >= 2 && argv[1][0] != '-') {
        char *resolved = realpath(argv[1], NULL);
        g_pendingPath = resolved ? resolved : strdup(argv[1]);
    }

    register_classes();
    id app = msg(cls("NSApplication"), "sharedApplication");
    msg_long(app, "setActivationPolicy:", 0 /* regular */);
    g_controller = new_obj("PMController");
    msg_id(app, "setDelegate:", g_controller);
    build_menu(app);

    objc_autoreleasePoolPop(pool);
    msg(app, "run");
    return 0;
}

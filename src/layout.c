

#include "clientlist.h"
#include "globals.h"
#include "ipc-protocol.h"
#include "settings.h"
#include "layout.h"
#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>

int* g_window_gap;
int* g_border_width;

void layout_init() {
    g_cur_monitor = 0;
    g_tags = g_array_new(false, false, sizeof(HSTag));
    g_monitors = g_array_new(false, false, sizeof(HSMonitor));
    // load settings
    g_window_gap = &(settings_find("window_gap")->value.i);
    g_border_width = &(settings_find("border_width")->value.i);
}

void layout_destroy() {
    int i;
    for (i = 0; i < g_tags->len; i++) {
        g_string_free(g_array_index(g_tags, HSTag, i).name, true);
    }
    g_array_free(g_tags, true);
    g_array_free(g_monitors, true);
}


HSFrame* frame_create_empty() {
    HSFrame* frame = g_new0(HSFrame, 1);
    frame->type = TYPE_CLIENTS;
    return frame;
}

void frame_insert_window(HSFrame* frame, Window window) {
    if (frame->type == TYPE_CLIENTS) {
        // insert it here
        Window* buf = frame->content.clients.buf;
        // append it to buf
        size_t count = frame->content.clients.count;
        count++;
        buf = g_renew(Window, buf, count);
        buf[count-1] = window;
        // write results back
        frame->content.clients.count = count;
        frame->content.clients.buf = buf;
    } else { /* frame->type == TYPE_FRAMES */
        frame_insert_window(frame->content.layout->a, window);
    }
}

bool frame_remove_window(HSFrame* frame, Window window) {
    if (frame->type == TYPE_CLIENTS) {
        Window* buf = frame->content.clients.buf;
        size_t count = frame->content.clients.count;
        int i;
        for (i = 0; i < count; i++) {
            if (buf[i] == window) {
                // if window was found
                // them remove it
                memmove(buf+i, buf+i+1, sizeof(Window)*(count - i - 1));
                count--;
                buf = g_renew(Window, buf, count);
                frame->content.clients.buf = buf;
                frame->content.clients.count = count;
                frame->content.clients.selection -=
                    (frame->content.clients.selection <= i) ? 0 : 1;
                return true;
            }
        }
        return false;
    } else { /* frame->type == TYPE_FRAMES */
        bool found = frame_remove_window(frame->content.layout->a, window);
        found = found || frame_remove_window(frame->content.layout->b, window);
        return found;
    }
}

void frame_destroy(HSFrame* frame, Window** buf, size_t* count) {
    if (frame->type == TYPE_CLIENTS) {
        *buf = frame->content.clients.buf;
        *count = frame->content.clients.count;
        g_free(frame);
    } else { /* frame->type == TYPE_FRAMES */
        size_t c1, c2;
        Window *buf1, *buf2;
        frame_destroy(frame->content.layout->a, &buf1, &c1);
        frame_destroy(frame->content.layout->b, &buf2, &c2);
        // append buf2 to buf1
        buf1 = g_renew(Window, buf1, c1 + c2);
        memcpy(buf1+c1, buf2, sizeof(Window) * c2);
        // free unused things
        g_free(buf2);
        g_free(frame);
        // return;
        *buf = buf1;
        *count = c1 + c2;
    }
}

void print_frame_tree(HSFrame* frame, int indent, GString** output) {
    unsigned int j;
    if (frame->type == TYPE_CLIENTS) {
        Window* buf = frame->content.clients.buf;
        size_t count = frame->content.clients.count;
        unsigned int i;
        for (j = 0; j < indent; j++) {
            *output = g_string_append(*output, " ");
        }
        g_string_append_printf(*output, "frame with wins:\n");
        for (i = 0; i < count; i++) {
            for (j = 0; j < indent; j++) {
                *output = g_string_append(*output, " ");
            }
            g_string_append_printf(*output, "  -> win %d\n", (int)buf[i]);
        }
    } else { /* frame->type == TYPE_FRAMES */
        for (j = 0; j < indent; j++) {
            *output = g_string_append(*output, " ");
        }
        HSLayout* layout = frame->content.layout;
        g_string_append_printf(*output,
            "layout %s, size %f%%\n", (layout->align ? "vert" : "horz"),
                ((double)layout->fraction*100)/(double)FRACTION_UNIT);
        print_frame_tree(layout->a, indent+2, output);
        print_frame_tree(layout->b, indent+2, output);
    }

}


void frame_apply_layout(HSFrame* frame, XRectangle rect) {
    if (frame->type == TYPE_CLIENTS) {
        // frame only -> apply window_gap
        rect.x += *g_window_gap;
        rect.y += *g_window_gap;
        rect.height -= *g_window_gap * 2;
        rect.width -= *g_window_gap * 2;
        // move windows
        Window* buf = frame->content.clients.buf;
        size_t count = frame->content.clients.count;
        if (count == 0) {
            return;
        }
        XRectangle cur = rect;
        cur.height /= count;
        int step = cur.height;
        int i;
        for (i = 0; i < count; i++) {
            XMoveWindow(g_display, buf[i], cur.x, cur.y);
            XResizeWindow(g_display, buf[i], cur.width, cur.height);
            cur.y += step;
        }
    } else { /* frame->type == TYPE_FRAMES */
        HSLayout* layout = frame->content.layout;
        XRectangle first = rect;
        XRectangle second = rect;
        if (layout->align == LAYOUT_VERTICAL) {
            first.height = (rect.height * layout->fraction) / FRACTION_UNIT;
            second.y += first.height;
            second.height -= first.height;
        } else { // (layout->align == LAYOUT_HORIZONTAL)
            first.width = (rect.width * layout->fraction) / FRACTION_UNIT;
            second.x += first.width;
            second.width -= first.width;
        }
        frame_apply_layout(layout->a, first);
        frame_apply_layout(layout->b, second);
    }
}

HSMonitor* add_monitor(XRectangle rect) {
    HSMonitor m;
    m.rect = rect;
    m.tag = NULL;
    // find an tag
    int i;
    for (i = 0; i < g_tags->len; i++) {
        HSTag* tag = &g_array_index(g_tags, HSTag, i);
        if (find_monitor_with_tag(tag) == NULL) {
            m.tag = tag;
        }
    }
    g_array_append_val(g_monitors, m);
    return &g_array_index(g_monitors, HSMonitor, g_monitors->len-1);
}

HSMonitor* find_monitor_with_tag(HSTag* tag) {
    int i;
    for (i = 0; i < g_monitors->len; i++) {
        HSMonitor* m = &g_array_index(g_monitors, HSMonitor, i);
        if (m->tag == tag) {
            return m;
        }
    }
    return NULL;
}

HSTag* add_tag(char* name) {
    HSTag tag;
    tag.frame = frame_create_empty();
    tag.name = g_string_new(name);
    g_array_append_val(g_tags, tag);
    return &g_array_index(g_tags, HSTag, g_tags->len-1);
}

void ensure_tags_are_available() {
    if (g_tags->len > 0) {
        // nothing to do
        return;
    }
    add_tag("default");
}

void ensure_monitors_are_available() {
    if (g_monitors->len > 0) {
        // nothing to do
        return;
    }
    // add monitor if necessary
    XRectangle rect = {
        .x = 0, .y = 0,
        .width = DisplayWidth(g_display, DefaultScreen(g_display)),
        .height = DisplayHeight(g_display, DefaultScreen(g_display)),
    };
    ensure_tags_are_available();
    add_monitor(rect);
}

HSFrame* frame_current_selection() {
    HSMonitor* m = &g_array_index(g_monitors, HSMonitor, g_cur_monitor);
    if (!m->tag) return NULL;
    HSFrame* frame = m->tag->frame;
    while (frame->type == TYPE_FRAMES) {
        frame = (frame->content.layout->selection == 0) ?
                frame->content.layout->a :
                frame->content.layout->b;
    }
    return frame;
}

int frame_current_cycle_selection(int argc, char** argv) {
    int delta = 1;
    if (argc >= 2) {
        delta = atoi(argv[1]);
    }
    // find current selection
    HSFrame* frame = frame_current_selection();
    int index = frame->content.clients.selection;
    index += delta;
    index %= frame->content.clients.count;
    index += frame->content.clients.count;
    index %= frame->content.clients.count;
    frame->content.clients.selection = index;
    Window window = frame->content.clients.buf[index];
    window_focus(window);
    return 0;
}

void frame_split(HSFrame* frame, int align, int fraction) {
    HSFrame* first = frame_create_empty();
    HSFrame* second = frame_create_empty();
    *first = *frame;
    first->parent = frame;
    second->parent = frame;
    second->type = TYPE_CLIENTS;
    frame->type = TYPE_FRAMES;
    frame->content.layout = g_new(HSLayout, 1);
    frame->content.layout->align = align;
    frame->content.layout->a = first;
    frame->content.layout->b = second;
    frame->content.layout->selection = 0;
    frame->content.layout->fraction = fraction;
    // redraw monitor if exists
    HSMonitor* m = monitor_with_frame(frame);
    if (m) {
        frame_apply_layout(m->tag->frame, m->rect);
    }
}

int frame_split_command(int argc, char** argv) {
    // usage: split (cur) h|v FRACTION
    if (argc < 4) {
        return HERBST_INVALID_ARGUMENT;
    }
    int align = LAYOUT_VERTICAL;
    if (argv[2][0] == 'h') {
        align = LAYOUT_HORIZONTAL;
    } // else: layout ist vertical
    int fraction = FRACTION_UNIT* CLAMP(atof(argv[3]), 0.0, 1.0);
    HSFrame* frame = frame_current_selection();
    if (!frame) return 0; // nothing to do
    frame_split(frame, align, fraction);
    return 0;
}

HSTag* find_tag_with_toplevel_frame(HSFrame* frame) {
    int i;
    for (i = 0; i < g_tags->len; i++) {
        HSTag* m = &g_array_index(g_tags, HSTag, i);
        if (m->frame == frame) {
            return m;
        }
    }
    return NULL;
}

HSMonitor* monitor_with_frame(HSFrame* frame) {
    // find toplevel Frame
    while (frame->parent) {
        frame = frame->parent;
    }
    HSTag* tag = find_tag_with_toplevel_frame(frame);
    return find_monitor_with_tag(tag);
}

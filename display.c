#include <stdio.h>

#include <context.h>
#include <defaultatoms.h>
#include <interop.h>
#include <mailbox.h>
#include <term.h>
#include <utils.h>

#include <epd_driver.h>
#include <epd_highlevel.h>

#include "font.h"

static void consume_display_mailbox(Context *ctx);

inline static float luma_rec709(uint8_t r, uint8_t g, uint8_t b)
{
    return 0.2126f * (float) r + 0.7152f * (float) g + 0.0722f * (float) b;
}

inline static uint8_t grey(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)(luma_rec709(r, g, b) + 0.5F);
}

static void draw_image(uint8_t *framebuffer, int x, int y, int width, int height, const char *data, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t *pixels = (uint32_t *) data;

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            uint8_t color;
            if ((*pixels >> 24) & 0xFF) {
                color = grey((*pixels) & 0xFF, (*pixels >> 8) & 0xFF, (*pixels >> 16) & 0xFF);
            } else {
                color = grey(0xFF, 0xFF, 0xFF);
            }
            epd_draw_pixel(x + j, y + i, color, framebuffer);
            pixels++;
        }
    }
}

static void draw_rect(uint8_t *framebuffer, int x, int y, int width, int height, uint8_t r, uint8_t g, uint8_t b)
{
    EpdRect rect = {
        .x = x,
        .y = y,
        .width = width,
        .height = height
    };
    epd_draw_rect(rect, grey(r, g, b), framebuffer);
}

static void draw_text(uint8_t *framebuffer, int x, int y, const char *text, uint8_t r, uint8_t g, uint8_t b)
{
    int len = strlen(text);
    uint8_t color = grey(r, g, b);

    for (int i = 0; i < len; i++) {
        unsigned const char *glyph = fontdata + ((unsigned char) text[i]) * 16;

        for (int j = 0; j < 16; j++) {
            unsigned char row = glyph[j];

            for (int k = 0; k < 8; k++) {
                if (row & (1 << (7 - k))) {
                    epd_draw_pixel(x + i * 8 + k, y + j, color, framebuffer);
                }
            }
        }
    }
}

static void process_message(Context *ctx)
{
    Message *message = mailbox_dequeue(ctx);
    term msg = message->message;

    term pid = term_get_tuple_element(msg, 1);
    term ref = term_get_tuple_element(msg, 2);
    term req = term_get_tuple_element(msg, 3);

    term cmd = term_get_tuple_element(req, 0);

    int local_process_id = term_to_local_process_id(pid);
    Context *target = globalcontext_get_process(ctx->global, local_process_id);

    uint8_t *framebuffer = epd_hl_get_framebuffer((EpdiyHighlevelState *) ctx->platform_data);

    if (cmd == context_make_atom(ctx, "\xC"
                                      "clear_screen")) {
        int color = term_to_int(term_get_tuple_element(req, 1));

        draw_rect(framebuffer, 0, 0, EPD_WIDTH, EPD_HEIGHT,
            (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);

    } else if (cmd == context_make_atom(ctx, "\xA"
                                             "draw_image")) {
        int x = term_to_int(term_get_tuple_element(req, 1));
        int y = term_to_int(term_get_tuple_element(req, 2));
        term img = term_get_tuple_element(req, 3);
        int color = term_to_int(term_get_tuple_element(req, 4));

        int width = term_to_int(term_get_tuple_element(img, 0));
        int height = term_to_int(term_get_tuple_element(img, 1));
        const char *data = term_binary_data(term_get_tuple_element(img, 2));

        draw_image(framebuffer, x, y, width, height, data, (color >> 16),
            (color >> 8) & 0xFF, color & 0xFF);

    } else if (cmd == context_make_atom(ctx, "\x9"
                                             "draw_rect")) {
        int x = term_to_int(term_get_tuple_element(req, 1));
        int y = term_to_int(term_get_tuple_element(req, 2));
        int width = term_to_int(term_get_tuple_element(req, 3));
        int height = term_to_int(term_get_tuple_element(req, 4));
        int color = term_to_int(term_get_tuple_element(req, 5));

        draw_rect(framebuffer, x, y, width, height,
            (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);

    } else if (cmd == context_make_atom(ctx, "\x9"
                                             "draw_text")) {
        int x = term_to_int(term_get_tuple_element(req, 1));
        int y = term_to_int(term_get_tuple_element(req, 2));
        term text_term = term_get_tuple_element(req, 3);
        int color = term_to_int(term_get_tuple_element(req, 4));

        int ok;
        char *text = interop_term_to_string(text_term, &ok);

        draw_text(framebuffer, x, y, text, (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);

        free(text);

    } else {
        fprintf(stderr, "display: ");
        term_display(stderr, req, ctx);
        fprintf(stderr, "\n");
    }

    if (UNLIKELY(memory_ensure_free(ctx, REF_SIZE + TUPLE_SIZE(2)) != MEMORY_GC_OK)) {
        abort();
    }

    epd_poweron();
    int temperature = epd_ambient_temperature();
    epd_hl_update_screen((EpdiyHighlevelState *) ctx->platform_data, MODE_GC16, temperature);
    epd_poweroff();

    term return_tuple = term_alloc_tuple(2, ctx);

    term_put_tuple_element(return_tuple, 0, ref);
    term_put_tuple_element(return_tuple, 1, OK_ATOM);

    mailbox_send(target, return_tuple);

    free(message);
}

static void consume_display_mailbox(Context *ctx)
{
    while (!list_is_empty(&ctx->mailbox)) {
        process_message(ctx);
    }
}

void display_port_driver_init(Context *ctx, term opts)
{
    UNUSED(opts);

    ctx->native_handler = consume_display_mailbox;

    EpdiyHighlevelState *hl = calloc(sizeof(EpdiyHighlevelState), 1);
    epd_init(EPD_OPTIONS_DEFAULT);
    *hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    ctx->platform_data = hl;

    uint8_t *framebuffer = epd_hl_get_framebuffer(hl);

    epd_poweron();

    epd_fill_rect(epd_full_screen(), 255, framebuffer);

    epd_clear();
    int temperature = epd_ambient_temperature();
    epd_hl_update_screen((EpdiyHighlevelState *) ctx->platform_data, MODE_GC16, temperature);

    epd_poweroff();
}

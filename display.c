#include <stdio.h>

#include <context.h>
#include <defaultatoms.h>
#include <interop.h>
#include <mailbox.h>
#include <term.h>
#include <utils.h>

#include <epd_driver.h>
#include <epd_highlevel.h>

#include "default16px_font.h"

#define AVM_EPDIY_ENABLE_CUSTOM_FONT CONFIG_AVM_EPDIY_ENABLE_CUSTOM_FONT

static void consume_display_mailbox(Context *ctx);

#if AVM_EPDIY_ENABLE_CUSTOM_FONT == true
extern const EpdFont avm_epdiy_custom_font;
#endif

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

static void draw_text(uint8_t *framebuffer, int x, int y, const EpdFont *font, const char *text,
    uint8_t r, uint8_t g, uint8_t b, uint8_t bgr, uint8_t bgg, uint8_t bgb)
{
    if (!font) {
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
    } else {
        y += font->ascender;
        epd_write_default(font, text, &x, &y, framebuffer);
    }
}

static void execute_command(Context *ctx, term req)
{
    uint8_t *framebuffer = epd_hl_get_framebuffer((EpdiyHighlevelState *) ctx->platform_data);

    term cmd = term_get_tuple_element(req, 0);

    if (cmd == context_make_atom(ctx, "\x5"
                                      "image")) {
        int x = term_to_int(term_get_tuple_element(req, 1));
        int y = term_to_int(term_get_tuple_element(req, 2));
        int bgcolor = term_to_int(term_get_tuple_element(req, 3));
        term img = term_get_tuple_element(req, 4);

        term format = term_get_tuple_element(img, 0);

        if (format != context_make_atom(ctx, "\x8"
                                             "rgba8888")) {
            fprintf(stderr, "warning: invalid image format: ");
            term_display(stderr, format, ctx);
            fprintf(stderr, "\n");
            return;
        }

        int width = term_to_int(term_get_tuple_element(img, 1));
        int height = term_to_int(term_get_tuple_element(img, 2));
        const char *data = term_binary_data(term_get_tuple_element(img, 3));

        draw_image(framebuffer, x, y, width, height, data, (bgcolor >> 16),
            (bgcolor >> 8) & 0xFF, bgcolor & 0xFF);

    } else if (cmd == context_make_atom(ctx, "\x4"
                                             "rect")) {
        int x = term_to_int(term_get_tuple_element(req, 1));
        int y = term_to_int(term_get_tuple_element(req, 2));
        int width = term_to_int(term_get_tuple_element(req, 3));
        int height = term_to_int(term_get_tuple_element(req, 4));
        int color = term_to_int(term_get_tuple_element(req, 5));

        draw_rect(framebuffer, x, y, width, height,
            (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);

    } else if (cmd == context_make_atom(ctx, "\x4"
                                             "text")) {
        int x = term_to_int(term_get_tuple_element(req, 1));
        int y = term_to_int(term_get_tuple_element(req, 2));
        term font_name = term_get_tuple_element(req, 3);
        uint32_t fgcolor = term_to_int(term_get_tuple_element(req, 4));
        uint32_t bgcolor = term_get_tuple_element(req, 5);
        term text_term = term_get_tuple_element(req, 6);

        int ok;
        char *text = interop_term_to_string(text_term, &ok);

        const EpdFont *font = NULL;
        if (font_name != context_make_atom(ctx, "\xB"
                                                "default16px")) {
#if AVM_EPDIY_ENABLE_CUSTOM_FONT == true
            font = &avm_epdiy_custom_font;
#else
            fprintf(stderr, "unsupported font: ");
            term_display(stderr, font, ctx);
            fprintf(stderr, "\n");
#endif
        }

        draw_text(framebuffer, x, y, font, text, (fgcolor >> 16) & 0xFF, (fgcolor >> 8) & 0xFF,
            fgcolor & 0xFF, (bgcolor >> 16) & 0xFF, (bgcolor >> 8) & 0xFF, bgcolor & 0xFF);

        free(text);

    } else {
        fprintf(stderr, "unsupported display list command: ");
        term_display(stderr, req, ctx);
        fprintf(stderr, "\n");
    }
}

static void execute_commands(Context *ctx, term display_list)
{
    term t = display_list;

    while (term_is_nonempty_list(t)) {
        execute_command(ctx, term_get_list_head(t));
        t = term_get_list_tail(t);
    }
}

static void process_message(Context *ctx)
{
    Message *message = mailbox_dequeue(ctx);
    term msg = message->message;

    if (!term_is_tuple(msg) ||
            term_get_tuple_arity(msg) != 3 ||
            term_get_tuple_element(msg, 0) != context_make_atom(ctx, "\x5" "$call")) {
        goto invalid_message;
    }

    term from = term_get_tuple_element(msg, 1);
    if (!term_is_tuple(from) || term_get_tuple_arity(from) != 2) {
        goto invalid_message;
    }

    term req = term_get_tuple_element(msg, 2);
    if (!term_is_tuple(req) || term_get_tuple_arity(req) < 1) {
        goto invalid_message;
    }

    term pid = term_get_tuple_element(from, 0);
    if (!term_is_pid(pid)) {
        goto invalid_message;
    }

    term cmd = term_get_tuple_element(req, 0);

    int local_process_id = term_to_local_process_id(pid);
    Context *target = globalcontext_get_process(ctx->global, local_process_id);

    if (cmd == context_make_atom(ctx, "\x6"
                                      "update")) {
        term display_list = term_get_tuple_element(req, 1);
        execute_commands(ctx, display_list);
    } else {
        fprintf(stderr, "unsupported command: ");
        term_display(stderr, req, ctx);
        fprintf(stderr, "\n");
    }

    if (UNLIKELY(memory_ensure_free(ctx, TUPLE_SIZE(3)) != MEMORY_GC_OK)) {
        abort();
    }

    epd_poweron();
    int temperature = epd_ambient_temperature();
    epd_hl_update_screen((EpdiyHighlevelState *) ctx->platform_data, MODE_GC16, temperature);
    epd_poweroff();

    term return_tuple = term_alloc_tuple(3, ctx);
    term_put_tuple_element(return_tuple, 0, context_make_atom(ctx, "\x6" "$reply"));
    term_put_tuple_element(return_tuple, 1, from);
    term_put_tuple_element(return_tuple, 2, OK_ATOM);

    mailbox_send(target, return_tuple);

    free(message);

    return;

invalid_message:
    fprintf(stderr, "Got invalid message: ");
    term_display(stderr, msg, ctx);
    fprintf(stderr, "\n");
    fprintf(stderr, "Expected gen_server call.\n");

    free(message);
}

static void consume_display_mailbox(Context *ctx)
{
    while (!list_is_empty(&ctx->mailbox)) {
        process_message(ctx);
    }
}

Context *display_create_port(GlobalContext *global, term opts)
{
    UNUSED(opts);

    Context *ctx = context_new(global);
    if (IS_NULL_PTR(ctx)) {
        fprintf(stderr, "Out of memory.");
        return NULL;
    }
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

    return ctx;
}

void display_init(GlobalContext *global)
{
}

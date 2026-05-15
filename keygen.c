#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
#include <X11/Xft/Xft.h>
#include <mpg123.h>
#include <alsa/asoundlib.h>
#include <jpeglib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <math.h>

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480
#define KEY_LENGTH 16
#define AUDIO_BUFFER 8192

/* Global state for audio thread */
typedef struct {
    snd_pcm_t *pcm;
    mpg123_handle *mh;
    pthread_t thread;
    int running;
    int muted;
    char *mp3_file;
} audio_state_t;

static audio_state_t g_audio;

/* Global image */
static XImage *g_bg_image = NULL;

/* Background images */
static const char *bg_images[] = {
    "img/samurai.jpg",
    "img/samurai-2.jpg",
    "img/samurai-3.jpg",
    "img/samurai-4.jpg",
};
#define NUM_BG_IMAGES (sizeof(bg_images) / sizeof(bg_images[0]))
static int g_bg_index = 0;

/* Background music */
static const char *bg_music[] = {
    "sounds/retro_platforming_david_fesliyan.mp3",
    "sounds/funny_bit_david_renka.mp3",
};
#define NUM_MUSIC (sizeof(bg_music) / sizeof(bg_music[0]))
static int g_music_index = 0;

/* Audio playback thread */
static void *audio_thread(void *arg) {
    audio_state_t *state = (audio_state_t *)arg;
    unsigned char decode_buffer[AUDIO_BUFFER];
    int16_t audio_buffer[AUDIO_BUFFER / 2];
    size_t done;
    long rate;
    int channels, encoding;
    int err;

    /* Initialize mpg123 */
    mpg123_init();
    state->mh = mpg123_new(NULL, &err);
    if (!state->mh) {
        fprintf(stderr, "Failed to create mpg123 handle: %s\n", mpg123_plain_strerror(err));
        return NULL;
    }

    /* Open MP3 file */
    if (mpg123_open(state->mh, state->mp3_file) != MPG123_OK) {
        fprintf(stderr, "Failed to open MP3 file\n");
        mpg123_delete(state->mh);
        return NULL;
    }

    /* Get audio format */
    if (mpg123_getformat(state->mh, &rate, &channels, &encoding) != MPG123_OK) {
        fprintf(stderr, "Failed to get audio format\n");
        mpg123_close(state->mh);
        mpg123_delete(state->mh);
        return NULL;
    }

    printf("MP3: %ld Hz, %d channels\n", rate, channels);

    /* Configure ALSA */
    err = snd_pcm_open(&state->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "Cannot open audio device: %s\n", snd_strerror(err));
        mpg123_close(state->mh);
        mpg123_delete(state->mh);
        return NULL;
    }

    snd_pcm_format_t format = SND_PCM_FORMAT_S16;
    err = snd_pcm_set_params(state->pcm,
                             format,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             channels,
                             rate,
                             1,
                             500000);
    if (err < 0) {
        fprintf(stderr, "Cannot set audio params: %s\n", snd_strerror(err));
        snd_pcm_close(state->pcm);
        mpg123_close(state->mh);
        mpg123_delete(state->mh);
        return NULL;
    }

    /* Playback loop */
    while (state->running) {
        /* Decode MP3 frame */
        err = mpg123_read(state->mh, decode_buffer, AUDIO_BUFFER, &done);

        if (err == MPG123_DONE) {
            mpg123_seek(state->mh, 0, SEEK_SET);
            continue;
        }

        if (err == MPG123_NEW_FORMAT) {
            if (mpg123_getformat(state->mh, &rate, &channels, &encoding) != MPG123_OK) {
                continue;
            }
            snd_pcm_set_params(state->pcm, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                               channels, rate, 1, 500000);
            printf("MP3 format changed: %ld Hz, %d channels\n", rate, channels);
            continue;
        }

        if (err != MPG123_OK) {
            continue;
        }

        int samples = done / sizeof(int16_t);
        if (state->muted) {
            memset(audio_buffer, 0, done);
        } else {
            int16_t *decoded = (int16_t *)decode_buffer;
            for (int i = 0; i < samples; i++) {
                audio_buffer[i] = decoded[i] / 4;
            }
        }

        snd_pcm_sframes_t frames = snd_pcm_writei(state->pcm, audio_buffer, samples / channels);
        if (frames < 0) {
            snd_pcm_prepare(state->pcm);
        }
    }

    /* Cleanup */
    snd_pcm_drop(state->pcm);
    snd_pcm_close(state->pcm);
    mpg123_close(state->mh);
    mpg123_delete(state->mh);
    mpg123_exit();

    return NULL;
}

/* Initialize audio playback */
static int init_audio(const char *mp3_file) {
    g_audio.mp3_file = strdup(mp3_file);
    g_audio.running = 1;
    g_audio.muted = 0;
    g_audio.pcm = NULL;
    g_audio.mh = NULL;

    return pthread_create(&g_audio.thread, NULL, audio_thread, &g_audio);
}

/* Stop audio playback */
static void stop_audio(void) {
    g_audio.running = 0;
    pthread_join(g_audio.thread, NULL);
    free(g_audio.mp3_file);
}

/* Change background music */
static void change_bg_music(void) {
    g_music_index = (g_music_index + 1) % NUM_MUSIC;
    printf("Changing music to: %s\n", bg_music[g_music_index]);

    /* Stop current audio */
    g_audio.running = 0;
    pthread_join(g_audio.thread, NULL);
    free(g_audio.mp3_file);

    /* Restart with new track */
    g_audio.mp3_file = strdup(bg_music[g_music_index]);
    g_audio.running = 1;
    pthread_create(&g_audio.thread, NULL, audio_thread, &g_audio);
}

/* Load JPEG image and scale to fit window while preserving aspect ratio */
static XImage *load_jpeg_image(Display *display, int screen, const char *filename,
                                int target_width, int target_height) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *infile;
    JSAMPARRAY buffer;
    int row_stride;
    unsigned char *image_data;
    unsigned char *scaled_data;
    XImage *ximage;
    int src_width, src_height;
    float scale;
    int new_width, new_height;
    int offset_x, offset_y;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    if ((infile = fopen(filename, "rb")) == NULL) {
        fprintf(stderr, "Cannot open image file: %s\n", filename);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    jpeg_stdio_src(&cinfo, infile);
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);

    src_width = cinfo.output_width;
    src_height = cinfo.output_height;

    row_stride = cinfo.output_width * cinfo.output_components;
    buffer = (*cinfo.mem->alloc_sarray)
        ((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

    /* Allocate image data */
    image_data = malloc(src_width * src_height * 3);
    if (!image_data) {
        fclose(infile);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    /* Read image */
    int y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);
        memcpy(&image_data[y * src_width * 3], buffer[0], src_width * cinfo.output_components);
        y++;
    }

    jpeg_finish_decompress(&cinfo);
    fclose(infile);
    jpeg_destroy_decompress(&cinfo);

    /* Calculate scale to fill window while preserving aspect ratio */
    float scale_w = (float)target_width / src_width;
    float scale_h = (float)target_height / src_height;
    scale = (scale_w > scale_h) ? scale_w : scale_h;

    new_width = (int)(src_width * scale);
    new_height = (int)(src_height * scale);

    /* Center the image */
    offset_x = (target_width - new_width) / 2;
    offset_y = (target_height - new_height) / 2;

    /* Allocate scaled data initialized to black */
    scaled_data = calloc(target_width * target_height, 4);

    /* Scale image to fit window */
    for (int dy = 0; dy < new_height; dy++) {
        for (int dx = 0; dx < new_width; dx++) {
            int src_x = (int)(dx / scale);
            int src_y = (int)(dy / scale);
            if (src_x >= src_width) src_x = src_width - 1;
            if (src_y >= src_height) src_y = src_height - 1;

            int dst_x = dx + offset_x;
            int dst_y = dy + offset_y;

            if (dst_x >= 0 && dst_x < target_width && dst_y >= 0 && dst_y < target_height) {
                int src_idx = (src_y * src_width + src_x) * 3;
                int dst_idx = (dst_y * target_width + dst_x) * 4;
                scaled_data[dst_idx] = image_data[src_idx];
                scaled_data[dst_idx + 1] = image_data[src_idx + 1];
                scaled_data[dst_idx + 2] = image_data[src_idx + 2];
                scaled_data[dst_idx + 3] = 0;
            }
        }
    }

    free(image_data);

    /* Create XImage */
    Visual *visual = DefaultVisual(display, screen);
    int depth = DefaultDepth(display, screen);

    ximage = XCreateImage(display, visual, depth, ZPixmap, 0, (char *)scaled_data,
                          target_width, target_height, 32, target_width * 4);

    return ximage;
}

/* Toggle mute state */
static void toggle_mute(void) {
    g_audio.muted = !g_audio.muted;
    printf("Music: %s\n", g_audio.muted ? "OFF (muted)" : "ON (unmuted)");
}

/* Change background image */
static void change_bg_image(Display *display, int screen) {
    if (g_bg_image) {
        g_bg_image->data = NULL;
        XDestroyImage(g_bg_image);
    }

    g_bg_index = (g_bg_index + 1) % NUM_BG_IMAGES;
    g_bg_image = load_jpeg_image(display, screen, bg_images[g_bg_index], WINDOW_WIDTH, WINDOW_HEIGHT);

    if (g_bg_image) {
        printf("Background image changed to: %s\n", bg_images[g_bg_index]);
    }
}

/* Generate a random alphanumeric key */
static void generate_key(char *buffer, size_t length) {
    const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const size_t charset_len = sizeof(charset) - 1;

    for (size_t i = 0; i < length; i++) {
        buffer[i] = charset[rand() % charset_len];
    }
    buffer[length] = '\0';
}

/* Format key with dashes every 4 characters */
static void format_key(const char *key, char *formatted, size_t formatted_size) {
    size_t j = 0;
    for (size_t i = 0; key[i] && j < formatted_size - 1; i++) {
        if (i > 0 && i % 4 == 0 && j < formatted_size - 2) {
            formatted[j++] = '-';
        }
        formatted[j++] = key[i];
    }
    formatted[j] = '\0';
}

/* Draw filled button with retro style */
static void draw_retro_button(Display *display, Window window, GC gc, Font font,
                               int x, int y, int w, int h, unsigned long fg, unsigned long bg) {
    XSetForeground(display, gc, bg);
    XFillRectangle(display, window, gc, x, y, w, h);

    XSetForeground(display, gc, fg);
    XDrawRectangle(display, window, gc, x, y, w, h);

    XFontStruct *font_info = XQueryFont(display, font);
    int text_w = XTextWidth(font_info, "Generate", 8);
    int text_x = x + (w - text_w) / 2;
    XDrawString(display, window, gc, text_x, y + 25, "Generate", 8);
    XFreeFontInfo(NULL, font_info, 1);
}

int main(void) {
    Display *display;
    Window window;
    XEvent event;
    GC gc;
    XFontStruct *font;
    Colormap colormap;
    XColor red_color;
    XftDraw *xft_draw;
    XftFont *xft_font;
    XftColor xft_white;
    char key_buffer[KEY_LENGTH + 1];
    char formatted_key[KEY_LENGTH + 4 + 1];

    /* Button region */
    int btn_w = 150, btn_h = 40;
    int btn_x, btn_y;

    /* Initialize random seed */
    srand((unsigned int)time(NULL));

    /* Start MP3 playback thread */
    if (init_audio(bg_music[g_music_index]) != 0) {
        fprintf(stderr, "Failed to initialize audio\n");
    }

    /* Open display */
    display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "Error: Cannot open display\n");
        stop_audio();
        return 1;
    }

    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    colormap = DefaultColormap(display, screen);

    /* Allocate red color */
    XAllocNamedColor(display, colormap, "red", &red_color, &red_color);

    /* Load background image */
    g_bg_image = load_jpeg_image(display, screen, bg_images[g_bg_index], WINDOW_WIDTH, WINDOW_HEIGHT);
    if (!g_bg_image) {
        fprintf(stderr, "Warning: Could not load background image\n");
    }

    /* Create window */
    window = XCreateSimpleWindow(display, root, 100, 100, WINDOW_WIDTH, WINDOW_HEIGHT, 2,
                                 BlackPixel(display, screen), WhitePixel(display, screen));

    XSelectInput(display, window, ExposureMask | ButtonPressMask | KeyPressMask);
    XStoreName(display, window, "Keygen - Samurai Edition");

    /* Create graphics context */
    gc = XCreateGC(display, window, 0, NULL);

    /* Load font */
    font = XLoadQueryFont(display, "fixed");
    if (font == NULL) {
        font = XLoadQueryFont(display, "*fixed*");
    }
    if (font != NULL) {
        XSetFont(display, gc, font->fid);
    }

    btn_x = (WINDOW_WIDTH - btn_w) / 2;
    btn_y = 420;

    xft_draw = XftDrawCreate(display, window, DefaultVisual(display, screen), DefaultColormap(display, screen));
    xft_font = XftFontOpenName(display, screen, "KanjiStrokeOrders:style=Regular-12");
    if (!xft_font) {
        xft_font = XftFontOpenName(display, screen, "DejaVu Sans Mono-12");
    }
    if (!xft_font) {
        xft_font = XftFontOpenName(display, screen, "Noto Sans CJK JP-12");
    }
    XftColorAllocName(display, DefaultVisual(display, screen), DefaultColormap(display, screen), "white", &xft_white);

    /* Map window */
    XMapWindow(display, window);

    /* Generate initial key */
    generate_key(key_buffer, KEY_LENGTH);
    format_key(key_buffer, formatted_key, sizeof(formatted_key));

    printf("Generated key: %s\n", formatted_key);

    /* Event loop */
    while (1) {
        XNextEvent(display, &event);

        switch (event.type) {
            case Expose: {
                /* Draw background image */
                if (g_bg_image) {
                    XPutImage(display, window, gc, g_bg_image, 0, 0, 0, 0,
                              WINDOW_WIDTH, WINDOW_HEIGHT);
                } else {
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XFillRectangle(display, window, gc, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
                }

                /* Draw title with shadow effect */
                int title_width = XTextWidth(font, "KEY GENERATOR", 13);
                int title_x = (WINDOW_WIDTH - title_width) / 2;
                XSetForeground(display, gc, BlackPixel(display, screen));
                XDrawString(display, window, gc, title_x + 1, 31, "KEY GENERATOR", 13);
                XSetForeground(display, gc, WhitePixel(display, screen));
                XDrawString(display, window, gc, title_x, 30, "KEY GENERATOR", 13);

                /* Draw key in a retro box */
                int key_box_w = 300, key_box_h = 50;
                int key_box_x = (WINDOW_WIDTH - key_box_w) / 2;
                int key_box_y = 60;

                XSetForeground(display, gc, BlackPixel(display, screen));
                XFillRectangle(display, window, gc, key_box_x, key_box_y, key_box_w, key_box_h);

                XSetForeground(display, gc, WhitePixel(display, screen));
                XDrawRectangle(display, window, gc, key_box_x, key_box_y, key_box_w, key_box_h);

                int text_width = XTextWidth(font, formatted_key, strlen(formatted_key));
                XDrawString(display, window, gc, key_box_x + (key_box_w - text_width) / 2,
                            key_box_y + 32, formatted_key, strlen(formatted_key));

                /* Draw Generate button */
                draw_retro_button(display, window, gc, font->fid, btn_x, btn_y, btn_w, btn_h,
                                  WhitePixel(display, screen), BlackPixel(display, screen));

                if (xft_font && xft_draw) {
                    XftDrawStringUtf8(xft_draw, &xft_white, xft_font, 10, 470,
                                      (const FcChar8 *)"花は桜木人は武士", 7);
                }

                /* Draw copyright in red */
                XSetForeground(display, gc, red_color.pixel);
                XDrawString(display, window, gc, 540, 460, "(c) by sbz", 10);

                break;
            }

            case ButtonPress: {
                int x = event.xbutton.x;
                int y = event.xbutton.y;

                /* Check if Generate button was clicked */
                if (x >= btn_x && x <= btn_x + btn_w &&
                    y >= btn_y && y <= btn_y + btn_h) {
                    generate_key(key_buffer, KEY_LENGTH);
                    format_key(key_buffer, formatted_key, sizeof(formatted_key));
                    printf("Generated key: %s\n", formatted_key);

                    /* Redraw */
                    XClearWindow(display, window);

                    if (g_bg_image) {
                        XPutImage(display, window, gc, g_bg_image, 0, 0, 0, 0,
                                  WINDOW_WIDTH, WINDOW_HEIGHT);
                    }

                    int title_width = XTextWidth(font, "KEY GENERATOR", 13);
                    int title_x = (WINDOW_WIDTH - title_width) / 2;
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XDrawString(display, window, gc, title_x + 1, 31, "KEY GENERATOR", 13);
                    XSetForeground(display, gc, WhitePixel(display, screen));
                    XDrawString(display, window, gc, title_x, 30, "KEY GENERATOR", 13);

                    int key_box_w = 300, key_box_h = 50;
                    int key_box_x = (WINDOW_WIDTH - key_box_w) / 2;
                    int key_box_y = 60;
                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XFillRectangle(display, window, gc, key_box_x, key_box_y, key_box_w, key_box_h);
                    XSetForeground(display, gc, WhitePixel(display, screen));
                    XDrawRectangle(display, window, gc, key_box_x, key_box_y, key_box_w, key_box_h);

                    int text_width = XTextWidth(font, formatted_key, strlen(formatted_key));
                    XDrawString(display, window, gc, key_box_x + (key_box_w - text_width) / 2,
                                key_box_y + 32, formatted_key, strlen(formatted_key));

                    draw_retro_button(display, window, gc, font->fid, btn_x, btn_y, btn_w, btn_h,
                                      BlackPixel(display, screen), WhitePixel(display, screen));

                    XSetForeground(display, gc, red_color.pixel);
                    XDrawString(display, window, gc, 540, 460, "(c) by sbz", 10);

                    if (xft_font && xft_draw) {
                        XftDrawStringUtf8(xft_draw, &xft_white, xft_font, 10, 460,
                                          (const FcChar8 *)"花は桜木人は武士", 7);
                    }
                }

                break;
            }

            case KeyPress: {
                KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_Escape || key == XK_q || key == XK_Q) {
                    goto cleanup;
                }
                if (key == XK_m || key == XK_M) {
                    toggle_mute();
                    XEvent expose;
                    expose.type = Expose;
                    expose.xexpose.window = window;
                    XSendEvent(display, window, False, ExposureMask, &expose);
                }
                if (key == XK_s || key == XK_S) {
                    change_bg_image(display, screen);
                    XEvent expose;
                    expose.type = Expose;
                    expose.xexpose.window = window;
                    XSendEvent(display, window, False, ExposureMask, &expose);
                }
                if (key == XK_n || key == XK_N) {
                    change_bg_music();
                }
                break;
            }

            case ClientMessage:
                goto cleanup;
        }
    }

cleanup:
    stop_audio();

    if (g_bg_image) {
        g_bg_image->data = NULL;
        XDestroyImage(g_bg_image);
    }

    if (xft_draw) {
        XftDrawDestroy(xft_draw);
    }
    if (xft_font) {
        XftFontClose(display, xft_font);
    }
    if (font != NULL) {
        XFreeFont(display, font);
    }
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}

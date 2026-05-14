#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
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
            memcpy(audio_buffer, decode_buffer, done);
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

/* Load JPEG image */
static XImage *load_jpeg_image(Display *display, int screen, const char *filename,
                                int width, int height) {
    (void)width; (void)height; /* Reserved for future scaling */
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *infile;
    JSAMPARRAY buffer;
    int row_stride;
    unsigned char *image_data;
    XImage *ximage;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    if ((infile = fopen(filename, "rb")) == NULL) {
        fprintf(stderr, "Cannot open image file: %s\n", filename);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    jpeg_stdio_src(&cinfo, infile);
    jpeg_read_header(&cinfo, TRUE);

    /* Set scaling to fit target dimensions */
    cinfo.scale_num = 1;
    cinfo.scale_denom = 1;

    jpeg_start_decompress(&cinfo);

    row_stride = cinfo.output_width * cinfo.output_components;
    buffer = (*cinfo.mem->alloc_sarray)
        ((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

    /* Allocate image data (4 bytes per pixel for 32-bit) */
    image_data = malloc(cinfo.output_width * cinfo.output_height * 4);
    if (!image_data) {
        fclose(infile);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    /* Read image */
    int y = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        jpeg_read_scanlines(&cinfo, buffer, 1);

        unsigned char *ptr = buffer[0];
        for (unsigned int x = 0; x < cinfo.output_width; x++) {
            int idx = (y * cinfo.output_width + x) * 3;
            image_data[idx] = ptr[0];
            image_data[idx + 1] = ptr[1];
            image_data[idx + 2] = ptr[2];
            ptr += cinfo.output_components;
        }
        y++;
    }

    /* Create XImage with proper color handling */
    Visual *visual = DefaultVisual(display, screen);
    int depth = DefaultDepth(display, screen);

    /* Convert from RGB planar to 32-bit packed pixels */
    unsigned char *temp = malloc(cinfo.output_width * cinfo.output_height * 4);
    for (JDIMENSION i = 0; i < cinfo.output_width * cinfo.output_height; i++) {
        unsigned char r = image_data[i * 3];
        unsigned char g = image_data[i * 3 + 1];
        unsigned char b = image_data[i * 3 + 2];
        /* Store as RGB in 32-bit pixels */
        temp[i * 4] = r;
        temp[i * 4 + 1] = g;
        temp[i * 4 + 2] = b;
        temp[i * 4 + 3] = 0;
    }
    free(image_data);

    ximage = XCreateImage(display, visual, depth, ZPixmap, 0, (char *)temp,
                          cinfo.output_width, cinfo.output_height, 32, cinfo.output_width * 4);

    jpeg_finish_decompress(&cinfo);
    fclose(infile);
    jpeg_destroy_decompress(&cinfo);

    return ximage;
}

/* Toggle mute state */
static void toggle_mute(void) {
    g_audio.muted = !g_audio.muted;
    printf("Music: %s\n", g_audio.muted ? "OFF (muted)" : "ON (unmuted)");
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
static void draw_retro_button(Display *display, Window window, GC gc, int x, int y,
                               int w, int h, unsigned long fg, unsigned long bg) {
    XSetForeground(display, gc, bg);
    XFillRectangle(display, window, gc, x, y, w, h);

    XSetForeground(display, gc, fg);
    XDrawRectangle(display, window, gc, x, y, w, h);

    XDrawString(display, window, gc, x + 35, y + 25, "Generate", 8);
}

int main(void) {
    Display *display;
    Window window;
    XEvent event;
    GC gc;
    XFontStruct *font;
    char key_buffer[KEY_LENGTH + 1];
    char formatted_key[KEY_LENGTH + 4 + 1];
    /* MP3 file path */
    const char *mp3_file = "sounds/retro_platforming_david_fesliyan.mp3";

    /* Button region */
    int btn_x = 245, btn_y = 420, btn_w = 150, btn_h = 40;

    /* Initialize random seed */
    srand((unsigned int)time(NULL));

    /* Start MP3 playback thread */
    if (init_audio(mp3_file) != 0) {
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

    /* Load background image */
    g_bg_image = load_jpeg_image(display, screen, "img/samurai.jpg", WINDOW_WIDTH, WINDOW_HEIGHT);
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
                XSetForeground(display, gc, BlackPixel(display, screen));
                XDrawString(display, window, gc, 321, 31, "KEY GENERATOR", 13);
                XSetForeground(display, gc, WhitePixel(display, screen));
                XDrawString(display, window, gc, 320, 30, "KEY GENERATOR", 13);

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
                draw_retro_button(display, window, gc, btn_x, btn_y, btn_w, btn_h,
                                  BlackPixel(display, screen), WhitePixel(display, screen));

                /* Draw copyright */
                XSetForeground(display, gc, WhitePixel(display, screen));
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

                    XSetForeground(display, gc, BlackPixel(display, screen));
                    XDrawString(display, window, gc, 321, 31, "KEY GENERATOR", 13);
                    XSetForeground(display, gc, WhitePixel(display, screen));
                    XDrawString(display, window, gc, 320, 30, "KEY GENERATOR", 13);

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

                    draw_retro_button(display, window, gc, btn_x, btn_y, btn_w, btn_h,
                                      BlackPixel(display, screen), WhitePixel(display, screen));

                    XSetForeground(display, gc, WhitePixel(display, screen));
                    XDrawString(display, window, gc, 540, 460, "(c) by sbz", 10);
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
                    /* Trigger redraw by generating an Expose event */
                    XEvent expose;
                    expose.type = Expose;
                    expose.xexpose.window = window;
                    XSendEvent(display, window, False, ExposureMask, &expose);
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
        g_bg_image->data = NULL; /* Data already freed with image */
        XDestroyImage(g_bg_image);
    }

    if (font != NULL) {
        XFreeFont(display, font);
    }
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}

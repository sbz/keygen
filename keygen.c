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
#include <setjmp.h>
#include <stdint.h>
#include <stdatomic.h>
#include <errno.h>
#if defined(__linux__) || defined(__FreeBSD__)
#include <sys/random.h>
#endif
#ifdef __FreeBSD__
#include <fcntl.h>
#include <unistd.h>
#include <sys/soundcard.h>
#include <sys/ioctl.h>
#endif

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480
#define KEY_LENGTH 16
#define AUDIO_BUFFER 8192

#define SBZ_STR "(c) by sbz"
#define KANJI_STR "花は桜木人は武士"

/* Global state for audio thread */
typedef struct {
    snd_pcm_t *alsa_pcm;
#ifdef __FreeBSD__
    int oss_fd;
    int use_oss;
#endif
    mpg123_handle *mh;
    pthread_t thread;
    atomic_int running;
    atomic_int muted;
    char *mp3_file;
} audio_state_t;

static audio_state_t g_audio;
static pthread_mutex_t g_audio_lock = PTHREAD_MUTEX_INITIALIZER;

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
    long latency_us = 50000;
    int channels, encoding;
    int err;

    state->mh = mpg123_new(NULL, &err);
    if (!state->mh) {
        fprintf(stderr, "Failed to create mpg123 handle: %s\n", mpg123_plain_strerror(err));
        state->running = 0;
        return NULL;
    }

    /* Open MP3 file */
    if (mpg123_open(state->mh, state->mp3_file) != MPG123_OK) {
        fprintf(stderr, "Failed to open MP3 file\n");
        mpg123_delete(state->mh);
        state->running = 0;
        return NULL;
    }

    /* Get audio format */
    if (mpg123_getformat(state->mh, &rate, &channels, &encoding) != MPG123_OK) {
        fprintf(stderr, "Failed to get audio format\n");
        mpg123_close(state->mh);
        mpg123_delete(state->mh);
        state->running = 0;
        return NULL;
    }

    printf("MP3: %ld Hz, %d channels\n", rate, channels);

    /* Configure ALSA */
    err = snd_pcm_open(&state->alsa_pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
#ifdef __FreeBSD__
        fprintf(stderr, "ALSA not available, trying OSS: %s\n", snd_strerror(err));
        state->alsa_pcm = NULL;
        state->use_oss = 1;

        state->oss_fd = open("/dev/dsp", O_WRONLY, 0);
        if (state->oss_fd < 0) {
            fprintf(stderr, "Cannot open OSS device /dev/dsp\n");
            mpg123_close(state->mh);
            mpg123_delete(state->mh);
            state->running = 0;
            return NULL;
        }

        int fmt = AFMT_S16_LE;
        if (ioctl(state->oss_fd, SNDCTL_DSP_SETFMT, &fmt) < 0) {
            fprintf(stderr, "Cannot set OSS format\n");
            close(state->oss_fd);
            mpg123_close(state->mh);
            mpg123_delete(state->mh);
            state->running = 0;
            return NULL;
        }

        int oss_rate = rate;
        if (ioctl(state->oss_fd, SNDCTL_DSP_SPEED, &oss_rate) < 0) {
            fprintf(stderr, "Cannot set OSS sample rate\n");
            close(state->oss_fd);
            mpg123_close(state->mh);
            mpg123_delete(state->mh);
            state->running = 0;
            return NULL;
        }

        int oss_channels = channels;
        if (ioctl(state->oss_fd, SNDCTL_DSP_CHANNELS, &oss_channels) < 0) {
            fprintf(stderr, "Cannot set OSS channels\n");
            close(state->oss_fd);
            mpg123_close(state->mh);
            mpg123_delete(state->mh);
            state->running = 0;
            return NULL;
        }

        printf("OSS: %ld Hz, %d channels\n", rate, channels);
#else
        fprintf(stderr, "Cannot open audio device: %s\n", snd_strerror(err));
        mpg123_close(state->mh);
        mpg123_delete(state->mh);
        return NULL;
#endif
    } else {
        /* Configure ALSA */
        snd_pcm_format_t format = SND_PCM_FORMAT_S16;
        err = snd_pcm_set_params(state->alsa_pcm,
                                 format,
                                 SND_PCM_ACCESS_RW_INTERLEAVED,
                                 channels,
                                 rate,
                                 1,
                                 latency_us);
        if (err < 0) {
            fprintf(stderr, "Cannot set audio params: %s\n", snd_strerror(err));
            snd_pcm_close(state->alsa_pcm);
            state->alsa_pcm = NULL;
            mpg123_close(state->mh);
            mpg123_delete(state->mh);
            state->mh = NULL;
            state->running = 0;
            return NULL;
        }
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
#ifdef __FreeBSD__
            if (state->use_oss) {
                /* OSS doesn't support dynamic format change, skip */
            } else
#endif
            if (state->alsa_pcm) {
                snd_pcm_set_params(state->alsa_pcm, SND_PCM_FORMAT_S16,
                                   SND_PCM_ACCESS_RW_INTERLEAVED, channels, rate, 1, latency_us);
            }
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

#ifdef __FreeBSD__
        if (state->use_oss) {
            write(state->oss_fd, audio_buffer, done);
        } else
#endif
        {
            snd_pcm_sframes_t frames = snd_pcm_writei(state->alsa_pcm, audio_buffer, samples / channels);
            if (frames < 0) {
                snd_pcm_prepare(state->alsa_pcm);
            }
        }
    }

    /* Cleanup */
#ifdef __FreeBSD__
    if (state->use_oss) {
        close(state->oss_fd);
    } else
#endif
    {
        snd_pcm_drop(state->alsa_pcm);
        snd_pcm_close(state->alsa_pcm);
    }
    mpg123_close(state->mh);
    mpg123_delete(state->mh);

    return NULL;
}

/* Initialize audio playback */
static int init_audio(const char *mp3_file) {
    g_audio.mp3_file = strdup(mp3_file);
    if (!g_audio.mp3_file) {
        return -1;
    }
    g_audio.running = 1;
    g_audio.muted = 0;
    g_audio.alsa_pcm = NULL;
#ifdef __FreeBSD__
    g_audio.oss_fd = -1;
    g_audio.use_oss = 0;
#endif
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
    pthread_mutex_lock(&g_audio_lock);

    g_music_index = (g_music_index + 1) % NUM_MUSIC;
    printf("Changing music to: %s\n", bg_music[g_music_index]);

    /* Stop current audio */
    g_audio.running = 0;
    pthread_join(g_audio.thread, NULL);
    free(g_audio.mp3_file);
    g_audio.mp3_file = NULL;

    /* Restart with new track */
    char *next = strdup(bg_music[g_music_index]);
    if (!next) {
        fprintf(stderr, "Out of memory selecting next track\n");
        pthread_mutex_unlock(&g_audio_lock);
        return;
    }
    g_audio.mp3_file = next;
    g_audio.running = 1;
    if (pthread_create(&g_audio.thread, NULL, audio_thread, &g_audio) != 0) {
        fprintf(stderr, "Failed to start audio thread\n");
        free(g_audio.mp3_file);
        g_audio.mp3_file = NULL;
        g_audio.running = 0;
    }

    pthread_mutex_unlock(&g_audio_lock);
}

/* Custom libjpeg error handler that longjmps instead of calling exit() */
struct keygen_jpeg_err {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void keygen_jpeg_error_exit(j_common_ptr cinfo) {
    struct keygen_jpeg_err *err = (struct keygen_jpeg_err *)cinfo->err;
    (*cinfo->err->output_message)(cinfo);
    longjmp(err->setjmp_buffer, 1);
}

/* Load JPEG image and scale to fit window while preserving aspect ratio */
static XImage *load_jpeg_image(Display *display, int screen, const char *filename,
                                int target_width, int target_height) {
    struct jpeg_decompress_struct cinfo;
    struct keygen_jpeg_err jerr;
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

    image_data = NULL;
    scaled_data = NULL;
    infile = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = keygen_jpeg_error_exit;
    if (setjmp(jerr.setjmp_buffer)) {
        /* libjpeg signaled a fatal error */
        jpeg_destroy_decompress(&cinfo);
        if (infile) fclose(infile);
        free(image_data);
        free(scaled_data);
        fprintf(stderr, "JPEG decode failed: %s\n", filename);
        return NULL;
    }
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

    /* Reject pathological dimensions to avoid size_t overflow below */
    if (src_width <= 0 || src_height <= 0 ||
        src_width > 16384 || src_height > 16384 ||
        (size_t)src_width > SIZE_MAX / 3 / (size_t)src_height) {
        fprintf(stderr, "JPEG dimensions out of range: %dx%d\n", src_width, src_height);
        fclose(infile);
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }

    row_stride = cinfo.output_width * cinfo.output_components;
    buffer = (*cinfo.mem->alloc_sarray)
        ((j_common_ptr)&cinfo, JPOOL_IMAGE, row_stride, 1);

    /* Allocate image data */
    image_data = malloc((size_t)src_width * (size_t)src_height * 3);
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
    scaled_data = calloc((size_t)target_width * (size_t)target_height, 4);
    if (!scaled_data) {
        free(image_data);
        return NULL;
    }

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
        XDestroyImage(g_bg_image);
    }

    g_bg_index = (g_bg_index + 1) % NUM_BG_IMAGES;
    g_bg_image = load_jpeg_image(display, screen, bg_images[g_bg_index], WINDOW_WIDTH, WINDOW_HEIGHT);

    if (g_bg_image) {
        printf("Background image changed to: %s\n", bg_images[g_bg_index]);
    }
}

/* Fill buf with `n` random bytes from the OS CSPRNG, falling back to rand() */
static void fill_random_bytes(unsigned char *buf, size_t n) {
#if defined(__linux__) || defined(__FreeBSD__)
    size_t off = 0;
    while (off < n) {
        ssize_t r = getrandom(buf + off, n - off, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (size_t)r;
    }
    if (off == n) return;
#endif
    for (size_t i = 0; i < n; i++) {
        buf[i] = (unsigned char)(rand() & 0xff);
    }
}

/* Generate a random alphanumeric key with uniform distribution */
static void generate_key(char *buffer, size_t length) {
    static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static const size_t charset_len = sizeof(charset) - 1; /* 36 */
    /* 252 is the largest multiple of 36 <= 256; bytes >= 252 are discarded
       so we get a uniform distribution over the charset. */
    static const unsigned int cutoff = 252;

    size_t i = 0;
    while (i < length) {
        unsigned char block[64];
        fill_random_bytes(block, sizeof(block));
        for (size_t j = 0; j < sizeof(block) && i < length; j++) {
            if (block[j] < cutoff) {
                buffer[i++] = charset[block[j] % charset_len];
            }
        }
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
static void draw_retro_button(Display *display, Window window, GC gc, XFontStruct *font,
                               int x, int y, int w, int h, unsigned long fg, unsigned long bg) {
    XSetForeground(display, gc, bg);
    XFillRectangle(display, window, gc, x, y, w, h);

    XSetForeground(display, gc, fg);
    XDrawRectangle(display, window, gc, x, y, w, h);

    int text_w = XTextWidth(font, "Generate", 8);
    int text_x = x + (w - text_w) / 2;
    XDrawString(display, window, gc, text_x, y + 25, "Generate", 8);
}

/* Render the full UI. Caller owns all the resources passed in. */
static void redraw_window(Display *display, int screen, Window window, GC gc,
                          XFontStruct *font, XftDraw *xft_draw, XftFont *xft_font,
                          XftColor *xft_white, XColor *red_color,
                          int btn_x, int btn_y, int btn_w, int btn_h,
                          const char *formatted_key) {
    if (g_bg_image) {
        XPutImage(display, window, gc, g_bg_image, 0, 0, 0, 0,
                  WINDOW_WIDTH, WINDOW_HEIGHT);
    } else {
        XSetForeground(display, gc, BlackPixel(display, screen));
        XFillRectangle(display, window, gc, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
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

    draw_retro_button(display, window, gc, font, btn_x, btn_y, btn_w, btn_h,
                      WhitePixel(display, screen), BlackPixel(display, screen));

    if (xft_font && xft_draw) {
        XftDrawStringUtf8(xft_draw, xft_white, xft_font, 10, 450,
                          (const FcChar8 *)KANJI_STR, 21);
    }

    XSetForeground(display, gc, red_color->pixel);
    XDrawString(display, window, gc, 540, 450, SBZ_STR, 10);
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

    /* Initialize mpg123 once for the lifetime of the process */
    mpg123_init();

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

    /* Allocate red color (fall back to a fixed RGB pixel if the named lookup fails) */
    if (!XAllocNamedColor(display, colormap, "red", &red_color, &red_color)) {
        red_color.pixel = 0xFF0000;
    }

    /* Load background image */
    g_bg_image = load_jpeg_image(display, screen, bg_images[rand() % NUM_BG_IMAGES], WINDOW_WIDTH, WINDOW_HEIGHT);
    if (!g_bg_image) {
        fprintf(stderr, "Warning: Could not load background image\n");
    }

    /* Create window */
    window = XCreateSimpleWindow(display, root, 100, 100, WINDOW_WIDTH, WINDOW_HEIGHT, 2,
                                 BlackPixel(display, screen), WhitePixel(display, screen));

    XSelectInput(display, window, ExposureMask | ButtonPressMask | KeyPressMask);
    XStoreName(display, window, "Keygen - Samurai Edition");

    /* Register for WM close button so we can exit cleanly */
    Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display, window, &wm_delete, 1);

    /* Create graphics context */
    gc = XCreateGC(display, window, 0, NULL);

    /* Load font */
    font = XLoadQueryFont(display, "fixed");
    if (font == NULL) {
        font = XLoadQueryFont(display, "*fixed*");
    }
    if (font == NULL) {
        fprintf(stderr, "Error: cannot load any X11 font\n");
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        stop_audio();
        return 1;
    }
    XSetFont(display, gc, font->fid);

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
            case Expose:
                redraw_window(display, screen, window, gc, font,
                              xft_draw, xft_font, &xft_white, &red_color,
                              btn_x, btn_y, btn_w, btn_h, formatted_key);
                break;

            case ButtonPress: {
                int x = event.xbutton.x;
                int y = event.xbutton.y;

                if (x >= btn_x && x <= btn_x + btn_w &&
                    y >= btn_y && y <= btn_y + btn_h) {
                    generate_key(key_buffer, KEY_LENGTH);
                    format_key(key_buffer, formatted_key, sizeof(formatted_key));
                    printf("Generated key: %s\n", formatted_key);

                    redraw_window(display, screen, window, gc, font,
                                  xft_draw, xft_font, &xft_white, &red_color,
                                  btn_x, btn_y, btn_w, btn_h, formatted_key);
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
                if ((Atom)event.xclient.data.l[0] == wm_delete) {
                    goto cleanup;
                }
                break;
        }
    }

cleanup:
    stop_audio();
    mpg123_exit();

    if (g_bg_image) {
        XDestroyImage(g_bg_image);
    }

    if (xft_draw) {
        XftDrawDestroy(xft_draw);
    }
    if (xft_font) {
        XftFontClose(display, xft_font);
    }
    XFreeFont(display, font);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    return 0;
}

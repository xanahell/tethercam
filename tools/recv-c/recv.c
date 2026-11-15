/* usbcam-recv-c — portable IUCM receiver for Windows, macOS and Linux.
 *
 * Talks to a TetherCam iPhone over usbmuxd (or plain TCP for the simulator),
 * runs the HELLO / START / CONFIG / VIDEO handshake of protocol/PROTOCOL.md,
 * and writes the HEVC stream as Annex-B to a file. Built on shared/ only, so
 * it is also the smallest proof that the Winsock path of usbmux.c works.
 * C counterpart of tools/scripts/usbmux_recv.py. MIT, (c) 2026 Bernhard
 * Goetzendorfer.
 *
 *   usbcam-recv-c --list
 *   usbcam-recv-c [--serial UDID] [--port 7878] [--seconds 5] [--dump out.hevc]
 *                 [--size 1920x1080] [--fps 30] [--bitrate 12000] [--camera 0]
 *                 [--tcp HOST:PORT] [--timeout 5]
 *
 * Exit 0 when at least one frame arrived, 1 on any protocol or link error,
 * 2 on a usage error.
 */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <time.h>
#endif

#include <ctype.h>
#include <stdarg.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame_parser.h"
#include "usbmux.h"

#define MAX_DEVICES 16
#define PING_INTERVAL_US 2000000ull
#define DEAD_LINK_US 6000000ull /* PROTOCOL.md 4.3: three missed PONGs */
#define PARAMSET_CAP 4096

static const uint8_t START_CODE[4] = {0, 0, 0, 1};

struct opts {
    const char *serial;
    const char *dump_path;
    const char *tcp;
    unsigned    port;
    double      seconds;
    double      hello_timeout;
    long        width, height, fps, bitrate, camera;
    int         list_only;
};

struct rx {
    const struct opts *o;
    int                fd;
    FILE              *dump;

    int      hello_seen, config_seen;
    uint16_t version;

    uint8_t ps[PARAMSET_CAP]; /* VPS/SPS/PPS from hvcC, Annex-B framed */
    size_t  ps_len;

    uint64_t frames, keyframes, nals, video_bytes;
    uint64_t first_pts, last_pts;
    int      have_pts;
    uint64_t deadline_us; /* 0 until the first CONFIG */

    int  done;   /* the requested capture time is over */
    int  failed; /* err holds the reason */
    char err[256];
};

static uint64_t now_us(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    /* split to keep the multiplication from overflowing after long uptimes */
    return (uint64_t)(cnt.QuadPart / freq.QuadPart) * 1000000ull +
           (uint64_t)(cnt.QuadPart % freq.QuadPart) * 1000000ull / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
#endif
}

static void fail(struct rx *r, const char *fmt, ...) {
    va_list ap;
    if (r->failed) return; /* keep the first reason */
    va_start(ap, fmt);
    vsnprintf(r->err, sizeof(r->err), fmt, ap);
    va_end(ap);
    r->failed = 1;
}

static int ieq(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* ---------------------------------------------------------------- hvcC */

/* hvcC: 22 fixed bytes, numOfArrays, then per array one type byte, a u16 NAL
 * count and u16-length-prefixed NALs (ISO/IEC 14496-15). Returns the number of
 * parameter sets written to out as Annex-B, or -1. */
static int hvcc_to_annexb(const uint8_t *h, uint32_t len, uint8_t *out, size_t cap,
                          size_t *out_len) {
    uint32_t off = 22;
    size_t   w   = 0;
    int      count = 0;
    unsigned arrays;
    if (len <= off) return -1;
    arrays = h[off++];
    while (arrays--) {
        unsigned nn;
        if (off + 3 > len) return -1;
        off += 1; /* completeness / NAL unit type */
        nn = ((unsigned)h[off] << 8) | h[off + 1];
        off += 2;
        while (nn--) {
            uint32_t n;
            if (off + 2 > len) return -1;
            n = ((uint32_t)h[off] << 8) | h[off + 1];
            off += 2;
            if (off + n > len || w + 4 + n > cap) return -1;
            memcpy(out + w, START_CODE, 4);
            memcpy(out + w + 4, h + off, n);
            w += 4 + n;
            off += n;
            count++;
        }
    }
    *out_len = w;
    return count ? count : -1;
}

/* ------------------------------------------------------- message handlers */

static int send_all(struct rx *r, const uint8_t *buf, size_t n) {
    if (usbmux_io_write_all(r->fd, buf, n) != USBMUX_OK) {
        fail(r, "write to the device failed");
        return -1;
    }
    return 0;
}

static int on_hello(struct rx *r, const struct iucm_msg *m) {
    struct iucm_hello h;
    uint8_t           out[128];
    size_t            n = 0;
    struct iucm_start s;
    int               i, rc = iucm_parse_hello(m->payload, m->length, &h);
    if (rc != IUCM_OK) {
        fail(r, "bad HELLO: %s", iucm_strerror(rc));
        return 1;
    }
    r->hello_seen = 1;
    r->version    = h.version;
    printf("HELLO   version=0x%04x device='%s' app=%s cameras=%u", h.version, h.name,
           h.app_version, h.camera_count);
    for (i = 0; i < h.camera_count; i++)
        printf(" [%u:%s:%s]", h.cameras[i].id,
               h.cameras[i].position == IUCM_CAMERA_FRONT ? "front" : "back",
               h.cameras[i].name);
    printf("\n");
    if (IUCM_VERSION_MAJOR(h.version) != 1) {
        fail(r, "unsupported protocol major version %u", IUCM_VERSION_MAJOR(h.version));
        return 1;
    }

    /* CLIENT_INFO is optional and only understood from 1.2 (PROTOCOL.md 4.11):
     * an older app would count it as an unknown frame at best. */
    if (IUCM_VERSION_MINOR(h.version) >= 2) {
        struct iucm_client_info ci;
        memset(&ci, 0, sizeof(ci));
        ci.kind = IUCM_CLIENT_TOOL;
        strcpy(ci.name, "usbcam-recv-c");
        strcpy(ci.version, "0.1");
        if (iucm_encode_client_info(out, sizeof(out), &ci, &n) != IUCM_OK) {
            fail(r, "cannot encode CLIENT_INFO");
            return 1;
        }
        if (send_all(r, out, n)) return 1;
    }

    /* flags stay 0: this tool never asks for audio, so the 11-byte START that
     * every app version accepts is what goes out (PROTOCOL.md 4.2). */
    memset(&s, 0, sizeof(s));
    s.camera_id    = (uint8_t)r->o->camera;
    s.width        = (uint16_t)r->o->width;
    s.height       = (uint16_t)r->o->height;
    s.fps          = (uint16_t)r->o->fps;
    s.bitrate_kbps = (uint32_t)r->o->bitrate;
    if (iucm_encode_start(out, sizeof(out), &s, &n) != IUCM_OK) {
        fail(r, "cannot encode START");
        return 1;
    }
    if (send_all(r, out, n)) return 1;
    printf("START   camera=%ld %ldx%ld@%ld %ld kbps\n", r->o->camera, r->o->width,
           r->o->height, r->o->fps, r->o->bitrate);
    return 0;
}

static int on_config(struct rx *r, const struct iucm_msg *m) {
    struct iucm_config c;
    int                rc = iucm_parse_config(m->payload, m->length, &c);
    if (rc != IUCM_OK) {
        fail(r, "bad CONFIG: %s", iucm_strerror(rc));
        return 1;
    }
    printf("CONFIG  %ux%u@%u hvcc_len=%u\n", c.width, c.height, c.fps, c.hvcc_len);
    if (hvcc_to_annexb(c.hvcc, c.hvcc_len, r->ps, sizeof(r->ps), &r->ps_len) < 0) {
        fail(r, "hvcC carries no usable parameter sets");
        return 1;
    }
    r->config_seen = 1;
    if (r->deadline_us == 0) r->deadline_us = now_us() + (uint64_t)(r->o->seconds * 1e6);
    return 0;
}

static int on_video(struct rx *r, const struct iucm_msg *m) {
    struct iucm_video_iter it;
    const uint8_t         *nal;
    uint32_t               n;
    int                    rc;
    if (!r->config_seen) {
        fail(r, "VIDEO before CONFIG");
        return 1;
    }
    rc = iucm_video_iter_init(&it, m->payload, m->length);
    if (rc != IUCM_OK) {
        fail(r, "bad VIDEO payload: %s", iucm_strerror(rc));
        return 1;
    }
    if (r->have_pts && it.pts_us <= r->last_pts) {
        fail(r, "pts not monotonic: %" PRIu64 " after %" PRIu64, it.pts_us, r->last_pts);
        return 1;
    }
    if (!r->have_pts) r->first_pts = it.pts_us;
    r->have_pts = 1;
    r->last_pts = it.pts_us;

    /* The parameter sets travel once, in hvcC: a keyframe gets them in front. */
    if ((m->flags & IUCM_FLAG_KEYFRAME) && r->dump &&
        fwrite(r->ps, 1, r->ps_len, r->dump) != r->ps_len) {
        fail(r, "cannot write the dump file");
        return 1;
    }
    while ((rc = iucm_video_iter_next(&it, &nal, &n)) > 0) {
        if (r->dump && (fwrite(START_CODE, 1, 4, r->dump) != 4 ||
                        fwrite(nal, 1, n, r->dump) != n)) {
            fail(r, "cannot write the dump file");
            return 1;
        }
        r->nals++;
    }
    if (rc < 0) {
        fail(r, "VIDEO payload is not 4-byte big-endian length prefixed");
        return 1;
    }
    r->frames++;
    r->video_bytes += m->length - 8;
    if (m->flags & IUCM_FLAG_KEYFRAME) r->keyframes++;
    if (r->deadline_us && now_us() >= r->deadline_us) {
        r->done = 1;
        return 1; /* stop feeding: the capture time is over */
    }
    return 0;
}

static int on_error(struct rx *r, const struct iucm_msg *m) {
    struct iucm_error e;
    if (iucm_parse_error(m->payload, m->length, &e) != IUCM_OK) {
        fail(r, "ERROR frame from the phone (unreadable)");
    } else {
        const char *hint = e.code == IUCM_ERRCODE_BUSY ? " (another receiver holds the phone)"
                         : e.code == IUCM_ERRCODE_CAMERA_DENIED ? " (camera permission)"
                         : "";
        fail(r, "ERROR %u from the phone: %s%s", e.code, e.text, hint);
    }
    return 1;
}

static int on_msg(void *ctx, const struct iucm_msg *m) {
    struct rx *r = (struct rx *)ctx;
    switch (m->type) {
    case IUCM_MSG_HELLO:  return on_hello(r, m);
    case IUCM_MSG_CONFIG: return on_config(r, m);
    case IUCM_MSG_VIDEO:  return on_video(r, m);
    case IUCM_MSG_ERROR:  return on_error(r, m);
    default:              return 0; /* STATS, PONG, AUDIO*: not needed here */
    }
}

/* ---------------------------------------------------------------- session */

static int run_session(struct rx *r) {
    struct iucm_parser p;
    uint8_t           *pbuf;
    uint8_t            chunk[65536];
    uint64_t           start = now_us(), last_ping = 0, last_rx = start;
    size_t             cap   = IUCM_HEADER_SIZE + IUCM_MAX_PAYLOAD;

    pbuf = (uint8_t *)malloc(cap);
    if (!pbuf || iucm_parser_init(&p, pbuf, cap) != IUCM_OK) {
        free(pbuf);
        fail(r, "out of memory");
        return 1;
    }

    for (;;) {
        uint64_t t = now_us();
        int      w, n, rc;

        if (r->hello_seen && t - last_ping >= PING_INTERVAL_US) {
            uint8_t ping[IUCM_HEADER_SIZE + 8];
            size_t  len = 0;
            if (iucm_encode_ping(ping, sizeof(ping), t, &len) != IUCM_OK ||
                send_all(r, ping, len))
                break;
            last_ping = t;
        }
        if (!r->hello_seen && (double)(t - start) / 1e6 > r->o->hello_timeout) {
            fail(r, "no HELLO within %.0f s (is TetherCam open and in the foreground on the phone?)",
                 r->o->hello_timeout);
            break;
        }

        w = usbmux_io_wait_readable(r->fd, 500);
        if (w < 0) {
            fail(r, "waiting for data failed");
            break;
        }
        if (w == 0) {
            if (t - last_rx > DEAD_LINK_US) {
                fail(r, "no data from the phone for 6 s");
                break;
            }
            continue;
        }
        n = usbmux_io_read(r->fd, chunk, sizeof(chunk));
        if (n == 0) {
            fail(r, "connection closed by the phone");
            break;
        }
        if (n < 0) {
            fail(r, "read from the phone failed");
            break;
        }
        last_rx = now_us();

        rc = iucm_parser_feed(&p, chunk, (size_t)n, on_msg, r);
        if (r->failed || r->done) break;
        if (rc != IUCM_OK) {
            fail(r, "framing error: %s", iucm_strerror(rc));
            break;
        }
    }
    free(pbuf);
    return r->failed ? 1 : 0;
}

/* ------------------------------------------------------------------- main */

static int usage(void) {
    fprintf(stderr,
            "usage: usbcam-recv-c [--list] [--serial UDID] [--port 7878] [--seconds 5]\n"
            "                     [--dump out.hevc] [--size 1920x1080] [--fps 30]\n"
            "                     [--bitrate 12000] [--camera 0] [--tcp HOST:PORT]\n"
            "                     [--timeout 5]\n");
    return 2;
}

static int parse_size(const char *s, long *w, long *h) {
    char *end;
    long  a = strtol(s, &end, 10), b;
    if (end == s || (*end != 'x' && *end != 'X')) return -1;
    s = end + 1;
    b = strtol(s, &end, 10);
    if (end == s || *end != '\0' || a <= 0 || b <= 0 || a > 65535 || b > 65535) return -1;
    *w = a;
    *h = b;
    return 0;
}

static int parse_args(int argc, char **argv, struct opts *o) {
    int i;
    memset(o, 0, sizeof(*o));
    o->port          = 7878;
    o->seconds       = 5.0;
    o->hello_timeout = 5.0;
    o->width = 1920, o->height = 1080, o->fps = 30, o->bitrate = 12000;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--list") == 0) {
            o->list_only = 1;
            continue;
        }
        if (!v) return -1;
        if (strcmp(a, "--serial") == 0) o->serial = v;
        else if (strcmp(a, "--dump") == 0) o->dump_path = v;
        else if (strcmp(a, "--tcp") == 0) o->tcp = v;
        else if (strcmp(a, "--port") == 0) o->port = (unsigned)strtoul(v, NULL, 10);
        else if (strcmp(a, "--seconds") == 0) o->seconds = strtod(v, NULL);
        else if (strcmp(a, "--timeout") == 0) o->hello_timeout = strtod(v, NULL);
        else if (strcmp(a, "--fps") == 0) o->fps = strtol(v, NULL, 10);
        else if (strcmp(a, "--bitrate") == 0) o->bitrate = strtol(v, NULL, 10);
        else if (strcmp(a, "--camera") == 0) o->camera = strtol(v, NULL, 10);
        else if (strcmp(a, "--size") == 0) {
            if (parse_size(v, &o->width, &o->height)) return -1;
        } else return -1;
        i++;
    }
    if (o->port == 0 || o->port > 65535 || o->seconds <= 0 || o->fps <= 0 || o->fps > 65535 ||
        o->bitrate <= 0 || o->camera < 0 || o->camera > 255)
        return -1;
    return 0;
}

static int open_link(const struct opts *o, struct rx *r) {
    struct usbmux_device devs[MAX_DEVICES];
    size_t               count = 0, i, pick = 0;
    int                  rc, result = -1;

    if (o->tcp) {
        r->fd = usbmux_connect_tcp(o->tcp);
        if (r->fd < 0) {
            fprintf(stderr, "error: cannot connect to %s\n", o->tcp);
            return 1;
        }
        printf("LINK    plain TCP to %s\n", o->tcp);
        return 0;
    }

    rc = usbmux_list_devices(devs, MAX_DEVICES, &count);
    if (rc != USBMUX_OK) {
        fprintf(stderr, "error: cannot query usbmuxd (%s)\n", usbmux_strerror(rc));
#ifdef _WIN32
        fprintf(stderr,
                "       Windows needs Apple Mobile Device Service: install iTunes or the\n"
                "       Apple Devices app from the Microsoft Store, then plug the phone in.\n");
#else
        fprintf(stderr, "       is usbmuxd running?\n");
#endif
        return 1;
    }
    if (o->list_only) {
        for (i = 0; i < count; i++)
            printf("DeviceID %-5u serial %s ProductID %u\n", devs[i].device_id, devs[i].serial,
                   devs[i].product_id);
        if (count == 0) printf("no USB device attached\n");
        return -1; /* handled: nothing more to do */
    }
    if (count == 0) {
        fprintf(stderr, "error: no USB device attached (cable, Trust prompt, unlocked phone?)\n");
        return 1;
    }
    if (o->serial) {
        for (i = 0; i < count; i++)
            if (ieq(devs[i].serial, o->serial)) break;
        if (i == count) {
            fprintf(stderr, "error: serial %s not among the %u attached device(s)\n", o->serial,
                    (unsigned)count);
            return 1;
        }
        pick = i;
    }
    printf("DEVICE  DeviceID %u serial %s\n", devs[pick].device_id, devs[pick].serial);

    r->fd = usbmux_connect(devs[pick].device_id, (uint16_t)o->port, &result);
    if (r->fd < 0) {
        const char *hint = result == USBMUX_RESULT_REFUSED
                               ? " (port closed: open TetherCam on the phone and keep it in the foreground)"
                           : result == USBMUX_RESULT_BAD_DEVICE ? " (bad device)"
                           : result == USBMUX_RESULT_BAD_VERSION ? " (bad version)"
                                                                 : "";
        fprintf(stderr, "error: Connect to device %u port %u -> Number %d%s\n",
                devs[pick].device_id, o->port, result, hint);
        return 1;
    }
    printf("LINK    usbmux tunnel open on port %u\n", o->port);
    return 0;
}

int main(int argc, char **argv) {
    struct opts o;
    struct rx   r;
    int         rc;

    /* Unbuffered on purpose: the Windows C runtime rejects _IOLBF with size 0
     * (invalid parameter handler, process abort) and has no real line mode. */
    setvbuf(stdout, NULL, _IONBF, 0);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); /* a vanished phone is a write error, not a kill */
#endif
    if (parse_args(argc, argv, &o)) return usage();

    memset(&r, 0, sizeof(r));
    r.o  = &o;
    r.fd = -1;

    rc = open_link(&o, &r);
    if (rc < 0) return 0; /* --list */
    if (rc > 0) return 1;

    if (o.dump_path) {
        r.dump = fopen(o.dump_path, "wb");
        if (!r.dump) {
            fprintf(stderr, "error: cannot open %s for writing\n", o.dump_path);
            usbmux_io_close(r.fd);
            return 1;
        }
    }

    rc = run_session(&r);

    if (r.fd >= 0) {
        uint8_t stop[IUCM_HEADER_SIZE];
        size_t  n = 0;
        if (r.hello_seen && iucm_encode_stop(stop, sizeof(stop), &n) == IUCM_OK &&
            usbmux_io_write_all(r.fd, stop, n) == USBMUX_OK)
            printf("STOP    sent\n");
        usbmux_io_close(r.fd);
    }
    if (r.dump) fclose(r.dump);

    if (r.frames > 1 && r.last_pts > r.first_pts) {
        double span = (double)(r.last_pts - r.first_pts) / 1e6;
        printf("VIDEO   frames=%" PRIu64 " in %.2fs -> %.1f fps, keyframes=%" PRIu64
               ", nals=%" PRIu64 ", %.0f kbps\n",
               r.frames, span, (double)(r.frames - 1) / span, r.keyframes, r.nals,
               (double)r.video_bytes * 8.0 / 1000.0 / span);
    } else {
        printf("VIDEO   frames=%" PRIu64 " keyframes=%" PRIu64 " nals=%" PRIu64 "\n", r.frames,
               r.keyframes, r.nals);
    }
    if (rc) {
        fprintf(stderr, "error: %s\n", r.err);
        return 1;
    }
    return r.frames > 0 ? 0 : 1;
}

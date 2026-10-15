#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "qhyccd.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define RECV_BUF_SIZE (128*1024*1024)
#define RECV_WORDS (32*1024*1024)
#define THUMB_W 1024
#define THUMB_H 1024

static unsigned char recv_buf[RECV_BUF_SIZE];
static unsigned char thumb_buf[THUMB_W * THUMB_H * 4];
static unsigned short *pixel_buf = NULL;
static int enable_x11 = 1;

Display *xdisp = NULL;
Window xwin = 0;
GC xgc, text_gc;
XImage *ximg = NULL;
static int cur_fps = 0, cur_total = 0, fps_acc = 0, fps_last_t = 0;

static void stretch_histogram(unsigned short *pix, int count, unsigned char *out)
{
    unsigned short lo = 65535, hi = 0;
    for(int i = 0; i < count; i++){
        unsigned short v = pix[i];
        if(v < lo) lo = v;
        if(v > hi) hi = v;
    }
    unsigned short range = hi - lo;
    if(range == 0) { memset(out, 0, count); return; }
    for(int i = 0; i < count; i++){
        unsigned int v = (unsigned int)(pix[i] - lo) * 255 / range;
        out[i] = (unsigned char)(v > 255 ? 255 : v);
    }
}

void make_thumbnail(unsigned char *raw, int w, int h, int bpp, unsigned char *thumb, int tw, int th, int avail_rows)
{
    int src_row_bytes = w * (bpp / 8);
    int src_pix_bytes = bpp / 8;
    int dst_row_bytes = tw * 4;
    int max_sy = (avail_rows > 0 && avail_rows < h) ? avail_rows : h;

    memset(thumb, 0, th * dst_row_bytes);

    float scale_x = (float)tw / w;
    float scale_y = (float)th / max_sy;
    float s = (scale_x < scale_y) ? scale_x : scale_y;
    int out_w = (int)(w * s);
    int out_h = (int)(max_sy * s);
    int off_x = (tw - out_w) / 2;
    int off_y = (th - out_h) / 2;

    if(!pixel_buf) pixel_buf = (unsigned short*)malloc(out_w * out_h * sizeof(unsigned short));

    int pixel_count = 0;
    for(int ty = 0; ty < out_h; ty++){
        int sy = (int)(ty / s);
        if(sy >= max_sy) break;
        for(int tx = 0; tx < out_w; tx++){
            int sx = (int)(tx / s);
            if(sx >= w) break;
            int src_off = sy * src_row_bytes + sx * src_pix_bytes;
            unsigned short pixel = (raw[src_off] << 8) | raw[src_off+1];
            pixel_buf[pixel_count++] = pixel;
        }
    }

    unsigned char *gray = (unsigned char*)malloc(pixel_count);
    stretch_histogram(pixel_buf, pixel_count, gray);

    int idx = 0;
    for(int ty = 0; ty < out_h; ty++){
        for(int tx = 0; tx < out_w; tx++){
            int dst_off = (ty + off_y) * dst_row_bytes + (tx + off_x) * 4;
            thumb[dst_off+0] = gray[idx];
            thumb[dst_off+1] = gray[idx];
            thumb[dst_off+2] = gray[idx];
            thumb[dst_off+3] = 0;
            idx++;
        }
    }
    free(gray);
}

void init_x11(int w, int h)
{
    xdisp = XOpenDisplay(NULL);
    if(!xdisp){ fprintf(stderr,"XOpenDisplay failed\n"); return; }
    int screen = DefaultScreen(xdisp);
    Window root = RootWindow(xdisp, screen);
    xwin = XCreateSimpleWindow(xdisp, root, 0, 0, w, h, 1,
                               BlackPixel(xdisp, screen), WhitePixel(xdisp, screen));
    XStoreName(xdisp, xwin, "QHYCCD Live Thumbnail");
    XSelectInput(xdisp, xwin, ExposureMask | StructureNotifyMask);
    XMapWindow(xdisp, xwin);
    xgc = DefaultGC(xdisp, screen);
    text_gc = XCreateGC(xdisp, root, 0, NULL);
    XSetForeground(xdisp, text_gc, WhitePixel(xdisp, screen));
    XSetBackground(xdisp, text_gc, BlackPixel(xdisp, screen));
    ximg = XCreateImage(xdisp, DefaultVisual(xdisp, screen), DefaultDepth(xdisp, screen),
                        ZPixmap, 0, (char*)thumb_buf, w, h, 32, 0);
}

void show_thumb()
{
    if(!xdisp) return;
    XPutImage(xdisp, xwin, xgc, ximg, 0, 0, 0, 0, THUMB_W, THUMB_H);
    char buf[64];
    snprintf(buf, sizeof(buf), "FPS: %d  Total: %d", cur_fps, cur_total);
    XDrawString(xdisp, xwin, text_gc, 8, 16, buf, strlen(buf));
    XSync(xdisp, False);
    while(XPending(xdisp)){ XEvent e; XNextEvent(xdisp, &e); }
}

void close_x11()
{
    if(xdisp){ if(ximg) XDestroyImage(ximg); XFreeGC(xdisp, text_gc); XCloseDisplay(xdisp); xdisp = NULL; }
}

static int check_event(Display *d){
    if(!d) return 0;
    while(XPending(d)){ XEvent e; XNextEvent(d, &e); if(e.type == DestroyNotify) return 1; }
    return 0;
}

int main(int argc, char *argv[])
{
    int num, ret;
    qhyccd_handle *camhandle = NULL;
    char id[32];
    int found = 0;
    unsigned int w = 0, h = 0, bpp = 0;
    double chipw, chiph, pixelw, pixelh;
    int duration = 0;
    if(argc > 1) duration = atoi(argv[1]);
    if(getenv("NO_X11") || !getenv("DISPLAY") || access("/tmp/.X11-unix/X0", F_OK) != 0) enable_x11 = 0;

    fprintf(stderr, "=== QHYCCD Live Thumbnail ===\n");
    ret = InitQHYCCDResource();
    if(ret != QHYCCD_SUCCESS){ fprintf(stderr,"Init failed\n"); return 1; }
    num = ScanQHYCCD();
    if(num <= 0){ fprintf(stderr,"No camera\n"); goto out; }
    for(int i=0;i<num;i++){ ret = GetQHYCCDId(i,id); if(ret == QHYCCD_SUCCESS){ found=1; break; }}
    if(!found){ fprintf(stderr,"No camera ID\n"); goto out; }
    fprintf(stderr,"Camera: %s\n", id);
    camhandle = OpenQHYCCD(id);
    if(!camhandle){ fprintf(stderr,"Open failed\n"); goto out; }
    SetQHYCCDReadMode(camhandle, 0);
    ret = IsQHYCCDControlAvailable(camhandle, CAM_LIVEVIDEOMODE);
    if(ret == QHYCCD_ERROR){ fprintf(stderr,"No live mode\n"); goto out; }
    SetQHYCCDStreamMode(camhandle, 1);
    ret = InitQHYCCD(camhandle);
    if(ret != QHYCCD_SUCCESS){ fprintf(stderr,"Init failed\n"); goto out; }
    ret = IsQHYCCDControlAvailable(camhandle, CONTROL_TRANSFERBIT);
    if(ret == QHYCCD_SUCCESS) SetQHYCCDBitsMode(camhandle, 16);
    ret = GetQHYCCDChipInfo(camhandle, &chipw, &chiph, &w, &h, &pixelw, &pixelh, &bpp);
    if(ret != QHYCCD_SUCCESS){ fprintf(stderr,"GetChipInfo failed\n"); goto out; }
    fprintf(stderr, "Max Resolution: %dx%d bpp=%d\n", w, h, bpp);
    ret = SetQHYCCDResolution(camhandle, 300, 500, 1024, 1024);
    if(ret != QHYCCD_SUCCESS){ fprintf(stderr,"SetResolution failed\n"); goto out; }
    w = 1024; h = 1024;
    fprintf(stderr, "ROI: (300,500) %dx%d\n", w, h);

    if(enable_x11) init_x11(THUMB_W, THUMB_H);
    fprintf(stderr, "Display: %s\n", enable_x11 ? "X11 window" : "console only");

    {
    int t_start = (int)time(NULL);
    int t_fps = t_start;
    int frames = 0;
    int fps = 0;
    unsigned int ddrnum = 0;

    fprintf(stderr, "Waiting for DDR...\n");
    int wait = 0;
    while(wait < 50){
        ddrnum = GetQHYCCDPcieDDRNum(camhandle);
        if(ddrnum > 16777216) break;
        usleep(200000); wait++;
    }
    fprintf(stderr, "DDR = %u %s\n", ddrnum, ddrnum > 16777216 ? "ready" : "timeout");

    while(1){
        if(duration > 0 && (int)time(NULL) - t_start >= duration) break;
        if(check_event(xdisp)) break;

        ddrnum = GetQHYCCDPcieDDRNum(camhandle);
        if(ddrnum > 16777216){
            memset(recv_buf, 0, 64);
            ret = QHYCCDPcieRecv(camhandle, recv_buf, RECV_WORDS, 25000);
            if(ret <= 0) continue;
            int magic = (recv_buf[0] == 0x44 && recv_buf[1] == 0x44 &&
                         recv_buf[2] == 0x55 && recv_buf[3] == 0x55);
            if(!magic) continue;
            frames++; fps++; cur_total = frames;
            int now = (int)time(NULL);
            if(fps_last_t == 0) fps_last_t = now;
            fps_acc++;
            if(now - fps_last_t >= 2){ cur_fps = fps_acc / 2; fps_acc = 0; fps_last_t = now; }
            int row_bytes = w * (bpp / 8);
            int buf_words = RECV_WORDS;
            int avail_bytes = ((int)ret < buf_words ? (int)ret : buf_words) * 4;
            int avail_rows = (avail_bytes > 16) ? (avail_bytes - 16) / row_bytes : 0;
            if(avail_rows > h) avail_rows = h;
            if(avail_rows > 0)
                make_thumbnail(recv_buf + 16, w, h, bpp, thumb_buf, THUMB_W, THUMB_H, avail_rows);
            if(enable_x11) show_thumb();
        }
        int t_now = (int)time(NULL);
        if(t_now - t_fps >= 5){
            fprintf(stderr, "fps = %d (total frames=%d)\n", fps / 5, frames);
            fps = 0; t_fps = t_now;
        }
    }

    close_x11();
    if(pixel_buf) free(pixel_buf);
    fprintf(stderr, "Total frames: %d\n", frames);
    }
    ret = CloseQHYCCD(camhandle); camhandle = NULL;
out:
    if(camhandle) CloseQHYCCD(camhandle);
    ReleaseQHYCCDResource();
    return 0;
}

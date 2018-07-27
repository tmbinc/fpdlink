#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

#define XRES 324
#define YRES 257

uint8_t framebuffer8[YRES][XRES];


#define AVGSIZE 100
int avgmax[AVGSIZE];
int avgptr;

int frame_enable = 0;
int frames_total = 0;

int encoder_fd;

void frame_init(void)
{
    encoder_fd = open("encoder.raw", O_WRONLY);
    if (encoder_fd < 0) {
        perror("encoder.raw");
        exit(0);
    }
}

void frame_handle(uint16_t *frame, int stride, int xres, int yres)
{
    static int tog;

    int avgsum = 0;
    for (int i = 0; i < AVGSIZE; ++i)
        avgsum += avgmax[i];
    avgsum /= AVGSIZE;
    int min = 0xFFFF, max = 0;
    int above_avg = 0;
    int frame_avg = 0;
    int npix = 0;
    int threshold = avgsum * 1.1;
    for (int y = 0; y < YRES; ++y) {
        uint16_t *line = frame + y * stride;
        for (int x = 0; x < XRES; ++x) {
            uint16_t pix = line[x];
            if (!pix) continue;
            if (pix < min) min = pix;
            if (pix > max) max = pix;
            if (pix > threshold) above_avg++;
            frame_avg += pix;
            npix++;
        }
    }

    if (npix)
    frame_avg /= npix;

    avgmax[avgptr++] = frame_avg;
    if (avgptr == AVGSIZE) avgptr = 0;
    fprintf(stderr, "%d..%d..%d, avgsum %d, .. %d %d %d (above %d)\n", min, frame_avg, max, avgsum, (int)(avgsum * 1.01), frame_enable, frames_total, above_avg);

    int interesting = frame_avg > avgsum * 1.01;
    interesting |= above_avg > 40;
    if (interesting) frame_enable = 15;

    max += max / 100;
    min -= min / 100;
    int d = max - min;
    if (!d) d = 1;
    if (max > min && frame_enable | 1) {
        for (int y = 0; y < YRES; ++y) {
            uint16_t *line = frame + y * stride;
            for (int x = 0; x < XRES; ++x) {
                uint16_t pix = line[x];
                if (pix) {
                    framebuffer8[y][x] = ((pix - min) << 8) / d;
                }
            }
        }
        
        write(encoder_fd, framebuffer8, XRES * YRES);
    //fprintf(stderr, "wrote frame, min=%d, max=%d\n", min, max);
        frames_total++;
        frame_enable--;

    }


}


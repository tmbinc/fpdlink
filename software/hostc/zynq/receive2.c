#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define YRES 257
#define XRES 324
uint16_t framebuffer[YRES][XRES];
uint8_t framebuffer8[YRES][XRES];
uint32_t last_sync;
int curx;

int ccnt;
int count = 0;

#define AVGSIZE 100
int avgmax[AVGSIZE];
int avgptr;

int frame_enable = 0;
int frames_total = 0;

void handle_packet(void *d, int l)
{
    uint32_t *p = d;
    
    int ccount = *p;
    if (ccount != count + 1) {
        fprintf(stderr, "%08x -> %08x\n", count, ccount);
    }
    count = ccount;
    
    p += 1;
    l -= 4;
    
    while (l >= 4) {
    
        uint32_t v = *p++;
        
        int cnt = (v>>24) & 0x7F;
        if (cnt != ccnt)
        {
            fprintf(stderr, "packet loss! %02x -> %02x\n", cnt, ccnt);
        }
        
        ccnt = (cnt + 1) & 0x7F;
        
        if (v & 0x010000) {
            if ((v & 0x8000) && (last_sync & 0xFFF) != 0x13) {
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
                    for (int x = 0; x < XRES; ++x) {
                        uint16_t pix = framebuffer[y][x];
                        if (!pix) continue;
                        pix -= 6000;
                        if (pix < min) min = pix;
                        if (pix > max) max = pix;
                        if (pix > threshold) above_avg++;
                        frame_avg += pix;
                        npix++;
                    }
                }
                
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
                if (max > min && frame_enable) {
                    for (int y = 0; y < YRES; ++y)
                        for (int x = 0; x < XRES; ++x) {
                            uint16_t pix = framebuffer[y][x];
                            if (pix) {
                                pix -= 6000;
                            framebuffer8[y][x] = ((pix - min) << 8) / d;
                            }
                        }
                    
                    write(1, framebuffer8, XRES * YRES);
//                fprintf(stderr, "wrote frame, min=%d, max=%d\n", min, max);
                    frames_total++;
                    frame_enable--;
                
                }
                

                memset(framebuffer, 0, sizeof(framebuffer));
            }
            last_sync = v;
            curx = 0;
        }
        
        if ((last_sync & 0x200) == 0) {
            if (curx > 0 && (curx < (XRES + 1)) && ((last_sync & 0xFFF) < (YRES + 1)) && (last_sync & 0xFFF) > 0) {
                framebuffer[(last_sync & 0xFFF) - 1][curx-1] = v & 0xFFFF;
            }
            curx++;
        }
    
        l -= 4;
    }
}

int main(void)
{
    struct sockaddr_in si_me, si_other;
    int s, i, blen, slen = sizeof(si_other);
    char buf[2000];

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == -1)
    {
        perror("socket");
        return 0;
    }

    memset((char *) &si_me, 0, sizeof(si_me));
    si_me.sin_family = AF_INET;
    si_me.sin_port = htons(1234);
    si_me.sin_addr.s_addr = 0;

    if (bind(s, (struct sockaddr*) &si_me, sizeof(si_me))==-1)
    {
        perror("bind");
        return 0;
    }

    while (1) {
        int blen = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr*) &si_other, &slen);
        if (blen == -1) {
            break;
        }
        handle_packet(buf, blen);
    }


    close(s);
    return 0;
}
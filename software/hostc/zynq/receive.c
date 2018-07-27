#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <sys/mman.h>
#define RAM_BASE 0x1f000000
#define RAM_SIZE 0x00400000

#define FB_XRES 1280
#define FB_YRES 720
#define FB_BPP 4


#define YRES 257
#define XRES 324
uint16_t framebuffer[YRES][XRES];
uint32_t last_sync;
int curx;

uint32_t *framebuffer_hdmi;

int ccnt;
int count = 0;

#define AVGSIZE 100
int avgmax[AVGSIZE];
int avgptr;

int frame_enable = 0;
int frames_total = 0;

uint32_t map[1 << 14];

void create_map(int min, int max)
{
    int d = max - min;
    for (int i = 0 ; i < 1 << 14; ++i) {
        uint8_t i8 = ((i - min) * 0xFF) / d;
        if (i < min) i8 = 0;
        if (i >= max) i8 = 0xFF;
        map[i] = 0x010101 * i8;
    }
}

void handle_packet(void *d, int l)
{
    uint32_t *p = d;
    
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
                #if 1
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
//                        pix -= 6000;
                        if (pix < min) min = pix;
                        if (pix > max) max = pix;
//                        if (pix > threshold) above_avg++;
//                        frame_avg += pix;
//                        npix++;
                    }
                }
                create_map(min, max);
                
                frame_avg /= npix;

                avgmax[avgptr++] = frame_avg;
                if (avgptr == AVGSIZE) avgptr = 0;
//                fprintf(stderr, "%d..%d..%d, avgsum %d, .. %d %d %d (above %d)\n", min, frame_avg, max, avgsum, (int)(avgsum * 1.01), frame_enable, frames_total, above_avg);
                
                int interesting = frame_avg > avgsum * 1.01;
                interesting |= above_avg > 40;
                if (interesting) frame_enable = 15;

                for (int y = 0; y < YRES; ++y) {
                    uint32_t *l = framebuffer_hdmi + y * FB_XRES * 2;
                    for (int x = 0; x < XRES; ++x) {
                        uint16_t pix = framebuffer[y][x];
                            uint32_t m = map[pix];
                            *l++ = m;
                            l[FB_XRES] = m;
                            *l++ = m;
                            l[FB_XRES] = m;
                        }
                }
                    
//                fprintf(stderr, "wrote frame, min=%d, max=%d\n", min, max);
                    frames_total++;
                    frame_enable--;
                
#endif                

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

void write_descriptor(volatile uint32_t *addr, uint32_t nextdesc, uint32_t buffer_address, uint32_t buffer_length)
{
    uint32_t control = buffer_length | (1 << 27) | (1 << 26);
    addr[0] = nextdesc;
    addr[1] = 0;
    addr[2] = buffer_address;
    addr[3] = 0;
    addr[4] = 0;
    addr[5] = 0;
    addr[6] = control;
    addr[7] = 0;
    addr[8] = 0;
    addr[9] = 0;
    addr[10] = 0;
    addr[11] = 0;
    addr[12] = 0;
    addr[13] = 0;
    addr[14] = 0;
    addr[15] = 0;
}


int main(void)
{
    int fd = open("/dev/fb0", O_RDWR | O_SYNC);
    if (fd < 0)
    {
      perror("open fb0");
    }
    
    framebuffer_hdmi = mmap(0, FB_XRES * FB_YRES * FB_BPP, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    
    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
    {
      perror("open");
    }
    
    volatile uint32_t *dma = mmap(0, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x40400000);
    volatile uint32_t *ram = mmap(0, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, RAM_BASE);
    
    const int S2MM_DMACR = 0x30 / 4;
    const int S2MM_DMASR = 0x34 / 4;
    const int S2MM_CURDESC = 0x38 / 4;
    const int S2MM_TAILDESC = 0x40 / 4;
    
    const int BUFFER_SIZE = 0x10000;
    const int NUMDESC = 32;
    
    int i;

    for (i = 0 ; i < NUMDESC; ++i) {
        uint32_t buffer_base = RAM_BASE + NUMDESC * 0x40 + i * BUFFER_SIZE;
        uint32_t nextdesc = RAM_BASE + (i + 1) % NUMDESC * 0x40;
        write_descriptor(ram + i * 0x10, nextdesc, buffer_base, BUFFER_SIZE);
    }
    
    printf("resetting...\n");
    dma[S2MM_DMACR] = 4;
    while (dma[S2MM_DMACR] & 4);
    dma[S2MM_CURDESC] = RAM_BASE;
    printf("starting transfer\n");
    dma[S2MM_DMACR] = 1;
    dma[S2MM_TAILDESC] = RAM_BASE + (NUMDESC-1) * 0x40;
    
    int curdesc = 0;
    
    for (;;) {
        uint32_t status = ram[curdesc * 0x10 + 0x1C/4];
        if (!(status & 0x80000000)) {
            continue;
        }
        
        handle_packet(ram + (NUMDESC * 0x40 + curdesc * BUFFER_SIZE) / 4, status & 0xFFFFFF);
        
        ram[curdesc * 0x10 + 0x1C/4] &= ~0x80000000; // clear cmpl
        dma[S2MM_TAILDESC] = RAM_BASE + curdesc * 0x40;
        
        ++curdesc;
        curdesc %= NUMDESC;
    }
}

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include "fastftdi.h"
#include "hw_common.h"
#include <SDL2/SDL.h>
#include <byteswap.h>

#define MAX_REGS 100

/////////////////////////////////////// SDL


SDL_Window *window;
SDL_Texture *texture;
SDL_Texture *texture_histogram;
SDL_Renderer *renderer;

#define SCREEN_XRES 640
#define SCREEN_YRES 512
#define XRES 324
#define YRES 257

void video_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS|SDL_INIT_NOPARACHUTE) != 0) {
        SDL_Log("failed to initialize SDL: %s\n", SDL_GetError());
        abort();
        return;
    }

    window = SDL_CreateWindow("sem", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_XRES, SCREEN_YRES, 0);
    if (!window) {
        SDL_Log("failed to create window: %s\n", SDL_GetError());
        abort();
    }

    renderer = SDL_CreateRenderer(window, 01, SDL_RENDERER_ACCELERATED);
    SDL_RendererInfo info;
    SDL_GetRendererInfo(renderer, &info);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, XRES, YRES);
    texture_histogram = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 256);
}

uint32_t framebuffer32[YRES][XRES];


#define AVGSIZE 100
int avgmax[AVGSIZE];
int avgptr;

int frame_enable = 0;
int frames_total = 0;
int new_frame;

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
    for (int y = 0; y < YRES - 4; ++y) {
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
                    framebuffer32[y][x] = (((pix - min) << 8) / d) * 0x01010101;
                }
            }
        }

        new_frame = 1;

    //fprintf(stderr, "wrote frame, min=%d, max=%d\n", min, max);
        frames_total++;
        frame_enable--;

    }


}


/////////////////////////////////////// handle data

uint16_t framebuffer14[YRES][XRES];
uint32_t last_sync;
int curx;
int cury;

int ccnt;
int count = 0;

void frame_init(void);
extern void frame_handle(uint16_t *frame, int stride, int xres, int yres);

void handle_packet(void *d, int l)
{
    uint32_t *p = d;

    while (l >= 4) {

        uint32_t v = bswap_32(*p++);

        int cnt = (v>>24) & 0x7F;
//        fprintf(stderr, "ccnt %d\n", cnt);
        if (cnt != ccnt)
        {
            fprintf(stderr, "packet loss! %02x -> %02x\n", cnt, ccnt);
        }

        ccnt = (cnt + 1) & 0x7F;

        if (v & 0x010000) {
            if ((v & 0x8000) && cury != 0x13) {
                frame_handle(framebuffer14[0], XRES, XRES, YRES);
                memset(framebuffer14, 0, sizeof(framebuffer14));
            }
            last_sync = v;
            cury = last_sync & 0xFFF;
            curx = 0;
        }

        if ((last_sync & 0x200) == 0) {
            if (curx > 0 && (curx < (XRES + 1)) && (cury < (YRES + 1)) && cury > 0) {
                if (cury > 20) {
                    framebuffer14[cury - 2][curx-1] = v & 0xFFFF;
                } else
                {
                    framebuffer14[cury - 1][curx-1] = v & 0xFFFF;
                }
            }
            curx++;
        }

        l -= 4;
    }
}

/////////////////////////////////////// OV

struct {
    const char *name;
    int start, end;
} registers[MAX_REGS];

int num_regs;
int quit;
FTDIDevice ftdi_dev;

int last_reg;

void add_reg(const char *name, int start_reg, int end_reg)
{
//    printf("adding %s from %08x to %08x\n", name, start_reg, end_reg);
    if (num_regs < MAX_REGS) {
        registers[num_regs].name = strdup(name);
        registers[num_regs].start = start_reg;
        registers[num_regs].end = end_reg;
        num_regs++;
    } else {
        fprintf(stderr, "too many registers in mapfile\n");
        abort();
    }
}

int find_reg(const char *name, int *start, int *end)
{
    for (int i = 0; i < num_regs; ++i) {
        if(!strcmp(name, registers[i].name)) {
            *start = registers[i].start;
            *end = registers[i].end;
            return 0;
        }
    }
    fprintf(stderr, "unknown register %s\n", name);
    return 1;
}

int rx_state;
int sdram_len;
int sdram_ptr;
unsigned char sdram_buf[512];

void handle_sdram(uint8_t *buffer, int len)
{
//    printf("%d bytes of sdram\n", len);
//    printf("%08x\n", *(int*)buffer);
    handle_packet(buffer, len);
//    uint32_t *t = (void*)buffer;
//    for (int i = 0; i < len; ++i) printf("%02x ", buffer[i]);
//    printf("\n");
}

int receive_callback(uint8_t *buffer, int length, FTDIProgressInfo *progress, void *userdata)
{
//    printf("received %d bytes\n", length);
//    for (int i = 0; i < length; ++i) printf("%02x ", buffer[i]);
//    printf("\n");
    for(int i = 0; i < length; ++i) {
        switch (rx_state)
        {
        case 0:
            switch (buffer[i]) {
            case 0x55:
                rx_state = 0x100; // REG response
                break;
            case 0xD0:
                rx_state = 0x200; // SDRAM
                break;
            default:
                fprintf(stderr, "UNKNOWN RXCMD %02x\n", buffer[i]);
                break;
            }
            break;
        case 0x100: // REG addr first (ignore)
        case 0x101: // REG addr second (ignore)
            last_reg <<= 8;
            last_reg |= buffer[i];
            rx_state++;
            break;
        case 0x102:
            printf("read val %04x = %02x\n", last_reg & 0x7FFF, buffer[i]);
            rx_state++;
            break;
        case 0x103:
            rx_state = 0;
            break;
        case 0x200:
            rx_state++;
            sdram_ptr = 0;
            sdram_len = (buffer[i] + 1) * 2;
            break;
        case 0x201:
            sdram_buf[sdram_ptr++] = buffer[i];
            if (sdram_ptr == sdram_len) {
                handle_sdram(sdram_buf, sdram_len);
                rx_state = 0;
            }
            break;
        default:
            fprintf(stderr, "invalid state\n");
            abort();
        }
    }
    return quit;
}

void *rx_thread_fnc(void *x)
{
    FTDIDevice_ReadStream(&ftdi_dev, FTDI_INTERFACE_A, receive_callback, 0, 4, 4);
    return 0;
}

int read_write_reg(const char *name, unsigned int value, int write)
{
    int start, end, size;
    if (find_reg(name, &start, &end)) {
        abort();
    }

    size = end - start + 1;

    for (int i = size; i--; ) {
        int addr = start + i;
        unsigned char write_cmd[5] = {0x55, (addr >> 8) | (write ? 0x80 : 0), (addr & 0xFF), value  & 0xFF};
        value >>= 8;
        unsigned char cs = 0;
        for (int j = 0; j < 4; ++j) {
            cs += write_cmd[j];
        }
        write_cmd[4] = cs;

        for (int j = 0; j < 5; ++j) {
            printf("%02x ", write_cmd[j]);
        }
        printf("\n");
        FTDIDevice_Write(&ftdi_dev, FTDI_INTERFACE_A, write_cmd, sizeof(write_cmd), 0);
    }
    return 0;
}

void write_reg(const char *name, unsigned int value)
{
    read_write_reg(name, value, 1);
}

int read_map(void)
{
    FILE *f = fopen("map.txt", "rt");
    if (!f) {
        return 1;
    }

    char line[128];
    while (fgets(line, 128, f)) {
        // ignore comments
        if (!line[0] || line[0] == '\n' || line[0] == '#') {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq || eq == line) {
            return 1;
        }

        if (eq[-1] != ' ') {
            return 1;
        }
        eq[-1] = 0;
        // skip to after whitespace RHS
        eq++;
        while (*eq == ' ') {
            eq++;
        }

        int start_reg, end_reg;

        char *colon = strchr(eq, ':');

        if (colon) {
            *colon++ = 0;
            start_reg = strtoul(eq, 0, 0);
            end_reg = strtoul(colon, 0, 0);
        } else {
            start_reg = end_reg = strtoul(eq, 0, 0);
        }

        add_reg(line, start_reg, end_reg);
    }

    return 0;
}

int main(void)
{
    video_init();
    if (FTDIDevice_Open(&ftdi_dev) != 0) {
        fprintf(stderr, "couldn't open fpga\n");
        return 1;
    }
    HW_Init(&ftdi_dev, "top.bit");
    if (read_map()) {
        fprintf(stderr, "failed to read map file\n");
        return 1;
    }

    pthread_t rxthread;

    if (pthread_create(&rxthread, NULL, rx_thread_fnc, 0) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        return 1;
    }

    write_reg("FPDTOP_DEBUG_MUX", 0);
    write_reg("FPDTOP_ADJUST_DIRECTION", 0);
    write_reg("FPDTOP_ADJUST", 19100);
    write_reg("FPDTOP_INVERT", 0);
    write_reg("FPDTOP_ADJUST_DIRECTION", 0);
    write_reg("FPDTOP_ADJUST", 19100);
    write_reg("FPDTOP_INVERT", 0);

#if 1
    write_reg("LEDS_MUX_2", 0);
    write_reg("LEDS_OUT", 0);

    write_reg("LEDS_MUX_0", 2);
    write_reg("LEDS_MUX_1", 2);

    int ring_base = 0;
    int ring_size = 16 * 1024 * 1024;
    int ring_end = ring_base + ring_size;
    write_reg("SDRAM_SINK_GO", 0);
    write_reg("SDRAM_HOST_READ_GO", 0);
    write_reg("SDRAM_SINK_RING_BASE", ring_base);
    write_reg("SDRAM_SINK_RING_END", ring_end);
    write_reg("SDRAM_HOST_READ_RING_BASE", ring_base);
    write_reg("SDRAM_HOST_READ_RING_END", ring_end);
    write_reg("SDRAM_SINK_GO", 1);
    write_reg("SDRAM_HOST_READ_GO", 1);
    write_reg("CSTREAM_CFG", 1);

    while (!quit) {

        if (new_frame) {
            SDL_UpdateTexture(texture, NULL, &framebuffer32[0], XRES * 4);
            SDL_Rect dest={0, 0, SCREEN_XRES, SCREEN_YRES};
            SDL_RenderCopy(renderer, texture, NULL, &dest);

            new_frame = 0;

            SDL_RenderPresent(renderer);
        }

        usleep(1000000 / 60);

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = 1;
            }
        }
    }

    write_reg("SDRAM_SINK_GO", 0);
    write_reg("SDRAM_HOST_READ_GO", 0);
    write_reg("CSTREAM_CFG", 0);
#endif

    quit = 1;

    pthread_join(rxthread, NULL);
}

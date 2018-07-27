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
            if ((v & 0x8000) && cury != 0x13) {
                frame_handle(framebuffer[0], XRES, XRES, YRES);
                memset(framebuffer, 0, sizeof(framebuffer));
            }
            last_sync = v;
            cury = last_sync & 0xFFF;
            curx = 0;
        }
        
        if ((last_sync & 0x200) == 0) {
            if (curx > 0 && (curx < (XRES + 1)) && (cury < (YRES + 1)) && cury > 0) {
                if (cury > 20) {
                    framebuffer[cury - 2][curx-1] = v & 0xFFFF;
                } else
                {
                    framebuffer[cury - 1][curx-1] = v & 0xFFFF;
                }
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
    
    frame_init();

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
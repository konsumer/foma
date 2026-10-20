/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Copyright (c) 2026 M-VAVE.
 * ST7789 initialization with a bounded FM1 DMA adapter. */
static uint8_t display_pending;
static uint32_t display_started;
static uint16_t display_row[240];

static void display_finish(void)
{
    if(display_pending) {
        fm1_lcd_wait();fm1_lcd_deselect();display_pending=0;
    }
}
static void display_command(uint8_t command)
{
    fm1_lcd_send_cmd(command);fm1_lcd_wait();fm1_lcd_deselect();
}
static void display_data(const void *bytes,unsigned length)
{
    if(length) {fm1_lcd_send_data(bytes,length);fm1_lcd_wait();fm1_lcd_deselect();}
}
static void display_window(unsigned x,unsigned y,unsigned width,unsigned height)
{
    unsigned xmax=x+width-1,ymax=y+height-1;
    const uint8_t bounds[8]={(uint8_t)(x>>8),(uint8_t)x,(uint8_t)(xmax>>8),(uint8_t)xmax,
                           (uint8_t)(y>>8),(uint8_t)y,(uint8_t)(ymax>>8),(uint8_t)ymax};
    display_command(0x2a);display_data(bounds,4);
    display_command(0x2b);display_data(bounds+4,4);display_command(0x2c);
}
static int display_valid(unsigned x,unsigned y,unsigned w,unsigned h,const void *pixels,size_t count)
{
    return pixels&&w&&h&&x<240&&y<240&&w<=240-x&&h<=240-y&&count>=w*h;
}
static uint16_t display_swap(uint16_t value) {return (uint16_t)((value>>8)|(value<<8));}
void fm1_screen_fill(unsigned x,unsigned y,unsigned w,unsigned h,uint16_t color)
{
    if(!w||!h||x>=240||y>=240)return;
    if(w>240-x)w=240-x;if(h>240-y)h=240-y;
    display_finish();
    unsigned pixels=w*h,batch=pixels<240?pixels:240;
    for(unsigned i=0;i<batch;i++)display_row[i]=display_swap(color);
    display_window(x,y,w,h);
    while(pixels) {unsigned n=pixels<batch?pixels:batch;display_data(display_row,2*n);pixels-=n;}
}
int fm1_screen_blit(unsigned x,unsigned y,unsigned w,unsigned h,const uint16_t *pixels,size_t count)
{
    if(!display_valid(x,y,w,h,pixels,count))return -1;
    display_finish();uint32_t errors=fm1_lcd_timeouts;
    display_window(x,y,w,h);
    for(unsigned remaining=w*h;remaining;) {
        unsigned n=remaining<240?remaining:240;
        for(unsigned i=0;i<n;i++)display_row[i]=display_swap(*pixels++);
        display_data(display_row,2*n);remaining-=n;
        if(errors!=fm1_lcd_timeouts)return -2;
    }
    return errors==fm1_lcd_timeouts?0:-2;
}
int fm1_screen_pending(void)
{
    if(!display_pending)return 0;
    if(fm1_lcd_ready()) {display_finish();return 0;}
    if((uint32_t)(fm1_millis()-display_started)<20)return 1;
    fm1_lcd_recover();display_pending=0;return -2;
}
int fm1_screen_submit(unsigned x,unsigned y,unsigned w,unsigned h,uint16_t *pixels,size_t count)
{
    if(!display_valid(x,y,w,h,pixels,count)||w*h>FM1_SCREEN_DMA_PIXELS)return -1;
    int busy=fm1_screen_pending();if(busy)return busy;
    uint32_t errors=fm1_lcd_timeouts;
    display_window(x,y,w,h);if(errors!=fm1_lcd_timeouts)return -2;
    for(unsigned i=0;i<w*h;i++)pixels[i]=display_swap(pixels[i]);
    display_started=fm1_millis();display_pending=1;fm1_lcd_send_data(pixels,2*w*h);
    return 0;
}
static const uint8_t display_setup[]={
    0x11,0, 0x45,120,
    0x2a,4,0,0,0,0xef, 0x2b,4,0,0x28,1,0x17,
    0xb2,5,0x0c,0x0c,0x0c,0,0x33, 0x20,0,
    0xb7,1,0x56, 0xbb,1,0x18, 0xc0,1,0x2c, 0xc2,1,1,
    0xc3,1,0x1f, 0xc4,1,0x20, 0xc6,1,0x0f, 0xd0,2,0xa6,0xa1,
    0xe0,14,0xd0,0x0d,0x14,0x0b,0x0b,0x07,0x3a,0x44,0x50,0x08,0x13,0x13,0x2d,0x32,
    0xe1,14,0xd0,0x0d,0x14,0x0b,0x0b,0x07,0x3a,0x44,0x50,0x08,0x13,0x13,0x2d,0x32,
    0x36,1,0, 0x3a,1,0x55, 0xe7,1,0, 0x51,1,255, 0x21,0
};
static void display_delay(unsigned ms)
{
    while(ms--)for(volatile unsigned spin=0;spin<25000;spin++) {}
}
void fm1_screen_init(void)
{
    display_finish();fm1_lcd_hw_init();fm1_lcd_baud(4);
    display_command(0x28);display_delay(100);
    for(unsigned i=0;i<sizeof display_setup;) {
        unsigned command=display_setup[i++],length=display_setup[i++];
        if(command==0x45)display_delay(length);
        else {
            uint8_t payload[16];
            for(unsigned n=0;n<length;n++)payload[n]=display_setup[i+n];
            display_command((uint8_t)command);display_data(payload,length);i+=length;
        }
    }
    fm1_screen_fill(0,0,240,240,0);display_command(0x29);
}

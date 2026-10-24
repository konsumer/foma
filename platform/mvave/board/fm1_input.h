/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Copyright (c) 2026 M-VAVE.
 * FM1 input wiring and AC791N SPI2 setup. */
#pragma once
#include <stdint.h>
#include <string.h>
#include "fm1_gpio.h"

enum { FM1_NCOL=11, FM1_NKEY=41, FM1_NENC=7 };
/* Packed column/row locations, in the application's control order. */
static const uint8_t fm1_key_pin[FM1_NKEY] FM1_AUDIO_DATA = {
    0x04,0x14,0x61,0x41,0x21,0x01,0x92,0x82,0x71,0x51,0x31,0x11,0xa2,0x72,
    0x34,0x24,0x54,0x44,0x74,0x64,0x84,0x94,0xa4,0x03,0x13,0x23,0x33,0x43,
    0x53,0x63,0x73,0x83,0x93,0xa3,0x02,0x12,0x22,0x32,0x42,0x62,0x52
};
static const uint8_t fm1_encoder_pin[FM1_NENC][2] FM1_AUDIO_DATA = {
    {0x00,0x10},{0x20,0x30},{0x81,0x91},{0x80,0x90},{0x60,0x70},{0x40,0x50},{0x05,0x15}
};
static const int8_t fm1_quadrature[16] FM1_AUDIO_DATA = {
    0,1,-1,0, -1,0,0,1, 1,0,0,-1, 0,-1,1,0
};
static struct {
    volatile uint32_t notes,buttons;
    volatile int16_t enc_steps[FM1_NENC];
    uint8_t rows[FM1_NCOL],press[FM1_NKEY],release[FM1_NKEY];
    uint8_t phase[FM1_NENC],rest[FM1_NENC];
    uint16_t stable[FM1_NENC];
    int8_t edges[FM1_NENC];
    uint8_t column,queued,stalled;
} fm1_in;
static volatile uint8_t fm1_led[FM1_NCOL];
static uint8_t fm1_scan_bytes[2] __attribute__((aligned(4)));

#ifndef FM1_SCAN_REG
#define FM1_SCAN_REG(offset) (*(volatile uint32_t *)(0x11e00u+(offset)))
#endif
#ifndef FM1_SCAN_DMA
#define FM1_SCAN_DMA(p) (FM1_SCAN_REG(12)=(uint32_t)(uintptr_t)(p))
#endif
#ifndef FM1_IO_MAP
#define FM1_IO_MAP (*(volatile uint32_t *)0x51020u)
#endif
#ifndef FM1_SCAN_BARRIER
#define FM1_SCAN_BARRIER() __asm__ volatile("csync" ::: "memory")
#endif

static inline __attribute__((always_inline)) void fm1_scan_queue(unsigned column)
{
    unsigned word=65535u^(1u<<column);
    if(column<2)word^=1u<<(11+column);
    fm1_scan_bytes[0]=(uint8_t)(word>>8);fm1_scan_bytes[1]=(uint8_t)word;
    FM1_SCAN_REG(0)|=1u<<14;
    FM1_SCAN_BARRIER();
    FM1_SCAN_DMA(fm1_scan_bytes);FM1_SCAN_REG(16)=2;
    fm1_in.queued=(uint8_t)column;
}
static inline __attribute__((always_inline)) void fm1_scan_latch(unsigned column)
{
    const uint32_t a_mask=(1u<<9)|(1u<<10),h_mask=(1u<<6)|(1u<<9);
    FM1_PR(FM1_PA,FM1_OUT)&=~a_mask;FM1_PR(FM1_PH,FM1_OUT)&=~h_mask;
    FM1_PR(FM1_PA,FM1_OUT)|=1u<<1;
    FM1_PR(FM1_PA,FM1_OUT)&=~(1u<<1);
    unsigned leds=fm1_led[column];
    FM1_PR(FM1_PA,FM1_OUT)|=(leds&6u)<<8;
    FM1_PR(FM1_PH,FM1_OUT)|=((leds&8u)<<3)|((leds&16u)<<5);
    fm1_in.column=(uint8_t)column;
}
static inline __attribute__((always_inline)) unsigned fm1_contact(unsigned pin)
{
    return (fm1_in.rows[pin>>4]>>(pin&15))&1u;
}
static FM1_AUDIO_CODE void fm1_scan_decode(void)
{
    for(unsigned key=0;key<FM1_NKEY;key++) {
        unsigned down=fm1_contact(fm1_key_pin[key]);
        uint32_t mask=1u<<(key<14?key:key-14);
        volatile uint32_t *bits=key<14?&fm1_in.buttons:&fm1_in.notes;
        if(down) {
            fm1_in.release[key]=0;
            if(fm1_in.press[key]<2&&++fm1_in.press[key]==2)*bits|=mask;
        } else {
            fm1_in.press[key]=0;
            if(fm1_in.release[key]<8&&++fm1_in.release[key]==8)*bits&=~mask;
        }
    }
    for(unsigned e=0;e<FM1_NENC;e++) {
        unsigned phase=(fm1_contact(fm1_encoder_pin[e][0])<<1)|fm1_contact(fm1_encoder_pin[e][1]);
        unsigned previous=fm1_in.phase[e];
        if(previous>3) {fm1_in.phase[e]=fm1_in.rest[e]=(uint8_t)phase;continue;}
        if(phase!=previous) {
            fm1_in.stable[e]=0;
            int delta=fm1_quadrature[previous*4+phase];
            /* A diagonal transition gives no reliable direction. */
            if((phase^previous)==3)fm1_in.edges[e]=0;
            else {
                int edges=fm1_in.edges[e]+delta;
                fm1_in.edges[e]=(int8_t)(edges>8?8:edges<-8?-8:edges);
            }
            fm1_in.phase[e]=(uint8_t)phase;
        } else {
            if(fm1_in.stable[e]<900)fm1_in.stable[e]++;
            if(fm1_in.stable[e]==900) {fm1_in.rest[e]=(uint8_t)phase;fm1_in.edges[e]=0;}
            if(phase==fm1_in.rest[e]) {
                int movement=fm1_in.edges[e]/4;
                int pending=fm1_in.enc_steps[e]+movement;
                fm1_in.enc_steps[e]=(int16_t)(pending>32767?32767:pending<-32768?-32768:pending);
                fm1_in.edges[e]=0;
            }
        }
    }
}
static FM1_AUDIO_CODE void fm1_input_tick(void)
{
    /* DMA runs between timer ticks. Never wait inside the scan interrupt. */
    if(!(FM1_SCAN_REG(0)&(1u<<15))) {
        if(++fm1_in.stalled==20) {
            FM1_SCAN_REG(0)=0;FM1_SCAN_BARRIER();FM1_SCAN_REG(16)=0;
            FM1_SCAN_REG(0)=(1u<<14)|(1u<<5)|1u;
            fm1_scan_queue(fm1_in.queued);fm1_in.stalled=0;
        }
        return;
    }
    fm1_in.stalled=0;
    unsigned column=fm1_in.column;
    uint32_t a=FM1_PR(FM1_PA,FM1_IN),b=FM1_PR(FM1_PB,FM1_IN);
    fm1_in.rows[column]=(uint8_t)(~((a&1)|((a>>4)&30)|((b>>2)&32))&63);
    unsigned next=fm1_in.queued;
    fm1_scan_latch(next);
    fm1_scan_queue(next==10?0:next+1);
    if(column==10)fm1_scan_decode();
}
static void fm1_input_init(void)
{
    memset(&fm1_in,0,sizeof fm1_in);memset(fm1_in.phase,255,sizeof fm1_in.phase);
    const uint32_t a_in=1u|(15u<<5),a_out=(1u<<1)|(1u<<3)|(1u<<4)|(3u<<9),h_out=(1u<<6)|(1u<<9);
    FM1_PR(FM1_PA,FM1_DIE)|=a_in|a_out;FM1_PR(FM1_PB,FM1_DIE)|=1u<<7;FM1_PR(FM1_PH,FM1_DIE)|=h_out;
    FM1_PR(FM1_PA,FM1_OUT)&=~a_out;FM1_PR(FM1_PH,FM1_OUT)&=~h_out;
    FM1_PR(FM1_PA,FM1_DIR)=(FM1_PR(FM1_PA,FM1_DIR)|a_in)&~a_out;
    FM1_PR(FM1_PB,FM1_DIR)|=1u<<7;FM1_PR(FM1_PH,FM1_DIR)&=~h_out;
    FM1_PR(FM1_PA,FM1_PU)=(FM1_PR(FM1_PA,FM1_PU)|a_in)&~a_out;
    FM1_PR(FM1_PA,FM1_PD)&=~(a_in|a_out);FM1_PR(FM1_PB,FM1_PU)|=1u<<7;FM1_PR(FM1_PB,FM1_PD)&=~(1u<<7);
    FM1_PR(FM1_PH,FM1_PU)&=~h_out;FM1_PR(FM1_PH,FM1_PD)&=~h_out;
    FM1_PR(FM1_PA,FM1_HD0)|=3u<<9;FM1_PR(FM1_PA,FM1_HD)|=3u<<9;
    FM1_PR(FM1_PH,FM1_HD0)|=h_out;FM1_PR(FM1_PH,FM1_HD)|=h_out;
    FM1_IO_MAP|=1u<<17;FM1_SCAN_REG(4)=29;FM1_SCAN_REG(0)=(1u<<14)|(1u<<5)|1u;
    fm1_scan_queue(0);
    unsigned wait=100000;
    while(!(FM1_SCAN_REG(0)&(1u<<15))&&--wait) {}
    if(wait) {fm1_scan_latch(0);fm1_scan_queue(1);}
}
static void fm1_led_key(uint32_t key,int on)
{
    if(key>=FM1_NKEY)return;
    unsigned pin=fm1_key_pin[key],column=pin>>4,mask=1u<<(pin&15);
    uint8_t value=fm1_led[column];fm1_led[column]=(uint8_t)(on?value|mask:value&~mask);
}

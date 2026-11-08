/* The background APRS message receiver on the radio: see aprs_bg.h.
 *
 * TIM15 (unused elsewhere) starts an ADC conversion of the RX audio on PA4
 * 9600 times a second, in hardware: the samples are evenly spaced however late
 * the interrupt runs (the demodulator loses frames with 20 us of jitter,
 * tools/aprs_rx_test). The ADC interrupt hands each sample to the receiver
 * before the next one, 104 us later. PA4 is held at mid-scale by the DAC as
 * the af_capture service does. The battery shares the ADC: the interrupt
 * reads it about once a second, between frames.
 * The inbox is kept in external flash too: aprs_store.c.
 */
#include <stddef.h>
#include <string.h>
#include "py32f0xx.h"

#include "app/aprs_bg.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "app/aprs_store.h"
#include "onepress/aprs_rx.h"

_Static_assert(sizeof(app_rx_msg_t) == sizeof(aprs_msg_t) && APP_RX_TEXT_MAX == APRS_RX_TEXT &&
               offsetof(app_rx_msg_t, text) == offsetof(aprs_msg_t, text) &&
               offsetof(app_rx_msg_t, id) == offsetof(aprs_msg_t, id) &&
               offsetof(app_rx_msg_t, unread) == offsetof(aprs_msg_t, unread), "app_rx_msg_t mirrors aprs_msg_t");

#define ADC_CH_PA4      4u
#define SMP4_POS        12u
#define SMP8_POS        24u
#define EXTSEL_MASK     ADC_CR2_EXTSEL
#define EXTSEL_TIM15    (ADC_CR2_EXTSEL_2 | ADC_CR2_EXTSEL_0)
#define EXTSEL_SW       (ADC_CR2_EXTSEL_2 | ADC_CR2_EXTSEL_1 | ADC_CR2_EXTSEL_0)
#define DAC_BIAS        ((1u << 0) | (1u << 1) | (1u << 2) | (7u << 3))  /* EN1 BOFF1 TEN1 TSEL1=111 */
#define BATTERY_EVERY   9600u                                          /* samples: a second */

enum { OFF, RUNNING, PAUSED };

static struct {
    uint32_t moder, rcc1, rcc2, dac, dhr, sqr3, smpr3, cr1, cr2;
} saved;
static volatile uint8_t state;
static volatile uint16_t battery, sinceBattery, lo = 0xFFFFu, hi;
static volatile uint32_t samples;
static char station[7];
static uint8_t stationSsid;

/* One software conversion of the battery channel, then back to the audio.
 * Called with the trigger stopped or from the interrupt, between samples. */
static uint16_t readBattery(void)
{
    ADC1->SQR3  = saved.sqr3;
    ADC1->SMPR3 = saved.smpr3;
    ADC1->CR2   = (ADC1->CR2 & ~EXTSEL_MASK) | EXTSEL_SW | ADC_CR2_EXTTRIG | ADC_CR2_SWSTART;
    for (uint16_t n = 0; !(ADC1->SR & ADC_SR_EOC) && n < 2000u; n++) {}
    const uint16_t v = (uint16_t)(ADC1->DR & 0x0FFFu);
    ADC1->SMPR3 = (saved.smpr3 & ~(7u << SMP4_POS)) | (((saved.smpr3 >> SMP8_POS) & 7u) << SMP4_POS);
    ADC1->SQR3  = (saved.sqr3 & ~0x1Fu) | ADC_CH_PA4;
    ADC1->CR2   = (ADC1->CR2 & ~EXTSEL_MASK) | EXTSEL_TIM15 | ADC_CR2_EXTTRIG;
    return v;
}

/* TIM15's clock: the 48 MHz core clock through the AHB and APB prescalers set
 * by the bootloader, doubled when the APB one divides. */
static uint32_t timerClock(void)
{
    const uint32_t cfgr = RCC->CFGR;
    uint32_t hpre = (cfgr & RCC_CFGR_HPRE_Msk) >> RCC_CFGR_HPRE_Pos, ppre = (cfgr & RCC_CFGR_PPRE_Msk) >> RCC_CFGR_PPRE_Pos;
    uint32_t hclk = 48000000u;
    if (hpre & 8u) hclk >>= (hpre & 7u) < 4u ? (hpre & 7u) + 1u : (hpre & 7u) + 2u;   /* /2 /4 /8 /16, then /64../512 */
    return ppre & 4u ? (hclk >> ((ppre & 3u) + 1u)) * 2u : hclk;
}

static void hwStart(void)
{
    saved.moder = GPIOA->MODER;
    saved.rcc1  = RCC->APBENR1;
    saved.rcc2  = RCC->APBENR2;
    saved.dac   = DAC1->CR;
    saved.dhr   = DAC1->DHR12R1;
    saved.sqr3  = ADC1->SQR3;
    saved.smpr3 = ADC1->SMPR3;
    saved.cr1   = ADC1->CR1;
    saved.cr2   = ADC1->CR2;
    GPIOA->MODER  = saved.moder | (3u << 8);        /* PA4 analog */
    RCC->APBENR1  = saved.rcc1 | RCC_APBENR1_DACEN;
    RCC->APBENR2  = saved.rcc2 | RCC_APBENR2_TIM15EN;
    DAC1->CR      = DAC_BIAS;
    DAC1->DHR12R1 = 2048u;
    DAC1->SWTRIGR = 1u;                             /* DHR -> DOR */
    battery = readBattery();                        /* also selects PA4 and the TIM15 trigger */
    sinceBattery = 0;
    ADC1->CR1 |= ADC_CR1_EOCIE;
    NVIC_SetPriority(ADC_COMP_IRQn, 0);
    NVIC_ClearPendingIRQ(ADC_COMP_IRQn);
    NVIC_EnableIRQ(ADC_COMP_IRQn);
    TIM15->CR1  = 0;
    TIM15->PSC  = 0;
    TIM15->ARR  = timerClock() / 9600u - 1u;
    TIM15->CR2  = TIM_CR2_MMS_1;                    /* TRGO on update */
    TIM15->EGR  = TIM_EGR_UG;
    TIM15->CR1  = TIM_CR1_CEN;
}

static void hwStop(void)
{
    TIM15->CR1 = 0;
    NVIC_DisableIRQ(ADC_COMP_IRQn);
    ADC1->CR1   = saved.cr1;
    ADC1->CR2   = saved.cr2;
    (void)ADC1->DR;                                 /* clears a last EOC */
    NVIC_ClearPendingIRQ(ADC_COMP_IRQn);
    ADC1->SQR3  = saved.sqr3;
    ADC1->SMPR3 = saved.smpr3;
    DAC1->CR      = saved.dac;
    DAC1->DHR12R1 = saved.dhr;
    RCC->APBENR1  = saved.rcc1;
    RCC->APBENR2  = saved.rcc2;
    GPIOA->MODER  = saved.moder;
}

void ADC_COMP_IRQHandler(void)
{
    if (!(ADC1->SR & ADC_SR_EOC))
        return;
    const uint16_t v = (uint16_t)(ADC1->DR & 0x0FFFu); /* reading DR clears EOC */
    aprs_rx_sample(v);
    samples++;
    if (v < lo) lo = v;
    if (v > hi) hi = v;
    if (++sinceBattery >= BATTERY_EVERY && (!aprs_rx_busy() || sinceBattery >= 3u * BATTERY_EVERY)) {
        battery = readBattery();
        sinceBattery = 0;
    }
}

void APRS_BG_Tune(void)
{
    if (state != RUNNING)
        return;
    BK4819_SetAF(BK4819_AF_FM);                     /* the RX audio out of the BK4829 ... */
    AUDIO_AudioPathOff();                           /* ... and the speaker off */
}

/* A tone through the BK4829 left it in its tone mode: back to RX. */
void APRS_BG_AfterTone(void)
{
    if (state != RUNNING)
        return;
    BK4819_TurnsOffTones_TurnsOnRX();
    APRS_BG_Tune();
}

void APRS_BG_Start(const char *call, uint8_t ssid)
{
    char c[7] = { 0 };
    for (uint8_t i = 0; i < 6u && call[i]; i++) c[i] = call[i];
    if (!c[0])
        return;
    if (state == RUNNING)
        hwStop();
    APRS_STORE_Sync();
    if (memcmp(c, station, 7) || ssid != stationSsid) {   /* another station: the inbox stays */
        aprs_rx_init(c, ssid);
        samples = 0;
        memcpy(station, c, 7);
        stationSsid = ssid;
    }
    hwStart();
    state = RUNNING;
    APRS_BG_Tune();
}

void APRS_BG_Stop(void)
{
    if (state == RUNNING)
        hwStop();
    state = OFF;
}

void APRS_BG_Pause(void)
{
    if (state != RUNNING)
        return;
    hwStop();
    state = PAUSED;
}

void APRS_BG_Resume(void)
{
    if (state != PAUSED)
        return;
    hwStart();
    state = RUNNING;
    APRS_BG_Tune();
}

bool APRS_BG_Station(char *call, uint8_t *ssid)
{
    memcpy(call, station, 7);
    *ssid = stationSsid;
    return state != OFF;
}

/* The inbox outlives the receiver and power off: readable when it is off. */
uint8_t       APRS_BG_Count(void)       { APRS_STORE_Sync(); return aprs_rx_count(); }
app_rx_msg_t *APRS_BG_Msg(uint8_t n)    { APRS_STORE_Sync(); return (app_rx_msg_t *)aprs_rx_msg(n); }
uint8_t       APRS_BG_Unread(void)      { APRS_STORE_Sync(); return aprs_rx_unread(); }

void APRS_BG_Stats(app_rx_stats_t *s)
{
    s->samples = samples;
    s->frames  = aprs_rx_frames;
    s->lo      = lo;
    s->hi      = hi;
    lo = 0xFFFFu;
    hi = 0;
    s->battery = battery;
    s->state   = state;
    s->pad     = 0;
}

bool APRS_BG_Battery(uint16_t *voltage)
{
    if (state != RUNNING)
        return false;
    *voltage = battery;
    return true;
}

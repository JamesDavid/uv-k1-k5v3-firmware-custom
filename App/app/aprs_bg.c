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
 * Numbered messages are acknowledged from here: APRS_BG_Service, called while
 * an app waits, keys the ack ~1.5 s after the message once the channel is
 * quiet, on the TX VFO, with the APRS TX app's AFSK (as OP APRS).
 */
#include <stddef.h>
#include <string.h>
#include "py32f0xx.h"

#include "app/aprs_bg.h"
#include "audio.h"
#include "driver/bk4819.h"
#include "app/aprs_store.h"
#include "frequencies.h"
#include "helper/battery.h"
#include "onepress/aprs_rx.h"
#include "onepress/aprs_tx.h"
#include "radio.h"
#include "scheduler.h"

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
#define ACK_DELAY_MS    1500u   /* after the message: digipeaters repeat it first */
#define ACK_RETRY_MS    300u    /* a frame on the air: later */
#define CYC_PER_BIT     (48000000u / 1200u)
#define F_MARK          12389u  /* REG_71 tone words, as APRS TX */
#define F_SPACE         22714u
#define AFSK_LVL        66u
#define TXDELAY_FLAGS   40u
#define TAIL_FLAGS      3u

enum { OFF, RUNNING, PAUSED };

static struct {
    uint32_t moder, rcc1, rcc2, dac, dhr, sqr3, smpr3, cr1, cr2;
} saved;
static volatile uint8_t state;
static volatile uint16_t battery, sinceBattery, lo = 0xFFFFu, hi;
static volatile uint32_t samples;
static char station[7];
static uint8_t stationSsid;             /* SSID, path << 4 for the acks */
static struct { char to[10], id[6], as[10]; uint32_t at; bool pending; } ackq;
static bool seeded;

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

/* ---- the ack: AFSK on the TX VFO ---- */
static struct { uint32_t prev, cyc, next; uint8_t space, ones; } afsk;
static uint32_t cycles(void)
{
    const uint32_t v = SysTick->VAL;
    afsk.cyc += v <= afsk.prev ? afsk.prev - v : afsk.prev + (SysTick->LOAD + 1u - v);
    afsk.prev = v;
    return afsk.cyc;
}
static void afskTone(void)
{
    BK4819_WriteRegister(0x71, (uint16_t)(afsk.space ? F_SPACE : F_MARK));
    BK4819_WriteRegister(0x70, (uint16_t)(0x8000u | (AFSK_LVL << 8)));
}
static void afskBit(uint8_t b)
{
    while ((int32_t)(cycles() - afsk.next) < 0) {}
    if (!b) { afsk.space ^= 1u; afskTone(); }
    afsk.next += CYC_PER_BIT;
}
static void afskByte(uint8_t v, bool stuff)
{
    for (uint8_t i = 0; i < 8u; i++, v >>= 1) {
        const uint8_t b = v & 1u;
        afskBit(b);
        if (stuff) {
            if (!b) afsk.ones = 0;
            else if (++afsk.ones == 5u) { afskBit(0); afsk.ones = 0; }
        }
    }
}

/* As the apps' tx_state: TX allowed on the TX VFO, battery fine, FM. */
static bool txAllowed(void)
{
    if (TX_freq_check(gTxVfo->pTX->Frequency) != 0 && gTxVfo->TX_LOCK) return false;
    if (gBatteryDisplayLevel == 0 || gBatteryDisplayLevel > 6) return false;
    return gTxVfo->Modulation == MODULATION_FM;
}

static void sendAck(void)
{
    uint8_t f[APRS_TX_FRAME_MAX];
    char text[10] = "ack";
    memcpy(text + 3, ackq.id, 6);
    const uint8_t n = aprs_tx_frame(f, ackq.as, (uint8_t)((stationSsid >> 4) & 3u), ackq.to, text);
    hwStop();                                       /* the interrupt off: even bits */
    RADIO_SetTxParameters();
    const uint16_t io = BK4819_ReadRegister(0x33);  /* the red LED, as the apps light it */
    if (io & 0x20u) BK4819_WriteRegister(0x33, (uint16_t)(io | 1u));
    BK4819_WriteRegister(0x51, 0);                  /* no CTCSS/DCS under the AFSK */
    BK4819_TransmitTone(false, 1200);
    afsk.space = 0;
    afskTone();
    afsk.prev = SysTick->VAL;
    afsk.cyc = 0;
    afsk.next = CYC_PER_BIT;
    for (uint8_t i = 0; i < TXDELAY_FLAGS; i++) afskByte(0x7E, false);
    afsk.ones = 0;
    for (uint8_t i = 0; i < n; i++) afskByte(f[i], true);
    for (uint8_t i = 0; i < TAIL_FLAGS; i++) afskByte(0x7E, false);
    while ((int32_t)(cycles() - afsk.next) < 0) {}
    BK4819_EnterTxMute();
    BK4819_ToggleGpioOut(BK4819_GPIO1_PIN29_PA_ENABLE, false);
    RADIO_SetupRegisters(true);
    hwStart();
    APRS_BG_Tune();
}

void APRS_BG_Service(void)
{
    if (state != RUNNING)
        return;
    APRS_STORE_Sync();
    const uint32_t now = SCHEDULER_GetTick10ms() * 10u;
    if (aprs_rx_ack_due(ackq.to, ackq.id, ackq.as)) {
        ackq.at = now + ACK_DELAY_MS;
        ackq.pending = true;
    }
    if (!ackq.pending || (int32_t)(now - ackq.at) < 0)
        return;
    if (aprs_rx_busy()) { ackq.at = now + ACK_RETRY_MS; return; }
    ackq.pending = false;
    if (txAllowed())
        sendAck();
}

uint8_t APRS_BG_MsgId(const char *to, const char *text)
{
    if (!seeded) {                                  /* a number unlikely to repeat the last session's */
        aprs_rx_seed((uint8_t)(SysTick->VAL ^ SCHEDULER_GetTick10ms()));
        seeded = true;
    }
    return aprs_rx_msgid(to, text);
}
uint8_t APRS_BG_Delivered(void) { return aprs_rx_delivered(); }

void APRS_BG_Start(const char *call, uint8_t ssid)
{
    char c[7] = { 0 };
    for (uint8_t i = 0; i < 6u && call[i]; i++) c[i] = call[i];
    if (!c[0])
        return;
    if (state == RUNNING)
        hwStop();
    APRS_STORE_Sync();
    if (memcmp(c, station, 7) || (ssid & 15u) != (stationSsid & 15u)) {   /* another station: the inbox stays */
        aprs_rx_init(c, ssid & 15u);
        samples = 0;
        memcpy(station, c, 7);
    }
    stationSsid = ssid;
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

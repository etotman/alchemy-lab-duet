/**
 * @file panel_i2c.cpp
 * @brief I2C4 slave panel-display link. See panel_i2c.h.
 *
 * The frames began life on a one-way UART link, which is why they are small
 * and self-delimiting. Over I2C the transport is:
 *
 *   master READ, always 32 bytes:   [n] [n stream bytes] [zero pad]
 *       n <= 31. The stream is the same AA 55 type len payload crc8 byte
 *       stream the UART carried; a frame may straddle two reads.
 *   master WRITE, 1 byte:           0x01 = ANNOUNCE
 *       "I have no labels for you" - sent by a receiver that booted after
 *       the module, or that sees a module it has not been introduced to.
 *   master WRITE, 1..8 bytes:       first byte >= 0x20 = an app message,
 *       handed whole to the firmware by ReceiveMessage(). The way one
 *       module hears another: the Mega relays it.
 *
 * Three things here are load-bearing:
 *
 * 1. **libDaisy's I2CHandle cannot do this.** Its slave mode is blocking
 *    (HAL_I2C_Slave_Transmit waits for the master to show up), and I2C4 has
 *    no DMA in libDaisy. So this drives the I2C4 registers directly from its
 *    own IRQ handlers, which libDaisy does not define. Do not "tidy" this
 *    into an I2CHandle.
 *
 * 2. **Clock stretching is on, and it is what makes the timing work.** The
 *    audio DMA interrupt sits at NVIC priority 0 and handpan's callback eats
 *    50-75 % of every block, so the I2C4 ISR routinely waits a few hundred
 *    microseconds for its turn. SCL is held low until it gets it, and the
 *    master simply waits. Nothing is lost; a 32-byte read just takes a few
 *    ms instead of 3. Never set NOSTRETCH.
 *
 * 3. **The ring has one writer and one reader in different contexts.** The
 *    control loop pushes (QueueFrame), the ISR pops (PrepareResponse). Head
 *    is published once per whole frame, after a barrier, so the ISR can never
 *    hand out a half-written frame.
 */

#include "common/panel_i2c.h"

#include "daisy_seed.h"
#include "per/gpio.h"
#include "alchemy/hw/alchemy_lab.h"
#include "alchemy/surface/jack.h"
#include "alchemy/surface/page.h"
#include "alchemy/surface/pager.h"
#include "alchemy/surface/virtual_button.h"
#include "alchemy/surface/virtual_knob.h"

#include <cstring>

using namespace alchemy;

/* Diagnostic: drive both pins as plain GPIO square waves instead of I2C, so
 * B3 and B5 can be told apart with a multimeter against header pin 5 (GND).
 *   PB6 (B3, SCL): 1 Hz    - 0.5 s high, 0.5 s low
 *   PB7 (B5, SDA): 0.25 Hz - 2 s high, 2 s low
 * The header's B1..B8 are the Seed2 DFM's own pad names; Electro-Smith's
 * Seed2_DFM_pinout.csv gives B3 = D13 = PB6 and B5 = D14 = PB7.
 * 0 = normal operation. */
#define PANEL_I2C_PIN_TEST 0

namespace panel_i2c {
namespace {

/* ── Wire format (must match mega/duet/duet.ino) ──────────────────────── */

constexpr uint8_t kMsgText   = 0x11;   /* slot, text[<=12]               */
constexpr uint8_t kMsgState  = 0x12;   /* page, btn_flags, act, val[6]   */
constexpr uint8_t kMsgCommit = 0x13;   /* kind, page, n_pages, r, g, b   */
constexpr uint8_t kMsgEvent  = 0x14;   /* app bytes[<=8], see SendEvent  */

constexpr uint8_t kCmdAnnounce = 0x01; /* master write: send everything  */
constexpr uint8_t kCmdAppFirst = 0x20; /* master write: app message      */

/* Received app messages. Written by the ISR at STOP, read by the control
 * loop - one writer each side, so head/tail need no lock. */
constexpr uint8_t kRxSlots = 8;        /* power of two                   */

constexpr uint8_t kReadLen   = 32;     /* = AVR Wire BUFFER_LENGTH        */

constexpr uint8_t kLbl       = 12;
constexpr uint8_t kNumKnobs  = 6;
constexpr uint8_t kNumBtns   = 3;
constexpr uint8_t kMaxJacks  = 10;

constexpr uint8_t kSlotPage  = 9;      /* 0-5 knobs, 6-8 buttons          */
constexpr uint8_t kSlotName  = 10;
constexpr uint8_t kSlotJack0 = 11;     /* 11-20                           */

constexpr uint8_t kCommitLabels = 0;   /* page change - repaint the text  */
constexpr uint8_t kCommitAll    = 1;   /* module change - repaint all     */

/* ── Sizing ──────────────────────────────────────────────────────────── */

constexpr uint16_t kTextWorst     = 5u + 1u + kLbl;             /* 18 B  */
constexpr uint16_t kCommitBytes   = 5u + 6u;                    /* 11 B  */
constexpr uint16_t kPageWorst     = 10u * kTextWorst + kCommitBytes;
constexpr uint16_t kAnnounceWorst = (11u + kMaxJacks) * kTextWorst + kCommitBytes;

/* A burst is queued only when it fits whole - otherwise it stays pending and
 * goes out a frame later. A partial burst would lose its COMMIT, and a
 * receiver without a COMMIT renders nothing. */
constexpr uint16_t kTxSize = 1024;
static_assert(kTxSize > kAnnounceWorst + kPageWorst, "ring must hold a burst");

/* STATE is latest-wins, so it is only queued onto a nearly empty ring. With a
 * master that pulls, the ring backs up whenever the TFT is busy redrawing;
 * stale knob positions queued behind that would just be drawn and overdrawn. */
constexpr uint16_t kStateGate = 64;

constexpr uint8_t  kStateDivider  = 2u;   /* 60 Hz frame -> 30 Hz state   */
constexpr float    kActivityDelta = 0.004f;

/* BUSY held this long with no event for us means a wedged bus (typically the
 * Mega reset mid-read with our SDA low). Toggling PE releases both lines. */
constexpr uint32_t kStuckMs = 100u;

/* I2C4's kernel clock is set explicitly to rcc_pclk4 (APB4, 120 MHz) in
 * Init. libDaisy never selects one - its I2c4ClockSelection = PLL3 is not in
 * the PeriphClockSelection mask. Measured 2026-09-27 it was already pclk4
 * (the reset default), so this only pins down what TIMINGR assumes. As a slave only the data delays matter:
 * PRESC 5 -> 50 ns ticks, SCLDEL 5 -> 300 ns setup, SDADEL 2 -> 100 ns hold,
 * inside spec for 100 kHz and 400 kHz masters. SCLH/SCLL are master-only. */
constexpr uint32_t kTimingR = (5u << 28) | (5u << 20) | (2u << 16) | 0x0F13u;

constexpr uint32_t kIrqPriority = 2u;

/* ── State ───────────────────────────────────────────────────────────── */

bool ready_ = false;

#if PANEL_I2C_PIN_TEST
daisy::GPIO test_scl_, test_sda_;
uint32_t    test_next_ = 0;
uint8_t     test_phase_ = 0;
#endif

AlchemyLab*         hw_    = nullptr;
Pager*              pager_ = nullptr;
Page* const*        pages_ = nullptr;
uint8_t             num_pages_ = 0;
const char*         name_  = "";
const Jack* const*  jacks_ = nullptr;
uint8_t             num_jacks_ = 0;
const char* const*  btns_  = nullptr;

uint8_t  tick_       = 0;
uint8_t  last_page_  = 0xFF;
bool     announced_  = false;
bool     page_pending_ = false;
float    last_norm_[kNumKnobs] = {};
bool     last_btn_[kNumBtns]   = {};

/* Ring: head written only by the control loop, tail only by the ISR. */
uint8_t           tx_[kTxSize];
volatile uint16_t tx_head_ = 0, tx_tail_ = 0;

/* ISR-side. */
uint8_t           resp_[kReadLen];
volatile uint8_t  resp_idx_     = 0;
volatile uint8_t  rx_idx_       = 0;
volatile bool     rx_open_      = false;   /* inside a master write      */
uint8_t           rx_buf_[kMaxMsg];
volatile bool     announce_req_ = false;

struct RxMsg { uint8_t len; uint8_t data[kMaxMsg]; };
RxMsg             rxq_[kRxSlots];
volatile uint8_t  rxq_head_ = 0, rxq_tail_ = 0;   /* head: ISR, tail: loop */
volatile uint32_t evt_count_    = 0;

uint32_t watch_evt_   = 0;
uint32_t watch_since_ = 0;

/* Register dump served as HELLO's build string - see Diag(). */
char     diag_[96]     = "i2c";
uint32_t d3ccipr_boot_ = 0;
uint32_t next_diag_    = 0;

inline uint16_t TxUsed() { return (uint16_t)((tx_head_ - tx_tail_) & (kTxSize - 1)); }
inline uint16_t TxFree() { return (uint16_t)(kTxSize - 1 - TxUsed()); }

uint8_t Crc8(uint8_t crc, uint8_t b)
{
    crc ^= b;
    for (uint8_t i = 0; i < 8; i++)
        crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
    return crc;
}

/* Whole frame or nothing, and head moves once at the end: the ISR may read
 * the ring at any instant and must only ever see complete frames. */
void QueueFrame(uint8_t type, const uint8_t* payload, uint8_t len)
{
    const uint16_t need = (uint16_t)(len + 5u);
    if (TxFree() < need) return;

    uint8_t crc = Crc8(0u, type);
    crc         = Crc8(crc, len);
    for (uint8_t i = 0; i < len; i++) crc = Crc8(crc, payload[i]);

    uint16_t h    = tx_head_;
    auto     push = [&h](uint8_t b) {
        tx_[h] = b;
        h      = (uint16_t)((h + 1) & (kTxSize - 1));
    };
    push(0xAAu);
    push(0x55u);
    push(type);
    push(len);
    for (uint8_t i = 0; i < len; i++) push(payload[i]);
    push(crc);

    __DMB();
    tx_head_ = h;
}

void QueueText(uint8_t slot, const char* s)
{
    uint8_t buf[1 + kLbl];
    buf[0]      = slot;
    uint8_t n   = 0;
    if (s)
        while (n < kLbl && s[n]) { buf[1 + n] = (uint8_t)s[n]; n++; }
    QueueFrame(kMsgText, buf, (uint8_t)(1u + n));
}

/* "#ffa830" -> 0xff, 0xa8, 0x30. Anything unparseable renders white. */
void ParseColor(const char* css, uint8_t out[3])
{
    out[0] = out[1] = out[2] = 0xFFu;
    if (!css) return;
    if (*css == '#') css++;
    for (uint8_t i = 0; i < 6; i++)
        if (!css[i]) return;
    for (uint8_t i = 0; i < 3; i++)
    {
        uint8_t v = 0;
        for (uint8_t j = 0; j < 2; j++)
        {
            const char c = css[i * 2 + j];
            uint8_t    d = 0;
            if      (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint8_t)(c - 'A' + 10);
            else return;
            v = (uint8_t)((v << 4) | d);
        }
        out[i] = v;
    }
}

const Page* PageByIndex(uint8_t idx)
{
    for (uint8_t i = 0; i < num_pages_; i++)
        if (pages_[i] && pages_[i]->Index() == idx) return pages_[i];
    return nullptr;
}

/* Legends for one page, placed by the pot each knob sits on. */
void QueuePageLabels(uint8_t idx)
{
    const Page* p = PageByIndex(idx);
    if (!p) return;

    const char* by_pot[kNumKnobs] = {};
    for (uint8_t i = 0; i < p->Count(); i++)
    {
        const VirtualKnob* k = p->At(i);
        if (k && k->Pot() < kNumKnobs) by_pot[k->Pot()] = k->Name();
    }
    for (uint8_t i = 0; i < kNumKnobs; i++) QueueText(i, by_pot[i]);

    if (btns_)
    {
        for (uint8_t i = 0; i < kNumBtns; i++)
            QueueText((uint8_t)(6u + i), btns_[(uint16_t)idx * kNumBtns + i]);
    }
    else
    {
        const char* by_hw[kNumBtns] = {};
        for (uint8_t i = 0; i < p->NumButtons(); i++)
        {
            const VirtualButton* b = p->ButtonAt(i);
            if (b && b->HasHw() && b->HwIndex() < kNumBtns)
                by_hw[b->HwIndex()] = b->Name();
        }
        for (uint8_t i = 0; i < kNumBtns; i++) QueueText((uint8_t)(6u + i), by_hw[i]);
    }

    QueueText(kSlotPage, p->TabName());
}

void QueueCommit(uint8_t kind, uint8_t idx)
{
    const Page* p = PageByIndex(idx);
    uint8_t     buf[6];
    buf[0] = kind;
    buf[1] = idx;
    buf[2] = num_pages_;
    ParseColor(p ? p->TabColor() : nullptr, &buf[3]);
    QueueFrame(kMsgCommit, buf, sizeof(buf));
}

/* Identity, jack silk and the current page. Returns false (nothing queued)
 * if the burst would not fit whole. */
bool QueueAnnounce()
{
    if (TxFree() < kAnnounceWorst) return false;

    QueueText(kSlotName, name_);
    for (uint8_t i = 0; i < num_jacks_ && i < kMaxJacks; i++)
        QueueText((uint8_t)(kSlotJack0 + i), jacks_[i] ? jacks_[i]->Id() : "");

    const uint8_t idx = pager_ ? pager_->ActivePage() : 0u;
    QueuePageLabels(idx);
    QueueCommit(kCommitAll, idx);
    last_page_    = idx;
    page_pending_ = false;
    return true;
}

bool QueuePageChange()
{
    if (TxFree() < kPageWorst) return false;
    const uint8_t idx = pager_ ? pager_->ActivePage() : 0u;
    QueuePageLabels(idx);
    QueueCommit(kCommitLabels, idx);
    last_page_ = idx;
    return true;
}

void QueueState()
{
    const uint8_t idx = pager_ ? pager_->ActivePage() : 0u;
    const Page*   p   = PageByIndex(idx);

    uint8_t buf[3 + kNumKnobs] = {};
    buf[0] = idx;

    bool active = false;

    float norm[kNumKnobs] = {};
    if (p)
        for (uint8_t i = 0; i < p->Count(); i++)
        {
            const VirtualKnob* k = p->At(i);
            if (k && k->Pot() < kNumKnobs) norm[k->Pot()] = k->Norm();
        }
    for (uint8_t i = 0; i < kNumKnobs; i++)
    {
        float n = norm[i];
        if (n < 0.f) n = 0.f;
        if (n > 1.f) n = 1.f;
        buf[3 + i] = (uint8_t)(n * 255.f + 0.5f);

        float d = n - last_norm_[i];
        if (d < 0.f) d = -d;
        if (d > kActivityDelta) active = true;
        last_norm_[i] = n;
    }

    uint8_t flags = 0;
    if (hw_)
        for (uint8_t i = 0; i < kNumBtns; i++)
        {
            /* Pressed(), not RisingEdge(): the app already consumes B3's
             * edge, and a second reader must not depend on who polled first. */
            const bool down = hw_->buttons[i].Pressed();
            if (down) flags |= (uint8_t)(1u << i);
            if (down != last_btn_[i]) active = true;
            last_btn_[i] = down;
        }
    buf[1] = flags;
    buf[2] = active ? 1u : 0u;

    QueueFrame(kMsgState, buf, sizeof(buf));
}

/* ── I2C4 slave ──────────────────────────────────────────────────────── */

void InitPins()
{
    __HAL_RCC_GPIOB_CLK_ENABLE();
    GPIO_InitTypeDef g = {};
    g.Pin       = GPIO_PIN_6 | GPIO_PIN_7;
    g.Mode      = GPIO_MODE_AF_OD;
    /* No pull-up here: the Mega's own 10k pull-ups to 5 V set the bus level
     * (the AVR needs 3.5 V for a TWI high). PB6/PB7 are 5 V tolerant. */
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_LOW;
    g.Alternate = GPIO_AF6_I2C4;
    HAL_GPIO_Init(GPIOB, &g);
}

void EnablePeripheral(uint8_t addr7)
{
    I2C_TypeDef* i2c = I2C4;
    i2c->CR1  = 0;                              /* PE off: config is legal */
    i2c->TIMINGR = kTimingR;
    i2c->OAR1 = 0;
    i2c->OAR1 = ((uint32_t)addr7 << 1) | I2C_OAR1_OA1EN;
    i2c->OAR2 = 0;
    i2c->CR2  = 0;
    /* Analog filter on (ANFOFF = 0), no digital filter, stretching allowed
     * (NOSTRETCH = 0), no SBC - slave ACKs every byte it is written. */
    i2c->CR1 = I2C_CR1_ADDRIE | I2C_CR1_RXIE | I2C_CR1_TXIE | I2C_CR1_STOPIE
             | I2C_CR1_NACKIE | I2C_CR1_ERRIE | I2C_CR1_PE;
}

/* PE low for at least 3 APB cycles resets the state machine and releases
 * SCL/SDA. Register contents (address, timing) survive. */
void ResetPeripheral()
{
    I2C4->CR1 &= ~I2C_CR1_PE;
    while (I2C4->CR1 & I2C_CR1_PE) {}
    for (volatile int i = 0; i < 16; i++) {}
    I2C4->CR1 |= I2C_CR1_PE;
    resp_idx_ = kReadLen;
    rx_idx_   = 0;
    rx_open_  = false;
}

/* One read's worth, taken off the ring at the address match. Committed then
 * rather than byte by byte, because TXDR is loaded a byte ahead of the wire
 * and a byte-exact count is not available anyway. The master always reads
 * all 32; if it ever aborts early the parser resyncs on the next AA 55. */
void PrepareResponse()
{
    uint16_t n = TxUsed();
    if (n > kReadLen - 1u) n = kReadLen - 1u;
    resp_[0]   = (uint8_t)n;
    uint16_t t = tx_tail_;
    for (uint16_t i = 0; i < n; i++)
    {
        resp_[1 + i] = tx_[t];
        t            = (uint16_t)((t + 1) & (kTxSize - 1));
    }
    for (uint16_t i = (uint16_t)(1 + n); i < kReadLen; i++) resp_[i] = 0;
    tx_tail_  = t;
    resp_idx_ = 0;
}

/* End of a master write: an app message goes on the queue whole. A full
 * queue drops the new one - the firmware has stopped reading, and the
 * oldest messages are the ones it is about to act on. */
void CloseWrite()
{
    if (!rx_open_) return;
    rx_open_ = false;
    if (rx_idx_ == 0 || rx_buf_[0] < kCmdAppFirst) return;

    const uint8_t h    = rxq_head_;
    const uint8_t next = (uint8_t)((h + 1u) & (kRxSlots - 1u));
    if (next == rxq_tail_) return;
    const uint8_t n = rx_idx_ < kMaxMsg ? rx_idx_ : kMaxMsg;
    rxq_[h].len     = n;
    memcpy(rxq_[h].data, rx_buf_, n);
    __DMB();
    rxq_head_ = next;
}

}  // namespace

/* ── IRQ handlers (libDaisy defines none for I2C4) ───────────────────── */

extern "C" void I2C4_EV_IRQHandler()
{
    I2C_TypeDef*   i2c = I2C4;
    const uint32_t isr = i2c->ISR;
    evt_count_ = evt_count_ + 1u;

    if (isr & I2C_ISR_RXNE)
    {
        const uint8_t b = (uint8_t)i2c->RXDR;
        if (rx_idx_ == 0 && b == kCmdAnnounce) announce_req_ = true;
        if (rx_idx_ < kMaxMsg) rx_buf_[rx_idx_] = b;
        if (rx_idx_ < 255) rx_idx_ = (uint8_t)(rx_idx_ + 1);
    }
    if (isr & I2C_ISR_NACKF) i2c->ICR = I2C_ICR_NACKCF;  /* master's last-byte NACK */
    if (isr & I2C_ISR_STOPF)
    {
        i2c->ICR = I2C_ICR_STOPCF;
        CloseWrite();
    }

    if (isr & I2C_ISR_ADDR)
    {
        /* A repeated start ends the write before it just as a STOP would. */
        CloseWrite();

        /* SCL is stretched until ADDRCF, so this can take its time. */
        if (isr & I2C_ISR_DIR)
        {
            i2c->ISR = I2C_ISR_TXE;       /* flush a byte left over from a NACK */
            PrepareResponse();
        }
        else
        {
            rx_idx_  = 0;
            rx_open_ = true;
        }
        i2c->ICR = I2C_ICR_ADDRCF;
        return;                           /* TXIS arrives as its own IRQ */
    }

    if (isr & I2C_ISR_TXIS)
    {
        const uint8_t i = resp_idx_;
        i2c->TXDR       = (i < kReadLen) ? resp_[i] : 0u;
        if (i < 255) resp_idx_ = (uint8_t)(i + 1);
    }
}

extern "C" void I2C4_ER_IRQHandler()
{
    I2C4->ICR = I2C_ICR_BERRCF | I2C_ICR_ARLOCF | I2C_ICR_OVRCF;
}

/* ── Public ──────────────────────────────────────────────────────────── */

void Init(const Config& cfg)
{
    hw_        = cfg.hw;
    pager_     = cfg.pager;
    pages_     = cfg.pages;
    num_pages_ = cfg.num_pages;
    name_      = cfg.module_name ? cfg.module_name : "";
    jacks_     = cfg.jacks;
    num_jacks_ = cfg.num_jacks;
    btns_      = cfg.btn_labels;

#if PANEL_I2C_PIN_TEST
    test_scl_.Init(daisy::seed::D13, daisy::GPIO::Mode::OUTPUT);
    test_sda_.Init(daisy::seed::D14, daisy::GPIO::Mode::OUTPUT);
    test_scl_.Write(false);
    test_sda_.Write(false);
    ready_ = false;
    return;
#endif

    const uint8_t addr = (cfg.address >= 0x08 && cfg.address <= 0x77)
                             ? cfg.address
                             : kDefaultAddress;

    d3ccipr_boot_ = RCC->D3CCIPR;
    __HAL_RCC_I2C4_CONFIG(RCC_I2C4CLKSOURCE_D3PCLK1);

    InitPins();
    __HAL_RCC_I2C4_CLK_ENABLE();
    EnablePeripheral(addr);

    HAL_NVIC_SetPriority(I2C4_EV_IRQn, kIrqPriority, 0);
    HAL_NVIC_SetPriority(I2C4_ER_IRQn, kIrqPriority, 0);
    HAL_NVIC_EnableIRQ(I2C4_EV_IRQn);
    HAL_NVIC_EnableIRQ(I2C4_ER_IRQn);

    resp_idx_    = kReadLen;
    watch_since_ = daisy::System::GetNow();
    ready_       = true;
}

/* k: RCC_D3CCIPR as found at Init (I2C4SEL is bits 9:8)
 * K: RCC_D3CCIPR now       e: APB4ENR (I2C4EN is bit 7)
 * c: I2C4 CR1  o: OAR1  t: TIMINGR  i: ISR
 * m: GPIOB MODER bits 15:12 (PB7,PB6; 2 = AF)  a: AFRL nibbles 7,6 (6 = I2C4)
 * n: ISR events seen */
static void Hex(char*& p, char tag, uint32_t v, uint8_t digits)
{
    static const char kHex[] = "0123456789abcdef";
    *p++ = tag;
    for (int8_t i = (int8_t)digits - 1; i >= 0; i--) *p++ = kHex[(v >> (4 * i)) & 0xFu];
}

static void RefreshDiag()
{
    char tmp[sizeof(diag_)];
    char* p = tmp;
    Hex(p, 'k', d3ccipr_boot_, 8);
    Hex(p, 'K', RCC->D3CCIPR, 8);
    Hex(p, 'e', RCC->APB4ENR, 8);
    Hex(p, 'c', I2C4->CR1, 8);
    Hex(p, 'o', I2C4->OAR1, 4);
    Hex(p, 't', I2C4->TIMINGR, 8);
    Hex(p, 'i', I2C4->ISR, 8);
    Hex(p, 'm', (GPIOB->MODER >> 12) & 0xFu, 1);
    Hex(p, 'a', (GPIOB->AFR[0] >> 24) & 0xFFu, 2);
    Hex(p, 'n', evt_count_, 8);
    *p = 0;
    memcpy(diag_, tmp, sizeof(diag_));
}

const char* Diag() { return diag_; }

bool SendEvent(const uint8_t* data, uint8_t len)
{
    if (!ready_ || !data || len == 0 || len > kMaxMsg) return false;
    if (TxFree() < (uint16_t)(len + 5u)) return false;
    QueueFrame(kMsgEvent, data, len);
    return true;
}

bool ReceiveMessage(uint8_t* data, uint8_t& len)
{
    const uint8_t t = rxq_tail_;
    if (t == rxq_head_) return false;
    __DMB();
    len = rxq_[t].len;
    memcpy(data, rxq_[t].data, len);
    rxq_tail_ = (uint8_t)((t + 1u) & (kRxSlots - 1u));
    return true;
}

void PageChanged()
{
    if (!ready_ || !announced_) return;
    page_pending_ = !QueuePageChange();
}

void Tick()
{
#if PANEL_I2C_PIN_TEST
    const uint32_t now_t = daisy::System::GetNow();
    if ((int32_t)(now_t - test_next_) >= 0)
    {
        test_next_ = now_t + 500u;
        test_phase_ = (uint8_t)((test_phase_ + 1u) & 7u);
        test_scl_.Write(test_phase_ & 1u);          /* 1 Hz    */
        test_sda_.Write((test_phase_ >> 2) & 1u);   /* 0.25 Hz */
    }
    return;
#endif

    if (!ready_) return;

    /* The first announce goes out unasked, so a display that is already up
     * when the module boots gets its labels straight away. After that the
     * receiver asks, which it does whenever it has none. */
    if (!announced_ || announce_req_)
    {
        /* Clear before queueing: a request the ISR takes mid-announce must
         * survive to the next frame, not be wiped by this one. */
        announce_req_ = false;
        if (QueueAnnounce()) announced_ = true;
        else                 announce_req_ = true;
    }
    else
    {
        /* OnPageChange is the intended path, but a jump from GoToPage, a
         * preset load or a host edit must not leave stale text - and a page
         * change that found the ring full is retried here. */
        const uint8_t idx = pager_ ? pager_->ActivePage() : 0u;
        if (page_pending_ || idx != last_page_) page_pending_ = !QueuePageChange();
    }

    if (++tick_ >= kStateDivider)
    {
        tick_ = 0;
        if (TxUsed() < kStateGate) QueueState();
    }

    /* Bus watchdog. */
    const uint32_t now = daisy::System::GetNow();
    if ((int32_t)(now - next_diag_) >= 0)
    {
        next_diag_ = now + 250u;
        RefreshDiag();
    }
    const uint32_t evt = evt_count_;
    if ((I2C4->ISR & I2C_ISR_BUSY) && evt == watch_evt_)
    {
        if ((uint32_t)(now - watch_since_) > kStuckMs)
        {
            ResetPeripheral();
            watch_since_ = now;
        }
    }
    else
    {
        watch_evt_   = evt;
        watch_since_ = now;
    }
}

}  // namespace panel_i2c

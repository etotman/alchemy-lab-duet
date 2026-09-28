/**
 * @file panel_i2c.h
 * @brief Serve panel legends and knob state to an external TFT over I2C.
 *
 * The module is an I2C **slave** on I2C4 and the Mega 2560 driving the TFT
 * is the master, polling it. The frames carry the panel's legends and live
 * state straight from the Page/VirtualKnob/Jack declarations. See
 * mega/duet/duet.ino for the receiver.
 *
 * Pins: header B3 = D13 = PB6 = I2C4_SCL -> Mega SCL, header B5 = D14 = PB7
 * = I2C4_SDA -> Mega SDA (AF6). The first bring-up had these two swapped
 * and failed silently: every address NACKs and the module sees nothing. This is NOT the board's own I2C bus - that is I2C1 on PB8/PB9 and
 * carries the PCA9557 expander and the MCP4728 DAC. PB6/PB7 are also USART1,
 * so nothing else in the firmware may use that UART.
 *
 * Why this beats the UART link: the master pulls, so it only reads when it is
 * ready. A TFT redraw can no longer overrun a 64-byte RX buffer, and the
 * module learns a display exists (it asks for an ANNOUNCE) instead of
 * re-announcing blind on a timer.
 */

#pragma once

#include <cstdint>

/* AlchemyLab is a board-dependent alias (V1/V2), not a class, so it cannot be
 * forward declared - the header has to come in. */
#include "alchemy/hw/alchemy_lab.h"

namespace alchemy {
class Pager;
class Page;
class Jack;
}

namespace panel_i2c {

/* Set per build by the Makefile: `make PANEL_I2C_ADDR=0x43`. */
#ifndef PANEL_I2C_ADDR
#define PANEL_I2C_ADDR 0x42
#endif

/** This module's 7-bit address. Every module on the bus needs its own; the
 *  receiver polls 0x42..0x44. Chosen at build time, not in the source, so one
 *  firmware can run on several modules at once. */
constexpr uint8_t kDefaultAddress = PANEL_I2C_ADDR;

/** What to put on the panel, and the bus address. */
struct Config
{
    alchemy::AlchemyLab*  hw        = nullptr;  /**< for button state       */
    alchemy::Pager*       pager     = nullptr;  /**< the active page        */
    alchemy::Page* const* pages     = nullptr;  /**< every page, any order  */
    uint8_t               num_pages = 0;

    const char*           module_name = "";     /**< panel title            */

    /** Panel jacks in display order: 5 across the upper row, then 5 across
     *  the lower. Fewer than 10 is fine; the rest render blank. */
    const alchemy::Jack* const* jacks     = nullptr;
    uint8_t                     num_jacks = 0;

    /** num_pages * 3 button legends, row-major (page * 3 + button).
     *  Optional: a firmware that declares VirtualButtons gets them read off
     *  the page. Required when the firmware polls hw.buttons[] directly. */
    const char* const* btn_labels = nullptr;

    uint8_t address = kDefaultAddress;          /**< 7-bit, 0x08..0x77      */
};

/** Wire it up. Call once from main(), after hw.Init(). */
void Init(const Config& cfg);

/** From the ControlLoop OnFrame hook. Queues state and serves requests. */
void Tick();

/** From the ControlLoop OnPageChange hook. Queues the new page's legends. */
void PageChanged();

/* ── App messages: module <-> Mega, for firmwares that talk through it ──
 *
 * Modules are all slaves and cannot reach each other, so anything one module
 * says to another is relayed by the Mega. These two calls are the ends of
 * that relay; what the bytes mean is the firmware's (and its Mega sketch's)
 * business. A firmware that uses neither is unaffected. */

/** Longest payload either way. */
constexpr uint8_t kMaxMsg = 8;

/**
 * Up: queue an EVENT frame (type 0x14) carrying @p len bytes. Unlike STATE
 * it is delivered in order and never coalesced. Control thread only - the
 * same thread as Tick(). False if the ring is full (nothing queued).
 */
bool SendEvent(const uint8_t* data, uint8_t len);

/**
 * Down: the next message the master wrote. The master's write is the whole
 * message, first byte included; first bytes 0x20..0xFF are app messages
 * (0x01 is ANNOUNCE and never appears here). Control thread only. False when
 * there is nothing waiting. @p data must hold kMaxMsg bytes; a longer write
 * is truncated to that.
 */
bool ReceiveMessage(uint8_t* data, uint8_t& len);

/**
 * Live register dump (I2C4, its clock, PB6/PB7), refreshed every 250 ms by
 * Tick(). Pass it as hostlink::Host's git_hash and HELLO carries it to the
 * Mac - tools/hostlink_reboot.py hello - which is the only way to see this
 * peripheral without a debugger. Reads "i2c" until the first Tick().
 */
const char* Diag();

}  // namespace panel_i2c

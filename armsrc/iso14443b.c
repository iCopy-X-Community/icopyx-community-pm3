//-----------------------------------------------------------------------------
// Jonathan Westhues, split Nov 2006
// piwi 2018
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// Routines to support ISO 14443B. This includes both the reader software and
// the `fake tag' modes.
//-----------------------------------------------------------------------------
#include "iso14443b.h"

#include "proxmark3_arm.h"
#include "common.h"  // access to global variable: DBGLEVEL
#include "util.h"
#include "string.h"
#include "crc16.h"
#include "protocols.h"
#include "appmain.h"
#include "BigBuf.h"
#include "cmd.h"
#include "fpgaloader.h"
#include "commonutil.h"
#include "dbprint.h"
#include "ticks.h"


// Delays in SSP_CLK ticks.
// SSP_CLK runs at 13,56MHz / 32 = 423.75kHz when simulating a tag
#define DELAY_READER_TO_ARM               8
#define DELAY_ARM_TO_READER               0

//SSP_CLK runs at 13.56MHz / 4 = 3,39MHz when acting as reader. All values should be multiples of 16
#define DELAY_ARM_TO_TAG                 16
#define DELAY_TAG_TO_ARM                 32

//SSP_CLK runs at 13.56MHz / 4 = 3,39MHz when sniffing. All values should be multiples of 16
#define DELAY_TAG_TO_ARM_SNIFF           32
#define DELAY_READER_TO_ARM_SNIFF        32

// defaults to 2000ms
#ifndef FWT_TIMEOUT_14B
# define FWT_TIMEOUT_14B 35312
#endif

// 330/848kHz = 1558us / 4 == 400us,
#define ISO14443B_READER_TIMEOUT           1700 //330

// 1024/3.39MHz = 302.1us between end of tag response and next reader cmd
#define DELAY_ISO14443B_VICC_TO_VCD_READER 600 // 1024
#define DELAY_ISO14443B_VCD_TO_VICC_READER 600// 1056

#ifndef RECEIVE_MASK
# define RECEIVE_MASK  (DMA_BUFFER_SIZE - 1)
#endif

// Guard Time (per 14443-2)
#ifndef TR0
# define TR0 64 // TR0 max is 256/fs = 256/(848kHz) = 302us or 64 samples from FPGA
#endif

// Synchronization time (per 14443-2)
#ifndef TR1
# define TR1 0
#endif
// Frame Delay Time PICC to PCD  (per 14443-3 Amendment 1)
#ifndef TR2
# define TR2 0
#endif

// 4sample
#define SEND4STUFFBIT(x) tosend_stuffbit(x);tosend_stuffbit(x);tosend_stuffbit(x);tosend_stuffbit(x);

static void iso14b_set_timeout(uint32_t timeout);
static void iso14b_set_maxframesize(uint16_t size);

// the block number for the ISO14443-4 PCB  (used with APDUs)
static uint8_t pcb_blocknum = 0;
static uint32_t iso14b_timeout = FWT_TIMEOUT_14B;


/* ISO 14443 B
*
* Reader to card | ASK  - Amplitude Shift Keying Modulation (PCD to PICC for Type B) (NRZ-L encodig)
* Card to reader | BPSK - Binary Phase Shift Keying Modulation, (PICC to PCD for Type B)
*
* fc - carrier frequency 13.56 MHz
* TR0 - Guard Time per 14443-2
* TR1 - Synchronization Time per 14443-2
* TR2 - PICC to PCD Frame Delay Time (per 14443-3 Amendment 1)
*
* Elementary Time Unit (ETU) is
* - 128 Carrier Cycles (9.4395 ??S) = 8 Subcarrier Units
* - 1 ETU = 1 bit
* - 10 ETU = 1 startbit, 8 databits, 1 stopbit (10bits length)
* - startbit is a 0
* - stopbit is a 1
*
* Start of frame (SOF) is
* - [10-11] ETU of ZEROS, unmodulated time
* - [2-3] ETU of ONES,
*
* End of frame (EOF) is
* - [10-11] ETU of ZEROS, unmodulated time
*
*  -TO VERIFY THIS BELOW-
* The mode FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_MODULATE_BPSK which we use to simulate tag
* works like this:
* - A 1-bit input to the FPGA becomes 8 pulses at 847.5kHz (1.18??S / pulse) == 9.44us
* - A 0-bit input to the FPGA becomes an unmodulated time of 1.18??S  or does it become 8 nonpulses for 9.44us
*
* FPGA doesn't seem to work with ETU.  It seems to work with pulse / duration instead.
*
* Card sends data ub 847.e kHz subcarrier
* subcar |duration| FC division
* -------+--------+------------
* 106kHz | 9.44??S | FC/128
* 212kHz | 4.72??S | FC/64
* 424kHz | 2.36??S | FC/32
* 848kHz | 1.18??S | FC/16
* -------+--------+------------
*
*  Reader data transmission:
*   - no modulation ONES
*   - SOF
*   - Command, data and CRC_B
*   - EOF
*   - no modulation ONES
*
*  Card data transmission
*   - TR1
*   - SOF
*   - data  (each bytes is:  1startbit, 8bits, 1stopbit)
*   - CRC_B
*   - EOF
*
* FPGA implementation :
* At this point only Type A is implemented. This means that we are using a
* bit rate of 106 kbit/s, or fc/128. Oversample by 4, which ought to make
* things practical for the ARM (fc/32, 423.8 kbits/s, ~50 kbytes/s)
*
* Let us report a correlation every 64 samples. I.e.
* one Q/I pair after 4 subcarrier cycles for the 848kHz subcarrier,
* one Q/I pair after 2 subcarrier cycles for the 424kHz subcarrier,
* one Q/I pair for each subcarrier cyle for the 212kHz subcarrier.
*/



//=============================================================================
// An ISO 14443 Type B tag. We listen for commands from the reader, using
// a UART kind of thing that's implemented in software. When we get a
// frame (i.e., a group of bytes between SOF and EOF), we check the CRC.
// If it's good, then we can do something appropriate with it, and send
// a response.
//=============================================================================

//-----------------------------------------------------------------------------
// Code up a string of octets at layer 2 (including CRC, we don't generate
// that here) so that they can be transmitted to the reader. Doesn't transmit
// them yet, just leaves them ready to send in ToSend[].
//-----------------------------------------------------------------------------
static void CodeIso14443bAsTag(const uint8_t *cmd, int len) {
    int i;

    tosend_reset();

    // Transmit a burst of ones, as the initial thing that lets the
    // reader get phase sync.
    // This loop is TR1, per specification
    // TR1 minimum must be > 80/fs
    // TR1 maximum 200/fs
    // 80/fs < TR1 < 200/fs
    // 10 ETU < TR1 < 24 ETU

    // Send TR1.
    // 10-11 ETU * 4times samples ONES
    for (i = 0; i < 20; i++) {
        SEND4STUFFBIT(1);
    }

    // Send SOF.
    // 10-11 ETU * 4times samples ZEROS
    for (i = 0; i < 10; i++) {
        SEND4STUFFBIT(0);
    }

    // 2-3 ETU * 4times samples ONES
    for (i = 0; i < 2; i++) {
        SEND4STUFFBIT(1);
    }

    // data
    for (i = 0; i < len; i++) {

        // Start bit
        SEND4STUFFBIT(0);

        // Data bits
        uint8_t b = cmd[i];
        for (int j = 0; j < 8; j++) {
            SEND4STUFFBIT(b & 1);
            b >>= 1;
        }

        // Stop bit
        SEND4STUFFBIT(1);

        // Extra Guard bit
        // For PICC it ranges 0-18us (1etu = 9us)
        //SEND4STUFFBIT(1);
    }

    // Send EOF.
    // 10-11 ETU * 4 sample rate = ZEROS
    for (i = 0; i < 10; i++) {
        SEND4STUFFBIT(0);
    }

    // why this?
    for (i = 0; i < 2; i++) {
        SEND4STUFFBIT(1);
    }

    tosend_t *ts = get_tosend();
    // Convert from last byte pos to length
    ts->max++;
}

//-----------------------------------------------------------------------------
// The software UART that receives commands from the reader, and its state
// variables.
//-----------------------------------------------------------------------------
static struct {
    enum {
        STATE_14B_UNSYNCD,
        STATE_14B_GOT_FALLING_EDGE_OF_SOF,
        STATE_14B_AWAITING_START_BIT,
        STATE_14B_RECEIVING_DATA
    }       state;
    uint16_t shiftReg;
    int      bitCnt;
    int      byteCnt;
    int      byteCntMax;
    int      posCnt;
    uint8_t  *output;
} Uart;

static void Uart14bReset(void) {
    Uart.state = STATE_14B_UNSYNCD;
    Uart.shiftReg = 0;
    Uart.bitCnt = 0;
    Uart.byteCnt = 0;
    Uart.byteCntMax = MAX_FRAME_SIZE;
    Uart.posCnt = 0;
}

static void Uart14bInit(uint8_t *data) {
    Uart.output = data;
    Uart14bReset();
}

//-----------------------------------------------------------------------------
// The software Demod that receives commands from the tag, and its state variables.
//-----------------------------------------------------------------------------

#define NOISE_THRESHOLD          80                   // don't try to correlate noise
#define MAX_PREVIOUS_AMPLITUDE   (-1 - NOISE_THRESHOLD)

static struct {
    enum {
        DEMOD_UNSYNCD,
        DEMOD_PHASE_REF_TRAINING,
        DEMOD_AWAITING_FALLING_EDGE_OF_SOF,
        DEMOD_GOT_FALLING_EDGE_OF_SOF,
        DEMOD_AWAITING_START_BIT,
        DEMOD_RECEIVING_DATA
    }       state;
    uint16_t bitCount;
    int      posCount;
    int      thisBit;
    uint16_t shiftReg;
    uint16_t max_len;
    uint8_t  *output;
    uint16_t len;
    int      sumI;
    int      sumQ;
} Demod;

// Clear out the state of the "UART" that receives from the tag.
static void Demod14bReset(void) {
    Demod.state = DEMOD_UNSYNCD;
    Demod.bitCount = 0;
    Demod.posCount = 0;
    Demod.thisBit = 0;
    Demod.shiftReg = 0;
    Demod.len = 0;
    Demod.sumI = 0;
    Demod.sumQ = 0;
}

static void Demod14bInit(uint8_t *data, uint16_t max_len) {
    Demod.output = data;
    Demod.max_len = max_len;
    Demod14bReset();
}


/*
* 9.4395 us = 1 ETU  and clock is about 1.5 us
* 13560000Hz
* 1000ms/s
* timeout in ETUs (time to transfer 1 bit, 9.4395 us)
*
* Formula to calculate FWT (in ETUs) by timeout (in ms):
* fwt = 13560000 * 1000 / (8*16) * timeout;
* Sample:  3sec == 3000ms
*  13560000 * 1000 / (8*16) * 3000  ==
*    13560000000 / 384000 = 35312 FWT
* @param timeout is in frame wait time, fwt, measured in ETUs
*/
static void iso14b_set_timeout(uint32_t timeout) {
#define MAX_TIMEOUT 40542464  // 13560000Hz * 1000ms / (2^32-1) * (8*16)
    if (timeout > MAX_TIMEOUT)
        timeout = MAX_TIMEOUT;

    iso14b_timeout = timeout;
    if (DBGLEVEL >= DBG_DEBUG) Dbprintf("ISO14443B Timeout set to %ld fwt", iso14b_timeout);
}

static void iso14b_set_maxframesize(uint16_t size) {
    if (size > 256)
        size = MAX_FRAME_SIZE;

    Uart.byteCntMax = size;
    if (DBGLEVEL >= DBG_DEBUG) Dbprintf("ISO14443B Max frame size set to %d bytes", Uart.byteCntMax);
}

/* Receive & handle a bit coming from the reader.
 *
 * This function is called 4 times per bit (every 2 subcarrier cycles).
 * Subcarrier frequency fs is 848kHz, 1/fs = 1,18us, i.e. function is called every 2,36us
 *
 * LED handling:
 * LED A -> ON once we have received the SOF and are expecting the rest.
 * LED A -> OFF once we have received EOF or are in error state or unsynced
 *
 * Returns: true if we received a EOF
 *          false if we are still waiting for some more
 */
static RAMFUNC int Handle14443bSampleFromReader(uint8_t bit) {
    switch (Uart.state) {
        case STATE_14B_UNSYNCD:
            if (bit == false) {
                // we went low, so this could be the beginning of an SOF
                Uart.state = STATE_14B_GOT_FALLING_EDGE_OF_SOF;
                Uart.posCnt = 0;
                Uart.bitCnt = 0;
            }
            break;

        case STATE_14B_GOT_FALLING_EDGE_OF_SOF:
            Uart.posCnt++;

            if (Uart.posCnt == 2) { // sample every 4 1/fs in the middle of a bit

                if (bit) {
                    if (Uart.bitCnt > 9) {
                        // we've seen enough consecutive
                        // zeros that it's a valid SOF
                        Uart.posCnt = 0;
                        Uart.byteCnt = 0;
                        Uart.state = STATE_14B_AWAITING_START_BIT;
                        LED_A_ON(); // Indicate we got a valid SOF
                    } else {
                        // didn't stay down long enough before going high, error
                        Uart.state = STATE_14B_UNSYNCD;
                    }
                } else {
                    // do nothing, keep waiting
                }
                Uart.bitCnt++;
            }

            if (Uart.posCnt >= 4) {
                Uart.posCnt = 0;
            }

            if (Uart.bitCnt > 12) {
                // Give up if we see too many zeros without a one, too.
                LED_A_OFF();
                Uart.state = STATE_14B_UNSYNCD;
            }
            break;

        case STATE_14B_AWAITING_START_BIT:
            Uart.posCnt++;

            if (bit) {

                // max 57us between characters = 49 1/fs,
                // max 3 etus after low phase of SOF = 24 1/fs
                if (Uart.posCnt > 50 / 2) {
                    // stayed high for too long between characters, error
                    Uart.state = STATE_14B_UNSYNCD;
                }

            } else {
                // falling edge, this starts the data byte
                Uart.posCnt = 0;
                Uart.bitCnt = 0;
                Uart.shiftReg = 0;
                Uart.state = STATE_14B_RECEIVING_DATA;
            }
            break;

        case STATE_14B_RECEIVING_DATA:

            Uart.posCnt++;

            if (Uart.posCnt == 2) {
                // time to sample a bit
                Uart.shiftReg >>= 1;
                if (bit) {
                    Uart.shiftReg |= 0x200;
                }
                Uart.bitCnt++;
            }

            if (Uart.posCnt >= 4) {
                Uart.posCnt = 0;
            }

            if (Uart.bitCnt == 10) {
                if ((Uart.shiftReg & 0x200) && !(Uart.shiftReg & 0x001)) {
                    // this is a data byte, with correct
                    // start and stop bits
                    Uart.output[Uart.byteCnt] = (Uart.shiftReg >> 1) & 0xFF;
                    Uart.byteCnt++;

                    if (Uart.byteCnt >= Uart.byteCntMax) {
                        // Buffer overflowed, give up
                        LED_A_OFF();
                        Uart.state = STATE_14B_UNSYNCD;
                    } else {
                        // so get the next byte now
                        Uart.posCnt = 0;
                        Uart.state = STATE_14B_AWAITING_START_BIT;
                    }
                } else if (Uart.shiftReg == 0x000) {
                    // this is an EOF byte
                    LED_A_OFF(); // Finished receiving
                    Uart.state = STATE_14B_UNSYNCD;
                    if (Uart.byteCnt != 0)
                        return true;

                } else {
                    // this is an error
                    LED_A_OFF();
                    Uart.state = STATE_14B_UNSYNCD;
                }
            }
            break;

        default:
            LED_A_OFF();
            Uart.state = STATE_14B_UNSYNCD;
            break;
    }
    return false;
}

//-----------------------------------------------------------------------------
// Receive a command (from the reader to us, where we are the simulated tag),
// and store it in the given buffer, up to the given maximum length. Keeps
// spinning, waiting for a well-framed command, until either we get one
// (returns true) or someone presses the pushbutton on the board (false).
//
// Assume that we're called with the SSC (to the FPGA) and ADC path set
// correctly.
//-----------------------------------------------------------------------------
static int GetIso14443bCommandFromReader(uint8_t *received, uint16_t *len) {
    // Set FPGA mode to "simulated ISO 14443B tag", no modulation (listen
    // only, since we are receiving, not transmitting).
    // Signal field is off with the appropriate LED
    LED_D_OFF();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_NO_MODULATION);

    // Now run a `software UART' on the stream of incoming samples.
    Uart14bInit(received);

    while (BUTTON_PRESS() == false) {
        WDT_HIT();

        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
            uint8_t b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
            for (uint8_t mask = 0x80; mask != 0x00; mask >>= 1) {
                if (Handle14443bSampleFromReader(b & mask)) {
                    *len = Uart.byteCnt;
                    return true;
                }
            }
        }
    }
    return false;
}


static void TransmitFor14443b_AsTag(uint8_t *response, uint16_t len) {

    // Signal field is off with the appropriate LED
    LED_D_OFF();

    // Modulate BPSK
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_MODULATE_BPSK);
    AT91C_BASE_SSC->SSC_THR = 0xFF;
    FpgaSetupSsc(FPGA_MAJOR_MODE_HF_SIMULATOR);

    // Transmit the response.
    for (uint16_t i = 0; i < len;) {

        // Put byte into tx holding register as soon as it is ready
        if (AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXRDY) {
            AT91C_BASE_SSC->SSC_THR = response[i++];
        }
    }
}
//-----------------------------------------------------------------------------
// Main loop of simulated tag: receive commands from reader, decide what
// response to send, and send it.
//-----------------------------------------------------------------------------
void SimulateIso14443bTag(uint32_t pupi) {

    LED_A_ON();
    // the only commands we understand is WUPB, AFI=0, Select All, N=1:
//    static const uint8_t cmdWUPB[] = { ISO14443B_REQB, 0x00, 0x08, 0x39, 0x73 }; // WUPB
    // ... and REQB, AFI=0, Normal Request, N=1:
//    static const uint8_t cmdREQB[] = { ISO14443B_REQB, 0x00, 0x00, 0x71, 0xFF }; // REQB
    // ... and HLTB
//  static const uint8_t cmdHLTB[] = { 0x50, 0xff, 0xff, 0xff, 0xff }; // HLTB
    // ... and ATTRIB
//    static const uint8_t cmdATTRIB[] = { ISO14443B_ATTRIB, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}; // ATTRIB

    // ... if not PUPI/UID is supplied we always respond with ATQB, PUPI = 820de174, Application Data = 0x20381922,
    // supports only 106kBit/s in both directions, max frame size = 32Bytes,
    // supports ISO14443-4, FWI=8 (77ms), NAD supported, CID not supported:
    uint8_t respATQB[] = {
        0x50,
        0x82, 0x0d, 0xe1, 0x74,
        0x20, 0x38, 0x19,
        0x22, 0x00, 0x21, 0x85,
        0x5e, 0xd7
    };

    // response to HLTB and ATTRIB
    static const uint8_t respOK[] = {0x00, 0x78, 0xF0};

    // ...PUPI/UID supplied from user. Adjust ATQB response accordingly
    if (pupi > 0) {
        num_to_bytes(pupi, 4, respATQB + 1);
        AddCrc14B(respATQB, 12);
    }

    // setup device.
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    // connect Demodulated Signal to ADC:
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    // Set up the synchronous serial port
    FpgaSetupSsc(FPGA_MAJOR_MODE_HF_SIMULATOR);

    // allocate command receive buffer
    BigBuf_free();
    BigBuf_Clear_ext(false);
    clear_trace();
    set_tracing(true);

    uint16_t len, cmdsReceived = 0;
    int cardSTATE = SIM_NOFIELD;
    int vHf = 0; // in mV

    tosend_t *ts = get_tosend();

    uint8_t *receivedCmd = BigBuf_malloc(MAX_FRAME_SIZE);

    // prepare "ATQB" tag answer (encoded):
    CodeIso14443bAsTag(respATQB, sizeof(respATQB));
    uint8_t *encodedATQB = BigBuf_malloc(ts->max);
    uint16_t encodedATQBLen = ts->max;
    memcpy(encodedATQB, ts->buf, ts->max);


    // prepare "OK" tag answer (encoded):
    CodeIso14443bAsTag(respOK, sizeof(respOK));
    uint8_t *encodedOK = BigBuf_malloc(ts->max);
    uint16_t encodedOKLen = ts->max;
    memcpy(encodedOK, ts->buf, ts->max);

    // Simulation loop
    while (BUTTON_PRESS() == false) {
        WDT_HIT();

        //iceman: limit with 2000 times..
        if (data_available()) {
            break;
        }

        // find reader field
        if (cardSTATE == SIM_NOFIELD) {

#if defined RDV4
            vHf = (MAX_ADC_HF_VOLTAGE_RDV40 * SumAdc(ADC_CHAN_HF_RDV40, 32)) >> 15;
#else
            vHf = (MAX_ADC_HF_VOLTAGE * SumAdc(ADC_CHAN_HF, 32)) >> 15;
#endif
            if (vHf > MF_MINFIELDV) {
                cardSTATE = SIM_IDLE;
                LED_A_ON();
            }
        }
        if (cardSTATE == SIM_NOFIELD) continue;

        // Get reader command
        if (!GetIso14443bCommandFromReader(receivedCmd, &len)) {
            Dbprintf("button pressed, received %d commands", cmdsReceived);
            break;
        }

        // ISO14443-B protocol states:
        // REQ or WUP request in ANY state
        // WUP in HALTED state
        if (len == 5) {
            if ((receivedCmd[0] == ISO14443B_REQB && (receivedCmd[2] & 0x8) == 0x8 && cardSTATE == SIM_HALTED) ||
                    receivedCmd[0] == ISO14443B_REQB) {
                LogTrace(receivedCmd, len, 0, 0, NULL, true);
                cardSTATE = SIM_SELECTING;
            }
        }

        /*
        * How should this flow go?
        *  REQB or WUPB
        *   send response  ( waiting for Attrib)
        *  ATTRIB
        *   send response  ( waiting for commands 7816)
        *  HALT
            send halt response ( waiting for wupb )
        */

        switch (cardSTATE) {
            //case SIM_NOFIELD:
            case SIM_HALTED:
            case SIM_IDLE: {
                LogTrace(receivedCmd, len, 0, 0, NULL, true);
                break;
            }
            case SIM_SELECTING: {
                TransmitFor14443b_AsTag(encodedATQB, encodedATQBLen);
                LogTrace(respATQB, sizeof(respATQB), 0, 0, NULL, false);
                cardSTATE = SIM_WORK;
                break;
            }
            case SIM_HALTING: {
                TransmitFor14443b_AsTag(encodedOK, encodedOKLen);
                LogTrace(respOK, sizeof(respOK), 0, 0, NULL, false);
                cardSTATE = SIM_HALTED;
                break;
            }
            case SIM_ACKNOWLEDGE: {
                TransmitFor14443b_AsTag(encodedOK, encodedOKLen);
                LogTrace(respOK, sizeof(respOK), 0, 0, NULL, false);
                cardSTATE = SIM_IDLE;
                break;
            }
            case SIM_WORK: {
                if (len == 7 && receivedCmd[0] == ISO14443B_HALT) {
                    cardSTATE = SIM_HALTED;
                } else if (len == 11 && receivedCmd[0] == ISO14443B_ATTRIB) {
                    cardSTATE = SIM_ACKNOWLEDGE;
                } else {
                    // Todo:
                    // - SLOT MARKER
                    // - ISO7816
                    // - emulate with a memory dump
                    if (DBGLEVEL >= DBG_DEBUG)
                        Dbprintf("new cmd from reader: len=%d, cmdsRecvd=%d", len, cmdsReceived);

                    // CRC Check
                    if (len >= 3) { // if crc exists

                        if (!check_crc(CRC_14443_B, receivedCmd, len)) {
                            if (DBGLEVEL >= DBG_DEBUG) {
                                DbpString("CRC fail");
                            }
                        }
                    } else {
                        if (DBGLEVEL >= DBG_DEBUG) {
                            DbpString("CRC passed");
                        }
                    }
                    cardSTATE = SIM_IDLE;
                }
                break;
            }
            default:
                break;
        }

        ++cmdsReceived;
    }

    if (DBGLEVEL >= DBG_DEBUG)
        Dbprintf("Emulator stopped. Trace length: %d ", BigBuf_get_traceLen());

    switch_off(); //simulate
}

//=============================================================================
// An ISO 14443 Type B reader. We take layer two commands, code them
// appropriately, and then send them to the tag. We then listen for the
// tag's response, which we leave in the buffer to be demodulated on the
// PC side.
//=============================================================================

/*
 * Handles reception of a bit from the tag
 *
 * This function is called 2 times per bit (every 4 subcarrier cycles).
 * Subcarrier frequency fs is 848kHz, 1/fs = 1,18us, i.e. function is called every 4,72us
 *
 * LED handling:
 * LED C -> ON once we have received the SOF and are expecting the rest.
 * LED C -> OFF once we have received EOF or are unsynced
 *
 * Returns: true if we received a EOF
 *          false if we are still waiting for some more
 *
 */
static RAMFUNC int Handle14443bSamplesFromTag(int ci, int cq) {

    int v;

// The soft decision on the bit uses an estimate of just the
// quadrant of the reference angle, not the exact angle.
#define MAKE_SOFT_DECISION() { \
		if(Demod.sumI > 0) { \
			v = ci; \
		} else { \
			v = -ci; \
		} \
		if(Demod.sumQ > 0) { \
			v += cq; \
		} else { \
			v -= cq; \
		} \
	}

#define SUBCARRIER_DETECT_THRESHOLD	8
// Subcarrier amplitude v = sqrt(ci^2 + cq^2), approximated here by max(abs(ci),abs(cq)) + 1/2*min(abs(ci),abs(cq)))
#define AMPLITUDE(ci,cq) (MAX(ABS(ci),ABS(cq)) + (MIN(ABS(ci),ABS(cq))/2))

    switch (Demod.state) {

        case DEMOD_UNSYNCD: {
            if (AMPLITUDE(ci, cq) > SUBCARRIER_DETECT_THRESHOLD) {	// subcarrier detected
                Demod.state = DEMOD_PHASE_REF_TRAINING;
                Demod.sumI = ci;
                Demod.sumQ = cq;
                Demod.posCount = 1;
            }
            break;
        }
        case DEMOD_PHASE_REF_TRAINING: {
            if (Demod.posCount < 8) {
                if (AMPLITUDE(ci, cq) > SUBCARRIER_DETECT_THRESHOLD) {
                    // set the reference phase (will code a logic '1') by averaging over 32 1/fs.
                    // note: synchronization time > 80 1/fs
                    Demod.sumI += ci;
                    Demod.sumQ += cq;
                    Demod.posCount++;
                } else {
                    // subcarrier lost
                    Demod.state = DEMOD_UNSYNCD;
                }
            } else {
                Demod.state = DEMOD_AWAITING_FALLING_EDGE_OF_SOF;
            }
            break;
        }
        case DEMOD_AWAITING_FALLING_EDGE_OF_SOF: {

            MAKE_SOFT_DECISION();

            if (v < 0) {	// logic '0' detected
                Demod.state = DEMOD_GOT_FALLING_EDGE_OF_SOF;
                Demod.posCount = 0;	// start of SOF sequence
            } else {
                if (Demod.posCount > 200 / 4) {	// maximum length of TR1 = 200 1/fs
                    Demod.state = DEMOD_UNSYNCD;
                }
            }
            Demod.posCount++;
            break;
        }
        case DEMOD_GOT_FALLING_EDGE_OF_SOF: {

            Demod.posCount++;
            MAKE_SOFT_DECISION();

            if (v > 0) {
                if (Demod.posCount < 9 * 2) { // low phase of SOF too short (< 9 etu). Note: spec is >= 10, but FPGA tends to "smear" edges
                    Demod.state = DEMOD_UNSYNCD;
                } else {
                    LED_C_ON(); // Got SOF
                    Demod.posCount = 0;
                    Demod.bitCount = 0;
                    Demod.len = 0;
                    Demod.state = DEMOD_AWAITING_START_BIT;
                }
            } else {
                if (Demod.posCount > 14 * 2) { // low phase of SOF too long (> 12 etu)
                    Demod.state = DEMOD_UNSYNCD;
                    LED_C_OFF();
                }
            }
            break;
        }
        case DEMOD_AWAITING_START_BIT: {
            Demod.posCount++;
            MAKE_SOFT_DECISION();
            if (v > 0) {
                if (Demod.posCount > 6 * 2) { 		// max 19us between characters = 16 1/fs, max 3 etu after low phase of SOF = 24 1/fs
                    LED_C_OFF();
                    if (Demod.bitCount == 0 && Demod.len == 0) { // received SOF only, this is valid for iClass/Picopass
                        return true;
                    } else {
                        Demod.state = DEMOD_UNSYNCD;
                    }
                }
            } else {							// start bit detected
                Demod.posCount = 1;				// this was the first half
                Demod.thisBit = v;
                Demod.shiftReg = 0;
                Demod.state = DEMOD_RECEIVING_DATA;
            }
            break;
        }
        case DEMOD_RECEIVING_DATA: {

            MAKE_SOFT_DECISION();

            if (Demod.posCount == 0) { 			// first half of bit
                Demod.thisBit = v;
                Demod.posCount = 1;
            } else {							// second half of bit
                Demod.thisBit += v;

                Demod.shiftReg >>= 1;
                if (Demod.thisBit > 0) {	// logic '1'
                    Demod.shiftReg |= 0x200;
                }

                Demod.bitCount++;
                if (Demod.bitCount == 10) {

                    uint16_t s = Demod.shiftReg;

                    if ((s & 0x200) && !(s & 0x001)) { // stop bit == '1', start bit == '0'
                        Demod.output[Demod.len] = (s >> 1);
                        Demod.len++;
                        Demod.bitCount = 0;
                        Demod.state = DEMOD_AWAITING_START_BIT;
                    } else {
                        Demod.state = DEMOD_UNSYNCD;
                        LED_C_OFF();
                        if (s == 0x000) {
                            // This is EOF (start, stop and all data bits == '0'
                            return true;
                        }
                    }
                }
                Demod.posCount = 0;
            }
            break;
        }
        default: {
            Demod.state = DEMOD_UNSYNCD;
            LED_C_OFF();
            break;
        }
    }
    return false;
}


/*
 *  Demodulate the samples we received from the tag, also log to tracebuffer
 */
static int Get14443bAnswerFromTag(uint8_t *response, uint16_t max_len, int timeout, uint32_t *eof_time) {

    int samples = 0, ret = 0;

    // Set up the demodulator for tag -> reader responses.
    Demod14bInit(response, max_len);

    // wait for last transfer to complete
    while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXEMPTY)) {};

    // And put the FPGA in the appropriate mode
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_SUBCARRIER_848_KHZ | FPGA_HF_READER_MODE_RECEIVE_IQ);

    // Setup and start DMA.
    FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);

    // The DMA buffer, used to stream samples from the FPGA
    dmabuf16_t *dma = get_dma16();
    if (FpgaSetupSscDma((uint8_t *) dma->buf, DMA_BUFFER_SIZE) == false) {
        if (DBGLEVEL > DBG_ERROR) Dbprintf("FpgaSetupSscDma failed. Exiting");
        return -1;
    }

    uint32_t dma_start_time = 0;
    uint16_t *upTo = dma->buf;

    for (;;) {

        volatile uint16_t behindBy = ((uint16_t *)AT91C_BASE_PDC_SSC->PDC_RPR - upTo) & (DMA_BUFFER_SIZE - 1);
        if (behindBy == 0)
            continue;

        samples++;

        if (samples == 1) {
            // DMA has transferred the very first data
            dma_start_time = GetCountSspClk() & 0xfffffff0;
        }

        volatile int8_t ci = *upTo >> 8;
        volatile int8_t cq = *upTo;
        upTo++;

        // we have read all of the DMA buffer content.
        if (upTo >= dma->buf + DMA_BUFFER_SIZE) {

            // start reading the circular buffer from the beginning again
            upTo = dma->buf;

            // DMA Counter Register had reached 0, already rotated.
            if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_ENDRX)) {

                // primary buffer was stopped
                if (AT91C_BASE_PDC_SSC->PDC_RCR == false) {
                    AT91C_BASE_PDC_SSC->PDC_RPR = (uint32_t) dma->buf;
                    AT91C_BASE_PDC_SSC->PDC_RCR = DMA_BUFFER_SIZE;
                }
                // secondary buffer sets as primary, secondary buffer was stopped
                if (AT91C_BASE_PDC_SSC->PDC_RNCR == false) {
                    AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dma->buf;
                    AT91C_BASE_PDC_SSC->PDC_RNCR = DMA_BUFFER_SIZE;
                }

                WDT_HIT();
                if (BUTTON_PRESS()) {
                    DbpString("stopped");
                    break;
                }
            }
        }

        if (Handle14443bSamplesFromTag(ci, cq)) {

            *eof_time = dma_start_time + (samples * 16) - DELAY_TAG_TO_ARM; // end of EOF

            if (Demod.len > Demod.max_len) {
                ret = -2; // overflow
            }
            break;
        }

        if (samples > timeout && Demod.state < DEMOD_PHASE_REF_TRAINING) {
            ret = -1;
            break;
        }
    }

    FpgaDisableSscDma();

    if (ret < 0) {
        return ret;
    }

    if (Demod.len > 0) {
        uint32_t sof_time = *eof_time
                            - (Demod.len * 8 * 8 * 16) // time for byte transfers
                            - (32 * 16)  // time for SOF transfer
                            - 0; // time for EOF transfer
        LogTrace(Demod.output, Demod.len, (sof_time * 4), (*eof_time * 4), NULL, false);
    }

    return Demod.len;
}

//-----------------------------------------------------------------------------
// Transmit the command (to the tag) that was placed in ToSend[].
//-----------------------------------------------------------------------------
static void TransmitFor14443b_AsReader(uint32_t *start_time) {

    tosend_t *ts = get_tosend();

    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_MODE_SEND_SHALLOW_MOD);

    if (*start_time < DELAY_ARM_TO_TAG) {
        *start_time = DELAY_ARM_TO_TAG;
    }

    *start_time = (*start_time - DELAY_ARM_TO_TAG) & 0xfffffff0;

    if (GetCountSspClk() > *start_time) { // we may miss the intended time
        *start_time = (GetCountSspClk() + 16) & 0xfffffff0; // next possible time
    }

    // wait
    while (GetCountSspClk() < *start_time) ;

    LED_B_ON();
    for (int c = 0; c < ts->max; c++) {
        volatile uint8_t data = ts->buf[c];

        for (int i = 0; i < 8; i++) {
            uint16_t send_word = (data & 0x80) ? 0x0000 : 0xffff;

            while (!(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY))) ;

            AT91C_BASE_SSC->SSC_THR = send_word;

            while (!(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY))) ;
            AT91C_BASE_SSC->SSC_THR = send_word;

            data <<= 1;
        }
        WDT_HIT();
    }
    LED_B_OFF();

    *start_time += DELAY_ARM_TO_TAG;
}

//-----------------------------------------------------------------------------
// Code a layer 2 command (string of octets, including CRC) into ToSend[],
// so that it is ready to transmit to the tag using TransmitFor14443b().
//-----------------------------------------------------------------------------
static void CodeIso14443bAsReader(const uint8_t *cmd, int len) {
    /*
    *  Reader data transmission:
    *   - no modulation ONES
    *   - SOF
    *   - Command, data and CRC_B
    *   - EOF
    *   - no modulation ONES
    *
    *   1 ETU == 1 BIT!
    *   TR0 - 8 ETUS minimum.
    *
    *   QUESTION:  how long is a 1 or 0 in pulses in the xcorr_848 mode?
    *              1 "stuffbit" = 1ETU (9us)
    */

    tosend_reset();

    // Send SOF
    // 10-11 ETUs of ZERO
    for (int i = 0; i < 10; i++)
        tosend_stuffbit(0);


    // 2-3 ETUs of ONE
    tosend_stuffbit(1);
    tosend_stuffbit(1);

    // Sending cmd, LSB
    // from here we add BITS
    for (int i = 0; i < len; i++) {
        // Start bit
        tosend_stuffbit(0);
        // Data bits
        uint8_t b = cmd[i];

        tosend_stuffbit(b & 1);
        tosend_stuffbit((b >> 1) & 1);
        tosend_stuffbit((b >> 2) & 1);
        tosend_stuffbit((b >> 3) & 1);
        tosend_stuffbit((b >> 4) & 1);
        tosend_stuffbit((b >> 5) & 1);
        tosend_stuffbit((b >> 6) & 1);
        tosend_stuffbit((b >> 7) & 1);

        // Stop bit
        tosend_stuffbit(1);
        // EGT extra guard time
        // For PCD it ranges 0-57us (1etu = 9us)
//        tosend_stuffbit(1);
//        tosend_stuffbit(1);
//        tosend_stuffbit(1);
    }

    // Send EOF
    // 10-11 ETUs of ZERO
    for (int i = 0; i < 10; i++)
        tosend_stuffbit(0);

    // Transition time. TR0 - guard time
    // 8ETUS minum?
    // Per specification, Subcarrier must be stopped no later than 2 ETUs after EOF.
    // I'm guessing this is for the FPGA to be able to send all bits before we switch to listening mode

    // ensure that last byte is filled up
    for (int i = 0; i < 8 ; ++i)
        tosend_stuffbit(1);

    // TR1 - Synchronization time
    // Convert from last character reference to length
    tosend_t *ts = get_tosend();
    ts->max++;
}

/*
*  Convenience function to encode, transmit and trace iso 14443b comms
*/
static void CodeAndTransmit14443bAsReader(const uint8_t *cmd, int len, uint32_t *start_time, uint32_t *eof_time) {
    tosend_t *ts = get_tosend();
    CodeIso14443bAsReader(cmd, len);
    TransmitFor14443b_AsReader(start_time);
    *eof_time = *start_time + (32 * (8 * ts->max));
    LogTrace(cmd, len, *start_time, *eof_time, NULL, true);
}

/* Sends an APDU to the tag
 * TODO: check CRC and preamble
 */
uint8_t iso14443b_apdu(uint8_t const *message, size_t message_length, uint8_t *response, uint16_t respmaxlen) {

    LED_A_ON();
    uint8_t message_frame[message_length + 4];
    // PCB
    message_frame[0] = 0x0A | pcb_blocknum;
    pcb_blocknum ^= 1;
    // CID
    message_frame[1] = 0;
    // INF
    memcpy(message_frame + 2, message, message_length);
    // EDC (CRC)
    AddCrc14B(message_frame, message_length + 2);

    // send
    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    CodeAndTransmit14443bAsReader(message_frame, sizeof(message_frame), &start_time, &eof_time);

    // get response
    if (response == NULL)  {
        LED_A_OFF();
        return 0;
    }

    eof_time += DELAY_ISO14443B_VCD_TO_VICC_READER;
    int retlen = Get14443bAnswerFromTag(response, respmaxlen, ISO14443B_READER_TIMEOUT, &eof_time);
    FpgaDisableTracing();

    if (retlen < 3) {
        LED_A_OFF();
        return 0;
    }

    // VALIDATE CRC
    if (!check_crc(CRC_14443_B, response, retlen)) {
        if (DBGLEVEL > DBG_DEBUG) DbpString("CRC fail");
        return 0;
    }

    return retlen;
}

/**
* SRx Initialise.
*/
static uint8_t iso14443b_select_srx_card(iso14b_card_select_t *card) {
    // INITIATE command: wake up the tag using the INITIATE
    static const uint8_t init_srx[] = { ISO14443B_INITIATE, 0x00, 0x97, 0x5b };
    uint8_t r_init[3] = {0x0};
    uint8_t r_select[3] = {0x0};
    uint8_t r_papid[10] = {0x0};

    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    CodeAndTransmit14443bAsReader(init_srx, sizeof(init_srx), &start_time, &eof_time);

    eof_time += DELAY_ISO14443B_VCD_TO_VICC_READER;
    int retlen = Get14443bAnswerFromTag(r_init, sizeof(r_init), ISO14443B_READER_TIMEOUT, &eof_time);
    FpgaDisableTracing();

    if (retlen <= 0)
        return 2;

    // Randomly generated Chip ID
    if (card) {
        card->chipid = Demod.output[0];
    }

    // SELECT command (with space for CRC)
    uint8_t select_srx[] = { ISO14443B_SELECT, 0x00, 0x00, 0x00};
    select_srx[1] = r_init[0];

    AddCrc14B(select_srx, 2);

    start_time = eof_time + DELAY_ISO14443B_VICC_TO_VCD_READER;
    CodeAndTransmit14443bAsReader(select_srx, sizeof(select_srx), &start_time, &eof_time);

    eof_time += DELAY_ISO14443B_VCD_TO_VICC_READER;
    retlen = Get14443bAnswerFromTag(r_select, sizeof(r_select), ISO14443B_READER_TIMEOUT, &eof_time);
    FpgaDisableTracing();

    if (retlen != 3) {
        return 2;
    }

    // Check the CRC of the answer:
    if (!check_crc(CRC_14443_B, r_select, retlen)) {
        return 3;
    }

    // Check response from the tag: should be the same UID as the command we just sent:
    if (select_srx[1] != r_select[0]) {
        return 1;
    }

    // First get the tag's UID:
    select_srx[0] = ISO14443B_GET_UID;

    AddCrc14B(select_srx, 1);

    start_time = eof_time + DELAY_ISO14443B_VICC_TO_VCD_READER;
    CodeAndTransmit14443bAsReader(select_srx, 3, &start_time, &eof_time); // Only first three bytes for this one

    eof_time += DELAY_ISO14443B_VCD_TO_VICC_READER;
    retlen = Get14443bAnswerFromTag(r_papid, sizeof(r_papid), ISO14443B_READER_TIMEOUT, &eof_time);
    FpgaDisableTracing();

    if (retlen != 10) {
        return 2;
    }

    // The check the CRC of the answer
    if (!check_crc(CRC_14443_B, r_papid, retlen)) {
        return 3;
    }

    if (card) {
        card->uidlen = 8;
        memcpy(card->uid, r_papid, 8);
    }

    return 0;
}
/* Perform the ISO 14443 B Card Selection procedure
 * Currently does NOT do any collision handling.
 * It expects 0-1 cards in the device's range.
 * TODO: Support multiple cards (perform anticollision)
 * TODO: Verify CRC checksums
 */
int iso14443b_select_card(iso14b_card_select_t *card) {
    // WUPB command (including CRC)
    // Note: WUPB wakes up all tags, REQB doesn't wake up tags in HALT state
    static const uint8_t wupb[] = { ISO14443B_REQB, 0x00, 0x08, 0x39, 0x73 };
    // ATTRIB command (with space for CRC)
    uint8_t attrib[] = { ISO14443B_ATTRIB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00};

    uint8_t r_pupid[14] = {0x0};
    uint8_t r_attrib[3] = {0x0};

    // first, wake up the tag
    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    CodeAndTransmit14443bAsReader(wupb, sizeof(wupb), &start_time, &eof_time);

    eof_time += DELAY_ISO14443B_VCD_TO_VICC_READER;;
    int retlen = Get14443bAnswerFromTag(r_pupid, sizeof(r_pupid), ISO14443B_READER_TIMEOUT, &eof_time);
    FpgaDisableTracing();

    // ATQB too short?
    if (retlen < 14) {
        return -1;
    }

    // VALIDATE CRC
    if (!check_crc(CRC_14443_B, r_pupid, retlen)) {
        return -2;
    }

    if (card) {
        card->uidlen = 4;
        memcpy(card->uid, r_pupid + 1, 4);
        memcpy(card->atqb, r_pupid + 5, 7);
    }

    // copy the PUPI to ATTRIB  ( PUPI == UID )
    memcpy(attrib + 1, r_pupid + 1, 4);

    // copy the protocol info from ATQB (Protocol Info -> Protocol_Type) into ATTRIB (Param 3)
    attrib[7] = r_pupid[10] & 0x0F;
    AddCrc14B(attrib, 9);
    start_time = eof_time + DELAY_ISO14443B_VICC_TO_VCD_READER;
    CodeAndTransmit14443bAsReader(attrib, sizeof(attrib), &start_time, &eof_time);

    eof_time += DELAY_ISO14443B_VCD_TO_VICC_READER;
    retlen = Get14443bAnswerFromTag(r_attrib, sizeof(r_attrib), ISO14443B_READER_TIMEOUT, &eof_time);
    FpgaDisableTracing();

    // Answer to ATTRIB too short?
    if (retlen < 3) {
        return -1;
    }

    // VALIDATE CRC
    if (!check_crc(CRC_14443_B, r_attrib, retlen)) {
        return -2;
    }

    if (card) {

        // CID
        card->cid = r_attrib[0];

        // MAX FRAME
        uint16_t maxFrame = card->atqb[5] >> 4;
        if (maxFrame < 5)       maxFrame = 8 * maxFrame + 16;
        else if (maxFrame == 5) maxFrame = 64;
        else if (maxFrame == 6) maxFrame = 96;
        else if (maxFrame == 7) maxFrame = 128;
        else if (maxFrame == 8) maxFrame = 256;
        else maxFrame = 257;
        iso14b_set_maxframesize(maxFrame);

        // FWT
        uint8_t fwt = card->atqb[6] >> 4;
        if (fwt < 16) {
            uint32_t fwt_time = (302 << fwt);
            iso14b_set_timeout(fwt_time);
        }
    }
    // reset PCB block number
    pcb_blocknum = 0;
    return 0;
}

// Set up ISO 14443 Type B communication (similar to iso14443a_setup)
// field is setup for "Sending as Reader"
void iso14443b_setup(void) {
    LEDsoff();
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    // allocate command receive buffer
    BigBuf_free();
    BigBuf_Clear_ext(false);

    // Initialize Demod and Uart structs
    Demod14bInit(BigBuf_malloc(MAX_FRAME_SIZE), MAX_FRAME_SIZE);
    Uart14bInit(BigBuf_malloc(MAX_FRAME_SIZE));

    // connect Demodulated Signal to ADC:
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    // Set up the synchronous serial port
    FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);

    // Signal field is on with the appropriate LED
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_MODE_SEND_SHALLOW_MOD);
    SpinDelay(100);

    // Start the timer
    StartCountSspClk();

    LED_D_ON();
}

//-----------------------------------------------------------------------------
// Read a SRI512 ISO 14443B tag.
//
// SRI512 tags are just simple memory tags, here we're looking at making a dump
// of the contents of the memory. No anticollision algorithm is done, we assume
// we have a single tag in the field.
//
// I tried to be systematic and check every answer of the tag, every CRC, etc...
//-----------------------------------------------------------------------------
static bool ReadSTBlock(uint8_t blocknr, uint8_t *block) {
    uint8_t cmd[] = {ISO14443B_READ_BLK, blocknr, 0x00, 0x00};
    AddCrc14B(cmd, 2);

    uint8_t r_block[6] = {0};

    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    CodeAndTransmit14443bAsReader(cmd, sizeof(cmd), &start_time, &eof_time);

    eof_time += DELAY_ISO14443B_VCD_TO_VICC_READER;
    int retlen = Get14443bAnswerFromTag(r_block, sizeof(r_block), ISO14443B_READER_TIMEOUT, &eof_time);
    FpgaDisableTracing();

    // Check if we got an answer from the tag
    if (retlen != 6) {
        DbpString("[!] expected 6 bytes from tag, got less...");
        return false;
    }
    // The check the CRC of the answer
    if (!check_crc(CRC_14443_B, r_block, retlen)) {
        DbpString("CRC fail");
        return false;
    }

    if (block) {
        memcpy(block, r_block, 4);
    }

    Dbprintf("Address=%02x, Contents=%08x, CRC=%04x",
             blocknr,
             (r_block[3] << 24) + (r_block[2] << 16) + (r_block[1] << 8) + r_block[0],
             (r_block[4] << 8) + r_block[5]);

    return true;
}

void ReadSTMemoryIso14443b(uint16_t numofblocks) {

    iso14443b_setup();

    uint8_t *mem = BigBuf_malloc((numofblocks + 1) * 4);

    iso14b_card_select_t card;
    uint8_t res = iso14443b_select_srx_card(&card);

    int isOK = PM3_SUCCESS;
    // 0: OK 2: attrib fail, 3:crc fail,
    if (res > 0) {
        isOK = PM3_ETIMEOUT;
        goto out;
    }

    ++numofblocks;

    for (uint8_t i = 0; i < numofblocks; i++) {

        if (ReadSTBlock(i, mem + (i * 4)) == false) {
            isOK = PM3_ETIMEOUT;
            break;
        }
    }

    // System area block (0xFF)
    if (ReadSTBlock(0xFF, mem + (numofblocks * 4)) == false)
        isOK = PM3_ETIMEOUT;

out:

    reply_ng(CMD_HF_SRI_READ, isOK, mem, numofblocks * 4);

    BigBuf_free();
    switch_off();
}

//=============================================================================
// Finally, the `sniffer' combines elements from both the reader and
// simulated tag, to show both sides of the conversation.
//=============================================================================

//-----------------------------------------------------------------------------
// Record the sequence of commands sent by the reader to the tag, with
// triggering so that we start recording at the point that the tag is moved
// near the reader.
//-----------------------------------------------------------------------------
/*
 * Memory usage for this function, (within BigBuf)
 * Last Received command (reader->tag) - MAX_FRAME_SIZE
 * Last Received command (tag->reader) - MAX_FRAME_SIZE
 * DMA Buffer - ISO14443B_DMA_BUFFER_SIZE
 * Demodulated samples received - all the rest
 */
void SniffIso14443b(void) {

    LEDsoff();
    LED_A_ON();

    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    DbpString("Starting to sniff. Press PM3 Button to stop.");

    BigBuf_free();
    clear_trace();
    set_tracing(true);

    // Initialize Demod and Uart structs
    uint8_t dm_buf[MAX_FRAME_SIZE] = {0};
    Demod14bInit(dm_buf, sizeof(dm_buf));

    uint8_t ua_buf[MAX_FRAME_SIZE] = {0};
    Uart14bInit(ua_buf);

    //Demod14bInit(BigBuf_malloc(MAX_FRAME_SIZE), MAX_FRAME_SIZE);
    //Uart14bInit(BigBuf_malloc(MAX_FRAME_SIZE));

    // Set FPGA in the appropriate mode
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_SUBCARRIER_848_KHZ | FPGA_HF_READER_MODE_SNIFF_IQ);
//    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_SUBCARRIER_848_KHZ | FPGA_HF_READER_MODE_SNIFF_AMPLITUDE);

    // connect Demodulated Signal to ADC:
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
    FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);

    StartCountSspClk();

    // The DMA buffer, used to stream samples from the FPGA
    dmabuf16_t *dma = get_dma16();

    // Setup and start DMA.
    if (!FpgaSetupSscDma((uint8_t *) dma->buf, DMA_BUFFER_SIZE)) {
        if (DBGLEVEL > DBG_ERROR) DbpString("FpgaSetupSscDma failed. Exiting");
        switch_off();
        return;
    }

    // We won't start recording the frames that we acquire until we trigger;
    // a good trigger condition to get started is probably when we see a
    // response from the tag.
    bool tag_is_active = false;
    bool reader_is_active = false;
    bool expect_tag_answer = false;
    int dma_start_time = 0;

    // Count of samples received so far, so that we can include timing
    int samples = 0;

    uint16_t *upTo = dma->buf;

    for (;;) {

        volatile int behind_by = ((uint16_t *)AT91C_BASE_PDC_SSC->PDC_RPR - upTo) & (DMA_BUFFER_SIZE - 1);
        if (behind_by < 1) continue;

        samples++;
        if (samples == 1) {
            // DMA has transferred the very first data
            dma_start_time = GetCountSspClk() & 0xfffffff0;
        }

        volatile int8_t ci = *upTo >> 8;
        volatile int8_t cq = *upTo;
        upTo++;

        // we have read all of the DMA buffer content.
        if (upTo >= dma->buf + DMA_BUFFER_SIZE) {

            // start reading the circular buffer from the beginning again
            upTo = dma->buf;

            // DMA Counter Register had reached 0, already rotated.
            if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_ENDRX)) {

                // primary buffer was stopped
                if (AT91C_BASE_PDC_SSC->PDC_RCR == false) {
                    AT91C_BASE_PDC_SSC->PDC_RPR = (uint32_t) dma->buf;
                    AT91C_BASE_PDC_SSC->PDC_RCR = DMA_BUFFER_SIZE;
                }
                // secondary buffer sets as primary, secondary buffer was stopped
                if (AT91C_BASE_PDC_SSC->PDC_RNCR == false) {
                    AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dma->buf;
                    AT91C_BASE_PDC_SSC->PDC_RNCR = DMA_BUFFER_SIZE;
                }

                WDT_HIT();
                if (BUTTON_PRESS()) {
                    DbpString("Sniff stopped");
                    break;
                }
            }
        }

        // no need to try decoding reader data if the tag is sending
        if (tag_is_active == false) {

            if (Handle14443bSampleFromReader(ci & 0x01)) {
                uint32_t eof_time = dma_start_time + (samples * 16) + 8; // - DELAY_READER_TO_ARM_SNIFF; // end of EOF
                if (Uart.byteCnt > 0) {
                    uint32_t sof_time = eof_time
                                        - Uart.byteCnt * 1 // time for byte transfers
                                        - 32 * 16          // time for SOF transfer
                                        - 16 * 16;         // time for EOF transfer
                    LogTrace(Uart.output, Uart.byteCnt, (sof_time * 4), (eof_time * 4), NULL, true);
                }
                // And ready to receive another command.
                Uart14bReset();
                Demod14bReset();
                reader_is_active = false;
                expect_tag_answer = true;
            }

            if (Handle14443bSampleFromReader(cq & 0x01)) {

                uint32_t eof_time = dma_start_time + (samples * 16) + 16; // - DELAY_READER_TO_ARM_SNIFF; // end of EOF
                if (Uart.byteCnt > 0) {
                    uint32_t sof_time = eof_time
                                        - Uart.byteCnt * 1 // time for byte transfers
                                        - 32 * 16          // time for SOF transfer
                                        - 16 * 16;         // time for EOF transfer
                    LogTrace(Uart.output, Uart.byteCnt, (sof_time * 4), (eof_time * 4), NULL, true);
                }
                // And ready to receive another command
                Uart14bReset();
                Demod14bReset();
                reader_is_active = false;
                expect_tag_answer = true;
            }

            reader_is_active = (Uart.state > STATE_14B_GOT_FALLING_EDGE_OF_SOF);
        }

        // no need to try decoding tag data if the reader is sending - and we cannot afford the time
        if (reader_is_active == false && expect_tag_answer) {

            if (Handle14443bSamplesFromTag((ci >> 1), (cq >> 1))) {

                uint32_t eof_time = dma_start_time + (samples * 16); // - DELAY_TAG_TO_ARM_SNIFF; // end of EOF
                uint32_t sof_time = eof_time
                                    - Demod.len * 8 * 8 * 16 // time for byte transfers
                                    - (32 * 16)             // time for SOF transfer
                                    - 0;                    // time for EOF transfer

                LogTrace(Demod.output, Demod.len, (sof_time * 4), (eof_time * 4), NULL, false);
                // And ready to receive another response.
                Uart14bReset();
                Demod14bReset();
                expect_tag_answer = false;
                tag_is_active = false;
            } else {
                tag_is_active = (Demod.state > DEMOD_GOT_FALLING_EDGE_OF_SOF);
            }
        }
    }

    FpgaDisableTracing();
    switch_off();

    DbpString("");
    DbpString(_CYAN_("Sniff statistics"));
    DbpString("=================================");
    Dbprintf("  DecodeTag State........%d", Demod.state);
    Dbprintf("  DecodeTag byteCnt......%d", Demod.len);
    Dbprintf("  DecodeTag posCount.....%d", Demod.posCount);
    Dbprintf("  DecodeReader State.....%d", Uart.state);
    Dbprintf("  DecodeReader byteCnt...%d", Uart.byteCnt);
    Dbprintf("  DecodeReader posCount..%d", Uart.posCnt);
    Dbprintf("  Trace length..........." _YELLOW_("%d"), BigBuf_get_traceLen());
    DbpString("");
}

static void iso14b_set_trigger(bool enable) {
    g_trigger = enable;
}

/*
 * Send raw command to tag ISO14443B
 * @Input
 * param   flags enum ISO14B_COMMAND.  (mifare.h)
 * len     len of buffer data
 * data    buffer with bytes to send
 *
 * @Output
 * none
 *
 */
void SendRawCommand14443B_Ex(PacketCommandNG *c) {

    iso14b_command_t param = c->oldarg[0];
    size_t len = c->oldarg[1] & 0xffff;
    uint32_t timeout = c->oldarg[2];
    uint8_t *cmd = c->data.asBytes;

    if (DBGLEVEL > DBG_DEBUG) Dbprintf("14b raw: param, %04x", param);

    // turn on trigger (LED_A)
    if ((param & ISO14B_REQUEST_TRIGGER) == ISO14B_REQUEST_TRIGGER)
        iso14b_set_trigger(true);

    if ((param & ISO14B_CONNECT) == ISO14B_CONNECT) {
        iso14443b_setup();
        clear_trace();
    }

    if ((param & ISO14B_SET_TIMEOUT))
        iso14b_set_timeout(timeout);

    set_tracing(true);

    int status;
    uint32_t sendlen = sizeof(iso14b_card_select_t);
    iso14b_card_select_t card;

    if ((param & ISO14B_SELECT_STD) == ISO14B_SELECT_STD) {
        status = iso14443b_select_card(&card);
        reply_mix(CMD_HF_ISO14443B_COMMAND, status, sendlen, 0, (uint8_t *)&card, sendlen);
        // 0: OK -1: attrib fail, -2:crc fail,
        if (status != 0) goto out;
    }

    if ((param & ISO14B_SELECT_SR) == ISO14B_SELECT_SR) {
        status = iso14443b_select_srx_card(&card);
        reply_mix(CMD_HF_ISO14443B_COMMAND, status, sendlen, 0, (uint8_t *)&card, sendlen);
        // 0: OK 2: demod fail, 3:crc fail,
        if (status > 0) goto out;
    }

    if ((param & ISO14B_APDU) == ISO14B_APDU) {
        uint8_t buf[100] = {0};
        status = iso14443b_apdu(cmd, len, buf, sizeof(buf));
        reply_mix(CMD_HF_ISO14443B_COMMAND, status, status, 0, buf, status);
    }

    if ((param & ISO14B_RAW) == ISO14B_RAW) {
        if ((param & ISO14B_APPEND_CRC) == ISO14B_APPEND_CRC) {
            AddCrc14B(cmd, len);
            len += 2;
        }
        uint8_t buf[100] = {0};

        uint32_t start_time = 0;
        uint32_t eof_time = 0;
        CodeAndTransmit14443bAsReader(cmd, len, &start_time, &eof_time);

        eof_time += DELAY_ISO14443B_VCD_TO_VICC_READER;
        status = Get14443bAnswerFromTag(buf, sizeof(buf), 5 * ISO14443B_READER_TIMEOUT, &eof_time); // raw
        FpgaDisableTracing();

        sendlen = MIN(Demod.len, PM3_CMD_DATA_SIZE);
        reply_mix(CMD_HF_ISO14443B_COMMAND, status, sendlen, 0, Demod.output, sendlen);
    }

out:
    // turn off trigger (LED_A)
    if ((param & ISO14B_REQUEST_TRIGGER) == ISO14B_REQUEST_TRIGGER)
        iso14b_set_trigger(false);

    // turn off antenna et al
    // we don't send a HALT command.
    if ((param & ISO14B_DISCONNECT) == ISO14B_DISCONNECT) {
        switch_off(); // disconnect raw
        SpinDelay(20);
    }
}

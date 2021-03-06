/* MSCAN module functions */

#include <hidef.h>  // For EnableInterrupts and DisableInterrupts macros
#include <mc9s12c32.h>
#include "mscan.h"

static byte volatile rxbuffer[PAYLOAD_SIZE] = {0};  // Array filled by receiver interrupt
static byte volatile data_available_flag = 0;


/*
 * Calculating CAN bus bit rate:
 *  b = f/KN
 * where b = bit rate
 *       f = CAN clock soruce frequency
 *       K = prescaler value
 *       N = number of quanta per bit time (1 + time_seg1 + time_seg2)
 */

/* Initialize CAN bus */
/* Only call this once during startup! */
void CANinit(word id) {
    CANCTL1_CANE = 1;   // Enable the MSCAN module (write once)
    
    CANCTL0_INITRQ = 1;     // Request to enter init mode
    while(!CANCTL1_INITAK); // Wait for ack bit to be set
    
    // Setup for 1Mbit/s bit rate
    SET_MSCAN_CLK_SRC(MSCAN_CLK_SRC_BUS);   // Use bus clock (8MHz) for MSCAN clock source
    SET_MSCAN_PRESCALE(MSCAN_PRESCALE_1);   // Set baud rate prescaler value to 1
    
    /* NOTE: Number of time quantas (Tq's) allowed one bit time is 8-25
     *  Time segment 1 can be 4-16 Tq's
     *  Time segment 2 can be 2-8 Tq's
     *  1 + time_seg1 + time_seg2 = 1 bit time
     */
    SET_MSCAN_TIME_SEG1(MSCAN_TIME_SEG1_4); // Use a bit time of 8 time quanta (1+4+3)
    SET_MSCAN_TIME_SEG2(MSCAN_TIME_SEG2_3);
    
    SET_MSCAN_JUMP_WIDTH(MSCAN_SJW_4TQ);    // Set sync jump width to the largest possible value (faster resync)
    SET_MSCAN_SAMPLE_BITS(MSCAN_SAMPLE_1);  // Sample once per bit (vs 3 times per bit)
    
    CANCTL0_TIME = 1;   // Add a 16-bit timestamp to each message
    CANCTL1_LISTEN = 0; // Cannot be in listen mode if we want to send messages
#ifdef USE_LOOPBACK
    CANCTL1_LOOPB = 1;  // Enable loopback for testing
#else
    CANCTL1_LOOPB = 0;  // Disable loopback for real CAN bus medium
#endif
    
    /** Start Filtering Setup **/
    /*  Registers are combined in pairs and addressed as one 16bit word
     *  Last three bits in mask must be set to "dont care" in 16bit filter mode (due to shifting of 11bit ID in registers)
     *  IDs must be shifted over to account for the RTR and IDE bits as well as the trailing three unused bits
     */
    
    MSCAN_ACC_MODE(MSCAN_ACC_4_16); // Use four 16bit acceptance filters (two banks with two levels per bank)
    
    // First filter bank
    //  First level of first bank
    *((word*)((word)(&CANIDMR0))) = (0x0007 | ~(id << 5));
    *((word*)((word)(&CANIDAR0))) = id << 5;
    /*
    //  Second level of first bank (not used)
    *((word*)((word)(&CANIDMR2))) = (0x0007 | ~(id << 5));
    *((word*)((word)(&CANIDAR2))) = id << 5;
    
    // Second filter bank (not used)
    //  First level of second bank
    *((word*)((word)(&CANIDMR4))) = (0x0007 | ~(id << 5));
    *((word*)((word)(&CANIDAR4))) = id << 5;
    
    //  Second level of second bank
    *((word*)((word)(&CANIDMR6))) = (0x0007 | ~(id << 5));
    *((word*)((word)(&CANIDAR6))) = id << 5;
    */
    /** End Filtering Setup **/
    
    CANCTL0_INITRQ = 0;     // Request to exit init mode before writing to the remaining registers
    while(CANCTL1_INITAK);  // Wait for ack bit to be unset
    
    
    CANRIER_RXFIE = 1;  // Enable MSCAN receiver interrupt when a new valid message is received
}

/* MSCAN transmit message */
/* non-zero return value indicates that it could not send a message */
byte CANsend(CANframe *frame) {
    byte txbuffer, i;
    
    // Check if all three tx buffers are already full (can't send message)
    // If all three bits in CANTFLG_TXE are unset, all three buffers are currently full
    if(CANTFLG_TXE == 0) return 1;
    
    CANTBSEL = CANTFLG;     // Select lowest tx buffer using tx buffer empty flags
    txbuffer = CANTBSEL;    // Save actual selected tx buffer to clear flag after message is constructed
    
    *((dword*)((dword)(&CANTXIDR0))) = (dword)frame->id << (5+16); // Load in ID of message as a 32bit dword (TXIDR0 - TXIDR3)
    
    // Truncate length to 0-8 bytes
    // Most CAN controllers will assume 8 bytes of data if message length value is >8
    if(frame->length > 8) frame->length = 8;
    
    // Copy payload data to data segment registers (memory mapped in sequential order)
    for(i=0; i<frame->length; i++)
        *(&CANTXDSR0 + i) = frame->payload[i];
    
    CANTXDLR = frame->length;
    CANTXTBPR = frame->priority;
    
    CANTFLG = txbuffer;     // Release tx buffer for transmission by clearing the associated flag
    while((CANTFLG & txbuffer) != txbuffer);    // Wait for transmission to complete
    
    return 0;
}

/* Fill a dataMessage struct with payload data */
void CANget(dataMessage *message) {
    byte i;
    
    DisableInterrupts;
    for(i=0; i<PAYLOAD_SIZE; i++) {
        *((byte*)message + i) = rxbuffer[i];
    }
    EnableInterrupts;
}

/* Return the data available flag */
byte data_available(void) {
    return data_available_flag;
}

/* Clear the data available flag */
void data_used(void) {
    DisableInterrupts;
    data_available_flag = 0;
    EnableInterrupts;
}

/*****************************************************************************/

/* MSCAN receiver interrupt */
// currently just writes data into a buffer and overwrites it each time there is a new message
interrupt VectorNumber_Vcanrx
void CANreceiveISR(void) {
    byte length, i;
    word timestamp;
    
    length = CANRXDLR_DLC;  // Length is 4 bits, max value of 8
    
    // Copy out payload data (data segment registers memory mapped in sequential order)
    for(i=0; i<length; i++)
        rxbuffer[i] = *(&CANRXDSR0 + i);
    
    timestamp = (CANTXTSRH << 8) | CANTXTSRL;
    
    data_available_flag = 1;
    CANRFLG = CANRFLG_RXF_MASK; // Clear RXF flag to release rx buffer
}
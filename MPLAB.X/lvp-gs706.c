/*******************************************************************************
Copyright 2016 Microchip Technology Inc. (www.microchip.com)

 Low Voltage Programming Interface

  Bit-Banged implementation of the dsPIC33EP128GS706 ICSP protocol

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

 *******************************************************************************/
#include "lvp.h"
#include "leds.h"
#include "uart.h"
#include <string.h>
#include <stdlib.h>

// device specific parameters (DS70005256A)
#define CFG_ADDRESS 0x015780    // address of config words area
#define DEV_ID      0xFF0000    // product ID
#define REV_ID      0xFF0002    // silicon revision ID
#define UID_ADDRESS 0x800F00    // address of UID words area

#define ROW_SIZE     64         // width of a flash row in words
#define CFG_NUM      12         // number of config words

#define WRITE_TIME   1       // mem write time ms
#define CFG_TIME     1       // cfg write time ms
#define BULK_TIME   30       // bulk erase time ms

/****************************************************************************/
// internal state
uint16_t row[ROW_SIZE]; // buffer containing row being formed
uint32_t row_address; // destination address of current row

// ICSP serial commands
#define SIX                 0
#define REGOUT              1

// ICSP commands (via the Programming Executive)
#define CMD_SCHECK          0x0000
#define CMD_READ_DATA       0x2000
#define CMD_PROG_2W         0x3000
#define CMD_PROG_PAGE       0x50C3
#define CMD_BULK_ERASE      0x7001      // BULK ERASE + length (1)

void ICSP_slaveReset(void) {
    ICSP_nMCLR = SLAVE_RESET;
    ICSP_TRIS_nMCLR = OUTPUT_PIN;
}

void ICSP_slaveRun(void) {
    ICSP_nMCLR = SLAVE_RUN;
    ICSP_TRIS_nMCLR = OUTPUT_PIN;
}

void ICSP_init(void) {
    RCSTAbits.SPEN = 0; // disable UART, control I/O
    __delay_us(1);
    ICSP_TRIS_DAT = INPUT_PIN;
    ICSP_CLK = 0;
    ICSP_TRIS_CLK = OUTPUT_PIN;
    ICSP_slaveRun();
}

void ICSP_release(void) {
    ICSP_TRIS_DAT = INPUT_PIN;
    ICSP_TRIS_CLK = INPUT_PIN;
    //    UART_Initialize();
    ICSP_slaveRun();
}

void ICSP_controlCode(uint8_t type) {
    ICSP_TRIS_DAT = OUTPUT_PIN;
    for (uint8_t i = 0; i < 4; i++) {
        if (type & 0x01)
            ICSP_DAT = 1;
        else
            ICSP_DAT = 0;
        __delay_us(1);
        ICSP_CLK = 1;
        type >>= 1;
        __delay_us(1);
        ICSP_CLK = 0;
    }
    __delay_us(1);
}

void ICSP_sendWord(uint16_t w) {
    ICSP_TRIS_DAT = OUTPUT_PIN;
    for (uint8_t i = 0; i < 16; i++) {
        if ((w & 0x8000) > 0) // Msb first
            ICSP_DAT = 1;
        else
            ICSP_DAT = 0;
        __delay_us(1); // > P1B (200ns)
        ICSP_CLK = 1; // rising edge latch
        w <<= 1;
        __delay_us(1); // > P1A (200ns)
        ICSP_CLK = 0;
    }
}

void ICSP_sendData24(uint24_t data) {
    uint8_t i;
    ICSP_TRIS_DAT = OUTPUT_PIN;
    for (i = 0; i < 24; i++) {
        if (data & 0x000001)
            ICSP_DAT = 1; // Lsb first
        else
            ICSP_DAT = 0; // Lsb first
        __delay_us(1);
        ICSP_CLK = 1;
        data >>= 1;
        __delay_us(1);
        ICSP_CLK = 0;
    }
    __delay_us(1);
}

void ICSP_Cmd(uint24_t data) {
    ICSP_controlCode(SIX);
    ICSP_sendData24(data);
}

void ICSP_signature(void) {
    ICSP_slaveReset();          // MCLR output => Vil (GND)
    __delay_ms(1);              // > P6 (100ns))
    ICSP_slaveRun();
    __delay_us(250);            // < P21 (500us) short pulse
    ICSP_slaveReset();
    __delay_ms(1);              // > P18 (1ms)
    ICSP_sendWord(0x4D43);
    ICSP_sendWord(0x4851);
    __delay_us(1);              // > P19 (20ns)
    ICSP_slaveRun();            // release MCLR
    // > P7(50ms) + P1 (500us)*5
    for(uint8_t i=0; i<55; i++) __delay_ms(1);

//    ICSP_Cmd(0);              // nop
    // add five clock cycles
    for (uint8_t i = 0; i < 5; i++) {
        ICSP_CLK = 1;
        __delay_us(1);
        ICSP_CLK = 0;
        __delay_us(1);
    }
}

void ICSP_exitResetVector(void) {
    ICSP_Cmd(0);          // nop
    ICSP_Cmd(0);          // nop
    ICSP_Cmd(0);          // nop
    ICSP_Cmd(0x040200);   // goto 200
    ICSP_Cmd(0);          // nop
    ICSP_Cmd(0);          // nop
    ICSP_Cmd(0);          // nop
}

void ICSP_unlockWR(void) {
    ICSP_Cmd(0x200551);   // mov  #55, W1
    ICSP_Cmd(0x883971);   // mov  W1, NVKEY
    ICSP_Cmd(0x200AA1);   // mov  #AA, W1
    ICSP_Cmd(0x883971);   // mov  W1, NVKEY
    ICSP_Cmd(0xA8E729);   // bset NVCOM, #WR
    ICSP_Cmd(0);          // nop
    ICSP_Cmd(0);          // nop
    ICSP_Cmd(0);          // nop]
}

uint16_t ICSP_getWord(void) {
    uint16_t w = 0;
    ICSP_TRIS_DAT = INPUT_PIN; // PGD input
    for (uint8_t i = 0; i < 16; i++) {
        ICSP_CLK = 1;
        w >>= 1; // shift right
        __delay_us(1);
        w |= (ICSP_DAT_IN ? 0x8000 : 0); // read port
        ICSP_CLK = 0;
        __delay_us(1);
    }
    return w;
}

uint16_t ICSP_readVISI(void) {
    ICSP_controlCode(REGOUT);
    for (uint8_t i = 0; i < 8; i++) {
        ICSP_CLK = 1;
        __delay_us(1);
        ICSP_CLK = 0;
        __delay_us(1);
    }
    return ICSP_getWord();
}

void ICSP_bulkErase(void) {
    ICSP_exitResetVector();
    ICSP_Cmd(0x2400EA); // mov  0x400E, W10
    ICSP_Cmd(0x88394A); // mov  W10, NVMCON
    ICSP_Cmd(0); // nop
    ICSP_Cmd(0); // nop
    ICSP_unlockWR();
    for(uint8_t i=0; i<BULK_TIME; i++) __delay_ms(1);
}

void ICSP_addressLoad(uint24_t address) {
    uint24_t destAddressHigh, destAddressLow;

    destAddressHigh = (address >> 16) & 0xff;
    destAddressLow = address & 0xffff;

    // Step 1: Exit the Reset vector.
    ICSP_exitResetVector();

    // Step 2: Set the TBLPG register for writing to the latches  (@FA0000)
    ICSP_Cmd(0x200FAC); // MOV #0xFA, W12
    ICSP_Cmd(0x883B0A); // MOV W12, TBLPG

    // Step 3: set NVMADR, NVMADRU to point to the destination
    ICSP_Cmd(0x200003 + (destAddressLow) << 4); // MOV #<DestinationAddress15:0>, W3
    ICSP_Cmd(0x200004 + (destAddressHigh) << 4); // MOV #<DestinationAddress23:16>, W4
    ICSP_Cmd(0x883953); // MOV W3, NVMADR
    ICSP_Cmd(0x883964); // MOV W4, NVMADRU

    // Step 4: Set the NVMCON to program 2 instruction words.
    ICSP_Cmd(0x24001A); // MOV #0x4001, W10
    ICSP_Cmd(0); // NOP
    ICSP_Cmd(0x883B0A); // MOV W10, NVMCON
    ICSP_Cmd(0); // NOP
    ICSP_Cmd(0); // NOP
}

void sendSIXMov(uint16_t lit, uint8_t reg) {
    uint24_t word = lit ;
    // format mov command as  op
    ICSP_Cmd(0x200000 + (word << 4) + reg);
}

void ICSP_rowWrite(uint16_t *buffer, uint8_t count) {
    uint16_t LSW0, MSB0;
    uint16_t LSW1, MSB1;
    uint16_t LSW2, MSB2;
    uint16_t LSW3, MSB3;

    // Step 5: init W7 to point to first latch
    ICSP_Cmd(0xEB0380); // CLR W7
    ICSP_Cmd(0x000000); // NOP

    for (uint8_t i = count / 4; i > 0; i--) // load 2 latches, 4 * 16-bit words
    {
        LSW0 = *buffer++;
        MSB0 = *buffer++;
        LSW1 = *buffer++;
        MSB1 = *buffer++;

        sendSIXMov(LSW0, 0); // MOV #<LSW0>, W0
        sendSIXMov(((MSB1 & 0xFF) << 8) + (MSB0 & 0xFF), 1); // MOV #<MSB1:MSB0>, W1
        sendSIXMov(LSW1, 2); // MOV #<LSW1>, W2

        ICSP_Cmd(0xEB0300); // CLR W6
        ICSP_Cmd(0); // NOP
        ICSP_Cmd(0xBB0BB6); // TBLWTL[W6++], [W7]
        ICSP_Cmd(0); // NOP
        ICSP_Cmd(0); // NOP
        ICSP_Cmd(0xBBDBB6); // TBLWTH.B[W6++], [W7++]
        ICSP_Cmd(0); // NOP
        ICSP_Cmd(0); // NOP
        ICSP_Cmd(0xBBEBB6); // TBLWTH.B[W6++], [++W7]
        ICSP_Cmd(0); // NOP
        ICSP_Cmd(0); // NOP
        ICSP_Cmd(0xBB1BB6); // TBLWTL[W6++], [W7++]
        ICSP_Cmd(0); // NOP
        ICSP_Cmd(0); // NOP
    }
    // Step 7. Initiate write cycle
    ICSP_unlockWR();
    __delay_ms(WRITE_TIME);
}

//void ICSP_cfgWrite(uint16_t *buffer, uint8_t count)
//{
//    uint16_t LSW0, MSB0;
//    uint16_t LSW1, MSB1;
//
//    while( count > 0)
//    {
//        LSW0 = *buffer++;       MSB0 = *buffer++;
//        LSW1 = *buffer++;       MSB1 = *buffer++;
//
//        sendSIXMov(LSW0, 0);    // MOV #<LSW0>, W0
//        sendSIXMov( ((MSB1 & 0xFF) << 8) + (MSB0 & 0xFF), 1); // MOV #<MSB1:MSB0>, W1
//        sendSIXMov(LSW1, 2);    // MOV #<LSW1>, W2
//
//        ICSP_Cmd(0xEB0300);   // CLR W6
//        ICSP_Cmd(0);          // NOP
//        ICSP_Cmd(0xBB0B00);   // TBLWTL W0, [W6]
//        ICSP_Cmd(0);          // NOP
//        ICSP_Cmd(0);          // NOP
//        ICSP_Cmd(0xBB9B01);   // TBLWTH W1, [W6++]
//        ICSP_Cmd(0);          // NOP
//        ICSP_Cmd(0);          // NOP
//        ICSP_Cmd(0xBB0B02);   // TBLWTL W2, [W6]
//        ICSP_Cmd(0);          // NOP
//        ICSP_Cmd(0);          // NOP
//        ICSP_Cmd(0xBB9B03);   // TBLWTH W3, [W6++]
//        ICSP_Cmd(0);          // NOP
//        ICSP_Cmd(0);          // NOP
//    }
//
//    // Step 7: Initiate the write cycle.
//     ICSP_unlockWR();
//    __delay_ms( CFG_TIME);
//
//    // Step 8: Wait for the Configuration Register Write operation to complete and make sure WR bit is clear.
//
//    //    ICSP_Cmd(0x803B00);  // MOV NVMCON, W0
//    //    ICSP_Cmd(0x883C20);  // MOV W0, VISI
//    //    ICSP_Cmd(0x000000);  // NOP
//    //
//    //    //TODO: Implement the regout command for instruction below
//    //    //<VISI>  // Clock out contents of VISI register.
//    //
//    //    ICSP_Cmd(0x040200);  // GOTO 0x200
//    //    ICSP_Cmd(0x000000);  // NOP
//    //
//    //    //Repeat until the WR bit is clear.
//}

/****************************************************************************/

bool lvp = false;

void LVP_enter(void) {
    LED_On(RED_LED);
    LED_Off(GREEN_LED);

    ICSP_init(); // configure I/Os
    ICSP_signature(); // enter LVP mode

    // fill row buffer with blanks
    memset((void*) row, 0xff, sizeof (row));
    row_address = -1;
    lvp = true;
}

void LVP_exit(void) {
    ICSP_slaveReset();
    __delay_ms(1);
    ICSP_release(); // release ICSP-DAT and ICSP-CLK
//    __delay_ms(1);
    lvp = false;
    LED_Off(RED_LED);
    LED_On(GREEN_LED);
}

bool LVP_inProgress(void) {
    return (lvp);
}

void LVP_write(void) {
    // check for first entry in lvp
    if (!LVP_inProgress()) {
        LVP_enter();
        ICSP_bulkErase();
    }
    if (row_address == -1) {
        /* do nothing */
    } else if (row_address >= (CFG_ADDRESS)) { // use the special cfg word sequence
        ICSP_addressLoad(row_address << 1);
        //        ICSP_cfgWrite( row, CFG_NUM);
    } else { // normal row programming sequence
        ICSP_addressLoad(row_address << 1);
        ICSP_rowWrite(row, ROW_SIZE);
    }
}

void LVP_commitRow(void) {
    // latch and program a row, skip if blank
    uint8_t i;
    uint16_t chk = 0xffff;
    for (i = 0; i < ROW_SIZE; i++) chk &= row[i]; // blank check
    if (chk != 0xffff) {
        LVP_write();
        memset((void*) row, 0xff, sizeof (row)); // fill buffer with blanks
    }
}

/**
 * Align and pack words in rows, ready for lvp programming
 * @param address       starting address
 * @param data          buffer
 * @param data_count    number of bytes
 */
void LVP_packRow(uint32_t address, uint8_t *data, uint8_t data_count) {
    // copy only the bytes from the current data packet up to the boundary of a row
    uint8_t index = (address & 0xfe) >> 1;
    uint32_t new_row = (address & 0xfffffff00) >> 1;
    if (new_row != row_address) {
        LVP_commitRow();
        row_address = new_row;
    }
    // ensure data is always even (rounding up)
    data_count = (data_count + 1) & 0xfe;
    // copy data up to the row boundaries
    while ((data_count > 0) && (index < ROW_SIZE)) {
        uint16_t word = *data++;
        word += ((uint16_t) (*data++) << 8);
        row[index++] = word;
        data_count -= 2;
    }
    // if a complete row was filled, proceed to programming
    if (index == ROW_SIZE) {
        LVP_commitRow();
        // next consider the split row scenario
        if (data_count > 0) { // leftover must spill into next row
            row_address += ROW_SIZE;
            index = 0;
            while (data_count > 0) {
                uint16_t word = *data++;
                word += ((uint16_t) (*data++) << 8);
                row[index++] = word;
                data_count -= 2;
            }
        }
    }
}

void LVP_programLastRow(void) {
    LVP_commitRow();
    LVP_exit();
}

void LVP_fiveNOP(void) {
    ICSP_Cmd(0); // NOP
    ICSP_Cmd(0); // NOP
    ICSP_Cmd(0); // NOP
    ICSP_Cmd(0); // NOP
    ICSP_Cmd(0); // NOP
}

uint16_t LVP_readWord(uint24_t addr) {
    ICSP_exitResetVector();
    sendSIXMov(addr >> 16, 0);      // mov (addru), W0
    ICSP_Cmd(0x20F887);           // mov #VISI, W7
    ICSP_Cmd(0x8802A0);           // mov W0, TBLPAG
    sendSIXMov(addr & 0xffff, 6);   // mov (addrl), W6
//    sendSIXMov(0x55AA,7);
    ICSP_Cmd(0);                  // nop
//    ICSP_Cmd(0xBA8B96);           // TBLRDH [w6],[w7]]
//    fiveNOP();
//    readVISI();
//    ICSP_Cmd(0x887C47);
    ICSP_Cmd(0xBA0B96);           // TBLRDL [w6],[w7]]
    LVP_fiveNOP();
    return ICSP_readVISI();
}

void utohex(char* buffer, uint16_t word){
    char c = word & 0xf;
    buffer += 3;
    *buffer-- = c + ((c<10) ? '0' : ('A'-10)); word>>=4; c = word & 0xf;
    *buffer-- = c + ((c<10) ? '0' : ('A'-10)); word>>=4; c = word & 0xf;
    *buffer-- = c + ((c<10) ? '0' : ('A'-10)); word>>=4; c = word & 0xf;
    *buffer-- = c + ((c<10) ? '0' : ('A'-10));
}

void catHexWord(char *buffer, uint24_t addr) {
    buffer += strlen(buffer); // append to buffer
    utohex(buffer, LVP_readWord(addr));
    buffer[strlen(buffer)] = ' ';
}

//void catDecWord(char *buffer, uint24_t addr) {
//    buffer += strlen(buffer);   // append to buffer
//    utoa(buffer, readWord(addr), 10);
//    buffer[strlen(buffer)] = ' ';
//}

uint16_t LVP_getInfoSize(void) {
    // a multiple of 64-char segments
    return 3 * 64;
}

void read_DevID(char *buffer) {
    //     read the DevID and RevID
    strcat(buffer, "\nDev ID: ");
    catHexWord(buffer, DEV_ID);
    strcat(buffer, "\n\nRev ID: ");
    catHexWord(buffer, REV_ID);
    strcat(buffer, "\n\nFlash : 128KB");
}

void read_Config(char *buffer) {
    // read the CONFIG
    strcat(buffer, "\n\nConfiguration:\n");
    catHexWord(buffer, CFG_ADDRESS+0);
    catHexWord(buffer, CFG_ADDRESS+16);
    catHexWord(buffer, CFG_ADDRESS+24);
    catHexWord(buffer, CFG_ADDRESS+28);
    catHexWord(buffer, CFG_ADDRESS+32);
}

void read_UID(char *buffer) {
    // read the Microchip Unique Identifier
    strcat(buffer, "\n\nUID:\n");
    catHexWord(buffer, UID_ADDRESS+0);
    catHexWord(buffer, UID_ADDRESS+2);
    catHexWord(buffer, UID_ADDRESS+4);
    catHexWord(buffer, UID_ADDRESS+6);
    catHexWord(buffer, UID_ADDRESS+8);
    catHexWord(buffer, UID_ADDRESS+10);
}


void LVP_getInfo(char* buffer, uint16_t seg) {
    // read device information, returns a fixed (64 byte) string at a time
    LVP_enter();
    //
    switch (seg) {
        case 0: read_DevID(buffer);  break;
        case 1: read_Config(buffer); break;
        case 2: read_UID(buffer); break;
        default: break;
    }
    //
    // padding with spaces (no \0 string terminator!)
    for (uint8_t i = strlen(buffer); i < 64; i++)
        buffer[i] = ' ';

    LVP_exit();
}
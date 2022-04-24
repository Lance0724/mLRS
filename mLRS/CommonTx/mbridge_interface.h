//*******************************************************
// Copyright (c) MLRS project
// GPL3
// https://www.gnu.org/licenses/gpl-3.0.de.html
// OlliW @ www.olliw.eu
//*******************************************************
// MBridge Interface Header
//********************************************************
#ifndef MBRIDGE_INTERFACE_H
#define MBRIDGE_INTERFACE_H
#pragma once


#if (defined DEVICE_HAS_JRPIN5)

#include "..\Common\fifo.h"
#include "setup_tx.h"
#include "jr_pin5_interface.h"
#include "mbridge_protocol.h"


uint16_t micros(void);


//-------------------------------------------------------
// Interface Implementation

class tMBridge : public tPin5BridgeBase, public tSerialBase
{
  public:
    void Init(bool enable_flag);
    bool ChannelsUpdated(tRcData* rc);
    bool TelemetryUpdateState(uint8_t* state);

    bool CommandReceived(uint8_t* cmd);
    void GetCommand(uint8_t* cmd, uint8_t* payload);
    uint8_t* GetPayloadPtr(void);
    void SendCommand(uint8_t cmd, uint8_t* payload);
    bool CommandInFifo(uint8_t* cmd);
    void Lock(uint8_t cmd);
    void Unlock(void);

    // for in-isr processing
    void uart_rx_callback(uint8_t c);
    void uart_tc_callback(void);
    void parse_nextchar(uint8_t c, uint16_t tnow_us) override;
    bool transmit_start(void) override; // returns true if transmission should be started
    uint8_t send_serial(void);
    void send_command(void);

    bool enabled;

    typedef enum {
        PARSE_TYPE_NONE = 0,
        PARSE_TYPE_SERIALPACKET,
        PARSE_TYPE_CHANNELPACKET,
        PARSE_TYPE_COMMANDPACKET,
    } PARSE_TYPE_ENUM;;
    uint8_t type;

    volatile bool channels_received;
    tMBridgeChannelBuffer channels;
    void fill_rcdata(tRcData* rc);

    volatile bool cmd_received;
    uint8_t cmd_r2m_frame[MBRIDGE_R2M_COMMAND_FRAME_LEN_MAX];

    volatile uint8_t cmd_m2r_available;
    uint8_t cmd_m2r_frame[MBRIDGE_M2R_COMMAND_FRAME_LEN_MAX];

    // front end to communicate with mbridge
    // mimics a serial interface to the main code
    void putc(char c) { tx_fifo.Put(c); }
    void putbuf(void* buf, uint16_t len) { tx_fifo.PutBuf(buf, len); }
    bool available(void) { return rx_fifo.Available(); }
    char getc(void) { return rx_fifo.Get(); }
    void flush(void) { rx_fifo.Flush(); }

    // backend
    // fills/reads the fifo's with the mBridge uart
    void serial_putc(char c) { rx_fifo.Put(c); }
    bool serial_rx_available(void) { return tx_fifo.Available(); }
    char serial_getc(void) { return tx_fifo.Get(); }

    FifoBase<char,TX_MBRIDGE_TXBUFSIZE> tx_fifo;
    FifoBase<char,TX_MBRIDGE_RXBUFSIZE> rx_fifo; // MissionPlanner is rude

    // for communication
    FifoBase<uint8_t,128> cmd_fifo;
    uint8_t cmd_in_process;
    uint8_t ack_cmd;
    bool ack_ok;
};

tMBridge mbridge;


// to avoid error: ISO C++ forbids taking the address of a bound member function to form a pointer to member function
void mbridge_uart_rx_callback(uint8_t c) { mbridge.uart_rx_callback(c); }
void mbridge_uart_tc_callback(void) { mbridge.uart_tc_callback(); }


// we do not add a delay here as with SpinOnce()
// the logic analyzer shows this gives a 30-35 us gap nevertheless, which is perfect

void tMBridge::uart_rx_callback(uint8_t c)
{
    if (state >= STATE_TRANSMIT_START) { // recover in case something went wrong
        state = STATE_IDLE;
    }

    uint16_t tnow_us = micros();
    parse_nextchar(c, tnow_us);

    if (transmit_start()) {
        pin5_tx_start();
    }
}


void tMBridge::uart_tc_callback(void)
{
    pin5_tx_enable(false);
    state = STATE_IDLE;
}


bool tMBridge::transmit_start(void)
{
uint8_t available = 0;

    if (state < STATE_TRANSMIT_START) return false; // we are in receiving

    if (state != STATE_TRANSMIT_START) {
        state = STATE_IDLE;
        return false;
    }

    if (cmd_m2r_available) {
        send_command(); // uses cmd_m2r_available
        available = cmd_m2r_available;
        cmd_m2r_available = 0;
    } else {
        available = send_serial();
    }

    if (!available) {
        state = STATE_IDLE;
        return false;
    }

    pin5_tx_enable(true);

    state = STATE_TRANSMITING;
    return true;
}


uint8_t tMBridge::send_serial(void)
{
    uint8_t count = 0;
    uint8_t payload[MBRIDGE_M2R_SERIAL_PAYLOAD_LEN_MAX];
    for (uint8_t i = 0; i < MBRIDGE_M2R_SERIAL_PAYLOAD_LEN_MAX; i++) {
        if (!serial_rx_available()) break;
        payload[count++] = serial_getc();
    }
    if (count > 0) {
        pin5_putc(0x00); // we can send anything we want which is not a command, send 0xoo so it is easy to recognize
        for (uint8_t i = 0; i < count; i++) {
            uint8_t c = payload[i];
            pin5_putc(c);
        }
    }
    return count;
}


void tMBridge::send_command(void)
{
    for (uint8_t i = 0; i < cmd_m2r_available; i++) {
        uint8_t c = cmd_m2r_frame[i];
        pin5_putc(c);
    }
}


#define MBRIDGE_TMO_US  250


void tMBridge::parse_nextchar(uint8_t c, uint16_t tnow_us)
{
    if (state != STATE_IDLE) {
        uint16_t dt = tnow_us - tlast_us;
        if (dt > MBRIDGE_TMO_US) state = STATE_IDLE; // timeout error
    }

    tlast_us = tnow_us;

    switch (state) {
    case STATE_IDLE:
        if (c == MBRIDGE_STX1) state = STATE_RECEIVE_MBRIDGE_STX2;
        break;

    case STATE_RECEIVE_MBRIDGE_STX2:
        if (c == MBRIDGE_STX2) state = STATE_RECEIVE_MBRIDGE_LEN; else state = STATE_IDLE; // error
        break;
    case STATE_RECEIVE_MBRIDGE_LEN:
        cnt = 0;
        if (c == MBRIDGE_CHANNELPACKET_STX) {
            len = MBRIDGE_CHANNELPACKET_SIZE;
            type = PARSE_TYPE_CHANNELPACKET;
            state = STATE_RECEIVE_MBRIDGE_CHANNELPACKET;
        } else
        if (c >= MBRIDGE_COMMANDPACKET_STX) {
            uint8_t cmd = c & (~MBRIDGE_COMMANDPACKET_MASK);
            cmd_r2m_frame[cnt++] = cmd;
            len = mbridge_cmd_payload_len(cmd);
            type = PARSE_TYPE_COMMANDPACKET;
            if (len == 0) {
                cmd_received = true;
                state = STATE_TRANSMIT_START;
            } else {
                state = STATE_RECEIVE_MBRIDGE_COMMANDPACKET;
            }
        } else
        if (c > MBRIDGE_R2M_SERIAL_PAYLOAD_LEN_MAX) {
            state = STATE_IDLE; // error
        } else
        if (c > 0) {
            len = c;
            type = PARSE_TYPE_SERIALPACKET;
            state = STATE_RECEIVE_MBRIDGE_SERIALPACKET;
        } else {
            type = PARSE_TYPE_NONE;
            state = STATE_TRANSMIT_START; // tx_len = 0, no payload
        }
        break;
    case STATE_RECEIVE_MBRIDGE_SERIALPACKET:
        serial_putc(c);
        cnt++;
        if (cnt >= len) state = STATE_TRANSMIT_START;
        break;
    case STATE_RECEIVE_MBRIDGE_CHANNELPACKET:
        channels.c[cnt++] = c;
        if (cnt >= len) {
            channels_received = true;
            state = STATE_TRANSMIT_START;
        }
        break;
    case STATE_RECEIVE_MBRIDGE_COMMANDPACKET:
        cmd_r2m_frame[cnt++] = c;
        if (cnt >= len + 1) {
            cmd_received = true;
            state = STATE_TRANSMIT_START;
        }
        break;
    }
}


// mBridge: ch0-15    11 bits, 1 .. 1024 .. 2047 for +-120%
//          ch16-17:  1 bit, 0 .. 1
// rcData:            11 bit, 1 .. 1024 .. 2047 for +-120%
void tMBridge::fill_rcdata(tRcData* rc)
{
    rc->ch[0] = channels.ch0;
    rc->ch[1] = channels.ch1;
    rc->ch[2] = channels.ch2;
    rc->ch[3] = channels.ch3;
    rc->ch[4] = channels.ch4;
    rc->ch[5] = channels.ch5;
    rc->ch[6] = channels.ch6;
    rc->ch[7] = channels.ch7;
    rc->ch[8] = channels.ch8;
    rc->ch[9] = channels.ch9;
    rc->ch[10] = channels.ch10;
    rc->ch[11] = channels.ch11;
    rc->ch[12] = channels.ch12;
    rc->ch[13] = channels.ch13;
    rc->ch[14] = channels.ch14;
    rc->ch[15] = channels.ch15;
    rc->ch[16] = (channels.ch16) ? 1876 : 172; // +-100%
    rc->ch[17] = (channels.ch17) ? 1876 : 172; // +-100%
}


//-------------------------------------------------------
// MBridge user interface

void tMBridge::Init(bool enable_flag)
{
    enabled = enable_flag;

    if (!enabled) return;

    uart_rx_callback_ptr = &mbridge_uart_rx_callback;
    uart_tc_callback_ptr = &mbridge_uart_tc_callback;

    tSerialBase::Init();
    tPin5BridgeBase::Init();

    type = PARSE_TYPE_NONE;
    channels_received = false;
    cmd_received = false;
    cmd_m2r_available = 0;

    tx_fifo.Init();
    rx_fifo.Init();

    cmd_fifo.Init();
    cmd_in_process = 0;
}


bool tMBridge::ChannelsUpdated(tRcData* rc)
{
    if (!enabled) return false;
    if (!channels_received) return false;

    channels_received = false;

    fill_rcdata(rc);
    return true;
}


bool tMBridge::TelemetryUpdateState(uint8_t* state)
{
    if (!enabled) return false;
    return tPin5BridgeBase::TelemetryUpdateState(state);
}


bool tMBridge::CommandReceived(uint8_t* cmd)
{
    if (!enabled) return false;
    if (!cmd_received) return false;

    cmd_received = false;

    *cmd = cmd_r2m_frame[0] & (~MBRIDGE_COMMANDPACKET_MASK);

    return true;
}


uint8_t* tMBridge::GetPayloadPtr(void)
{
    return &(cmd_r2m_frame[1]);
}


void tMBridge::GetCommand(uint8_t* cmd, uint8_t* payload)
{
    *cmd = cmd_r2m_frame[0] & (~MBRIDGE_COMMANDPACKET_MASK);

    uint8_t payload_len = mbridge_cmd_payload_len(*cmd);
    memcpy(payload, &(cmd_r2m_frame[1]), payload_len);
}


void tMBridge::SendCommand(uint8_t cmd, uint8_t* payload)
{
    memset(cmd_m2r_frame, 0, MBRIDGE_M2R_COMMAND_FRAME_LEN_MAX);

    uint8_t payload_len = mbridge_cmd_payload_len(cmd);

    cmd_m2r_frame[0] = MBRIDGE_COMMANDPACKET_STX + (cmd & (~MBRIDGE_COMMANDPACKET_MASK));
    memcpy(&(cmd_m2r_frame[1]), payload, payload_len);

    cmd_m2r_available = payload_len + 1;
}


bool tMBridge::CommandInFifo(uint8_t* cmd)
{
    if (cmd_in_process) return false;

    if (!cmd_fifo.Available()) return false;

    cmd_in_process = 0;

    *cmd = cmd_fifo.Get();

    return true;
}


void tMBridge::Lock(uint8_t cmd = 0xFF)
{
    cmd_in_process = cmd;
}


void tMBridge::Unlock(void)
{
    cmd_in_process = 0;
}


//-------------------------------------------------------
// convenience helper

void mbridge_send_LinkStats(void)
{
tMBridgeLinkStats lstats = {0};

    lstats.LQ = txstats.GetLQ(); // it's the same as GetLQ_serial_data() // = LQ_valid_received; // number of valid packets received on transmitter side
    lstats.rssi1_instantaneous = stats.last_rx_rssi1;
    lstats.rssi2_instantaneous = stats.last_rx_rssi2;
    lstats.snr_instantaneous = stats.GetLastRxSnr();
    lstats.receive_antenna = stats.last_rx_antenna;
    lstats.transmit_antenna = stats.last_tx_antenna;
#ifdef USE_DIVERSITY
    lstats.diversity = 1;
#else
    lstats.diversity = 0;
#endif
    lstats.rx1_valid = txstats.rx1_valid;
    lstats.rx2_valid = txstats.rx2_valid;

    lstats.rssi1_filtered = RSSI_INVALID;
    lstats.rssi2_filtered = RSSI_INVALID;
    lstats.snr_filtered = SNR_INVALID;

    // receiver side of things

    lstats.receiver_LQ = stats.received_LQ; // valid_crc1_received, number of rc data packets received on receiver side
    lstats.receiver_LQ_serial = stats.received_LQ_serial_data; // valid_frames_received, number of completely valid packets received on receiver side
    lstats.receiver_rssi_instantaneous = stats.received_rssi;
    lstats.receiver_receive_antenna = stats.received_antenna;
    lstats.receiver_transmit_antenna = stats.received_transmit_antenna;
    lstats.receiver_diversity = 0; // TODO: this we do not know currently

    lstats.receiver_rssi_filtered = RSSI_INVALID;

    // further stats acquired on transmitter side

    lstats.LQ_fresh_serial_packets_transmitted = stats.fresh_serial_data_transmitted.GetLQ();
    lstats.bytes_per_sec_transmitted = stats.GetTransmitBandwidthUsage();

    lstats.LQ_valid_received = stats.valid_frames_received.GetLQ(); // number of completely valid packets received per sec
    lstats.LQ_fresh_serial_packets_received = stats.fresh_serial_data_received.GetLQ();
    lstats.bytes_per_sec_received = stats.GetReceiveBandwidthUsage();

    lstats.LQ_received = stats.frames_received.GetLQ(); // number of packets received per sec, not practically relevant

    lstats.fhss_curr_i = txstats.fhss_curr_i;
    lstats.fhss_cnt = fhss.cnt;

    lstats.vehicle_state = mavlink_vehicle_state(); // 3 = invalid

    mbridge.SendCommand(MBRIDGE_CMD_TX_LINK_STATS, (uint8_t*)&lstats);
}


void mbridge_start_ParamRequestList(void);

uint8_t mbridge_send_RequestCmd(uint8_t* payload)
{
tMBridgeRequestCmd* request = (tMBridgeRequestCmd*)payload;

    switch (request->requested_cmd) {
    case MBRIDGE_CMD_DEVICE_ITEM_TX:
        mbridge.cmd_fifo.Put(MBRIDGE_CMD_DEVICE_ITEM_TX);
        break;

    case MBRIDGE_CMD_DEVICE_ITEM_RX:
        mbridge.cmd_fifo.Put(MBRIDGE_CMD_DEVICE_ITEM_RX);
        break;

    case MBRIDGE_CMD_INFO:
        mbridge.cmd_fifo.Put(MBRIDGE_CMD_INFO);
        break;

    case MBRIDGE_CMD_REQUEST_INFO:
        mbridge.cmd_fifo.Put(MBRIDGE_CMD_DEVICE_ITEM_TX);
        mbridge.cmd_fifo.Put(MBRIDGE_CMD_DEVICE_ITEM_RX);
        mbridge.cmd_fifo.Put(MBRIDGE_CMD_INFO);
        break;

    case MBRIDGE_CMD_PARAM_REQUEST_LIST:
        mbridge_start_ParamRequestList();
        break;

    case MBRIDGE_CMD_PARAM_ITEM:{
        //??
        //uint8_t idx = request->index;
        //if (request->name[0] != 0) { // name is specified, so search for index of parameter
        //}
        // TODO mbridge.cmd_task_fifo.Put(MBRIDGE_CMD_PARAM_ITEM);
        }break;
    }

    return request->requested_cmd;
}


uint8_t mbridge_send_RequestCmd(uint8_t request_cmd)
{
    return mbridge_send_RequestCmd(&request_cmd); // this is somewhat dirty, but does the job :)
}


void mbridge_send_Info(void)
{
tMBridgeInfo info = {0};

    info.receiver_sensitivity = sx.ReceiverSensitivity_dbm(); // is equal for Tx and Rx
    info.tx_actual_power_dbm = sx.RfPower_dbm();
    if (USE_ANTENNA1 && USE_ANTENNA2) {
        info.tx_actual_diversity = 0;
    } else
    if (USE_ANTENNA1) {
        info.tx_actual_diversity = 1;
    } else
    if (USE_ANTENNA2) {
        info.receiver_sensitivity = sx2.ReceiverSensitivity_dbm();
        info.tx_actual_power_dbm = sx2.RfPower_dbm();
        info.tx_actual_diversity = 2;
    } else {
        info.tx_actual_diversity = 3; // 3 = invalid
    }

    if (SetupMetaData.rx_available) {
        info.rx_available = 1;
        info.rx_actual_power_dbm = SetupMetaData.rx_actual_power_dbm;
        info.rx_actual_diversity = SetupMetaData.rx_actual_diversity;
    } else {
        info.rx_available = 0;
        info.rx_actual_power_dbm = INT8_MAX; // INT8_MAX = invalid
        info.rx_actual_diversity = 3; // 3 = invalid
    }

    mbridge.SendCommand(MBRIDGE_CMD_INFO, (uint8_t*)&info);
}


void mbridge_send_DeviceItemTx(void)
{
tMBridgeDeviceItem item = {0};

    item.firmware_version_u16 = version_to_u16(VERSION);
    item.setup_layout = SETUPLAYOUT;
    strbufstrcpy(item.device_name_20, DEVICE_NAME, 20);
    mbridge.SendCommand(MBRIDGE_CMD_DEVICE_ITEM_TX, (uint8_t*)&item);
}


void mbridge_send_DeviceItemRx(void)
{
tMBridgeDeviceItem item = {0};

    if (SetupMetaData.rx_available) {
        item.firmware_version_u16 = version_to_u16(SetupMetaData.rx_firmware_version);
        item.setup_layout = SetupMetaData.rx_setup_layout;
        strbufstrcpy(item.device_name_20, SetupMetaData.rx_device_name, 20);
    } else {
        item.firmware_version_u16 = 0;
        item.setup_layout = 0;
        strbufstrcpy(item.device_name_20, "", 20);
    }
    mbridge.SendCommand(MBRIDGE_CMD_DEVICE_ITEM_RX, (uint8_t*)&item);
}


uint8_t param_idx;
uint8_t param_itemtype_cnt;

// we have to send (much) more than SETUP_PARAMETER_NUM PARAM_ITEM messages
// since all parameters need 2 and some even 3 of them
// currently it are about 80 for the 36 parameters => 80 x 20ms = 1600 ms

void mbridge_start_ParamRequestList(void)
{
    param_idx = 0;
    param_itemtype_cnt = 0;

    mbridge.cmd_fifo.Put(MBRIDGE_CMD_PARAM_ITEM); // trigger sending out first
}


void mbridge_send_ParamItem(void)
{
    if (param_idx >= SETUP_PARAMETER_NUM) {
        // we send a mbridge message, but don't put a MBRIDGE_CMD_PARAM_ITEM into the fifo, this stops it
        tMBridgeParamItem item = {0};
        item.index = UINT8_MAX; // indicates end of list
        mbridge.SendCommand(MBRIDGE_CMD_PARAM_ITEM, (uint8_t*)&item);
        return;
    }

    bool item3_needed = false;

    if (param_itemtype_cnt == 0) {
        tMBridgeParamItem item = {0};
        item.index = param_idx;
        switch (SetupParameter[param_idx].type) {
        case SETUP_PARAM_TYPE_UINT8:
            item.type = MBRIDGE_PARAM_TYPE_UINT8;
            item.value.u8 = *(uint8_t*)(SetupParameter[param_idx].ptr);
            break;
        case SETUP_PARAM_TYPE_INT8:
            item.type = MBRIDGE_PARAM_TYPE_INT8;
            item.value.i8 = *(int8_t*)(SetupParameter[param_idx].ptr);
            break;
        case SETUP_PARAM_TYPE_UINT16:
            item.type = MBRIDGE_PARAM_TYPE_UINT16;
            item.value.u16 = *(uint16_t*)(SetupParameter[param_idx].ptr);
            break;
        case SETUP_PARAM_TYPE_INT16:
            item.type = MBRIDGE_PARAM_TYPE_INT16;
            item.value.i16 = *(int16_t*)(SetupParameter[param_idx].ptr);
            break;
        case SETUP_PARAM_TYPE_LIST:
            item.type = MBRIDGE_PARAM_TYPE_LIST;
            item.value.u8 = *(uint8_t*)(SetupParameter[param_idx].ptr);
            break;
        case SETUP_PARAM_TYPE_STR6:
            item.type = MBRIDGE_PARAM_TYPE_STR6;
            strbufstrcpy(item.str6_6, (char*)(SetupParameter[param_idx].ptr), 6);
            break;
        }
        strbufstrcpy(item.name_16, SetupParameter[param_idx].name, 16);

        mbridge.SendCommand(MBRIDGE_CMD_PARAM_ITEM, (uint8_t*)&item);

        param_itemtype_cnt = 1;

    } else
    if (param_itemtype_cnt == 1) {
        tMBridgeParamItem2 item2 = {0};
        item2.index = param_idx;
        switch (SetupParameter[param_idx].type) {
        case SETUP_PARAM_TYPE_UINT8:
            item2.dflt.u8 = SetupParameter[param_idx].dflt.UINT8_value;
            item2.min.u8 = SetupParameter[param_idx].min.UINT8_value;
            item2.max.u8 = SetupParameter[param_idx].max.UINT8_value;
            strbufstrcpy(item2.unit_6, SetupParameter[param_idx].unit, 6);
            break;
        case SETUP_PARAM_TYPE_INT8:
            item2.dflt.i8 = SetupParameter[param_idx].dflt.INT8_value;
            item2.min.i8 = SetupParameter[param_idx].min.INT8_value;
            item2.max.i8 = SetupParameter[param_idx].max.INT8_value;
            strbufstrcpy(item2.unit_6, SetupParameter[param_idx].unit, 6);
            break;
        case SETUP_PARAM_TYPE_UINT16:
            item2.dflt.u16 = SetupParameter[param_idx].dflt.UINT16_value;
            item2.min.u16 = SetupParameter[param_idx].min.UINT16_value;
            item2.max.u16 = SetupParameter[param_idx].max.UINT16_value;
            strbufstrcpy(item2.unit_6, SetupParameter[param_idx].unit, 6);
            break;
        case SETUP_PARAM_TYPE_INT16:
            item2.dflt.i16 = SetupParameter[param_idx].dflt.INT16_value;
            item2.min.i16 = SetupParameter[param_idx].min.INT16_value;
            item2.max.i16 = SetupParameter[param_idx].max.INT16_value;
            strbufstrcpy(item2.unit_6, SetupParameter[param_idx].unit, 6);
            break;
        case SETUP_PARAM_TYPE_LIST:
            if (SetupParameter[param_idx].allowed_mask_ptr != nullptr) {
                item2.allowed_mask = *SetupParameter[param_idx].allowed_mask_ptr;
            } else {
                item2.allowed_mask = UINT16_MAX;
            }
            strbufstrcpy(item2.options_21, SetupParameter[param_idx].optstr, 21);
            if (strlen(SetupParameter[param_idx].optstr) >= 21) item3_needed = true;
            break;
        }

        mbridge.SendCommand(MBRIDGE_CMD_PARAM_ITEM2, (uint8_t*)&item2);

        if (item3_needed) {
            param_itemtype_cnt = 2;
        } else {
            // next param item
            param_itemtype_cnt = 0;
            param_idx++;
        }
    } else
    if (param_itemtype_cnt >= 2) {
        tMBridgeParamItem3 item3 = {0};
        item3.index = param_idx;
        strbufstrcpy(item3.options2_23, SetupParameter[param_idx].optstr + 21, 23);

        mbridge.SendCommand(MBRIDGE_CMD_PARAM_ITEM3, (uint8_t*)&item3);

        // next param item
        param_itemtype_cnt = 0;
        param_idx++;
    }

    mbridge.cmd_fifo.Put(MBRIDGE_CMD_PARAM_ITEM); // trigger sending out next
}


bool mbridge_do_ParamSet(uint8_t* payload, bool* rx_param_changed)
{
tMBridgeParamSet* param = (tMBridgeParamSet*)payload;

    *rx_param_changed = false;

    if (param->index >= SETUP_PARAMETER_NUM) return false;

    if (SetupParameter[param->index].type <= SETUP_PARAM_TYPE_LIST) {
        *rx_param_changed = setup_set_param(param->index, param->value);
        return true;
    }

    if (SetupParameter[param->index].type == SETUP_PARAM_TYPE_STR6) {
        *rx_param_changed = setup_set_param_str6(param->index, param->str6_6);
        return true;
    }

    return false;
}


#else

class tMBridge : public tSerialBase
{
  public:
    void Init(bool enable_flag) {};
    void TelemetryStart(void) {}
    void TelemetryTick_ms(void) {}
    void tMBridge::Unlock(void) {}
};

tMBridge mbridge;

#endif // if (defined DEVICE_HAS_JRPIN5)

#endif // MBRIDGE_INTERFACE_H

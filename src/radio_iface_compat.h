#pragma once
// Local copy of decoder_modules/radio/src/radio_interface.h (SDR++ master),
// so this module builds out-of-tree without a path into the radio module.
// Values are positional enums and have been stable since the interface was added.
enum {
    RADIO_IFACE_CMD_GET_MODE,
    RADIO_IFACE_CMD_SET_MODE,
    RADIO_IFACE_CMD_GET_BANDWIDTH,
    RADIO_IFACE_CMD_SET_BANDWIDTH,
    RADIO_IFACE_CMD_GET_SQUELCH_MODE,
    RADIO_IFACE_CMD_SET_SQUELCH_MODE,
    RADIO_IFACE_CMD_GET_SQUELCH_LEVEL,
    RADIO_IFACE_CMD_SET_SQUELCH_LEVEL,
    RADIO_IFACE_CMD_GET_CTCSS_TONE,
    RADIO_IFACE_CMD_SET_CTCSS_TONE,
    RADIO_IFACE_CMD_GET_HIGHPASS,
    RADIO_IFACE_CMD_SET_HIGHPASS
};

enum {
    RADIO_IFACE_MODE_NFM,
    RADIO_IFACE_MODE_WFM,
    RADIO_IFACE_MODE_AM,
    RADIO_IFACE_MODE_DSB,
    RADIO_IFACE_MODE_USB,
    RADIO_IFACE_MODE_CW,
    RADIO_IFACE_MODE_LSB,
    RADIO_IFACE_MODE_RAW
};

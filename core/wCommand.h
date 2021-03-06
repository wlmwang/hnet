
/**
 * Copyright (C) Anny Wang.
 * Copyright (C) Hupu, Inc.
 */

#ifndef _W_COMMAND_H_
#define _W_COMMAND_H_

#include "wCore.h"

namespace hnet {

#pragma pack(1)

const uint8_t kCmdNull = 0;
const uint8_t kParaNull = 0;

// 消息ID
inline uint16_t CmdId(uint8_t cmd, uint8_t para) {
    if (kLittleEndian) {
        return (static_cast<uint16_t>(para) << 8) | (static_cast<uint16_t>(cmd));
    } else {
        return (static_cast<uint16_t>(cmd) << 8) | (static_cast<uint16_t>(para));
    }
}

struct wNull_t {
public:
    wNull_t(const uint8_t cmd, const uint8_t para): mCmd(cmd), mPara(para) { }

    inline uint16_t GetId() const {
        return mId;
    }
    inline uint8_t GetCmd() const {
        return mCmd;
    }
    inline uint8_t GetPara() const {
        return mPara;
    }
    
    union {
        struct {
            uint8_t mCmd; 
            uint8_t mPara;
        };
        uint16_t mId;
    };
};

struct wCommand : public wNull_t {
public:
    wCommand(const uint8_t cmd = kCmdNull, const uint8_t para = kParaNull): wNull_t(cmd, para) { }

    inline void ParseFromArray(char* buf, uint32_t len) {
        const wCommand* cmd = reinterpret_cast<wCommand*>(buf);
        if (GetId() == cmd->GetId()) {
            memcpy(this, buf, len);
        }
    }
};

#pragma pack()

}   // namespace hnet

#endif
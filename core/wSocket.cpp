
/**
 * Copyright (C) Anny Wang.
 * Copyright (C) Hupu, Inc.
 */

#include "wSocket.h"
#include "wLog.h"
#include "wMisc.h"

namespace hnet {

wSocket::wSocket(SockType type, SockProto proto, SockFlag flag) : mFD(kFDUnknown), mPort(0), mRecvTm(0), mSendTm(0), 
mMakeTm(misc::GetTimeofday()), mSockType(type), mSockProto(proto), mSockFlag(flag) { }

wSocket::~wSocket() {
    Close();
}

wStatus wSocket::Close() {
    if (close(mFD) == -1) {
        return mStatus = wStatus::IOError("wSocket::Close failed", strerror(errno));
    }
    mFD = kFDUnknown;
    return mStatus = wStatus::Nothing();
}

wStatus wSocket::SetFL(bool nonblock) {
    int flags = fcntl(mFD, F_GETFL, 0);
    if (flags == -1) {
        return mStatus = wStatus::IOError("wSocket::SetFL F_GETFL failed", strerror(errno));
    }
    if (fcntl(mFD, F_SETFL, (nonblock == true ? flags | O_NONBLOCK : flags & ~O_NONBLOCK)) == -1) {
        return mStatus = wStatus::IOError("wSocket::SetFL F_SETFL failed", strerror(errno));
    }
    return mStatus = wStatus::Nothing();
}

wStatus wSocket::RecvBytes(char buf[], size_t len, ssize_t *size) {
    mRecvTm = misc::GetTimeofday();
    
    while (true) {
        *size = recv(mFD, buf, len, 0);
        if (*size > 0) {
            mStatus = wStatus::Nothing();
            break;
        } else if (*size == 0) {
            mStatus = wStatus::IOError("wSocket::RecvBytes, client was closed", "");
            *size = static_cast<ssize_t>(kSeClosed);
            break;
        } else if (errno == EAGAIN) {
            mStatus = wStatus::Nothing();
            *size = static_cast<ssize_t>(0);
            break;
        } else if (errno == EINTR) {
            // 操作被信号中断，中断后唤醒继续处理
            // 注意：系统中信号安装需提供参数SA_RESTART，否则请按 EAGAIN 信号处理
            continue;
        } else {
            mStatus = wStatus::IOError("wSocket::RecvBytes, recv failed", strerror(errno));
            break;
        }
    }
    return mStatus;
}

wStatus wSocket::SendBytes(char buf[], size_t len, ssize_t *size) {
    mSendTm = misc::GetTimeofday();
    
    ssize_t sendedlen = 0, leftlen = len;
    while (true) {
        sendlen = send(mFD, buf + sendedlen, leftlen, 0);
        if (sendlen >= 0) {
            sendedlen += sendlen;
            if ((leftlen -= sendlen) == 0) {
                mStatus = wStatus::Nothing();
                *size = sendedlen;
                break;
            }
        } else if (errno == EAGAIN) {
            mStatus = wStatus::Nothing();
            *size = static_cast<ssize_t>(0);
            break;
        } else if (errno == EINTR) {
            // 操作被信号中断，中断后唤醒继续处理
            // 注意：系统中信号安装需提供参数SA_RESTART，否则请按 EAGAIN 信号处理
            continue;
        } else {
            *size = -1;
            mStatus = wStatus::IOError("wSocket::SendBytes, send failed", strerror(errno));
            break;
        }
    }
    return mStatus;
}

}   // namespace hnet


/**
 * Copyright (C) Anny Wang.
 * Copyright (C) Hupu, Inc.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include "wMisc.h"
#include "wTcpSocket.h"

namespace hnet {

wStatus wTcpSocket::Open() {
	if ((mFD = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Open socket() AF_INET failed", strerror(errno));
	}

	int flags = 1;
	if (setsockopt(mFD, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(flags)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Open setsockopt() SO_REUSEADDR failed", strerror(errno));
	}

	struct linger stLing = {0, 0};	// 优雅断开
	if (setsockopt(mFD, SOL_SOCKET, SO_LINGER, &stLing, sizeof(stLing)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Open setsockopt() SO_LINGER failed", strerror(errno));
	}

	// 启用保活机制
	if (mIsKeepAlive) {
		return SetKeepAlive(kKeepAliveTm, kKeepAliveTm, kKeepAliveCnt);
	}
	return mStatus = wStatus::Nothing();
}

wStatus wTcpSocket::Bind(string host, uint16_t port) {
	mHost = host;
	mPort = port;

	struct sockaddr_in stSocketAddr;
	stSocketAddr.sin_family = AF_INET;
	stSocketAddr.sin_port = htons((short)mPort);
	stSocketAddr.sin_addr.s_addr = inet_addr(mHost.c_str());

	if (bind(mFD, (struct sockaddr *)&stSocketAddr, sizeof(stSocketAddr)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Bind bind failed", strerror(errno));
	}
	return mStatus = wStatus::Nothing();
}

wStatus wTcpSocket::Listen(string host, uint16_t port) {
	if (!Bind(host, port).Ok()) {
		return mStatus;
	}

	// 设置发送缓冲大小4M
	int iOptLen = sizeof(socklen_t);
	int iOptVal = 0x400000;
	if (setsockopt(mFD, SOL_SOCKET, SO_SNDBUF, (const void *)&iOptVal, iOptLen) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Listen setsockopt() SO_SNDBUF failed", strerror(errno));
	}
	
	if (listen(mFD, kListenBacklog) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Listen listen failed", strerror(errno));
	}
	return SetFL();
}

wStatus wTcpSocket::Connect(int64_t *ret, string host, uint16_t port, float timeout) {
	mHost = host;
	mPort = port;

	struct sockaddr_in stSockAddr;
	memset(&stSockAddr, 0, sizeof(sockaddr_in));
	stSockAddr.sin_family = AF_INET;
	stSockAddr.sin_port = htons((short)mPort);
	stSockAddr.sin_addr.s_addr = inet_addr(mHost.c_str());

	socklen_t iOptVal = 100*1024;
	socklen_t iOptLen = sizeof(socklen_t);
	if (setsockopt(mFD, SOL_SOCKET, SO_SNDBUF, (const void *)&iOptVal, iOptLen) == -1) {
		*ret = -1;
		return mStatus = wStatus::IOError("wTcpSocket::Connect setsockopt() SO_SNDBUF failed", strerror(errno));
	}

	// 超时设置
	if (timeout > 0) {
		if (!SetFL().Ok()) {
			*ret = static_cast<int64_t>(-1);
			return mStatus;
		}
	}

	*ret = static_cast<int64_t>(connect(mFD, (const struct sockaddr *)&stSockAddr, sizeof(stSockAddr)));
	if (*ret == -1 && timeout > 0) {
		// 建立启动但是尚未完成
		if (errno == EINPROGRESS) {
			while (true) {
				struct pollfd pfd;
				pfd.fd = mFD;
				pfd.events = POLLIN | POLLOUT;
				int rt = poll(&pfd, 1, timeout * 1000000);	// 微妙
				if (rt == -1) {
					if (errno == EINTR) {
					    continue;
					}
					return mStatus = wStatus::IOError("wTcpSocket::Connect poll failed", strerror(errno));
				} else if(rt == 0) {
					*ret = static_cast<int64_t>(kSeTimeout);
					return mStatus = wStatus::IOError("wTcpSocket::Connect connect timeout", timeout);
				} else {
					int val, len = sizeof(int);
					if (getsockopt(mFD, SOL_SOCKET, SO_ERROR, (char*)&val, (socklen_t*)&len) == -1) {
					    return mStatus = wStatus::IOError("wTcpSocket::Connect getsockopt SO_ERROR failed", strerror(errno));
					}
					if (val > 0) {
					    return mStatus = wStatus::IOError("wTcpSocket::Connect connect failed", strerror(errno));
					}

					// 连接成功
					*ret = 0;
					break;
				}
			}
		} else {
			return mStatus = wStatus::IOError("wTcpSocket::Connect connect directly failed", strerror(errno));
		}
	}
	
	return mStatus = wStatus::Nothing();
}

wStatus wTcpSocket::Accept(int64_t *fd, struct sockaddr* clientaddr, socklen_t *addrsize) {
	if (mSockType != kStListen) {
		*fd = -1;
		return mStatus = wStatus::InvalidArgument("wTcpSocket::Accept", "is not listen socket");
	}

	while (true) {
		*fd = static_cast<int64_t>(accept(mFD, clientaddr, addrsize));
		if (*fd > 0) {
            mStatus = wStatus::Nothing();
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
            mStatus = wStatus::IOError("wSocket::Accept, accept failed", strerror(errno));
            break;
        }
	}

	if (mStatus.Ok()) {
		// 设置发送缓冲大小3M
		int iOptLen = sizeof(socklen_t);
		int iOptVal = 0x300000;
		if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const void *)&iOptVal, iOptLen) == -1) {
			mStatus = wStatus::IOError("wTcpSocket::Accept setsockopt() SO_SNDBUF failed", strerror(errno));
		}
	}

	return mStatus;
}

wStatus wTcpSocket::SetTimeout(float timeout) {
	if (SetSendTimeout(timeout).Ok()) {
		return mStatus;
	}
	return SetRecvTimeout(timeout);
}

wStatus wTcpSocket::SetSendTimeout(float timeout) {
	struct timeval stTimetv;
	stTimetv.tv_sec = (int)timeout>=0 ? (int)timeout : 0;
	stTimetv.tv_usec = (int)((timeout - (int)timeout) * 1000000);
	if (stTimetv.tv_usec < 0 || stTimetv.tv_usec >= 1000000 || (stTimetv.tv_sec == 0 && stTimetv.tv_usec == 0)) {
		stTimetv.tv_sec = 30;
		stTimetv.tv_usec = 0;
	}

	if (setsockopt(mFD, SOL_SOCKET, SO_SNDTIMEO, &stTimetv, sizeof(stTimetv)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Connect setsockopt() SO_SNDTIMEO failed", strerror(errno));
	}
	return mStatus = wStatus::Nothing();
}

wStatus wTcpSocket::SetRecvTimeout(float timeout) {
	struct timeval stTimetv;
	stTimetv.tv_sec = (int)timeout>=0 ? (int)timeout : 0;
	stTimetv.tv_usec = (int)((timeout - (int)timeout) * 1000000);
	if (stTimetv.tv_usec < 0 || stTimetv.tv_usec >= 1000000 || (stTimetv.tv_sec == 0 && stTimetv.tv_usec == 0)) {
		stTimetv.tv_sec = 30;
		stTimetv.tv_usec = 0;
	}
	
	if (setsockopt(mFD, SOL_SOCKET, SO_RCVTIMEO, &stTimetv, sizeof(stTimetv)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Connect setsockopt() SO_RCVTIMEO failed", strerror(errno));
	}
	return mStatus = wStatus::Nothing();
}

wStatus wTcpSocket::SetKeepAlive(int idle, int intvl, int cnt) {
	int flages = 1;
	if (setsockopt(mFD, SOL_SOCKET, SO_KEEPALIVE, &flages, sizeof(flages)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Connect setsockopt() SO_KEEPALIVE failed", strerror(errno));
	}
	if (setsockopt(mFD, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Connect setsockopt() TCP_KEEPIDLE failed", strerror(errno));
	}
	if (setsockopt(mFD, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Connect setsockopt() TCP_KEEPINTVL failed", strerror(errno));
	}
	if (setsockopt(mFD, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Connect setsockopt() TCP_KEEPCNT failed", strerror(errno)); 
	}

	// Linux Kernel 2.6.37
	// 如果发送出去的数据包在十秒内未收到ACK确认，则下一次调用send或者recv，则函数会返回-1，errno设置为ETIMEOUT
#	ifdef TCP_USER_TIMEOUT
	unsigned int timeout = 10000;
	if (setsockopt(mFD, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout, sizeof(timeout)) == -1) {
		return mStatus = wStatus::IOError("wTcpSocket::Connect setsockopt() TCP_USER_TIMEOUT failed", strerror(errno)); 
	}
#	endif

	return mStatus = wStatus::Nothing();
}

}	// namespace hnet

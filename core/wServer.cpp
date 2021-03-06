
/**
 * Copyright (C) Anny Wang.
 * Copyright (C) Hupu, Inc.
 */

#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <algorithm>
#include "wServer.h"
#include "wConfig.h"
#include "wShm.h"
#include "wMaster.h"
#include "wLogger.h"
#include "wWorker.h"
#include "wTcpSocket.h"
#include "wUdpSocket.h"
#include "wUnixSocket.h"
#include "wChannelSocket.h"
#include "wTcpTask.h"
#include "wUdpTask.h"
#include "wUnixTask.h"
#include "wChannelTask.h"
#include "wHttpTask.h"

namespace hnet {

wServer::wServer(wConfig* config): mExiting(false), mTick(0), mHeartbeatTurn(kHeartbeatTurn), mEpollFD(kFDUnknown), mTimeout(10), 
mShm(NULL), mAcceptAtomic(NULL), mAcceptFL(NULL), mUseAcceptTurn(kAcceptTurn), mAcceptHeld(false), mAcceptDisabled(0), 
mMaster(NULL), mConfig(config), mEnv(wEnv::Default()) {
	assert(mConfig != NULL);
    mLatestTm = soft::TimeUsec();
    mHeartbeatTimer = wTimer(kKeepAliveTm);
}

wServer::~wServer() {
    CleanTask();
    HNET_DELETE(mShm);
}

int wServer::PrepareStart(const std::string& ipaddr, uint16_t port, const std::string& protocol) {
	// 创建非阻塞listen socket
	int ret = AddListener(ipaddr, port, protocol);
    if (ret == -1) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::PrepareStart AddListener() failed", "");
		return ret;
    }

    ret = PrepareRun();
    if (ret == -1) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::PrepareStart PrepareRun() failed", "");
    	return ret;
    }
    return ret;
}

int wServer::SingleStart(bool daemon) {
	int ret = InitEpoll();
    if (ret == -1) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::SingleStart InitEpoll() failed", "");
		return ret;
    }

    ret = Listener2Epoll(true);
    if (ret == -1) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::SingleStart Listener2Epoll() failed", "");
		return ret;
    }
    
    // 单进程关闭惊群锁
    mUseAcceptTurn = false;

    // 进入服务主循环
    while (daemon) {
    	soft::TimeUpdate();

    	if (mExiting) {
		    ProcessExit();
		    CleanListenSock();
		    exit(0);
    	}

		Recv();
		HandleSignal();
		Run();
		CheckTick();
    }
    return 0;
}

int wServer::WorkerStart(bool daemon) {
	// 初始化epoll，并监听listen socket、channel socket事件
    int ret = InitEpoll();
    if (ret == -1) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::WorkerStart InitEpoll() failed", "");
		return ret;
    }

    ret = Listener2Epoll(true);
    if (ret == -1) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::WorkerStart Listener2Epoll() failed", "");
		return ret;
    }

    ret = Channel2Epoll(true);
    if (ret == -1) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::WorkerStart Channel2Epoll() failed", "");
    	return ret;
    }

    // 清除epoll中listen socket监听事件，由各worker争抢惊群锁
    if (mUseAcceptTurn == true && mMaster->WorkerNum() > 1) {
    	if (RemoveListener(false) == -1) {
    		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::WorkerStart RemoveListener() failed", "");
    		return -1;
    	}
    }

    // 进入服务主循环
    while (daemon) {
    	soft::TimeUpdate();
    	
    	if (mExiting) {
    	    if (kAcceptStuff == 0 && mShm) {
    	    	mAcceptAtomic->CompareExchangeWeak(mMaster->mWorker->mPid, -1);
    	    	mShm->Remove();
    	    }
    	   	ProcessExit();
			CleanListenSock();
		    exit(0);
    	}
    
		Recv();
		Run();
		CheckTick();
		HandleSignal();
    }
    return -1;
}

int wServer::HandleSignal() {
    if (hnet_terminate) {
	    if (kAcceptStuff == 0 && mShm) {
	    	mAcceptAtomic->CompareExchangeWeak(mMaster->mWorker->mPid, -1);
	    	mShm->Remove();
	    }
	   	ProcessExit();
		exit(0);
    } else if (hnet_quit)	{
		hnet_quit = 0;
		if (!mExiting) {
		    mExiting = true;
		}
    }
    return 0;
}

int wServer::NewTcpTask(wSocket* sock, wTask** ptr) {
    HNET_NEW(wTcpTask(sock), *ptr);
    if (!*ptr) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::NewTcpTask new() failed", "");
    	return -1;
    }
    return 0;
}

int wServer::NewUdpTask(wSocket* sock, wTask** ptr) {
    HNET_NEW(wUdpTask(sock), *ptr);
    if (!*ptr) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::NewUdpTask new() failed", "");
    	return -1;
    }
    return 0;
}

int wServer::NewUnixTask(wSocket* sock, wTask** ptr) {
    HNET_NEW(wUnixTask(sock), *ptr);
    if (!*ptr) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::NewUnixTask new() failed", "");
		return -1;
    }
    return 0;
}

int wServer::NewChannelTask(wSocket* sock, wTask** ptr) {
	HNET_NEW(wChannelTask(sock, mMaster), *ptr);
    if (!*ptr) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::NewChannelTask new() failed", "");
		return -1;
    }
    return 0;
}

int wServer::NewHttpTask(wSocket* sock, wTask** ptr) {
    HNET_NEW(wHttpTask(sock), *ptr);
    if (!*ptr) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::NewHttpTask new() failed", "");
		return -1;
    }
    return 0;
}

int wServer::InitAcceptMutex() {
	if (mUseAcceptTurn == true && mMaster->WorkerNum() > 1) {
		if (kAcceptStuff == 0) {
    		if (mEnv->NewShm(soft::GetAcceptPath(), &mShm, sizeof(wAtomic<int>)) == 0) {
    			if (mShm->CreateShm() == 0) {
    				void* ptr = mShm->AllocShm(sizeof(wAtomic<int>));
    				if (ptr) {
    					HNET_NEW((ptr)wAtomic<int>, mAcceptAtomic);
    					mAcceptAtomic->Exchange(-1);
    				} else {
    					HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::InitAcceptMutex AllocShm() failed", "");
    					return -1;
    				}
    			} else {
    				HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::InitAcceptMutex CreateShm() failed", "");
    				return -1;
    			}
    		}
		} else if (kAcceptStuff == 1) {
    		int fd;
    		if (mEnv->OpenFile(soft::GetAcceptPath(), fd) == 0) {
    			mEnv->CloseFD(fd);
    		}
		}
	} else if (mUseAcceptTurn == true) {
		mUseAcceptTurn = false;
	}
	return 0;
}

int wServer::ReleaseAcceptMutex(int pid) {
	if (kAcceptStuff == 0 && mShm) {
		mAcceptAtomic->CompareExchangeWeak(pid, -1);
	}
	return 0;
}

int wServer::Recv() {
	// 争抢accept锁
	if (mUseAcceptTurn == true && mAcceptHeld == false) {
		if ((kAcceptStuff == 0 && mAcceptAtomic->CompareExchangeWeak(-1, mMaster->mWorker->mPid)) ||
			(kAcceptStuff == 1 && mEnv->LockFile(soft::GetAcceptPath(), &mAcceptFL) == 0)) {
			Listener2Epoll(false);
			mAcceptHeld = true;
		}
	}

	// 事件循环
	struct epoll_event evt[kListenBacklog];
	int ret = epoll_wait(mEpollFD, evt, kListenBacklog, mTimeout);
	if (ret == -1) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Recv epoll_wait() failed", error::Strerror(errno).c_str());
	}

	for (int i = 0; i < ret && evt[i].data.ptr; i++) {
		wTask* task = reinterpret_cast<wTask*>(evt[i].data.ptr);

		if (task->Socket()->FD() == kFDUnknown || evt[i].events & (EPOLLERR | EPOLLPRI)) {
			if (task->Socket()->SP() != kSpUdp && task->Socket()->SP() != kSpChannel) {	// udp无需删除task
				task->DisConnect();
				RemoveTask(task);
			}
		} else if (task->Socket()->ST() == kStListen && task->Socket()->SS() == kSsListened) {
			if (evt[i].events & EPOLLIN) {	// 套接口准备好了接受新连接
				if (AcceptConn(task) == -1) {
					HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Recv AcceptConn() failed", "");
				}
			} else {
				HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Recv () failed", "error event");
			}
		} else if (task->Socket()->ST() == kStConnect && task->Socket()->SS() == kSsConnected) {
			if (evt[i].events & EPOLLIN) {	// 套接口准备好了读取操作
				ssize_t size;
				if (task->TaskRecv(&size) == -1) {
					if (task->Socket()->SP() != kSpUdp && task->Socket()->SP() != kSpChannel) {	// udp无需删除task
						task->DisConnect();
						RemoveTask(task);
					}
				}
			} else if (evt[i].events & EPOLLOUT) {
				if (task->SendLen() <= 0) {	// 清除写事件
					AddTask(task, EPOLLIN, EPOLL_CTL_MOD, false);
				} else {
					// 套接口准备好了写入操作
					// 写入失败，半连接，对端读关闭（udp无需删除task）
					ssize_t size;
					if (task->TaskSend(&size) == -1) {
						if (task->Socket()->SP() != kSpUdp && task->Socket()->SP() != kSpChannel) {
							task->DisConnect();
							RemoveTask(task);
						}
					}
				}
			}
		}
	}

	// 释放accept锁
	if (mUseAcceptTurn == true && mAcceptHeld == true) {
		if (kAcceptStuff == 0 && mAcceptAtomic->CompareExchangeWeak(mMaster->mWorker->mPid, -1)) {
			RemoveListener(false);
			mAcceptHeld = false;
		} else if (kAcceptStuff == 1 && mEnv->UnlockFile(mAcceptFL) == 0) {
			RemoveListener(false);
			mAcceptHeld = false;
		}
	}
    return 0;
}

int wServer::AcceptConn(wTask *task) {
	wTask *ctask = NULL;
	int ret, fd;
    if (task->Socket()->SP() == kSpUnix) {
		struct sockaddr_un sockAddr;
		socklen_t sockAddrSize = sizeof(sockAddr);
		ret = task->Socket()->Accept(&fd, reinterpret_cast<struct sockaddr*>(&sockAddr), &sockAddrSize);
		if (ret == -1) {
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn Accept() failed", "");
			return -1;
		} else if (fd <= 0) {
			HNET_ERROR(soft::GetLogPath(), "%s : %s[%d]", "wServer::AcceptConn Accept() failed", "fd", fd);
			return -1;
		}

		// unix socket
		wUnixSocket *socket = NULL;
		HNET_NEW(wUnixSocket(kStConnect), socket);
		if (!socket) {
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn new() failed", error::Strerror(errno).c_str());
			return -1;
		}

		socket->FD() = fd;
		socket->Host() = sockAddr.sun_path;
		socket->Port() = 0;
		socket->SS() = kSsConnected;

		ret = socket->SetNonblock();
		if (ret == -1) {
			HNET_DELETE(socket);
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn SetNonblock() failed", "");
			return ret;
		}

		ret = NewUnixTask(socket, &ctask);
		if (ret == -1) {
			HNET_DELETE(socket);
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn NewUnixTask() failed", "");
			return ret;
		}

    } else if (task->Socket()->SP() == kSpTcp) {
		struct sockaddr_in sockAddr;
		socklen_t sockAddrSize = sizeof(sockAddr);
		ret = task->Socket()->Accept(&fd, reinterpret_cast<struct sockaddr*>(&sockAddr), &sockAddrSize);
		if (ret == -1) {
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn Accept() failed", "");
			return -1;
		} else if (fd <= 0) {
			HNET_ERROR(soft::GetLogPath(), "%s : %s[%d]", "wServer::AcceptConn Accept() failed", "fd", fd);
			return -1;
		}

		// tcp socket
		wTcpSocket *socket = NULL;
		HNET_NEW(wTcpSocket(kStConnect), socket);
		if (!socket) {
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn new() failed", error::Strerror(errno).c_str());
			return -1;
		}

		socket->FD() = fd;
		socket->Host() = inet_ntoa(sockAddr.sin_addr);
		socket->Port() = sockAddr.sin_port;
		socket->SS() = kSsConnected;

		ret = socket->SetNonblock();
		if (ret == -1) {
			HNET_DELETE(socket);
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn SetNonblock() failed", "");
			return ret;
		}

		ret = NewTcpTask(socket, &ctask);
		if (ret == -1) {
			HNET_DELETE(socket);
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn NewTcpTask() failed", "");
			return ret;
		}

	} else if (task->Socket()->SP() == kSpHttp) {
		struct sockaddr_in sockAddr;
		socklen_t sockAddrSize = sizeof(sockAddr);	
		ret = task->Socket()->Accept(&fd, reinterpret_cast<struct sockaddr*>(&sockAddr), &sockAddrSize);
		if (ret == -1) {
		    HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn Accept() failed", "");
		    return -1;
		} else if (fd <= 0) {
			HNET_ERROR(soft::GetLogPath(), "%s : %s[%d]", "wServer::AcceptConn Accept() failed", "fd", fd);
			return -1;
		}

		// http socket
		wTcpSocket *socket = NULL;
		HNET_NEW(wTcpSocket(kStConnect, kSpHttp), socket);
		if (!socket) {
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn new() failed", error::Strerror(errno).c_str());
			return -1;
		}

		socket->FD() = fd;
		socket->Host() = inet_ntoa(sockAddr.sin_addr);
		socket->Port() = sockAddr.sin_port;
		socket->SS() = kSsConnected;

		ret = socket->SetNonblock();
		if (ret == -1) {
			HNET_DELETE(socket);
		    HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn SetNonblock() failed", "");
		    return ret;
		}

		ret = NewHttpTask(socket, &ctask);
		if (ret == -1) {
			HNET_DELETE(socket);
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn NewHttpTask() failed", "");
			return ret;
		}

    } else {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn () failed", "");
		return -1;
    }

    ret = AddTask(ctask);
	if (ret == -1) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn AddTask() failed", "");
	    return RemoveTask(ctask);
	}

	ret = ctask->Connect();
	if (ret == -1) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AcceptConn Connect() failed", "");
		return RemoveTask(ctask);
	}
    return 0;
}

int wServer::Broadcast(char *cmd, int len) {
	for (std::vector<wTask*>::iterator it = mTaskPool.begin(); it != mTaskPool.end(); it++) {
		if ((*it)->Socket()->ST() == kStConnect && (*it)->Socket()->SS() == kSsConnected && (*it)->Socket()->SP() == kSpTcp && 
			((*it)->Socket()->SF() == kSfSend || (*it)->Socket()->SF() == kSfRvsd)) {
			Send(*it, cmd, len);
		}
	}
    return 0;
}

#ifdef _USE_PROTOBUF_
int wServer::Broadcast(const google::protobuf::Message* msg) {
	for (std::vector<wTask*>::iterator it = mTaskPool.begin(); it != mTaskPool.end(); it++) {
		if ((*it)->Socket()->ST() == kStConnect && (*it)->Socket()->SS() == kSsConnected && (*it)->Socket()->SP() == kSpTcp && 
			((*it)->Socket()->SF() == kSfSend || (*it)->Socket()->SF() == kSfRvsd)) {
			Send(*it, msg);
		}
	}
    return 0;
}
#endif

int wServer::Send(wTask *task, char *cmd, size_t len) {
	int ret = task->Send2Buf(cmd, len);
	if (ret == 0) {
	    ret = AddTask(task, EPOLLIN | EPOLLOUT, EPOLL_CTL_MOD, false);
	}
    return ret;
}

#ifdef _USE_PROTOBUF_
int wServer::Send(wTask *task, const google::protobuf::Message* msg) {
	int ret = task->Send2Buf(msg);
	if (ret == 0) {
	    ret = AddTask(task, EPOLLIN | EPOLLOUT, EPOLL_CTL_MOD, false);
	}
    return ret;
}
#endif

int wServer::FindTaskBySocket(wTask** task, const wSocket* sock) {
	if (!sock) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::FindTaskBySocket () failed", "sock null");
		return -1;
	}

	for (std::vector<wTask*>::iterator it = mTaskPool.begin(); it != mTaskPool.end(); it++) {
		if ((*it)->Socket() == sock) {	// 直接地址比较
			*task = *it;
			return 0;
		}
	}
	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::FindTaskBySocket () failed", "not found");
	return -1;
}

int wServer::AsyncWorker(char *cmd, int len, uint32_t solt, const std::vector<uint32_t>* blackslot) {
	if (Master()->WorkerNum() <= 1) {
		return 0;
	}
	if (solt == kMaxProcess) {	// 广播消息
		for (uint32_t i = 0; i < kMaxProcess; i++) {
			if (mMaster->Worker(i)->mPid == -1 || mMaster->Worker(i)->ChannelFD(0) == kFDUnknown) {
				continue;
			} else if (blackslot && std::find(blackslot->begin(), blackslot->end(), i) != blackslot->end()) {
				continue;
			}

			wTask *task = NULL;
			if (FindTaskBySocket(&task, mMaster->Worker(i)->Channel()) == 0) {
				Send(task, cmd, len);
			}
	    }
	} else {
		if (mMaster->Worker(solt)->mPid != -1 && mMaster->Worker(solt)->ChannelFD(0) != kFDUnknown) {

			wTask *task = NULL;
			if (FindTaskBySocket(&task, mMaster->Worker(solt)->Channel()) == 0) {
				Send(task, cmd, len);
			}
		}
	}
    return 0;
}

#ifdef _USE_PROTOBUF_
int wServer::AsyncWorker(const google::protobuf::Message* msg, uint32_t solt, const std::vector<uint32_t>* blackslot) {
	if (Master()->WorkerNum() <= 1) {
		return 0;
	}
	if (solt == kMaxProcess) {	// 广播消息
		for (uint32_t i = 0; i < kMaxProcess; i++) {
			if (mMaster->Worker(i)->mPid == -1 || mMaster->Worker(i)->ChannelFD(0) == kFDUnknown) {
				continue;
			} else if (blackslot && std::find(blackslot->begin(), blackslot->end(), i) != blackslot->end()) {
				continue;
			}

			wTask *task = NULL;
			if (FindTaskBySocket(&task, mMaster->Worker(i)->Channel()) == 0) {
				Send(task, msg);
			}
	    }
	} else {
		if (mMaster->Worker(solt)->mPid != -1 && mMaster->Worker(solt)->ChannelFD(0) != kFDUnknown) {
			
			wTask *task = NULL;
			if (FindTaskBySocket(&task, mMaster->Worker(solt)->Channel()) == 0) {
				Send(task, msg);
			}
		}
	}
    return 0;
}
#endif

int wServer::SyncWorker(char *cmd, int len, uint32_t solt, const std::vector<uint32_t>* blackslot) {
	if (Master()->WorkerNum() <= 1) {
		return 0;
	}
	ssize_t ret;
	char buf[kPackageSize];
	if (solt == kMaxProcess) {	// 广播消息
		for (uint32_t i = 0; i < kMaxProcess; i++) {
			if (mMaster->Worker(i)->mPid == -1 || mMaster->Worker(i)->ChannelFD(0) == kFDUnknown) {
				continue;
			} else if (blackslot && std::find(blackslot->begin(), blackslot->end(), i) != blackslot->end()) {
				continue;
			}

			/* TODO: EAGAIN */
			wTask::Assertbuf(buf, cmd, len);
			mMaster->Worker(i)->Channel()->SendBytes(buf, sizeof(uint32_t) + sizeof(uint8_t) + len, &ret);
	    }
	} else {
		if (mMaster->Worker(solt)->mPid != -1 && mMaster->Worker(solt)->ChannelFD(0) != kFDUnknown) {

			/* TODO: EAGAIN */
			wTask::Assertbuf(buf, cmd, len);
			mMaster->Worker(solt)->Channel()->SendBytes(buf, sizeof(uint32_t) + sizeof(uint8_t) + len, &ret);
		}
	}
    return 0;
}

#ifdef _USE_PROTOBUF_
int wServer::SyncWorker(const google::protobuf::Message* msg, uint32_t solt, const std::vector<uint32_t>* blackslot) {
	if (Master()->WorkerNum() <= 1) {
		return 0;
	}
	ssize_t ret;
	char buf[kPackageSize];
	uint32_t len = sizeof(uint8_t) + sizeof(uint16_t) + msg->GetTypeName().size() + msg->ByteSize();
	if (solt == kMaxProcess) {	// 广播消息
		for (uint32_t i = 0; i < kMaxProcess; i++) {
			if (mMaster->Worker(i)->mPid == -1 || mMaster->Worker(i)->ChannelFD(0) == kFDUnknown) {
				continue;
			} else if (blackslot && std::find(blackslot->begin(), blackslot->end(), i) != blackslot->end()) {
				continue;
			}

			/* TODO: EAGAIN */
			wTask::Assertbuf(buf, msg);
			mMaster->Worker(i)->Channel()->SendBytes(buf, sizeof(uint32_t) + len, &ret);
	    }
	} else {
		if (mMaster->Worker(solt)->mPid != -1 && mMaster->Worker(solt)->ChannelFD(0) != kFDUnknown) {

			/* TODO: EAGAIN */
			wTask::Assertbuf(buf, msg);
			mMaster->Worker(solt)->Channel()->SendBytes(buf, sizeof(uint32_t) + len, &ret);
		}
	}
    return 0;
}
#endif

int wServer::AddListener(const std::string& ipaddr, uint16_t port, const std::string& protocol) {
    wSocket *socket = NULL;
    if (protocol == "UDP") {
		HNET_NEW(wUdpSocket(kStConnect), socket);	// udp无 listen socket
	} else if (protocol == "UNIX") {
		HNET_NEW(wUnixSocket(kStListen), socket);
    } else if(protocol == "TCP") {
    	HNET_NEW(wTcpSocket(kStListen), socket);
    } else if (protocol == "HTTP") {
    	HNET_NEW(wTcpSocket(kStListen, kSpHttp), socket);
    }
    
    if (!socket) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AddListener new() failed", "");
		return -1;
    }

    int ret = socket->Open();
	if (ret == -1) {
	    HNET_DELETE(socket);
	    HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AddListener Open() failed", "");
	    return ret;
	}

	ret = socket->Listen(ipaddr, port);
	if (ret == -1) {
	    HNET_DELETE(socket);
	    HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AddListener Listen() failed", "");
	    return ret;
	}

	if (socket->SP() == kSpUdp) {	// udp无listen socket
		socket->SS() = kSsConnected;
	} else {
		socket->SS() = kSsListened;
	}
	mListenSock.push_back(socket);
    return 0;
}

int wServer::InitEpoll() {
    int ret = epoll_create(kListenBacklog);
    if (ret == -1) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::InitEpoll epoll_create() failed", error::Strerror(errno).c_str());
		return ret;
    }
    mEpollFD = ret;
    return 0;
}

int wServer::Listener2Epoll(bool addpool) {
    for (std::vector<wSocket *>::iterator it = mListenSock.begin(); it != mListenSock.end(); it++) {
    	if (!addpool) {
    		bool oldtask = false;
        	for (std::vector<wTask*>::iterator im = mTaskPool.begin(); im != mTaskPool.end(); im++) {
        		if ((*im)->Socket() == *it) {
        			oldtask = true;
        			AddTask(*im, EPOLLIN, EPOLL_CTL_ADD, false);
        			break;
        		}
        	}

        	if (!oldtask) {
        		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Listener2Epoll AddTask() failed", error::Strerror(errno).c_str());
        		return -1;
        	} else {
        		continue;
        	}
    	}

    	wTask *ctask = NULL;
    	switch ((*it)->SP()) {
		case kSpTcp:
		    if (NewTcpTask(*it, &ctask) == -1) {
		    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Listener2Epoll NewTcpTask() failed", "");
		    	return -1;
		    }
		    break;
		case kSpUdp:
		    if (NewUdpTask(*it, &ctask) == -1) {
		    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Listener2Epoll NewUdpTask() failed", "");
		    	return -1;
		    }
		    break;
		case kSpUnix:
		    if (NewUnixTask(*it, &ctask) == -1) {
		    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Listener2Epoll NewUnixTask() failed", "");
		    	return -1;
		    }
		    break;
		case kSpHttp:
		    if (NewHttpTask(*it, &ctask) == -1) {
		    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Listener2Epoll NewHttpTask() failed", "");
		    	return -1;
		    }
		    break;
		default:
			HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Listener2Epoll () failed", "unknown sp");
			return -1;
		}

	    if (AddTask(ctask) == -1) {
			RemoveTask(ctask);
	    }
    }
    return 0;
}

int wServer::RemoveListener(bool delpool) {
    for (std::vector<wSocket*>::iterator it = mListenSock.begin(); it != mListenSock.end(); it++) {
    	for (std::vector<wTask*>::iterator im = mTaskPool.begin(); im != mTaskPool.end(); im++) {
    		if ((*im)->Socket() == *it) {
    			RemoveTask(*im, &im, delpool);
    			break;
    		}
    	}
    }
    return 0;
}

int wServer::Channel2Epoll(bool addpool) {
	for (uint32_t i = 0; i < kMaxProcess; i++) {
		if (mMaster != NULL && mMaster->Worker(i)->mPid == -1) {
			continue;
		}

		wChannelSocket *socket = mMaster->Worker(i)->Channel();
		if (socket) {
			if (i == mMaster->mSlot) {
				socket->FD() = (*socket)[1];	// 自身worker供读取描述符
			} else {
				socket->FD() = (*socket)[0];	// 其他worker供写入描述符
			}
			socket->SS() = kSsConnected;

			wTask *ctask;
			if (NewChannelTask(socket, &ctask) == -1) {
				HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::Channel2Epoll NewChannelTask() failed", "");
				return -1;
			}

			if (AddTask(ctask) == -1) {
				RemoveTask(ctask);
			}
		}
	}
    return 0;
}

int wServer::AddTask(wTask* task, int ev, int op, bool addpool) {
    // 方便异步发送
    task->SetServer(this);

    struct epoll_event evt;
    evt.events = ev | EPOLLERR | EPOLLHUP;
    evt.data.ptr = task;
    int ret = epoll_ctl(mEpollFD, op, task->Socket()->FD(), &evt);
    if (ret == -1) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::AddTask epoll_ctl() failed", error::Strerror(errno).c_str());
    	return ret;
    }

    if (addpool) {
    	return AddToTaskPool(task);
    }
    return ret;
}

int wServer::RemoveTask(wTask* task, std::vector<wTask*>::iterator* iter, bool delpool) {
    struct epoll_event evt;
    evt.events = 0;
    evt.data.ptr = NULL;
    int ret = epoll_ctl(mEpollFD, EPOLL_CTL_DEL, task->Socket()->FD(), &evt);
    if (ret == -1) {
    	HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::RemoveTask epoll_ctl() failed", error::Strerror(errno).c_str());
    }

    if (delpool) {
        std::vector<wTask*>::iterator it = RemoveTaskPool(task);
        if (iter) {
        	*iter = it;
        }
    }
    return ret;
}

int wServer::CleanTask() {
    CleanTaskPool(mTaskPool);

    int ret = close(mEpollFD);
    if (ret == -1) {
		HNET_ERROR(soft::GetLogPath(), "%s : %s", "wServer::CleanTask close() failed", error::Strerror(errno).c_str());
    }

    mEpollFD = kFDUnknown;
    return ret;
}

int wServer::AddToTaskPool(wTask* task) {
    mTaskPool.push_back(task);
    return 0;
}

std::vector<wTask*>::iterator wServer::RemoveTaskPool(wTask* task) {
    std::vector<wTask*>::iterator it = std::find(mTaskPool.begin(), mTaskPool.end(), task);
    if (it != mTaskPool.end()) {
    	HNET_DELETE(task);
        it = mTaskPool.erase(it);
    }
    return it;
}

int wServer::CleanTaskPool(std::vector<wTask*> pool) {
	for (std::vector<wTask*>::iterator it = pool.begin(); it != pool.end(); it++) {
	    HNET_DELETE(*it);
	}
    pool.clear();
    return 0;
}

int wServer::CleanListenSock() {
	for (std::vector<wSocket*>::iterator it = mListenSock.begin(); it != mListenSock.end(); it++) {
		HNET_DELETE(*it);
	}
	return 0;
}

int wServer::DeleteAcceptFile() {
    if (kAcceptStuff == 0 && mShm) {
    	mShm->Destroy();
    }
    mEnv->DeleteFile(soft::GetAcceptPath());
	return 0;
}

void wServer::CheckTick() {
	mTick = soft::TimeUsec() - mLatestTm;
	if (mTick < 10*1000) {
		return;
	}
	mLatestTm += mTick;

	if (mHeartbeatTurn && mHeartbeatTimer.CheckTimer(mTick/1000)) {
		CheckHeartBeat();
	}
}

void wServer::CheckHeartBeat() {
	for (std::vector<wTask*>::iterator it = mTaskPool.begin(); it != mTaskPool.end();) {
		if ((*it)->Socket()->ST() == kStConnect && ((*it)->Socket()->SP() == kSpTcp || (*it)->Socket()->SP() == kSpUnix)) {
			if ((*it)->Socket()->SS() == kSsUnconnect) {	// 断线连接
				(*it)->DisConnect();
				RemoveTask(*it, &it);
				continue;
			} else {	// 心跳检测
				(*it)->HeartbeatSend();	// 发送心跳
				if ((*it)->HeartbeatOut()) {	// 心跳超限
					
					(*it)->DisConnect();
					RemoveTask(*it, &it);
					continue;
				}
			}
		}
		it++;
	}
}

}	// namespace hnet

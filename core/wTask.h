
/**
 * Copyright (C) Anny Wang.
 * Copyright (C) Hupu, Inc.
 */

#ifndef _W_TASK_H_
#define _W_TASK_H_

#include "wCore.h"
#include "wNoncopyable.h"
#include "wEvent.h"
#include "wServer.h"
#include "wMultiClient.h"
#include "wLogger.h"

#ifdef _USE_PROTOBUF_
#include <google/protobuf/message.h>
#endif

namespace hnet {

// 消息绑定函数参数类型
struct Request_t {
	char* mBuf;
	uint32_t mLen;
	Request_t(char buf[], uint32_t len) : mBuf(buf), mLen(len) { }
};

class wSocket;

class wTask : private wNoncopyable {
public:
    wTask(wSocket *socket, int32_t type = 0);
    void ResetBuffer();
    virtual ~wTask();

    virtual int Connect() {
        return 0;
    }
    
    virtual int DisConnect() {
        return 0;
    }

    virtual int ReConnect() {
        return 0;
    }

    // 处理接受到数据，转发给业务处理函数 Handlemsg 处理。每条消息大小[1b,512k]
    // size = -1 对端发生错误|稍后重试
    // size = 0  对端关闭
    // size > 0  接受字符
    virtual int TaskRecv(ssize_t *size);

    // 处理接受到数据
    // size = -1 对端发生错误|稍后重试|对端关闭
    // size >= 0 发送字符
    virtual int TaskSend(ssize_t *size);

    // 解析消息
    virtual int Handlemsg(char cmd[], uint32_t len);

    // 异步发送：将待发送客户端消息写入buf，等待TaskSend发送
    int Send2Buf(char cmd[], size_t len);
#ifdef _USE_PROTOBUF_
    int Send2Buf(const google::protobuf::Message* msg);
#endif

    // 同步发送确切长度消息
    // size = -1 对端发生错误|稍后重试|对端关闭
    // size >= 0 发送字符
    int SyncSend(char cmd[], size_t len, ssize_t *size);
#ifdef _USE_PROTOBUF_
    int SyncSend(const google::protobuf::Message* msg, ssize_t *size);
#endif

    // SyncSend的异步发送版本
    int AsyncSend(char cmd[], size_t len);
#ifdef _USE_PROTOBUF_
    int AsyncSend(const google::protobuf::Message* msg);
#endif

    // 同步接受一条合法的、非心跳消息 或 接受一条指定长度合法的、非心跳消息（该消息必须为一条即将接受的消息）
    // 调用者：保证此sock未加入epoll中，否则出现事件竞争；且该sock需为阻塞的fd；另外也要确保buf有足够长的空间接受自此同步消息
    // size = -1 对端发生错误|稍后重试
    // size = 0  对端关闭
    // size > 0  接受字符
    int SyncRecv(char cmd[], ssize_t *size, size_t msglen = 0, uint32_t timeout = 30);
#ifdef _USE_PROTOBUF_
    int SyncRecv(google::protobuf::Message* msg, ssize_t *size, size_t msglen = 0, uint32_t timeout = 30);
#endif

    // 同步广播其他worker进程
    int SyncWorker(char cmd[], size_t len);
#ifdef _USE_PROTOBUF_
    int SyncWorker(const google::protobuf::Message* msg);
#endif

    // 异步广播其他worker进程
    int AsyncWorker(char cmd[], size_t len);
#ifdef _USE_PROTOBUF_
    int AsyncWorker(const google::protobuf::Message* msg);
#endif

    virtual int HttpGet(const std::string& url, const std::map<std::string, std::string>& header, std::string& res, uint32_t timeout = 30) {
        HNET_ERROR(soft::GetLogPath(), "%s : %s", "wSocket::HttpGet () failed", "method should be inherit");
        return -1;
    }

    virtual int HttpPost(const std::string& url, const std::map<std::string, std::string>& data, const std::map<std::string, std::string>& header, std::string& res, uint32_t timeout = 30) {
        HNET_ERROR(soft::GetLogPath(), "%s : %s", "wSocket::HttpPost () failed", "method should be inherit");
        return -1;
    }

    static void Assertbuf(char buf[], const char cmd[], size_t len);
#ifdef _USE_PROTOBUF_
    static void Assertbuf(char buf[], const google::protobuf::Message* msg);
#endif

    int HeartbeatSend();

    inline bool HeartbeatOut() {
        return mHeartbeat > kHeartbeat;
    }

    inline void HeartbeatReset() {
        mHeartbeat = 0;
    }

    // 添加epoll可写事件
    int Output();

    // 设置服务端对象（方便异步发送）
    inline void SetServer(wServer* server) {
    	mSCType = 0;
    	mServer = server;
    }
    template<typename T = wServer*>
    inline T& Server() { return reinterpret_cast<T&>(mServer);}

    // 设置客户端对象（方便异步发送）
    inline void SetClient(wMultiClient* client) {
    	mSCType = 1;
    	mClient = client;
    }
    template<typename T = wMultiClient*>
    inline T& Client() { return reinterpret_cast<T&>(mClient);}

    template<typename T = wConfig*>
    inline T Config() {
    	T config = NULL;
    	if (mSCType == 0 && mServer != NULL) {
    		config = mServer->Config<T>();
    	} else if (mSCType == 1 && mClient != NULL) {
    		config = mClient->Config<T>();
    	}
    	return config;
    }

    inline size_t SendLen() { return mSendLen;}
    inline int32_t Type() { return mType;}
    inline wSocket* Socket() { return mSocket;}
    
protected:
    // command消息路由器
    template<typename T = wTask>
    void On(int8_t cmd, int8_t para, int (T::*func)(struct Request_t *argv), T* target) {
    	mEventCmd.On(CmdId(cmd, para), std::bind(func, target, std::placeholders::_1));
    }
    wEvent<uint16_t, std::function<int(struct Request_t *argv)>, struct Request_t*> mEventCmd;

    // protobuf消息路由器
    template<typename T = wTask>
    void On(const std::string& pbname, int (T::*func)(struct Request_t *argv), T* target) {
    	mEventPb.On(pbname, std::bind(func, target, std::placeholders::_1));
    }
    wEvent<std::string, std::function<int(struct Request_t *argv)>, struct Request_t*> mEventPb;

    int32_t mType;
    wSocket *mSocket;

    uint8_t mHeartbeat;

    char mTempBuff[kPackageSize];    // 同步发送、接受消息缓冲
    char mRecvBuff[kPackageSize];    // 异步接受消息缓冲
    char mSendBuff[kPackageSize];    // 异步发送消息缓冲
    
    char *mRecvRead;
    char *mRecvWrite;
    size_t mRecvLen;  // 已接受数据长度

    char *mSendRead;
    char *mSendWrite;
    size_t mSendLen;  // 可发送数据长度

    wServer* mServer;
    wMultiClient* mClient;

    // 0为server，1为client
    uint8_t mSCType;
};

}	// namespace hnet

#endif

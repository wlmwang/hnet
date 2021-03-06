
/**
 * Copyright (C) Anny Wang.
 * Copyright (C) Hupu, Inc.
 */

#ifndef _W_MISC_H_
#define _W_MISC_H_

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <net/if.h>
#include <time.h>
#include <map>
#include <vector>
#include "wCore.h"
#include "wSlice.h"
#include "wCommand.h"
#include "wSignal.h"

namespace hnet {
namespace coding {

void EncodeFixed8(char* dst, uint8_t value);
void EncodeFixed16(char* dst, uint16_t value);
void EncodeFixed32(char* dst, uint32_t value);
void EncodeFixed64(char* dst, uint64_t value);

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);

inline uint8_t DecodeFixed8(const char* ptr) {
    if (kLittleEndian) {
        uint8_t result;
        memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
        return result;
    } else {
        return static_cast<uint8_t>(static_cast<unsigned char>(ptr[0]));
    }
}

inline uint16_t DecodeFixed16(const char* ptr) {
    if (kLittleEndian) {
        uint16_t result;
        memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
        return result;
    } else {
        return ((static_cast<uint32_t>(static_cast<unsigned char>(ptr[0])))
            | (static_cast<uint32_t>(static_cast<unsigned char>(ptr[1])) << 8));
    }
}

inline uint32_t DecodeFixed32(const char* ptr) {
    if (kLittleEndian) {
        uint32_t result;
        memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
        return result;
    } else {
        return ((static_cast<uint32_t>(static_cast<unsigned char>(ptr[0])))
            | (static_cast<uint32_t>(static_cast<unsigned char>(ptr[1])) << 8)
            | (static_cast<uint32_t>(static_cast<unsigned char>(ptr[2])) << 16)
            | (static_cast<uint32_t>(static_cast<unsigned char>(ptr[3])) << 24));
    }
}

inline uint64_t DecodeFixed64(const char* ptr) {
    if (kLittleEndian) {
        uint64_t result;
        memcpy(&result, ptr, sizeof(result));  // gcc optimizes this to a plain load
        return result;
    } else {
        uint64_t lo = DecodeFixed32(ptr);
        uint64_t hi = DecodeFixed32(ptr + 4);
        return (hi << 32) | lo;
    }
}

}   // namespace coding

namespace logging {

// 将整型按字符方式追加到str字符串结尾
void AppendNumberTo(std::string* str, uint64_t num);

// 在str后面添加value字符，并将value不可见字符转化为16进制表示的字符
void AppendEscapedStringTo(std::string* str, const wSlice& value);

// 返回整型字符串
std::string NumberToString(uint64_t num);

// 消费转化in字符串前缀的十进制数字符串，返回是否转化正确
bool DecimalStringToNumber(const std::string& in, uint64_t* val, uint8_t *width = NULL);

// 返回不可见字符被转化为16进制表示的字符的value字符串
std::string EscapeString(const wSlice& value);

}   // namespace logging

namespace misc {

// 哈希值 murmur hash类似算法
uint32_t Hash(const char* data, size_t n, uint32_t seed);

// 最大公约数
uint64_t Ngcd(uint64_t arr[], size_t n);

// 复制字符串
char *Cpystrn(char *dst, const char *src, size_t n);

// 转化成小写
void Strlow(char *dst, const char *src, size_t n);

// 转化成大写
void Strupper(char *dst, const char *src, size_t n);

int32_t Strcmp(const std::string& str1, const std::string& str2, size_t n);

// 查找字符串位置
int32_t Strpos(const std::string& haystack, const std::string& needle);

// 分隔字符串
std::vector<std::string> SplitString(const std::string& src, const std::string& delim);

// unix时间戳转化tm格式
int FastUnixSec2Tm(time_t unix_sec, struct tm* tm, int time_zone = 8);

// 网卡获取地址
int GetIpList(std::vector<unsigned int>& iplist);
unsigned int GetIpByIF(const char* ifname);

// 切换进程工作目录
// 行成功则返回0, 失败返回-1, errno 为错误代码
int SetBinPath(std::string bin_path = "", std::string self = "/proc/self/exe");

// 错误返回NULL
inline const char* IP2Text(uint32_t ip) {
    struct in_addr in;
    in.s_addr = ip;
    return inet_ntoa(in);
}

// 正确返回32无符号，错误返回：INADDR_NONE
inline in_addr_t Text2IP(const char* cp) {
    return inet_addr(cp);    // typedef uint32_t in_addr_t
}

inline void GetTimeofday(struct timeval* pVal) {
    pVal != NULL && gettimeofday(pVal, NULL);
}

inline int64_t GetTimeofday() {
    struct timeval tv;
    GetTimeofday(&tv);
    return (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec;
}

inline uint8_t AlignMent() {
    return sizeof(unsigned long);
}

inline uint32_t Align(uint32_t d, uint32_t a) {
    return (((d) + (a - 1)) & ~(a - 1));
}

inline u_char* AlignPtr(char* p, char* a) {
    return (u_char *) (((uintptr_t) (p) + ((uintptr_t) a - 1)) & ~((uintptr_t) a - 1));
}

}   // namespace misc

namespace error {

// 初始化系统错误字符
void StrerrorInit();

// 获取错误字符
const std::string& Strerror(int32_t err);

}	// namespace error

namespace http {

struct StatusCode_t {
    const char  code[4];
    const char  status[64];
};

void StatusCodeInit();

const std::string& Status(const std::string& code);

std::string UrlEncode(const std::string& str);
std::string UrlDecode(const std::string& str);

}   // namespace http

namespace soft {

int64_t TimeUpdate();
int64_t TimeUsec();
time_t TimeUnix();

// 设置运行用户
uid_t SetUser(uid_t uid);
gid_t SetGroup(gid_t gid);

// 设置版本信息
const std::string& SetSoftName(const std::string& name);
const std::string& SetSoftVer(const std::string& ver);

// 设置路径
void SetRuntimePath(const std::string& path);
void SetLogdirPath(const std::string& path);

// 设置文件名
void SetAcceptFilename(const std::string& filename);
void SetLockFilename(const std::string& filename);
void SetPidFilename(const std::string& filename);
void SetLogFilename(const std::string& filename);

uid_t GetUser();
gid_t GetGroup();
const std::string& GetSoftName();
const std::string& GetSoftVer();

const std::string& GetRuntimePath();
const std::string& GetLogdirPath();

const std::string& GetLockPath(bool fullpath = true);
const std::string& GetPidPath(bool fullpath = true);
const std::string& GetLogPath(bool fullpath = true);
const std::string& GetAcceptPath(bool fullpath = true);

}	// namespace soft

}   // namespace hnet

#endif

// ============================================================
// health-probe —— 业务级健康检查探针
//
// 用途：替代 docker-compose 里形同虚设的 `nc -z 127.0.0.1 8000`。
//
// 为什么不用 nc -z：
//   nc 只验证 TCP 能握手。下列真实故障全部会被误判为 healthy：
//     - epoll / 线程池全部阻塞：端口仍 LISTEN，但没人 accept
//     - MySQL 连接池耗尽或网络分区：端口在，业务全失败
//   这两种情况恰恰是最需要告警的。
//
// 探测原理：
//   按 packdef.h 的自定义协议发一个 LOGIN_RQ（使用一个不可能存在的手机号），
//   服务端 clogic.cpp::LoginRq 的行为决定了本探针能否判定存活：
//     · 正常            -> SelectMysql 成功，查无此人，SendData 回 STRU_LOGIN_RS
//     · MySQL 不可用    -> SelectMysql 失败直接 return，**不回包**
//     · 线程池/epoll 卡死 -> 没人处理，**不回包**
//   因此「收到结构正确的 LOGIN_RS」同时证明了：
//   进程活着 + epoll 在收发 + 业务逻辑执行 + MySQL 查询通路正常。
//   这是真正的业务级健康检查。
//
// 副作用评估：
//   该手机号不存在时不会写库、不会建立会话。每 10 秒一次，MySQL 侧仅一次
//   带索引的 SELECT（tel 列有索引，见 schema.sql），开销可忽略。
//
// 编译： g++ -std=c++11 -O2 -o health-probe health-probe.cpp
// 用法： health-probe [host] [port] [timeout_sec]
//        退出码 0 = 健康，非 0 = 不健康（ compose healthcheck 据此判定）
// ============================================================
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// 与 deploy/verify.py、packdef.h 保持一致
static const int LOGIN_RQ = 10002;   // BASE(10000) + 2
static const int LOGIN_RS = 10003;   // BASE(10000) + 3
static const int TEL_LEN  = 40;
static const int PW_LEN   = 40;
static const int RS_BODY  = 72;      // type(4) + userid(4) + result(4) + name(60)

// 一个不可能被真实注册占用的号码；即便被占用，回包依然会发生，不影响判定。
static const char* PROBE_TEL = "0000000000";

namespace {

void pack_i32(char* p, int v) {
    // 协议是小端序（见 block_epoll_net.cpp / verify.py 的 struct "<i"）
    unsigned int u = static_cast<unsigned int>(v);
    p[0] = static_cast<char>(u & 0xFF);
    p[1] = static_cast<char>((u >> 8) & 0xFF);
    p[2] = static_cast<char>((u >> 16) & 0xFF);
    p[3] = static_cast<char>((u >> 24) & 0xFF);
}

int unpack_i32(const char* p) {
    unsigned int u = 0;
    u |= static_cast<unsigned char>(p[0]);
    u |= static_cast<unsigned char>(p[1]) << 8;
    u |= static_cast<unsigned char>(p[2]) << 16;
    u |= static_cast<unsigned char>(p[3]) << 24;
    return static_cast<int>(u);
}

bool write_exact_helper(int fd, const char* buf, int n) {
    int sent = 0;
    while (sent < n) {
        ssize_t w = send(fd, buf + sent, static_cast<size_t>(n - sent), MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        sent += static_cast<int>(w);
    }
    return true;
}

bool read_exact(int fd, char* buf, int n, int timeout_sec) {
    struct timeval tv;
    tv.tv_sec  = timeout_sec;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0) {
        return false;
    }
    int got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, static_cast<size_t>(n - got), 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return false;  // 超时
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;  // 对端关闭连接
        got += static_cast<int>(r);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* host    = (argc > 1) ? argv[1] : "127.0.0.1";
    int         port    = (argc > 2) ? atoi(argv[2]) : 8000;
    int         timeout = (argc > 3) ? atoi(argv[3]) : 5;
    if (port <= 0)    port = 8000;
    if (timeout <= 0) timeout = 5;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "health-probe: socket() failed: %s\n", std::strerror(errno));
        return 2;
    }

    // 收发双端都加超时，避免 healthcheck 挂死导致 docker inspect 永远 pending
    struct timeval tv;
    tv.tv_sec = timeout; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        std::fprintf(stderr, "health-probe: 非法地址 %s\n", host);
        ::close(fd);
        return 2;
    }

    // 用 select 实现 connect 超时：直接 connect 遇到 SYN 丢弃会阻塞很久
    {
        int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof addr);
        if (rc != 0 && errno != EINPROGRESS) {
            std::fprintf(stderr, "health-probe: connect %s:%d failed: %s\n",
                         host, port, std::strerror(errno));
            ::close(fd);
            return 3;
        }
        if (rc != 0) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval ctv;
            ctv.tv_sec = timeout; ctv.tv_usec = 0;
            int sel = ::select(fd + 1, NULL, &wfds, NULL, &ctv);
            if (sel <= 0) {
                std::fprintf(stderr, "health-probe: connect %s:%d 超时\n", host, port);
                ::close(fd);
                return 3;
            }
            int err = 0;
            socklen_t len = sizeof err;
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                std::fprintf(stderr, "health-probe: connect %s:%d 错误: %s\n",
                             host, port, std::strerror(err));
                ::close(fd);
                return 3;
            }
        }
        ::fcntl(fd, F_SETFL, flags);  // 恢复阻塞模式，配合 SO_RCVTIMEO
    }

    // 组包：[int32 包体长度][包体]，长度不含前缀（与 verify.py::pkt 一致）
    const int body_len = 4 + TEL_LEN + PW_LEN;   // 84
    char req[4 + body_len];
    std::memset(req, 0, sizeof req);
    pack_i32(req, body_len);
    pack_i32(req + 4, LOGIN_RQ);
    std::snprintf(req + 8, TEL_LEN, "%s", PROBE_TEL);
    // 密码留空即可：本探针只关心「有没有回包」，不关心登录结果

    if (!write_exact_helper(fd, req, static_cast<int>(sizeof req))) {
        std::fprintf(stderr, "health-probe: 发送请求失败\n");
        ::close(fd);
        return 4;
    }

    // 读响应：先 4 字节长度头，再包体
    char hdr[4];
    if (!read_exact(fd, hdr, 4, timeout)) {
        std::fprintf(stderr, "health-probe: 未收到响应头（服务未处理 / MySQL 不可用 / 线程池阻塞）\n");
        ::close(fd);
        return 5;
    }
    int n = unpack_i32(hdr);
    if (n != RS_BODY) {
        std::fprintf(stderr, "health-probe: 包体长度异常 %d（期望 %d）\n", n, RS_BODY);
        ::close(fd);
        return 6;
    }

    char body[RS_BODY];
    if (!read_exact(fd, body, RS_BODY, timeout)) {
        std::fprintf(stderr, "health-probe: 响应包体读取不完整\n");
        ::close(fd);
        return 7;
    }

    int type = unpack_i32(body);
    if (type != LOGIN_RS) {
        std::fprintf(stderr, "health-probe: 响应类型异常 %d（期望 LOGIN_RS=%d）\n", type, LOGIN_RS);
        ::close(fd);
        return 8;
    }
    int result = unpack_i32(body + 8);

    // result: 0=用户不存在 1=密码错误 2=成功 3=成功但邮箱为空
    // 只要服务端能走完 LoginRq 并回包，链路就是通的。
    std::printf("ok type=%d result=%d\n", type, result);
    ::close(fd);
    return 0;
}

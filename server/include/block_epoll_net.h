#ifndef BLOCK_EPOLL_NET_H
#define BLOCK_EPOLL_NET_H

#include <stdlib.h>
#include <stdio.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include<iostream>
#include<vector>
#include<list>
#include<map>

#include"Thread_pool.h"

#define MAX_EVENTS 4096
using namespace  std;

//阻塞IO epoll


class Block_Epoll_Net;

//数据缓存
struct DataBuffer
{
    DataBuffer( Block_Epoll_Net* _pNet,  int _sock , char* _buf , int _nlen )
        :pNet(_pNet),sockfd(_sock),buf(_buf),nlen(_nlen){}

    Block_Epoll_Net* pNet;
    int sockfd;
    char* buf;
    int nlen;

};

template<class K , class V>
struct MyMap
{
public:
    MyMap(){
        pthread_mutex_init(&m_lock , NULL);
    }
    map<K , V> m_map;

    //获取的结果 找不到 如果是对象 v  如果是指针 应该是NULL 规定该函数使用时, 调用是确保一定有
    V find( K k )
    {
        pthread_mutex_lock(&m_lock );
        //【安全修复-P0：map 污染】原代码：V v = m_map[k];
        // 修改原因: std::map 的 operator[] 在 key 不存在时会【插入】一个默认值。
        //           对 MyMap<int,UserInfo*> 而言，任何人对未登录的 userid 调一次 find，
        //           map 里就多出一条 {id -> NULL}；之后 JoinRoomRq 的 IsExist 检查被
        //           这条假数据骗过，find 返回 NULL 后直接解引用 -> 空指针崩溃。
        //           这是唯一一条"未认证、两个普通协议包即可远程打崩进程"的路径。
        // 修改效果: 改用 std::map::find 迭代器查找，key 不存在时不插入任何数据，
        //           返回 V()（指针类型即 NULL）。未登录 userid 查询不再污染 map，
        //           调用方拿到的 NULL 即代表"不存在"，与既有 NULL 判断逻辑兼容。
        // 原代码: V v = m_map[k];
        V v = V();
        typename std::map<K, V>::iterator it = m_map.find( k );
        if( it != m_map.end() )
            v = it->second;
		pthread_mutex_unlock(&m_lock );
        return v;
    }
    //【安全修复-P1：锁内条件删除】供断开清理使用：在持有锁的前提下
    //  删除所有满足条件的键值对，返回被删除的 key 列表。
    //  修改原因: 断开清理需要"按条件遍历并删除"，若调用方直接摸 m_map 会绕过锁。
    //  修改效果: 条件判断在锁内完成，删除原子化，调用方拿到结果再做业务处理。
    template<class Pred>
    list<K> erase_if( Pred pred )
    {
        list<K> lstRemoved;
        pthread_mutex_lock(&m_lock );
        for( typename std::map<K, V>::iterator it = m_map.begin(); it != m_map.end(); )
        {
            if( pred( it->first, it->second ) )
            {
                lstRemoved.push_back( it->first );
                m_map.erase( it++ );
            }
            else
            {
                ++it;
            }
        }
        pthread_mutex_unlock(&m_lock );
        return lstRemoved;
    }
    //【安全修复-P1：锁内遍历修改】在持有锁的前提下对每个 value 执行 f(key, value)。
    //  修改原因: 房间成员列表的"拷贝->修改->写回"两步之间存在竞态窗口，
    //            并发加入/离开同一房间时后写覆盖前写会丢成员；
    //            断开清理也需要在锁内从各房间列表移除掉线用户。
    //  修改效果: f 直接操作锁内的 value 引用，修改原子化，不再有写回覆盖窗口。
    template<class F>
    void for_each_locked( F f )
    {
        pthread_mutex_lock(&m_lock );
        for( typename std::map<K, V>::iterator it = m_map.begin(); it != m_map.end(); ++it )
            f( it->first, it->second );
        pthread_mutex_unlock(&m_lock );
    }
    void insert( K k , V v)
    {
        pthread_mutex_lock(&m_lock );
        m_map[k] = v;
        pthread_mutex_unlock(&m_lock );
    }
    void erase(K k )
    {
        pthread_mutex_lock(&m_lock );
        m_map.erase(k);
        pthread_mutex_unlock(&m_lock );
    }
    bool IsExist( K k )
    {
        bool flag = false;
        pthread_mutex_lock(&m_lock );
        if( m_map.count(k) > 0 )
            flag = true;
        pthread_mutex_unlock(&m_lock );
        return flag;
    }
private:
    pthread_mutex_t m_lock;
    //**map<K , V> m_map;
};


//事件结构
struct myevent_s {

    int fd; //cfd listenfd
    int epoll_fd; //epoll_create 句柄
    int events; //EPOLLIN EPLLOUT
    int status;/* status:1表示在监听事件中，0表示不在 */
    Block_Epoll_Net* pNet;

    myevent_s(Block_Epoll_Net* _pNet)
    {
        this->pNet = _pNet;
    }
    void eventset(int fd ,int efd/*epoll_create返回的句柄*/)
    {
        this->fd = fd;
        this->events = 0;
        this->status = 0;
        epoll_fd = efd;
    }
    //上监听树
    void eventadd( int events)
    {
        struct epoll_event epv = {0, {0}};
        int op;
        epv.data.ptr = this;
        epv.events = this->events = events;
        if (this->status == 1) {
            op = EPOLL_CTL_MOD;
        }
        else {
            op = EPOLL_CTL_ADD;
            this->status = 1;
        }
        int ret = epoll_ctl(epoll_fd, op, this->fd, &epv);
        if (ret < 0)
        {
            //【修复-P1：epoll_ctl 失败静默吞掉】
            // 修改原因: 原代码此处为空 if{}，epoll_ctl 失败（fd 已关闭/系统 fd 耗尽等）
            //           完全无感知，连接"看似正常"实际永远收不到事件。
            // 修改效果: 失败时打印 fd 与 errno，运维可用日志定位问题；不改变控制流。
            printf("[myevent_s::eventadd] epoll_ctl failed op=%d fd=%d errno=%d(%s)\n",
                   op, this->fd, errno, strerror(errno));
            fflush(stdout);
        }

        return;
    }
    //下监听树
    void eventdel()
    {
        struct epoll_event epv = {0, {0}};
        if (this->status != 1)
            return;
        epv.data.ptr = this;
        this->status = 0;
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, this->fd, &epv);
        return;
    }
};


class Block_Epoll_Net
{
public:
    //【修复-P1：成员未初始化】构造函数补齐回调指针初始化
    // 修改原因: 原构造函数为空，m_close_callback 为未初始化的野指针，
    //           若上层忘记 setCloseCallback 就 close 连接，调用野指针直接崩溃。
    // 修改效果: 默认 NULL，回调调用点判空后跳过，未接线时行为与旧版一致。
    // 原代码: Block_Epoll_Net(){}
    //【安全修复-P0 配套：全部成员就地初始化】
    // 修改原因: 上层 TcpKernel::Open 在 DB 失败时提前 return，InitNet 可能
    //           从未执行，此时 m_listenfd/m_epoll_fd/m_threadpool/m_listenEv
    //           均为随机值，EventLoop 使用即未定义行为。
    // 修改效果: 构造时统一置 -1/NULL，任何"未初始化即使用"路径都可判别。
    // 原代码: Block_Epoll_Net(){ m_close_callback = NULL; }
    Block_Epoll_Net()
        : m_recv_callback( NULL ),
          m_close_callback( NULL ),
          m_listenEv( NULL ),
          m_listenfd( -1 ),
          m_epoll_fd( -1 ),
          m_threadpool( NULL )
    {}
    ~Block_Epoll_Net(){}
    //由于服务器 不会主动关闭, 没有写回收
public:

    //初始化 采用回调的方式, 解决数据接收处理
    bool InitNet(int port , void (*recv_callback)( int , char* , int ));
    //epoll事件循环
    void EventLoop();
    //发送数据
    int SendData(int fd, char* szbuf , int nlen );

    //【修复-P1：断开无业务层清理】关闭回调接线接口
    // 修改原因: 连接断开（错误路径）时网络层只 close+erase 事件，
    //           业务层 m_mapIDToUserInfo / 房间成员列表永不清真理——
    //           UserInfo* 泄漏、死 fd 残留，fd 被新连接复用后音视频帧会串线到无关连接。
    // 修改效果: 连接关闭时回调上层（TcpKernel -> CLogic::onClientClosed），
    //           由业务层按 fd 清理在线用户与房间成员。未接线时无副作用。
    void setCloseCallback( void (*close_callback)(int fd) )
    {
        m_close_callback = close_callback;
    }
    //【修复-P1：主动断开接口——评估后未提供】
    // 修改说明: 曾计划提供 CloseClient(fd) 供业务层强制下线，评估后放弃——
    //           事件结构 ev 被 recv_task 线程持有，业务层直接 delete 会与
    //           recv_task 错误清理路径构成 double-free。
    //           强制下线改为业务层直接 ::close(fd)：在途 recv 立即返回错误，
    //           由 recv_task 既有错误路径统一做 下树/close/erase/回收，单一清理方，无竞态。

    //socket 设置
    static void setNonBlockFd( int fd );
    static void setRecvBufSize(int fd);
    static void setSendBufSize(int fd);
    static void setNoDelay(int fd);
private:
    //接收处理回调函数
    void (*m_recv_callback)( int , char* , int );
    //【修复-P1：断开无业务层清理】连接关闭回调
    void (*m_close_callback)( int fd );
    //初始线程池
    bool InitThreadPool();
    /*接收数据和处理在两个线程完成 避免相互影响*/
    //线程函数 处理数据包
    static void *Buffer_Deal(void *arg);
    //线程函数 接收数据
    static void *recv_task(void *arg);

    //epoll事件处理
    void accept_event();
    void recv_event(myevent_s *ev); //接收: 事件到来recv_event --> 接收数据 recv_task -> 处理 Buffer_Deal
    void epollout_event(myevent_s *ev);
    //监听套接字对应的是事件
    myevent_s * m_listenEv;
    //监听套接字
    int m_listenfd;
    // epoll_create 句柄
    int m_epoll_fd;

    //每一个套接字 对应一个事件结构
    /*map*/MyMap< int , myevent_s*> m_mapSockfdToEvent;
    /* 事件循环使用 */
    struct epoll_event events[MAX_EVENTS+1];
    //线程池相关
    thread_pool *m_threadpool;


};

#endif // BLOCK_EPOLL_NET_H

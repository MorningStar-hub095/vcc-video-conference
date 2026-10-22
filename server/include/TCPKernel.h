#ifndef _TCPKERNEL_H
#define _TCPKERNEL_H


#include"block_epoll_net.h"
#include "Mysql.h"



//类成员函数指针 , 用于定义协议映射表
class TcpKernel;
typedef int sock_fd;


#define NetPackMap(a)  TcpKernel::GetInstance()->m_NetPackMap[ a - DEF_PACK_BASE ]

class CLogic;
typedef void (CLogic::*PFUN)(sock_fd,char*,int nlen);

class TcpKernel
{
public:
    //单例模式
    static TcpKernel* GetInstance();

    //开启核心服务
    int Open(int port);
    //初始化随机数
    void initRand();
    //设置协议映射
    void setNetPackMap();
    //关闭核心服务
    void Close();
    //处理网络接收
    static void DealData(sock_fd clientfd, char*szbuf, int nlen);
    //【修复-P1：断开无业务层清理】连接关闭回调（静态，转调 CLogic::onClientClosed）
    // 修改原因: 网络层断开时业务层永不清理，UserInfo 泄漏、fd 复用后帧串线。
    // 修改效果: Block_Epoll_Net 关闭 fd 前回调本函数完成业务层清理。
    // 原代码: （无此声明）
    static void OnClientClosed(int fd);
    //事件循环
    void EventLoop();
    //发送数据
    void SendData( sock_fd clientfd, char*szbuf, int nlen );
    //注冊 / 登錄等协议包的实现已迁移至 CLogic（见 server/src/clogic.cpp），
    //TcpKernel 只负责网络收发与按包类型分发，不再声明各业务处理函数。
    //本类原先的 RegisterRq / LoginRq 声明已随实现一并移除（会误导读者以为
    //TcpKernel 仍处理注册登录，实际 NetPackMap 映射的都是 CLogic 的成员函数）。


private:
    TcpKernel();
    ~TcpKernel();
    //数据库
    CMysql * m_sql;
    //网络
    Block_Epoll_Net * m_tcp;
    //协议映射表
    PFUN m_NetPackMap[DEF_PACK_COUNT];
//    //STL map不是線程安全的，需要加鎖避免添加移除元素出現問題
//    MyMap< int , UserInfo*> m_mapIDToUserInfo;
//    MyMap< int , list<int>> m_mapIDToRoomid;

    CLogic* m_logic;
    friend class CLogic;
};

#endif

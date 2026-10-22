#ifndef CLOGIC_H
#define CLOGIC_H

#include"TCPKernel.h"
#include <map>
#include <string>
#include <ctime>

class CLogic
{
public:
    CLogic( TcpKernel* pkernel )
    {
        m_pKernel = pkernel;
        m_sql = pkernel->m_sql;
        m_tcp = pkernel->m_tcp;
        //【P2修复：房间成员列表非原子读改写】初始化房间列表互斥锁
        // 修改原因: m_mapIDToRoomid 的 value(list<int>) 在 JoinRoom/LeaveRoom 中
        //           走「find 拷贝 -> 修改 -> insert 写回」三步，各步分别加锁但整体不原子，
        //           并发进出房间会互相覆盖（lost update）。
        // 修改效果: 该三步序列整体由 m_roomLock 串行化，房间成员不再丢更新。
        pthread_mutex_init( &m_roomLock , NULL );
        //【安全加固-P1：登录防暴力破解】初始化尝试记录锁
        pthread_mutex_init( &m_loginLock , NULL );
    }
public:
    //设置协议映射
    void setNetPackMap();
    /************** 发送数据*********************/
    void SendData( sock_fd clientfd, char*szbuf, int nlen )
    {
        m_pKernel->SendData( clientfd ,szbuf , nlen );
    }
    /************** 网络处理 *********************/
    //注册
    void RegisterRq(sock_fd clientfd, char*szbuf, int nlen);
    //登录
    void LoginRq(sock_fd clientfd, char*szbuf, int nlen);
    //創建房間
    void CreateRoomRq(int clientfd, char * szbuf, int nlen);
    //加入房間
    void JoinRoomRq(int clientfd, char * szbuf, int nlen);
    //離開房間
    void LeaveRoomRq(int clientfd, char * szbuf, int nlen);
    //處理音頻幀
    void AudioFrameRq(int clientfd, char * szbuf, int nlen);
    //處理視頻幀
    void VideoFrameRq(int clientfd, char * szbuf, int nlen);
    //音頻注冊
    void AudioRegister(int clientfd, char* szbuf, int nlen);
    //視頻注冊
    void VideoRegister(int clientfd, char* szbuf, int nlen);
    //【拓展优化-新增】补全邮箱
    //已注册用户登录后若邮箱为空，客户端弹出对话框让用户补全邮箱
    //用户提交后通过此函数处理
    void UpdateEmailRq(int clientfd, char * szbuf, int nlen);
    // [新增] 处理H.264视频帧
    // 功能：转发H.264编码的视频帧给房间内其他成员
    // 与VideoFrameRq的区别：包头结构不同（H.264包含frameType、frameSeq等字段）
    void VideoFrameH264Rq(int clientfd, char * szbuf, int nlen);

    // [新增] 处理关键帧请求（PLI）
    // 功能：当新成员加入房间或解码失败时，请求发送方强制发送I帧
    // 服务端将PLI请求转发给房间内其他成员（即视频发送方）
    void PliRequestRq(int clientfd, char * szbuf, int nlen);

    //【修复-P1：断开无业务层清理】连接断开回调（由网络层经 TcpKernel 转入）
    // 功能：按 fd 清理在线用户表中的 UserInfo 与各房间成员列表中的成员。
    // 修改原因: 原代码连接断开后业务层永不知情，UserInfo* 泄漏、死 fd 残留，
    //           fd 被新连接复用后音视频帧会串线到无关连接。
    // 修改效果: fd 关闭即从 m_mapIDToUserInfo / m_mapIDToRoomid 中移除对应项。
    void onClientClosed(int fd);

    /*******************************************/

private:
    TcpKernel* m_pKernel;
    CMysql * m_sql;
    Block_Epoll_Net * m_tcp;

    //STL map不是線程安全的，需要加鎖避免添加移除元素出現問題
    MyMap< int , UserInfo*> m_mapIDToUserInfo;
    MyMap< int , list<int>> m_mapIDToRoomid;

    //【P2修复：房间成员列表非原子读改写】保护「find拷贝->修改->insert写回」整体原子性的锁
    // 加锁顺序约定：先 m_roomLock 再 MyMap 内部锁（与 onClientClosed 一致），避免死锁
    pthread_mutex_t m_roomLock;

    //【安全加固-P1：登录防暴力破解】
    // 按「登录标识」记录失败：60 秒窗口内失败满 5 次，锁定 300 秒。
    // 锁定期间一律回 password_error（与口令错误同码，不泄露"已被锁定"信息）。
    // 仅内存态（重启清零），不落库 —— 阻断在线爆破已够用，持久化黑名单属后续增强。
    struct LoginAttempt
    {
        int   nFails;
        time_t tWindowStart;
        time_t tLockUntil;
        LoginAttempt() : nFails(0), tWindowStart(0), tLockUntil(0) {}
    };
    std::map<std::string, LoginAttempt> m_loginAttempts;
    pthread_mutex_t m_loginLock;
    // 锁定中返回 true；同时惰性清理过期窗口
    bool IsLoginLocked(const std::string& key);
    // 记录一次失败，达到阈值即写入 tLockUntil
    void RecordLoginFail(const std::string& key);
    // 登录成功后清空该标识的失败记录
    void ClearLoginFail(const std::string& key);

};

#endif // CLOGIC_H

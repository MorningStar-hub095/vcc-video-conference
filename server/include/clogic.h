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
        //【2026-11-18 审查修复-C1】初始化登录挑战会话锁
        pthread_mutex_init( &m_chalLock , NULL );
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
    //【2026-11-18 审查修复-C1】登录挑战：下发一次性随机数，凭据不上线
    void LoginChallengeRq(sock_fd clientfd, char*szbuf, int nlen);
    //【2026-11-18 审查修复-C1】登录校验：比对 MD5(h+nonce)，通过后走登录成功路径
    void LoginVerifyRq(sock_fd clientfd, char*szbuf, int nlen);
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

    //【2026-11-18 审查修复-H5：房间数量无上限】全局房间数上限。
    // 修改原因: 原代码任何登录用户可无限 CreateRoom，内存无界增长（远程内存 DoS）。
    // 修改效果: 建房时全局房间数达上限即拒绝（rs.m_lresult=0）；另有单用户限 1 房，
    //           空房间在 LeaveRoomRq 已有回收，本宏只封顶总量。
    // 原代码: （无上限）
    #define MAX_ROOMS_GLOBAL (256)

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
    //【2026-11-18 审查修复-H3：失败记录表无界增长】条目总数上限。
    // 修改原因: 原表只增不减（仅登录成功 ClearLoginFail 单键删除），攻击者伪造海量
    //           标识刷失败即可撑爆内存（远程内存 DoS）。
    // 修改效果: 达到上限先清扫过期条目（锁定期已过且窗口已过），仍超限则逐出最早
    //           窗口的条目，表大小恒有界；正常用户失败记录不受影响。
    // 原代码: （无上限）
    #define LOGIN_ATTEMPT_MAX_ENTRIES (10000)
    std::map<std::string, LoginAttempt> m_loginAttempts;
    pthread_mutex_t m_loginLock;
    // 锁定中返回 true；同时惰性清理过期窗口
    bool IsLoginLocked(const std::string& key);
    // 记录一次失败，达到阈值即写入 tLockUntil
    void RecordLoginFail(const std::string& key);
    // 登录成功后清空该标识的失败记录
    void ClearLoginFail(const std::string& key);

    /*===============【2026-11-18 审查修复-C1】登录 challenge-response 状态（新增）===============*/
    //挑战会话：challenge→verify 两步登录的中间态，按连接 fd 一对一。
    //生命周期: LoginChallengeRq 创建 → LoginVerifyRq 消费（一次性）/ 30 秒超时 /
    //          连接断开(onClientClosed)回收。
    struct LoginChallenge
    {
        std::string strLoginId;   //挑战对应的登录标识（校验阶段必须与其一致）
        std::string strNonce;     //32 字符十六进制随机数（128 位）
        int         nUserId;      //挑战时查到的用户 id；查不到置 0（防枚举：照发 nonce）
        bool        bHasVerifier; //该用户是否已有 challenge 凭据（t_user.verify_hash 非空）
        time_t      tCreate;      //签发时间，用于 TTL 与容量回收
        LoginChallenge() : nUserId(0), bHasVerifier(false), tCreate(0) {}
    };
    std::map<int, LoginChallenge> m_mapLoginChallenge;
    pthread_mutex_t m_chalLock;
    //挑战 TTL（秒）与内存上限（超限先清过期，仍超限则逐出最早签发的会话）
    #define LOGIN_CHALLENGE_TTL_SECS   (30)
    #define LOGIN_CHALLENGE_MAX_ENTRIES (2048)
    //签名一个挑战会话（LoginChallengeRq 内部用），返回写入的 nonce 十六进制串
    std::string SignLoginChallenge(int clientfd, const std::string& strLoginId, int nUserId, bool bHasVerifier);
    /*===============【C1 状态结束】===============*/

};

#endif // CLOGIC_H

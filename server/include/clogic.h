#ifndef CLOGIC_H
#define CLOGIC_H

#include"TCPKernel.h"

class CLogic
{
public:
    CLogic( TcpKernel* pkernel )
    {
        m_pKernel = pkernel;
        m_sql = pkernel->m_sql;
        m_tcp = pkernel->m_tcp;
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

    /*******************************************/

private:
    TcpKernel* m_pKernel;
    CMysql * m_sql;
    Block_Epoll_Net * m_tcp;

    //STL map不是線程安全的，需要加鎖避免添加移除元素出現問題
    MyMap< int , UserInfo*> m_mapIDToUserInfo;
    MyMap< int , list<int>> m_mapIDToRoomid;

};

#endif // CLOGIC_H

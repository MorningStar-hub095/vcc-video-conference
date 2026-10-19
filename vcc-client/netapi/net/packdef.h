#pragma once

#include<memory.h>

#define _DEF_BUFFER         (4096)
#define _DEF_CONTENT_SIZE	(1024)
#define _MAX_SIZE           (40)
#define _MAX_PATH           (260)

//自定义协议   先写协议头 再写协议结构
//登录 注册 获取好友信息 添加好友 聊天 发文件 下线请求
#define _DEF_PORT   (8000)
#define _DEF_SERVERIP   ("127.0.0.1")



#define DEF_PACK_BASE	(10000)
#define DEF_PACK_COUNT  (100)

//注册
#define DEF_PACK_REGISTER_RQ	(DEF_PACK_BASE + 0 )
#define DEF_PACK_REGISTER_RS	(DEF_PACK_BASE + 1 )
//登录
#define DEF_PACK_LOGIN_RQ	(DEF_PACK_BASE + 2 )
#define DEF_PACK_LOGIN_RS	(DEF_PACK_BASE + 3 )
//创建房间
#define DEF_PACK_CREATEROOM_RQ  (DEF_PACK_BASE + 4)
#define DEF_PACK_CREATEROOM_RS  (DEF_PACK_BASE + 5)
//加入房间
#define DEF_PACK_JOINROOM_RQ  (DEF_PACK_BASE + 6)
#define DEF_PACK_JOINROOM_RS  (DEF_PACK_BASE + 7)
//房间列表
#define DEF_PACK_ROOM_MEMBER    (DEF_PACK_BASE + 8)
//音频数据
#define DEF_PACK_AUDIO_FRAME    (DEF_PACK_BASE + 9)
//视频数据
#define DEF_PACK_VIDEO_FRAME    (DEF_PACK_BASE + 10)
//退出房间请求
#define DEF_PACK_LEAVEROOM_RQ   (DEF_PACK_BASE + 11)
//退出房间回复
#define DEF_PACK_LEAVEROOM_RS   (DEF_PACK_BASE + 12)
//音频注册
#define DEF_PACK_AUDIO_REGISTER   (DEF_PACK_BASE + 13)
//视频注册
#define DEF_PACK_VIDEO_REGISTER   (DEF_PACK_BASE + 14)
//补全邮箱请求和回复
#define DEF_PACK_UPDATE_EMAIL_RQ  (DEF_PACK_BASE + 15)
#define DEF_PACK_UPDATE_EMAIL_RS  (DEF_PACK_BASE + 16)

//  H.264相关协议包类型
//H.264视频帧（与JPEG协议共存，接收端按type字段分流）
#define DEF_PACK_VIDEO_FRAME_H264  (DEF_PACK_BASE + 17)
//关键帧请求（PLI机制，新成员加入时触发）
#define DEF_PACK_PLI_REQUEST       (DEF_PACK_BASE + 18)
//SPS/PPS单独分发（可选，编码器默认inline模式可不使用）
#define DEF_PACK_VIDEO_SPSPPS      (DEF_PACK_BASE + 19)



//  H.264分片大小常量
//  分片大小从4KB提升到8KB，重组超时从100ms提升到150ms
// 原值(SD): H264_FRAGMENT_SIZE=4096, H264_REASSEMBLE_TIMEOUT=100
// 修改原因: 1280x720 H264 I帧可达50-150KB，4KB分片需要13-38片，
//           增大到8KB后仅需7-19片，减少分片数量和包头开销
//           重组超时从100ms增至150ms，适应更大帧的传输延迟
// 影响范围: 单片8KB+包头约8.1KB，仍远低于1MB安全阈值；接收端重组超时延长50ms
#define H264_FRAGMENT_SIZE       (8192)   //  4096 → 8192
//H.264分片重组超时（毫秒）
#define H264_REASSEMBLE_TIMEOUT  (150)    //  100 → 150


// PLI关键帧请求限流参数
// 修改原因: PLI请求无限流时，弱网下I帧(50-150KB,7-19个分片)本身最易重组超时，
//           会触发更多PLI → 更多I帧尖峰 → 挤占带宽拖垮P帧的正反馈风暴
// 修改内容: 新增接收端PLI限流窗口(500ms)与发送端强制I帧最小间隔(300ms)两个常量
// 影响范围: 仅客户端PLI/强制I帧路径；双端限流形成防风暴的双层保险
#define PLI_THROTTLE_INTERVAL_MS   (500)   // 接收端: 每用户PLI最小发送间隔
#define H264_KEYFRAME_MIN_INTERVAL_MS (300) // 发送端: 强制I帧最小间隔


//返回的结果
//注册请求的结果
#define tel_is_exist		(0)
#define register_success	(1)
#define name_is_exist       (2)
// 写库失败（服务端未能完成注册）。必须与服务端 packdef.h 保持一致。
// 注意：这个值【不能】复用 tel_is_exist —— 用户会以为换个手机号就行，
// 而真实原因可能是昵称过长、服务端写库失败等，方向完全不同。
#define register_failed     (3)
//登录请求的结果
#define user_not_exist		(0)
#define password_error		(1)
#define login_success		(2)
//登录成功但邮箱为空，需要补全邮箱
#define login_success_no_email (3)


typedef int PackType;
#define MAX_SIZE            (60)

//协议结构
//注册请求
typedef struct STRU_REGISTER_RQ
{
    STRU_REGISTER_RQ()
    {
        m_nType = DEF_PACK_REGISTER_RQ;
        memset( m_tel  , 0, MAX_SIZE);
        memset( m_name  , 0,MAX_SIZE);
        memset( m_password , 0,MAX_SIZE );
        //初始化邮箱字段
        memset( m_email , 0, MAX_SIZE );
    }
    //需要手机号码 , 密码, 昵称
    PackType m_nType;//包类型
    char m_tel[MAX_SIZE];//电话号
    char m_name[MAX_SIZE];//昵称
    char m_password[MAX_SIZE];//密码
    //邮箱字段
    //用于注册时存储用户邮箱，支持后续通过邮箱登录
    char m_email[MAX_SIZE];//邮箱

}STRU_REGISTER_RQ;

//注册回复
typedef struct STRU_REGISTER_RS
{
    //回复结果
    STRU_REGISTER_RS()
    {
        m_nType = DEF_PACK_REGISTER_RS;
        m_lresult = 0;
    }
    PackType m_nType;//包类型
    int m_lresult;//注册结果

}STRU_REGISTER_RS;

//登录
typedef struct STRU_LOGIN_RQ
{
	//登录需要: 手机号 密码
    STRU_LOGIN_RQ()
	{
        m_nType = DEF_PACK_LOGIN_RQ;
        //【内存安全修复】字段声明是 _MAX_SIZE(40)，此前按 MAX_SIZE(60) 清零，
        //  每次构造都越界写 20 字节。用 sizeof 跟随字段宽度，杜绝复发。
        memset( m_tel, 0, sizeof(m_tel) );
        memset( m_password, 0, sizeof(m_password) );
	}
    PackType m_nType;//包类型
    //登录标识字段
    //原字段 m_tel 仅支持手机号登录
    //现复用该字段作为通用登录标识，可传入手机号/用户名（昵称）/邮箱
    //服务端会自动识别类型并查询对应字段
    char m_tel[_MAX_SIZE];//登录标识（手机号/用户名/邮箱）
    char m_password[_MAX_SIZE];//密码

}STRU_LOGIN_RQ;

//登录回复
typedef struct STRU_LOGIN_RS
{
	// 需要 结果 , 用户的id
    STRU_LOGIN_RS()
	{
        m_nType = DEF_PACK_LOGIN_RS;
        m_userid = 0;
        m_lresult = 0;
        memset(m_name , 0 , MAX_SIZE);
	}
    PackType m_nType;//包类型
    int  m_userid;
    int  m_lresult;//注册结果
    char m_name[MAX_SIZE];//密码

}STRU_LOGIN_RS;

//创建房间请求
typedef struct STRU_CREATEROOM_RQ
{

    STRU_CREATEROOM_RQ()
    {
        m_nType = DEF_PACK_CREATEROOM_RQ;
        m_UserID = 0;

    }
    PackType m_nType;//包类型
    int  m_UserID;


}STRU_CREATEROOM_RQ;

//创建房间回复
typedef struct STRU_CREATEROOM_RS
{

    STRU_CREATEROOM_RS()
    {
        m_nType = DEF_PACK_CREATEROOM_RS;
        m_RoomID = 0;
        m_lresult = 0;

    }
    PackType m_nType;//包类型
    int  m_RoomID;
    int  m_lresult;


}STRU_CREATEROOM_RS;

//加入房间请求
typedef struct STRU_JOINROOM_RQ
{

    STRU_JOINROOM_RQ()
    {
        m_nType = DEF_PACK_JOINROOM_RQ;
        m_UserID = 0;
        m_RoomID = 0;

    }
    PackType m_nType;//包类型
    int  m_UserID;
    int  m_RoomID;


}STRU_JOINROOM_RQ;

//加入房间回复
typedef struct STRU_JOINROOM_RS
{

    STRU_JOINROOM_RS()
    {
        m_nType = DEF_PACK_JOINROOM_RS;
        m_RoomID = 0;
        m_lresult = 0;

    }
    PackType m_nType;//包类型
    int  m_RoomID;
    int  m_lresult;


}STRU_JOINROOM_RS;

//房间成员请求
typedef struct STRU_ROOM_MEMBER_RQ
{

    STRU_ROOM_MEMBER_RQ()
    {
        m_nType = DEF_PACK_ROOM_MEMBER;
        m_UserID = 0;
        memset(m_szUser ,0,MAX_SIZE);

    }
    PackType m_nType;//包类型
    int  m_UserID;
    char m_szUser[MAX_SIZE];


}STRU_ROOM_MEMBER_RQ;

//离开房间请求
typedef struct STRU_LEAVEROOM_RQ
{

    STRU_LEAVEROOM_RQ()
    {
        m_nType = DEF_PACK_LEAVEROOM_RQ;
        m_nUserID = 0;
        m_RoomID = 0;
        memset(szUserName ,0,MAX_SIZE);

    }
    PackType m_nType;//包类型
    int  m_nUserID;
    int m_RoomID;
    char szUserName[MAX_SIZE];


}STRU_LEAVEROOM_RQ;

//用户个人信息id->UserInfo
typedef struct UserInfo
{

    UserInfo()
    {
        m_sock = 0;
        m_id = 0;
        m_roomid = 0;
        memset(m_userName , 0 , MAX_SIZE);
    }
    int  m_sock;
    int  m_id;
    int m_roomid;
    char m_userName[MAX_SIZE];

}UserInfo;

//注册音频
struct STRU_AUDIO_REGISTER
{
    STRU_AUDIO_REGISTER():m_nType(DEF_PACK_AUDIO_REGISTER)
    {
        m_userid = 0;
    }
    PackType m_nType;//包类型
    int m_userid;

};

//注册视频
struct STRU_VIDEO_REGISTER
{
    STRU_VIDEO_REGISTER():m_nType(DEF_PACK_VIDEO_REGISTER)
    {
        m_userid = 0;
    }
    PackType m_nType;//包类型
    int m_userid;

};


///音频数据帧
///成员描述
///int type;
///int userId;
///int roomId;
///int min;
///int sec;
///int msec;
///QByteArray audioFrame;

///视频数据帧
///成员描述
///int type;
///int userId;
///int roomId;
///int min;
///int sec;
///int msec;
///QByteArray audioFrame;

//邮箱请求
//已注册用户登录后若邮箱为空，客户端弹出对话框让用户补全邮箱
//用户提交后通过此包发送给服务端
struct STRU_UPDATE_EMAIL_RQ
{
    STRU_UPDATE_EMAIL_RQ()
    {
        m_nType = DEF_PACK_UPDATE_EMAIL_RQ;
        m_userid = 0;
        memset( m_email , 0, MAX_SIZE );
    }
    PackType m_nType;//包类型
    int m_userid;//用户ID
    char m_email[MAX_SIZE];//邮箱
};

//邮箱回复
struct STRU_UPDATE_EMAIL_RS
{
    STRU_UPDATE_EMAIL_RS()
    {
        m_nType = DEF_PACK_UPDATE_EMAIL_RS;
        m_lresult = 0;
    }
    PackType m_nType;//包类型
    int m_lresult;//补全结果 1=成功 0=失败
};

//  H.264视频帧包结构定义
//包结构: 
//        
//        ]
//字段说明:
//  frameType: 0=I帧, 1=P帧
//  frameSeq: 帧序号（同一帧的多片共享seq）
//  totalFragments/fragmentIndex: 分片信息（4KB/片）
typedef struct STRU_VIDEO_FRAME_H264_HEADER
{
    STRU_VIDEO_FRAME_H264_HEADER()
    {
        m_nType = DEF_PACK_VIDEO_FRAME_H264;
        m_userid = 0;
        m_roomid = 0;
        m_min = 0;
        m_sec = 0;
        m_msec = 0;
        m_frameType = 0;
        m_frameSeq = 0;
        m_totalFragments = 0;
        m_fragmentIndex = 0;
    }
    PackType m_nType;//包类型 DEF_PACK_VIDEO_FRAME_H264
    int m_userid;//发送者ID
    int m_roomid;//房间ID
    int m_min;//时间戳-分钟
    int m_sec;//时间戳-秒
    int m_msec;//时间戳-毫秒
    int m_frameType;//0=I帧, 1=P帧
    int m_frameSeq;//帧序号
    int m_totalFragments;//总分片数
    int m_fragmentIndex;//当前分片序号（0-based）
    //后接 char m_h264Data 可变长数据
}STRU_VIDEO_FRAME_H264_HEADER;

//  关键帧请求包结构
typedef struct STRU_PLI_REQUEST
{
    STRU_PLI_REQUEST():m_nType(DEF_PACK_PLI_REQUEST)
    {
        m_userid = 0;
        m_roomid = 0;
    }
    PackType m_nType;//包类型 DEF_PACK_PLI_REQUEST
    int m_userid;//请求者ID
    int m_roomid;//房间ID
}STRU_PLI_REQUEST;


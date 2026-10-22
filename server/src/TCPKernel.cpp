#include<TCPKernel.h>
#include "packdef.h"
#include<stdio.h>
#include<sys/time.h>
#include"clogic.h"

using namespace std;


//设置网络协议映射
void TcpKernel::setNetPackMap()
{
    //清空映射
    bzero( m_NetPackMap , sizeof(m_NetPackMap) );

    //协议映射赋值
    m_logic->setNetPackMap();
//        NetPackMap(DEF_PACK_REGISTER_RQ)         =&TcpKernel::RegisterRq;
//        NetPackMap(DEF_PACK_LOGIN_RQ)            =&TcpKernel::LoginRq;
//        NetPackMap(DEF_PACK_CREATEROOM_RQ)       =&TcpKernel::CreateRoomRq;
//        NetPackMap(DEF_PACK_JOINROOM_RQ)         =&TcpKernel::JoinRoomRq;
//        NetPackMap(DEF_PACK_LEAVEROOM_RQ)        =&TcpKernel::LeaveRoomRq;
//        NetPackMap(DEF_PACK_AUDIO_FRAME)         =&TcpKernel::AudioFrameRq;
//        NetPackMap(DEF_PACK_VIDEO_FRAME)         =&TcpKernel::VideoFrameRq;
//        NetPackMap(DEF_PACK_VIDEO_REGISTER)      =&TcpKernel::AudioRegister;
//        NetPackMap(DEF_PACK_VIDEO_REGISTER)      =&TcpKernel::VideoRegister;


}


TcpKernel::TcpKernel()
{
    //【修复-P1：构造函数不初始化成员】
    // 修改原因: 原构造函数为空，m_sql/m_tcp/m_logic 为野指针；
    //           若 Open() 提前失败返回，main 继续调 EventLoop() 会解引用野 m_tcp 崩溃，
    //           析构函数 `if(m_logic) delete` 判断的也是野指针。
    // 修改效果: 三指针默认 NULL，任何失败路径下判空/析构都是安全空操作。
    // 原代码: TcpKernel::TcpKernel() { }
    m_sql   = NULL;
    m_tcp   = NULL;
    m_logic = NULL;
}

TcpKernel::~TcpKernel()
{
    if( m_logic ) delete m_logic;
}

TcpKernel *TcpKernel::GetInstance()
{
    static TcpKernel kernel;
    return &kernel;
}

int TcpKernel::Open( int port)
{
    initRand();

    m_sql = new CMysql;
    // 数据库连接参数全部来自环境变量 DB_IP / DB_USER / DB_PWD / DB_NAME，
    // 未注入时回退到 packdef.h 里的默认值（口令默认为空）。
    // 库若不存在会连接失败，需要先自行创建。
    if(  !m_sql->ConnectMysql( _DEF_DB_IP , _DEF_DB_USER, _DEF_DB_PWD, _DEF_DB_NAME )  )
    {
        return FALSE;
    }
    else
    {
    }
    //初始网络
    m_tcp = new Block_Epoll_Net;
    bool res = m_tcp->InitNet( port , &TcpKernel::DealData ) ;
    if( !res )
        err_str( "net init fail:" ,-1);

    //【修复-P1：断开无业务层清理】接线：连接关闭 -> CLogic::onClientClosed
    // 修改原因: 网络层断开时业务层（在线用户表/房间成员表）永不清理，
    //           UserInfo 泄漏、死 fd 复用后音视频帧串线。
    // 修改效果: fd 关闭即触发业务层清理；m_logic 在 InitNet 之后创建，
    //           注意回调执行时 m_logic 必已就绪（连接事件只会发生在 Open 完成后）。
    // 原代码: （无接线）
    m_tcp->setCloseCallback( &TcpKernel::OnClientClosed );

    m_logic = new CLogic(this);

    setNetPackMap();

    return TRUE;
}

void TcpKernel::Close()
{
    m_sql->DisConnect();

}

//随机数初始化
void TcpKernel::initRand()
{
    struct timeval time;
    gettimeofday( &time , NULL);
    srand( time.tv_sec + time.tv_usec );
}

void TcpKernel::DealData(sock_fd clientfd,char *szbuf,int nlen)
{
    //【修复-P1：DealData 读包头前未校验包长】
    // 修改原因: *(PackType*)szbuf 固定读 4 字节，nlen 为 1~3 的残包会越界读。
    // 修改效果: 包体不足 4 字节直接丢弃，不再进入类型分发。
    // 原代码: PackType type = *(PackType*)szbuf;（无校验）
    if( nlen < (int)sizeof(PackType) ) return;

    PackType type = *(PackType*)szbuf;
    if( (type >= DEF_PACK_BASE) && ( type < DEF_PACK_BASE + DEF_PACK_COUNT) )
    {
        PFUN pf = NetPackMap( type );
        if( pf )
        {
            (TcpKernel::GetInstance()->m_logic->*pf)( clientfd , szbuf , nlen);
        }
        else
        {
        }
    }
    else
    {
    }

    return;
}

void TcpKernel::EventLoop()
{
    m_tcp->EventLoop();
}

//【修复-P1：断开无业务层清理】连接关闭回调（静态，转调业务层）
// 修改原因: 见 Open() 中 setCloseCallback 接线处注释。
// 修改效果: fd 关闭 -> CLogic::onClientClosed 清理在线用户与房间成员；
//           m_logic 未就绪（理论不可达，防御性判空）时静默跳过。
// 原代码: （无此函数）
void TcpKernel::OnClientClosed( int fd )
{
    TcpKernel * pKernel = TcpKernel::GetInstance();
    if( pKernel && pKernel->m_logic )
        pKernel->m_logic->onClientClosed( fd );
}

void TcpKernel::SendData(sock_fd clientfd, char *szbuf, int nlen)
{
    m_tcp->SendData(clientfd , szbuf ,nlen );
}


// -----------------------------------------------------------------------------
// 原RegisterRq / LoginRq / CreateRoomRq / JoinRoomRq / LeaveRoomRq / AudioFrameRq /
// VideoFrameRq / AudioRegister / VideoRegister 的实现（约 320 行）曾位于本文件，
// 现已全部迁移至 CLogic（见 server/src/clogic.cpp），本类不再处理任何协议包。
// 保留此说明以说明 TcpKernel 的职责边界：它只负责网络收发与包类型分发。
// -----------------------------------------------------------------------------

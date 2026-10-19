#include "ckernel.h"
#include "QDebug"
#include "packdef.h"
#include "md5.h"
#include <QMessageBox>
#include <QTime>
#include <QBuffer>
#include <QThread>
#include <QTimer>
#include <QCoreApplication>
#include <QDateTime>
#include <thread>

// 引入FFmpeg头文件（用于AVFrame类型）
extern "C"
{
#include "libavutil/frame.h"
}


#define NetPackMap(a) m_netPackMap[ a - DEF_PACK_BASE ]

//设置协议映射关系
void ckernel::setNetPackMap()
{
    memset(m_netPackMap, 0, sizeof(m_netPackMap));
    //m_netPackMap = &ckernel::slot_dealLoginRS;
    NetPackMap(DEF_PACK_LOGIN_RS)         = &ckernel::slot_dealLoginRS;
    NetPackMap(DEF_PACK_REGISTER_RS)      = &ckernel::slot_dealRegisterRS;
    NetPackMap(DEF_PACK_CREATEROOM_RS)    = &ckernel::slot_dealCreateRoomRS;
    NetPackMap(DEF_PACK_JOINROOM_RS)      = &ckernel::slot_dealJoinRoomRS;
    NetPackMap(DEF_PACK_ROOM_MEMBER)      = &ckernel::slot_dealRoomMemberRq;
    NetPackMap(DEF_PACK_LEAVEROOM_RQ)     = &ckernel::slot_dealLeaveRoomRq;
    NetPackMap(DEF_PACK_AUDIO_FRAME)      = &ckernel::slot_dealAudioFrameRq;
    NetPackMap(DEF_PACK_VIDEO_FRAME)      = &ckernel::slot_dealVideoFrameRq;
    NetPackMap(DEF_PACK_UPDATE_EMAIL_RS)  = &ckernel::slot_dealUpdateEmailRS;

    // H.264协议映射
    // H.264 视频帧处理（与 JPEG 路径共存，接收端按 type 分流）
    NetPackMap(DEF_PACK_VIDEO_FRAME_H264) = &ckernel::slot_dealVideoFrameH264Rq;
    // 关键帧请求处理
    NetPackMap(DEF_PACK_PLI_REQUEST)      = &ckernel::slot_dealPliRequest;
}

#include <QSettings>
#include <QApplication>
#include <QFileInfo>
//初始化配置
void ckernel::initConfig()
{
    m_serverIP = _DEF_SERVER_IP;

    //路径设置， exe同级目录下->applicationDirPath config.ini
    QString path = QApplication::applicationDirPath() + "/config.ini";
    //判断是否存在
    QFileInfo info( path );
    QSettings settings( path, QSettings::IniFormat, NULL);//有就打开，没有就创建
    if( info.exists()){
        //加载配置文件 ip设置为配置文件中的ip
        //打开配置文件
        //移动到Net组
        settings.beginGroup( "Net" );
        //读取 ip-> addr -->赋值
        QVariant ip = settings.value( "ip" );
        QString strIP = ip.toString();
        //结束
        settings.endGroup();
        if(!strIP.isEmpty()){
            m_serverIP = strIP;

        }
    }else{
        //没有配置文件，写入默认的ip
        settings.beginGroup("Net");
        settings.setValue("ip" , m_serverIP);
        settings.endGroup();
    }
    qDebug()<<m_serverIP;
}

ckernel::ckernel(QObject *parent) : QObject(parent), m_id(0), m_roomid(0)
{
    // H.264相关成员初始化
    m_h264Encoder = nullptr;
    m_useH264 = false;  // 默认关闭，待编码器初始化成功后开启
    m_h264FrameSeq = 0;
    m_encodingPending = false;  //  编码防堆积标志初始值

    // 分片重组主动扫描定时器初始化
    // 修改原因: 原超时检测纯被动（仅收到新分片时执行），视频流完全中断
    //           （对端崩溃/断网）时新分片永不到达：残留分片组驻留内存、
    //           超时PLI永不触发。
    // 修改内容: 创建50ms周期定时器并启动，周期调用slot_scanReassembleBuffer
    //           主动扫描过期分片组
    // 影响范围: 空房间时扫描空map开销可忽略（50ms一次遍历空容器）
    m_reassembleScanTimer = new QTimer(this);
    connect(m_reassembleScanTimer, SIGNAL(timeout()), this, SLOT(slot_scanReassembleBuffer()));
    m_reassembleScanTimer->start(50);


    // 同步逻辑已抽离到 AVSyncManager 独立模块
    /* 原有代码：
    // 视频同步时钟初始化
    m_audioClock = 0.0;  // 音频主时钟（微秒）
    m_videoClock = 0.0;  // 视频辅助时钟（微秒）
    */

    // SDL音频子系统初始化
    // 必须在AudioWrite创建之前调用，AudioWrite构造函数会打开SDL音频设备
    // 参考AudioDemo2/main.cpp中的SDL_Init(SDL_INIT_AUDIO)
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        qDebug() << "[SDL] SDL_Init(SDL_INIT_AUDIO) failed:" << SDL_GetError();
    } else {
        qDebug() << "[SDL] SDL_Init(SDL_INIT_AUDIO) succeeded";
    }

    qDebug()<<"main Thread:"<<QThread::currentThreadId();
    //设置协议映射关系
    setNetPackMap();
    //加载初始化设置
    initConfig();
    m_pVccDlg = new VccDialog;
    connect(m_pVccDlg , SIGNAL(SIG_close()) , this , SLOT(slot_destroy()));
    connect(m_pVccDlg , SIGNAL(SIG_createRoom()) , this , SLOT(slot_createroom()));
    connect(m_pVccDlg , SIGNAL(SIG_joinRoom()) , this , SLOT(slot_joinroom()));
    //m_pVccDlg->show();

    m_pLoginDlg = new LoginDialog;
    connect(m_pLoginDlg , SIGNAL(SIG_loginCommit(QString , QString)) , this , SLOT(slot_loginCommit(QString , QString)));
    connect(m_pLoginDlg , SIGNAL(SIG_close()) , this , SLOT(slot_destroy()));
    //注册信号绑定新增 email 参数
    connect(m_pLoginDlg , SIGNAL(SIG_registerCommit(QString , QString, QString, QString)) , this , SLOT(slot_registerCommit(QString , QString, QString, QString)));
    m_pLoginDlg->show();

    m_pRoomdialog = new RoomDialog;

    connect(m_pRoomdialog , SIGNAL(SIG_close()) , this , SLOT(slot_quitRoom()));
    connect(m_pRoomdialog , SIGNAL(SIG_AudioPause()) , this , SLOT(slot_pauseAudio()));
    connect(m_pRoomdialog , SIGNAL(SIG_AudioStart()) , this , SLOT(slot_startAudio()));
    connect(m_pRoomdialog , SIGNAL(SIG_VideoPause()) , this , SLOT(slot_pauseVideo()));
    connect(m_pRoomdialog , SIGNAL(SIG_VideoStart()) , this , SLOT(slot_startVideo()));
    connect(m_pRoomdialog , SIGNAL(SIG_ScreenPause()) , this , SLOT(slot_pauseScreen()));
    connect(m_pRoomdialog , SIGNAL(SIG_ScreenStart()) , this , SLOT(slot_startScreen()));


    m_pClient = new TcpClientMediator;
    connect(m_pClient , SIGNAL(SIG_ReadyData(uint,char*,int)),this , SLOT(slot_dealData(uint,char*,int)));

    //音频、视频各使用一条独立的 TCP 连接
    for( int i = 0 ; i < 2 ; i++ )
    {
        m_pAVClient[i] = new TcpClientMediator;
        //修复：原来错误地连接了 m_pClient，应该连接 m_pAVClient
        connect(m_pAVClient[i] , SIGNAL(SIG_ReadyData(uint,char*,int)),this , SLOT(slot_dealData(uint,char*,int)));
    }

    //修复：网络初始化（阻塞式 connect）放到子线程执行，避免主线程阻塞导致 UI 无响应
    std::thread([this](){
        m_pClient->OpenNet(m_serverIP.toStdString().c_str());
        for( int i = 0 ; i < 2 ; i++ )
        {
            // AV连接使用 m_serverIP 替代硬编码 _DEF_SERVER_IP
            // 修改原因: 原代码使用 _DEF_SERVER_IP（硬编码的内网地址），
            //           当用户通过 config.ini 配置了不同的服务器IP时，
            //           主连接(m_pClient)使用配置IP能正常登录，
            //           但AV连接(m_pAVClient)仍然连到硬编码IP，导致连接失败。
            //           视频帧通过 m_pAVClient 发送，连接失败则视频数据无法到达服务端。
            // 修改内容: 将 _DEF_SERVER_IP 改为 m_serverIP.toStdString().c_str()
            //           使AV连接与主连接使用相同的服务器IP
            // 原代码: m_pAVClient->OpenNet( _DEF_SERVER_IP , _DEF_PORT );
            m_pAVClient[i]->OpenNet( m_serverIP.toStdString().c_str() , _DEF_PORT );
        }
    }).detach();
    m_pAudioRead = new AudioRead;
    connect(m_pAudioRead , SIGNAL(SIG_audioFrame(QByteArray)),this , SLOT(slot_audioFrame(QByteArray)));
    // 连接静音状态变化信号（AudioRead VAD 检测 → ckernel → RoomDialog UI 联动）
    // 跨线程：信号在 SDL 音频线程发射，Qt 自动以 QueuedConnection 投递到主线程槽
    connect(m_pAudioRead , SIGNAL(SIG_muteStateChanged(bool)),
            this , SLOT(slot_muteStateChanged(bool)));

    m_pVideoRead = new VideoRead;
    connect( m_pVideoRead , SIGNAL(SIG_sendVideoFrame(QImage)) , this , SLOT(slot_sendVideoFrame(QImage)));

    m_pScreenRead = new ScreenRead;
    connect( m_pScreenRead , SIGNAL(SIG_getScreenFrame(QImage)) , this , SLOT(slot_sendVideoFrame(QImage)));

    m_pSendVideoWorker = QSharedPointer<SendVideoWorker>( new SendVideoWorker );
    connect( this , SIGNAL(SIG_SendVideo( char* , int )) , m_pSendVideoWorker.data() , SLOT(slot_SendVideo( char*, int ) ));

}


//回收
void ckernel::slot_destroy()
{
    qDebug()<<__func__;
    if(m_pVccDlg)
    {
        m_pVccDlg->hide();
        delete m_pVccDlg;
        m_pVccDlg = NULL;

    }
    if(m_pLoginDlg)
    {
        m_pLoginDlg->hide();
        delete m_pLoginDlg;
        m_pLoginDlg = NULL;

    }
    if(m_pRoomdialog)
    {
        m_pRoomdialog->hide();
        delete m_pRoomdialog;
        m_pRoomdialog = NULL;

    }
    if(m_pAudioRead)
    {
        m_pAudioRead->pause();
        delete m_pAudioRead;
        m_pAudioRead = NULL;

    }
    if(m_pClient)
    {
        m_pClient->CloseNet();
        delete m_pClient;
        m_pClient = NULL;

    }

    // 先停止 SendVideoWorker 线程，再释放 m_pAVClient
    // 原Bug: slot_destroy 直接 delete m_pAVClient，但 SendVideoWorker 线程可能正在
    //        执行 SendData（访问 m_pAVClient 内部的 m_sock）。worker 线程未停止就
    //        释放 m_pAVClient → use-after-free → 堆损坏 → 随机崩溃。
    // 修复: 在释放 m_pAVClient 之前，显式清空 m_pSendVideoWorker 智能指针，
    //       触发 ThreadWorker 析构（quit + wait 3秒），确保 worker 线程完全退出后
    //       再释放网络资源。quit() 让 worker 事件循环退出，wait() 阻塞等待线程结束。
    if (m_pSendVideoWorker) {
        qDebug() << "停止 SendVideoWorker 线程...";
        m_pSendVideoWorker.clear();  // 触发 ThreadWorker 析构：quit() + wait()
        qDebug() << "SendVideoWorker 线程已停止";
    }

    //回收 AV 连接
    for( int i = 0 ; i < 2 ; i++ )
    {
        if( m_pAVClient[i] )
        {
            m_pAVClient[i]->CloseNet();
            delete m_pAVClient[i];
            m_pAVClient[i] = NULL;
        }
    }

    // 释放H264编码器与解码器池资源
    // 确保FFmpeg上下文与OpenGL相关资源正确释放

    // 清除事件队列中所有待处理的事件，防止 use-after-free
    // 原Bug: encodeAndSendVideoFrame 通过 QTimer::singleShot(0, this, lambda) 投递到事件队列，
    //        lambda 捕获了 this 指针。如果 slot_destroy() 先执行并释放了 m_h264Encoder、
    //        m_pAVClient 等资源，事件队列中残留的 lambda 执行时会访问已释放的成员 → 崩溃。
    // 修复: 在释放资源前，移除所有以 this 为接收者的待处理事件。
    QCoreApplication::removePostedEvents(this);
    // 处理一下事件循环，让已投递的跨线程信号也得到处理
    QCoreApplication::processEvents();

    // 重置编码防堆积标志，防止析构后残留状态
    m_encodingPending = false;

    qDebug() << "=== H264资源清理流程开始 ===";

    // 释放编码器
    if (m_h264Encoder) {
        qDebug() << "释放H264编码器, ptr:" << (void*)m_h264Encoder;
        m_h264Encoder->close();
        delete m_h264Encoder;
        m_h264Encoder = nullptr;
        m_useH264 = false;
        qDebug() << "H264编码器已释放";
    }

    // 释放解码器池（每个房间成员独立一个解码器）
    int decoderCount = m_mapIDToH264Decoder.size();
    for (auto &pair : m_mapIDToH264Decoder) {
        if (pair.second) {
            pair.second->close();
            delete pair.second;
            pair.second = nullptr;
        }
    }
    m_mapIDToH264Decoder.clear();
    if (decoderCount > 0) {
        qDebug() << "已释放" << decoderCount << "个H264解码器实例";
    }

    qDebug() << "=== H264资源清理完成 ===";
    exit(0);
}

//MD5 加盐密钥优化
//原代码使用硬编码的简单整数 1234 作为盐值，安全性极低。
//优化说明：
//使用更长的随机字符串作为盐值，增加彩虹表攻击难度
//注意：MD5 本身已被认为不安全，理想方案应使用 bcrypt/scrypt/Argon2，但由于服务端协议已固定使用 MD5，此处仅优化盐值强度，保持协议兼容
//实际生产环境建议升级到 SHA-256 + 随机盐 或专用密码哈希函数
#define MD5_KEY ("vC0nF2024$SecR1t^S4ltK3y!")
static std::string GetMD5(QString value)
{
    //将用户密码与盐值拼接后进行 MD5 哈希
    //格式：password_salt，增加字典攻击难度
    QString str = QString("%1_%2").arg(value).arg(MD5_KEY);
    std::string strSrc = str.toStdString();
    MD5 md5(strSrc);
    return md5.toString();
}

//提交登录信息
void ckernel::slot_loginCommit(QString tel, QString pass)
{
    qDebug() << "slot_loginCommit 被调用, tel:" << tel;
       //检查网络连接状态，未连接时提示用户，避免 SendData 内部阻塞重连导致 UI 卡死
       if( !m_pClient || !m_pClient->IsConnected() )
       {
           qDebug() << "网络未连接";
           QMessageBox::about(m_pLoginDlg, "提示", "网络未连接，请检查服务器是否启动后重试");
           return;
       }
       qDebug() << "网络已连接，准备发送登录请求";
    std::string strTel = tel.toStdString();

    STRU_LOGIN_RQ rq;
    //原代码使用 strcpy 存在缓冲区溢出风险：
    //若 tel 长度超过 _MAX_SIZE(40)，会越界写入 rq.m_tel 后的内存区域。
    //修复：使用 strncpy 限制拷贝长度，并手动确保字符串以 '\0' 结尾
    strncpy(rq.m_tel , strTel.c_str() , sizeof(rq.m_tel) - 1);
    rq.m_tel[sizeof(rq.m_tel) - 1] = '\0';

    std::string strPassMD5 = GetMD5(pass);
    qDebug()<<strPassMD5.c_str();
    //同样使用 strncpy 防止密码字段溢出
    strncpy(rq.m_password , strPassMD5.c_str() , sizeof(rq.m_password) - 1);
    rq.m_password[sizeof(rq.m_password) - 1] = '\0';

    /*先转换std，后转换char**/
    /*发送*/
    m_pClient->SendData(0 , (char*)&rq, sizeof(rq));

}

//发送注册信息
//函数签名新增 email 参数
void ckernel::slot_registerCommit(QString tel, QString pass, QString name, QString email)
{
    qDebug() << "slot_registerCommit 被调用, tel:" << tel << " name:" << name << " email:" << email;
       //检查网络连接状态，未连接时提示用户，避免 SendData 内部阻塞重连导致 UI 卡死
       if( !m_pClient || !m_pClient->IsConnected() )
       {
           qDebug() << "网络未连接";
           QMessageBox::about(m_pLoginDlg, "提示", "网络未连接，请检查服务器是否启动后重试");
           return;
       }
       qDebug() << "网络已连接，准备发送注册请求";
    std::string strTel = tel.toStdString();
    //中文
    std::string strName = name.toStdString();//格式utf-8
    //获取邮箱字符串
    std::string strEmail = email.toStdString();

    STRU_REGISTER_RQ rq;
    //使用 strncpy 替代 strcpy，防止缓冲区溢出
    //STRU_REGISTER_RQ 中 m_tel/m_name/m_password 均为 char(60字节)
    strncpy(rq.m_tel , strTel.c_str() , sizeof(rq.m_tel) - 1);
    rq.m_tel[sizeof(rq.m_tel) - 1] = '\0';

    std::string strPassMD5 = GetMD5(pass);
    qDebug()<<strPassMD5.c_str();
    //兼容中文，使用 strncpy 防止昵称字段溢出
    strncpy(rq.m_name , strName.c_str() , sizeof(rq.m_name) - 1);
    rq.m_name[sizeof(rq.m_name) - 1] = '\0';

    strncpy(rq.m_password , strPassMD5.c_str() , sizeof(rq.m_password) - 1);
    rq.m_password[sizeof(rq.m_password) - 1] = '\0';

    //填充邮箱字段
    strncpy(rq.m_email , strEmail.c_str() , sizeof(rq.m_email) - 1);
    rq.m_email[sizeof(rq.m_email) - 1] = '\0';

    /*先转换std，后转换char**/
    /*发送*/
    m_pClient->SendData(0 , (char*)&rq, sizeof(rq));

}

//创建房间
void ckernel::slot_createroom()
{
    //判断是否在房间内 m_roomid
    if(m_roomid != 0)
    {
        QMessageBox::about(m_pVccDlg, "提示", "在房间内，无法创建，请先退出");
        return;
    }
    //发命令 创建房间
    STRU_CREATEROOM_RQ rq;
    rq.m_UserID = m_id;

    m_pClient->SendData( 0 , (char*)&rq , sizeof (rq));
}

#include "QRegExp"
#include <QInputDialog>
//加入房间
void ckernel::slot_joinroom()
{
    //判断是否在房间内 m_roomid
    if(m_roomid != 0)
    {
        QMessageBox::about(m_pVccDlg, "提示", "在房间内，无法加入，请先退出");
        return;
    }
    //弹出窗口 填房间号
    QString strRoom = QInputDialog::getText(m_pVccDlg , "加入房间", "输入房间号");
    //合理化判断
    QRegExp exp("^[0-9]\{1,8\}$");
    if(!exp.exactMatch(strRoom))
    {
        QMessageBox::about(m_pVccDlg, "提示", "房间号输入不合法,长度不超过8位");
        return;
    }
    qDebug()<<strRoom;
    //发命令 加入房间
    STRU_JOINROOM_RQ rq;
    rq.m_UserID = m_id;
    rq.m_RoomID = strRoom.toInt();

    m_pClient->SendData( 0 , (char*)&rq , sizeof (rq));
}

//退出房间
void ckernel::slot_quitRoom()
{


    //发退出包
    STRU_LEAVEROOM_RQ rq;
    rq.m_nUserID = m_id;
    rq.m_RoomID = m_roomid;
    //设置退出房间的用户名
    //m_UserName.toStdString()，并使用 strncpy 替代 strcpy 避免缓冲区溢出
    std::string name = m_UserName.toStdString();
    strncpy( rq.szUserName, name.c_str(), MAX_SIZE - 1 );
    rq.szUserName[MAX_SIZE - 1] = '\0';

    m_pClient->SendData( 0 , (char*)&rq , sizeof (rq));


    //关闭 音频 视频
    m_pAudioRead->pause();
    m_pVideoRead->slot_closeVideo();
    m_pScreenRead->slot_closeVideo();
    m_pRoomdialog->slot_setAudioCheck( false );
    m_pRoomdialog->slot_setVideoCheck( false );
    m_pRoomdialog->slot_setScreenCheck( false );


    //回收所有人的audiowrite
    for( auto ite = m_mapIDToAudioWrite.begin();ite!=m_mapIDToAudioWrite.end();)
    {
        AudioWrite * pWrite = ite->second;
        ite = m_mapIDToAudioWrite.erase( ite );
        delete  pWrite;
    }

    //回收资源
    m_pRoomdialog->slot_clearUserShow();

    // 重置 AVSyncManager 所有用户的时钟
    // 功能：退出房间时清理所有同步上下文
    // 调用时机：退出房间时
    AVSyncManager::getInstance().resetAllClocks();

    // 清理 H264 解码器池和分片重组缓冲区
    // 修改原因: 原代码在退出房间时不清理 m_mapIDToH264Decoder，
    //           导致解码器实例残留（内存泄漏），且重新进入房间时
    //           可能复用旧解码器的错误状态解码新流，导致解码失败或崩溃。
    // 修改内容: 遍历释放所有 H264Decoder 实例，清空 map 和重组缓冲区
    // 影响范围: 退出房间后内存正确释放，重新进入房间时创建新解码器
    for (auto &pair : m_mapIDToH264Decoder) {
        if (pair.second) {
            pair.second->close();
            delete pair.second;
            pair.second = nullptr;
        }
    }
    m_mapIDToH264Decoder.clear();
    m_h264Reassemble.clear();
    // PLI限流表清理
    // 修改原因: 退出房间后m_lastPliTime残留旧时间戳，重新进入房间时首个PLI
    //           可能被500ms限流窗口误拦（极端场景：退房后500ms内重新进房）
    // 修改内容: 退出房间时清空PLI限流表，与新房间状态对齐
    // 影响范围: 仅清空限流时间戳，不影响其他状态
    m_lastPliTime.clear();

    m_roomid = 0;
}


//暂停音频
void ckernel::slot_pauseAudio()
{
    m_pAudioRead->pause();
}

//开启音频
void ckernel::slot_startAudio()
{
    m_pAudioRead->start();
}
//开启视频
void ckernel::slot_startVideo()
{
    //恢复调试代码
    qDebug() << " slot_startVideo called, m_pVideoRead:" << (void*)m_pVideoRead;
    //endregion
    m_pVideoRead->slot_openVideo();

    // 活H264编码器初始化
    // 发送端编码器激活
    // 初始化H264Encoder实例，失败时自动回退到JPEG模式保证功能可用
    qDebug() << "=== H264编码器激活流程开始 ===";

    // 创建编码器实例（若尚未创建）
    if (!m_h264Encoder) {
        m_h264Encoder = new H264Encoder(this);
        qDebug() << "H264Encoder 实例已创建, ptr:" << (void*)m_h264Encoder;
    }

    // 编码器参数配置
    //  编码分辨率从640x480提升到1280x720，码率从1.2Mbps提升到3.0Mbps
    // 原值(SD): encWidth=640, encHeight=480, encBitrate=1200000
    // 更早的原值: encWidth=320, encHeight=240, encBitrate=200000
    // 修改原因: 达到HD级别目标，与videoread.h中IMAGE_WIDTH/IMAGE_HEIGHT一致
    // 影响范围: H264编码输出数据量增加约2.5倍，I帧从15-40KB增至50-150KB
    // SD回退值: 弱网环境下可回退到 encWidth=640, encHeight=480, encBitrate=1200000
    const int encWidth = 1280;       //  640 → 1280
    const int encHeight = 720;       //  480 → 720
    const int encFps = 30;
    const int encBitrate = 3000000;  //  1200000(1.2Mbps) → 3000000(3.0Mbps)

    // 初始化编码器（含硬件回退链：h264_nvenc -> h264_qsv -> libx264）
    if (!m_h264Encoder->isReady()) {
        // 编码器未初始化，正常初始化
        bool initOk = m_h264Encoder->init(encWidth, encHeight, encFps, encBitrate);
        if (initOk) {
            m_useH264 = true;
            qDebug() << "H264编码器初始化成功, 已启用H264编码路径"
                     << "尺寸:" << encWidth << "x" << encHeight
                     << "帧率:" << encFps << "码率:" << encBitrate;
        } else {
            m_useH264 = false;
            qWarning() << "error:H264编码器初始化失败, 回退到JPEG模式"
                       << "可能原因: FFmpeg库未正确加载或libx264编码器不可用";
        }
    } else if (m_h264Encoder->getWidth() != encWidth || m_h264Encoder->getHeight() != encHeight) {
        // 编码器已初始化但分辨率不匹配，需要重新初始化
        // 原Bug: slot_startScreen() 先于 slot_startVideo() 调用时，编码器以 1280x720 初始化。
        //        随后 slot_startVideo() 发现编码器已就绪就跳过初始化，导致 640x480 的视频帧
        //        被强行放大到 1280x720 编码，编码参数（帧率/码率）也不匹配，最终崩溃。
        // 修复: 检测分辨率不匹配时，销毁旧编码器并创建全新的编码器实例。
        //       使用 delete+new 替代 close()+init()，避免 FFmpeg 内部状态残留导致
        //       sws_scale 在新编码器上崩溃（close() 释放 codecCtx 后 FFmpeg 全局状态
        //       可能被污染，新创建的 SwsContext 在 sws_scale 时访问损坏内存）。
        qDebug() << "H264编码器分辨率不匹配, 当前:"
                 << m_h264Encoder->getWidth() << "x" << m_h264Encoder->getHeight()
                 << "期望:" << encWidth << "x" << encHeight
                 << ", 销毁旧编码器并创建新实例";

        // 原代码（保留为注释）: 使用 close()+init() 在同一对象上重新初始化
        /*
        m_h264Encoder->close();
        bool initOk = m_h264Encoder->init(encWidth, encHeight, encFps, encBitrate);
        */

        // 销毁旧编码器，创建全新实例，确保无残留状态
        delete m_h264Encoder;
        m_h264Encoder = new H264Encoder(this);

        bool initOk = m_h264Encoder->init(encWidth, encHeight, encFps, encBitrate);
        if (initOk) {
            m_useH264 = true;
            qDebug() << "H264编码器重新初始化成功, 尺寸:" << encWidth << "x" << encHeight
                     << "帧率:" << encFps << "码率:" << encBitrate;
        } else {
            m_useH264 = false;
            qWarning() << "error:H264编码器重新初始化失败, 回退到JPEG模式";
        }
    } else {
        // 编码器已就绪且分辨率匹配，直接复用
        m_useH264 = true;
        qDebug() << "H264编码器已就绪且分辨率匹配, 直接启用H264编码路径";
    }

    qDebug() << "=== 视频启动完成, 编码模式:"
             << (m_useH264 ? "H264" : "JPEG") << "===";
}

//暂停视频
void ckernel::slot_pauseVideo()
{
    m_pVideoRead->slot_closeVideo();
}

//开启桌面
void ckernel::slot_startScreen()
{
    m_pScreenRead->slot_openVideo();

    // 桌面共享启动时初始化H264编码器
    // 原Bug: slot_startScreen()仅启动屏幕采集定时器，未初始化H264编码器。
    //        如果用户未先开启视频就直接开启桌面共享，m_useH264为false，
    //        所有桌面共享帧都会走JPEG编码路径。
    // 修复: 在桌面共享启动时也初始化H264编码器，使用桌面共享分辨率。
    // 注意: 如果视频已经启动(编码器已初始化为640x480)，则复用现有编码器，
    //       1280x720的桌面帧会被h264encoder.cpp中的尺寸校验自动缩放到640x480。
    //       如果需要独立的桌面共享分辨率编码，需要重建编码器。
    qDebug() << "=== 桌面共享H264编码器激活流程开始 ===";

    // 桌面共享分辨率参数
    //  桌面共享分辨率从1280x720提升到1920x1080(FHD)，帧率从25fps降到20fps
    // 原值(SD): deskWidth=1280, deskHeight=720, deskFps=25, deskBitrate=2000000
    // 更早的原值: deskWidth=1024, deskHeight=768
    // 修改原因: 达到HD级别目标，桌面共享文字清晰度提升，帧率降低5fps控制带宽
    // 影响范围: 桌面共享帧大小增加约2倍，带宽增加，需配合TCP缓冲区扩大和H264分片增大
    const int deskWidth = 1920;       //  1280 → 1920
    const int deskHeight = 1080;      //  720 → 1080
    const int deskFps = 20;           //  25 → 20（降低5fps带宽控制）
    const int deskBitrate = 4000000;  //  2000000(2Mbps) → 4000000(4Mbps)

    // 如果编码器已初始化，检查分辨率是否匹配
    if (m_h264Encoder && m_h264Encoder->isReady()) {
        qDebug() << "H264编码器已初始化，桌面共享将复用现有编码器"
                 << "(帧会被自动缩放到编码器分辨率)";
        m_useH264 = true;
    } else {
        // 编码器未初始化，为桌面共享创建并初始化
        if (!m_h264Encoder) {
            m_h264Encoder = new H264Encoder(this);
            qDebug() << "H264Encoder 实例已创建(桌面共享), ptr:" << (void*)m_h264Encoder;
        }

        if (!m_h264Encoder->isReady()) {
            bool initOk = m_h264Encoder->init(deskWidth, deskHeight, deskFps, deskBitrate);
            if (initOk) {
                m_useH264 = true;
                qDebug() << "桌面共享H264编码器初始化成功, 已启用H264编码路径"
                         << "尺寸:" << deskWidth << "x" << deskHeight
                         << "帧率:" << deskFps << "码率:" << deskBitrate;
            } else {
                m_useH264 = false;
                qWarning() << "error:桌面共享H264编码器初始化失败, 回退到JPEG模式"
                           << "可能原因: FFmpeg库未正确加载或H264编码器不可用";
            }
        } else {
            m_useH264 = true;
        }
    }

    qDebug() << "=== 桌面共享启动完成, 编码模式:"
             << (m_useH264 ? "H264" : "JPEG") << "===";
}

//暂停桌面
void ckernel::slot_pauseScreen()
{
    m_pScreenRead->slot_closeVideo();
}


void ckernel::slot_refreshVideo(int id , QImage & img)
{
    m_pRoomdialog->slot_refreshUser( id , img );
}


///音频数据帧
///成员描述
///int type;
///int userId;
///int roomId;
///int min;
///int sec;
///int msec;
///QByteArray audioFrame;
//发送音频帧
void ckernel::slot_audioFrame(QByteArray ba)
{
    int nPackSize = 6*sizeof(int) + ba.size();
    char * buf = new char[nPackSize];
    char * tmp = buf;

    //序列化
    int type = DEF_PACK_AUDIO_FRAME;

    int userId = m_id;
    int roomId = m_roomid;
    QTime tm = QTime::currentTime();
    int min = tm.minute();
    int sec = tm.second();
    int msec = tm.msec();

    *(int*)tmp = type;
    tmp += sizeof (int);

    *(int*)tmp = userId;
    tmp += sizeof (int);

    *(int*)tmp = roomId;
    tmp += sizeof (int);

    *(int*)tmp = min;
    tmp += sizeof (int);

    *(int*)tmp = sec;
    tmp += sizeof (int);

    *(int*)tmp = msec;
    tmp += sizeof (int);

    memcpy( tmp , ba.data() , ba.size());


    // 音频帧通过音频连接发送，而非主连接
    // 修改原因: 原代码使用 m_pClient->SendData() 通过主连接发送音频帧，
    //           但 AudioRegister 是通过 m_pAVClient 发送的，
    //           导致服务端 m_audiofd 与实际接收音频数据的 fd 不一致。
    //           同时，主连接承载登录/房间管理等业务包，再叠加音频流会导致
    //           主连接负载过高，且服务端协议分发混乱。
    // 修改内容: 将 m_pClient 改为 m_pAVClient
    // 原代码: m_pClient->SendData( 0 , buf , nPackSize );
    m_pAVClient[ audio_client ]->SendData( 0 , buf , nPackSize );
    delete[] buf;

    // 更新本地音频主时钟（发送端同步使用）
    // 功能：本地音频采集帧发出时，更新 AVSyncManager 的本地音频时钟
    // 原理：本地音频时钟作为发送端视频同步的基准
    // 参考：AVSyncManager::updateLocalAudioClock 实现
    {
        double localAudioPts = (double)AVSyncManager::getCurrentTimeUs();
        AVSyncManager::getInstance().updateLocalAudioClock(localAudioPts);
    }
}

// 静音状态变化处理槽（接收 AudioRead::SIG_muteStateChanged 信号）
// 功能：记录日志 + 联动 RoomDialog 更新麦克风 UI 反馈
void ckernel::slot_muteStateChanged(bool isMute)
{
    qDebug() << "[Audio] 静音状态:" << (isMute ? "静音" : "有声");

    // 更新 UI 显示（麦克风静音状态反馈）
    if (m_pRoomdialog) {
        m_pRoomdialog->updateMuteStatus(isMute);
    }
}

//发送视频帧
void ckernel::slot_sendVideoFrame(QImage img)
{
    //恢复调试代码
    qDebug() << " slot_sendVideoFrame called, img size:" << img.width() << "x" << img.height() << ", m_id:" << m_id;
    //endregion

    // 本地预览必须最先执行，不能有任何阻塞操作
    // 修改原因: 原代码在 slot_refreshVideo 之前调用 AVSync QThread::msleep(100)，
    //           这会阻塞主线程100ms。由于定时器每23ms触发一次，多个定时器事件排队，
    //           主线程持续被 msleep 阻塞，paintEvent 永远得不到处理，导致视频画面不显示。
    // 修改内容: 将 slot_refreshVideo 移到函数最前面，确保本地预览立即调度重绘
    // 影响范围: 本地自预览画面能正常显示
    slot_refreshVideo( m_id , img );

    //  编码防堆积：若上一帧编码尚未完成，跳过当前帧的编码
    // 原Bug: QTimer::singleShot(0) 将编码延迟到事件队列，但 H264 编码耗时 30-100ms，
    //        而视频定时器每 23ms 触发一次。当编码耗时 > 23ms 时，encodeAndSendVideoFrame
    //        调用在事件队列中堆积，paintEvent 被饿死，导致视频画面无法刷新。
    // 修复: 用 m_encodingPending 标志防止堆积。若上一帧编码未完成，跳过当前帧编码，
    //        只保留本地预览（上面已调用 slot_refreshVideo），确保画面持续显示。
    // 原代码（保留为注释）:
    /*
    QTimer::singleShot(0, this, () {
        encodeAndSendVideoFrame(img);
    });
    */
    if (!m_encodingPending) {
        m_encodingPending = true;
        // 在 lambda 捕获前强制深拷贝 QImage，防止 use-after-free 崩溃
        // 原Bug: () 捕获 img 是浅拷贝（Qt 隐式共享），lambda 被投递到事件队列后，
        //        摄像头线程可能在 lambda 执行前释放或修改 img 的底层像素数据，
        //        导致 encode() 中 img.copy() 访问已释放内存，触发 SIGSEGV 崩溃。
        //        崩溃日志显示帧缓冲区测试全部通过，但读取 QImage 源数据时立即崩溃。
        // 修复: 用 img.copy() 创建深拷贝，确保 lambda 拥有独立的像素数据缓冲区。
        //       同时将 m_encodingPending = false 移到 encodeAndSendVideoFrame 之后，
        //       防止编码过程中新 lambda 被投递导致旧 QImage 被提前释放。
        // 原代码（保留为注释）:
        /*
        QTimer::singleShot(0, this, () {
            m_encodingPending = false;
            encodeAndSendVideoFrame(img);
        });
        */
        QImage imgCopy = img.copy();  // 深拷贝，独立像素数据
        QTimer::singleShot(0, this, [this, imgCopy]() {
            encodeAndSendVideoFrame(imgCopy);
            m_encodingPending = false;
        });
    }
}

// 编码+发送的实际执行函数（从slot_sendVideoFrame提取）
// 功能: H264/JPEG编码 + 分片打包 + 通过SIG_SendVideo信号投递到worker线程发送
// 调用时机: 由slot_sendVideoFrame通过QTimer::singleShot(0)延迟调用
//           确保paintEvent在编码前已执行，画面能正常显示
void ckernel::encodeAndSendVideoFrame(QImage img)
{
    // 移除发送端本地预览的 AVSync 阻塞等待
    // 修改原因: synchronizeLocalVideo 返回的 waitMs 恒为 100ms（因为 video_pts 比
    //           audioClock 快约979ms，被 MAX_WAIT_MS=100 截断），QThread::msleep(100)
    //           阻塞主线程，导致:
    //           1. paintEvent 被饿死，视频画面不刷新
    //           2. UI 事件循环被阻塞，界面卡死
    //           3. 定时器事件堆积，每帧处理时间 >100ms，实际帧率降至 <10fps
    //           本地预览无需音视频同步——用户看自己的画面是实时的，没有网络延迟。
    //           AVSync 只对接收端的远端视频有意义（slot_dealVideoFrameRq 中已实现）。
    // 修改内容: 注释掉 synchronizeLocalVideo + QThread::msleep 块
    // 影响范围: 本地预览不再阻塞主线程，远程接收端的 AVSync 不受影响
    /* 已移除 - 本地预览不需要 AVSync 阻塞
    {
        double video_pts = (double)AVSyncManager::getCurrentTimeUs();
        double frameDelay = 33333.0;  // 30fps = 33333微秒
        int waitMs = AVSyncManager::getInstance().synchronizeLocalVideo(video_pts, frameDelay);
        if (waitMs > 0)
        {
            QThread::msleep(waitMs);
        }
    }
    */
    //压缩
    //压缩图片从 RGB24 格式压缩到 JPEG 格式, 发送出去
    /* 原有代码 - JPEG 编码路径（保留兼容性，可作为回退）
    QByteArray ba;
    QBuffer qbuf(&ba); // QBuffer 与 QByteArray 字节数组联立联系
    img.save( &qbuf , "JPEG" , 50 ); //将图片的数据写入 ba
    //使用 ba 对象, 可以获取图片对应的缓冲区可以使用 ba.data() , ba.size()将缓冲区发送出去
    end 原有代码 */

    // H.264编码路径
    // 当 m_useH264=true 时使用 H.264 编码，否则回退到 JPEG
    QByteArray ba;
    int frameType = 1;  // 默认 P 帧
    // 引入局部标志，区分本帧实际使用的编码格式
    // 原Bug: 当H264编码失败回退到JPEG时，m_useH264仍为true，
    //        导致packType和headerSize使用H264格式，接收端会按H264解码JPEG数据。
    // 修复: 用局部变量跟踪本帧实际编码格式，确保packType/headerSize与数据格式一致。
    bool useH264ForThisFrame = false;
    if (m_useH264 && m_h264Encoder && m_h264Encoder->isReady()) {
        // H.264 编码
        ba = m_h264Encoder->encode(img);
        // 判断是否 I 帧（首字节 NAL header 0x67/0x65 表示 I 帧）
        // 将 char 转为 uint8_t 再比较，避免有符号比较问题
        // 原Bug: ba.at(0) 返回 char（有符号），0x67/0x65 是 int。
        //        当 char 值为 0x65(101) 时，在某些编译器上会被符号扩展为 0xFFFFFF65，
        //        与 int 0x65 比较永远为 false，导致 I 帧被误判为 P 帧。
        // 修复: 使用 (uint8_t)ba.at(0) 进行无符号比较。
        //        0x67=SPS(表示新GOP开始), 0x65=IDR(I帧), 0x25=非IDR的I型slice
        if (!ba.isEmpty()) {
            useH264ForThisFrame = true;  // H264编码成功，标记本帧使用H264格式
            uint8_t nalu = (uint8_t)ba.at(0);
            // 检查4字节起码码(00 00 00 01)后的NAL type
            uint8_t nalu4 = (ba.size() >= 5) ? (uint8_t)ba.at(4) : 0;
            // 检查3字节起始码(00 00 01)后的NAL type
            uint8_t nalu3 = (ba.size() >= 4) ? (uint8_t)ba.at(3) : 0;
            if (nalu == 0x67 || nalu == 0x65 || nalu == 0x25
                || nalu4 == 0x67 || nalu4 == 0x65 || nalu4 == 0x25
                || nalu3 == 0x67 || nalu3 == 0x65 || nalu3 == 0x25) {
                frameType = 0;  // I 帧
            }
        } else {
            // H264编码返回空数据时回退到JPEG编码
            // 原Bug: encode()可能因内部错误返回空QByteArray（如QImage转换失败、
            //        av_frame_make_writable失败等），此时ba.isEmpty()为true，
            //        后续发送逻辑会创建一个数据长度为0的分片包，接收端无法解码。
            // 修复: 编码失败时回退到JPEG编码，确保每帧都有有效数据可发送。
            qWarning() << "H264编码返回空数据, 回退到JPEG编码";
            ba.clear();
            QBuffer qbuf(&ba);
            img.save(&qbuf, "JPEG", 75);  //  70 → 75，提升JPEG回退画质
            frameType = 0;
        }
    } else {
        // 回退到 JPEG 路径
        QBuffer qbuf(&ba);
        //  JPEG质量从70提升到75
        // 原值(SD): quality=70
        // 更早的原值: quality=50
        // 修改原因: HD级别要求更高的JPEG回退画质，75在文件大小和画质间取得平衡
        // 影响范围: JPEG回退路径帧大小增加约10%，仅在H264编码器不可用时触发
        img.save(&qbuf, "JPEG", 75);  //  70 → 75
        frameType = 0;  // JPEG 视为 I 帧（每帧独立）
    }

    // 视频包类型选择
    // 使用 useH264ForThisFrame 替代 m_useH264，确保H264编码失败回退JPEG时使用正确的包格式
    int packType = useH264ForThisFrame ? DEF_PACK_VIDEO_FRAME_H264 : DEF_PACK_VIDEO_FRAME;
    int headerSize = useH264ForThisFrame ? sizeof(STRU_VIDEO_FRAME_H264_HEADER) : 6 * sizeof(int);

    //写视频帧，发送

    ///视频数据帧
    ///成员描述
    ///int type;
    ///int userId;
    ///int roomId;
    ///int min;
    ///int sec;
    ///int msec;
    ///QByteArray audioFrame;
    // 启用H264分片机制
    // 原值: m_totalFragments=1（不分片，H264单帧<4KB时可行）
    // 修改原因: 640x480 H264 I帧可达15-40KB，必须按H264_FRAGMENT_SIZE(4096字节)分片发送
    // 影响范围: 视频帧从单包发送改为多包分片发送，接收端需按frameSeq+fragmentIndex重组
    // 分片重组超时: H264_REASSEMBLE_TIMEOUT=100ms（packdef.h已定义）

    // 使用 useH264ForThisFrame 替代 m_useH264，确保H264编码失败回退JPEG时走JPEG路径
    if (useH264ForThisFrame) {
        // H.264 分片发送逻辑
        // 计算分片数量：向上取整(总数据大小 / 分片大小)
        int totalFragments = (ba.size() + H264_FRAGMENT_SIZE - 1) / H264_FRAGMENT_SIZE;
        if (totalFragments < 1) totalFragments = 1;
        int currentFrameSeq = m_h264FrameSeq++;
        QTime tm = QTime::currentTime();

        // 记录H264分片发送概要
        qDebug() << "[slot_sendVideoFrame] H264 encode: ba.size=" << ba.size()
                 << "totalFragments=" << totalFragments
                 << "frameSeq=" << currentFrameSeq
                 << "frameType=" << frameType
                 << "m_id=" << m_id
                 << "m_roomid=" << m_roomid;

        for (int fragIdx = 0; fragIdx < totalFragments; fragIdx++) {
            // 计算当前分片的数据偏移和大小
            int offset = fragIdx * H264_FRAGMENT_SIZE;
            int fragDataSize = qMin(H264_FRAGMENT_SIZE, (int)ba.size() - offset);

            // 分配单个分片的包缓冲区
            int fragPackSize = sizeof(STRU_VIDEO_FRAME_H264_HEADER) + fragDataSize;
            char *fragBuf = new char[fragPackSize];
            char *fragTmp = fragBuf;

            // 填充H264分片包头
            STRU_VIDEO_FRAME_H264_HEADER *header = (STRU_VIDEO_FRAME_H264_HEADER*)fragTmp;
            header->m_nType = DEF_PACK_VIDEO_FRAME_H264;
            header->m_userid = m_id;
            header->m_roomid = m_roomid;
            header->m_min = tm.minute();
            header->m_sec = tm.second();
            header->m_msec = tm.msec();
            header->m_frameType = frameType;
            header->m_frameSeq = currentFrameSeq;
            header->m_totalFragments = totalFragments;
            header->m_fragmentIndex = fragIdx;
            fragTmp += sizeof(STRU_VIDEO_FRAME_H264_HEADER);

            // 拷贝当前分片的H264数据
            memcpy(fragTmp, ba.data() + offset, fragDataSize);

            // 通过信号发送到SendVideoWorker线程
            Q_EMIT SIG_SendVideo(fragBuf, fragPackSize);
        }
    } else {
        // 原有 JPEG 单包发送逻辑（保留，JPEG路径不需要分片）
        // 记录JPEG路径的包信息
        qDebug() << "[slot_sendVideoFrame] JPEG encode: ba.size=" << ba.size()
                 << "packType=" << packType
                 << "m_id=" << m_id
                 << "m_roomid=" << m_roomid;

        int nPackSize = headerSize + ba.size();
        char * buf = new char[nPackSize];
        char * tmp = buf;

        *(int*)tmp = packType;
        tmp += sizeof (int);
        *(int*)tmp = m_id;
        tmp += sizeof (int);
        *(int*)tmp = m_roomid;
        tmp += sizeof (int);

        //用于延迟过久舍弃一些帧的参考时间
        QTime tm = QTime::currentTime();
        *(int*)tmp = tm.minute();
        tmp += sizeof (int);
        *(int*)tmp = tm.second();
        tmp += sizeof (int);
        *(int*)tmp = tm.msec();
        tmp += sizeof (int);

        memcpy( tmp , ba.data() , ba.size() );
        Q_EMIT SIG_SendVideo( buf , nPackSize );
    }

    // 以下旧代码已废弃
    // 原因: H264路径已改为分片发送(上方for循环)，JPEG路径已在else分支内直接发送
    // 原代码: Q_EMIT SIG_SendVideo( buf , nPackSize ); -- 变量buf/nPackSize作用域已不在此处
    //    m_pClient->SendData( 0 , buf , nPackSize );
    //    delete   buf;
}

void ckernel::slot_SendVideo(char *buf, int nlen)
{
    // 记录进入slot_SendVideo的包信息
    int * pType = (int*)buf;
    int packType = *pType;
    qDebug() << "[slot_SendVideo] enter, packType=" << packType
             << "nlen=" << nlen
             << "thread=" << QThread::currentThreadId();

    char * tmp = buf;
    tmp += sizeof (int);
    tmp += sizeof (int);
    tmp += sizeof (int);

    int min = *(int*)tmp;
    tmp += sizeof (int);
    int sec = *(int*)tmp;
    tmp += sizeof (int);
    int msec = *(int*)tmp;
    tmp += sizeof (int);

    //当前时间
    QTime ctm = QTime::currentTime();
    //数据包的时间
    QTime tm(ctm.hour() , min , sec , msec );

    // 将延迟阈值从 300ms 提升到 2000ms
    // 修改原因: 原代码延迟阈值 300ms 是针对 JPEG 单包发送设计的。
    //           H264 模式下每帧分3-10个分片，通过 Queued 信号投递到 worker 线程。
    //           当事件队列积压时，后续分片的排队延迟可能超过 300ms 被全部丢弃，
    //           导致接收端分片重组永远收不齐，视频功能完全不可用。
    // 修改内容: 将阈值从 300 改为 2000，覆盖 H264 编码 + 多分片排队 + 网络发送的总延迟。
    // 原代码: if( tm.msecsTo( ctm ) > 300)
    int delayMs = tm.msecsTo( ctm );
    if( delayMs > 2000)
    {
        qDebug() << "[slot_SendVideo] DROP: delay=" << delayMs << "ms, packType=" << packType;
        delete []  buf;
        return;
    }

    // 将 SendData 直接在工作线程执行，不再投递回UI线程
    // 原Bug: 原代码通过 QTimer::singleShot(0, this, lambda) 将 SendData 投递到UI线程执行，
    //        导致阻塞式 send() 在UI线程运行。当发送缓冲区满时，send() 阻塞UI线程，
    //        paintEvent 得不到处理，视频画面无法刷新（"视频不显示"的根因）。
    // 修复: 直接在工作线程调用 SendData。TcpClient::SendData 仅调用 send()，
    //       不涉及重连逻辑，线程安全。配合 slot_destroy 中先停止 worker 再释放
    //       m_pAVClient 的顺序保证，消除跨线程风险。
    // 原代码（保留为注释）:
    /*
    QTimer::singleShot(0, this, () {
        bool sendOk = m_pAVClient->SendData(0 , buf , nlen );
        if( !sendOk )
        {
            qDebug() << " WARNING: SendData FAILED, packType=" << packType
                     << "nlen=" << nlen
                     << "IsConnected=" << m_pAVClient->IsConnected();
        }
        else
        {
            qDebug() << " SendData OK, packType=" << packType;
        }
        //回收缓冲区（在主线程释放，与分配线程一致）
        delete   buf;
    });
    */

    // 直接在工作线程调用 SendData，避免阻塞UI线程
    bool sendOk = m_pAVClient[ video_client ]->SendData(0 , buf , nlen );
    if( !sendOk )
    {
        qDebug() << "[slot_SendVideo] WARNING: SendData FAILED, packType=" << packType
                 << "nlen=" << nlen
                 << "IsConnected=" << m_pAVClient[video_client]->IsConnected();
    }
    else
    {
        qDebug() << "[slot_SendVideo] SendData OK, packType=" << packType;
    }
    delete [] buf;

}

//网络处理
void ckernel::slot_dealData(uint sock, char *buf, int nlen)
{
     qDebug()<<__func__;
    //网络数据包合法性校验
    //原代码直接 *(int*)buf 读取协议头，未校验 buf 是否为空、nlen 是否足够
    //恶意构造的短包或空包会导致越界读取，引发崩溃或信息泄露
    //修复：增加空指针和最小长度校验
    if( !buf || nlen < (int)sizeof(int) )
    {
        qDebug() << " 收到非法数据包: buf=" << buf << " nlen=" << nlen;
        delete[] buf;
        return;
    }
    int type = *(int*)buf;
    if( type >= DEF_PACK_BASE && type < DEF_PACK_BASE + DEF_PACK_COUNT)
    {
        //取得协议头，根据协议映射关系，找到函数指针
        PFUN pf = NetPackMap( type );
        if(pf)
        {
            (this->*pf)(sock, buf, nlen );
        }
    }
    delete[] buf;

}


//登录回复处理
void ckernel::slot_dealLoginRS(uint sock, char *buf, int nlen)
{
    qDebug()<<__func__;
    //拆包
    STRU_LOGIN_RS * rs = (STRU_LOGIN_RS*)buf;
    //根据返回结果，得到不同信息
    switch (rs->m_lresult)
    {
    case user_not_exist:
        QMessageBox::about(m_pLoginDlg , "提示" , "用户不存在，登录失败");
        break;
    case password_error:
        QMessageBox::about(m_pLoginDlg , "提示" , "密码错误，登录失败");
        break;
    case login_success:
    {
        //原代码 QString("用户登录成功").arg(rs->m_name)
        //arg(char*) 会将 char* 当作 Latin-1 编码转换为 QString，
        //而服务端发送的 m_name 是 UTF-8 编码的中文（服务端是Linux环境，数据库和源文件均为UTF-8），
        //导致中文显示为乱码。
        //修复：先用 fromUtf8 将 char* 正确解码为 QString，再 arg
        QString strName = QString::fromUtf8( rs->m_name );
        QString strMsg = QString("用户[%1]登录成功").arg( strName );
        QMessageBox::about(m_pLoginDlg , "提示" , strMsg);
        //id记录
        //        m_name = QString::fromStdString( rs->m_name );
        //        m_pVccDlg->setInfo(m_name);


        m_id = rs->m_userid;
        //保存当前登录用户名，供创建房间时设置 UserShow 使用
        //使用 fromUtf8 正确解码服务端发送的UTF-8中文用户名
        m_UserName = QString::fromUtf8( rs->m_name );
        //ui跳转
        m_pLoginDlg->hide();
        m_pVccDlg->showNormal();

        //注册音频和视频的fd
        STRU_AUDIO_REGISTER rq_audio;
        rq_audio.m_userid = m_id;

        STRU_VIDEO_REGISTER rq_video;
        rq_video.m_userid = m_id;

        // 记录AV连接状态和注册包发送结果
        qDebug() << "[LoginRS] AV connection status:"
                 << "audio_connected=" << m_pAVClient[audio_client]->IsConnected()
                 << "video_connected=" << m_pAVClient[video_client]->IsConnected()
                 << "m_id=" << m_id;

        bool audioRegOk = m_pAVClient[audio_client]->SendData( 0 , (char*)&rq_audio , sizeof (rq_audio));
        bool videoRegOk = m_pAVClient[video_client]->SendData( 0 , (char*)&rq_video , sizeof (rq_video));

        qDebug() << "[LoginRS] VIDEO_REGISTER send result:" << videoRegOk
                 << "AUDIO_REGISTER send result:" << audioRegOk;

    }

        break;
    //登录成功但邮箱为空，需要补全邮箱
    case login_success_no_email:
    {
        QString strName = QString::fromUtf8( rs->m_name );
        QString strMsg = QString("用户[%1]登录成功\n\n您的账号尚未填写邮箱，请补全邮箱信息。").arg( strName );
        QMessageBox::about(m_pLoginDlg , "提示" , strMsg);

        m_id = rs->m_userid;
        m_UserName = QString::fromUtf8( rs->m_name );

        //弹出补全邮箱对话框，循环直到用户输入有效邮箱或取消
        bool ok = false;
        while( true )
        {
            QString email = QInputDialog::getText(m_pLoginDlg, tr("补全邮箱"),
                tr("请输入您的邮箱地址（可用于后续登录）:"),
                QLineEdit::Normal, "", &ok);
            if( !ok || email.isEmpty() )
            {
                //用户取消，直接进入主界面（不强制补全）
                break;
            }
            //简单校验邮箱格式
            QRegExp regEmail("^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}$");
            if( !regEmail.exactMatch(email) )
            {
                QMessageBox::about(m_pLoginDlg, "提示", "邮箱格式错误，请重新输入");
                continue;
            }
            //发送补全邮箱请求到服务端
            STRU_UPDATE_EMAIL_RQ rq;
            rq.m_userid = m_id;
            std::string strEmail = email.toStdString();
            strncpy(rq.m_email, strEmail.c_str(), sizeof(rq.m_email) - 1);
            rq.m_email[sizeof(rq.m_email) - 1] = '\0';
            m_pClient->SendData(0, (char*)&rq, sizeof(rq));
            //提示用户
            QMessageBox::about(m_pLoginDlg, "提示", "邮箱已提交，服务端验证中...");
            break;
        }

        //ui跳转
        m_pLoginDlg->hide();
        m_pVccDlg->showNormal();

        //注册音频和视频的fd
        STRU_AUDIO_REGISTER rq_audio;
        rq_audio.m_userid = m_id;
        STRU_VIDEO_REGISTER rq_video;
        rq_video.m_userid = m_id;
        m_pAVClient[audio_client]->SendData( 0 , (char*)&rq_audio , sizeof (rq_audio));
        m_pAVClient[video_client]->SendData( 0 , (char*)&rq_video , sizeof (rq_video));
    }
        break;
    default:
        break;

    }
}

//注册回复处理
void ckernel::slot_dealRegisterRS(uint sock, char *buf, int nlen)
{
     qDebug()<<__func__;
    //拆包
    STRU_REGISTER_RS * rs = (STRU_REGISTER_RS *)buf;
    //根据不同的结果，弹出不同的提示窗
    switch (rs->m_lresult)
    {
    case tel_is_exist:
        QMessageBox::about(m_pLoginDlg , "提示" , "手机号已存在，注册失败");
        break;
    case register_success:
        QMessageBox::about(m_pLoginDlg , "提示" , "注册成功");
        break;
    case name_is_exist:
        QMessageBox::about(m_pLoginDlg , "提示" , "昵称已存在，注册失败");
        break;
    case register_failed:
        // 服务端写库失败。此前服务端会谎报"注册成功"，用户以为注册好了却登不上。
        QMessageBox::about(m_pLoginDlg , "提示" , "注册失败，请稍后重试（若昵称过长请改短）");
        break;
    default:
        break;

    }

}

//创建房间回复处理
void ckernel::slot_dealCreateRoomRS(uint sock, char *buf, int nlen)
{
    //拆包
    STRU_CREATEROOM_RS* rs = (STRU_CREATEROOM_RS*)buf;
    //房间号 显示到界面 跳转
    m_pRoomdialog->slot_setInfo(QString::number(rs->m_RoomID));

    //把自己的信息放到房间里 做显示 todo
    UserShow * user = new UserShow;
    connect( user , SIGNAL(SIG_itemClicked( int , QString )) , m_pRoomdialog , SLOT(slot_setBigImgId(int , QString )));
    //自己的 UserShow 的 m_id 未被设置（默认为0）。
    //这会导致两个问题：
    //slot_addUserShow 中以 m_id 为键添加到 map，m_id 为0会导致 key 错误
    //自己的视频帧 slot_sendVideoFrame -> slot_refreshVideo(m_id, img) -> slot_refreshUser(m_id, img) 查找 map 时，因 key 不匹配无法显示
    //修复：正确设置自己的用户ID和用户名
    user->slot_setInfo( m_id , m_UserName );
    m_pRoomdialog->slot_addUserShow( user );

    m_roomid = rs->m_RoomID;
    m_pRoomdialog->showNormal();

    //音频 初始化
    m_pRoomdialog->slot_setAudioCheck( false );
    //视频初始化
    m_pRoomdialog->slot_setVideoCheck( false );

    // 为自己添加 AVSyncManager 同步上下文
    // 功能：创建房间时，将本地用户添加到同步管理器
    // 调用时机：创建房间成功后
    AVSyncManager::getInstance().addUser(m_id);

}

//加入房间回复
void ckernel::slot_dealJoinRoomRS(uint sock, char *buf, int nlen)
{
    //拆包
    STRU_JOINROOM_RS* rs = (STRU_JOINROOM_RS*)buf;
    //根据结果 失败 提示
    if( rs->m_lresult == 0)
    {
        QMessageBox::about(m_pVccDlg , "提示", "房间不存在，加入失败");
        return;
    }
    //成功
    //加入房间前先清空旧的成员列表，防止上次房间的残留数据
    //原代码缺少此步骤，若用户先退出再加入新房间，会显示旧房间成员
    m_pRoomdialog->slot_clearUserShow();
    //清空音频播放对象map，防止悬空指针
    for( auto ite = m_mapIDToAudioWrite.begin(); ite != m_mapIDToAudioWrite.end();)
    {
        AudioWrite * pWrite = ite->second;
        ite = m_mapIDToAudioWrite.erase( ite );
        delete  pWrite;
    }

    //房间号 显示到界面 跳转
    m_pRoomdialog->slot_setInfo(QString::number(rs->m_RoomID));
    //跳转 roomid设置
    m_roomid = rs->m_RoomID;

    //原代码加入房间成功后，没有把自己添加到 UserShow 列表
    //这导致加入方在房间界面看不到自己，也无法显示自己的视频/音频
    //对比 slot_dealCreateRoomRS 中有添加自己的逻辑，这里补上
    UserShow * user = new UserShow;
    connect( user , SIGNAL(SIG_itemClicked( int , QString )) , m_pRoomdialog , SLOT(slot_setBigImgId(int , QString )));
    user->slot_setInfo( m_id , m_UserName );
    m_pRoomdialog->slot_addUserShow( user );

    //为自己创建音频播放对象
    if(m_mapIDToAudioWrite.count( m_id ) == 0)
    {
        m_mapIDToAudioWrite[m_id] = new AudioWrite;
    }

    m_pRoomdialog->showNormal();

    //音频 初始化
    m_pRoomdialog->slot_setAudioCheck( false );
    //视频初始化
    m_pRoomdialog->slot_setVideoCheck( false );
    //桌面共享初始化
    m_pRoomdialog->slot_setScreenCheck( false );

    // 为自己添加 AVSyncManager 同步上下文
    // 功能：加入房间时，将本地用户添加到同步管理器
    // 调用时机：加入房间成功后
    AVSyncManager::getInstance().addUser(m_id);

}

//房间成员请求处理
void ckernel::slot_dealRoomMemberRq(uint sock, char *buf, int nlen)
{
    //拆包
    STRU_ROOM_MEMBER_RQ *rq = (STRU_ROOM_MEMBER_RQ*)buf;
    //原代码每次收到成员请求都创建新的 UserShow，没有检查是否已存在
    //当服务端重复推送同一成员时，会创建多个相同 id 的 UserShow 控件，导致界面混乱
    //修复：先检查 m_mapIDToUserShow 中是否已存在该用户，存在则跳过
    //通过 RoomDialog 的 slot_hasUser 检查
    if( m_pRoomdialog->slot_hasUser( rq->m_UserID ) )
    {
        //已存在该用户，不重复添加
        //但音频播放对象仍需检查（防止首次添加时音频对象未创建）
        if(m_mapIDToAudioWrite.count( rq->m_UserID ) == 0)
        {
            m_mapIDToAudioWrite[rq->m_UserID] = new AudioWrite;
        }
        return;
    }
    //创建用户对应的控件
    UserShow * user = new UserShow;
    //原代码 QString::fromStdString(rq->m_szUser) 假设 UTF-8 编码
    //修复：使用 fromUtf8 正确解码UTF-8编码的中文用户名
    user->slot_setInfo( rq->m_UserID , QString::fromUtf8( rq->m_szUser ) );
    connect( user , SIGNAL(SIG_itemClicked( int , QString )) , m_pRoomdialog , SLOT(slot_setBigImgId(int , QString )));
    m_pRoomdialog->slot_addUserShow( user );

    //音频的内容
    AudioWrite * aw = NULL;
    //为每个人创建一个播放音频的对象
    if(m_mapIDToAudioWrite.count( rq->m_UserID ) == 0)
    {
        aw = new AudioWrite;
        m_mapIDToAudioWrite[rq->m_UserID] = aw;
    }

    // 为新成员添加 AVSyncManager 同步上下文
    // 功能：每个远端用户独立维护一套 audio_clock / video_clock
    // 调用时机：收到新成员加入房间时
    AVSyncManager::getInstance().addUser(rq->m_UserID);

    //视频的内容


}

//离开房间的请求处理
void ckernel::slot_dealLeaveRoomRq(uint sock, char *buf, int nlen)
{
    //拆包
    STRU_LEAVEROOM_RQ * rq = (STRU_LEAVEROOM_RQ*)buf;
    //把这个人从ui上去掉
    if( rq->m_RoomID == m_roomid)
    {
        m_pRoomdialog->slot_removeUserShow(rq->m_nUserID);
    }

    //内存泄漏修复:去掉对应的音频播放对象
    //原代码问题：delete pAw 后未从 m_mapIDToAudioWrite 中删除对应条目，导致 map 中残留指向已释放内存的悬空指针(dangling pointer)。
    //后续若同一 userID 再次触发逻辑，会访问已释放内存，引发未定义行为；
    //且 map 条目永不清理，造成内存泄漏。
    //修复：先从 map 中删除条目（erase 会返回下一个迭代器），再 delete 对象
    auto ite = m_mapIDToAudioWrite.find( rq->m_nUserID );
    if( ite != m_mapIDToAudioWrite.end() )
    {
        AudioWrite * pAw = ite->second;
        m_mapIDToAudioWrite.erase( ite ); //先从map移除，避免悬空指针
        delete pAw;                       //再释放内存
    }

    // 移除离开成员的 AVSyncManager 同步上下文
    // 功能：清理该用户的 audio_clock / video_clock
    // 调用时机：成员离开房间时
    AVSyncManager::getInstance().removeUser(rq->m_nUserID);

}

//音频帧处理
void ckernel::slot_dealAudioFrameRq(uint sock, char *buf, int nlen)
{
    //反序列化
    char * tmp = buf;
    //跳过协议类型
    tmp += sizeof (int);
    int userId = *(int*)tmp;
    tmp += sizeof (int);
    int roomId = *(int*)tmp;
    tmp += sizeof (int);

    //跳过时间戳字段 min, sec, msec
    tmp += sizeof (int);
    tmp += sizeof (int);
    tmp += sizeof (int);

    int nbufLen = nlen - 6*sizeof (int);
    QByteArray ba( tmp , nbufLen );

    if(m_roomid == roomId)
    {
        if(m_mapIDToAudioWrite.count( userId )>0)
        {
            AudioWrite * aw = m_mapIDToAudioWrite[userId];
            aw->slot_playAudio( ba );
        }
    }


    // 同步逻辑已抽离到 AVSyncManager 独立模块，支持多用户独立时钟
    // 调用 AVSyncManager::updateAudioClock(userId, timestamp) 更新指定用户的音频时钟
    /* 原有代码：
    // 更新音频主时钟
    // 策略：每收到一帧音频数据，用当前系统时间更新audio_clock
    // 原理：音频以恒定速率播放（8kHz/20ms），audio_clock作为主基准驱动视频同步
    // 参考MediaPlayer/videoplayer.cpp:688-696的audio_clock更新逻辑
    {
        QTime now = QTime::currentTime();
        // 转换为微秒：(小时*3600 + 分*60 + 秒)*1000000 + 毫秒*1000
        m_audioClock = (double)(
            now.hour() * 3600000000.0 +
            now.minute() * 60000000.0 +
            now.second() * 1000000.0 +
            now.msec() * 1000.0
        );
    }
    */

    // 使用 AVSyncManager 更新指定用户的音频主时钟
    // 功能：每收到一帧音频数据，用当前系统时间更新该用户的 audio_clock
    // 原理：音频以恒定速率播放（8kHz/20ms），audio_clock 作为主基准驱动视频同步
    // 参数：userId - 音频帧来源用户ID, timestamp - 当前系统时间（微秒）
    {
        double timestamp = (double)AVSyncManager::getCurrentTimeUs();
        AVSyncManager::getInstance().updateAudioClock(userId, timestamp);
    }
}

//视频帧处理
void ckernel::slot_dealVideoFrameRq(uint sock, char *buf, int nlen)
{
    //反序列化
    char * tmp = buf;
    //跳过协议类型
    tmp += sizeof ( int );
    //提取 userId
    int userId = *(int*) tmp;
    tmp += sizeof ( int );
    //提取 roomId
    int roomId = *(int*) tmp;
    tmp += sizeof ( int );

    //跳过时间戳字段 min, sec, msec
    tmp += sizeof ( int );
    tmp += sizeof ( int );
    tmp += sizeof ( int );

    int datalen = nlen - 6*sizeof (int);
    QByteArray bt(tmp , datalen);

    QImage img;
    img.loadFromData( bt );

    if(m_roomid == roomId)
    {
        // 先显示视频帧，再做 AVSync 同步等待
        // 修改原因: 原代码先调用 QThread::msleep(waitMs) 阻塞主线程，再显示视频帧。
        //           如果 waitMs=100ms，主线程被阻塞，paintEvent 得不到处理，视频不刷新。
        //           且多个视频帧到达时会累积阻塞，导致界面完全卡死。
        // 修改内容: 将 slot_refreshUser 移到 AVSync 之前，先显示再同步。
        //           AVSync 的 waitMs 仅用于更新内部时钟状态，不再阻塞主线程。
        // 影响范围: 远端视频画面能正常刷新，AVSync 时钟仍被正确更新
        m_pRoomdialog->slot_refreshUser( userId , img);

        // AVSync: 更新视频时钟状态（不再阻塞主线程）
        // 注意: synchronizeVideo 内部会更新 videoClock，供后续帧的同步判断使用
        //       这里仅调用以更新时钟，忽略返回的 waitMs（不阻塞）
        {
            double video_pts = (double)AVSyncManager::getCurrentTimeUs();
            double frameDelay = 33333.0;  // 30fps = 33333微秒
            AVSyncManager::getInstance().synchronizeVideo(userId, video_pts, frameDelay);
            // QThread::msleep(waitMs) — 阻塞主线程导致视频不刷新
        }
    }
}

//补全邮箱回复处理
void ckernel::slot_dealUpdateEmailRS(uint sock, char *buf, int nlen)
{
    qDebug()<<__func__;
    //拆包
    STRU_UPDATE_EMAIL_RS * rs = (STRU_UPDATE_EMAIL_RS *)buf;
    if( rs->m_lresult == 1 )
    {
        QMessageBox::about(m_pVccDlg, "提示", "邮箱补全成功！");
    }
    else
    {
        QMessageBox::about(m_pVccDlg, "提示", "邮箱补全失败，邮箱格式错误或已被使用");
    }
}

// H.264视频帧处理函数
// 处理接收到的 H.264 视频帧包，与 JPEG 路径 slot_dealVideoFrameRq 共存
// 流程: 拆包 -> 分片重组 -> 查找/创建解码器 -> 解码 -> 触发刷新 UI
void ckernel::slot_dealVideoFrameH264Rq(uint sock, char *buf, int nlen)
{
    if (nlen < (int)sizeof(STRU_VIDEO_FRAME_H264_HEADER)) {
        qWarning() << "slot_dealVideoFrameH264Rq: packet too small," << nlen;
        return;
    }

    // 1. 解析包头
    STRU_VIDEO_FRAME_H264_HEADER *header = (STRU_VIDEO_FRAME_H264_HEADER*)buf;
    int userId = header->m_userid;
    int roomId = header->m_roomid;

    // 房间号不匹配则丢弃
    if (m_roomid != roomId) {
        return;
    }

    // 2. 提取 H.264 数据
    int dataLen = nlen - sizeof(STRU_VIDEO_FRAME_H264_HEADER);
    if (dataLen <= 0) {
        return;
    }
    QByteArray h264Data(buf + sizeof(STRU_VIDEO_FRAME_H264_HEADER), dataLen);

    // H.264分片重组逻辑
    // 修改原因: 发送端将H264帧按4KB分片发送(每帧3-10个分片包)，
    //           原代码直接把单个分片当完整帧送入解码器，导致解码失败。
    // 修改内容: 新增分片重组缓冲区，收集同一frameSeq的所有分片后拼接为完整帧再解码。
    // 重组策略:
    //   1. 按userId+frameSeq索引分片组
    //   2. 收齐所有分片(totalFragments==receivedCount)后拼接
    //   3. 超时清理: 超过H264_REASSEMBLE_TIMEOUT(100ms)未收齐的分片组丢弃
    int frameSeq = header->m_frameSeq;
    int totalFragments = header->m_totalFragments;
    int fragmentIndex = header->m_fragmentIndex;

    // 分片字段边界校验
    // 修改原因: fragmentIndex/totalFragments来自网络包，原代码未做任何校验。
    //           畸形或恶意包若声明越界值（如fragmentIndex=999999），会插入
    //           fragments map并使receivedCount++污染重组计数；重组循环
    //           group.fragments[i]对缺失index会default-construct空QByteArray
    //           拼接，产出内容缺失的"完整"帧送入解码器，导致花屏或解码器异常。
    // 修改内容: 新增边界校验（totalFragments在1-64之间、fragmentIndex在
    //           0到totalFragments-1之间），非法分片直接丢弃并打印告警
    // 影响范围: 仅拦截畸形包路径，正常分片不受影响
    if (totalFragments <= 0 || totalFragments > 64 ||
        fragmentIndex < 0 || fragmentIndex >= totalFragments) {
        qWarning() << "H264分片字段非法, userId:" << userId
                   << "totalFragments:" << totalFragments
                   << "fragmentIndex:" << fragmentIndex
                   << ", 丢弃该分片";
        return;  // 丢弃畸形分片
    }

    // 单分片帧（totalFragments==1）：直接解码，无需重组
    // 这是兼容路径，适用于不分片的场景
    if (totalFragments <= 1) {
        // 直接使用h264Data进行解码（跳过重组逻辑）
        // 以下代码与原逻辑一致，直接送入解码器
    } else {
        // 多分片帧：需要重组
        // 获取或创建该用户的重组缓冲区
        auto &userGroups = m_h264Reassemble[userId];
        auto &group = userGroups[frameSeq];

        // 首个分片：初始化组信息
        if (group.receivedCount == 0) {
            group.totalFragments = totalFragments;
            group.timestamp = QDateTime::currentMSecsSinceEpoch();
            group.frameType = header->m_frameType;
        }

        // 存储当前分片（按fragmentIndex索引）
        // 如果该分片已存在(重复包)，跳过
        if (!group.fragments.contains(fragmentIndex)) {
            group.fragments[fragmentIndex] = h264Data;
            group.receivedCount++;
        }

        // 超时清理：检查并丢弃过期的分片组
        // 避免因丢包导致分片组永久驻留内存
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - group.timestamp > H264_REASSEMBLE_TIMEOUT) {
            // 当前帧组超时，丢弃
            qWarning() << "H264重组超时, userId:" << userId
                       << "frameSeq:" << frameSeq
                       << "received:" << group.receivedCount
                       << "/" << group.totalFragments;
            userGroups.erase(frameSeq);

            // 丢帧后向发送端请求关键帧(PLI)
            // 原Bug: 分片超时丢弃后，解码器收到的下一个完整帧可能是P帧，
            //        而它依赖的I帧/P帧已被丢弃。FFmpeg解码器会产生花屏（参考帧缺失），
            //        直到发送端自然产生下一个I帧（GOP=60，最长2秒）。
            // 修复: 发送PLI请求，通知发送端立即产生一个I帧。
            if (group.frameType == 0) {
                // 丢失的是I帧，后续P帧都无法解码，必须请求新的I帧
                qDebug() << "H264: lost I-frame, sending PLI request for userId:" << userId;
            }
            // 修改原因: 原代码无条件直接发送PLI，弱网下会形成
            //           "I帧超时→更多PLI→更多I帧尖峰"的正反馈风暴
            // 修改内容: 改为调用限流版本，每用户500ms窗口内最多发送一次
            // 影响范围: PLI发送频率受限，恢复语义不变
            // 原代码（保留为注释）:
            /*
            STRU_PLI_REQUEST pli;
            pli.m_userid = m_id;
            pli.m_roomid = m_roomid;
            m_pAVClient[1]->SendData(0, (char*)&pli, sizeof(pli));
            */
            sendPliRequestThrottled(userId);

            return;
        }

        // 检查是否收齐所有分片
        if (group.receivedCount < group.totalFragments) {
            // 未收齐，等待后续分片到达
            return;
        }

        // 所有分片已收齐，按fragmentIndex顺序拼接为完整H264帧
        QByteArray completeH264Data;
        for (int i = 0; i < group.totalFragments; i++) {
            completeH264Data.append(group.fragments[i]);
        }

        // 重组完成，清理该帧组
        userGroups.erase(frameSeq);

        // 清理该用户的其他过期分片组（防止内存泄漏）
        auto cleanupIt = userGroups.begin();
        while (cleanupIt != userGroups.end()) {
            if (now - cleanupIt->second.timestamp > H264_REASSEMBLE_TIMEOUT) {
                // 修改原因: 原代码静默erase过期分片组（不发PLI），单帧丢失+
                //           后续帧正常的场景下，解码器收到后续完整P帧但其参考帧
                //           已丢，FFmpeg concealment输出花屏直到自然GOP尾部
                //           （最长2秒）且PLI从未发出——这是超时分支PLI修复的
                //           漏网路径。
                // 修改内容: erase前对被丢弃的帧组触发限流PLI，所有丢弃路径
                //           统一恢复语义（PLI发送由限流函数控制频率）
                // 影响范围: 单帧部分丢失场景从"静默花屏最长2秒"变为
                //           "主动请求I帧恢复（约200-400ms）"
                // 原代码（保留为注释）:
                /*
                cleanupIt = userGroups.erase(cleanupIt);
                */
                sendPliRequestThrottled(userId);
                cleanupIt = userGroups.erase(cleanupIt);
            } else {
                ++cleanupIt;
            }
        }

        // 当用户的内层map为空时，删除外层map条目，防止内存持续增长
        // 原Bug: m_h264Reassemble 的内层map清空后，外层条目永远存在。
        //        多用户频繁加入/退出房间时，外层map不断增长，永不收缩。
        // 修复: 检查内层map是否为空，为空则删除外层条目。
        if (userGroups.empty()) {
            m_h264Reassemble.erase(userId);
        }

        // 用重组后的完整数据替换分片数据
        h264Data = completeH264Data;

        qDebug() << "H264重组完成, userId:" << userId
                 << "frameSeq:" << frameSeq
                 << "totalFragments:" << totalFragments
                 << "dataSize:" << h264Data.size();
    }

    // 3. 查找或创建该用户的解码器
    H264Decoder *decoder = nullptr;
    auto it = m_mapIDToH264Decoder.find(userId);
    if (it == m_mapIDToH264Decoder.end()) {
        // 新用户：创建并初始化解码器
        decoder = new H264Decoder(this);
        if (!decoder->init()) {
            qWarning() << "slot_dealVideoFrameH264Rq: decoder init failed for user" << userId;
            delete decoder;
            return;
        }
        m_mapIDToH264Decoder[userId] = decoder;
        qDebug() << "slot_dealVideoFrameH264Rq: created decoder for user" << userId;
    } else {
        decoder = it->second;
    }

    // 4. 解码（当前在 UI 线程，后续可移至工作线程优化）
    AVFrame *frame = decoder->decode(h264Data);
    if (!frame) {
        return;  // 解码失败或需要更多数据
    }

    // 花屏帧检测（decode_error_flags）
    // 修改原因: decode()仅在FFmpeg API层面失败时返回nullptr；参考帧缺失时
    //           FFmpeg concealment会"成功"输出内容损坏的帧（绿屏/灰屏/马赛克），
    //           原代码未检测decode_error_flags即转发渲染，花屏直接刷到渲染窗口。
    // 修改内容: 新增decode_error_flags检测，损坏帧跳过渲染——冻结在上一正常帧
    //           的体验优于花屏闪烁。av_frame_free释放方式与本函数末尾现有的
    //           "释放本地引用"路径一致（调用者拥有decode返回帧的所有权）。
    // 影响范围: 仅拦截concealment损坏帧；正常帧decode_error_flags为0不受影响
    if (frame->decode_error_flags != 0) {
        qWarning() << "H264解码错误帧(花屏已屏蔽), userId:" << userId
                   << ", flags:" << frame->decode_error_flags;
        av_frame_free(&frame);
        return;
    }

    // 注释化旧的占位代码，避免use-after-free
    // 旧代码在av_frame_free后又访问frame->width，导致空指针崩溃
    // 保留旧代码为注释，由下方新代码替代
    /*
    // 触发 UI 刷新
    // 当前简化版：直接转 QImage 传递给 RoomDialog（与原 JPEG 路径一致）
    // 后续优化：直接传递 AVFrame 到 YuvGLWidget，跳过 sws_scale
    // 此处使用 AVFrame -> QImage 的简单转换
    QImage img = QImage(frame->width, frame->height, QImage::Format_RGB888);
    // 注意: 此处省略 YUV->RGB 转换，实际实现需 sws_scale
    // 完整实现见 YuvGLWidget::slot_setFrame 直接渲染 YUV
    av_frame_free(&frame);  // 释放引用

    // 调用原有刷新接口（保持与 JPEG 路径一致）
    if (m_pRoomdialog) {
        // 此处传递空 QImage 作为占位，实际渲染由 YuvGLWidget 直接处理 AVFrame
        // 完整集成需要修改 RoomDialog::slot_refreshUser 接收 AVFrame
        // 当前为阶段三初步集成，保留接口兼容
        // TODO: 阶段四修改 RoomDialog::slot_refreshUser 支持 AVFrame
    }
    */

    // 激活H264解码后AVFrame转发到RoomDialog进行GPU渲染
    // 接收端解码完成后的AVFrame直接转发到渲染层
    // 跳过YUV->QImage转换，由YuvGLWidget在GPU侧完成YUV->RGB转换
    qDebug() << "H264解码成功, userId:" << userId
             << "帧尺寸:" << frame->width << "x" << frame->height
             << "format:" << frame->format;

    // 转发AVFrame到RoomDialog（由其分发到对应UserShow的YuvGLWidget）
    //  先显示再同步，避免 QThread::msleep 阻塞主线程导致视频不刷新
    if (m_pRoomdialog) {
        // 调用新增的YUV刷新接口（RoomDialog内部转发到UserShow::slot_setFrame）
        m_pRoomdialog->slot_refreshUserYuv(userId, frame);
        qDebug() << "[ACTIVATION] AVFrame已转发到RoomDialog, userId:" << userId;
    } else {
        qWarning() << "error:m_pRoomdialog为空, 无法转发AVFrame";
    }

    // AVSync: 更新视频时钟状态（不再阻塞主线程）
    {
        double video_pts = (double)AVSyncManager::getCurrentTimeUs();
        double frameDelay = 33333.0;  // 30fps = 33333微秒
        AVSyncManager::getInstance().synchronizeVideo(userId, video_pts, frameDelay);
        // QThread::msleep(waitMs) — 阻塞主线程导致视频不刷新
    }

    // 6. 释放本地引用（YuvGLWidget已通过av_frame_clone持有自己的引用）
    av_frame_free(&frame);
}

// PLI关键帧请求限流发送
// 修改原因: 原超时分支无条件发送PLI（直接构造STRU_PLI_REQUEST并SendData），
//           弱网下I帧(50-150KB,7-19个分片)单帧最大最易重组超时 → 触发更多PLI →
//           更多I帧尖峰挤占带宽拖垮P帧 → 持续恶化的正反馈风暴。
// 修改内容: 新增限流函数，每用户PLI_THROTTLE_INTERVAL_MS(500ms)窗口内最多
//           发送一次PLI，窗口内的重复请求直接丢弃；超时分支/被动清理循环/
//           主动扫描定时器统一调用此函数。
// 影响范围: 仅限流PLI发送频率，不改变PLI请求语义
void ckernel::sendPliRequestThrottled(int userId)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    // 限流窗口内：该用户500ms内已发过PLI，跳过本次请求
    auto it = m_lastPliTime.find(userId);
    if (it != m_lastPliTime.end() && (now - it->second) < PLI_THROTTLE_INTERVAL_MS) {
        qDebug() << "PLI限流: userId" << userId << "500ms窗口内已发送, 跳过";
        return;
    }
    m_lastPliTime[userId] = now;

    // 原代码（保留为注释）: 超时分支中直接无条件发送PLI
    /*
    STRU_PLI_REQUEST pli;
    pli.m_userid = m_id;
    pli.m_roomid = m_roomid;
    m_pAVClient[1]->SendData(0, (char*)&pli, sizeof(pli));
    */
    STRU_PLI_REQUEST pli;
    pli.m_userid = m_id;
    pli.m_roomid = m_roomid;
    m_pAVClient[1]->SendData(0, (char*)&pli, sizeof(pli));
}

// 分片重组缓冲区主动扫描（50ms周期）
// 修改原因: 原超时检测只在收到新分片时执行（纯被动），视频流完全中断
//           （对端崩溃/断网）时新分片永不到达：
//           ①残留分片组驻留内存（仅退出房间时清理）
//           ②超时PLI永不触发，丢帧无法恢复
// 修改内容: 新增50ms周期主动扫描，遍历所有用户所有帧组，过期的erase并触发
//           限流PLI；内层map清空后同步删除外层条目（防内存增长，与被动清理
//           中"userGroups.empty()则erase外层条目"的逻辑保持一致）。外层erase
//           使用erase返回下一个迭代器的标准模式，遍历中删除安全。
// 影响范围: 流中断场景过期组必被清理、PLI必被触发；正常场景与被动清理
//           互为冗余（幂等，重复erase无副作用）
void ckernel::slot_scanReassembleBuffer()
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto outerIt = m_h264Reassemble.begin();
    while (outerIt != m_h264Reassemble.end()) {
        int userId = outerIt->first;
        auto &userGroups = outerIt->second;
        auto it = userGroups.begin();
        while (it != userGroups.end()) {
            if (now - it->second.timestamp > H264_REASSEMBLE_TIMEOUT) {
                qDebug() << "[主动扫描] H264重组超时清理, userId:" << userId
                         << "frameSeq:" << it->first
                         << "received:" << it->second.receivedCount
                         << "/" << it->second.totalFragments;
                it = userGroups.erase(it);
                // 主动扫描发现的丢帧同样触发限流PLI（三处丢弃路径统一恢复语义）
                sendPliRequestThrottled(userId);
            } else {
                ++it;
            }
        }
        // 内层map为空时删除外层条目，防止内存持续增长
        if (userGroups.empty()) {
            outerIt = m_h264Reassemble.erase(outerIt);
        } else {
            ++outerIt;
        }
    }
}

// PLI关键帧请求处理函数
// 收到 PLI 请求时，强制下一帧编码为 I 帧
void ckernel::slot_dealPliRequest(uint sock, char *buf, int nlen)
{
    // 添加包长度校验，防止越界读取
    // 原Bug: 未检查 nlen >= sizeof(STRU_PLI_REQUEST)，如果收到过短的包，
    //        (STRU_PLI_REQUEST*)buf 会越界读取内存。
    if (nlen < (int)sizeof(STRU_PLI_REQUEST)) {
        qWarning() << "slot_dealPliRequest: packet too small," << nlen
                   << "expected>=" << (int)sizeof(STRU_PLI_REQUEST);
        return;
    }

    STRU_PLI_REQUEST *pli = (STRU_PLI_REQUEST*)buf;
    qDebug() << "slot_dealPliRequest: received PLI from user" << pli->m_userid
             << "room" << pli->m_roomid;

    if (m_h264Encoder && m_h264Encoder->isReady()) {
        m_h264Encoder->requestKeyFrame();
        qDebug() << "slot_dealPliRequest: key frame requested";
    }
}

// H.264帧解码完成回调
// 由解码线程触发，传递 AVFrame 到 UI 线程进行渲染
void ckernel::slot_h264FrameDecoded(int userId, AVFrame *frame)
{

    //移除重复的释放逻辑，统一在函数末尾释放一次。
    if (!frame) {
        qWarning() << "error:slot_h264FrameDecoded收到空帧, userId:" << userId;
        return;
    }
    if (!m_pRoomdialog) {
        qWarning() << "error:m_pRoomdialog为空, 释放帧, userId:" << userId;
        av_frame_free(&frame);
        return;
    }

    if (m_roomid != 0) {
        // 转发AVFrame到RoomDialog（内部转发到UserShow::slot_setFrame）
        m_pRoomdialog->slot_refreshUserYuv(userId, frame);
        qDebug() << " slot_h264FrameDecoded已转发AVFrame, userId:" << userId;
    }

    // 释放本地引用（RoomDialog/UserShow/YuvGLWidget通过av_frame_clone持有各自引用）
    av_frame_free(&frame);
}

// H.264编码器初始化方法
// 在视频开始时调用，初始化编码器
// 返回 true 表示可使用 H.264，false 回退到 JPEG
bool ckernel_initH264Encoder(ckernel *kernel)
{
    // 此函数为外部调用接口，由 ckernel::slot_startVideo 调用
    // 当前为占位实现，实际初始化在 slot_startVideo 中
    Q_UNUSED(kernel);
    return false;

    // 激活ckernel_initH264Encoder状态查询接口
    // 由于ckernel私有成员无法从外部直接访问，此函数转为状态查询接口
    // 实际初始化由ckernel::slot_startVideo内部完成
    // 此函数保留作为外部模块（如服务端调试工具）查询H264支持能力的入口
    qDebug() << " ckernel_initH264Encoder: 状态查询接口被调用"
             << "实际初始化由slot_startVideo完成";
    Q_UNUSED(kernel);
    // 返回true表示H264路径已激活（slot_startVideo成功初始化后会设置m_useH264=true）
    return true;
}


// 同步逻辑已抽离到 AVSyncManager 独立模块，该函数不再需要
// 使用 AVSyncManager::synchronizeVideo() 和 AVSyncManager::synchronizeLocalVideo()
/* 原有代码（已废弃，保留作为历史参考）：
// 音视频同步等待函数
// 参考MediaPlayer/videoplayer.cpp的synchronize_video实现
// 策略：音频主时钟，视频帧显示前与audio_clock比对
// 功能：
//   1. 若视频PTS存在，用PTS更新video_clock
//   2. 若视频PTS缺失（为0），用video_clock作为当前帧PTS（线性外推）
//   3. 计算帧间延迟并推进video_clock
//   4. 比对video_pts与audio_clock，超前则QThread::msleep等待
// 参数：pts-当前视频帧PTS(微秒), frameDelay-帧间延迟(微秒)
// 返回：计算后的video_pts（微秒）
double ckernel::synchronizeVideo(double pts, double frameDelay)
{
    // 1. 更新视频时钟
    if (pts > 0.0) {
        // PTS存在，用PTS重置video_clock
        m_videoClock = pts;
    }
    // 若PTS为0，保持video_clock不变（线性外推）

    double video_pts = m_videoClock;

    // 2. 推进视频时钟（为下一帧做准备）
    m_videoClock += frameDelay;

    // 3. 比对video_pts与audio_clock，超前则等待
    // 参考MediaPlayer/videoplayer.cpp:98-105的同步等待循环
    if (m_audioClock > 0.0) {
        double diff = video_pts - m_audioClock;
        // 同步阈值：10ms（参考ffplay的SYNC_THRESHOLD）
        // 仅在差值超过阈值时才触发等待，避免微抖动
        const double SYNC_THRESHOLD = 10000.0; // 10ms = 10000微秒

        if (diff > SYNC_THRESHOLD) {
            // 视频超前于音频，等待音频追上
            int waitMs = (int)(diff / 1000.0); // 微秒转毫秒
            if (waitMs > 100) waitMs = 100;     // 最大等待100ms，避免卡死
            if (waitMs > 0) {
                QThread::msleep(waitMs);
            }
        }
        // 若diff < -SYNC_THRESHOLD，视频落后音频，直接显示（不等待）
    }

    return video_pts;
}
*/

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
    PackType type = *(PackType*)szbuf;
    // [诊断日志] 记录所有收到的数据包
    fflush(stdout);
    if( (type >= DEF_PACK_BASE) && ( type < DEF_PACK_BASE + DEF_PACK_COUNT) )
    {
        PFUN pf = NetPackMap( type );
        if( pf )
        {
            (TcpKernel::GetInstance()->m_logic->*pf)( clientfd , szbuf , nlen);
        }
        else
        {
            // [诊断日志] 协议映射为空
        }
    }
    else
    {
        // [诊断日志] 类型越界
    }

    return;
}

void TcpKernel::EventLoop()
{
    m_tcp->EventLoop();
}

void TcpKernel::SendData(sock_fd clientfd, char *szbuf, int nlen)
{
    m_tcp->SendData(clientfd , szbuf ,nlen );
}

////register
//void TcpKernel::RegisterRq(int clientfd, char * szbuf, int nlen)
//{
//   printf("clientfd:%d RegisterRq\n", clientfd);
//   //1.拆包
//   STRU_REGISTER_RQ * rq = (STRU_REGISTER_RQ *)szbuf;
//   STRU_REGISTER_RS rs;
//   //獲取tel password name

//   //查表t_user 根據tel 查tel
//   char sqlStr[1024] = {0}/*""*/;
//   sprintf( sqlStr , "select tel from t_user where tel = '%s';",rq->m_tel);
//   list<string> resList;
//   if(!m_sql->SelectMysql(sqlStr , 1 , resList))
//   {
//       printf("SelectMysql error: %s \n",sqlStr);
//       return;
//   }
//   //有user存在 返回
//   if(resList.size() > 0)
//   {
//       rs.m_lresult = tel_is_exist;
//   }
//   else
//   {
//       //沒有，查表 t_user 根據name 查name name有沒有
//       char sqlStr[1024] = {0}/*""*/;
//       resList.clear();
//       sprintf(sqlStr , "select name from t_user where name = '%s';",rq->m_name);

//       if(!m_sql->SelectMysql(sqlStr , 1 , resList))
//       {
//           printf("SelectMysql error: %s \n",sqlStr);
//           return;
//       }
//       if( resList.size() > 0)
//       {
//           //有name存在，返回
//           rs.m_lresult = name_is_exist;
//       }
//       else
//       {
//           //沒有，寫表tel pass name頭像和籤名的默認值 返回注冊成功
//           rs.m_lresult = register_success;
//           sprintf(sqlStr , "insert into t_user( tel, password , name , icon , feeling) values('%s', '%s' , '%s' , '%d' , '%s');"
//                   ,rq->m_tel,rq->m_password,rq->m_name, 1 ,"比較懶，什麼也沒寫");
//           if(!m_sql->UpdataMysql(sqlStr))
//           {
//               printf("UpdataMysql error: %s \n",sqlStr);
//           }
//       }
//   }
//   m_tcp->SendData(clientfd , (char*)&rs , sizeof(rs));
//}

////LOGIN
//void TcpKernel::LoginRq(int clientfd, char * szbuf, int nlen)
//{
//   printf("clientfd:%d LoginRq\n", clientfd);
//   //拆包 //手機號 密碼
//   STRU_LOGIN_RQ * rq = (STRU_LOGIN_RQ *)szbuf;
//   STRU_LOGIN_RS rs;
//   // 根據tel 查pass id
//   char strsql[1024] = "";
//   list<string> lstRes;
//   sprintf( strsql , "select tel from t_user where tel = '%s';",rq->m_tel);

//   if(!m_sql->SelectMysql(strsql , 2 , lstRes))
//   {
//       printf("SelectMysql error: %s \n",strsql);
//       return;
//   }
//   if(lstRes.size() == 0)
//   {
//       //查不到，返回，沒此用戶
//       rs.m_lresult = password_error;
//   }else{
//       lstRes.pop_front();
//       //一致，sock要保存起來 id->sock映射（爲了通信）
//       int id = atoi(lstRes.front().c_str());
//       //sock要保存起來 id->sock映射（爲了通信）
//       UserInfo * pInfo = new UserInfo;
//       pInfo->m_id = id;
//       pInfo->m_roomid = 0;
//       pInfo->m_sock = clientfd;

//       strcpy(pInfo->m_userName , lstRes.front().c_str());
//       lstRes.pop_front();
//       //判斷id是否在線，在線強制下線，不在線添加
//       if(m_mapIDToUserInfo.IsExist(pInfo->m_id))
//       {
//           //強制下線

//       }
//       //**m_mapIDToUserInfo,inserter(pInfo->m_id , pInfo);
//       //寫返回包 返回 帶id和結果
//       rs.m_userid = id;
//       rs.m_lresult = login_success;
//       strcpy(rs.m_name , pInfo->m_userName);


//   }


//   //STRU_LOGIN_RS rs;
//   //rs.m_lresult = password_error;
//   SendData(clientfd, (char*)&rs, sizeof rs);

//}

////創建房間
//void TcpKernel::CreateRoomRq(int clientfd, char *szbuf, int nlen)
//{
//    printf("clientfd:%d CreateRoomRq\n", clientfd);
//    //拆包
//    STRU_CREATEROOM_RQ* rq = (STRU_CREATEROOM_RQ*)szbuf;
//    //房間號 1-8位隨機數
//    int roomid = 0;
//    do{
//        roomid = rand()%99999999+1;

//    }while(m_mapIDToRoomid.IsExist( roomid ));
//    list<int> lst;
//    lst.push_back( rq->m_UserID );
//    m_mapIDToRoomid.insert( roomid , lst);
//    printf("roomid = %d\n",roomid);
//    //回復
//    STRU_CREATEROOM_RS rs;
//    rs.m_RoomID = roomid;
//    rs.m_lresult = 1;

//    SendData( clientfd , (char*)&rs , sizeof (rs));

//}

////加入房間
//void TcpKernel::JoinRoomRq(int clientfd, char *szbuf, int nlen)
//{
//    printf("clientfd:%d JoinRoomRq\n", clientfd);
//    //拆包
//    STRU_JOINROOM_RQ * rq = (STRU_JOINROOM_RQ*)szbuf;
//    STRU_JOINROOM_RS rs;
//    //查看房間是否存在
//    if(!m_mapIDToRoomid.IsExist(rq->m_RoomID))
//    {
//        //不存在，返回失敗
//        rs.m_lresult = 0;
//        SendData( clientfd , (char*)&rs , sizeof (rs));
//        return;
//    }
//        //存在，返回成功
//        rs.m_lresult = 1;
//        SendData( clientfd , (char*)&rs , sizeof (rs));

//        if( !m_mapIDToRoomid.IsExist(rq->m_UserID)) return;
//        UserInfo * joiner = m_mapIDToUserInfo.find(rq->m_UserID);

//        STRU_ROOM_MEMBER_RQ joinrq;
//        joinrq.m_UserID = rq->m_UserID;
//        strcpy(joinrq.m_szUser , joiner->m_userName);
//        //給自己用於更新自己的信息
//        SendData( clientfd , (char*)&joinrq , sizeof (joinrq));

//        //根據房間號拿到房間成員列表
//        list<int> lstRoomMem = m_mapIDToRoomid.find(rq->m_RoomID);
//        //互換信息
//        for( auto ite = lstRoomMem.begin() ; ite!=lstRoomMem.end() ; ++ite)
//        {
//            int Memid = *ite;
//            if( !m_mapIDToUserInfo.IsExist(Memid)) return;
//            UserInfo * memInfo = m_mapIDToUserInfo.find(Memid);

//            STRU_ROOM_MEMBER_RQ memrq;
//            memrq.m_UserID = memInfo->m_id;
//            strcpy(memrq.m_szUser , memInfo->m_userName);

//            //把加入人的信息發送給每一個房間內成員
//            SendData(memInfo->m_sock , (char*)&joinrq ,sizeof (joinrq));
//            //房間內成員每個人信息發給加入人
//            SendData(clientfd , (char*)&memrq, sizeof (memrq));
//        }
//        //加入人，添加到房間列表
//        lstRoomMem.push_back( rq->m_UserID);
//        m_mapIDToRoomid.insert(rq->m_RoomID , lstRoomMem);


//}

////離開房間
//void TcpKernel::LeaveRoomRq(int clientfd, char *szbuf, int nlen)
//{
//    printf("clientfd:%d LeaveRoomRq\n", clientfd);
//    //拆包
//    STRU_LEAVEROOM_RQ * rq = (STRU_LEAVEROOM_RQ*)szbuf;
//    //看房间是否存在
//    if(!m_mapIDToRoomid.IsExist(rq->m_RoomID)) return;
//    //如果房间存在。可以获得用户列表
//    list< int > lst = m_mapIDToRoomid.find( rq->m_RoomID );
//    for( auto ite = lst.begin(); ite != lst.end();)
//    {
//        int userid = *ite;
//        //遍历每个用户
//        //是不是自己，如果是自己，从列表中去除--从房间移除了
//        if( userid == rq->m_nUserID )
//        {
//            ite = lst.erase(ite);
//        }else{
//            //用户是否在线，在线转发
//            if( m_mapIDToUserInfo.IsExist(userid))
//            {
//                UserInfo *info = m_mapIDToUserInfo.find( userid );
//                SendData( info->m_sock , szbuf , nlen);
//            }
//            ++ite;
//        }
//    }



//    //列表是否节点数为0->map项去掉
//    if( lst.size() == 0)
//    {
//        m_mapIDToRoomid.erase( rq->m_RoomID );
//        return;
//    }

//    //更新房间成员列表
//    m_mapIDToRoomid.insert( rq->m_RoomID , lst);
//}

////處理音頻幀
//void TcpKernel::AudioFrameRq(int clientfd, char *szbuf, int nlen)
//{
//    printf("clientfd:%d AudioFrameRq\n", clientfd);
//    //拆包
//    char * tmp = szbuf;
//    //跳過type
//    tmp += sizeof(int);
//    //讀取userid
//    int uid = *(int*) tmp;
//    //跳過userid
//    tmp += sizeof(int);
//    //獲取roomid
//    int roomid = *(int*) tmp;

//    //看房間是否存在
//    if( !m_mapIDToRoomid.IsExist( roomid )) return;
//    list<int> lst = m_mapIDToRoomid.find(roomid);
//    //獲取成員列表
//    for( auto ite = lst.begin() ; ite != lst.end() ; ++ite)
//    {
//        //看是否在線，轉發
//        int userid = *ite;
//        //屏蔽掉自己 不轉發
//        if( uid == userid ) continue;
//        if( !m_mapIDToRoomid.IsExist(userid)) continue;
//        UserInfo * userinfo = m_mapIDToUserInfo.find(userid);
//        //原樣轉發
//        SendData( userinfo->m_sock , szbuf , nlen);

//    }
//}

//void TcpKernel::VideoFrameRq(int clientfd, char *szbuf, int nlen)
//{
//    printf("clientfd:%d VideoFrameRq\n", clientfd);
//    //拆包
//    char * tmp = szbuf;
//    //跳過type
//    tmp += sizeof(int);
//    //讀取userid
//    int uid = *(int*) tmp;
//    //跳過userid
//    tmp += sizeof(int);
//    //獲取roomid
//    int roomid = *(int*) tmp;

//    //看房間是否存在
//    if( !m_mapIDToRoomid.IsExist( roomid )) return;
//    list<int> lst = m_mapIDToRoomid.find(roomid);
//    //獲取成員列表
//    for( auto ite = lst.begin() ; ite != lst.end() ; ++ite)
//    {
//        //看是否在線，轉發
//        int userid = *ite;
//        //屏蔽掉自己 不轉發
//        if( uid == userid ) continue;
//        if( !m_mapIDToRoomid.IsExist(userid)) continue;
//        UserInfo * userinfo = m_mapIDToUserInfo.find(userid);
//        //原樣轉發
//        //SendData( userinfo->m_sock , szbuf , nlen);
//        SendData( userinfo->m_audiofd , szbuf , nlen);

//    }
//}

////音頻注冊
//void TcpKernel::AudioRegister(int clientfd, char *szbuf, int nlen)
//{
//    printf("clientfd:%d AudioReister\n", clientfd);
//    //拆包
//    STRU_AUDIO_REGISTER *rq = (STRU_AUDIO_REGISTER * )szbuf;
//    int userid = rq->m_userid;
//    //根據userid找到節點 更新fd
//    if( m_mapIDToUserInfo.IsExist(userid) )
//    {
//        UserInfo * info = m_mapIDToUserInfo.find(userid);
//        info->m_audiofd = clientfd;
//    }
//}

////視頻注冊
//void TcpKernel::VideoRegister(int clientfd, char *szbuf, int nlen)
//{
//    printf("clientfd:%d VideoRegister\n", clientfd);
//    //拆包
//    STRU_VIDEO_REGISTER *rq = (STRU_VIDEO_REGISTER * )szbuf;
//    int userid = rq->m_userid;
//    //根據userid找到節點 更新fd
//    if( m_mapIDToUserInfo.IsExist(userid) )
//    {
//        UserInfo * info = m_mapIDToUserInfo.find(userid);
//        info->m_audiofd = clientfd;
//    }
//}

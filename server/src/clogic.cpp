#include "clogic.h"
#include "bcrypt_wrapper.h"  // [P0新增] bcrypt 密码哈希封装

void CLogic::setNetPackMap()
{
    NetPackMap(DEF_PACK_REGISTER_RQ)         =&CLogic::RegisterRq;
    NetPackMap(DEF_PACK_LOGIN_RQ)            =&CLogic::LoginRq;
    NetPackMap(DEF_PACK_CREATEROOM_RQ)       =&CLogic::CreateRoomRq;
    NetPackMap(DEF_PACK_JOINROOM_RQ)         =&CLogic::JoinRoomRq;
    NetPackMap(DEF_PACK_LEAVEROOM_RQ)        =&CLogic::LeaveRoomRq;
    NetPackMap(DEF_PACK_AUDIO_FRAME)         =&CLogic::AudioFrameRq;
    NetPackMap(DEF_PACK_VIDEO_FRAME)         =&CLogic::VideoFrameRq;
    NetPackMap(DEF_PACK_AUDIO_REGISTER)      =&CLogic::AudioRegister;
    NetPackMap(DEF_PACK_VIDEO_REGISTER)      =&CLogic::VideoRegister;
    //【拓展优化-新增】注册补全邮箱协议映射
    NetPackMap(DEF_PACK_UPDATE_EMAIL_RQ)     =&CLogic::UpdateEmailRq;
    // [新增] H.264视频帧处理映射
    // 功能：转发H.264编码的视频帧给房间内其他成员
    NetPackMap(DEF_PACK_VIDEO_FRAME_H264)  =&CLogic::VideoFrameH264Rq;
    // [新增] 关键帧请求处理映射
    // 功能：转发PLI请求给房间内其他成员（视频发送方）
    NetPackMap(DEF_PACK_PLI_REQUEST)       =&CLogic::PliRequestRq;

}

//register
void CLogic::RegisterRq(int clientfd, char * szbuf, int nlen)
{
   //1.拆包
   STRU_REGISTER_RQ * rq = (STRU_REGISTER_RQ *)szbuf;
   STRU_REGISTER_RS rs;
   //獲取tel password name

   //【安全修复-SQL注入】tel / name / email 三个字段全部来自客户端包体，
   //  在本函数里被拼进 4 条 SQL（3 条 SELECT + 1 条 INSERT），一次性转义后全程复用。
   //  上一批只改了 INSERT、漏掉前置三条 SELECT，教训就在这里：
   //    1) 只要还有一条没转义，注入照样打得进去；
   //    2) 前置 SELECT 先执行，SQL 被改写得语法错误后函数直接 return，
   //       客户端永远收不到回包（表现为"卡死"），比报错更隐蔽。
   //  nMaxLen 传 sizeof(字段)：包体是定长 char[]，恶意客户端可以填满整片数组
   //  却不写 '\0'，交给 strlen 会越过字段边界读到下一字段去。
   std::string strSafeTel   = m_sql->EscapeMysql( rq->m_tel   , sizeof(rq->m_tel)   );
   std::string strSafeName  = m_sql->EscapeMysql( rq->m_name  , sizeof(rq->m_name)  );
   std::string strSafeEmail = m_sql->EscapeMysql( rq->m_email , sizeof(rq->m_email) );

   //查表t_user 根據tel 查tel
   char sqlStr[1024] = {0}/*""*/;
   sprintf( sqlStr , "select tel from t_user where tel = '%s';", strSafeTel.c_str());
   list<string> resList;
   if(!m_sql->SelectMysql(sqlStr , 1 , resList))
   {
       return;
   }
   //有user存在 返回
   if(resList.size() > 0)
   {
       rs.m_lresult = tel_is_exist;
   }
   else
   {
       //沒有，查表 t_user 根據name 查name name有沒有
       char sqlStr[1024] = {0}/*""*/;
       resList.clear();
       sprintf(sqlStr , "select name from t_user where name = '%s';", strSafeName.c_str());

       if(!m_sql->SelectMysql(sqlStr , 1 , resList))
       {
           return;
       }
       if( resList.size() > 0)
       {
           //有name存在，返回
           rs.m_lresult = name_is_exist;
       }
       else
       {
           //【拓展优化-新增】检查邮箱是否已存在
           //原代码未检查邮箱唯一性，可能导致同一邮箱注册多个账号
           //修复：注册前检查邮箱是否已存在于数据库
           resList.clear();
           sprintf(sqlStr , "select email from t_user where email = '%s';", strSafeEmail.c_str());
           if(!m_sql->SelectMysql(sqlStr , 1 , resList))
           {
               return;
           }
           if( resList.size() > 0)
           {
               //邮箱已存在，返回错误（复用 name_is_exist 错误码表示"已存在"）
               rs.m_lresult = name_is_exist;
           }
           else
           {
               //沒有，寫表tel pass name email 頭像和籤名的默認值 返回注冊成功
               rs.m_lresult = register_success;
               // [P0改进] 密码存储从「明文 MD5 哈希」升级为「bcrypt(MD5+随机盐)」
               // 原代码(SD):
               //   sprintf(sqlStr , "insert into t_user( tel, password , name , email , icon , feeling) values('%s', '%s' , '%s' , '%s' , '%d' , '%s');"
               //           ,rq->m_tel,rq->m_password,rq->m_name, rq->m_email, 1 ,"比較懶，什麼也沒寫");
               // 新代码(P0): 对客户端发来的 MD5 哈希值再做 bcrypt 后入库
               std::string strPwdBcrypt = bcrypt_hash(rq->m_password);
               if (strPwdBcrypt.empty()) {
                   // 原来这里填的是 0（= tel_is_exist），会让客户端提示"手机号已存在"，
                   // 可问题根本不在手机号。填 register_failed 才是实话。
                   fprintf(stderr, "[RegisterRq] bcrypt 计算失败，注册中止\n");
                   rs.m_lresult = register_failed;
                   m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
                   return;
               }
               //【拓展优化-修改】INSERT 语句新增 email 字段
               //原代码只插入 tel, password, name, icon, feeling
               //修复：新增 email 字段插入
               //  password 是服务端自己算的 bcrypt 串（字符集 ./0-9A-Za-z$），天然安全，
               //  feeling 是常量，两者无需转义；tel / name / email 复用函数开头已转义的值。
               sprintf(sqlStr , "insert into t_user( tel, password , name , email , icon , feeling) values('%s', '%s' , '%s' , '%s' , '%d' , '%s');"
                       ,strSafeTel.c_str(),strPwdBcrypt.c_str(),strSafeName.c_str(), strSafeEmail.c_str(), 1 ,"比較懶，什麼也沒寫");
               //【修复-注册静默失败】原来这里是空实现 {}：
               //  写库失败照样把上面预置的 register_success 发回客户端，
              //  用户看到"注册成功"，账号却根本不存在（随后登录必失败），
              //  服务端不留任何痕迹 —— 排查时最难定位的一类问题。
              //  现在如实返回失败，并把失败原因打到 stderr 供运维取证。
              if(!m_sql->UpdataMysql(sqlStr))
              {
                  fprintf(stderr, "[RegisterRq] 写库失败，注册未完成. sql=%s\n", sqlStr);
                  rs.m_lresult = register_failed;
              }
           }
       }
   }
   m_tcp->SendData(clientfd , (char*)&rs , sizeof(rs));
}

//LOGIN
void CLogic::LoginRq(int clientfd, char * szbuf, int nlen)
{
   //拆包 //手機號 密碼
   STRU_LOGIN_RQ * rq = (STRU_LOGIN_RQ *)szbuf;
   STRU_LOGIN_RS rs;
   //【Bug修复】原代码SQL只查询了tel一列，但SelectMysql传入2列，
   //且后续逻辑试图获取password、id、name，导致数据错乱、登录逻辑异常
   //修复：查询 password, id, name 三列，正确验证密码并获取用户信息
   char strsql[1024] = "";
   list<string> lstRes;
   //【拓展优化-修改】支持手机号/用户名/邮箱三种方式登录
   //原代码只能通过手机号登录：where tel = '...'
   //修复：根据输入特征自动识别登录类型
   //  - 包含 '@' → 邮箱登录，查询 email 字段
   //  - 全数字且长度11位 → 手机号登录，查询 tel 字段
   //  - 其他 → 用户名（昵称）登录，查询 name 字段
   string strLoginId = rq->m_tel;
   string strField = "tel"; //默认按手机号查询
   if( strLoginId.find('@') != string::npos )
   {
       //包含@符号，识别为邮箱
       strField = "email";
   }
   else if( strLoginId.find_first_not_of("0123456789") == string::npos && strLoginId.length() == 11 )
   {
       //全数字且11位，识别为手机号
       strField = "tel";
   }
   else
   {
       //其他情况，识别为用户名（昵称）
       strField = "name";
   }
   //【拓展优化-修改】SQL 查询新增 email 字段
   //原代码：select password,id,name from t_user where ...
   //修改后：select password,id,name,email from t_user where ...
   //用于登录成功后判断邮箱是否为空，若为空返回 login_success_no_email
   //【安全修复-SQL注入】这条语句原本是 `where %s = '%s'`，
   //  把【字段名】当成普通字符串拼进 SQL —— 而 MySQL 对标识符没有任何转义办法，
   //  参数化也管不了标识符。今天 strField 恰好只来自上面三个分支常量，
   //  看着"打不进去"，但只要有人后来给它加一条来自包体的赋值，立刻变成无条件注入。
   //  另外第二个 '%s' 塞的是裸rq->m_tel，一个单引号就能改写整条语句。
   //  改法双管齐下：
   //    1) 三条分支各自写死整段 SQL，标识符永远由服务端常量决定，不接受任何输入；
   //    2) 登录标识走 EscapeMysql 转义。
   std::string strSafeLoginId = m_sql->EscapeMysql( rq->m_tel , sizeof(rq->m_tel) );
   if( strField == "email" )
   {
       sprintf( strsql , "select password,id,name,email from t_user where email = '%s';", strSafeLoginId.c_str() );
   }
   else if( strField == "tel" )
   {
       sprintf( strsql , "select password,id,name,email from t_user where tel = '%s';", strSafeLoginId.c_str() );
   }
   else
   {
       //默认按用户名（昵称）查询
       sprintf( strsql , "select password,id,name,email from t_user where name = '%s';", strSafeLoginId.c_str() );
   }

   if(!m_sql->SelectMysql(strsql , 4 , lstRes))
   {
       return;
   }
   if(lstRes.size() == 0)
   {
       //查不到，返回，沒此用戶
       rs.m_lresult = password_error;
   }else{
       //【Bug修复】原代码pop_front逻辑混乱，password/id/name取值错位
       //修复：按顺序取出 password, id, name, email
       string strPwd = lstRes.front(); lstRes.pop_front();
       string strId  = lstRes.front(); lstRes.pop_front();
       string strName = lstRes.front(); lstRes.pop_front();
       //【拓展优化-新增】取出 email 字段
       string strEmail = lstRes.front(); lstRes.pop_front();

       // [P0改进] 密码验证从 strcmp 明文比较升级为 bcrypt_verify，含旧密码自动迁移
       // 原代码(SD):
       //   if( strcmp(rq->m_password, strPwd.c_str()) != 0 )
       //   {
       //       rs.m_lresult = password_error;
       //   }else{
       bool bPwdCorrect = false;
       bool bIsBcrypt = (strPwd.size() >= 3 && strPwd[0] == '$' && strPwd[1] == '2');
       if (bIsBcrypt) {
           bPwdCorrect = bcrypt_verify(rq->m_password, strPwd);
       } else {
           // 旧 MD5 格式：直接比较 + 自动迁移
           bPwdCorrect = (strcmp(rq->m_password, strPwd.c_str()) == 0);
           if (bPwdCorrect) {
               std::string strNewPwd = bcrypt_hash(rq->m_password);
               if (!strNewPwd.empty()) {
                   //【安全修复-SQL注入】id 原本用 %s 拼，等于把值当裸字符串塞进语句。
                   //  它虽然来自上一次查询的结果，但"来自数据库"不等于"下一次拼接安全"——
                   //  中间任何一环被改过，这里就是无条件注入点。id 本就是整型，用 %d 一劳永逸。
                   //  password 用的是 bcrypt 串（字符集 ./0-9A-Za-z$，不含引号），
                   //  **故意不转义**：万一转义函数因连接异常返回空串，反而会把口令清空、锁死用户。
                   int nUserId = atoi( strId.c_str() );
                   if( nUserId > 0 )
                   {
                       char strUpdate[512] = {0};
                       sprintf(strUpdate, "update t_user set password='%s' where id=%d;",
                               strNewPwd.c_str(), nUserId);
                       m_sql->UpdataMysql(strUpdate);
                   }
               }
           }
       }

       if (!bPwdCorrect) {
           rs.m_lresult = password_error;
       } else {
           //密码正确
           int id = atoi(strId.c_str());
           //sock要保存起來 id->sock映射（爲了通信）
           UserInfo * pInfo = new UserInfo;
           pInfo->m_id = id;
           pInfo->m_roomid = 0;
           pInfo->m_sock = clientfd;
           strcpy(pInfo->m_userName , strName.c_str());

           //判斷id是否在線，在線強制下線，不在線添加
           if(m_mapIDToUserInfo.IsExist(pInfo->m_id))
           {
               //強制下線（原代码未实现，暂保留）
               m_mapIDToUserInfo.erase(pInfo->m_id);
           }
           m_mapIDToUserInfo.insert(pInfo->m_id , pInfo);
           //寫返回包 返回 帶id和結果
           rs.m_userid = id;
           strcpy(rs.m_name , pInfo->m_userName);
           //【拓展优化-新增】判断邮箱是否为空
           //若邮箱为空，返回 login_success_no_email，客户端收到后弹出补全邮箱对话框
           //若邮箱不为空，返回 login_success
           if( strEmail.empty() )
           {
               rs.m_lresult = login_success_no_email;
           }
           else
           {
               rs.m_lresult = login_success;
           }
       }
   }


   //STRU_LOGIN_RS rs;
   //rs.m_lresult = password_error;
   SendData(clientfd, (char*)&rs, sizeof rs);

}

//創建房間
void CLogic::CreateRoomRq(int clientfd, char *szbuf, int nlen)
{
    //拆包
    STRU_CREATEROOM_RQ* rq = (STRU_CREATEROOM_RQ*)szbuf;
    //房間號 1-8位隨機數
    int roomid = 0;
    do{
        roomid = rand()%99999999+1;

    }while(m_mapIDToRoomid.IsExist( roomid ));
    list<int> lst;
    lst.push_back( rq->m_UserID );
    m_mapIDToRoomid.insert( roomid , lst);
    //回復
    STRU_CREATEROOM_RS rs;
    rs.m_RoomID = roomid;
    rs.m_lresult = 1;

    SendData( clientfd , (char*)&rs , sizeof (rs));

}

//加入房間
void CLogic::JoinRoomRq(int clientfd, char *szbuf, int nlen)
{
    //拆包
    STRU_JOINROOM_RQ * rq = (STRU_JOINROOM_RQ*)szbuf;
    STRU_JOINROOM_RS rs;
    //查看房間是否存在
    if(!m_mapIDToRoomid.IsExist(rq->m_RoomID))
    {
        //不存在，返回失敗
        rs.m_lresult = 0;
        SendData( clientfd , (char*)&rs , sizeof (rs));
        return;
    }
        //存在，返回成功
        rs.m_lresult = 1;
        //【Bug修复】原代码没有设置 rs.m_RoomID，而 STRU_JOINROOM_RS 构造函数中 m_RoomID = 0
        //导致发送给客户端的房间号始终为0，客户端界面 lb_title 显示"房间号:0"
        //修复：将请求中的房间号填入回复包
        rs.m_RoomID = rq->m_RoomID;
        SendData( clientfd , (char*)&rs , sizeof (rs));

        //【Bug修复】原代码 if( !m_mapIDToRoomid.IsExist(rq->m_UserID)) return;
        //检查的是"房间ID的map中是否存在用户ID"，但 m_mapIDToRoomid 的key是roomid不是userid！
        //这导致几乎总是return，加入房间的后续逻辑（互换成员信息）完全不执行，
        //其他房间成员看不到加入者，加入者也看不到其他成员。
        //修复：应该检查 m_mapIDToUserInfo 中是否存在该用户ID
        if( !m_mapIDToUserInfo.IsExist(rq->m_UserID)) return;
        UserInfo * joiner = m_mapIDToUserInfo.find(rq->m_UserID);

        STRU_ROOM_MEMBER_RQ joinrq;
        joinrq.m_UserID = rq->m_UserID;
        strcpy(joinrq.m_szUser , joiner->m_userName);
        //給自己用於更新自己的信息
        SendData( clientfd , (char*)&joinrq , sizeof (joinrq));

        //根據房間號拿到房間成員列表
        list<int> lstRoomMem = m_mapIDToRoomid.find(rq->m_RoomID);
        //互換信息
        for( auto ite = lstRoomMem.begin() ; ite!=lstRoomMem.end() ; ++ite)
        {
            int Memid = *ite;
            //【Bug修复】原代码 if( !m_mapIDToUserInfo.IsExist(Memid)) return;
            //遍历成员列表时如果某个成员不在线就直接return，导致后续成员信息不发送
            //修复：改为 continue，跳过不在线的成员，继续处理其他成员
            if( !m_mapIDToUserInfo.IsExist(Memid)) continue;
            UserInfo * memInfo = m_mapIDToUserInfo.find(Memid);

            STRU_ROOM_MEMBER_RQ memrq;
            memrq.m_UserID = memInfo->m_id;
            strcpy(memrq.m_szUser , memInfo->m_userName);

            //把加入人的信息發送給每一個房間內成員
            SendData(memInfo->m_sock , (char*)&joinrq ,sizeof (joinrq));
            //房間內成員每個人信息發給加入人
            SendData(clientfd , (char*)&memrq, sizeof (memrq));
        }
        //加入人，添加到房間列表
        lstRoomMem.push_back( rq->m_UserID);
        m_mapIDToRoomid.insert(rq->m_RoomID , lstRoomMem);


}

//離開房間
void CLogic::LeaveRoomRq(int clientfd, char *szbuf, int nlen)
{
    //拆包
    STRU_LEAVEROOM_RQ * rq = (STRU_LEAVEROOM_RQ*)szbuf;
    //看房间是否存在
    if(!m_mapIDToRoomid.IsExist(rq->m_RoomID)) return;
    //如果房间存在。可以获得用户列表
    list< int > lst = m_mapIDToRoomid.find( rq->m_RoomID );
    for( auto ite = lst.begin(); ite != lst.end();)
    {
        int userid = *ite;
        //遍历每个用户
        //是不是自己，如果是自己，从列表中去除--从房间移除了
        if( userid == rq->m_nUserID )
        {
            ite = lst.erase(ite);
        }else{
            //用户是否在线，在线转发
            if( m_mapIDToUserInfo.IsExist(userid))
            {
                UserInfo *info = m_mapIDToUserInfo.find( userid );
                SendData( info->m_sock , szbuf , nlen);
            }
            ++ite;
        }
    }



    //列表是否节点数为0->map项去掉
    if( lst.size() == 0)
    {
        m_mapIDToRoomid.erase( rq->m_RoomID );
        return;
    }

    //更新房间成员列表
    m_mapIDToRoomid.insert( rq->m_RoomID , lst);
}

//處理音頻幀
void CLogic::AudioFrameRq(int clientfd, char *szbuf, int nlen)
{
    //拆包
    char * tmp = szbuf;
    //跳過type
    tmp += sizeof(int);
    //讀取userid
    int uid = *(int*) tmp;
    //跳過userid
    tmp += sizeof(int);
    //獲取roomid
    int roomid = *(int*) tmp;

    //看房間是否存在
    if( !m_mapIDToRoomid.IsExist( roomid )) return;
    list<int> lst = m_mapIDToRoomid.find(roomid);
    //獲取成員列表
    for( auto ite = lst.begin() ; ite != lst.end() ; ++ite)
    {
        //看是否在線，轉發
        int userid = *ite;
        //屏蔽掉自己 不轉發
        if( uid == userid ) continue;
        //【Bug修复】原代码 if( !m_mapIDToRoomid.IsExist(userid)) continue;
        //检查的是roomid的map中是否存在userid，逻辑错误
        //修复：应该检查 m_mapIDToUserInfo 中是否存在该用户
        if( !m_mapIDToUserInfo.IsExist(userid)) continue;
        UserInfo * userinfo = m_mapIDToUserInfo.find(userid);
        //原樣轉發
        SendData( userinfo->m_sock , szbuf , nlen);

    }
}

void CLogic::VideoFrameRq(int clientfd, char *szbuf, int nlen)
{
    //拆包
    char * tmp = szbuf;
    //跳過type
    tmp += sizeof(int);
    //讀取userid
    int uid = *(int*) tmp;
    //跳過userid
    tmp += sizeof(int);
    //獲取roomid
    int roomid = *(int*) tmp;

    //看房間是否存在
    if( !m_mapIDToRoomid.IsExist( roomid )) return;
    list<int> lst = m_mapIDToRoomid.find(roomid);
    //獲取成員列表
    for( auto ite = lst.begin() ; ite != lst.end() ; ++ite)
    {
        //看是否在線，轉發
        int userid = *ite;
        //屏蔽掉自己 不轉發
        if( uid == userid ) continue;
        //【Bug修复】原代码 if( !m_mapIDToRoomid.IsExist(userid)) continue;
        //检查的是roomid的map中是否存在userid，逻辑错误
        //修复：应该检查 m_mapIDToUserInfo 中是否存在该用户
        if( !m_mapIDToUserInfo.IsExist(userid)) continue;
        UserInfo * userinfo = m_mapIDToUserInfo.find(userid);
        //【Bug修复】原代码 SendData( userinfo->m_audiofd , szbuf , nlen);
        //视频帧发送到了音频fd（m_audiofd），而不是视频fd（m_videofd）！
        //客户端的视频接收线程监听的是m_videofd，发到m_audiofd自然收不到视频帧，
        //导致wdg_UserShow不同步画面。
        //修复：发送到 m_videofd；如果 m_videofd 为0（未注册），回退到 m_sock
        if( userinfo->m_videofd != 0 )
            SendData( userinfo->m_videofd , szbuf , nlen);
        else
            SendData( userinfo->m_sock , szbuf , nlen);

    }
}

//音頻注冊
void CLogic::AudioRegister(int clientfd, char *szbuf, int nlen)
{
    //拆包
    STRU_AUDIO_REGISTER *rq = (STRU_AUDIO_REGISTER * )szbuf;
    int userid = rq->m_userid;
    //根據userid找到節點 更新fd
    if( m_mapIDToUserInfo.IsExist(userid) )
    {
        UserInfo * info = m_mapIDToUserInfo.find(userid);
        info->m_audiofd = clientfd;
    }
}

//視頻注冊
void CLogic::VideoRegister(int clientfd, char *szbuf, int nlen)
{
    //拆包
    STRU_VIDEO_REGISTER *rq = (STRU_VIDEO_REGISTER * )szbuf;
    int userid = rq->m_userid;
    //根據userid找到節點 更新fd
    if( m_mapIDToUserInfo.IsExist(userid) )
    {
        UserInfo * info = m_mapIDToUserInfo.find(userid);
        //【Bug修复】原代码 info->m_audiofd = clientfd;
        //视频注册却更新了音频fd（m_audiofd），导致视频fd（m_videofd）始终为0，
        //VideoFrameRq中发送视频帧到m_audiofd，客户端视频线程收不到。
        //修复：更新 m_videofd
        info->m_videofd = clientfd;
    }
}

//【拓展优化-新增】补全邮箱请求处理
//功能：已注册用户登录后若邮箱为空，客户端弹出对话框让用户补全邮箱
//用户提交后通过此函数更新数据库中的 email 字段
//流程：
//  1. 拆包获取 userid 和 email
//  2. 校验邮箱格式（简单校验：包含 @ 和 .）
//  3. 检查邮箱是否已被其他用户使用
//  4. 更新 t_user 表中该 userid 的 email 字段
//  5. 返回补全结果
void CLogic::UpdateEmailRq(int clientfd, char * szbuf, int nlen)
{
    //1.拆包
    STRU_UPDATE_EMAIL_RQ * rq = (STRU_UPDATE_EMAIL_RQ *)szbuf;
    STRU_UPDATE_EMAIL_RS rs;

    //【安全修复-IDOR】m_userid 直接取自客户端包体，原代码拿它就去改库，
    //  等于"声称自己是几号就能改几号的邮箱" —— 任何登录用户都能改写他人邮箱。
    //  校验办法：这个 id 必须已在线（登录过），且它登记的连接就是当前这条连接。
    //  m_mapIDToUserInfo 在 LoginRq 里按 (id → UserInfo{含 m_sock}) 登记。
    UserInfo * pSelf = m_mapIDToUserInfo.find( rq->m_userid );
    if( NULL == pSelf || pSelf->m_sock != clientfd )
    {
        fprintf(stderr, "[UpdateEmailRq] 越权被拒：userid=%d 与当前连接不匹配\n", rq->m_userid);
        rs.m_lresult = 0;
        SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }

    //2.简单校验邮箱格式（包含 @ 和 .）
    string strEmail = rq->m_email;
    if( strEmail.find('@') == string::npos || strEmail.find('.') == string::npos )
    {
        rs.m_lresult = 0; //失败：邮箱格式错误
        SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }

    //【安全修复-SQL注入】m_email 来自客户端包体，下面要拼进两条 SQL。
    //  第 2 步只校验了"包含 @ 和 ."，**没有排除引号** —— 形如
    //  `a@b.com' or '1'='1` 的输入照样能通过格式校验，然后闭合掉单引号改写语句。
    //  m_userid 是 int，用 %d 输出，天然安全。
    std::string strSafeEmail = m_sql->EscapeMysql( rq->m_email , sizeof(rq->m_email) );

    //3.检查邮箱是否已被其他用户使用
    char sqlStr[1024] = {0};
    list<string> resList;
    sprintf(sqlStr, "select id from t_user where email = '%s' and id != %d;", strSafeEmail.c_str(), rq->m_userid);
    if(!m_sql->SelectMysql(sqlStr, 1, resList))
    {
        rs.m_lresult = 0;
        SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }
    if(resList.size() > 0)
    {
        //邮箱已被其他用户使用
        rs.m_lresult = 0;
        SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }

    //4.更新 t_user 表中该 userid 的 email 字段
    sprintf(sqlStr, "update t_user set email = '%s' where id = %d;", strSafeEmail.c_str(), rq->m_userid);
    if(!m_sql->UpdataMysql(sqlStr))
    {
        rs.m_lresult = 0;
        SendData(clientfd, (char*)&rs, sizeof(rs));
        return;
    }

    //5.返回补全成功
    rs.m_lresult = 1;
    SendData(clientfd, (char*)&rs, sizeof(rs));
}

// [新增] 处理H.264视频帧
// 功能：转发H.264编码的视频帧给房间内其他成员
// 与VideoFrameRq的区别：
//   1. 包头结构不同（H.264包含frameType、frameSeq、分片信息等字段）
//   2. 使用STRU_VIDEO_FRAME_H264_HEADER解析包头
//   3. 转发逻辑相同：根据roomid找到房间成员，转发给除发送者外的所有成员
void CLogic::VideoFrameH264Rq(int clientfd, char *szbuf, int nlen)
{

    // 1. 校验包长度
    if(nlen < (int)sizeof(STRU_VIDEO_FRAME_H264_HEADER))
    {
        return;
    }

    // 2. 解析H.264包头
    STRU_VIDEO_FRAME_H264_HEADER *header = (STRU_VIDEO_FRAME_H264_HEADER*)szbuf;
    int uid = header->m_userid;
    int roomid = header->m_roomid;

    // 3. 检查房间是否存在
    if(!m_mapIDToRoomid.IsExist(roomid)) return;
    list<int> lst = m_mapIDToRoomid.find(roomid);

    // 4. 遍历房间成员，转发给除发送者外的所有成员
    for(auto ite = lst.begin(); ite != lst.end(); ++ite)
    {
        int userid = *ite;
        // 屏蔽掉自己，不转发
        if(uid == userid) continue;
        // 检查用户是否在线
        if(!m_mapIDToUserInfo.IsExist(userid)) continue;
        UserInfo * userinfo = m_mapIDToUserInfo.find(userid);

        // 优先发送到视频fd，如果未注册则回退到主fd
        if(userinfo->m_videofd != 0)
            SendData(userinfo->m_videofd, szbuf, nlen);
        else
            SendData(userinfo->m_sock, szbuf, nlen);
    }
}

// [新增] 处理关键帧请求（PLI）
// 功能：当新成员加入房间或解码失败时，请求发送方强制发送I帧
// 流程：
//   1. 解析PLI请求包，获取请求者ID和房间ID
//   2. 根据房间ID找到房间成员列表
//   3. 将PLI请求转发给房间内其他成员（即视频发送方）
//   4. 发送方收到PLI后，强制下一帧编码为I帧
void CLogic::PliRequestRq(int clientfd, char *szbuf, int nlen)
{

    // 1. 校验包长度
    if(nlen < (int)sizeof(STRU_PLI_REQUEST))
    {
        return;
    }

    // 2. 解析PLI请求包
    STRU_PLI_REQUEST *pli = (STRU_PLI_REQUEST*)szbuf;
    int uid = pli->m_userid;
    int roomid = pli->m_roomid;

    // 3. 检查房间是否存在
    if(!m_mapIDToRoomid.IsExist(roomid)) return;
    list<int> lst = m_mapIDToRoomid.find(roomid);

    // 4. 遍历房间成员，将PLI请求转发给其他成员
    for(auto ite = lst.begin(); ite != lst.end(); ++ite)
    {
        int userid = *ite;
        // 屏蔽掉自己，不转发（请求者不需要收到自己的PLI）
        if(uid == userid) continue;
        // 检查用户是否在线
        if(!m_mapIDToUserInfo.IsExist(userid)) continue;
        UserInfo * userinfo = m_mapIDToUserInfo.find(userid);

        // 转发PLI请求
        if(userinfo->m_videofd != 0)
            SendData(userinfo->m_videofd, szbuf, nlen);
        else
            SendData(userinfo->m_sock, szbuf, nlen);
    }
}

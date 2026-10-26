#include "clogic.h"
#include "bcrypt_wrapper.h"  // [P0新增] bcrypt 密码哈希封装
#include <fcntl.h>
#include <unistd.h>

//【安全修复-P0：房间号可预测】读 4 字节真随机数；失败返回递增兜底值
static unsigned int urandom32()
{
    unsigned int v = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if( fd >= 0 )
    {
        ssize_t got = read(fd, &v, sizeof(v));
        close(fd);
        if( got == (ssize_t)sizeof(v) )
            return v;
    }
    static unsigned int s_fb = 0xC0FFEE00;
    return ++s_fb;
}

//【安全修复-P0：音视频通道劫持】生成登录会话令牌
// 取自 /dev/urandom，32 位随机值；rand()+srand(time) 可被预测，不用于鉴权。
// 生成失败返回递增兜底值（低熵但功能不中断），正常路径熵源充分。
static int gen_session_token()
{
    unsigned int token = urandom32();
    if( token != 0 )
        return (int)token;
    static unsigned int s_fallback = 0x5EED0000;
    return (int)(++s_fallback);
}

//【安全加固-P1：登录防暴力破解】三个辅助方法的实现
// 策略：60 秒滑动窗口内失败满 5 次 → 锁定 300 秒。锁定期间 LoginRq 直接回
// password_error（与口令错误同码，避免泄露"账号被锁定"这一枚举信号）。
// 锁定检查放在 bcrypt_verify 之前：bcrypt cost=12 单次约 250ms，
// 不先拦截的话攻击者刷登录即可把 CPU 打满。
#define LOGIN_MAX_FAILS     5
#define LOGIN_WINDOW_SECS   60
#define LOGIN_LOCK_SECS     300

bool CLogic::IsLoginLocked(const std::string& key)
{
    bool bLocked = false;
    time_t now = time(NULL);
    pthread_mutex_lock( &m_loginLock );
    std::map<std::string, LoginAttempt>::iterator it = m_loginAttempts.find( key );
    if( it != m_loginAttempts.end() )
    {
        if( it->second.tLockUntil > now )
        {
            bLocked = true;
        }
        else if( it->second.nFails > 0 && now - it->second.tWindowStart > LOGIN_WINDOW_SECS )
        {
            // 窗口已过期且未在锁定期：清零重新计数，防表无限膨胀
            it->second.nFails = 0;
            it->second.tWindowStart = 0;
        }
    }
    pthread_mutex_unlock( &m_loginLock );
    return bLocked;
}

void CLogic::RecordLoginFail(const std::string& key)
{
    time_t now = time(NULL);
    pthread_mutex_lock( &m_loginLock );
    LoginAttempt& a = m_loginAttempts[ key ];
    if( a.nFails == 0 || now - a.tWindowStart > LOGIN_WINDOW_SECS )
    {
        a.nFails = 0;
        a.tWindowStart = now;
    }
    ++a.nFails;
    if( a.nFails >= LOGIN_MAX_FAILS )
    {
        a.tLockUntil = now + LOGIN_LOCK_SECS;
        a.nFails = 0;   // 进入锁定期后重新计数
        fprintf(stderr, "[LoginRq] 登录标识连续失败达阈值，锁定300秒\n");
    }
    pthread_mutex_unlock( &m_loginLock );
}

void CLogic::ClearLoginFail(const std::string& key)
{
    pthread_mutex_lock( &m_loginLock );
    m_loginAttempts.erase( key );
    pthread_mutex_unlock( &m_loginLock );
}


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

//【修复-P1/P2：格式校验服务端兜底】与客户端注册/登录正则对齐的校验（纯 std 实现）。
// 背景：原 RegisterRq 完全信任客户端校验，直连协议可写入 tel="abc"、email="x" 等脏数据；
//       UpdateEmailRq 仅查 '@' 和 '.' 是否存在，"a@."、"@x.com"、".@." 均可放行入库。
// 口径：总长 <= 39（登录缓冲区 m_tel[40] 上限，超长邮箱将无法用于邮箱登录）。
static bool is_valid_tel( const std::string & tel )
{
    if( tel.size() != 11 ) return false;          // 11 位定长
    if( tel[0] != '1' ) return false;             // 1 号段
    if( tel[1] < '3' || tel[1] > '9' ) return false; // 13-19 现行号段
    for( size_t i = 2; i < tel.size(); ++i )
        if( tel[i] < '0' || tel[i] > '9' ) return false;
    return true;
}

static bool is_valid_email( const std::string & email )
{
    if( email.empty() || email.size() > 39 ) return false;
    size_t at = email.find('@');
    // 恰好一个 '@'，且 local / domain 均非空
    if( at == std::string::npos || at == 0
            || email.find('@', at + 1) != std::string::npos ) return false;
    std::string local  = email.substr( 0, at );
    std::string domain = email.substr( at + 1 );
    // local：[A-Za-z0-9._%+-]+，且不以 '.' 开头/结尾、无连续 '.'
    for( size_t i = 0; i < local.size(); ++i )
    {
        char c = local[i];
        if( !( (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
               || c == '.' || c == '_' || c == '%' || c == '+' || c == '-' ) ) return false;
    }
    if( local[0] == '.' || local[local.size() - 1] == '.' ) return false;
    if( local.find("..") != std::string::npos ) return false;
    // domain：[A-Za-z0-9.-]+，不以 '.' 开头/结尾、无连续 '.'（分段即非空）
    for( size_t i = 0; i < domain.size(); ++i )
    {
        char c = domain[i];
        if( !( (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
               || (c >= '0' && c <= '9') || c == '.' || c == '-' ) ) return false;
    }
    if( domain[0] == '.' || domain[domain.size() - 1] == '.' ) return false;
    if( domain.find("..") != std::string::npos ) return false;
    // 逐段检查：标签不得以 '-' 开头/结尾（禁 "@-x.com"、"@x-.com"）
    size_t start = 0;
    for( size_t i = 0; i <= domain.size(); ++i )
    {
        if( i == domain.size() || domain[i] == '.' )
        {
            if( domain[start] == '-' || domain[i - 1] == '-' ) return false;
            start = i + 1;
        }
    }
    // TLD：最后一个 '.' 之后须为 2 位以上纯字母
    size_t lastDot = domain.rfind('.');
    if( lastDot == std::string::npos ) return false;
    if( domain.size() - lastDot - 1 < 2 ) return false;
    for( size_t i = lastDot + 1; i < domain.size(); ++i )
    {
        char c = domain[i];
        if( !( (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ) ) return false;
    }
    return true;
}

//register
void CLogic::RegisterRq(int clientfd, char * szbuf, int nlen)
{
   //【修复-P1：包长校验缺失】强转结构体前先确认包体足够长
   // 修改原因: 包长校验只拦 <=0，1~3 字节的残包也能通过，直接强转 STRUCT 会越界读
   //           出垃圾字段（注册接口还会把垃圾拼进 SQL）。
   // 修改效果: 包体短于结构体时直接丢弃，不再进入业务逻辑。
   // 原代码: （无校验，直接拆包）
   if( nlen < (int)sizeof(STRU_REGISTER_RQ) ) return;

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

   //【修复-P2：注册格式服务端校验】原代码完全信任客户端校验，直连协议可写入
   //  tel="abc"、email="x" 等脏数据入库。此处按与客户端一致的口径复核
   //  （is_valid_tel / is_valid_email，实现见文件头部）；不合格式返回
   //  register_failed（枚举 3=通用失败，客户端已有对应提示），不写库。
   //  注：合法 tel/email 均不含 EscapeMysql 会转义的字符，对转义后字符串
   //  校验与对原文校验等价。
   if( !is_valid_tel( strSafeTel ) || !is_valid_email( strSafeEmail ) )
   {
       fprintf(stderr, "[RegisterRq] 格式校验未通过：tel长度=%d email长度=%d\n",
               (int)strSafeTel.size(), (int)strSafeEmail.size());
       rs.m_lresult = register_failed;
       m_tcp->SendData(clientfd, (char*)&rs, sizeof(rs));
       return;
   }

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
               //【安全修复-P1：定长 char[] 越界读】m_password 可能被填满不写 '\0'，
               // 直接构造 std::string 会越过字段边界。按 strnlen 截取。
               // 原代码: bcrypt_hash(rq->m_password)
               std::string strPwdCliReg( rq->m_password, strnlen(rq->m_password, sizeof(rq->m_password)) );
               std::string strPwdBcrypt = bcrypt_hash(strPwdCliReg);
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
                  //【安全修复-P1：日志脱敏】原样输出完整 SQL 会把 bcrypt 口令哈希
                  // 打进日志 —— 哈希一旦泄露即可离线撞库。改为只输出脱敏后的
                  // 业务字段与 mysql 错误码定位线索，不再输出含哈希的完整语句。
                  // 原代码: fprintf(stderr, "... sql=%s\n", sqlStr);
                  fprintf(stderr, "[RegisterRq] 写库失败，注册未完成. tel=%s（完整SQL含口令哈希，已脱敏）\n",
                          strSafeTel.c_str());
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
   //【修复-P1：包长校验缺失】同 RegisterRq，短包强转会越界读
   // 原代码: （无校验，直接拆包）
   if( nlen < (int)sizeof(STRU_LOGIN_RQ) ) return;

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
   //【安全加固-P1：登录防暴力破解】以登录标识原文为 key 先查锁，
   // 锁定期间直接回 password_error，不再进入 bcrypt 验证（防 CPU 耗尽）。
   // 原代码: （无防爆破机制）
   std::string strAttemptKey( rq->m_tel, strnlen(rq->m_tel, sizeof(rq->m_tel)) );
   if( IsLoginLocked( strAttemptKey ) )
   {
       rs.m_lresult = password_error;
       SendData(clientfd, (char*)&rs, sizeof rs);
       return;
   }

   //【安全修复-P1：定长 char[] 越界读】m_tel 是 char[40]，恶意客户端可填满
   // 不写 '\0'，直接构造 std::string 会越过字段边界续读。统一按 strnlen 截取。
   // 原代码: string strLoginId = rq->m_tel;
   string strLoginId( rq->m_tel, strnlen(rq->m_tel, sizeof(rq->m_tel)) );
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
       //【有意为之】查不到用户时同样返回 password_error，而不是 user_not_exist。
       //这是刻意的安全设计：若按真实原因返回，登录接口就成了「账号枚举」oracle，
       //攻击者可借此批量探测哪些手机号/昵称已注册。
       //代价是已注册用户输错标识时看到的提示不够精确，属于可接受的取舍。
      //user_not_exist 枚举保留在协议定义中，供未来启用更安全的注册流程时使用。
      //查不到，返回，沒此用戶
      //【安全加固-P1】计入登录失败（同码返回，不泄露账号不存在）
      RecordLoginFail( strAttemptKey );
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
      //【安全修复-P1：定长 char[] 越界读】m_password 是 char[40]，
      // 可能被恶意客户端填满不写 '\0'，bcrypt_verify/strcmp 直接用会越界读。
      // 统一按 strnlen 截取成安全串后再交给验证函数。
      // 原代码: bcrypt_verify(rq->m_password, strPwd) / strcmp(rq->m_password, ...)
      std::string strPwdCli( rq->m_password, strnlen(rq->m_password, sizeof(rq->m_password)) );
      if (bIsBcrypt) {
          bPwdCorrect = bcrypt_verify(strPwdCli, strPwd);
      } else {
          // 旧 MD5 格式：直接比较 + 自动迁移
          bPwdCorrect = (strncmp(rq->m_password, strPwd.c_str(), sizeof(rq->m_password)) == 0);
          if (bPwdCorrect) {
              std::string strNewPwd = bcrypt_hash(strPwdCli);
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
          //【安全加固-P1】计入登录失败
          RecordLoginFail( strAttemptKey );
          rs.m_lresult = password_error;
      } else {
           //密码正确
           int id = atoi(strId.c_str());
           //sock要保存起來 id->sock映射（爲了通信）
           UserInfo * pInfo = new UserInfo;
           pInfo->m_id = id;
           pInfo->m_roomid = 0;
           pInfo->m_sock = clientfd;
           //【安全修复-P0：音视频通道劫持】登录成功即签发会话令牌：
           // 之后音/视频通道注册包必须原样带回该令牌，防止任意连接冒名抢注通道。
           pInfo->m_token = gen_session_token();
           //【安全修复-缓冲区溢出】strcpy 无长度上限，strName 来自数据库 name 列，
           // 不受协议 MAX_SIZE 约束（历史数据/列定义更长即溢出 char[MAX_SIZE]）。
           // 与客户端已完成的 strncpy 加固保持一致。
           strncpy(pInfo->m_userName , strName.c_str() , sizeof(pInfo->m_userName) - 1);
           pInfo->m_userName[sizeof(pInfo->m_userName) - 1] = '\0';

           //判斷id是否在線，在線強制下線，不在線添加
           if(m_mapIDToUserInfo.IsExist(pInfo->m_id))
           {
               //【修复-P1：强制下线泄漏 UserInfo 且旧连接不关闭】
               // 修改原因: 原代码只 erase 内存项，旧 UserInfo 从不 delete（泄漏），
               //           旧连接 fd 也不关闭（残留连接仍可收发包）。
               // 修改效果: 取出旧 UserInfo，close 其三个连接 fd 后 delete。
               //           close 后在途 recv 立即出错，recv_task 走既有错误路径统一
               //           回收事件结构，不会 double-free；旧连接随即真正断开。
               // 原代码: m_mapIDToUserInfo.erase(pInfo->m_id);（仅此一行，TODO 注释保留在下方）
               UserInfo * pOld = m_mapIDToUserInfo.find( pInfo->m_id );
               if( NULL != pOld )
               {
                   if( pOld->m_sock    > 0 ) ::close( pOld->m_sock );
                   if( pOld->m_audiofd > 0 ) ::close( pOld->m_audiofd );
                   if( pOld->m_videofd > 0 ) ::close( pOld->m_videofd );
                   delete pOld;
                   fprintf(stderr, "[LoginRq] userid=%d 重复登录，旧连接已强制下线\n", pInfo->m_id);
               }
               m_mapIDToUserInfo.erase(pInfo->m_id);
           }
           m_mapIDToUserInfo.insert(pInfo->m_id , pInfo);
           //【安全加固-P1】登录成功，清除该标识的失败记录
           ClearLoginFail( strAttemptKey );
           //寫返回包 返回 帶id和結果
           rs.m_userid = id;
           //【安全修复-P0：音视频通道劫持】令牌随登录回复下发
           rs.m_token = pInfo->m_token;
           strncpy(rs.m_name , pInfo->m_userName , sizeof(rs.m_name) - 1);
           rs.m_name[sizeof(rs.m_name) - 1] = '\0';
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
    //【修复-P1：包长校验缺失】同 RegisterRq，短包强转会越界读
    // 原代码: （无校验，直接拆包）
    if( nlen < (int)sizeof(STRU_CREATEROOM_RQ) ) return;

    //拆包
    STRU_CREATEROOM_RQ* rq = (STRU_CREATEROOM_RQ*)szbuf;
    //【安全修复-P0：冒名建房】m_UserID 取自包体，不校验与连接的绑定关系时，
    // 任意连接可冒充任意在线用户建房。与 UpdateEmailRq 的 IDOR 校验同一思路：
    // 该 id 必须在线，且其登记的主连接就是当前这条连接。
    // 原代码: （无归属校验，直接用 rq->m_UserID 建房）
    {
        UserInfo * pSelf = m_mapIDToUserInfo.find( rq->m_UserID );
        if( NULL == pSelf || pSelf->m_sock != clientfd ) return;
    }
    //【P2修复：房间成员列表非原子读改写】建房查重+插入整体持锁
    // 修改原因: 原代码 do-while(IsExist)+insert 无锁，两个线程可能拿到同一个随机房间号
    // 修改效果: 查重与插入原子化，房间号不再撞号
    // 原代码保留备查：下方 do-while 与 insert 原样，仅外包 m_roomLock
    pthread_mutex_lock( &m_roomLock );
    //房間號 1-8位隨機數
    //【安全修复-P1：房间号可预测】原 rand()+srand(time) 播种可被预测，
    // 攻击者可枚举他人房间号后强行加入窃听。改用 /dev/urandom 真随机。
    // 原代码: roomid = rand()%99999999+1;
    int roomid = 0;
    do{
        roomid = (int)( urandom32() % 99999999u ) + 1;

    }while(m_mapIDToRoomid.IsExist( roomid ));
    list<int> lst;
    lst.push_back( rq->m_UserID );
    m_mapIDToRoomid.insert( roomid , lst);
    pthread_mutex_unlock( &m_roomLock );
    //回復
    STRU_CREATEROOM_RS rs;
    rs.m_RoomID = roomid;
    rs.m_lresult = 1;

    SendData( clientfd , (char*)&rs , sizeof (rs));

}

//加入房間
void CLogic::JoinRoomRq(int clientfd, char *szbuf, int nlen)
{
    //【修复-P1：包长校验缺失】同 RegisterRq，短包强转会越界读
    // 原代码: （无校验，直接拆包）
    if( nlen < (int)sizeof(STRU_JOINROOM_RQ) ) return;

    //拆包
    STRU_JOINROOM_RQ * rq = (STRU_JOINROOM_RQ*)szbuf;
    //【安全修复-P0：冒名进房】同 CreateRoomRq：userid 必须在线且主连接即当前连接
    // 原代码: （无归属校验）
    {
        UserInfo * pSelfChk = m_mapIDToUserInfo.find( rq->m_UserID );
        if( NULL == pSelfChk || pSelfChk->m_sock != clientfd ) return;
    }
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
        //【修复-P0 配套：find 结果判空】
        // 修改原因: MyMap::find 已修复为不污染 map（不再对不存在的 key 插入默认值），
        //           但本函数在解引用 joiner 前依赖 IsExist 与 find 两次加锁之间存在
        //           时间窗口（并发下线可能刚好移除该用户）。多一层判空兜底，
        //           杜绝 joiner->m_userName 空指针解引用（这正是 P0 的崩溃点）。
        // 修改效果: 极端并发下安全跳过，不再崩溃；正常路径行为不变。
        // 原代码: （无判空，直接 joiner->m_userName）
        if( NULL == joiner ) return;

        STRU_ROOM_MEMBER_RQ joinrq;
        joinrq.m_UserID = rq->m_UserID;
        strncpy(joinrq.m_szUser , joiner->m_userName , sizeof(joinrq.m_szUser) - 1);
        joinrq.m_szUser[sizeof(joinrq.m_szUser) - 1] = '\0';
        //給自己用於更新自己的信息
        SendData( clientfd , (char*)&joinrq , sizeof (joinrq));

        //根據房間號拿到房間成員列表
        //【P2修复：房间成员列表非原子读改写】「find拷贝->push_back->insert写回」整体持锁
        // 修改原因: 三步各自短暂加锁但整体不原子，与 LeaveRoomRq/onClientClosed 并发时
        //           互相覆盖成员列表（lost update），可能出现“人已离开却仍在列表里收帧”。
        // 修改效果: 该序列由 m_roomLock 串行化；SendData 仅发送小包，持锁发送风险可控。
        // 原代码保留备查：下方 find/遍历/push_back/insert 语句原样，仅外包 m_roomLock
        pthread_mutex_lock( &m_roomLock );
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
            strncpy(memrq.m_szUser , memInfo->m_userName , sizeof(memrq.m_szUser) - 1);
            memrq.m_szUser[sizeof(memrq.m_szUser) - 1] = '\0';

            //把加入人的信息發送給每一個房間內成員
            SendData(memInfo->m_sock , (char*)&joinrq ,sizeof (joinrq));
            //房間內成員每個人信息發給加入人
            SendData(clientfd , (char*)&memrq, sizeof (memrq));
        }
        //加入人，添加到房間列表
        lstRoomMem.push_back( rq->m_UserID);
        m_mapIDToRoomid.insert(rq->m_RoomID , lstRoomMem);
        pthread_mutex_unlock( &m_roomLock );//【P2修复】配对解锁，见上


}

//離開房間
void CLogic::LeaveRoomRq(int clientfd, char *szbuf, int nlen)
{
    //【修复-P1：包长校验缺失】同 RegisterRq，短包强转会越界读
    // 原代码: （无校验，直接拆包）
    if( nlen < (int)sizeof(STRU_LEAVEROOM_RQ) ) return;

    //拆包
    STRU_LEAVEROOM_RQ * rq = (STRU_LEAVEROOM_RQ*)szbuf;
    //【安全修复-P0：冒名退房】同 CreateRoomRq：userid 必须在线且主连接即当前连接，
    // 否则任意连接可把任意用户"移出"房间并向全房间广播。
    // 原代码: （无归属校验）
    {
        UserInfo * pSelfChk = m_mapIDToUserInfo.find( rq->m_nUserID );
        if( NULL == pSelfChk || pSelfChk->m_sock != clientfd ) return;
    }
    //看房间是否存在
    //【P2修复：房间成员列表非原子读改写】「find拷贝->erase自己->写回/删房」整体持锁
    // 修改原因: 与 JoinRoomRq 并发时互相覆盖成员列表（lost update）
    // 修改效果: 成员增删由 m_roomLock 串行化；原代码语句原样保留，仅外包锁
    pthread_mutex_lock( &m_roomLock );
    if(!m_mapIDToRoomid.IsExist(rq->m_RoomID))
    {
        pthread_mutex_unlock( &m_roomLock );//【P2修复】提前返回前解锁
        return;
    }
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
        pthread_mutex_unlock( &m_roomLock );//【P2修复】提前返回前解锁
        return;
    }

    //更新房间成员列表
    m_mapIDToRoomid.insert( rq->m_RoomID , lst);
    pthread_mutex_unlock( &m_roomLock );//【P2修复】配对解锁，见上
}

//處理音頻幀
void CLogic::AudioFrameRq(int clientfd, char *szbuf, int nlen)
{
    //【修复-P1：包长校验缺失】帧头至少 3 个 int（type/userid/roomid）
    // 修改原因: 短包直接读 *(int*)tmp 会越界。
    // 修改效果: 不足 12 字节直接丢弃。
    // 原代码: （无校验，直接读偏移）
    if( nlen < (int)(3 * sizeof(int)) ) return;

    //【修复-P1：IDOR 帧归属校验】uid 全取自包体，任何人可伪造他人身份注帧
    // 修改原因: 原代码拿到 uid/roomid 就转发，未验证"这条连接真的属于 uid"。
    //           任意连接可冒充任意在线用户向任意房间注入音频帧。
    // 修改效果: uid 必须已登录，且 clientfd 必须命中该用户的
    //           m_sock/m_audiofd/m_videofd 之一，否则丢弃。与 UpdateEmailRq 的
    //           IDOR 修复同一套校验思路；正常客户端帧本就从自己的连接发出，不受影响。
    // 原代码: （无归属校验）
    {
        char * pChk = szbuf + sizeof(int);
        int uidChk = *(int*)pChk;
        UserInfo * pSelf = m_mapIDToUserInfo.find( uidChk );
        if( NULL == pSelf ||
            ( pSelf->m_sock    != clientfd &&
              pSelf->m_audiofd != clientfd &&
              pSelf->m_videofd != clientfd ) )
            return;
    }

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
    //【修复-P1：包长校验缺失】帧头至少 3 个 int（type/userid/roomid）
    // 原代码: （无校验，直接读偏移）
    if( nlen < (int)(3 * sizeof(int)) ) return;

    //【修复-P1：IDOR 帧归属校验】同 AudioFrameRq
    // 修改效果: 伪造他人 userid 的视频帧直接丢弃；正常发送不受影响。
    // 原代码: （无归属校验）
    {
        char * pChk = szbuf + sizeof(int);
        int uidChk = *(int*)pChk;
        UserInfo * pSelf = m_mapIDToUserInfo.find( uidChk );
        if( NULL == pSelf ||
            ( pSelf->m_sock    != clientfd &&
              pSelf->m_audiofd != clientfd &&
              pSelf->m_videofd != clientfd ) )
            return;
    }

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
    //【修复-P1：包长校验缺失】同 RegisterRq，短包强转会越界读
    // 原代码: （无校验，直接拆包）
    if( nlen < (int)sizeof(STRU_AUDIO_REGISTER) ) return;

    //拆包
    STRU_AUDIO_REGISTER *rq = (STRU_AUDIO_REGISTER * )szbuf;
    int userid = rq->m_userid;
    //根據userid找到節點 更新fd
    //【安全修复-P0：音视频通道劫持】原代码仅凭包体里的 userid 就把当前连接登记为
    // 该用户的音频通道 —— 任何连接发送此包即可劫持他人的音频流。
    // 现要求注册包携带登录时下发的会话令牌（rs.m_token），比对通过才登记；
    // 老客户端（不带令牌）token 恒为 0，与任何会话令牌都不匹配，会被拒绝。
    // 原代码: if( m_mapIDToUserInfo.IsExist(userid) ){ info->m_audiofd = clientfd; }
    if( m_mapIDToUserInfo.IsExist(userid) )
    {
        UserInfo * info = m_mapIDToUserInfo.find(userid);
        if( NULL == info || info->m_token != rq->m_token )
        {
            fprintf(stderr, "[AudioRegister] 通道注册被拒：userid=%d 令牌不匹配\n", userid);
            return;
        }
        info->m_audiofd = clientfd;
    }
}

//視頻注冊
void CLogic::VideoRegister(int clientfd, char *szbuf, int nlen)
{
    //【修复-P1：包长校验缺失】同 RegisterRq，短包强转会越界读
    // 原代码: （无校验，直接拆包）
    if( nlen < (int)sizeof(STRU_VIDEO_REGISTER) ) return;

    //拆包
    STRU_VIDEO_REGISTER *rq = (STRU_VIDEO_REGISTER * )szbuf;
    int userid = rq->m_userid;
    //根據userid找到節點 更新fd
    //【安全修复-P0：音视频通道劫持】同 AudioRegister：须携带登录会话令牌。
    // 原代码: if( m_mapIDToUserInfo.IsExist(userid) ){ info->m_videofd = clientfd; }
    if( m_mapIDToUserInfo.IsExist(userid) )
    {
        UserInfo * info = m_mapIDToUserInfo.find(userid);
        if( NULL == info || info->m_token != rq->m_token )
        {
            fprintf(stderr, "[VideoRegister] 通道注册被拒：userid=%d 令牌不匹配\n", userid);
            return;
        }
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
//  2. 校验邮箱格式（is_valid_email：与客户端注册/登录同口径）
//  3. 检查邮箱是否已被其他用户使用
//  4. 更新 t_user 表中该 userid 的 email 字段
//  5. 返回补全结果
void CLogic::UpdateEmailRq(int clientfd, char * szbuf, int nlen)
{
    //【修复-P1：包长校验缺失】同 RegisterRq，短包强转会越界读
    // 修改效果: 包体短于结构体直接丢弃，后续 IDOR 校验/SQL 不再接触垃圾字段。
    // 原代码: （无校验，直接拆包）
    if( nlen < (int)sizeof(STRU_UPDATE_EMAIL_RQ) ) return;

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

    //2.校验邮箱格式
    //【修复-P1：格式口径升级】原代码仅查 '@' 和 '.' 是否存在，
    //  "a@."、"@x.com"、".@." 均可放行入库。现改为与客户端注册/登录
    //  同一口径的完整校验（is_valid_email，实现见文件头部）。
    string strEmail = rq->m_email;
    if( !is_valid_email( strEmail ) )
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

    //【修复-P1：IDOR 帧归属校验】同 AudioFrameRq：uid 必须已登录且 clientfd 命中其三个 fd 之一
    // 修改效果: 伪造他人 userid 的 H.264 帧/分片直接丢弃，防止向他人房间注帧。
    // 原代码: （无归属校验）
    {
        UserInfo * pSelf = m_mapIDToUserInfo.find( uid );
        if( NULL == pSelf ||
            ( pSelf->m_sock    != clientfd &&
              pSelf->m_audiofd != clientfd &&
              pSelf->m_videofd != clientfd ) )
            return;
    }

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

    //【修复-P1：IDOR 帧归属校验】同 AudioFrameRq
    // 修改效果: 伪造他人 userid 的 PLI 请求直接丢弃，防止恶意触发他人编码器刷 I 帧。
    // 原代码: （无归属校验）
    {
        UserInfo * pSelf = m_mapIDToUserInfo.find( uid );
        if( NULL == pSelf ||
            ( pSelf->m_sock    != clientfd &&
              pSelf->m_audiofd != clientfd &&
              pSelf->m_videofd != clientfd ) )
            return;
    }

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

//【修复-P1：断开无业务层清理】连接断开回调实现
// 功能：网络层关闭 fd 时回调至此，按 fd 清理业务层状态。
// 修改原因: 原代码连接断开后 m_mapIDToUserInfo 与 m_mapIDToRoomid 永不清理——
//           UserInfo* 泄漏、死 fd 残留；当系统把回收的 fd 分配给新连接时，
//           房间内其他成员的音视频帧会被转发到毫不相干的新连接（串线）。
// 修改效果: 1) 在锁内（MyMap::erase_if）找到并删除 m_sock/m_audiofd/m_videofd
//              命中该 fd 的 UserInfo（delete 归还内存）；
//           2) 在锁内（MyMap::for_each_locked）从所有房间成员列表移除该 userid，
//              空房间随之擦除，不再有"拷贝->写回"竞态与泄漏。
//           未登录连接（fd 不命中任何用户）直接返回，行为与旧版一致。
// 线程说明: UserInfo* 的删除存在"其他线程可能仍持有裸指针"的理论窗口，
//           该竞态是本服务端既有的指针共享设计所致，本次不扩大重构范围；
//           相比旧行为（永不清理），本修复净收益为正。
void CLogic::onClientClosed(int fd)
{
    // 1. 找到并删除挂在该 fd 上的 UserInfo（erase_if 在 MyMap 锁内完成条件判断与删除）
    list<int> lstRemoved = m_mapIDToUserInfo.erase_if(
        [fd]( int /*id*/, UserInfo* pInfo ) -> bool
        {
            return NULL != pInfo &&
                   ( pInfo->m_sock    == fd ||
                     pInfo->m_audiofd == fd ||
                     pInfo->m_videofd == fd );
        } );

    for( list<int>::iterator ite = lstRemoved.begin(); ite != lstRemoved.end(); ++ite )
    {
        int userid = *ite;
        fprintf( stderr, "[CLogic::onClientClosed] fd=%d 关联用户 userid=%d 已清理\n", fd, userid );

        // 2. 从所有房间成员列表移除该用户（for_each_locked 在 MyMap 锁内直接改 list）
        //【P2修复：房间成员列表非原子读改写】外层再加 m_roomLock，与 JoinRoom/LeaveRoom
        // 的「拷贝->修改->写回」序列互斥（锁序均为 m_roomLock -> MyMap 内部锁，无死锁）。
        // 原代码保留备查：下方 for_each_locked 与 erase 语句原样，仅外包 m_roomLock
        pthread_mutex_lock( &m_roomLock );
        list<int> lstEmptyRooms;
        m_mapIDToRoomid.for_each_locked(
            [userid, &lstEmptyRooms]( int roomid, list<int>& lstMembers ) -> void
            {
                size_t nBefore = lstMembers.size();
                lstMembers.remove( userid );
                if( nBefore != lstMembers.size() && lstMembers.empty() )
                    lstEmptyRooms.push_back( roomid );
            } );
        // 房间被删空后从 map 摘除（erase 自带加锁，此处已在锁外，安全）
        for( list<int>::iterator itRoom = lstEmptyRooms.begin();
             itRoom != lstEmptyRooms.end(); ++itRoom )
        {
            m_mapIDToRoomid.erase( *itRoom );
        }
        pthread_mutex_unlock( &m_roomLock );//【P2修复】配对解锁，见上
    }
}

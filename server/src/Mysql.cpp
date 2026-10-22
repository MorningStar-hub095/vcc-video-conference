#include "Mysql.h"



int CMysql::ConnectMysql(const char* server,const char* user,const char* password, const char* database)
{
    conn = mysql_init(NULL);  // 创建MYSQL对象。NULL表示加载默认值。
    //【安全修复-P1：mysql_init 判空】内存耗尽时返回 NULL，直接传给
    // mysql_real_connect 会段错误。判空后如实报错退出启动流程。
    // 原代码: （无判空）
    if( NULL == conn )
    {
        fprintf(stderr, "[CMysql] mysql_init 失败（内存不足）\n");
        return FALSE;
    }
    //【P2修复】原代码在 mysql_real_connect 之前调用 mysql_set_character_set，
    // 原代码保留备查：
    // mysql_set_character_set(conn,"utf8");//设置字符集为UTF8
    // 修改原因: MySQL 官方文档规定连接的字符集须在握手成功后设置；
    //           connect 前调用对本次连接不生效（应使用 mysql_options(MYSQL_SET_CHARSET_NAME)），
    //           实际生效的是 libmysql 默认的 latin1，含中文的昵称入库即乱码。
    // 修改效果: 移到 connect 成功之后设置，utf8 真正生效，中文昵称/邮箱不再乱码。
    if(!mysql_real_connect(conn,server,user,password,database,0,NULL,0))//连接数据库
    {
        //【安全修复-P1：连接失败无日志】原代码静默返回 FALSE，运维无从定位。
        // 注意：错误信息里可能含主机名/用户名，不含口令本体，可安全输出。
        fprintf(stderr, "[CMysql] 连接失败: %s (server=%s, user=%s, db=%s)\n",
                mysql_error(conn), server, user, database);
        return FALSE;// 连接失败，返回FALSE
    }
    mysql_set_character_set(conn,"utf8");//设置字符集为UTF8（P2修复：移至连接成功后，见上）
    pthread_mutex_init(&m_lock , NULL); // 初始化互斥锁

    return TRUE;  // 连接成功，返回TRUE
}

int CMysql::SelectMysql(char* szSql,int nColumn,list<string>& lst)
{
    MYSQL_RES * results = NULL;

    pthread_mutex_lock(&m_lock ); // 加锁

    if(mysql_query(conn,szSql)) {  // 查询SQL是否合法
        //【安全修复-P1：查询失败无日志】记下 mysql_error 便于定位（不含口令本体）
        fprintf(stderr, "[CMysql] query 失败: %s (sql前80字节: %.80s)\n",
                mysql_error(conn), szSql);
        pthread_mutex_unlock(&m_lock );  // 查询失败，解锁并返回FALSE
        return FALSE;
    }
    //【安全修复-P1：空结果与出错不可区分】mysql_store_result 返回 NULL 有两种可能：
    //   a) 语句本就无结果集（INSERT/UPDATE）或结果为空 —— 不该当错误；
    //   b) 读结果集出错 —— 应报错。用 mysql_field_count 区分。
    // 原代码: if(NULL == results)return FALSE;
    results = mysql_store_result(conn);  // 将结果放在result中
    pthread_mutex_unlock(&m_lock );   // 解锁

    if(NULL == results)
    {
        if( mysql_field_count(conn) > 0 )
        {
            fprintf(stderr, "[CMysql] store_result 失败: %s\n", mysql_error(conn));
            return FALSE;   // 确属出错
        }
        return TRUE;        // 无结果集的正常语句，视为成功（lst 保持为空）
    }
    //【P2修复】调用方传入的 nColumn 可能大于结果集实际列数（表结构变更/调用方笔误），
    // 原代码直接按 nColumn 循环读 record[i]，越界读 libmysql 内部内存。
    // 修改原因: 用 mysql_num_fields 把列数钳制在结果集真实列数内。
    // 修改效果: 超出部分不再读取，杜绝越界读；列数不足时行为不变。
    // 原代码保留备查：
    // while((record = mysql_fetch_row(results)))  // 遍历查询结果
    // {
    //     for(int i=0; i<nColumn; i++)
    unsigned int nRealColumn = mysql_num_fields(results);
    int nClampColumn = ( nColumn > 0 && (unsigned int)nColumn <= nRealColumn ) ? nColumn : (int)nRealColumn;
    MYSQL_ROW record;     // 定义记录行
    while((record = mysql_fetch_row(results)))  // 遍历查询结果
    {
        for(int i=0; i<nClampColumn; i++)
        {
            //【Bug修复】字段值为 SQL NULL 时 mysql_fetch_row 返回 NULL 指针，
            // 直接 push_back 会构造 std::string(NULL) -> strlen(NULL) 段错误。
            // 统一存为空串，调用方原有的"字段为空"判断逻辑不受影响。
            lst.push_back( record[i] ? record[i] : "" );
        }
    }
    //【Bug修复】原代码无 mysql_free_result，每次查询泄漏一个结果集（内存由 libmysql 分配），
    // 服务端长跑必然耗尽内存。必须在取完数据后释放。
    mysql_free_result(results);
    return TRUE;  // 查询成功，返回TRUE
}


int CMysql::UpdataMysql(char *szsql)
{
    if(!szsql)return FALSE;   // SQL为空，返回FALSE
    pthread_mutex_lock(&m_lock );   // 加锁
    if(mysql_query(conn,szsql)){    // 执行SQL语句
        //【安全修复-P1：执行失败无日志】
        fprintf(stderr, "[CMysql] update 失败: %s (sql前80字节: %.80s)\n",
                mysql_error(conn), szsql);
        pthread_mutex_unlock(&m_lock );  // 执行失败，解锁并返回FALSE
        return FALSE;
    }
    pthread_mutex_unlock(&m_lock );  // 解锁
    return TRUE;   // SQL执行成功，返回TRUE
}

//【安全修复-SQL注入】对外提供「SQL 字面量转义」能力。
//
// 为什么必须用 mysql_real_escape_string，而不是手写 字符串替换：
//   手写替换（把 ' 换成 \'）在 GBK / Big5 这类多字节字符集下能被绕过：
//   某些汉字的第二字节恰是 0x5C（反斜杠），替换后会被前面的字节吞成半个汉字，
//   留下一个裸引号。官方 API 按【连接字符集】处理多字节，不会拆错。
//
// 为什么要传 nMaxLen：
//   协议包体里的字段是定长 char[]，恶意客户端可以填满整个数组而不写 '\0'。
//   此时 strlen 会越过字段边界一直读到下一个字段，导致长度算错、转义范围错。
//   所以长度必须由调用方按 sizeof(字段) 限定死。
std::string CMysql::EscapeMysql(const char* szSrc, size_t nMaxLen)
{
    std::string strOut;
    if( NULL == szSrc || NULL == conn )
    {
        // 取不到连接时返回空串而不是原串：宁可让这条业务失败，也不能放行未转义的值
        return strOut;
    }

    size_t nLen = 0;
    while( nLen < nMaxLen && szSrc[nLen] != '\0' ) ++nLen;

    // 最坏情况：每个字符都要被转义（长度翻倍），再留 1 字节余量
    strOut.assign( nLen * 2 + 1 , '\0' );
    //【P2修复】原代码无锁直接使用共享 conn，与 SelectMysql/UpdataMysql 里的
    // mysql_query 并发时可能读到半条 SQL 的连接状态（libmysql 的 conn 非线程安全）。
    // 修改原因: mysql_real_escape_string 依赖连接字符集状态，必须与 query 互斥。
    // 修改效果: 转义期间持锁，杜绝并发损坏；函数返回前释放锁，调用方行为不变。
    // 原代码保留备查：
    // unsigned long nRet = mysql_real_escape_string( conn , &strOut[0] , szSrc , (unsigned long)nLen );
    pthread_mutex_lock(&m_lock);
    unsigned long nRet = mysql_real_escape_string( conn , &strOut[0] , szSrc , (unsigned long)nLen );
    pthread_mutex_unlock(&m_lock);
    strOut.resize( nRet );
    return strOut;
}

void CMysql::DisConnect()
{
    mysql_close(conn);  // 关闭数据库连接
}

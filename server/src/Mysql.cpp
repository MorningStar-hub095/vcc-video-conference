#include "Mysql.h"



int CMysql::ConnectMysql(const char* server,const char* user,const char* password, const char* database)
{
    conn = mysql_init(NULL);  // 创建MYSQL对象。NULL表示加载默认值。
    mysql_set_character_set(conn,"utf8");//设置字符集为UTF8
    if(!mysql_real_connect(conn,server,user,password,database,0,NULL,0))//连接数据库
    {
        return FALSE;// 连接失败，返回FALSE
    }
    pthread_mutex_init(&m_lock , NULL); // 初始化互斥锁

    return TRUE;  // 连接成功，返回TRUE
}

int CMysql::SelectMysql(char* szSql,int nColumn,list<string>& lst)
{
    MYSQL_RES * results = NULL;

    pthread_mutex_lock(&m_lock ); // 加锁

    if(mysql_query(conn,szSql)) {  // 查询SQL是否合法
        pthread_mutex_unlock(&m_lock );  // 查询失败，解锁并返回FALSE
        return FALSE;  
    }
    results = mysql_store_result(conn);  // 将结果放在result中
    pthread_mutex_unlock(&m_lock );   // 解锁

    if(NULL == results)return FALSE;   // result为空，返回FALSE
    MYSQL_ROW record;     // 定义记录行
    while((record = mysql_fetch_row(results)))  // 遍历查询结果
    {
        for(int i=0; i<nColumn; i++)   
        {
            lst.push_back( record[i] );  // 将查询结果存入list
        }
    }
    return TRUE;  // 查询成功，返回TRUE
}


int CMysql::UpdataMysql(char *szsql)
{
    if(!szsql)return FALSE;   // SQL为空，返回FALSE
    pthread_mutex_lock(&m_lock );   // 加锁
    if(mysql_query(conn,szsql)){    // 执行SQL语句
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
    unsigned long nRet = mysql_real_escape_string( conn , &strOut[0] , szSrc , (unsigned long)nLen );
    strOut.resize( nRet );
    return strOut;
}

void CMysql::DisConnect()
{
    mysql_close(conn);  // 关闭数据库连接
}

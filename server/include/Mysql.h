#ifndef _MYSQL_H
#define _MYSQL_H
#include "packdef.h"
#include <mysql/mysql.h>
#include<list>
#include<string>

using namespace  std;


class CMysql{
public:
    int ConnectMysql(const char *server, const char *user, const char *password, const char *database);
    int SelectMysql(char* szSql,int nColumn,list<string>& lst);
    int UpdataMysql(char *szsql);
    void DisConnect();
    //【安全修复-SQL注入】把外部输入转义成安全的 SQL 字面量。
    //   conn 是私有成员，外部拿不到 MYSQL*，无法自行调用官方转义 API，
    //   因此必须在这里开一个出口（详见 Mysql.cpp 里的实现注释）。
    //   nMaxLen = 调用方字段宽度，用于有界取长度，防止读到相邻字段。
    std::string EscapeMysql(const char* szSrc, size_t nMaxLen);
private:
    MYSQL *conn;

    pthread_mutex_t m_lock;
};




#endif

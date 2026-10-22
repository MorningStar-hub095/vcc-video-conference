#include<INet.h>

//【安全修复-P0：WSACleanup 引用计数失衡】进程级 Winsock 引用计数
// 原代码: bool INet::m_isLoadlib = false;
int INet::m_wsaRefCount = 0;

bool INet::wsaStartupRef()
{
    if( m_wsaRefCount == 0 )
    {
        WORD wVersionRequested = MAKEWORD(2, 2);
        WSADATA wsaData;
        int err = WSAStartup(wVersionRequested, &wsaData);
        if (err != 0) {
            return false;
        }
        if (LOBYTE(wsaData.wVersion) != 2 || HIBYTE(wsaData.wVersion) != 2) {
            WSACleanup();
            return false;
        }
    }
    ++m_wsaRefCount;
    return true;
}

void INet::wsaCleanupRef()
{
    if( m_wsaRefCount > 0 )
    {
        --m_wsaRefCount;
        if( m_wsaRefCount == 0 )
            WSACleanup();
    }
}

//ip long型向字符串转换
std::string INet::GetIPString( long ip )
{
    SOCKADDR_IN sockaddr;
    sockaddr.sin_addr.S_un.S_addr = ip;
    return inet_ntoa(sockaddr.sin_addr);
}

//获取本地的所有网络ip 地址列表  key - value  std::set
std::set<long> INet::GetValidIPList()
{
    //1.获取主机名字
    char bufip[100];
    if( SOCKET_ERROR != gethostname( bufip , 100) )
    {
        //2. 获取ip地址
        struct hostent *remoteHost;

        struct in_addr addr;

        std::set<long> setIP;
        remoteHost = gethostbyname( bufip );
        int i = 0 ;
        while (remoteHost->h_addr_list[i] != 0) {
           addr.s_addr = *(u_long *) remoteHost->h_addr_list[i++];
           setIP.insert( addr.S_un.S_addr );
        }
        return setIP;
    }else
        return std::set<long>();
}

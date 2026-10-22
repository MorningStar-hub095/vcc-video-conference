#include "err_str.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

void err_str(const char* str,int err)
{
    //【P2修复：零日志静默退出】
    // 修改原因: 原实现只 exit(err)，不打印任何信息；启动失败（连不上库/端口被占）
    //           时进程直接消失，运维只能靠猜。docker 下 stderr 会被日志驱动收集。
    // 修改效果: 退出前把 str、errno 及其描述写到 stderr，方便排障；退出码语义不变。
    // 原代码保留备查：
    //     exit(err);
    fprintf( stderr , "[err_str] exit(%d): %s (errno=%d: %s)\n" ,
             err , str ? str : "(null)" , errno , strerror(errno) );
    fflush( stderr );
    exit(err);
}

#ifndef _THREADPOOL_H
#define _THREADPOOL_H
#include "packdef.h"


typedef struct
{
    void * (*task)(void*);
    void * arg;
} task_t;

typedef struct STRU_POOL_T
{
    STRU_POOL_T( int max,int min,int que_max );
    int thread_max;
    int thread_min;
    int thread_alive;
    int thread_busy;
    int thread_shutdown;
    int thread_wait;
    int queue_max;
    int queue_cur;
    int queue_front;
    int queue_rear;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    pthread_mutex_t lock;
    task_t * queue_task;
    pthread_t *tids;
    pthread_t manager_tid;
} pool_t;

class thread_pool
{
public:
    bool Pool_create(int,int,int);
    void pool_destroy();
    int Producer_add( void *(*)(void*),void*);
    //【安全修复-P0：主循环阻塞】非阻塞入队：队列满立即返回 -1，不等待。
    // 供 epoll 事件循环线程使用 —— 阻塞版 Producer_add 在队满时会卡死在
    // not_full 条件变量上，epoll 主循环停摆 = 整个服务对所有人失去响应。
    int Producer_add_nb( void *(*)(void*),void*);
    static void * Custom(void*);
    static void * Manager(void*);

    static int if_thread_alive(pthread_t);
private:
    pool_t * m_pool;
};



#endif





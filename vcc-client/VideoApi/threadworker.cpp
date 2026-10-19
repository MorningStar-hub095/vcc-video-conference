#include "threadworker.h"
#include <QDebug>

ThreadWorker::ThreadWorker(QObject *parent) : QObject(parent)
{
    m_pThread = new QThread;

    this->moveToThread( m_pThread );
    m_pThread->start();

}

ThreadWorker::~ThreadWorker()
{
    if( m_pThread )
    {
        m_pThread->quit();
        //  增加线程等待时间，避免强制终止导致资源泄漏
        // 原Bug: wait(10) 只等待10ms，线程几乎不可能在10ms内完成清理。
        //        随后调用 terminate() 强制终止线程，Qt文档明确警告：
        //        "terminate is dangerous and can cause mutex locks to be left locked,
        //         resources to be leaked, etc。"
        // 修复: 先等待3秒让线程自然退出，仅在超时后才 terminate 并打印警告。
        // 原代码: m_pThread->wait(10);
        //         if( m_pThread->isRunning()) { m_pThread->terminate(); m_pThread->wait(10); }
        if( !m_pThread->wait(3000) )
        {
            qWarning() << "ThreadWorker: thread did not exit in 3s, force terminating";
            m_pThread->terminate();
            m_pThread->wait(1000);
        }
        delete m_pThread;
        m_pThread = NULL;
    }

}

worker::~worker()
{
    qDebug() << "~worker" ;
}

void worker::slot_doWork()
{
    qDebug() << "worker:" << QThread::currentThreadId();
}

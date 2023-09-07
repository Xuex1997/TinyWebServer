#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"

// 线程池类，将它定义为模板类是为了代码复用，模板参数T是任务类
template< typename T>
class threadpool {
public:
    threadpool(int actor_model, connection_pool *connPool, int thread_number = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T* request, int state);
    bool append_p(T* request);

private:
    static void* worker(void * arg);
    void run();

private:
    int m_thread_number;                // 线程的数量
    pthread_t * m_threads;              // 线程池数组，大小为 m_thread_number
    int m_max_requests;                 // 请求队列中最多允许等待处理的请求数量
    std::list<T*> m_workqueue;          // 请求队列
    locker m_queuelocker;               // 互斥锁
    sem m_queuestat;                    // 信号量，是否有任务需要处理
    connection_pool *m_connPool;        // 数据链接库
    int m_actor_model;                  // 事件处理模式
};

template< typename T>
threadpool<T>::threadpool(int actor_model, connection_pool *connPool, int thread_number, int max_requests) :
        m_actor_model(actor_model), m_thread_number(thread_number), m_max_requests(max_requests),
        m_connPool(connPool), m_threads(NULL) {
    if ((thread_number <= 0) || (max_requests <= 0)) {
        throw std::exception();
    }

    m_threads = new pthread_t[m_thread_number];
    if (!m_threads) {
        throw std::exception();
    }

    // 创建thread_number个线程，并将它们都设置为 detach
    for (int i = 0; i < thread_number; ++i) {
        printf("create the %dth thread\n", i);
        // 在C++中，worker必须是静态函数，因为非静态函数有一个隐藏的this指针
        if (pthread_create(m_threads+i, NULL, worker, this ) != 0) {
            delete [] m_threads;
            throw std::exception();
        }
        if (pthread_detach(m_threads[i])) {
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template< typename T>
threadpool<T>::~threadpool() {
    delete [] m_threads;
}

// 向工作队列中添加请求，由工作线程来处理
template< typename T>
bool threadpool<T>::append(T* request, int state) {
    m_queuelocker.lock();
    // 如果工作队列已经超过最大请求数量，则解锁并返回false
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    request->m_state = state;
    // 将请求插入工作队列
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    // 信号量+1，以通知工作线程有任务需要处理
    m_queuestat.post();
    return true;
}

template <typename T>
bool threadpool<T>::append_p(T *request)
{
    m_queuelocker.lock();
    if (m_workqueue.size() >= m_max_requests)
    {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_queuelocker.unlock();
    m_queuestat.post();
    return true;
}

// 工作线程运行的函数，它不断从工作队列中取出任务并执行
// 这是一个静态函数，所以没有this指针，但是可以通过传入的参数arg来访问线程池的对象
template< typename T>
void * threadpool<T>::worker(void * arg) {
    threadpool * pool = (threadpool *)arg;
    pool->run();
    return pool;
}

template< typename T>
void threadpool<T>::run() {
    while (true) {
        // 信号量等待
        m_queuestat.wait();
        // 走到这里说明：被请求队列上的任务唤醒，则加互斥锁
        m_queuelocker.lock();
        // 走到这里说明：加锁成功
        if (m_workqueue.empty()) {
            // 虽然抢到了锁，但是任务已经被支持了，则解锁，再次等待被唤醒
            m_queuelocker.unlock();
            continue;
        }
        // 取出请求队列的第一个任务
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        // 解锁，让其他线程可以继续去任务执行
        m_queuelocker.unlock();
        if (!request) {
            continue;
        }
        // 执行任务
        if (m_actor_model == 1) {
            // reactor模式
            if (request->m_state == 0) {
                // 读
                if (request->read()) {
                    request->improv = 1;
                    connectionRAII mysqlcon(&request->mysql, m_connPool);
                    request->process();
                } else {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            } else {
                // 写
                if (request->write()) {
                    request->improv = 1;
                } else {
                    request->improv = 1;
                    request->timer_flag = 1;
                }
            }
        } else {
            // proactor
            connectionRAII mysqlcon(&request->mysql, m_connPool);
            request->process();
        }
    }
}

#endif
#include "sql_connection_pool.h"

connection_pool* connection_pool::get_instance() {
    static connection_pool connPool;
    return &connPool;
}

connection_pool::connection_pool() {
    m_curconn = 0;
    m_freeconn = 0;
}

// RAII机制销毁连接池
connection_pool::~connection_pool() {
    destory_pool();
}

// 初始化
void connection_pool::init(string url, string user, string password, 
            string databasename, int port, int maxconn, int close_log) {
    m_url = url;
    m_user = user;
    m_password = password;
    m_databasename = databasename;
    m_port = port;
    m_close_log = close_log;

    for (int i = 0; i < maxconn; i++) {
        MYSQL *conn = NULL;
        conn = mysql_init(conn);
        if (conn == NULL) {
            LOG_ERROR("MySQL Error");
            exit(1);
        }
        conn = mysql_real_connect(conn, url.c_str(), user.c_str(),
                        password.c_str(), databasename.c_str(), port, NULL, 0);
        if (conn == NULL) {
            LOG_ERROR("MySQL Error");
            exit(1);
        }

        m_connList.push_back(conn);
        ++m_freeconn;
    }
    m_reserve = sem(m_freeconn);
    m_maxconn = m_freeconn;
}

// 销毁数据库连接池
void connection_pool::destory_pool() {
    m_lock.lock();
    if (m_connList.size() > 0) {
        // 遍历数据库List，关闭连接
        for (auto conn : m_connList) {
            mysql_close(conn);
        }
        m_curconn = 0;
        m_freeconn = 0;
        m_connList.clear();
        m_lock.unlock();
    }
    m_lock.unlock();
}

// 当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
MYSQL* connection_pool::get_connection() {
    MYSQL* conn = NULL;
    // 与Log中的条件变量相比，如果信号量wait返回则必然得到资源然后将信号量减1
    if (m_connList.size() == 0) {
        return NULL;
    }

    m_reserve.wait();
    m_lock.lock();
    conn = m_connList.front();
    m_connList.pop_front();

    --m_freeconn;
    ++m_curconn;

    m_lock.unlock();

    return conn;
}

// 释放连接
bool connection_pool::release_connection(MYSQL* conn) {
    if (conn == NULL)
        return false;
    m_lock.lock();

    m_connList.push_back(conn);
    ++m_freeconn;
    --m_curconn;

    m_lock.unlock();
    m_reserve.post();
    return true;
}

// 当前空闲的连接数
int connection_pool::get_freeconn() {
    return m_freeconn;
}

connectionRAII::connectionRAII(MYSQL** conn, connection_pool *connPool) {
    *conn = connPool->get_connection();
    connRAII = *conn;
    poolRAII = connPool;
}
connectionRAII::~connectionRAII() {
    poolRAII->release_connection(connRAII);
}
#ifndef SQL_CONNECTION_POOL_H
#define SQL_CONNECTION_POOL_H

#include <mysql/mysql.h>
#include <list>
#include <string>
#include <iostream>
#include <string.h>
#include <stdio.h>

#include "../lock/locker.h"
#include "../log/log.h"

using namespace std;
class connection_pool {
public:
    static connection_pool* get_instance();
    void init(string url, string user, string passWord, 
            string databasename, int port, int maxconn, int close_log);
    void destory_pool();                    // 销毁所有连接
    MYSQL* get_connection();                // 获取连接
    bool release_connection(MYSQL* conn);   // 释放连接
    int get_freeconn();                     // 返回空闲连接数
    
    string m_url;               // 主机地址
    string m_port;              // 数据库端口号
    string m_user;              // 登陆数据库用户名
    string m_password;          // 登录数据库密码
    string m_databasename;      // 登录数据库名称
    int m_close_log;            // 开关日志

private:
    connection_pool();
    ~connection_pool();

    int m_curconn;      // 当前已使用连接数
    int m_freeconn;     // 当前空闲连接数
    int m_maxconn;      // 最大连接数
    locker m_lock;
    list<MYSQL *> m_connList;
    sem m_reserve;
};

class connectionRAII {
public:
    connectionRAII(MYSQL** conn, connection_pool *connPool);
    ~connectionRAII();
private:
    MYSQL* connRAII;
    connection_pool* poolRAII;
};

#endif 
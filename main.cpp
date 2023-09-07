#include "webserver.h"
using namespace std;
int main (int argc, char* argv[]) {
    int PORT = 10000;        // 端口号,默认10000
    int LOGWrite = 0;       // 日志写入方式，默认同步
    int TRIGMode = 0;       // 触发组合模式,默认listenfd LT + connfd LT
    int LISTENTrigmode = 0; // listenfd触发模式，默认LT
    int CONNTrigmode = 0;   // connfd触发模式，默认LT
    int OPT_LINGER = 0;     // 优雅关闭链接，默认不使用
    int sql_num = 8;        // 数据库连接池数量,默认8
    int thread_num = 8;     // 线程池内的线程数量,默认8
    int close_log = 0;      // 关闭日志,默认不关闭
    int actor_model = 0;    // 并发模型,默认是proactor

    int opt;
    const char *str = "p:l:m:o:s:t:c:a:";
    while ((opt = getopt(argc, argv, str)) != -1)
    {
        switch (opt)
        {
        case 'p':
        {
            PORT = atoi(optarg);
            break;
        }
        case 'l':
        {
            LOGWrite = atoi(optarg);
            break;
        }
        case 'm':
        {
            TRIGMode = atoi(optarg);
            break;
        }
        case 'o':
        {
            OPT_LINGER = atoi(optarg);
            break;
        }
        case 's':
        {
            sql_num = atoi(optarg);
            break;
        }
        case 't':
        {
            thread_num = atoi(optarg);
            break;
        }
        case 'c':
        {
            close_log = atoi(optarg);
            break;
        }
        case 'a':
        {
            actor_model = atoi(optarg);
            break;
        }
        default:
            break;
        }
    }

    //需要修改的数据库信息,登录名,密码,库名
    string user = "root";
    string passwd = "root";
    string databasename = "tinyserverdb";

    WebServer server;
    // 初始化
    server.init(PORT, user, passwd, databasename, LOGWrite, OPT_LINGER, TRIGMode,
                    sql_num, thread_num, close_log, actor_model);
    // 日志
    server.log_write();
    // 数据库
    server.sql_pool();
    // 线程池
    server.thread_pool();
    // 触发模式
    server.trig_mode();
    // 监听
    server.eventListen();
    // 运行
    server.eventLoop();
    return 0;
}
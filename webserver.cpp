#include "webserver.h"
WebServer::WebServer() {
    m_http_users = new http_conn[MAX_FD];

    //root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    m_root = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(m_root, server_path);
    strcat(m_root, root);

    //定时器
    m_users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer() {
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[0]);
    close(m_pipefd[1]);
    delete [] m_http_users;
    delete [] m_users_timer;
    delete m_pool;
}

void WebServer::init(int port , string user, string passWord, string databaseName,
              int log_write , int opt_linger, int trigmode, int sql_num,
              int thread_num, int close_log, int actor_model)
{
    m_port = port;
    m_user = user;
    m_password = passWord;
    m_databasename = databaseName;
    m_log_write = log_write;
    m_OPT_LINGER = opt_linger;
    m_TRIGMode = trigmode;
    m_sql_num = sql_num;
    m_thread_num = thread_num;
    m_close_log = close_log;
    m_actormodel = actor_model;
}

void WebServer::trig_mode() {
    // LT + LT
    if (0 == m_TRIGMode) {
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 0;
    } else if (1 == m_TRIGMode) {
        // LT + ET
        m_LISTENTrigmode = 0;
        m_CONNTrigmode = 1;
    } else if (2 == m_TRIGMode) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 0;
    } else if (3 == m_TRIGMode) {
        m_LISTENTrigmode = 1;
        m_CONNTrigmode = 1;
    }
}
void WebServer::log_write() {
    if(m_close_log == 0) {
        if (m_log_write == 1) {
            // 异步写
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 800);
        } else {
            // 同步写
            Log::get_instance()->init("./ServerLog", m_close_log, 2000, 800000, 0);
        }
    }
}

void WebServer::sql_pool() {
    // 初始化数据库连接池
    m_connpool = connection_pool::get_instance();
    m_connpool->init("localhost", m_user, m_password, m_databasename, 3306,
                    m_sql_num, m_close_log);
    // 初始化数据库读取表
    m_http_users->initmysql_result(m_connpool);
}
void WebServer::thread_pool() {
    m_pool = new threadpool<http_conn>(m_actormodel, m_connpool, m_thread_num);
}


void WebServer::eventListen() {
    m_listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);
    // 优雅关闭连接
    if (0 == m_OPT_LINGER) {
        struct linger tmp = {0, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    } else if (1 == m_OPT_LINGER) {
        struct linger tmp = {1, 1};
        setsockopt(m_listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));
    }

    // 端口复用
    int reuse = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(m_port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    int ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret != -1);
    // 监听
    ret = listen(m_listenfd, 5);

    assert(ret != -1);
    // 工具函数初始化
    m_utils.init(TIMESLOT);

    // epoll创建内核事件
    epoll_event events[MAX_EVENT_NUMBER];
    // 创建epoll对象
    m_epollfd = epoll_create(5);
    assert(m_epollfd != -1);
    // 向epoll对象中添加监听套接字
    m_utils.addfd(m_epollfd, m_listenfd, false, m_LISTENTrigmode);
    // 设置http_conn的静态变量
    http_conn::m_epollfd = m_epollfd;
    
    // 创建管道，用于计时器
    // 计时器流程：
    // 1. 调用计时函数，进行计时，时间到，函数发出SIGALRM信号;
    // 2. 程序进入注册的SIGALRME信号对应的信号捕捉函数sig_handler，该函数将接受到的信号送入管道的写端。
    // 3. epoll检测到管道有数据，程序处理管道中的信号，将timeout置为1，
    //    这使得在一次循环的最后，可以调用timer_handler函数进行处理，并且重新定时，
    //    以达到大约在超时事件内，进行超时处理得目的。
    // 4. timer_handler函数根据升序链表，对已经超时的计时器调用超时处理回调函数，
    //    删除对应sockdet上注册的epoll事件并关闭连接。
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    // 设置管道写端非阻塞
    m_utils.setnonblocking(m_pipefd[1]);
    // 设置管道读端ET非阻塞
    m_utils.addfd(m_epollfd, m_pipefd[0], false, 0);
    
    // 注册信号处理函数
    // 忽略SIGPIPE信号
    // 这里为什么会将SIGPIPE信号设置为忽略
    // 因为若客户端关闭连接，服务器端回复ACK+FIN报文后，又发送消息给客户端，会收到RST报文
    // 如果再一次发送数据给客户端就会发生SIGPIPE信号，这个信号默认会终止进程。
    m_utils.addsig(SIGPIPE, SIG_IGN);
    m_utils.addsig(SIGALRM, m_utils.sig_handler, false);
    m_utils.addsig(SIGTERM, m_utils.sig_handler, false);

    alarm(TIMESLOT);

    Utils::u_epollfd = m_epollfd;
    Utils::u_pipefd = m_pipefd;
}
void WebServer::eventLoop() {
    bool timeout = false;
    bool stop_server = false;
    while (!stop_server) {
        int number = epoll_wait(m_epollfd, m_events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR) {
            LOG_ERROR("%s", "EPOLL failure");
            break;
        }

        for (int i = 0; i < number; ++i) {
            int sockfd = m_events[i].data.fd;
            if (sockfd == m_listenfd) {
                bool flag = dealclinetdata();
                if (flag == false)
                    continue;
            } else if (m_events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR )) {
                util_timer * timer = m_users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            } else if ((sockfd == m_pipefd[0]) && (m_events[i].events & EPOLLIN)) {
                bool flag = dealwithsignal(timeout, stop_server);
                if (flag == false) {
                    LOG_ERROR("%s", "dealclientdata failure");
                }
            } else if (m_events[i].events & EPOLLIN) {
                dealwithread(sockfd);
            } else if (m_events[i].events & EPOLLOUT) {
                dealwithwrite(sockfd);
            }
        }
        if (timeout) {
            m_utils.timer_handler();
            LOG_INFO("%s", "timer tick");
            timeout = false;
        }
    }
}

void WebServer::timer(int connfd, struct sockaddr_in client_address) {
    printf("m_root:%s\n", m_root);
    m_http_users[connfd].init(connfd, client_address, m_root, m_CONNTrigmode, 
                             m_close_log, m_user, m_password, m_databasename);
    // 初始化client_data数据
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    m_users_timer[connfd].address = client_address;
    m_users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &m_users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    m_users_timer[connfd].timer = timer;
    m_utils.m_timer_lst.add_timer(timer);
}
void WebServer::adjust_timer(util_timer *timer) {
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    m_utils.m_timer_lst.adjust_timer(timer);
    LOG_INFO("%s", "adjsut timer once");
}
void WebServer::deal_timer(util_timer *timer, int sockfd) {
    timer->cb_func(&m_users_timer[sockfd]);
    if (timer) {
        m_utils.m_timer_lst.del_timer(timer);
    }
    LOG_INFO("close fd %d", m_users_timer[sockfd].sockfd);
}

bool WebServer::dealclinetdata() {
    struct sockaddr_in client_address;
    socklen_t client_addrlen = sizeof(client_address);
    if (m_LISTENTrigmode == 0)
    {   // LT
        int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlen);
        if (connfd < 0) {
            LOG_ERROR("%s: errno is:%d", "accept error", errno);
            return false;
        }
        if (http_conn::m_user_count >= MAX_FD) {
            m_utils.show_error(connfd, "Internal server busy");
            LOG_ERROR("%s", "Internal server busy");
            return false;
        }
        timer(connfd, client_address);
    } else { // ET
        while (1) {
            int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlen);
            if (connfd < 0) {
                LOG_ERROR("%s: errno is:%d", "accept error", errno);
                break;
            }
            if (http_conn::m_user_count >= MAX_FD) {
                m_utils.show_error(connfd, "Internal server busy");
                LOG_ERROR("%s", "Internal server busy");
                break;
            }
            timer(connfd, client_address);
        }
        return false;
    }
    return true;
}
bool WebServer::dealwithsignal(bool& timeout, bool& stop_server) {
    char signals[1024];
    int ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    if (ret == -1) {
       return false;
    } else if (ret == 0) {
        return false;
    } else {
        for (int i = 0; i < ret; i++) {
            switch (signals[i]) {
                case SIGALRM:
                    timeout = true;
                    break;
                case SIGTERM:
                    stop_server = true;
                    break;
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd) {
    util_timer* timer = m_users_timer[sockfd].timer;
    if (m_actormodel == 1) {
        if (timer) {
            adjust_timer(timer);
        }
        // reatcor模式，在工作线程进行数据读取
        // 将事件放入请求队列，以唤醒某个工作线程读取sockfd上的数据并处理
        m_pool->append(m_http_users + sockfd, 0);
        while (true) {
            if (m_http_users[sockfd].improv == 1) {
                // 工作线程读操作完成
                if (m_http_users[sockfd].timer_flag == 1) {
                    // 工作线程读操作出错
                    deal_timer(timer, sockfd);
                    m_http_users[sockfd].timer_flag = 0; // 重置工作线程读取出错flag
                }
                // 重置工作线程读取完成标志
                m_http_users[sockfd].improv = 0;
                break;
            } 
        }
    } else {
        // proactor模式，直接在主线程进行数据读取
        if (m_http_users[sockfd].read()) {
            LOG_INFO("deal with the client(%s)", inet_ntoa(m_http_users[sockfd].get_address()->sin_addr));
            // 主线程读取成功后，将事件放入请求队列，唤醒某个工作线程进行处理
            m_pool->append_p(m_http_users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        } else {
            // 主线程读取失败
            deal_timer(timer, sockfd);
        }
    }
}
void WebServer::dealwithwrite(int sockfd) {
    util_timer* timer = m_users_timer[sockfd].timer;
    // proatcor模式，在工作线程进行数据写入
    if (m_actormodel == 1) {
        if (timer) {
            adjust_timer(timer);
        }
        // 若监测到写事件，将该事件放入请求队列
        m_pool->append(m_http_users + sockfd, 1);
        while (true) {
            if (m_http_users[sockfd].improv == 1) {
                // 子线程进行了读操作
                if (m_http_users[sockfd].timer_flag == 1) {
                    // 子线程读操作出错
                    deal_timer(timer, sockfd);
                    m_http_users[sockfd].timer_flag = 0;
                }
                // 重置
                m_http_users[sockfd].improv = 0;
                break;
            } 
        }
    } else {
        // proactor模式，在主线程进行数据写入
        if (m_http_users[sockfd].write()) {
            LOG_INFO("send data to the client(%s)", inet_ntoa(m_http_users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        } else {
            deal_timer(timer, sockfd);
        }
    }
}
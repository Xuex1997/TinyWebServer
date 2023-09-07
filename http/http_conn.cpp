#include "http_conn.h"
#include <mysql/mysql.h>

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 所有http对象共享的锁和用户密钥对
// 或许可以使用类的静态成员进行改写
locker lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool) {
    // 先从连接池中取一个连接
    MYSQL* mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    // 在数据库的user表中检索username，passwd数据
    if (mysql_query(mysql, "SELECT username,passwd FROM user")) {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集合
    MYSQL_RES *result = mysql_store_result(mysql);
    // 返回结果集中的列数
    int num_fields = mysql_num_fields(result);
    // 返回所有字段结构的数据
    MYSQL_FIELD *fields = mysql_fetch_field(result);
    // 从结果集中获取每一行，将对应的用户名和密码存入map
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }

}

// 设置文件描述符非阻塞
void setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);
}

// 向epollfd标识的epoll内核事件表中添加fd上的事件, ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;
    // EPOLLRDHUP: TCP连接被对方关闭，或者对方关闭了写操作，或者对方关闭了半关闭连接
    // EPOLLIN: 表示对应的文件描述符可以读（包括对端SOCKET正常关闭）
    // EPOLLET: 将EPOLL设为边缘触发(Edge Triggered)模式
    if (TRIGMode == 1)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;
    // EPOLLONESHOT: 只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个socket的话，需要再次把这个socket加入到EPOLL队列里
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    // 向epoll内核事件表中注册事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epollfd标识的epoll内核事件表中删除fd上的所有注册事件
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;
    if (TRIGMode == 1)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化静态成员, 为所有连接共享
int http_conn::m_epollfd = -1;
int http_conn::m_user_count = 0;

// 初始化连接
void http_conn::init(int sockfd, const sockaddr_in & addr, char *root, int TRIGMode,
                    int close_log, string user, string passwd, string sqlname) {
    m_sockfd = sockfd;
    m_address = addr;
    
    // debug
    m_doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    // 将m_sockfd加入epoll监听事件
    addfd(m_epollfd, m_sockfd, true, m_TRIGMode);
    m_user_count++;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());
    
    init();
}

void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE; // 初始状态为检查请求行
    m_checked_idx = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    m_method = GET;
    m_url = NULL;
    m_version = NULL;
    m_host = NULL;
    m_linger = false;
    m_content_length = 0;
    bytes_to_send = 0;
    bytes_have_send = 0;
    mysql = NULL;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 关闭连接
void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        LOG_INFO("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 读取浏览器端发来的全部数据
bool http_conn::read() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    // 已经读取到的字节
    int bytes_read = 0;
    // LT读取数据
    if (m_TRIGMode == 0) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        if (bytes_read <= 0) {
            return false;
        }
        return true;
    } else { // ET读数据
        while (true) {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 读取完毕
                    break;
                } else {
                    return false;
                }
            } else if (bytes_read == 0) {
                // 对方关闭连接
                return false;
            }
            m_read_idx += bytes_read;
        }
        //printf("%s", m_read_buf);
        return true;
    }
}

// 解析和响应HTTP请求
void http_conn::process() {
    // 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    // 生成HTTP应答
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    // 使用了EPOLLONESHOT，所以需要重新注册事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = NULL;

    // 在GET请求报文中，每一行都是\r\n作为结束，所以对报文进行拆解时，仅用从状态机
    // 的状态line_status=parse_line() == LINE_OK语句即可。
    // 但在POST请求报文中，消息体的末尾没有任何字符，所以不能使用从状态机的状态，
    // 这里转而使用主状态机的状态作为循环入口条件，而且解析完消息体后，报文的完整
    // 解析就完成了，但此时主状态机的状态还是CHECK_STATE_CONTENT，也就是说，符合
    // 循环入口条件，还会再次进入循环，这并不是我们所希望的。所以判断line_status == LINE_OK
    // 并在完成消息体解析后，将line_status变量更改为LINE_OPEN，此时可以跳出循环，完成报文解析任务。
    while ( ((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK)) 
          || ((line_status = parse_line()) == LINE_OK) ) {
        // 获取一行数据
        text = get_line();
        // m_start_line是每一个数据行在m_read_buf中的起始位置
        // m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;
        LOG_INFO("get one http line: %s", text);
        // 主状态机的三种状态转移逻辑
        switch(m_check_state) {
            case CHECK_STATE_REQUESTLINE:
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            case CHECK_STATE_HEADER:
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    return do_request();
                }
                break;
            case CHECK_STATE_CONTENT:
                ret = parse_content(text);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                // 这里将从状态机设为LINE_OPEN以便在解析完请求体后循环不会再次进去
                line_status = LINE_OPEN;
                break;
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

/* 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性，
 * 如果目标文件存在、对所有用户可读，且不是目录，则使用mmap将其
 * 映射到内存地址m_file_address处，并告诉调用者获取文件成功
*/
http_conn::HTTP_CODE http_conn::do_request() {
    // "/root/xxx/xxx/xxx/index.html"
    strcpy(m_real_file, m_doc_root);
    int len = strlen(m_doc_root);
    // 在 m_url 中搜索最后一次出现字符/的位置 /index.html
    const char* p = strrchr(m_url, '/');
    // 实现登录和注册校验
    if (cgi == 1 && (*(p+1) == '2' || *(p+1) == '3'))
    {
        // 根据标志位判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char*)malloc(sizeof(char)*200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url+2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        // 将用户名和密码从请求体m_string中提取出来
        char name[100], passwd[100];
        // 格式为user=123&password=123
        int i = 0;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i-5] = m_string[i];
        name[i-5] = '\0';
        int j = 0;
        for (i = i+10; m_string[i] != '\0'; ++i, ++j)
            passwd[j] = m_string[i];
        passwd[j] = '\0';
        
        if (*(p + 1) == '3') {
            // 如果是注册，先检测数据库中是否有重名的
            if (users.find(name) == users.end()) {
                // 没有重名，则向数据库添加一条
                char *sql_insert = (char*)malloc(sizeof(char)*200);
                strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
                strcat(sql_insert, "'");
                strcat(sql_insert, name);
                strcat(sql_insert, "', '");
                strcat(sql_insert, passwd);
                strcat(sql_insert, "')");
                lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string,string>(name,passwd));
                lock.unlock();
                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            } else {
                // 有重名，注册失败
                strcpy(m_url, "/registerError.html");
            }
        } else if (*(p + 1) == '2') {
            // 如果是登陆，检查用户密码是否正确
            if (users.find(name) != users.end() && users[name] == passwd)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }
    // 如果请求的资源为/0表示跳转注册界面
    if (*(p + 1) == '0') {
        char *m_url_real = (char*)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
        
    } else if (*(p + 1) == '1') { // 如果请求资源为/1，表示跳转登录界面
        char *m_url_real = (char*)malloc(sizeof(char)*200);
        strcpy(m_url_real,"/log.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));
        free(m_url_real);
    } else if (*(p + 1) == '5') { // 如果请求资源为/5, 挑战图片页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '6') { // 如果请求资源为/6, 跳转视频页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else if (*(p + 1) == '7') { // 如果请求资源为/7, 跳转关注页面
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    } else {
        // 如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
        // 这里的情况是welcome界面，请求服务器上的一个图片
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    }

    // 通过stat获取请求资源文件信息，成功则将信息更新到 m_file_stat 结构体
    // 失败返回 NO_RESOURCE 状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }
    // 判断文件的权限,不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }
    // 判断文件类型, 如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }
    // 以只读方式打开文件,通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射
    m_file_address = (char *)mmap(NULL, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    // 表示请求文件存在，且可以访问
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = NULL;
    }
}

// 解析请求行, 获得请求方法，目标url及http版本号, GET /index.html HTTP/1.1
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    // 请求行中最先含有空格或\t任一字符的位置并返回
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *(m_url++) = '\0';

    // 取出数据，并通过与GET和POST比较，以确定请求方式
    char *method = text;
    // 忽略大小写比较
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else if (strcasecmp(method, "POST") == 0) {
        m_method = POST;
        // POST 请求，cgi标志位置为1
        cgi = 1;
    } else {
        // 目前只支持GET,POST请求
        return BAD_REQUEST;
    }
    // m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    // 将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url, " \t");
    // 使用与判断请求方式的相同逻辑，判断HTTP版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    // GET\0/index.html\0HTTP/1.1
    *(m_version++) = '\0';
    m_version += strspn(m_version, " \t");
    // 仅支持HTTP/1.1
    if(strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }
    // 有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7; // 192.168.110.129:10000/index.html
        m_url = strchr(m_url, '/'); // /index.html
    }
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    // 一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }
    
    // 当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");

    // 请求行解析完毕，状态转移到头部字段的分析
    m_check_state = CHECK_STATE_HEADER;

    return NO_REQUEST;
}

// 解析请求头，目前只支持connection字段、content-length字段、host字段
http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0') {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    } else if (strncasecmp(text, "connection:", 11) == 0) {
        // 处理Connection头部字段
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        // 处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        // 目前只支持上面三个字段
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}

// 解析请求内容
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    // 请求头中获取了m_content_length，表示请求体的长度
    // 如果目前读取到的数据已经包含了消息体，则读取成功，否则继续读取
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        // POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 从状态机，用于解析出一行内容,\r\n表示一行的结束
// 返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
// m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
// m_checked_idx指向从状态机当前正在分析的字节
http_conn::LINE_STATUS http_conn::parse_line(){
    if (m_check_state == CHECK_STATE_CONTENT) {
        if (m_read_idx >= m_content_length + m_checked_idx) {
            return LINE_OK;
        } else {
            return LINE_OPEN;
        }
    }

    char tmp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        tmp = m_read_buf[m_checked_idx];
        if (tmp == '\r') {
            if ((m_checked_idx+1) == m_read_idx) {
                // \r是最后一个字符，说明当前读取到的数据还不完整，需要继续读取
                return LINE_OPEN;
            } else if (m_read_buf[m_checked_idx+1] == '\n') {
                // \r后面接着\n，说明读取到了完整的一行
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            // 如果都不符合，则返回语法错误
            return LINE_BAD;
        } else if (tmp == '\n') {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx-1] == '\r')) {
                // \n前面是\r，说明读取到了完整的一行
                m_read_buf[m_checked_idx-1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    // 并没有找到\r\n，需要继续接收
    return LINE_OPEN;
}


bool http_conn::write(){
    int temp = 0;
    if (bytes_to_send == 0) {
        // 将要发送的字节为0，这一次响应结束
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1) {
        // 分散写
        // 将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if (temp < 0) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但这可以保证连接的完整性
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            // 如果发送失败，但不是缓冲区问题，取消映射
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        if (bytes_have_send >= m_iv[0].iov_len) {
            // 第一个iovec头部信息已发送完，发送第二个iovec数据
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            // 第一个iovec头部信息未发送完，继续发送第一个iovec头部信息
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        // 数据已全部发送完
        if (bytes_to_send <= 0) {
            unmap();
            // 在epoll上重置EPOLLONESHOT事件
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
            // 浏览器的请求为长连接
            if (m_linger) {
                // 重新初始化HTTP对象
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    // 如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    // 定义可变参数列表
    va_list arg_list;
    // 将变量arg_list初始化为传入参数
    va_start(arg_list, format);
    // 将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    // 如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
        va_end(arg_list);
        return false;
    }
    // 更新m_write_idx位置
    m_write_idx += len;
    // 清空可变参列表
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    return true;
}

bool http_conn::add_headers( int content_len ) {
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}

bool http_conn::add_content( const char* content ) {
    return add_response("%s", content);
}

// bool http_conn::add_content_type() {
//     return add_response("Content-Type:%s\r\n", "text/html");
// }

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_content_length( int content_length ) {
    return add_response("Content-Length:%d\r\n", content_length);
}

bool http_conn::add_linger() {
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            // 如果请求的资源存在
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            } else {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) {
                    return false;
                }
            }
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
#include "lst_timer.h"
#include "../http/http_conn.h"
sort_timer_lst::sort_timer_lst() : head(NULL), tail(NULL) {}

sort_timer_lst::~sort_timer_lst() {
    util_timer* tmp = head;
    while(tmp) {
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

// 按照升序将timer插入到双向链表sort_timer_lst中
void sort_timer_lst::add_timer(util_timer* timer) {
    if (!timer)
        return;
    // 链表为空，插入的节点作为首尾节点
    if (!head) {
        head = timer;
        tail = timer;
        return;
    }
    // 链表不为空，且插入的节点超时时间小于首节点的时间，而该节点作为新的首节点
    if (timer->expire < head->expire) {
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    // 链表不为空，且不是要在链表头部插入
    add_timer(timer, head);

}

// 定时器被延长，节点需要向后移动
void sort_timer_lst::adjust_timer(util_timer* timer) {
    if (!timer) return;

    // 如果修改的节点是尾节点或者节点的下一个节点依旧比它的延时长，就不用再向后移动
    if (!timer->next || (timer->next->expire > timer->expire)) return;

    // 修改的节点是头节点
    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    } else {// 修改的节点是中间节点
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
// 将目标定时器 timer 从链表中删除
void sort_timer_lst::del_timer(util_timer* timer) {
    if (!timer) return;

    if ((timer == head) && (timer == tail)) {
        delete timer;
        head = tail = NULL;
        return;
    }

    if (timer == head) {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }

    if (timer == tail) {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// SIGALARM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务
void sort_timer_lst::tick() {
    if (!head) return;
    time_t cur = time(NULL); // 当前系统时间
    util_timer* tmp = head;
    // 从头节点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
    while (tmp) {
        if (tmp->expire > cur) {
            break;
        }
        // 调用定时器的回调函数，以执行定时任务
        tmp->cb_func(tmp->user_data);
        // 执行完定时器中的定时任务之后，就将它从链表中删除，并重置链表头节点
        head = tmp->next;
        if (head) {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head) {
    util_timer* prev = lst_head;
    util_timer* cur = prev->next;
    while (cur) {
        // 当遍历到的当前节点的超时时间大于要插入的节点的超时时间，则该节点需要在当前节点的前面插入
        if(cur->expire > timer->expire) {
            prev->next = timer;
            timer->prev = prev;
            timer->next = cur;
            cur->prev = timer;
            break;
        }
        prev = cur;
        cur = cur->next;
    }
    // 遍历到最后了，说明节点都还没插入，要查到链表的尾部
    if (!cur) {
        prev->next = timer;
        timer->prev = tail;
        timer->next = NULL;
        tail = timer;
    }
}

void Utils::init(int timeslot) {
    m_TIMESLOT = timeslot;
}

//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode) {
    epoll_event event;
    event.data.fd = fd;
    if (TRIGMode == 1) {
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    } else {
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }

    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

// 信号处理函数
void Utils::sig_handler(int sig) {
    // 为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg =  sig;
    send(u_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler() {
    m_timer_lst.tick();
    alarm(m_TIMESLOT);
}

void Utils::show_error(int connfd, const char *info) {
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int* Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

void cb_func(client_data *user_data) {
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_count--;
}
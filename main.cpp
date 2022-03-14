#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "threadpool.h"
#include "httpconn.h"
#include "locker.h"
#include "time_heap.h"
#define MAX_FD 65536           // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 监听的最大的事件数量
#define TIMESLOT 5             //最小超时单位

static int pipefd[2];
static time_heap client_time_heap(1024);
static int epollfd = 0;

// 添加文件描述符
extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);
extern int setnonblocking(int fd);

//信号处理函数
void sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;

    //将信号值从管道写端写入，传输字符类型，而非整型
    send(pipefd[1], (char *)&msg, 1, 0);

    //将原来的errno赋值为当前的errno
    errno = save_errno;
}

void addsig(int sig, void(handler)(int), bool restart = true)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}
int do_error(int fd, int *error)
{
    fprintf(stderr, "error: %s\n", strerror(errno));
    *error = errno;
    while ((close(fd) == -1) && (errno == EINTR));
    errno = *error;
    return 1;
}
//定时器回调函数
void cb_func(client_data *user_data)
{
    //删除非活动连接在socket上的注册事件
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);

    //关闭文件描述符
    close(user_data->sockfd);

    //减少连接数
    http_conn::m_user_count--;
}
void timer_handler()
{
    client_time_heap.tick();
    alarm(TIMESLOT);
}


int main(int argc, char *argv[])
{

    if (argc <= 1)
    {
        printf("usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi(argv[1]);
    // int port=9999;
    int error;
    addsig(SIGPIPE, SIG_IGN);

    threadpool<http_conn> *pool = NULL;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch (...)
    {
        return 1;
    }

    http_conn *users = new http_conn[MAX_FD];

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    // 端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    if (ret == -1)
        return do_error(listenfd, &error);
    ret = listen(listenfd, 5);
    if (ret == -1)
        return do_error(listenfd, &error);

    // 创建epoll对象，和事件数组，添加
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    if (ret == -1)
        return do_error(listenfd, &error);
    // 添加到epoll对象中

    /****/
     //addfd( epollfd, listenfd, false );
    epoll_event event;
    event.data.fd = listenfd;
    event.events = EPOLLIN | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &event);
    //设置文件描述符非阻塞
    setnonblocking(listenfd);
    /****/
    //创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    bool stop_server = false;
    bool timeout = false;
    client_data *users_timer = new client_data[MAX_FD];
   // bool timeout = false;
    alarm(TIMESLOT);

    http_conn::m_epollfd = epollfd;

    while (!stop_server)
    {

        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);

        if ((number < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++)
        {

            int sockfd = events[i].data.fd;

            if (sockfd == listenfd)
            {

                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlength);

                if (connfd < 0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }

                if (http_conn::m_user_count >= MAX_FD)
                {
                    close(connfd);
                    continue;
                }
                users[connfd].init(connfd, client_address);

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                heap_timer *timer = new heap_timer(TIMESLOT);
                if (timer)
                {
                    timer->user_data = &users_timer[connfd];
                    timer->cb_func = cb_func;
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    ret = client_time_heap.add_timer(timer);
                }
                else
                {
                    fprintf(stderr, "client:%d add heap_timer failed.\n", connfd);
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                users[sockfd].close_conn();
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                {
                    continue;
                }
                else if (ret == 0)
                {
                    continue;
                }
                else
                {
                    for (int i = 0; i < ret; ++i)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }
            else if (events[i].events & EPOLLIN)
            {
                heap_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].read())
                {
                    //若监测到读事件，将该事件放入请求队列
                    pool->append(users + sockfd);
                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        client_time_heap.adjust_timer(timer);
                    }
                }
                else
                {
                   // timer->cb_func(&users_timer[sockfd]);
                    if (timer)
                    {
                        client_time_heap.del_timer(timer);
                    }
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                heap_timer *timer = users_timer[sockfd].timer;
                if (users[sockfd].write())
                {
                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if (timer)
                    {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        client_time_heap.adjust_timer(timer);
                    }
                }
                else
                {
                    if (timer)
                    {
                        client_time_heap.del_timer(timer);
                    }
                    users[sockfd].close_conn();
                }
            }
        }
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
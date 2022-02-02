#include<stdio.h>
#include<stdlib.h>
#include<pthread.h>
#include<string.h>
#include<fcntl.h>
#include<unistd.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<sys/epoll.h>
#include<arpa/inet.h>
#include<errno.h>
#define myperror(errmsg) {\
    perror(errmsg);\
    exit(1);\
}
#define PORT 2400 //服务端监听端口
#define FD_SIZE 1100 //连接的描述符大小
#define TASK_SIZE 1100 //线程池任务队列大小
pthread_mutex_t handle_mutex[FD_SIZE + 100]; //每个连接创建一把锁，解决临界区资源问题
int listenfd, epollfd;
char bufpool[FD_SIZE + 100][4100]; //缓冲区
int bufidx[FD_SIZE + 100];//缓冲区指针，用来记录缓冲区的大小

void* handler_run(void* arg);

//任务队列
typedef struct Task_Pool
{
    int task[TASK_SIZE];
    int head, tail;
    pthread_mutex_t mutex;
    pthread_cond_t notempty;
}Task_Pool;

Task_Pool taskpool;

//入队操作
void push_task(int fd)
{
    pthread_mutex_lock(&taskpool.mutex);
    if(taskpool.head == (taskpool.tail + 1))
    {
        //printf("任务队列已满！\n");
        return;
    }
    taskpool.task[taskpool.tail] = fd;
    taskpool.tail = taskpool.tail + 1;
    if(taskpool.tail == TASK_SIZE) taskpool.tail = 0;
    pthread_cond_signal(&taskpool.notempty);
    pthread_mutex_unlock(&taskpool.mutex);
}

//出队操作
int pop_task()
{
    pthread_mutex_lock(&taskpool.mutex);
    while(taskpool.head == taskpool.tail)
        pthread_cond_wait(&taskpool.notempty, &taskpool.mutex);
    int retfd = taskpool.task[taskpool.head];
    taskpool.head = taskpool.head + 1;
    if(taskpool.head == TASK_SIZE) taskpool.head = 0;
    pthread_mutex_unlock(&taskpool.mutex);
    return retfd;
}

void socket_bind()
{
    struct sockaddr_in serveraddr;
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if(listenfd < 0) myperror("socket");
    int retstatus;
    serveraddr.sin_port = htons(PORT);
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = INADDR_ANY;
    retstatus = bind(listenfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr));
    if(retstatus < 0) myperror("bind");
    retstatus = listen(listenfd, 1024);
}

//创建线程池
void taskpool_init()
{
    pthread_t thread;
    for(int i = 0; i < 1; i++)
    {
        pthread_create(&thread, NULL, handler_run, NULL);
        pthread_detach(thread);
    }
}

//将fd从事件中删除
void del_event(int fd)
{
    //将对应缓冲区下标标记为可用
    bufidx[fd] = 0;
    if((epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL)) < 0)
        myperror("EPOLL_CTL_DEL");
    close(fd);
}

//将fd加入到事件中
void add_event(int fd, int status)
{
    bufidx[fd] = 0;
    struct epoll_event ev;
    ev.events = status;
    ev.data.fd = fd;
    //设置文件为非阻塞
    int oldoption = fcntl(fd, F_GETFL, 0);
    int newoption = oldoption | O_NONBLOCK;
    fcntl(fd, F_SETFL, newoption);
    if((epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev)) < 0)
        myperror("EPOLL_CTL_ADD");
}

//handler：处理IO请求
void* handler_run(void* arg)
{
    char tempbuf[4100];
    int retlen;
    while(1)
    {
        int cur_fd = pop_task();
        retlen = read(cur_fd, tempbuf, 4100);
        pthread_mutex_lock(handle_mutex + cur_fd);
        //如果-1报错
        if(retlen < 0 && errno != EWOULDBLOCK) perror("read"), del_event(cur_fd);
        //if(retlen < 0) perror("read");
        //如果0关闭连接
        if(retlen == 0) /*printf("del\n"),*/ del_event(cur_fd);
        //write(1, tempbuf, retlen);
        //printf("\n\n\n");
        //处理内容
        //printf("accept content, length = [%d].\n", retlen);
        //获取fd所在buf缓冲区数组下标
        else
        {
            for(int i = 0; i < retlen; i++)
            {
                //实现大小写转换
                if(tempbuf[i] >= 'a' && tempbuf[i] <= 'z')
                    tempbuf[i] -= 32;
                else if(tempbuf[i] >= 'A' && tempbuf[i] <= 'Z')
                    tempbuf[i] += 32;
                bufpool[cur_fd][bufidx[cur_fd]++] = tempbuf[i];
                //收到\n时，将之前的信息统一处理
                if(tempbuf[i] == '\n')
                {
                    write(cur_fd, bufpool[cur_fd], bufidx[cur_fd]);
                    continue;
                }
            }
        }
        //printf("IO finished.\n");
        pthread_mutex_unlock(handle_mutex + cur_fd);
    }
}

//acceptor：监听IO请求
void acceptor_run()
{
    struct epoll_event ev, events[FD_SIZE];
    epollfd = epoll_create(FD_SIZE);
    add_event(listenfd, EPOLLIN | EPOLLET);
    while(1)
    {
        int retfds = epoll_wait(epollfd, events, FD_SIZE, -1);
        if(retfds <= 0) myperror("epoll_wait");
        for(int i = 0; i < retfds; i++)
        {
            int cur_fd = events[i].data.fd;
            //如果是监听fd，加入到epoll
            if(cur_fd == listenfd && (events[i].events & EPOLLIN))
            {
                int acceptfd;
                while((acceptfd = accept(listenfd, NULL, NULL)) > 0)
                {
                    add_event(acceptfd, EPOLLIN | EPOLLET);
                    //printf("new client connected, fd = %d.\n", acceptfd);
                }
                if(acceptfd < 0 && errno != EWOULDBLOCK) myperror("accept");
            }
            //如果是其他fd，push_task，交给handler处理
            else if(events[i].events & EPOLLIN) push_task(cur_fd);
        }
    }
}

//主线程：acceptor，采用epoll机制
int main()
{
    pthread_mutex_init(&taskpool.mutex, NULL);
    pthread_cond_init(&taskpool.notempty, NULL);

    for(int i = 0; i < FD_SIZE + 100; i++)
        pthread_mutex_init(handle_mutex + i, NULL);
    socket_bind();
    taskpool_init();
    //printf("ready.\n");
    acceptor_run();
    pthread_cond_destroy(&taskpool.notempty);
    pthread_mutex_destroy(&taskpool.mutex);
    for(int i = 0; i < FD_SIZE + 100; i++)
        pthread_mutex_destroy(handle_mutex + i);
    return 0;
}

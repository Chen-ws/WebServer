#include<sys/socket.h>
#include<netinet/in.h>
#include<arpa/inet.h>
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<fcntl.h>
#include<stdlib.h>
#include<cassert>
#include<sys/epoll.h>

#include "./lock/locker.h"
#include "./threadpool/threadpool.h"
#include "./timer/lst_timer.h"
#include "./http/http_conn.h"
#include "./log/log,h"
#include "./CGImysql/sql_connection_pool.h"

#define MAX_FD 65535 //最大文件描述符
#define MAX_EVENT_NUMBER 10000 //最大事件数
#define TIMESLOT 5 //最小超时单位

#define SYMLOG //同步写日志
//#define ASYNLOG //异步写日志

//#define listenfdET //边缘触发阻塞
#define listenfdLT //水平触发阻塞

//这三个函数在http_conn.cpp中定义，改变链接属性


//设置定时器相关参数
static int pipefd[2];
static sort_timer_lst timer_lst; //待补充
static int epllfd = 0;

void sig_handler(int sig){
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1],(char*)&msg,1,0);
    errno = save_errno;
}

//设置信号函数
void addsig(int sig,void (handler)(int), bool restart = true){
    struct sigaction sa;
    memset(&sa,'\0',sizeof(sa));
    sa.sa_handler = handler;
    if(restart){
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);//在信号集设置所有信号，屏蔽所有信号
    assert(sigaction(sig,&sa,NULL) != -1);//捕捉sig信号，该信号的处理方式设置为sa
}

//定时处理任务，重新定时以不断触发SIGALRM信号
void time_handler(){
    timer_lst.tick();
    alarm(TIMESLOT);
}
//定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
void cb_func(client_data *user_data){//
    epoll_ctl(epollfd,EPOLL_CTL_DEL,user_data->sockfd,0);
    assert(user_data);
    close(user_data -> sockfd);
    http::m_user_count--;
    LOG_INFO("close fd %d",user_data -> data);
    Log::get_instance() -> flush();
}

void show_error(int connfd, const char *info){
    printf("%s",info);
    send(connfd,info,strlen(info),0);
    close(connfd);
}

int main(int argc,char *argv[]){
#ifdef ASYNLOG
    Log:;get_instance() -> init("ServerLog",2000,800000,8);//异步日志模型
#endif

#ifdef SYNLOG
    Log::get_instance() -> init("ServerLog",2000,800000,0);//同步日志模型
#endif

    if(argc <= 1){
        printf("usage:%s ip_address port_number\n",basename(argv[0]));//basename()获取文件路径最后的文件名
        return 1;
    }
    int port = atoi(argv[1]);

    addsig(SIGPIPE,SIG_IGN);//往读端被关闭的管道或者socket连接中写数据触发的SIGPIPE信号 忽略

    //创建数据库连接池
    conection_pool *connPool = connection_pool::GetInstance();
    connPool->init("localhost","root","root","db",3306,8);
    //创建连接池
    threadpool<http_conn> *pool = NULL;
    try{
        pool = new threadpool<http_conn>(connPool);//创建一个线程池，里面默认创建8个工作线程，等待处理的请求数量默认为10000
    }
    catch(...)
    {
        return 1;
    }
    http_conn *users = new http_conn[MAX_FD];//创建65535个连接实例，文件描述的数量
    assert(users);
    //初始化数据库读取表，从数据库连接池中获取一个连接，从数据库中查询数据
    users->initmysql_result(connPool);

    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    assert(listenfd >= 0);


    //struct linger tmp = {1,0};
    //SO_LINGER若有数据待发送，延迟关闭
    //setsockopt(listenfd,SOL_SOCKET,SO_LINGER,&tmp,sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzreo(&address,sizeof(address));
    address.sin_family = AF_INET; 
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    int flag = 1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&flag,sizeof(flag));//设置socket文件描述符属性，SOL_SOCKET表述通用socket选项，与协议无关。SO_REUSEADDR表示重用本地地址 设置为flag
    ret = bind(listenfd,(struct sockaddr *)&address,sizeof(address));
    assert(ret >= 0);

    //创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5);
    assert(epollfd != -1);

    addfd(epollfd,listenfd,false);//http_conn中提供
    http_conn::m_epollfd = epollfd;//静态的文件描述符m_epollfd

    //创建管道
    ret = socketpair(PF_UNIX,SOCK_STREAM,0,pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd,pipifd[0],false);
     
    addsig(SIGALRM,sig_handler,false);//由alarm或setitimer设置的实时闹钟超时引起
    addsig(SIGTERM,sig_handler,false);//终止进程。kill命令默认发送的信号就是SIGTERM
    bool stop_server = false;

    client_data *users_timer = new client_data[MAX_FD];//对每个连接创建定时器的结构

    bool timeout = false;
    alarm(TIMESLOT);//在<unistd.h>这个 文件中

    while(!stop_server){

        int number = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if(number < 0 && errno != EINTR){
            LOG_ERROR("%s","epoll failure");
            break;
        }
        for(int i = 0;i < number;i++){
            int sockfd = events[i].data.fd;
        
            //处理新到的客户端链接
            if(sockfd == listenfd){
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
#ifdef listenfdLT
                int connfd = accept(listenfd,(struct sockaddr *)&client_address,&client_addrlength);
                if(connfd < 0){
                    LOG_ERROR("%s:errno is : %d","accept error",errno);
                    continue;
                }
                if(http::m_user_count >= MAX_FD){
                    show_error(connfd,"Internal server busy");//如果文件描述数量大于最大值，需要打印错误原因，并发送给想要的connfd
                    LOG_ERROR("%s","Internal server busy");
                    continue;
                }
                //users是http_conn类型，表示一个用户连接
                users[connfd].init(connfd,client_address);//创建一个新的连接,http_conn类型的指针，将该connfd加入到epollfd内核事件表中

                //初始化client_data数据
                //创建定时器，设置回调函数和超时时间，绑定用户，将定时器添加到聊表中
                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                util_timer *timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users_timer[connfd].timer = timer;
                timer_lst.add_timer(timer);
#endif

#ifdef listenfdEL
                while(1){
                    //这段代码不会被重复触发，所以需要循环读取
                    int connfd = accept(listenfd,(struct sockaddr *)&client_address,&client_addrlength);
                    if(connfd < 0){
                        LOG_ERROR("%s:errno id :%d","accept error",errno);
                        break;
                    }
                    if(http_conn::m_user_count > MAX_fd){
                        show_error(connfd,"Internal server busy");
                        LOG_ERROR("%s","Internal server busy");
                        break;
                    }
                    users[connfd].init(connfd, client_address);

                    //初始化client_data数据
                    //创建定时器，设置毁掉函数和超时时间，绑定用户数据，将定时器添加到链表中
                    user_timer[connfd].address = client_address;
                    user_timer[connfd].sockfd  = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &user_timer[connfd];
                    time_t cur = time(NULL);
                    timer -> expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
#endif
            }
            /*EPOLLRDHUP表示读关闭，1、对端发送 FIN (对端调用close 或者 shutdown(SHUT_WR)).
            2.本端调用 shutdown(SHUT_RD). 当然，关闭 SHUT_RD 的场景很少。
            EPOLLHUP表示读写都关闭，1、本端调用shutdown(SHUT_RDWR)。 不能是close，close 之后，文件描述符已经失效
            2、本端调用 shutdown(SHUT_WR)，对端调用 shutdown(SHUT_WR)。
            EPOLLERR表示错误
            */
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //服务器端关闭连接，移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                //定时器回调函数，删除非活动连接在socket上的注册事件，并关闭
                timer->cb_func(&users_timer[sockfd]);

                if (timer)
                {
                    timer_lst.del_timer(timer);
                }
            }
            //处理信号
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)){
                int sig;
                char signals[1024];
                ret = recv(pipe[0],signals,sizefo(signals),0);
                if(ret == -1){
                    continue;
                }else if(ret == 0){
                    continue;
                }else{
                    for(int i = 0; i < ret;++i){
                        switch(signals[i]){
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
            //处理客户端连接上接受到的数据
             else if(events[i].events & EPOLLIN){
                util_timer *timer = users_timer[sockfd].timer;
                if(users[sockfd].read_once()){//如果能读出数据，主线程检测读事件，调用read_once()函数
                    LOG_INFO("deal with the client(%s)",inet_ntoa(users[sockfd].get_address() -> sin_addr);
                    Log::get_instance() -> flush();
                    //监听到读事件，将该事件放入到请求队列，请求队列的m_queuestat信号量执行V操作，通知工作线程
                    pool -> append(users + sockfd);//pool为http连接池

                    //若有数据传输，则将定时器往后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if(timer){
                        time_t cur = time(NULL);
                        timer -> expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s","adjust timer once");
                        Log::instance() -> flush();
                        timer_lst.adjust_timer(timer);
                    }
                }else{
                    
                    timer -> cb_func(&users_timer[sockfd]);
                    if(timer){
                        timer_lst.del_timer(timer);
                    }
                }
            }
            //处理需要输出的数据
            else if(events[i].events & EPOLLOUT){
                util_timer *timer = user_timer[sockfd].timer;
                if(user[sockfd].write()){//如果写的数据大小不为0
                    LOG_INFO("send data to the client(%s)",inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::instance()->flush();
                    //若有数据传输，则将定时器向后延迟3个单位
                    //并对新的定时器在链表上的位置进行调整
                    if(timer){
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        LOG_INFO("%s","adjust timer once");
                        Log::get_instance() -> flush();
                        timer_lst.adjust_timer(timer);
                    }
                }else{
                    timer -> cb_func(&users_timer[sockfd]);
                    if(timer){
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if(timeout){
            timer_handler();
            timeout = false;
        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;


}
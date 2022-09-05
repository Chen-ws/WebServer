#include"http_conn.h"
#include"../log/log.h"
#include<map>
#include<mysql/mysql.h>
#include<fstream>

//#define connfdET //边缘出发非阻塞
#define connfdLT //水平触发阻塞

//#define listenfdET //边缘触发非阻塞
#define listenfdLT //水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossile to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found.";
const char *error_404_form = "The request file was not found on this server.\n";
const char *error_500_title = "Internal Error.";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/qgy/github/TinyWebServer/root";//后期可修改。。。。。。。。。。。。。

//将标中的用户名和密码放入map
map<string,string> users;
locker m_lock;

http_conn(){}
~http_conn(){}

//从数据库连接池中获取一个可用的连接，然后查询数据库，获得库中所有的用户名和密码
void http_conn::initmysql_result(connection_pool *connPool){
    //先从数据库连接池中获取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql,connPool);

    //在user表中检索username，passwd数据，**浏览器端输入??应该是吧所有的用户名和密码结果筛出来
    if(musql_query(mysql,"SELECT username,passwd FROM user")){
        LOG_ERROR("SELECT error:%s\n",mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回所有结果集中的列数
    int num_fields = mysql_num_fields(result);//nums_fieldes的作用是什么

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);//fields的作用是什么

    //从结果集中获取下一行，将对应的用户名和密码存入map中
    while(MYSQL_ROW row = mysql_fetch_row(result)){
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd){
    int old_option = fcntl(fd,F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd,F_SETFL,new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT，EPOLLONESHOT是设置一个事件上的数据必须有一个线程来处理
//对于注册了EPOLLONESHOT事件的文件描述符，操作系统最多出发其上注册的一个可读、可写或者异常事件，
//且最多触发一次，除非使用epoll_ctl函数重置该文件描述符上的注册的EPOLLONESHOT事件。
void addfd(int epollfd ,int fd,bool one_shot){
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = EPOLLIN | EPOLLLT | EPOLLRDHUP;
#endif

#ifdef listenfdET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef listenfdLT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if(one_shot){
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&event);
    setnonblocking(fd);
}

//从内核事件表中删除描述符
void removefd(int epollfd,int fd){
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,0);
    close(fd);
}

//将事件重置EPOLLONESHOT
void modfd(int epollfd ,int fd,int ev){
    epoll_event event;
    event.data.fd = fd;

#ifdef connfdET
    event.events = ev | EPOLLET |EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef connfdLT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&event);
}

int http_conn::m_user_count = 0;//客户数量
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户量总量减一
int http_conn::close_conn(bool real_close){
    if(real_close && (m_sockfd != -1)){
        removefd(m_epollfd,m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接，外部调用初始化套接字地址
void http_conn::init(int sockfd,const sockaddr_in &addr){
    m_sockfd  = sockfd;
    m_address = addr;
    //以下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应该去掉
    //int reuse = 1;
    //setsockopt(m_sockfd,SQL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd,sockfd,true);
    m_user_count++;
    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init(){
    mysql = NULL;//数据库连接
    bytes_to_send = 0;//已发送的字节数
    bytes_have_send = 0;//待发送的字节数
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;/
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;/
    cgi = 0;//cgi标志
    memset(m_read_buf,'\0',READ_BUFFER_SIZE);//读缓冲区
    memset(m_write_buf,'\0',WRITE_BUFFER_SIZE);//写缓冲区
    memset(m_real_file,'\0',FILENAME_LEN);//客户请求的目标文件的完整路径，其内容等于doc_root + m_url,doc_root是网站根目录
}

//从状态机，用于分析出一行内容，主要根据回车符和换行符，\r\n即为一行读取完毕，从m_read_buf中读取
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for(; m_checked_idx < m_read_idx;++m_checked_idx){
        //获取当前要分析的字节
        temp = m_read_buf[m_checked_idx];
        //如果当前的字节是“\r“，即回车符，说明可能读取到一个完整的行
        if(temp == '\r'){
            /*如果”\r“字符碰巧是目前buffer中的最后一个已经被读入的客户数据，那么这次分析没有
            读取到一个完整的行，返回LINE_OPEN以表示还需要继续读取客户数据才能进一步分析*/
            if((m_checked_idx + 1) == m_read_idx){
                return LINE_OPEN;
            }
            //如果下一个字符是“\n“，则说明成功读取到一个完整的行
            else if(m_read_buf[m_checked_idx + 1] == '\n'){
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //否则说明客户发送的HTTP请求存在语法错误
            return LINE_BAD;
        }
        //如果当前的字节是”\n“，即换行符，则也说明可能读取到一个完整的行
        else if(temp == '\n'){
            if(m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r'){
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //如果分析完所有内容也没有遇到”\r“字符，则返回LINE_OPEN,表示还需要继续读取客户数据
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或者对方关闭连接，读到m_read_buf中
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once(){
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read = 0;
#ifdef connfdLT
    //只要socket读缓存中还有未读出的数据，这段代码就被触发
    bytes_read = recv(m_sockfd,m_read_buf + m_read_idx,READ_BUFFER_SIZE - m_read_idx,0);
    m_read_idx += bytes_read;
    if(bytes_read <= 0){
        return false;
    }
    return true;

#endif

#ifdef connfdET 
    //while中的代码不会被重复触发，需要循环读取数据，以确保把socket存中的读缓所有数据读出
    while(true){
        bytes_read = recv(m_sockgfd,m_read_buf + m_read_idx,READ_BUFFER_SIZE - m_read_idx,0);
        if(bytes_read == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                break;
            }
            return false;
        }else if(bytes_read == 0){
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
#endif
}

//解析http请求行，获得请求方法，目标url和http版本号
http::HTTP_CODE http_conn::parse_request_line(char *text)
{   //“\t“为水平制表符
    //请求行中最先含有空格和\t任一字符的位置并返回
    m_url = strpbrk(text," \t");//char *strpbrk(char *s1,char *s2)函数的作用是检索两个字符串中首个相同字符的位置**指针**
    //如果请求行中没有空白字符或”\t“字符，则HTTP请求格式必有问题
    if(!m_url)
    {
        return BAD_REQUEST;
    }
    //用于将前面的数据取出
    *m_url++ = '\0';

    //取出数据，确定请求方式
    char *method = text;
    if(strcasecmp(method,"GET") == 0)//int strcasecmp (const char *s1, const char *s2);忽略大小写比较两个字符串的。若相等返回0。
    {
        m_method = GET;
    }
    else if(strcasecmp(method,"POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
    {
        reutrn BAD_REQUEST;
    }

    //m_url此时跳过了第一个空格或者\t字符，但是后面还可能存在
    //不断后移找到请求资源的第一个字符
    //size_t strspn (const char *s,const char * accept);返回值 返回字符串s开头连续包含字符串accept内的字符数目。
    m_url += strspn(m_url,"\t");//m_url指针向后移动，跳过\t
    //判断http的版本号
    m_version = strpbrk(m_url,"\t");
    if(!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version,"\t");
    //仅支持http1.1
    if(strcasecmp(m_version,"HTTP/1.1") != 0 )
    {
        return BAD_REQUEST;
    }
    //对请求资源的前7个字符进行判断
    //对某些带有http://的报文进行单独处理
    if(strncasecmp(m_url,"http://",7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url,'/');//char *strchr(const char *str, int c)，即在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
    }
    //https的情况
    if(strncasecmp(m_url,"https://",8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url,'/');
    }
    //不符合规则的报文
    if(!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    //当url为/时，显示欢迎（判断）界面
    if(strlen(m_url) == 1)
    {   //extern char *strcat(char *dest, const char *src);
        //把src所指向的字符串（包括“\0”）复制到dest所指向的字符串后面（删除*dest原来末尾的“\0”）。
        //要保证*dest足够长，以容纳被复制进来的*src。*src中原有的字符不变。返回指向dest的指针。
        strcat(m_url,"judge.html");
    }
    //主状态机状态转移
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;

}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{   
    //判断是空行还是请求头
    if(text[0] == '\0')
    {   
        //post请求需要改变主状态机的状态
        if(m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    //解析Connection头部连接字段
    else if(strncasecmp(text,"Connection:",11) == 0)
    {
        text += 11;
        //跳过空格和\t字符
        text += strspn(text," \t");
        if(strncasecmp(text,"keep-alive") == 0)
        {
            m_linger = true;//保持连接
        }
    }
    //处理content_length头部字段
    else if(strncasecmp(text,"Content-length:",15) == 0)
    {
        //指针text向后移动，跳过“Content-length:”这部分，再跳过\t
        text += 15;
        text += strspn(text,"\t");
        m_content_length = atol(text);
    }
    //处理Host字段
    else if(strncasecmp(text,"Host:",5) == 0)
    {
        text += 5;
        text += strspn(text,"\t");
        m_host = text;
    }
    else{
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header:%s",text);
        Log::get_instance() -> flush();
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入，未真正解析HTTP请求的消息体
http_conn::HTTP_CODE http_conn::parse_connect(char *text)
{
    //判断是否读取了消息体
    if(m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后输入的用户名和密码
        m_string = text;//m_string为请求头数据
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//通过while循环，封装主状态机对每一行进行循环处理
//此时 从状态机已经修改完毕 主状态机可以取出完整的行进行解析
//主状态机。分析HTTP请求的入口函数
http_conn::HTTP_CODE http_conn::process_read()
{
    //初始化从状态机的状态
    LINE_STATUS line_status = LINE_OK;
    //记录HTTP请求的处理结果
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;
    //主状态机，用于从buffer中取出所有完整的行
    //判断条件，这里就是从状态机驱动主状态机
    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        //m_start_line 是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中的读取位置
        m_start_line = m_checked_idx;//m_start_line是行在buffer中的起始位置。记录下一行的位置
        LOG_INFO("%s",text);
        Log::get_instance() -> flush();
        //三种状态转换逻辑
        switch(m_check_state)
        {
        case CHECK_STATE_REQUESTLINE://第一个状态，分析请求行
        {
            //解析请求行
            ret = parse_request_line(text);
            if(ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER://第二个状态，分析头部字段
        {   
            //解析请求头
            ret = parse_headers(text);
            if(ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            //作为get请求，则需跳转到报文响应函数
            else if(ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT://第三个状态，分析请求体
        {
            //解析消息体
            ret = parse_content(text);
            //对于post请求 跳转到报文响应函数
            if(ret == GET_REQUEST)
            {
                return do_request();
            }
            //更新 跳出循环 代表解析完了消息体
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

//当得到一个完整、正确的HTTP请求时，就分析目标文件的属性。如果目标文件存在、对所有用户可读，且不是目录
//则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功。
http_conn::HTTP_CODE http_conn::do_request()
{
    //将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file,doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n",m_url);
    //char *strrchr(const char *str, int c);查找一个字符c在另一个字符串str中末次
    //出现的位置（也就是从str的右侧开始查找字符c首次出现的位置），并返回这个位置的地址。
    //如果未能找到指定字符，那么函数将返回NULL。
    //找到m_url中/的位置
    const char *p = strrchr(m_url,'/');

    //处理cgi
    //实现登录和注册校验
    if(cgi == 1 && *(p + 1) == '2' || *(p + 1) == '3')
    {

        //处理标志判断是登陆还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real,"/");
        strcat(m_url_real,m_url + 2);
        strncpy(m_real_file + len,m_url_real,FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100],password[100];
        int i ;
        for(int i = 5;m_string[i] != '&';++i)
        {
            name[i - 5] = m_string[i];
        }
        name[i - 5] = '\0';

        int j = 0;
        for(i = i + 10;m_string[i] != '\0';++i,++j)
        {
            password[j] = m_string[i];
        }
        password[j] = '\0';

        //同步线程登陆校验
        if(*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名，没有重名的，进行插入数据，注册
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert,"INSERT INFO user(username,passwd) VALUES(");
            strcat(sql_insert,"'");
            strcat(sql_insert,name);
            strcat(sql_insert,"','");
            strcat(sql_insert,password);
            strcat(sql_insert,"')");
            //如果数据库中没有重复用户名，则插入用户名和密码
            if(users.find(name) == user.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql,sql_insert);
                users.insert(pair<string,string>(name,password));
                m_lock.unlock();
                if(!res)
                {
                    strcpy(m_url,"/log.html");
                }
                else{
                    strcpy(m_url,"registerError.html");
                }
            }
            //否则将m_url修改为注册失败的页面
            else
            {
                strcpy(m_url,"/registerError.html");
            }
        }
        //如果是登陆，直接判断，若浏览器端输入的用户名和密码再表中可以查到，返回1，否则返回0
        else if(*(p + 1) == '2')
        {
            if(users.find(name) != users.end() && users[name] == password)
            {
                strcpy(m_url,"/welcome.html");
            }
            else{
                strcpy(m_url,"/logError.html");
            }
        }
    }//ifcgi

    //如果请求资源为/0，表示跳转注册界面
    if(*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real,"/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len,m_url_real,strlen(m_url_real));
        free(m_url_real);
    }
    else if(*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) *200);
        strcpy(m_url_real,"/log.html");
        strncpy(m_real_file + len,m_url_real,strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/1,表示跳转登录界面
    else if(*(p + 1 ) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char)* 200);
        strcpy(m_url_real,"/picture.html");
        strncpy(m_real_file + len, m_url_real,strlen(m_url_real));

        free(m_url_real);
    }
    else if(*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char)* 200);
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strcpy(m_url_file + len,m_url_real,strlen(m_url_real));

        free(m_real_url);
    }
    else if(*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real,"/fans.html");
        strncpy(m_real_file + len,m_url_real,strlen(m_url_real));
        
        free(m_url_real);
    }
    else
    {
        strncpy(m_real_file + len,m_url,FILENAME_LEN - len - 1);
    }
    /*int stat(const char *path, struct stat *buf);
    通过path获取文件信息，并保存在buf所指的结构体stat中
     执行成功则返回0，失败返回-1，错误代码存于errno
    */
    if(stat(m_real_file,&m_file_stat) < 0)//文件不存在
        return NO_RESOURCE;
    if(!(m_file_stat.st_mode & S_IROTH))//st_mode表示文件对应的模式，文件，目录等。S_IROTH表示其他用户具有可读权限
        return FORBIDDEN_REQUEST;//客户对资源的没有足够的访问权限
    if(S_ISDIR(m_file_stat.st_mode))//访问的是目录
        return BAD_REQUEST;
    int fd = open(m_real_stat,O_RDONLY);
    //将客户请求的目标文件mmap到内存中，用m_file_address指向起始位置
    m_file_address = (char *)mmap(0,m_file_stat.st_size,PROT_READ,MAP_PRIVATE,fd,0);
    close(fd);
    return FILE_REQUEST;  
}

//对内存映射去执行munmap操作，也就是释放m_file_address所指向的内存区域（目标文件在缓存中所映射的区域）
void http_conn::unmap()
{
    if(m_file_address)
    {
        /*int munmap(void *start,size_t length)；
        munmap()用来取消参数start所指的映射内存起始地址，参数length则是欲取消的内存大小。
        当进程结束或利用exec相关函数来执行其他程序时，映射内存会自动解除，但关闭对应的文件
        描述符时不会解除映射。*/
        munmap(m_file_address,m_file_stat.st_size);
        m_file_address = 0;
    }
}

//写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    //如果待发送的数据都发完了，则将sockfd置为等待读取数据
    if(bytes_to_send == 0)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        init();
        return true;
    }

    while(1)
    {
        //ssize_t writev( int fd, const struct iovec *iov, int cnt );
        //writev将多个数据存储在一起，将驻留在两个或更多的不连接的缓冲区中的数据一次写出去。
        //返回值：传输字节数，出错时返回-1.
        temp = writev(m_sockfd,m_iv,m_iv_count);
        if(temp < 0)
        {
            //如果TCP写缓冲区没有空间，则等待下一轮EPOLLOUT事件。虽然在此期间，服务器无法立即
            //接受到同一客户的下一个请求，但这可以保证连接的完整性。
            if(errno == EAGAIN)
            {
                modfd(m_epollfd,m_sockfd,EPOLLOUT);
                return true;
            }
            unmap();//释放内存区域
            return false;
        }
        bytes_have_send += temp;
        bytes_to_send -= temp;
        //如果已经发送的字节数大于m_iv[0]这个缓冲区的长度，也就是说m_iv[0]的内容都发送完了
        if(bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        //若已经没有等待发送的数据，则释放m_file_address所指向的内存区域，修改文件描述符属性
        if(bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd,m_sockfd,EPOLLIN);

            if(m_linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}

//往写缓冲区写入待发送的数据
bool http_conn::add_response(const char* format,...)
{
    if(m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    //VA_LIST 是在C语言中解决变参问题的一组宏，
    //所在头文件：#include <stdarg.h>，用于获取不确定个数的参数。
    va_list arg_list;
    /*va_start宏，获取可变参数列表的第一个参数的地址（list是类型为va_list的指针，param1是可变参数最左边的参数）：
      #define va_start(list,param1) (list = (va_list)param1 + sizeof(param1) )
    */
    va_start(arg_list,format);
    //vsnprintf函数将可变参数格式化输出到一个字符数组
    //返回值：执行成功，返回最终生成字符串的长度，若生成字符串的长度大于size，则将字符串的
    //前size个字符复制到str，同时将原串的长度返回（不包含终止符）；执行失败，返回负值，并置errno.
    int len = vsnprintf(m_wirte_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx,format,arg_list);
    //如果可变参数生成字符串的长度大于写缓冲区的剩余空闲长度，则情况可变参数列表，返回false
    if(len >= (WRITE_BUFFER_SIZE - 1 - m_wirte_idx)
    {
        //va_end宏，清空va_list可变参数列表：
        //#define va_end(list) ( list = (va_list)0 )
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s",m_write_buf);
    Log::get_instance() -> flush();
    return true;
}

bool http_conn::add_status_line(int status,const char *title)
{
    return add_response("%s %d %s\r\n","HTTP/1.1",status,title);
}

bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len)
{
    return add_response("Content_Length:%d\r\n",content_len);
}

bool http_conn::add_content_type()
{
    return add_response("Content_Type:%s\r\n","text/html");
}

bool http_conn::add_linger()
{
    return add_response("Contention:%s\r\n",(m_linger == true) ? "keep_alive" : "close");

}

bool http_conn::add_blank_line()
{
    return add_response("%s","\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s",content);
}

//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500,error_500_title);
        add_headers(strlen(error_500_form));
        if(!add_content(errpr_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404,error_404_title);
        add_headers(strlrn(error_404_form));
        if(!add_content(error_404_form))
        {
            return false;
        }
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403,error_403_title);
        add_headers(strlen(error_403_form));
        if(!add_content(error_403_form))
        {
            return false;
        }
        break;
    }
    case FILE_REQUEST:
    {
        adds_status_line(200,ok_200_title);
        if(m_file_stat.st_size != 0)
        {
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            cosnt char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if(!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_wirte_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//由线程池中的工作线程调用，这是处理HTTP请求的入口函数
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST)
    {
        modfd(m_epollfd,m_sockfd,EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if(!write_ret)
    {
        close_conn();
    }

    modfd(m_epollfd,m_sockfd,EPOLLOUT);
}
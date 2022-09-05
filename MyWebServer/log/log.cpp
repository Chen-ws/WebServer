#include<string.h>
#include<time.h>
#include<sys/time.h>
#include<stdarg.h>
#include"log.h"
#include<pthread.h>

using namespace std;
Log::Log()
{
    m_count = 0;
    m_is_async = false;
}

Log::~Log()
{
    if(m_fp != NULL)
    {
        fclose(m_fp);
    }
}
//异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name,int log_buf_size,int split_lines,int max_queue_size)
{
    //如果设置了max_queue_size,则设置为异步
    if(max_queue_size >= 1)
    {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        //flush_log_thread为回调函数，这里表示创建线程异步写日志
        pthread_create(&tid,NULL,flush_log_thread,NULL);
    }

    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf,'\0',m_log_buf_size);
    m_split_lines = split_lines;
    
    //time_t time(time_t *timer)；获得机器的日历时间或者设置日历时间，
    //timer=NULL时，获得机器日历时间；time_t(time_t是一个long类型)
    time_t t = time(NULL);
    //struct tm *localtime(const time_t *timer)；返回一个以tm结构表达的机器时间信息
    //timer：使用time()函数得到的机器时间。tm的结构可以查到
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm ;

    //查找字符在指定字符串中从右面开始的第一次出现的位置，
    //如果成功，返回该字符以及其后面的字符，如果失败，则返回 NULL。
    const char *p = strrchr(file_name,'/');
    char log_full_name[256];
    
    //如果file_name中没有‘/’，则按照指定的file_name拼接路径名+日志文件名
    if(p == NULL)
    {
        //函数原型为int snprintf(char *str, size_t size, const char *format, ...)。
        //将可变参数 “…” 按照format的格式格式化为字符串，然后再将其拷贝至str中。
        //若成功则返回预写入的字符串长度，若出错则返回负值。
        //与snprintf的返回值不同，sprintf的返回值是成功写入的字符串长度，此处需要谨慎处理。
        snprintf(log_full_name,255,"%d_%02d_%02d_%s",my_tm.tm_year + 1900,my_tm.tm_mon + 1,my_tm.tm_mday,file_name);

    }
    //如果有‘/’，则按照dir_name+时间+log_name拼接log_full_name
    else
    {
        strcpy(log_name,p + 1);
        strncpy(dir_name,file_name,p - file_name + 1);
        snprintf(log_full_name,255,"%s%d_%02d_%02d_%s",dir_name,my_tm.tm_year + 1900,my_tm.tm_mon + 1,my_tm.tm_mday,log_name);
    }
    m_today = my_tm.tm_mday;

    m_fp = fopen(log_full_name,"a");
    if(m_fp == NULL)
    {
        return false;
    }
    return true;
}

void Log::write_log(int level,const char *format,...)
{
    struct timeval now = {0,0};
    //int gettimeofday(struct timeval*tv, struct timezone *tz);
    //使用C语言编写程序需要获得当前精确时间（1970年1月1日到现在的时间），或者为执行计时，可以使用gettimeofday()函数。
    //其参数tv是保存获取时间结果的结构体，参数tz用于保存时区结果
    //函数执行成功后返回0，失败后返回-1，错误代码存于errno中。
    gettimeofday(&now,NULL);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    char s[16] = {0};
    switch(level)
    {
    case 0 :
        strcpy(s,"[debug]:");
        break;
    case 1:
        strcpy(s,"[info]:");
        break;
    case 2:
        strcpy(s,"[warn]:");
        break;
    case 3:
        strcpy(s,"[error]:");
        break;
    default:
        strcpy(s,"[info]:");
        break;
    }
    //写入一个log,对m_count++,m_split_lines最大行数
    m_mutex.lock();
    m_count++;//日志行数++

    //如果不是当天的日志文件，或者当前日志文件行数满了，则需要创建新的日志文件
    if(m_today != my_tm.tm_mday || m_count % m_split_lines == 0 )
    {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};

        snprintf(tail,16,"%d_%02d_%02d_",my_tm.tm_year + 1900,my_tm.tm_mon + 1,my_tm.tm_mday);

        if(m_today != my_tm.tm_mday)
        {
            snprintf(new_log,255,"%s%s%s",dir_name,tail,log_name);
            m_today = my_tm.tm_mday;
            m_count = 0;
        }
        else
        {
            snprintf(new_log,255,"%s%s%s.%lld",dir_name,tail,log_name,m_count / m_split_lines);
        }
        m_fp = fopen(new_log,"a");
    }

    m_mutex.unlock();

    va_list valst;
    //获取可变参数列表的第一个参数的地址,http_conn.cpp中有该函数的解释
    va_start(valst,format);

    string  log_str;
    m_mutex.lock();

    //写入的具体时间内容格式。snprintf是把日志文件中每一行开头的时间信息，日志类型写到m_buf中
    int n = snprintf(m_buf,48,"%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                    my_tm.tm_year + 1900,my_tm.tm_mon + 1,my_tm.tm_mday,
                    my_tm.tm_hour,my_tm.tm_min,my_tm.tm_sec,now.tv_usec,s);
    //_vsnprintf是C语言库函数之一，属于可变参数。用于向字符串中打印数据、数据格式用户自定义。头文件是#include <stdarg.h>。
    //将可变参数格式化输出到一个字符数组。
    //vsnprintf是把日志的具体信息追加到开头内容之后
    int m = vsnprintf(m_buf + n,m_log_buf_size - 1,format,valst);
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\n';
    log_str = m_buf;

    m_mutex.unlock();

    //如果是异步，且日志队列没满，将log_str日志信息加入到日志队列中
    if(m_is_async && !m_log_queue -> full())
    {
        m_log_queue->push(log_str);
    }
    else
    {
        m_mutex.lock();
        //否则直接把日志信息写到m_fp所指向的日志文件中
        fputs(log_str.c_str(),m_fp);
        m_mutex.unlock();
    }
    va_end(valst);
}

void Log::flush(void)
{
        m_mutex.lock();
        //强制刷新写入流缓冲区
        fflush(m_fp);
        m_mutex.unlock();
}
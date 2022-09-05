#ifndef _CONNECTION_POOL_
#define _CONNECTION_POOL_
#include<stdio.h>
#include<list>
#include<mysql/mysql.h>
#include<error.h>
#include<string.h>
#include<iostream>

#include "../lock/locker.h"


using namespace std;
class connection_pool{
public:
    MYSQL *GetConnection();             //获取数据库连接
    bool ReleaseConnection(MYSQL *conn);//释放连接
    int GETFreeConn();                  //获取链接
    void DestroyPool():                 //销毁所有连接

    //单例模式
    static connection_pool *GetInstance();

    void init(string url,string User,string PassWord,string DataBaseName,int Port,unsigned int MaxConn);

    connection_pool();
    ~connection_pool();

private:
    unsigned int MaxConn;       //最大连接数
    unsigned int CurConn;       //当前已使用的连接数
    unsigned int FreeConn;      //当前的空闲连接数

private:
    locker lock;
    list<MYSQL *> connList;//连接池
    sem reserve;//信号量reserve

private:
    string url;         //主机地址
    string Port;        //数据库端口号
    string User;        //登录数据库用户名
    string PassWord;    //登录数据库密码
    string DatabaseName;//使用数据库名
}

class connectionRAII{

public:
    connectionRAII(MYSQL **conn, connection_pool *connPoll);
    ~connectionRAII();
private:
    MYSQL *conRAII;
    connection_pool *poolRAII;
}
/*
RAII（Resource Acquisition Is Initialization）,也称为“资源获取就是初始化”，
是C++语言的一种管理资源、避免泄漏的惯用法。C++标准保证任何情况下，已构造的对象最终会销毁，
即它的析构函数最终会被调用。简单的说，RAII 的做法是使用一个对象，在其构造时获取资源，在对
象生命期控制对资源的访问使之始终保持有效，最后在对象析构的时候释放资源。
*/

#endif
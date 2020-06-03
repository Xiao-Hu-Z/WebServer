#include <mysql/mysql.h>
#include <stdio.h>
#include <string>
#include <string.h>
#include <stdlib.h>
#include <list>
#include <pthread.h>
#include <iostream>
#include "sql_connection_pool.h"

using namespace std;

connection_pool::connection_pool()
{
	this->CurConn = 0;
	this->FreeConn = 0;
}

connection_pool *connection_pool::GetInstance()
{
	static connection_pool connPool;
	return &connPool;
}

//构造初始化
void connection_pool::init(string url, string User, string PassWord, string DBName, int Port, unsigned int MaxConn)
{
        //初始化数据库信息
	this->url = url;
	this->Port = Port;
	this->User = User;
	this->PassWord = PassWord;
	this->DatabaseName = DBName;

	lock.lock();
        //创建MaxConn条数据库连接,MaxConn:最大连接数
	for (int i = 0; i < MaxConn; i++)
	{
		MYSQL *con = NULL;
                /*  MySQL *mysql_init(MYSQL *mysql)；
                函数用来分配或者初始化一个MYSQL对象，用于连接mysql服务端。
                如果你传入的参数是NULL指针，它将自动为你分配一个MYSQL对象*/
		con = mysql_init(con);

		if (con == NULL)
		{
			cout << "Error:" << mysql_error(con);
			exit(1);
		}
                //mysql_real_connect()试图建立到运行host的一个MySQL数据库引擎的一个连接
                //string转为char
		con = mysql_real_connect(con, url.c_str(), User.c_str(), PassWord.c_str(), DBName.c_str(), Port, NULL, 0);

		if (con == NULL)
		{
			cout << "Error: " << mysql_error(con);
			exit(1);
		}
                //连接池connList
		connList.push_back(con);
                //更新连接池和空闲连接数量
		++FreeConn;
	}

        //将信号量初始化为数据库的连接总数
        //使用信号量实现多线程争夺连接的同步机制，这里将信号量初始化为数据库的连接总数*/
	reserve = sem(FreeConn);

	this->MaxConn = FreeConn;
	
	lock.unlock();
}


//当有请求时，从数据库连接池中返回一个可用连接，更新使用和空闲连接数
//多线程操作连接池，会造成竞争，这里使用互斥锁完成同步
/*当线程数量大于数据库连接数量时，使用信号量进行同步，每次取出连接，信号量原子减1，释放连接原子加1，若连接池内没有连接了，则阻塞等待。*/
MYSQL *connection_pool::GetConnection()
{
	MYSQL *con = NULL;

	if (0 == connList.size())
		return NULL;

        //取出连接，信号量原子减1，为0则等待
        //若连接池内没有连接了，为0，则阻塞等待
	reserve.wait();
	
	lock.lock();

	con = connList.front();
	connList.pop_front();

	--FreeConn;
        //CurConn:当前已使用的连接数
	++CurConn;

	lock.unlock();
	return con;
}

//释放当前使用的连接
bool connection_pool::ReleaseConnection(MYSQL *con)
{
	if (NULL == con)
		return false;

	lock.lock();

	connList.push_back(con);
	++FreeConn;
        //CurConn:当前已使用的连接数
	--CurConn;

	lock.unlock();
        //释放连接原子加1
	reserve.post();
	return true;
}

//销毁数据库连接池
//通过迭代器遍历连接池链表，关闭对应数据库连接，清空链表并重置空闲连接和现有连接数量
void connection_pool::DestroyPool()
{

	lock.lock();
	if (connList.size() > 0)
	{
		list<MYSQL *>::iterator it;
		for (it = connList.begin(); it != connList.end(); ++it)
		{
			MYSQL *con = *it;
			mysql_close(con);
		}
		CurConn = 0;
		FreeConn = 0;
                //清空list
		connList.clear();

		lock.unlock();
	}

	lock.unlock();
}

//当前空闲的连接数
int connection_pool::GetFreeConn()
{
	return this->FreeConn;
}

connection_pool::~connection_pool()
{
	DestroyPool();
}

/*销毁连接池没有直接被外部调用，将数据库连接的获取与释放通过RAII机制封装，避免手动释放，在connectionRAII类构造连接池对象，在析构函数自动释放*/
connectionRAII::connectionRAII(MYSQL **SQL, connection_pool *connPool){
	*SQL = connPool->GetConnection();//从连接池connList获取一个可用连接
	conRAII = *SQL;
	poolRAII = connPool;//连接池类的对象
}

connectionRAII::~connectionRAII(){
	poolRAII->ReleaseConnection(conRAII);//释放连接池资源
}

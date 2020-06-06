#include "http_conn.h"
#include "../log/log.h"
#include <map>
#include <mysql/mysql.h>
#include <fstream>

//同步校验
#define SYNSQL

////CGI多进程登录校验,用连接池
//#define CGISQLPOOL

//CGI多进程登录校验,不用连接池
//#define CGISQL

//#define ET       //边缘触发非阻塞
#define LT         //水平触发阻塞

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

//当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
const char *doc_root = "/home/xiaohu/文档/WebServer/root";

//将表中的用户名和密码放入map
map<string, string> users;

#ifdef SYNSQL

//同步数据库校验
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    //将查询的全部结果读取到客户端，分配1个MYSQL_RES结构，并将结果置于该结构中
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回结果集中代表字段（列）的对象的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中会返回下一个字符串数组的地址，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

#endif

#ifdef CGISQLPOOL
//CGI数据库校验
void http_conn::initresultFile(connection_pool *connPool)
{
    //写操作
    ofstream out("./CGImysql/id_passwd.txt");
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        out << temp1 << " " << temp2 << endl;
        users[temp1] = temp2;
    }

    out.close();
}

#endif

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

/*一个socket连接在任一时刻都只被一个线程处理，可使用epoll的EPOLLONESHOT事件
注册了EPOLLONESHOT事件的文件描述符，操作系统最多触发其上注册的一个可读、可写或者异常事件，且只触发一次
将fd上的EPOLLIN、EPOLLET和EPOLLRDHUP事件注册到epollfd指示的epoll内核时间表中
参数one_shot指定是否注册fd上的EPOLLONESHOT事件*/
void addfd(int epollfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef ET
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
#endif

#ifdef LT
    event.events = EPOLLIN | EPOLLRDHUP;
#endif

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从epollfd标识的epoll内核事件表中删除fd上的所有注册事件
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
//EPOLL_CTL_MOD：更改注册的文件描述符的关注事件发生情况
/*
EPOLLET：ET边沿触发，只能被触发一次
EPOLLONESHOT:只能被一个线程处理
EPOLLRDHUP：TCP连接被关闭，或者对方关闭了写操作，它由GUN引入
*/
void modfd(int epollfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;

#ifdef ET
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
#endif

#ifdef LT
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
#endif

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    //int reuse=1;
    //设置socket文件描述符的属性,SO_REUSEADDR:重置本地地址
    //setsockopt(m_sockfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    m_user_count++;
    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    //check_state默认为分析请求行状态
    m_check_state = CHECK_STATE_REQUESTLINE;
    //HTTP请求是否保持连接
    m_linger = false;
    //请求方法
    m_method = GET;
    //客户端请求的目标文件的文件名
    m_url = 0;
    //HTTP协议版本号，仅支持HTTP/1.1
    m_version = 0;
    //HTTP请求请求消息体的长度
    m_content_length = 0;
    //主机名
    m_host = 0;
    //当前正在解析的行的起始位置
    m_start_line = 0;
    //当前正在分析的字符在读缓冲区中的位置
    m_checked_idx = 0;
    //标识缓冲区已经读入的客户数据的最后一个字节的下一位置
    m_read_idx = 0;
    //写缓冲区待发送的字节数
    m_write_idx = 0;
    cgi = 0;//启用的POST，cgi=1,未启用POST,cgi=0
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);//读缓冲区
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);//写缓冲区
    //客户端请求的完整路径，其内容为doc_root+m_url,doc_root是网站根目录
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于解析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    //m_checked_idx：当前正在分析的字符在读缓冲区中的位置
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            //如果当前的字符是”\r"，即回车符，则可能读到一个完整的行
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            //如果下一个字符是“\n",则说明成功读取到一个完整的行
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //否则HTTP客户发送的数据存在语法错误
            return LINE_BAD;
        }
        //如果当前的字符是”\n"，即回车符，则可能读到一个完整的行
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;
    while (true)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        else if (bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;//m_read_idx:标识读缓冲区中已经读入的客户数据的最后一个字节的下一个位置
    }
    return true;
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    /*
    char *strpbrk(const char *str1, const char *str2) 
    检验字符串 str1 中的字符，当被检验字符在字符串 str2 中也包含时，则停止检验，并返回该字符位置
    */
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;//HTTP请求采用POST方式
        cgi = 1;//是否启用的POST
    }
    else
        return BAD_REQUEST;
    //size_t strspn(const char *str1, const char *str2) 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    /*
    strcasecmp() 函数比较两个字符串
    =0 : 如果两个字符串相等
    <0 : 如果 string1 小于 string2
    >0 : 如果 string1 大于 string2

    */
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        //char *strchr(const char *str, int c) 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;//HTTP请求报文不完整，继续读取数据

}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        //请求报文实体主体长度为0
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        //检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        //printf("oop!unknow header: %s\n",text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;
}

//没有真正解析HTTP请求的消息体，只是判断他是否被完整的读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;//存储请求报文的实体主体
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//服务器处理HTTP的请求结果
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;//记录当前的读取状态
    HTTP_CODE ret = NO_REQUEST;//记录HTTP请求的处理结果
    char *text = 0;

    //CHECK_STATE_CONTENT:请求报文消息体的状态
    //知道读取完整一行或主状态机所处状态为CHECK_STATE_CONTENT
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        //char *get_line() { return m_read_buf + m_start_line; };
        //m_start_line：当前正在解析行的起始位置
        text = get_line();

        //m_checked_idx：当前正在分析的字符在读缓冲区中的位置
        //m_checked_idx：在parse_line()函数中for遍历增加 
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        Log::get_instance()->flush();


        //m_check_state:主状态机当前的状态，init()初始化，默认分析请求行
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)////请求报文实体主体长度为0
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)//获取了完整请求报文
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;//服务器内部错误
        }
    }
    return NO_REQUEST;
}

/*当得到一完整的HTTP请求时，就分析目标文件的属性
如果目标文件存在，对所有用户可读，且不是目录
则使用mmap将其映射到内存地址m_file_address处，并告诉调用者获取文件成功*/
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    /*
    char *strrchr(const char *str, int c) 在参数 str 所指向的字符串中搜索最后一次出现字符 c（一个无符号字符）的位置
*/
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

//同步线程登录校验
#ifdef SYNSQL
        pthread_mutex_t lock;
        pthread_mutex_init(&lock, NULL);

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            
            if (users.find(name) == users.end())
            {

                pthread_mutex_lock(&lock);
                //查询是否有用户名和密码
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                pthread_mutex_unlock(&lock);

                if (!res)
                    //查找到res为0，登录
                    strcpy(m_url, "/log.html");
                else
                    //查找到res为1，注册
                    strcpy(m_url, "/registerError.html");
            }
            else//被注册了
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
#endif


//CGI多进程登录校验,用连接池,注册在父进程,登录在子进程
#ifdef CGISQLPOOL

        //注册
        pthread_mutex_t lock;
        pthread_mutex_init(&lock, NULL);

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                pthread_mutex_lock(&lock);
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                pthread_mutex_unlock(&lock);

                if (!res)
                {
                    strcpy(m_url, "/log.html");
                    pthread_mutex_lock(&lock);
                    //每次都需要重新更新id_passwd.txt
                    ofstream out("./CGImysql/id_passwd.txt", ios::app);
                    out << name << " " << password << endl;
                    out.close();
                    pthread_mutex_unlock(&lock);
                }
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //登录
        else if (*(p + 1) == '2')
        {
            pid_t pid;
            int pipefd[2];
            if (pipe(pipefd) < 0)
            {
                LOG_ERROR("pipe() error:%d", 4);
                return BAD_REQUEST;
            }
            if ((pid = fork()) < 0)
            {
                LOG_ERROR("fork() error:%d", 3);
                return BAD_REQUEST;
            }

            if (pid == 0)
            {
                //标准输出，文件描述符是1，然后将输出重定向到管道写端
                dup2(pipefd[1], 1);
                //关闭管道的读端
                close(pipefd[0]);
                //父进程去执行cgi程序，m_real_file,name,password为输入
                execl(m_real_file, name, password, "./CGImysql/id_passwd.txt", NULL);
            }
            else
            {
                //子进程关闭写端，打开读端，读取父进程的输出
                close(pipefd[1]);
                char result;
                int ret = read(pipefd[0], &result, 1);

                if (ret != 1)
                {
                    LOG_ERROR("管道read error:ret=%d", ret);
                    return BAD_REQUEST;
                }

                LOG_INFO("%s", "登录检测");
                Log::get_instance()->flush();
                //当用户名和密码正确，则显示welcome界面，否则显示错误界面
                if (result == '1')
                    strcpy(m_url, "/welcome.html");
                else
                    strcpy(m_url, "/logError.html");

                //回收进程资源
                waitpid(pid, NULL, 0);
            }
        }
#endif


//CGI多进程登录校验,不用数据库连接池
//子进程完成注册和登录
#ifdef CGISQL
        //fd[0]:读管道，fd[1]:写管道
        pid_t pid;
        int pipefd[2];
        if (pipe(pipefd) < 0)
        {
            LOG_ERROR("pipe() error:%d", 4);
            return BAD_REQUEST;
        }
        if ((pid = fork()) < 0)
        {
            LOG_ERROR("fork() error:%d", 3);
            return BAD_REQUEST;
        }

        if (pid == 0)
        {
            //标准输出，文件描述符是1，然后将输出重定向到管道写端
            dup2(pipefd[1],1);
            //关闭管道的读端
            close(pipefd[0]);
            //父进程去执行cgi程序，m_real_file,name,password为输入
            //./check.cgi name password
            execl(m_real_file, &flag, name,password,NULL);
        }
        else
        {
            //子进程关闭写端，打开读端，读取父进程的输出
            close(pipefd[1]);
            char result;
            int ret = read(pipefd[0], &result, 1);

            if (ret != 1)
            {
                LOG_ERROR("管道read error:ret=%d", ret);
                return BAD_REQUEST;
            }
            if (flag == '2')
            {
                //printf("登录检测\n");
                LOG_INFO("%s", "登录检测");
                Log::get_instance()->flush();
                //当用户名和密码正确，则显示welcome界面，否则显示错误界面
                if (result == '1')
                    strcpy(m_url, "/welcome.html");
                else
                    strcpy(m_url, "/logError.html");
            }
            else if (flag == '3')
            {
                LOG_INFO("%s", "注册检测");
                Log::get_instance()->flush();
                //当成功注册后，则显示登陆界面，否则显示错误界面
                if (result == '1')
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            //回收进程资源
            waitpid(pid, NULL, 0);
        }
#endif


    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}


void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
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

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);

            if (m_linger)
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

//往缓冲区写入待发送的数据
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    //VA_LIST 是在C语言中解决变参问题的一组宏，变参问题是指参数的个数不定，可以是传入一个参数也可以是多个
    va_list arg_list;
    va_start(arg_list, format);
    //如果成功调用此函数，返回写到buffer中的字符的个数（不包括结尾的'\0'）
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

//响应报文的状态行：版本+状态码+短语+CRLF，status状态码
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//响应报文首部字段
bool http_conn::add_headers(int content_len)
{
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}

//响应报文首部字段，内容长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
//响应报文首部的内容类型
bool http_conn::add_content_type()
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
//首部字段：m_linger:HTTP的请求保持连接
bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
//首部字段加入空白行
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

//报文主体（消息体）
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}


//HTTP_CODE:枚举类型，服务器处理HTTP的请求结果
//根据服务器处理HTTP请求的结果，决定返回给客户端的内容
//process_write返回true或false
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        add_status_line(200, ok_200_title);
        //m_file_stat:成员变量：目标文件（实体主体）的状态
        if (m_file_stat.st_size != 0)
        {
            //首部行，添加描述文件的长度
           /*
           HTTP响应报文在内存中分两块，状态行+首部字段+空格被web服务器放置在一块内存中
           而文档内容通常被读入到另一块内存中（通过mmap函数）
           */
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;//写缓冲区待发送的字节数
            m_iv[1].iov_base = m_file_address;//客户端请求的文件被mmap到内存的起始位置
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;//写内存块的数量
            bytes_to_send = m_write_idx + m_file_stat.st_size;//发送的字节数
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    //响应报文没有消息实体没有
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//由线程池中的工作线程调用，处理HTTP请求的入口函数
void http_conn::process()
{
    //process_read()服务器处理HTTP的请求结果
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        //将文件描述符fd重置为EPOLLONESHOT
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    //read_ret:服务器处理HTTP的请求结果
    //根据服务器处理HTTP请求的结果，决定返回给客户端的内容
    //process_write返回true或false
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        //关闭连接，关闭一个连接，客户总量减一
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}

#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
class http_conn
{
public:
    //文件名的最大长度
    static const int FILENAME_LEN = 200;
    //读缓冲区的大小
    static const int READ_BUFFER_SIZE = 2048;
    //写缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;
    //HTTP的请求方法
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //解析客户请求时，主状态机所处的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    //服务器HTTP的请求可能结果
    enum HTTP_CODE
    {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    //行的读取状态
    enum LINE_STATUS
    {
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化新接受的连接
    void init(int sockfd, const sockaddr_in &addr);
    //关闭连接
    void close_conn(bool real_close = true);
    //处理客户请求
    void process();
    //非阻塞读操作
    bool read_once();
    //非阻塞写操作
    bool write();
    //获取连接的socket地址
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    
    //将用户名和密码存入字典users中
    void initmysql_result(connection_pool *connPool);
    void initresultFile(connection_pool *connPool);

private:
    //初始化连接
    void init();
    //解析HTTP请求
    HTTP_CODE process_read();
    //填充HTTP应答
    bool process_write(HTTP_CODE ret);

    //下面一组函数被process_write调用以分析HTTP请求
    //从状态机，用于解析出一行内容
    HTTP_CODE parse_request_line(char *text);
    //分析头部字段
    HTTP_CODE parse_headers(char *text);
    ////没有真正解析HTTP请求的消息体，只是判断他是否被完整的读入
    HTTP_CODE parse_content(char *text);
    
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    //从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
    LINE_STATUS parse_line();

    //下面一组函数被process_write调用以填充HTTP应答
    void unmap();
    //往缓冲区写入待发送的数据
    bool add_response(const char *format, ...);
    //报文主体（消息体）
    bool add_content(const char *content);
    //响应报文的状态行：版本+状态码+短语+CRLF，status状态码
    bool add_status_line(int status, const char *title);
    //响应报文首部字段
    bool add_headers(int content_length);
    //响应报文首部的内容类型
    bool add_content_type();
    //响应报文首部字段，内容长度
    bool add_content_length(int content_length);
    //首部字段：m_linger:HTTP的请求保持连接
    bool add_linger();
    //首部字段加入空白行
    bool add_blank_line();

public:
    //所有socket被注册到同一个epoll内核事件表中，epoll文件描述符设置静态
    static int m_epollfd;
    //统计用户数量
    static int m_user_count;
    MYSQL *mysql;

private:
    //该HTTP连接的socket和对方的socket地址
    int m_sockfd;
    sockaddr_in m_address;
    //读缓冲区
    char m_read_buf[READ_BUFFER_SIZE];
    //标识读缓冲区中已经读入的客户数据的最后一个字节的下一个位置
    int m_read_idx;
    //当前正在分析的字符在读缓冲区中的位置
    int m_checked_idx;
    //当前正在解析行的起始位置
    int m_start_line;
    //写缓冲区
    char m_write_buf[WRITE_BUFFER_SIZE];
    //写缓冲区待发送的字节数
    int m_write_idx;

    //主状态机当前所处的状态
    CHECK_STATE m_check_state;
    //请求方法
    METHOD m_method;
    
    //客户端请求目标文件的完整路径，其内容等于doc_root+m_url，doc_root是网站目录
    char m_real_file[FILENAME_LEN];
    //客户端请求的目标文件的文件名
    char *m_url;
    //HTTP协议版本号，仅支持HTTP/1.1
    char *m_version;
    //主机名
    char *m_host;
    //HTTP请求消息体的长度
    int m_content_length;
    //HTTP请求是否保持连接
    bool m_linger;
    
    //客户端请求的目标文件被mmap到内存中的起始位置
    char *m_file_address;
    //目标文件的状态，通过它可以判断文件是否存在，是否可读，并获取文件的大小等信息
    struct stat m_file_stat;

    //采用writev来执行写操作，定义下面两个成员，其中m_iv_count表示被写内存块的数量
    struct iovec m_iv[2];
    int m_iv_count;

    int cgi;//是否启用的POST
    char *m_string; //存储请求头数据
    
    int bytes_to_send;
    int bytes_have_send;
};

#endif

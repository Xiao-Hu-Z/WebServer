



# Linux下C++搭建的Web服务器

## 说明


- 利用模拟Proactor事件处理模式实现半同步/半反应堆线程池，采用epoll边沿触发模式作为I/O复用技术
- 利用状态机的设计思想，按状态转移的方式实现了http的连接处理类，支持http长连接
- 利用升序的双向链表实现定时器处理非活跃连接，超时释放连接资源
- 实现了简单的数据库连接池，通过同步线程完成web端用户注册、登录校验
- 服务器可以请求图片和视频，经Webbench压力测试可以实现上万的并发连接数据交换    





## 框架




## Demo

## ![image2](https://github.com/Xiao-Hu-Z/WebServer/blob/master/root/demo.gif)

## 基础测试

* 服务器测试环境
	* Ubuntu版本16.04
	* MySQL版本5.7.29
* 测试前确认已安装MySQL数据库

    ```C++
    // 建立yourdb库
    create database yourdb;

    // 创建user表
    USE yourdb;
    CREATE TABLE user(
        username char(50) NULL,
        passwd char(50) NULL
    )ENGINE=InnoDB;

    // 添加数据
    INSERT INTO user(username, passwd) VALUES('name', 'passwd');
    ```

* 修改main.c中的数据库初始化信息

    ```C++
    // root 123修改为服务器数据库的登录名和密码
	// qgydb修改为上述创建的yourdb库名
    connPool->init("localhost", "root", "123", "yourdb", 3306, 8);
    ```

* 修改http_conn.cpp中的root路径

    ```C++
	// 修改为root文件夹所在路径
    const char* doc_root="/home/xiaohu/文档/WebServer/root";
    ```

* 生成server

    ```C++
    make server
    ```

* 启动server

    ```C++
    ./server port
    ```

* 浏览器端

    ```C++
    ip:port
    ```





参考：

Linux高性能服务器编程，游双著.

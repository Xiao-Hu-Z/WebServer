



# Linux下C++搭建的Web服务器

## 说明



- 使用**线程池 + epoll(ET和LT均实现) + 模拟Proactor模式**的并发模型

* 使用**状态机**解析HTTP请求报文，支持解析**GET和POST**请求
* 通过访问服务器数据库实现web端用户**注册、登录**功能，可以请求服务器**图片和视频文件**
* 实现**同步/异步日志系统**，记录服务器运行状态
* 经Webbench压力测试可以实现**上万的并发连接**数据交换

## 框架

 ![image1](https://github.com/Xiao-Hu-Z/WebServer/blob/master/root/frame.jpg)






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
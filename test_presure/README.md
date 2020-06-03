

服务器压力测试
===============
Webbench是有名的网站压力测试工具，它是由[Lionbridge](http://www.lionbridge.com)公司开发。

> * 测试处在相同硬件上，不同服务的性能以及不同硬件上同一个服务的运行状况。
> * 展示服务器的两项内容：每秒钟响应请求数和每秒钟传输数据量。

## 安装Webbench

```
wget http://home.tiscali.cz/~cz210552/distfiles/webbench-1.5.tar.gz

tar zxvf webbench-1.5.tar.gz
cd webbech-1.5

sudo apt-get install ctags

su
make & make install
```



测试规则
------------
* 测试示例

    ```C++
	webbench -c 500  -t  30   http://127.0.0.1:3307
    ```
* 参数

> * `-c` 表示客户端数
> * `-t` 表示时间


测试结果
---------
Webbench对服务器进行压力测试，经压力测试可以实现上万的并发连接.
> * 并发连接总数：10500
> * 访问服务器时间：5s
> * 每秒钟响应请求数：371664pages/min
> * 每秒钟传输数据量：693481bytes/sec
> * 所有访问均成功





![压力测试1](/home/xiaohu/文档/WebServer/root/压力测试1.png)
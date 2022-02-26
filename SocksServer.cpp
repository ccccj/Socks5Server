#include "./common.h"
#define MAXLINE 128
#define SPORT "7890" // 右边的下一个服务器的 port

enum State {
    STEP1,
    STEP2,
    FORWARD
};

struct Connect {
    int _client_fd;
    std::string _client_str;
    int _server_fd;
    std::string _server_str;
    int _ref;
    State _state;

    Connect() {
        _ref = 0;
    }
};

class SocksServer {
public:
    SocksServer() {
        _main_fd = -1;
        _events_fd = -1;
    }

    ~SocksServer() {
        if (_main_fd != -1) {
            close(_main_fd);
        }
    }

    int _main_fd; // 监听用的fd
    int _events_fd;

    std::map<int, struct Connect*> _con_map; // 存 new_fd

    void Start();
    int open_listenfd();
    void epoll_start();
    void events_ctl(int fd, int op, int how);
    void connect_handler();
    void read_handler(int fd);
    void forward(Connect* con, int fd, bool change);
    void send_loop(Connect* con, const char* str, int len, bool change);
    void write_handler(int fd);
    int negotiation1(int fd);
    int negotiation2(int fd);
    int open_clientfd(char ip[4], char port[2]);
    void SetNonBlock(int fd);

};




//



int SocksServer::open_listenfd() {
    PriInfo("open_listenfd start");
    // 成功，返回0；失败，返回-1
    struct addrinfo *p, *lisp, hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG | AI_PASSIVE;
    hints.ai_flags |= AI_NUMERICSERV;
    /*
    hint.ai_flags 需要修改：
    AI_ADDRCONFIG：在使用连接时设置这个。
    AI_CANONNAME：ai_canonname 默认为 NULL。设置这个标志后，将链表里第一个结构的 ai_canonname 指向主机的？？？
    AI_NUMERICSERV：原本 getaddrinfo 第二个参数，可以为服务名或端口号，设置后强制为端口号。
    AI_PASSIVE：返回的套接字地址为服务器监听套接字。此时 hostname 为 NULL
    */
    int err = getaddrinfo(NULL, SPORT, &hints, &lisp);
    if (err != 0) {
        printf("getaddrinfo error：%s\n", gai_strerror(err));
    }
    p = lisp;
    int optval = 1;
    for (p = lisp; p; p = p->ai_next) {
        _main_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (_main_fd < 0) continue; // 看链表中下一个结构体
        
        // 在 TCP 连接中，recv 等函数默认为阻塞模式(block)，即直到有数据到来之前函数不会返回，而我们有时则需要一种超时机制，使其在一定时间后返回，而不管是否有数据到来:
        // int  setsockopt(int fd, int level, int optname, void* optval, socklen_t* optlen);
        // 一个端口释放后会等待两分钟之后才能再被使用，SO_REUSEADDR是让端口释放后立即就可以被再次使用。
        setsockopt(_main_fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));
        // bind 服务器用来绑定本地端点地址（创建 主套接字）
        err = bind(_main_fd, p->ai_addr, p->ai_addrlen);
        if (err == 0) { // 绑定成功
            if ((err = listen(_main_fd, 1024)) == -1) {
                PriInfo("transfer error");
                close(_main_fd);
                //return -1;
            } else {
                break; // 成功建立连接
            }
        }
    }
    freeaddrinfo(lisp);
    if (p == NULL) {
        PriInfo("open_listenfd ：no addr");
        return -1; // 失败
    } else {
        PriInfo("open_listenfd ：success ; main_fd : ", _main_fd);
        SetNonBlock(_main_fd);
        return 0; // 成功
    }
}

void SocksServer::events_ctl(int fd, int op, int how) {
    // int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    // op：EPOLL_CTL_ADD、EPOLL_CTL_MOD、EPOLL_CTL_DEL
    struct epoll_event e;
    e.data.fd = fd;
    e.events = how;
    epoll_ctl(_events_fd, op, fd, &e);
}

void SocksServer::connect_handler() {
    PriInfo("connect_handler : Start");
    char clientaddr[1000];
    socklen_t clientlen = 1000;

    int left_fd = accept(_main_fd, (struct sockaddr *)&clientaddr, &clientlen);
    events_ctl(left_fd, EPOLL_CTL_ADD, EPOLLIN);
    PriInfo("accept success, left_fd:", left_fd);

    SetNonBlock(left_fd);

    Connect* con = new Connect();
    con->_client_fd = left_fd;
    ++con->_ref;
    con->_state = STEP1;
    
    _con_map[left_fd] = con;

    PriInfo("connect_handler : Success Left");

}
void SocksServer::send_loop(Connect* con, const char* str, int len, bool change) { // 要发送到右边去
    int fd;
    if (change == 0) {
        fd = con->_server_fd;
    } else {
        fd = con->_client_fd;
    }
    PriInfo("send_loop start");
    int send_len = send(fd, str, len, 0);
    PriInfo("send_loop finish");

    if (send_len < 0) {
        PriInfo("send error");
    } else if (send_len < len) {
        PriInfo("Send_loop Success");
        printf("\n\n\n\nsend_len : %d, len : %d\n\n\n\n\n", send_len, len);
        events_ctl(fd, EPOLL_CTL_MOD, EPOLLIN | EPOLLOUT | EPOLLONESHOT);

        if (change == 0) {
            con->_server_str.append(str + send_len);    
        } else {
            con->_client_str.append(str + send_len);
        }
    } else if (send_len == len) {
        PriInfo("send success in once");
    }
}

void SocksServer::forward(Connect* con, int fd, bool change) {
    int left = con->_client_fd, right = con->_server_fd;
    if (change) {
        std::swap(left, right);
    }

    PriInfo("forward : Start");
    printf("forward from %d to %d\n", left, right);
    
    //printf("left_fd : %d, right_fd : %d\n", con->_client_fd, con->_server_fd);
    
    char buf[4096];
    int len = recv(left, buf, sizeof(buf), 0);
    
    if (len == 0) {
        //PriInfo("buf长度为0");
        // 说明传完了！！！
        // 之前是分了多次写只关闭写端，才开了对S写端的监听，现在写完了，关掉对S写的监听
        // 传完立即关闭写端监听
        shutdown(right, SHUT_WR);
        events_ctl(left, EPOLL_CTL_DEL, 0); // 此时第三个参数是忽略的
        if (--con->_ref == 0) {
            _con_map.erase(left);
            delete con;
        }
    } else if (len < 0) {
        PriInfo("read error");
    } else {
        XORcode(buf, len); // 加密
        buf[len] = '\0';
        
        PriInfo("buf长度大于0，长度为", len);
        // for (int i = 0; i < len; ++i) {
        //     printf("buf[%d] : %d %c\n", i, buf[i], buf[i]);
        // }
        PriInfo(buf);
        
        send_loop(con, buf, len, change);
    }
}

int SocksServer::negotiation1(int fd) { // 成功 1， 失败 -1
    PriInfo("negotiation1111111 Start");
    /*
    1、客户端发送的报头
    +---------+---------------+-----------+
    | VERSION | METHODS_COUNT |  METHODS  |
    +---------+---------------+-----------+
    |   0x05  |       1       |   '00'    |
    +---------+---------------+-----------+
    前两个量都是一个字节的长度，第三个量是可变长度的，长度由第二个字节的值表示
    调试时发现，浏览器发的是 5 1 0，服务器回的是 5 0，成功
    */
    char buf[4096];
    int len = recv(fd, buf, sizeof(buf), 0);
    if (len == 0) {
        return 0;
    }
    if (len > 0) {
        PriInfo("第一次认证开始！");
        char ret[2];
        ret[0] = 0x05;

        XORcode(buf, len);
        if (buf[0] == 0x05) {
            ret[1] = 0x00; // 选择 不加密方式
            XORcode(ret, 2);
            send(fd, ret, sizeof(ret), 0);
            PriInfo("第一次认证成功！");
            return 1;
        } else {
            ret[1] = 0xFF;
            XORcode(ret, 2);
            send(fd, ret, sizeof(ret), 0);
            return -1;
        }
    } else {
        return -1;
    }
}

int SocksServer::negotiation2(int fd) {
    PriInfo("negotiation222222 Start");
    /*
    客户端发送需要访问的IP和端口，以及协议
    +---------+---------+-------+------+----------+----------+
    | VERSION | COMMAND |  RSV  | TYPE | DST.ADDR | DST.PORT |
    +---------+---------+-------+------+----------+----------+
    |    1    |    1    |   1   |  1   | Variable |    2     |
    +---------+---------+-------+------+----------+----------+
    TYPE:
    0x01：表示 IPv4 地址 / 0x03：域名地址(没有打错，就是没有0x02) / 0x04：IPv6 地址
    */
    char buf[4096];
    int len = recv(fd, buf, 4, 0);
    if (len <= 0) return -1;
    if (len < 4) return 0;
    PriInfo("第二次认证开始！");
    for (int i = 0; i < len; ++i) {
        printf("buf[%d] : %d %c\n", i, buf[i], buf[i]);
    }
    
    
    XORcode(buf, len);
    if (buf[0] != 0x05 || buf[2] != 0x00) return -1;
    
    char ip4[MAXLINE], port[5];

    if (buf[3] == 0x04) { // 不支持ipv6
        return -1;
    } else if (buf[3] == 0x01) { // ipv4
        PriInfo("buf[3] == 0x01 ; ipv4");
        len = recv(fd, ip4, 4, 0);
        if (len < 4) return 0;
        
        XORcode(ip4, 4);
        len = recv(fd, port, 2, 0);

        if (len < 2) return 0;
        XORcode(port, 4);
        ip4[4] = '\0';
        port[2] = '\0';
        int client_fd = open_clientfd(ip4, port); 
        return client_fd;
    } else if (buf[3] == 0x03) { // 域名字段
        PriInfo("buf[3] == 0x03 ; doname");

        // 域名字段中第一个字节是真实的域名的长度，后面才是真实的域名
        char doname_len;
        char doname[MAXLINE];
        len = recv(fd, &doname_len, 1, 0);
        printf("doname_len的长度%d\n", len);
        
        if (len < 1) return 0;
        XORcode(&doname_len, 1);
        
        len = recv(fd, doname, doname_len, 0);
        printf("doname的长度%d\n", len);

        // for (int i = 0; i < len; ++i) {
        //     printf("doname[%d] : %d %c\n", i, doname[i], doname[i]);
        // }
        // if (len < 1) return -1;
        
        XORcode(doname, len);
        doname[len] = '\0';
        PriInfo(doname); // 对的
        

        struct hostent* host = gethostbyname(doname);
        
        PriInfo("1");
        memcpy(ip4, host->h_addr, host->h_length);
        PriInfo("2");
        len = recv(fd, port, 2, 0);
        printf("port的长度%d\n", len);
        if (len < 2) return 0;
        XORcode(port, 2);
        PriInfo("3");
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in server;
        server.sin_family = AF_INET;
        memcpy(&server.sin_addr.s_addr, ip4, 4);
        server.sin_port = *((uint16_t*)port);

        if (connect(client_fd, (struct sockaddr*)&server, sizeof(server))) {
            PriInfo("connect success");
            close(client_fd);
            return 0;
        }
        return client_fd;

    } else {
        return -1;
    }
}

int SocksServer::open_clientfd(char* ip, char* port) {
    PriInfo("open_clientfd start");
    
    struct addrinfo *p, *lisp, hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    
    //getaddrinfo
    int err = getaddrinfo(ip, port, &hints, &lisp);

    if (err != 0) {
        printf("getaddrinfo error：%s\n", gai_strerror(err));
    }

    p = lisp;
    int fd;
    for (p = lisp; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue; // 看链表中下一个结构体
        }
        /*
        int connect(int clientfd, const struct sockaddr *addr, socklen_t addrlen);
        客户端用来建立和服务器的连接（指定S端点地址），返回值：OK(0)，error(-1)
        */
        err = connect(fd, p->ai_addr, p->ai_addrlen);

        if (err == 0) {
            freeaddrinfo(lisp);
            PriInfo("open_clientfd sucess");
            return fd;
        } else {
            close(fd); // 建立连接失败
        }
    }

    freeaddrinfo(lisp);
    return -1;
}



void SocksServer::read_handler(int fd) {
    std::map<int, Connect*>::iterator it = _con_map.find(fd);
    
    if (it != _con_map.end()) {
        Connect* con = it->second;


        if (con->_state == STEP1) {
            printf("step1, left_fd: %d\n", fd);
            
            int ret = negotiation1(fd);
            if (ret == 1) {
                con->_state = STEP2;
            } else if (ret == -1) {
                events_ctl(con->_client_fd, EPOLL_CTL_DEL, 0); // 此时第三个参数是忽略的
                if (--con->_ref == 0) {
                    _con_map.erase(con->_client_fd);
                    delete con;
                }
            }
        } else if (con->_state == STEP2) {
            printf("=============step2, left_fd: %d=============\n", fd);

            char ret[10];
            memset(ret, 0, 10);
            ret[0] = 0x05;
            int right_fd = negotiation2(fd);

            if (right_fd == 0) {
                PriInfo("?????????????");
            } else if (right_fd > 0) {
                ret[1] = 0x00;
                ret[3] = 0x01;
                XORcode(ret, 10);
                send(con->_client_fd, ret, 10, 0);

                events_ctl(right_fd, EPOLL_CTL_ADD, EPOLLIN);
                PriInfo("socket right success, right_fd:", right_fd);
                SetNonBlock(right_fd);
                con->_server_fd = right_fd;
                ++con->_ref;
                con->_state = FORWARD;

                _con_map[right_fd] = con;
                

                printf("=========第二次认证成功，left_fd = %d，-> FORWARD=======\n", fd);

            } else if (right_fd == -1) {
                PriInfo("第二次认证失败！");
                ret[1] = 0x01;
                XORcode(ret, 10);
                send(con->_client_fd, ret, 10, 0);

                events_ctl(con->_client_fd, EPOLL_CTL_DEL, 0); // 此时第三个参数是忽略的
                if (--con->_ref == 0) {
                    _con_map.erase(con->_client_fd);
                    delete con;
                }
            }
        } else if (con->_state == FORWARD) {
            PriInfo("转发！！！");
            if (fd == con->_client_fd) {
                forward(con, fd, 0);
            } else {
                forward(con, fd, 1);
            }
            
        } else {
            PriInfo("read_handler error");
        }
    }
}



void SocksServer::write_handler(int fd) {
    PriInfo("=========write_handler======");
    PriInfo("write_handler : Start");
    std::map<int, struct Connect*>::iterator it = _con_map.find(fd);
    if (it != _con_map.end()) {
        Connect* con = it->second;
        std::string buf;

        bool change = 0;
        if (con->_client_fd == fd) {
            change = 1;
            buf.swap(con->_client_str);
            //std::swap(con->_client_fd, con->_server_fd);
            //std::swap(con->_client_str, con->_server_str);
        } else {
            buf.swap(con->_server_str);
        }
        send_loop(con, buf.c_str(), buf.size(), change);
    }
}

void SocksServer::SetNonBlock(int fd) {
    // fcntl系统调用可以用来对已打开的文件描述符进行各种控制操作以改变已打开文件的的各种属性
    // F_GETFL 读取文件状态标志
    int iFlags = iFlags = fcntl(fd, F_GETFL, 0);
    // F_SETF 设置文件状态标志
    fcntl(fd, F_SETFL, iFlags | O_NONBLOCK);
}

void SocksServer::epoll_start() {
    PriInfo("epoll_start：Start");
    _events_fd = epoll_create(10000);
    PriInfo("_events_fd:", _events_fd);
    events_ctl(_main_fd, EPOLL_CTL_ADD, EPOLLIN);
    struct epoll_event events[100000];
    
    while (1) {
        int num = epoll_wait(_events_fd, events, 100000, 0); 
        // PriInfo("num : ", num);
        // events[i].events是触发行为，如 EPOLLIN
        // events[i].data.fd是触发的fd
        
        for (int i = 0; i < num; ++i) {
            if (events[i].data.fd == _main_fd) {
                // 监测连接请求
                connect_handler();
            } else if(events[i].events & (EPOLLIN | EPOLLPRI)) {
                read_handler(events[i].data.fd);
            } else if(events[i].events & EPOLLOUT) {
                PriInfo("write events！");
                write_handler(events[i].data.fd);
            } else{
                PriInfo("epoll error");
            }
        }
    }
}

void SocksServer::Start() {
    open_listenfd();
    epoll_start();
}

int main() {
    PriInfo("Start");
    SocksServer s;
    s.Start();
}

//

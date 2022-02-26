#include "./common.h"
#define MAXLINE 128
#define DONAME "167.179.119.148"
//#define DONAME "180.101.49.12" // 右边的下一个服务器的 ip
//#define DONAME "www.baidu.com"
#define PORT "7890" // 右边的下一个服务器的 port
#define SPORT "5678" // 自己作为S，开放的port

struct Connect {
    int _client_fd;
    std::string _client_str;
    int _server_fd;
    std::string _server_str;
    int _ref;
    Connect() {
        _ref = 0;
    }
};

class Transfer {
public:
    Transfer(const char* hostname, const char* port);
    ~Transfer() {
        if (_main_fd != -1) {
            close(_main_fd);
        }
    }

    int _main_fd; // 监听用的fd
    int _events_fd;

    char _hostname[MAXLINE + 1];
    char _port[MAXLINE + 1];

    std::map<int, struct Connect*> _con_map; // 存 new_fd


    void Start();
    int open_listenfd();
    void epoll_start();
    void events_ctl(int fd, int op, int how);
    void connect_handler();
    int open_clientfd();
    void read_handler(int fd);
    void forward(Connect* con, bool change);
    void send_loop(Connect* con, const char* str, int len, bool change);
    void write_handler(int fd);
    void SetNonBlock(int fd);

};






//

Transfer::Transfer(const char* hostname, const char* port) {

    _main_fd = -1;
    _events_fd = -1;

    int len = strlen(hostname);
    if (len <= MAXLINE) {
        memcpy(_hostname, hostname, len);
        _hostname[len] = '\0';
    }

    len = strlen(port);
    if (len <= MAXLINE) {
        memcpy(_port, port, len);
        _port[len] = '\0';
    }
}

int Transfer::open_listenfd() {
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
                PriInfo("listen error");
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

void Transfer::events_ctl(int fd, int op, int how) {
    // int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    // op：EPOLL_CTL_ADD、EPOLL_CTL_MOD、EPOLL_CTL_DEL
    struct epoll_event e;
    e.data.fd = fd;
    e.events = how;
    epoll_ctl(_events_fd, op, fd, &e);
}

int Transfer::open_clientfd() {
    
    struct addrinfo *p, *lisp, hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;
    int err = getaddrinfo(_hostname, _port, &hints, &lisp);

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
            PriInfo("open_clientfd success");
            return fd;
        } else {
            PriInfo("open_clientfd 失败");
            close(fd); // 建立连接失败
        }
    }

    freeaddrinfo(lisp);
    return -1;
}

void Transfer::connect_handler() {
    PriInfo("connect_handler : Start");
    char clientaddr[1000];
    socklen_t clientlen = 1000;

    int left_fd = accept(_main_fd, (struct sockaddr *)&clientaddr, &clientlen);
    events_ctl(left_fd, EPOLL_CTL_ADD, EPOLLIN);
    PriInfo("accept success, left_fd:", left_fd);
    
    int right_fd = open_clientfd();
    if (right_fd < 0) {
        PriInfo("open_clientfd 失败");
    }
    events_ctl(right_fd, EPOLL_CTL_ADD, EPOLLIN);
    PriInfo("accept success, right_fd:", right_fd);

    SetNonBlock(left_fd);
    SetNonBlock(right_fd);

    Connect* con = new Connect();
    con->_client_fd = left_fd;
    con->_server_fd = right_fd;
    con->_ref += 2;
    
    _con_map[left_fd] = con;
    _con_map[right_fd] = con;
    
    PriInfo("connect_handler : Success");
}

void Transfer::send_loop(Connect* con, const char* str, int len, bool change) { // 要发送到右边去
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

void Transfer::forward(Connect* con, bool change) {
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

void Transfer::read_handler(int fd) {
    std::map<int, Connect*>::iterator it = _con_map.find(fd);
    
    if (it != _con_map.end()) {
        Connect* con = it->second;
        bool change = 0;
        if (con->_server_fd == fd) {
            change = 1;
            //printf("read handler/ left_fd : %d, right_fd : %d\n", con->_client_fd, con->_server_fd);
            //PriInfo("read_handler : Start, read right");
            //std::swap(con->_client_fd, con->_server_fd);
            //std::swap(con->_client_str, con->_server_str);
            
            //printf("read new fd/ left_fd : %d, right_fd : %d\n", con->_client_fd, con->_server_fd);

        } else {
            PriInfo("read_handler : Start, read left");
        }
        forward(con, change);
    }
}

void Transfer::write_handler(int fd) {
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

void Transfer::SetNonBlock(int fd) {
    // fcntl系统调用可以用来对已打开的文件描述符进行各种控制操作以改变已打开文件的的各种属性
    // F_GETFL 读取文件状态标志
    int iFlags = iFlags = fcntl(fd, F_GETFL, 0);
    // F_SETF 设置文件状态标志
    fcntl(fd, F_SETFL, iFlags | O_NONBLOCK);
}

void Transfer::epoll_start() {
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

void Transfer::Start() {
    open_listenfd();
    epoll_start();
}

void signal_func(int sig) {}

int main() {
    signal(SIGPIPE, signal_func);
    PriInfo("Start");
    Transfer t(DONAME, PORT);
    t.Start();
}

//

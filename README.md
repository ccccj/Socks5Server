# Socks5Server
使用Socks5Server实现加密隧道穿透

将 common.h 和 Transfer.cpp 运行在墙内的服务器上，common.h 和 Socks5Server.cpp 运行在墙外的服务器上，可以实现穿透防火墙。前者的服务器实现加密转发功能，后者的服务器实现解密，Socks5协议解析功，以及转发功能。

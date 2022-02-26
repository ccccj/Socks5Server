#include <cstdio>
#include <cstring>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <iostream>
#include <cstring>
#include <map>
#include <sys/epoll.h>
#include<fcntl.h>


void PriInfo(const char* str, int num = -2) {
    printf("\n\n*******************************************\n");
    
    if (num == -2) {
        printf("%s\n", str);
    } else {
        printf("%s %d\n", str, num);
    }
    printf("*******************************************\n\n\n");
}

void XORcode(char* str, int len) {
    for (int i = 0; i < len; ++i) {
        str[i] ^= 1;
    }
}





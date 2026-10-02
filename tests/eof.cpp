#include <gtest/gtest.h>
#include "Buffer.hpp"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
using namespace std;
static void makePair(int &readFd,int &writeFd){
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    readFd = fds[0];
    writeFd = fds[1];
    // 关键：读端设成"非阻塞"，否则 readFd 读完现有数据后会一直等着收新数据
    fcntl(readFd, F_SETFL, fcntl(readFd, F_GETFL, 0) | O_NONBLOCK);
}
TEST(eof , checkreadFdback){
    int rd,wd;
    makePair(rd,wd);
    write(wd, "hello", 5);
    close(wd);
    Buffer b;
    int n=b.readFd(rd);
    EXPECT_EQ(b.readableBytes(),5);

    EXPECT_EQ(n,-1);
    n=b.readFd(rd);
    EXPECT_EQ(b.readableBytes(),5);
    EXPECT_EQ(n,-1);
    
    close(rd);

}
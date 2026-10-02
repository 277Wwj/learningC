// tests/buffer_test.cpp —— Buffer 的单元测试
#include <gtest/gtest.h>
#include "Buffer.hpp"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
using namespace std;

// 小工具：造一对"连通的 socket"——往 writeFd 写，readFd 那头就能收到
// （专门用来测 readFd；readFd 要喂一个"真的 fd"才能工作）
static void makePair(int& readFd, int& writeFd) {
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    readFd = fds[0];
    writeFd = fds[1];
    // 关键：读端设成"非阻塞"，否则 readFd 读完现有数据后会一直等着收新数据
    fcntl(readFd, F_SETFL, fcntl(readFd, F_GETFL, 0) | O_NONBLOCK);
}

// ============ 示例 1：最简单的 ============
TEST(BufferTest, InitEmpty) {          // TEST(测试组名, 用例名)
    Buffer b;
    EXPECT_EQ(b.readableBytes(), 0u);  // 期望：可读字节数 == 0
}

// ============ 示例 2：完整流程 ============
TEST(BufferTest, ReadAndRetrieve) {
    int rd, wr;
    makePair(rd, wr);

    write(wr, "hello", 5);             // 模拟客户端发来 5 字节

    Buffer b;
    int n = b.readFd(rd);
    EXPECT_EQ(n, 5);                                        // 读到 5 字节
    EXPECT_EQ(b.readableBytes(), 5u);                       // 缓冲区里有 5 字节
    EXPECT_EQ(string(b.peek(), b.readableBytes()), "hello");// 内容正确

    b.retrieve(2);                                          // 取走 2 字节
    EXPECT_EQ(b.readableBytes(), 3u);                       // 还剩 3
    EXPECT_EQ(b.retrieveAllAsString(), "llo");              // 全部取出 = llo
    EXPECT_EQ(b.readableBytes(), 0u);                       // 掏空了

    close(rd);
    close(wr);
}
TEST(BufferTest,RetrieveTooMuch){
    int rd,wr;
    makePair(rd,wr);
    write(wr,"hello",5);
    Buffer b;
    b.readFd(rd);
    b.retrieve(1000);
    EXPECT_EQ(b.readableBytes(), 0u);  
    close(rd);
    close(wr);
}
TEST(BufferTest,writesum){
    int rd,wr;
    makePair(rd,wr);
    write(wr,"hello",5);
    Buffer b;
    b.readFd(rd);
    EXPECT_EQ(b.readableBytes(),5);
    write(wr,"hello",5);
    b.readFd(rd);
    EXPECT_EQ(b.readableBytes(),10);
    close(rd);
    close(wr);
}
TEST(BufferTest,helfgetandallget){
    int rd,wr;
    makePair(rd,wr);
    write(wr,"hello",5);
    Buffer b;
    b.readFd(rd);
    b.retrieve(2);
    EXPECT_EQ(b.readableBytes(),3);
    
    EXPECT_EQ(b.retrieveAllAsString(), "llo");
    close(rd);
    close(wr);

}
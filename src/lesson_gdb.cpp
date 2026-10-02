// lesson_gdb.cpp —— GDB 课的"案发现场"
// 场景：连接表里按 fd 找连接，再处理这个连接
#include <bits/stdc++.h>
using namespace std;

struct Connection {
    int fd;
    string name;
};

// 在连接表里按 fd 找连接
Connection* findConn(vector<Connection*>& conns, int fd) {
    for (auto* c : conns) {
        if (c->fd == fd) return c;
    }
    return nullptr;
}

// 处理一个连接上的请求
void handleRequest(Connection* c) {
    cout << "处理连接 fd=" << c->fd << " 用户=" << c->name << endl;
}

void processFd(vector<Connection*>& conns, int fd) {
    Connection* c = findConn(conns, fd);
    if(c)
    handleRequest(c);
    else {
    cout << "连接 fd=" << fd << " 不存在，忽略" << endl;
}


}

int main() {
    vector<Connection*> conns;
    conns.push_back(new Connection{3, "alice"});
    conns.push_back(new Connection{5, "bob"});

    cout << "--- 处理 fd=3（存在）---" << endl;
    processFd(conns, 3);

    cout << "--- 处理 fd=99（不存在）---" << endl;
    processFd(conns, 99);

    cout << "程序正常结束" << endl;
    return 0;
}

#include<bits/stdc++.h>
using namespace std;
int main(){
    string raw=
    "GET /index.html HTTP/1.1\r\n"
    "Host: localhost:8080\r\n"
    "User-Agent: curl/8.0\r\n"
    "\r\n";
    size_t header_end=raw.find("\r\n\r\n");
    if(header_end==string ::npos){
        printf("头部还没收齐（半包，要等更多数据）\n");
        return 0;
    }
    string header=raw.substr(0,header_end);
    printf("完整头部：\n%s\n-----\n", header.c_str());
    stringstream ss(header);
    string line;
    getline(ss,line,'\n');
    if(!line.empty()&&line.back()=='\r')line.pop_back();
    stringstream ls(line);
    string method,path,version;
    ls>>method>>path>>version;
    printf("方法=%s  路径=%s  版本=%s\n", method.c_str(), path.c_str(), version.c_str());
    while(getline(ss,line)){
        if(!line.empty()&&line.back()=='\r')
        line.pop_back();
        size_t colon=line.find(':');
        if(colon!=string::npos){
            string key=line.substr(0,colon);
            string value=line.substr(colon+2);
            printf("头部[%s] = %s\n", key.c_str(), value.c_str());
        }
        
    }
    return 0;
}
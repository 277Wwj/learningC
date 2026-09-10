#pragma once
#include <vector>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <errno.h>
class Buffer{
    public:
    Buffer(){}
    int readFd(int fd){
        int total=0;
        char buf[4096];
        while(true){
            int n=recv(fd,buf,sizeof(buf),0);
            if(n>0){
                append(buf,n);
                total+=n;
            }
            else if(n==0){
                return -1;

            }
            else{
                if(errno==EAGAIN||errno==EWOULDBLOCK){
                    break;
                }
                return -1;
            }
        }
        return total;

    }
    size_t readableBytes()const{
        return writeIndex_-readIndex_;
    }
    const char *peek()const{
        return data_.data()+readIndex_;
    }
    void retrieve(size_t n){
        if(n>=readableBytes()){
            readIndex_=writeIndex_=0;
            
        }
        else
        {
            readIndex_+=n;
        }
    }
    string retrieveAllAsString(){
        string s(peek(),readableBytes());
        retrieve(readableBytes());
        return s;
    }
    private:
    void append(const char* data,size_t len){
        if(data_.size()<writeIndex_+len){
            data_.resize(writeIndex_+len);

        }
        memcpy(data_.data()+writeIndex_,data,len);
        writeIndex_+=len;
    }
    vector<char>data_;
    size_t readIndex_=0;
    size_t writeIndex_=0;
};



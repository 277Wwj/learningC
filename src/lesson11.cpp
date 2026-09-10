#include <bits/stdc++.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <unistd.h>
using namespace std;
int main(){
    int tfd=timerfd_create(CLOCK_MONOTONIC,0);
    struct itimerspec spec{};
    spec.it_value.tv_sec=2;
    spec.it_interval.tv_sec=2;
    timerfd_settime(tfd,0,&spec,nullptr);
    int epfd=epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);
    epoll_event events[8];
    for(int i=0;i<10;i++){
        int n=epoll_wait(epfd,events,8,-1);
        for(int j=0;j<n;j++){
            if(events[j].data.fd==tfd){
                uint64_t expriations;
                read(tfd,&expriations,sizeof(expriations));
                printf("定时器触发第 %d 次，到期了 %lu 次\n", i + 1, expriations);
            }
        }
    }
close(tfd);
    close(epfd);
    return 0;
}


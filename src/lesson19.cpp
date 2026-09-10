#include<bits/stdc++.h>
using namespace std;
int  counter1=0;
void add1(){
    for(int i=0;i<100000;i++){
        counter1++;
    }
}
atomic<int>counter2=0;
void add2(){
    for(int i=0;i<100000;i++){
        counter2++;
    }
}
int main(){
    cout << "--- 实验1：普通 int ---\n";
    thread t1(add1),t2(add1);
    t1.join();t2.join();
    cout<<"counter1="<<counter1<<"(期望200000)\n\n";
    cout << "--- 实验2：atomic<int> ---\n";
    thread t3(add2),t4(add2);
    t3.join();
    t4.join();
    cout << "counter2 = " << counter2 << "（期望 200000）\n";
    return 0;
}
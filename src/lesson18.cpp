#include<bits/stdc++.h>
using namespace std;
struct Foo{
    Foo(){
        cout<<"Foo构造\n";
    }
    ~Foo(){
        cout<<"Foo析构（内存释放）\n";
    }
};
int main(){
    cout << "--- 实验1：unique_ptr 自动释放 ---\n";
    {
        unique_ptr <Foo> p=make_unique<Foo>();
        //相当于Foo *a=new Foo();
        //unique_ptr<Foo> b(a);
    }
    cout << "（看到析构了吗？没有手动 delete！）\n\n";
    cout << "--- 实验2：shared_ptr 引用计数 ---\n";
    shared_ptr<Foo> a = make_shared<Foo>();
    
    cout<<"计数="<<a.use_count()<<"\n";
    {
        shared_ptr<Foo>b=a;
        cout<<"计数="<<a.use_count()<<"\n";
    }
    cout<<"计数="<<a.use_count()<<"\n";
    cout << "--- 实验3：unique_ptr 不能拷贝只能移动 ---\n";
    unique_ptr<Foo> p1=make_unique<Foo>();
    unique_ptr<Foo> p2 = move(p1);
    cout << "p1 变空了吗？ " << (p1 == nullptr ? "是" : "否") << "\n";
    return 0;
}
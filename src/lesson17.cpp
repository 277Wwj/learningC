#include<bits/stdc++>
using namespace std;
class Big{
    public:
    Big():data_(new int [1000]) {
        cout<<"默认构造年\n";
    }
    Big(const Big&other):data_(new int [1000]){
        copy(other.data_,other.data+1000,data_);
        cout<<"拷贝构造（慢）"\n;

    }
    Big(Big &&other)noexcept:data_(other.data_){
        other.data_=nullptr;
        cout<<"移动构造（快）";

    }
    ~Big(){
        delete[] data_;

    }
    private:
    int *data_;

}
int main(){
    cout << "--- 实验 1：临时对象入 vector ---\n";
    vector<Big>v;
    v.push_back(Big());
    cout << "--- 实验 2：std::move ---\n";
    Big a;
    Big b=move(a);
    cout << "--- 实验 3：普通拷贝 ---\n";
    Big c=b;
    return 0;
    
}
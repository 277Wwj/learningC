#include <bits/stdc++.h>
using namespace std;

// —— 实验1：多态 ——
class Animal {
public:
    virtual void speak() { cout << "Animal speaks\n"; }
    virtual ~Animal() { cout << "~Animal\n"; }   // 虚析构
};
class Dog : public Animal {
public:
    void speak() override { cout << "Woof!\n"; }
    ~Dog() { cout << "~Dog\n"; }
};
class Cat : public Animal {
public:
    void speak() override { cout << "Meow!\n"; }
    ~Cat() { cout << "~Cat\n"; }
};

// —— 实验2：虚析构的坑 ——
class Base {
public:
    ~Base() { cout << "~Base（非虚）\n"; }
};
class Derived : public Base {
public:
    ~Derived() { cout << "~Derived\n"; }   // 这行永远执行不到！
};

int main() {
    cout << "--- 实验1：多态 ---\n";
    Animal* a = new Dog();
    a->speak();                 // 输出 Woof!
    delete a;                   // 输出 ~Dog ~Animal（虚析构生效）
    a = new Cat();
    a->speak();                 // 输出 Meow!
    delete a;                   // 输出 ~Cat ~Animal

    cout << "\n--- 实验2：非虚析构的坑 ---\n";
    Base* p = new Derived();
    delete p;                   // 只输出 ~Base，~Derived 被吞了！
    return 0;
}
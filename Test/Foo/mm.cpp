// main.cpp
#include <iostream>

// 声明命名空间中的函数（不使用头文件）
namespace MathUtils {
    int add(int a, int b);
    int multiply(int a, int b);
}

// 声明全局函数
void printMessage(const char* message);

int main() {
    // 使用命名空间中的函数
    std::cout << "5 + 3 = " << MathUtils::add(5, 3) << std::endl;
    std::cout << "5 * 3 = " << MathUtils::multiply(5, 3) << std::endl;
    
    // 调用全局函数
    printMessage("Hello from main!");
    
    return 0;
}
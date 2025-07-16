// math_functions.cpp
#include <iostream>

// 定义一个命名空间
namespace MathUtils {
    // 计算两个数的和
    int add(int a, int b) {
        return a + b;
    }

    // 计算两个数的乘积
    int multiply(int a, int b) {
        return a * b;
    }
}

// 全局函数
void printMessage(const char* message) {
    std::cout << "Message: " << message << std::endl;
}
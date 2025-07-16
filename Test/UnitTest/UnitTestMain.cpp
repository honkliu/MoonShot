#include <future>
#include <iostream>
#include <map>
#include <functional>
#include <string>
#include <exception>

extern std::map<std::string, std::function<void()>> testRegistry; 

int main(int argc, char* argv[]) {

    try {
        // 无参数时显示帮助
        if (argc < 2) {
            std::cout << "Available tests:\n";
            for (const auto& [name, _] : testRegistry) {
                std::cout << "  " << name << "\n";
            }
            std::cout << "\nUsage: " << argv[0] << " <test_name> [test_name2 ...]\n";
            return 1;
        }

        // 运行所有指定的测试
        for (int i = 1; i < argc; ++i) {
            std::string testName = argv[i];
            if (auto it = testRegistry.find(testName); it != testRegistry.end()) {
                std::cout << "===== Running test: " << testName << " =====\n";
                it->second(); // 执行测试函数
                std::cout << "+++++ Test passed: " << testName << " +++++\n\n";
            } else {
                std::cerr << "!!!!! Unknown test: " << testName << " !!!!!\n";
                std::cerr << "Valid tests:";
                for (const auto& [name, _] : testRegistry) {
                    std::cerr << " " << name;
                }
                std::cerr << "\n";
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "!!!!! Test execution failed: " << e.what() << " !!!!!\n";
        return 3;
    }
    
    return 0;
}

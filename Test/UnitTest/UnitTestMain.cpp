#include "IndexContext.h"
#include "EvalExpression.h"
#include "IndexReader.h"
#include "AdvancedIndexReader.h"
#include "AdvancedIndexWriter.h"
#include "IndexSearchExecutor.h"
#include "IndexSearchCompiler.h"
#include "ConfigParameters.h"
#include "Tokenizer.h"
#include "BlockTable.h"
#include <future>
#include <iostream>
#include <map>
#include <functional>
#include <string>
#include <exception>


// 测试函数注册表
std::map<std::string, std::function<void()>> testRegistry = {
    {"TestSingleRead", IndexAccessTests::TestSingleRead},
    {"TestingEndToEnd", IndexAccessTests::TestingEndToEnd},
    {"TestCompositeRead", IndexAccessTests::TestCompositeRead},
    {"TestVectorRead", IndexAccessTests::TestVectorRead}
};

int main(int argc, char* argv[]) {
    using namespace IndexAccessTests;
    
    // 初始化配置
    ConfigParameters config;
    IndexBlockTable table;
    SetupIndex("test_index", &table, &config);

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

    // 清理资源
    delete index_context;
    index_context = nullptr;
    
    return 0;
}

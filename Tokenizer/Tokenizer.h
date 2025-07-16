#ifndef TOKENIZER_H__
#define TOKENIZER_H__

#include <vector>
#include <string>

class Tokenizer
{
    public:
        Tokenizer() = default;
        virtual std::vector<std::string> Tokenize(const char * text) = 0; 
        virtual ~Tokenizer() = default;
};

class SmartTokenizer : public Tokenizer
{
    public:
        SmartTokenizer() = default;
        std::vector<std::string> Tokenize(const char * text) override;
        ~SmartTokenizer() = default;
};
#endif
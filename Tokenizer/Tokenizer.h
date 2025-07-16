#ifndef TOKENIZER_H__
#define TOKENIZER_H__

#include <vector>
#include <string>

class Tokenizer
{
    public:
        virtual std::vector<std::string> Tokenize(const char * text); 
};

class SmartTokenizer : public Tokenizer
{
    public:
        std::vector<std::string> Tokenize(const char * text) override;
};
#endif
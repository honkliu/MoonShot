#ifndef TOKENIZER_H__
#define TOKENIZER_H__

class Tokenizer
{
    public:
        virtual void Tokenize(); 
};

class SmartTokenizer : Tokenizer
{
    public:
        void Tokenize();
}
#endif
#ifndef TOKENIZER_H__
#define TOKENIZER_H__

#include <vector>
#include <string>
#include <memory>       // For shared_ptr
#include <stdexcept>    // For runtime_error

#include <unicode/unistr.h>
#include <unicode/brkiter.h>

// Avoid "using namespace" in header files (especially global namespaces)
class Tokenizer
{
    public:
        explicit Tokenizer() = default;
        virtual std::vector<std::string> Tokenize(const char * text) = 0; 
        virtual ~Tokenizer() = default;
};

class SmartTokenizer : public Tokenizer
{
    public:
        explicit SmartTokenizer(const icu::Locale& locale = icu::Locale::getEnglish());
        std::vector<std::string> Tokenize(const char * text) override;
        ~SmartTokenizer();

    private:
        icu::Locale m_Locale;
        std::shared_ptr<icu::BreakIterator> m_WordBreaker;
};

#endif // TOKENIZER_H__
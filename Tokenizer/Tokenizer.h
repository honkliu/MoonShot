#ifndef TOKENIZER_H__
#define TOKENIZER_H__

#include <vector>
#include <string>
#include <memory>

#include <unicode/unistr.h>
#include <unicode/brkiter.h>
#include <unicode/normalizer2.h>

class Tokenizer
{
    public:
        explicit Tokenizer() = default;
        virtual std::vector<std::string> Tokenize(const char* text) = 0;
        virtual ~Tokenizer() = default;
};

/*
* SmartTokenizer — ICU-based Unicode word segmentation.
* Handles all scripts: Latin, CJK, Arabic, Devanagari, Cyrillic, etc.
* NFC-normalises input; lowercases via locale-aware toLower.
*/
class SmartTokenizer : public Tokenizer
{
    public:
        explicit SmartTokenizer(const icu::Locale& locale = icu::Locale::getEnglish());
        std::vector<std::string> Tokenize(const char* text) override;
        ~SmartTokenizer();

    private:
        icu::Locale                         m_Locale;
        std::shared_ptr<icu::BreakIterator> m_WordBreaker;
        const icu::Normalizer2*             m_Normalizer = nullptr;
};

#endif // TOKENIZER_H__

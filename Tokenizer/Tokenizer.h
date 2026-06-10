#ifndef TOKENIZER_H__
#define TOKENIZER_H__

#include <vector>
#include <string>
#include <memory>
#include <cctype>
#include <stdexcept>

#ifdef HAVE_ICU
#include <unicode/unistr.h>
#include <unicode/brkiter.h>
#endif

class Tokenizer
{
    public:
        explicit Tokenizer() = default;
        virtual std::vector<std::string> Tokenize(const char * text) = 0;
        virtual ~Tokenizer() = default;
};

/*
* SimpleTokenizer — splits on non-alphanumeric characters and lowercases.
* Does not require ICU; handles ASCII and basic Latin Unicode via std::isalnum.
*/
class SimpleTokenizer : public Tokenizer
{
    public:
        std::vector<std::string> Tokenize(const char * text) override
        {
            std::vector<std::string> tokens;
            if (!text) return tokens;

            std::string current;
            for (const char* p = text; *p; ++p) {
                unsigned char c = static_cast<unsigned char>(*p);
                if (std::isalnum(c)) {
                    current += static_cast<char>(std::tolower(c));
                } else if (!current.empty()) {
                    if (current.size() <= 64)
                        tokens.push_back(current);
                    current.clear();
                }
            }
            if (!current.empty() && current.size() <= 64)
                tokens.push_back(current);
            return tokens;
        }
};

#ifdef HAVE_ICU

/*
* SmartTokenizer — ICU-based word segmentation with Unicode normalisation.
* Only available when the project is built with HAVE_ICU defined.
*/
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

#else

/*
* When ICU is not available SmartTokenizer is an alias for SimpleTokenizer
* so the rest of the codebase compiles unchanged.
*/
class SmartTokenizer : public SimpleTokenizer
{
    public:
        SmartTokenizer() = default;
        /*
        * Accept (and ignore) a locale argument to match the ICU signature.
        */
        template<typename T>
        explicit SmartTokenizer(const T&) {}
};

#endif // HAVE_ICU

#endif // TOKENIZER_H__

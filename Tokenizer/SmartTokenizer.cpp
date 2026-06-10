#include "Tokenizer.h"

#ifdef HAVE_ICU

#include <cctype>
#include <unicode/normlzr.h>
#include <unicode/unistr.h>
#include <stdexcept>

using namespace std;
using namespace icu;

SmartTokenizer::SmartTokenizer(const Locale& locale)
    : m_Locale(locale)
{
    UErrorCode status = U_ZERO_ERROR;
    BreakIterator* breaker = BreakIterator::createWordInstance(m_Locale, status);

    if (U_FAILURE(status) || !breaker) {
        throw runtime_error("Failed to create word break iterator");
    }

    m_WordBreaker = shared_ptr<BreakIterator>(breaker);
}

SmartTokenizer::~SmartTokenizer() {
}

vector<string> SmartTokenizer::Tokenize(const char* text)
{
    if (!text || *text == '\0') {
        return {};
    }

    UErrorCode status = U_ZERO_ERROR;
    vector<string> tokens;

    UnicodeString unicode_text = UnicodeString::fromUTF8(text);

    UnicodeString normalized_text;
    Normalizer::normalize(unicode_text, UNORM_NFC, 0, normalized_text, status);
    if (U_FAILURE(status)) {
        normalized_text = unicode_text;
        status = U_ZERO_ERROR;
    }

    m_WordBreaker->setText(normalized_text);

    int32_t start = m_WordBreaker->first();
    int32_t end = m_WordBreaker->next();

    while (end != BreakIterator::DONE) {
        int32_t rule_status = m_WordBreaker->getRuleStatus();

        if (rule_status != UBRK_WORD_NONE) {
            UnicodeString word = normalized_text.tempSubStringBetween(start, end);

            word.toLower(m_Locale);

            string utf8_word;
            word.toUTF8String(utf8_word);

            if (!utf8_word.empty()) {
                tokens.push_back(move(utf8_word));
            }
        }

        start = end;
        end = m_WordBreaker->next();
    }

    return tokens;
}

#endif // HAVE_ICU

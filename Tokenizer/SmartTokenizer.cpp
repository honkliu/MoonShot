#include "Tokenizer.h"

#include <stdexcept>
#include <unicode/normalizer2.h>
#include <unicode/unistr.h>
#include <unicode/uchar.h>

using namespace std;
using namespace icu;

static bool IsIndexableToken(const string& utf8)
{
    if (utf8.empty() || utf8.size() > 64)
        return false;

    for (unsigned char ch : utf8) {
        if (ch < 0x20 || ch == 0x7f)
            return false;
    }

    UErrorCode status = U_ZERO_ERROR;
    UnicodeString u = UnicodeString::fromUTF8(StringPiece(utf8));
    if (U_FAILURE(status))
        return false;

    for (int32_t i = 0; i < u.length();) {
        UChar32 ch = u.char32At(i);
        i += U16_LENGTH(ch);
        UCharCategory cat = static_cast<UCharCategory>(u_charType(ch));
        if (cat == U_CONTROL_CHAR ||
            cat == U_FORMAT_CHAR ||
            cat == U_PRIVATE_USE_CHAR ||
            cat == U_SURROGATE ||
            cat == U_UNASSIGNED) {
            return false;
        }
    }

    return true;
}

SmartTokenizer::SmartTokenizer(const Locale& locale)
    : m_Locale(locale)
{
    UErrorCode status = U_ZERO_ERROR;

    m_Normalizer = Normalizer2::getNFCInstance(status);
    if (U_FAILURE(status) || !m_Normalizer)
        throw runtime_error("Failed to get NFC normalizer");

    BreakIterator* breaker = BreakIterator::createWordInstance(m_Locale, status);
    if (U_FAILURE(status) || !breaker)
        throw runtime_error("Failed to create ICU word break iterator");

    m_WordBreaker = shared_ptr<BreakIterator>(breaker);
}

SmartTokenizer::~SmartTokenizer() {}

vector<string> SmartTokenizer::Tokenize(const char* text)
{
    if (!text || !*text)
        return {};

    UErrorCode status = U_ZERO_ERROR;

    UnicodeString utext      = UnicodeString::fromUTF8(text);
    UnicodeString normalized = m_Normalizer->normalize(utext, status);
    if (U_FAILURE(status))
        normalized = utext;

    /* clone per call — shares rule tables, only iterator state copied */
    unique_ptr<BreakIterator> breaker(m_WordBreaker->clone());
    breaker->setText(normalized);

    vector<string> tokens;
    int32_t start = breaker->first();
    int32_t end   = breaker->next();

    while (end != BreakIterator::DONE) {
        if (breaker->getRuleStatus() != UBRK_WORD_NONE) {
            UnicodeString word = normalized.tempSubStringBetween(start, end);
            word.toLower(m_Locale);

            string utf8;
            word.toUTF8String(utf8);

            if (IsIndexableToken(utf8))
                tokens.push_back(move(utf8));
        }
        start = end;
        end   = breaker->next();
    }

    return tokens;
}

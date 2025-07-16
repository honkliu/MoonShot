#include "Tokenizer.h"
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
    // Shared_ptr will automatically handle cleanup
}

vector<string> SmartTokenizer::Tokenize(const char* text)
{
    if (!text || *text == '\0') {
        return {};
    }
    
    UErrorCode status = U_ZERO_ERROR;
    vector<string> tokens;
    
    // Convert input text to ICU UnicodeString
    UnicodeString unicode_text = UnicodeString::fromUTF8(text);
    
    // Normalize text to NFC form
    UnicodeString normalized_text;
    Normalizer::normalize(unicode_text, UNORM_NFC, 0, normalized_text, status);
    if (U_FAILURE(status)) {
        normalized_text = unicode_text; // Fallback to original
        status = U_ZERO_ERROR;
    }
    
    // Set text for word breaking
    m_WordBreaker->setText(normalized_text);
    
    // Process text using word breaker
    int32_t start = m_WordBreaker->first();
    int32_t end = m_WordBreaker->next();
    
    while (end != BreakIterator::DONE) {
        // Get rule status to determine word boundaries
        int32_t rule_status = m_WordBreaker->getRuleStatus();
        
        // Only process words (skip spaces, punctuation, etc.)
        if (rule_status != UBRK_WORD_NONE) {
            // Extract word substring
            UnicodeString word = normalized_text.tempSubStringBetween(start, end);
            
            // Convert to lowercase
            word.toLower(m_Locale);
            
            // Convert to UTF-8
            string utf8_word;
            word.toUTF8String(utf8_word);
            
            // Add to tokens if not empty
            if (!utf8_word.empty()) {
                tokens.push_back(move(utf8_word));
            }
        }
        
        // Move to next boundary
        start = end;
        end = m_WordBreaker->next();
    }
    
    return tokens;
}
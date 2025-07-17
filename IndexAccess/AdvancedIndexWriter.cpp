#include "AdvancedIndexWriter.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

using namespace std;

void AdvancedIndexWriter::Write(vector<string>&& words, uint64_t documentId, const char * postingType) {
    // Example implementation: print the tokens, documentId, and posting type
    cout << "AdvancedIndexWriter::Write called with documentId: " << documentId << ", posting: " << postingType << endl;
    
    sort(words.begin(), words.end());
    for (const auto& token : words) {
        cout << "Token: " << token << endl;
    }
}

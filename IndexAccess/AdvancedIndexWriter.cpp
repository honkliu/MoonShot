#include "AdvancedIndexWriter.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstdint>

using namespace std;

void AdvancedIndexWriter::Write(const vector<string>& words, uint64_t documentId, PostingType posting) {
    // Example implementation: print the tokens, documentId, and posting type
    cout << "AdvancedIndexWriter::Write called with documentId: " << documentId << ", posting: " << static_cast<int>(posting) << endl;
    for (const auto& token : words) {
        cout << "Token: " << token << endl;
    }
}

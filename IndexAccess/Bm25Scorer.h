#ifndef BM25SCORER_H__
#define BM25SCORER_H__

#include <cstdint>
#include <cmath>
#include <algorithm>

/*
* Okapi BM25 relevance scorer.
*
*   IDF(t)       = ln( (N - df + 0.5) / (df + 0.5) + 1 )
*   TF_norm(t,d) = tf * (k1+1) / (tf + k1*(1 - b + b*dl/avgdl))
*
* Default parameters: k1 = 1.2, b = 0.75
*/
class Bm25Scorer {
public:
    Bm25Scorer(uint64_t num_docs, float avg_doc_len,
               float k1 = 1.2f, float b = 0.75f)
        : k1_(k1), b_(b)
        , n_(static_cast<float>(num_docs > 0 ? num_docs : 1))
        , avg_dl_(avg_doc_len > 0.0f ? avg_doc_len : 1.0f)
    {}

    float Idf(uint32_t doc_freq) const
    {
        float df = static_cast<float>(std::max(1u, doc_freq));
        return std::max(0.0f,
            std::log((n_ - df + 0.5f) / (df + 0.5f) + 1.0f));
    }

    float TfNorm(uint32_t tf, uint32_t doc_len) const
    {
        float f  = static_cast<float>(tf);
        float dl = static_cast<float>(std::max(1u, doc_len));
        return f * (k1_ + 1.0f) /
               (f + k1_ * (1.0f - b_ + b_ * dl / avg_dl_));
    }

    float Score(uint32_t tf, uint32_t doc_len, uint32_t doc_freq) const
    {
        return Idf(doc_freq) * TfNorm(tf, doc_len);
    }

private:
    float k1_, b_, n_, avg_dl_;
};

#endif

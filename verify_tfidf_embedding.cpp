/* Quick syntax check for TFIDFSemanticEmbedding */
#include <vector>
#include <string>
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <memory>

class IEmbeddingModel {
public:
    virtual ~IEmbeddingModel() = default;
    virtual std::vector<float> Embed(const std::vector<std::string>& tokens) const = 0;
    virtual size_t GetDimension() const { return 512; }
};

class TFIDFSemanticEmbedding : public IEmbeddingModel {
public:
    explicit TFIDFSemanticEmbedding(size_t dim = 512) : m_Dim(dim) {}

    std::vector<float> Embed(const std::vector<std::string>& tokens) const override
    {
        std::vector<float> result(m_Dim, 0.0f);
        if (tokens.empty())
            return result;

        std::unordered_map<std::string, size_t> tokenFreq;
        for (const auto& token : tokens) {
            if (!token.empty()) {
                tokenFreq[token]++;
            }
        }

        for (const auto& [token, freq] : tokenFreq) {
            if (token.empty()) continue;
            size_t slot = GetSlotForToken(token);
            float tfWeight = 1.0f + std::log(1.0f + static_cast<float>(freq));
            float idfAdjust = 1.0f + (token.length() > 0 ? std::log(1.0f + 3.0f / token.length()) : 1.0f);
            result[slot] += tfWeight * idfAdjust;
        }

        float norm = 0.0f;
        for (float v : result) norm += v * v;
        if (norm > 0.0f) {
            norm = std::sqrt(norm);
            for (float& v : result) v /= norm;
        }
        return result;
    }

    size_t GetDimension() const override { return m_Dim; }

private:
    size_t m_Dim;

    size_t GetSlotForToken(const std::string_view& token) const
    {
        uint64_t h = 14695981039346656037ull;
        for (unsigned char ch : token) {
            h ^= ch;
            h *= 1099511628211ull;
        }
        return static_cast<size_t>(h % m_Dim);
    }
};

int main() {
    auto model = std::make_shared<TFIDFSemanticEmbedding>(512);
    
    std::vector<std::string> tokens = {"search", "engine", "production", "semantic"};
    auto vec = model->Embed(tokens);
    
    // Verify normalization
    float norm = 0.0f;
    for (float v : vec) norm += v * v;
    norm = std::sqrt(norm);
    
    return (norm > 0.99f && norm < 1.01f) ? 0 : 1;
}

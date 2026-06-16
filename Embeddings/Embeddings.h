#ifndef EMBEDDINGS_H__
#define EMBEDDINGS_H__

/*
* All files are used for internal only
*
* Author: honkliu@hotmail.com
*/

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

enum class VectorMetric {
    Cosine,
    DotProduct,
    L2,
};

struct VectorSearchResult {
    uint64_t doc_id = 0;
    float score = 0.0f;
};

inline std::string VectorSidecarPath(const std::string& indexPath)
{
    return indexPath + ".v.index";
}

inline float DotProduct(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min(a.size(), b.size());
    float value = 0.0f;
    for (size_t i = 0; i < n; ++i) value += a[i] * b[i];
    return value;
}

inline float L2Squared(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min(a.size(), b.size());
    float value = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float delta = a[i] - b[i];
        value += delta * delta;
    }
    return value;
}

inline float CosineSimilarity(const std::vector<float>& a, const std::vector<float>& b)
{
    const size_t n = std::min(a.size(), b.size());
    float dot = 0.0f;
    float na = 0.0f;
    float nb = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na <= 0.0f || nb <= 0.0f) return 0.0f;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

inline float VectorScore(const std::vector<float>& query,
                         const std::vector<float>& doc,
                         VectorMetric metric = VectorMetric::Cosine)
{
    switch (metric) {
    case VectorMetric::DotProduct:
        return DotProduct(query, doc);
    case VectorMetric::L2:
        return 1.0f / (1.0f + L2Squared(query, doc));
    case VectorMetric::Cosine:
    default:
        return CosineSimilarity(query, doc);
    }
}

class FreshDiskAnnVectorIndex {
public:
    struct Node {
        uint64_t doc_id = 0;
        std::vector<float> vector;
        size_t level = 0;
        std::vector<std::vector<size_t>> neighbors;
    };

    explicit FreshDiskAnnVectorIndex(size_t maxNeighbors = 32,
                                     size_t efConstruction = 200)
        : m_MaxNeighbors(std::max<size_t>(maxNeighbors, 2))
        , m_EfConstruction(std::max<size_t>(efConstruction, maxNeighbors))
    {}

    void Clear()
    {
        m_Dim = 0;
        m_Nodes.clear();
        m_DocToNode.clear();
        m_EntryPoint = npos();
        m_MaxLevel = 0;
    }

    bool Add(uint64_t docId, std::vector<float> vector)
    {
        if (docId == 0 || vector.empty()) return false;
        if (m_Dim == 0) m_Dim = vector.size();
        if (vector.size() != m_Dim) return false;

        auto existing = m_DocToNode.find(docId);
        if (existing != m_DocToNode.end()) {
            m_Nodes[existing->second].vector = std::move(vector);
            return true;
        }

        Node node;
        node.doc_id = docId;
        node.vector = std::move(vector);
        node.level = RandomLevel(docId);
        node.neighbors.resize(node.level + 1);

        const size_t newIndex = m_Nodes.size();
        m_Nodes.push_back(std::move(node));
        m_DocToNode[docId] = newIndex;

        if (m_EntryPoint == npos()) {
            m_EntryPoint = newIndex;
            m_MaxLevel = m_Nodes[newIndex].level;
            return true;
        }

        size_t entry = m_EntryPoint;
        for (int level = static_cast<int>(m_MaxLevel); level > static_cast<int>(m_Nodes[newIndex].level); --level)
            entry = GreedySearchLayer(m_Nodes[newIndex].vector, entry, static_cast<size_t>(level));

        const size_t top = std::min(m_MaxLevel, m_Nodes[newIndex].level);
        for (int level = static_cast<int>(top); level >= 0; --level) {
            auto candidates = SearchLayer(m_Nodes[newIndex].vector, entry, m_EfConstruction, static_cast<size_t>(level));
            auto selected = SelectNeighbors(candidates, m_MaxNeighbors);
            m_Nodes[newIndex].neighbors[static_cast<size_t>(level)] = selected;

            for (size_t neighbor : selected)
                LinkBack(neighbor, newIndex, static_cast<size_t>(level));

            if (!candidates.empty())
                entry = candidates.front().second;
        }

        if (m_Nodes[newIndex].level > m_MaxLevel) {
            m_EntryPoint = newIndex;
            m_MaxLevel = m_Nodes[newIndex].level;
        }

        return true;
    }

    const std::vector<float>* Get(uint64_t docId) const
    {
        auto it = m_DocToNode.find(docId);
        return it == m_DocToNode.end() ? nullptr : &m_Nodes[it->second].vector;
    }

    std::vector<VectorSearchResult> Search(const std::vector<float>& query,
                                           size_t topK = 20,
                                           VectorMetric metric = VectorMetric::Cosine,
                                           size_t efSearch = 200) const
    {
        std::vector<VectorSearchResult> results;
        if (query.empty() || m_Nodes.empty() || (m_Dim != 0 && query.size() != m_Dim)) return results;

        size_t entry = m_EntryPoint;
        for (int level = static_cast<int>(m_MaxLevel); level > 0; --level)
            entry = GreedySearchLayer(query, entry, static_cast<size_t>(level), metric);

        const size_t wanted = topK == 0 ? m_Nodes.size() : topK;
        auto candidates = SearchLayer(query, entry, std::max(efSearch, wanted), 0, metric);

        if (topK == 0 && candidates.size() < m_Nodes.size()) {
            std::unordered_set<size_t> seen;
            for (const auto& c : candidates) seen.insert(c.second);
            for (size_t i = 0; i < m_Nodes.size(); ++i)
                if (!seen.count(i))
                    candidates.push_back({VectorScore(query, m_Nodes[i].vector, metric), i});
        }

        results.reserve(candidates.size());
        for (const auto& candidate : candidates)
            results.push_back({m_Nodes[candidate.second].doc_id,
                               VectorScore(query, m_Nodes[candidate.second].vector, metric)});

        std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.doc_id < b.doc_id;
        });
        if (topK > 0 && results.size() > topK) results.resize(topK);
        return results;
    }

    size_t Dimension() const { return m_Dim; }
    size_t Size() const { return m_Nodes.size(); }
    bool Empty() const { return m_Nodes.empty(); }
    size_t MaxLevel() const { return m_MaxLevel; }
    size_t MaxNeighbors() const { return m_MaxNeighbors; }
    size_t EfConstruction() const { return m_EfConstruction; }
    const std::vector<Node>& Nodes() const { return m_Nodes; }

    const std::unordered_map<uint64_t, std::vector<float>>& AllVectors() const
    {
        m_VectorView.clear();
        m_VectorView.reserve(m_Nodes.size());
        for (const auto& node : m_Nodes)
            m_VectorView.emplace(node.doc_id, node.vector);
        return m_VectorView;
    }

    void LoadNodes(size_t dim,
                   size_t maxNeighbors,
                   size_t efConstruction,
                   std::vector<Node> nodes,
                   size_t entryPoint,
                   size_t maxLevel)
    {
        m_Dim = dim;
        m_MaxNeighbors = std::max<size_t>(maxNeighbors, 2);
        m_EfConstruction = std::max<size_t>(efConstruction, m_MaxNeighbors);
        m_Nodes = std::move(nodes);
        m_DocToNode.clear();
        for (size_t i = 0; i < m_Nodes.size(); ++i)
            m_DocToNode[m_Nodes[i].doc_id] = i;
        m_EntryPoint = entryPoint < m_Nodes.size() ? entryPoint : (m_Nodes.empty() ? npos() : 0);
        m_MaxLevel = maxLevel;
    }

private:
    static size_t npos() { return std::numeric_limits<size_t>::max(); }

    size_t RandomLevel(uint64_t docId) const
    {
        uint64_t hash = docId * 11400714819323198485ull + 0x9e3779b97f4a7c15ull;
        size_t level = 0;
        while ((hash & 0x3ull) == 0 && level < 16) {
            ++level;
            hash >>= 2;
        }
        return level;
    }

    float Score(size_t nodeIndex, const std::vector<float>& query, VectorMetric metric = VectorMetric::Cosine) const
    {
        return VectorScore(query, m_Nodes[nodeIndex].vector, metric);
    }

    size_t GreedySearchLayer(const std::vector<float>& query,
                             size_t entry,
                             size_t level,
                             VectorMetric metric = VectorMetric::Cosine) const
    {
        bool changed = true;
        size_t best = entry;
        float bestScore = Score(best, query, metric);
        while (changed) {
            changed = false;
            if (level >= m_Nodes[best].neighbors.size()) break;
            for (size_t neighbor : m_Nodes[best].neighbors[level]) {
                float score = Score(neighbor, query, metric);
                if (score > bestScore) {
                    best = neighbor;
                    bestScore = score;
                    changed = true;
                }
            }
        }
        return best;
    }

    std::vector<std::pair<float, size_t>> SearchLayer(const std::vector<float>& query,
                                                      size_t entry,
                                                      size_t ef,
                                                      size_t level,
                                                      VectorMetric metric = VectorMetric::Cosine) const
    {
        using Candidate = std::pair<float, size_t>;
        auto bestFirst = [](const Candidate& a, const Candidate& b) { return a.first < b.first; };
        auto worstFirst = [](const Candidate& a, const Candidate& b) { return a.first > b.first; };
        std::priority_queue<Candidate, std::vector<Candidate>, decltype(bestFirst)> candidates(bestFirst);
        std::priority_queue<Candidate, std::vector<Candidate>, decltype(worstFirst)> results(worstFirst);
        std::unordered_set<size_t> visited;

        float entryScore = Score(entry, query, metric);
        candidates.push({entryScore, entry});
        results.push({entryScore, entry});
        visited.insert(entry);

        while (!candidates.empty()) {
            auto current = candidates.top();
            candidates.pop();
            if (!results.empty() && current.first < results.top().first)
                break;
            if (level >= m_Nodes[current.second].neighbors.size()) continue;
            for (size_t neighbor : m_Nodes[current.second].neighbors[level]) {
                if (!visited.insert(neighbor).second) continue;
                float score = Score(neighbor, query, metric);
                if (results.size() < ef || score > results.top().first) {
                    candidates.push({score, neighbor});
                    results.push({score, neighbor});
                    if (results.size() > ef) results.pop();
                }
            }
        }

        std::vector<Candidate> out;
        while (!results.empty()) {
            out.push_back(results.top());
            results.pop();
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
        return out;
    }

    std::vector<size_t> SelectNeighbors(const std::vector<std::pair<float, size_t>>& candidates,
                                        size_t maxNeighbors) const
    {
        std::vector<std::pair<float, size_t>> sorted = candidates;
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });
        std::vector<size_t> out;
        for (const auto& candidate : sorted) {
            if (out.size() >= maxNeighbors) break;
            out.push_back(candidate.second);
        }
        return out;
    }

    void LinkBack(size_t node, size_t neighbor, size_t level)
    {
        while (m_Nodes[node].neighbors.size() <= level)
            m_Nodes[node].neighbors.emplace_back();
        auto& links = m_Nodes[node].neighbors[level];
        if (std::find(links.begin(), links.end(), neighbor) == links.end())
            links.push_back(neighbor);
        if (links.size() > m_MaxNeighbors) {
            const auto& query = m_Nodes[node].vector;
            std::sort(links.begin(), links.end(), [&](size_t a, size_t b) {
                float sa = VectorScore(query, m_Nodes[a].vector);
                float sb = VectorScore(query, m_Nodes[b].vector);
                if (sa != sb) return sa > sb;
                return a < b;
            });
            links.resize(m_MaxNeighbors);
        }
    }

    size_t m_Dim = 0;
    size_t m_MaxNeighbors = 32;
    size_t m_EfConstruction = 200;
    std::vector<Node> m_Nodes;
    std::unordered_map<uint64_t, size_t> m_DocToNode;
    size_t m_EntryPoint = npos();
    size_t m_MaxLevel = 0;
    mutable std::unordered_map<uint64_t, std::vector<float>> m_VectorView;
};

inline uint64_t HashTokenForEmbedding(std::string_view token)
{
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char ch : token) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::vector<float> BuildHashedEmbedding(const std::vector<std::string>& tokens,
                                               size_t dim = 128)
{
    std::vector<float> vector(dim, 0.0f);
    if (dim == 0) return vector;
    for (const auto& token : tokens) {
        if (token.empty()) continue;
        uint64_t hash = HashTokenForEmbedding(token);
        size_t slot = static_cast<size_t>(hash % dim);
        float sign = (hash & (1ull << 63)) ? -1.0f : 1.0f;
        vector[slot] += sign;
    }

    float norm = 0.0f;
    for (float value : vector) norm += value * value;
    if (norm > 0.0f) {
        norm = std::sqrt(norm);
        for (float& value : vector) value /= norm;
    }
    return vector;
}

inline bool SaveVectorSidecar(const std::string& vectorPath,
                              const std::unordered_map<uint64_t, std::vector<float>>& vectors)
{
    FreshDiskAnnVectorIndex index;
    std::vector<std::pair<uint64_t, std::vector<float>>> ordered(vectors.begin(), vectors.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [docId, vector] : ordered)
        index.Add(docId, std::move(vector));

    std::ofstream out(vectorPath, std::ios::binary);
    if (!out) return false;

    uint8_t magic[8] = {'M','O','O','N','H','N','S','W'};
    uint32_t version = 2;
    uint32_t dim = static_cast<uint32_t>(index.Dimension());
    uint32_t maxNeighbors = static_cast<uint32_t>(index.MaxNeighbors());
    uint32_t efConstruction = static_cast<uint32_t>(index.EfConstruction());
    uint64_t count = static_cast<uint64_t>(index.Nodes().size());
    uint64_t entryPoint = 0;
    uint32_t maxLevel = static_cast<uint32_t>(index.MaxLevel());

    out.write(reinterpret_cast<const char*>(magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&dim), sizeof(dim));
    out.write(reinterpret_cast<const char*>(&maxNeighbors), sizeof(maxNeighbors));
    out.write(reinterpret_cast<const char*>(&efConstruction), sizeof(efConstruction));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    out.write(reinterpret_cast<const char*>(&entryPoint), sizeof(entryPoint));
    out.write(reinterpret_cast<const char*>(&maxLevel), sizeof(maxLevel));

    for (const auto& node : index.Nodes()) {
        uint64_t docId = node.doc_id;
        uint32_t level = static_cast<uint32_t>(node.level);
        out.write(reinterpret_cast<const char*>(&docId), sizeof(docId));
        out.write(reinterpret_cast<const char*>(&level), sizeof(level));
        out.write(reinterpret_cast<const char*>(node.vector.data()), static_cast<std::streamsize>(dim * sizeof(float)));
        for (uint32_t l = 0; l <= level; ++l) {
            uint32_t links = l < node.neighbors.size() ? static_cast<uint32_t>(node.neighbors[l].size()) : 0;
            out.write(reinterpret_cast<const char*>(&links), sizeof(links));
            for (size_t neighbor : node.neighbors[l]) {
                uint64_t n = static_cast<uint64_t>(neighbor);
                out.write(reinterpret_cast<const char*>(&n), sizeof(n));
            }
        }
    }
    return static_cast<bool>(out);
}

inline bool LoadVectorSidecar(const std::string& vectorPath, FreshDiskAnnVectorIndex& index)
{
    std::ifstream in(vectorPath, std::ios::binary);
    if (!in) return false;

    uint8_t magic[8] = {};
    in.read(reinterpret_cast<char*>(magic), sizeof(magic));
    const uint8_t hnswMagic[8] = {'M','O','O','N','H','N','S','W'};
    const uint8_t flatMagic[8] = {'M','O','O','N','V','E','C','1'};

    if (std::memcmp(magic, hnswMagic, sizeof(hnswMagic)) == 0) {
        uint32_t version = 0, dim = 0, maxNeighbors = 0, efConstruction = 0, maxLevel = 0;
        uint64_t count = 0, entryPoint = 0;
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
        in.read(reinterpret_cast<char*>(&maxNeighbors), sizeof(maxNeighbors));
        in.read(reinterpret_cast<char*>(&efConstruction), sizeof(efConstruction));
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        in.read(reinterpret_cast<char*>(&entryPoint), sizeof(entryPoint));
        in.read(reinterpret_cast<char*>(&maxLevel), sizeof(maxLevel));
        if (!in || version != 2 || dim == 0) return false;

        std::vector<FreshDiskAnnVectorIndex::Node> nodes;
        nodes.resize(static_cast<size_t>(count));
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t docId = 0;
            uint32_t level = 0;
            in.read(reinterpret_cast<char*>(&docId), sizeof(docId));
            in.read(reinterpret_cast<char*>(&level), sizeof(level));
            nodes[static_cast<size_t>(i)].doc_id = docId;
            nodes[static_cast<size_t>(i)].level = level;
            nodes[static_cast<size_t>(i)].vector.resize(dim);
            nodes[static_cast<size_t>(i)].neighbors.resize(static_cast<size_t>(level) + 1);
            in.read(reinterpret_cast<char*>(nodes[static_cast<size_t>(i)].vector.data()), static_cast<std::streamsize>(dim * sizeof(float)));
            for (uint32_t l = 0; l <= level; ++l) {
                uint32_t links = 0;
                in.read(reinterpret_cast<char*>(&links), sizeof(links));
                nodes[static_cast<size_t>(i)].neighbors[l].resize(links);
                for (uint32_t j = 0; j < links; ++j) {
                    uint64_t n = 0;
                    in.read(reinterpret_cast<char*>(&n), sizeof(n));
                    nodes[static_cast<size_t>(i)].neighbors[l][j] = static_cast<size_t>(n);
                }
            }
            if (!in) return false;
        }
        index.LoadNodes(dim, maxNeighbors, efConstruction, std::move(nodes), static_cast<size_t>(entryPoint), maxLevel);
        return true;
    }

    if (std::memcmp(magic, flatMagic, sizeof(flatMagic)) == 0) {
        uint32_t version = 0, dim = 0;
        uint64_t count = 0;
        in.read(reinterpret_cast<char*>(&version), sizeof(version));
        in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!in || version != 1 || dim == 0) return false;
        index.Clear();
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t docId = 0;
            std::vector<float> vector(dim);
            in.read(reinterpret_cast<char*>(&docId), sizeof(docId));
            in.read(reinterpret_cast<char*>(vector.data()), static_cast<std::streamsize>(dim * sizeof(float)));
            if (!in) return false;
            index.Add(docId, std::move(vector));
        }
        return true;
    }

    return false;
}

#endif

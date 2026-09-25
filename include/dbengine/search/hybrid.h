#pragma once

// Hybrid Search: BM25-lite (Token-Hash) + dense (extern, s08-unabhaengig) + Fusion.
// - Tokenisierung: lowercase, alphanumerisch, Split auf Rest.
// - Term-Repr: FNV-1a 64-bit Hash (deterministisch, plattformstabil).
// - BM25-lite: IDF = ln(1 + (N - df + 0.5) / (df + 0.5)), k1=1.2, b=0.75 default.
// - dense: wird als Parameter uebergeben (float pro Doc-ID), kein s08-Include.
// - Fusion: Weighted (w_dense*norm_dense + w_bm25*norm_bm25, min-max-normiert)
//            oder RRF (w/(k+rank), k=60 default).
// - Top-K via Min-Heap, deterministischer Tie-Break (Score desc, ID asc).

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dbengine::search {

uint64_t tokenHash(std::string_view tok);
std::vector<std::string> tokenize(std::string_view text);
std::vector<uint64_t> tokenizeHashed(std::string_view text);

enum class FusionMode { Weighted, RRF };

struct HybridConfig {
  double k1 = 1.2;
  double b = 0.75;
  double w_dense = 0.5;
  double w_bm25 = 0.5;
  FusionMode mode = FusionMode::Weighted;
  int rrf_k = 60;
};

struct DenseScore {
  uint32_t id = 0;
  float score = 0.0f;
};

struct HybridHit {
  uint32_t id = 0;
  double fused = 0.0;
  double bm25 = 0.0;
  double dense = 0.0;
  bool has_dense = false;
};

class HybridIndex {
 public:
  explicit HybridIndex(HybridConfig cfg = {});

  void setConfig(const HybridConfig& cfg);
  const HybridConfig& config() const;

  // Dokument hinzufuegen/ersetzen. Danach ist build() faellig (wird lazy nachgeholt).
  void addDocument(uint32_t id, const std::string& text);
  void clear();

  // DF/avgdl neu berechnen. search()/bm25* rufen das bei Bedarf automatisch auf.
  void build() const;

  size_t size() const;
  double avgdl() const;
  double idf(uint64_t term) const;

  // BM25-Score eines Docs fuer bereits gehashte, deduplizierte Query-Terme.
  double bm25(uint32_t id, const std::vector<uint64_t>& qterms) const;

  // Reine BM25 Top-K Suche (absteigend, Tie-Break ID aufsteigend).
  std::vector<std::pair<uint32_t, double>> bm25Search(std::string_view query,
                                                      size_t topK) const;

  // Hybrid-Suche. dense kommt vom Caller (z.B. s08-HNSW oder Stub im Test).
  std::vector<HybridHit> search(std::string_view query,
                                const std::vector<DenseScore>& dense,
                                size_t topK) const;
  std::vector<HybridHit> search(std::string_view query,
                                const std::unordered_map<uint32_t, float>& dense,
                                size_t topK) const;

 private:
  struct DocEntry {
    std::string text;
    std::unordered_map<uint64_t, uint32_t> tf;
    size_t len = 0;
  };

  static std::vector<uint64_t> uniqueTerms(std::vector<uint64_t> v);

  void ensureBuilt() const;

  HybridConfig cfg_;
  std::unordered_map<uint32_t, DocEntry> docs_;
  mutable std::unordered_map<uint64_t, size_t> df_;
  mutable size_t totalLen_ = 0;
  mutable double avgdl_ = 0.0;
  mutable bool dirty_ = true;
};

}  // namespace dbengine::search

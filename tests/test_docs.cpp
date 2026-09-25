// s20-docs: Doku/Ops-Guard (STL-only, kein Build-Bruch).
// Prueft Existenz + Nicht-Leer + Markdown-Titel fuer:
//   docs/ARCH.md, docs/RUNBOOK.md, k8s/statefulset.yaml, SBOM.md (Fallback docs/SBOM.md)
// Pfadaufloesung: DBENGINE_SOURCE_DIR (CMake) -> CWD-Fallbacks (build/ + root).
// YAML braucht keinen Markdown-Titel, aber Replica/PVC-Keywords.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

#ifndef DBENGINE_SOURCE_DIR
#define DBENGINE_SOURCE_DIR "."
#endif

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
  if (cond) {
    std::cout << "  ok: " << what << "\n";
  } else {
    std::cout << "  FAIL: " << what << "\n";
    ++failures;
  }
}

fs::path resolve(const std::string& rel) {
  // 1. Compile-time Source-Dir (robust gegen CWD=build).
  {
    fs::path p = fs::path(DBENGINE_SOURCE_DIR) / rel;
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
  }
  // 2. CWD-Fallbacks: ., .., ../.. (ctest CWD = build).
  for (const char* base : {".", "..", "../..", "./.."}) {
    fs::path p = fs::path(base) / rel;
    std::error_code ec;
    if (fs::exists(p, ec)) return p;
  }
  // 3. Absoluter Fallback relativ zu CWD (fuer Fehlermeldung).
  return fs::path(DBENGINE_SOURCE_DIR) / rel;
}

std::string read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

bool has_markdown_title(const std::string& content) {
  std::string line;
  std::string cur;
  for (char c : content) {
    if (c == '\n') {
      // trim leading space
      size_t i = cur.find_first_not_of(" \t\r");
      if (i != std::string::npos && cur[i] == '#') return true;
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  size_t i = cur.find_first_not_of(" \t\r");
  return (i != std::string::npos && cur[i] == '#');
}

bool contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

void check_doc(const std::string& rel, size_t min_bytes,
               const std::vector<std::string>& keywords, bool need_title) {
  fs::path p = resolve(rel);
  std::error_code ec;
  bool exists = fs::exists(p, ec);
  check(exists, rel + " existiert (" + p.string() + ")");
  if (!exists) return;
  std::string c = read_all(p);
  check(!c.empty(), rel + " nicht-leer");
  check(c.size() >= min_bytes, rel + " >= " + std::to_string(min_bytes) + " Bytes");
  if (need_title) check(has_markdown_title(c), rel + " hat Markdown-Titel (# ...)");
  for (const auto& kw : keywords) {
    check(contains(c, kw), rel + " enthaelt \"" + kw + "\"");
  }
}

}  // namespace

int main() {
  std::cout << "[docs] Existenz + Nicht-Leer + Markdown-Titel\n";

  check_doc("docs/ARCH.md", 1024, {"WAL", "MVCC", "Shard", "HNSW"}, true);
  check_doc("docs/RUNBOOK.md", 256, {"Build", "Recovery"}, true);

  // k8s-Stub: YAML, 1 Replica + PVC.
  {
    fs::path p = resolve("k8s/statefulset.yaml");
    std::error_code ec;
    bool exists = fs::exists(p, ec);
    check(exists, "k8s/statefulset.yaml existiert (" + p.string() + ")");
    if (exists) {
      std::string c = read_all(p);
      check(!c.empty(), "k8s/statefulset.yaml nicht-leer");
      check(contains(c, "replicas: 1"), "statefulset replicas: 1");
      check(contains(c, "volumeClaimTemplates") || contains(c, "PVC") ||
                contains(c, "claim"),
            "statefulset hat PVC (volumeClaimTemplates)");
    }
  }

  // SBOM: root bevorzugt, docs/ als Fallback (s20 liefert beide).
  {
    fs::path p = resolve("SBOM.md");
    std::error_code ec;
    if (!fs::exists(p, ec)) p = resolve("docs/SBOM.md");
    bool exists = fs::exists(p, ec);
    check(exists, "SBOM.md existiert (" + p.string() + ")");
    if (exists) {
      std::string c = read_all(p);
      check(!c.empty(), "SBOM.md nicht-leer");
      check(has_markdown_title(c), "SBOM.md hat Markdown-Titel");
      check(contains(c, "STL") || contains(c, "POSIX"), "SBOM nennt STL/POSIX");
      check(contains(c, "FAISS"), "SBOM dokumentiert FAISS-Status (kein FAISS)");
    }
  }

  if (failures == 0) {
    std::cout << "DOCS TESTS PASSED\n";
    return 0;
  }
  std::cout << "DOCS TESTS FAILED (" << failures << ")\n";
  return 1;
}

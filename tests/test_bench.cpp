// s18-bench Tests: smoke run klein (N=1000) + CSV parse.
// Ruft das dbbench-Binary mit --smoke auf, parst den CSV-Report
// (op,throughput,lat_p95) und prueft Messwerte auf Plausibilitaet.
// Stil: assert + cout, kein gtest. CTest-Name "bench" (via test_bench)
// plus direkter Smoke "bench" (dbbench --smoke), beide matchen -R bench.

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_fail = 0;

void Check(bool cond, const std::string& name) {
  if (cond) {
    std::cout << "PASS " << name << "\n";
  } else {
    std::cout << "FAIL " << name << "\n";
    ++g_fail;
  }
}

std::string BasenameDir(const std::string& argv0) {
  auto p = argv0.find_last_of('/');
  if (p == std::string::npos) return ".";
  if (p == 0) return "/";
  return argv0.substr(0, p);
}

bool FileExists(const std::string& p) {
  FILE* f = std::fopen(p.c_str(), "r");
  if (!f) return false;
  std::fclose(f);
  return true;
}

std::string FindDbbench(const std::string& argv0) {
  std::vector<std::string> cands;
  std::string dir = BasenameDir(argv0);
  cands.push_back(dir + "/dbbench");
  cands.push_back("./build/dbbench");
  cands.push_back("build/dbbench");
  cands.push_back("./dbbench");
  cands.push_back("dbbench");
  for (auto& c : cands) {
    // popen braucht ggf. Pfad; Existenzcheck nur fuer pfadartige.
    if (c.find('/') != std::string::npos && !FileExists(c)) continue;
    return c;
  }
  return dir + "/dbbench";
}

int RunCapture(const std::string& cmd, std::string& out, int& exit_code) {
  out.clear();
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return -1;
  std::array<char, 4096> buf{};
  while (std::fgets(buf.data(), (int)buf.size(), p))
    out += buf.data();
  int rc = pclose(p);
  exit_code = rc;
  return 0;
}

std::vector<std::string> Split(const std::string& s, char delim) {
  std::vector<std::string> parts;
  std::string cur;
  std::istringstream in(s);
  while (std::getline(in, cur, delim)) parts.push_back(cur);
  return parts;
}

struct CsvRow {
  std::string op;
  double throughput = 0.0;
  double lat_p95 = 0.0;
};

}  // namespace

int main(int argc, char** argv) {
  std::string bin = FindDbbench(argc > 0 ? argv[0] : std::string("test_bench"));
  std::cout << "dbbench binary: " << bin << "\n";

  // 1) Smoke run klein: --smoke (kv=1000, sql=1000, ann=1000,16,10).
  std::string out;
  int rc = -1;
  int run_ok = RunCapture(bin + " --smoke 2>/tmp/bench_smoke.stderr", out, rc);
  Check(run_ok == 0, "smoke/popen-ok");
  Check(rc == 0, "smoke/exit-0");
  std::cout << "--- dbbench --smoke stdout ---\n" << out << "--- end ---\n";

  // 2) CSV parse: Header + Zeilen.
  std::istringstream lines(out);
  std::string header;
  Check(!!std::getline(lines, header), "csv/has-header");
  // CR tolerieren.
  if (!header.empty() && header.back() == '\r') header.pop_back();
  Check(header == "op,throughput,lat_p95", "csv/header");

  std::vector<CsvRow> rows;
  bool cols_ok = true, vals_ok = true;
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    auto cols = Split(line, ',');
    if (cols.size() != 3) {
      cols_ok = false;
      continue;
    }
    CsvRow r;
    r.op = cols[0];
    try {
      r.throughput = std::stod(cols[1]);
      r.lat_p95 = std::stod(cols[2]);
    } catch (...) {
      vals_ok = false;
      continue;
    }
    if (r.op.empty() || !std::isfinite(r.throughput) || r.throughput <= 0.0 ||
        !std::isfinite(r.lat_p95) || r.lat_p95 < 0.0) {
      vals_ok = false;
      continue;
    }
    rows.push_back(r);
  }
  Check(cols_ok, "csv/3-cols");
  Check(vals_ok, "csv/values-plausible");
  std::cout << "csv rows: " << rows.size() << "\n";
  Check(rows.size() >= 3, "csv/smoke-3-rows");

  bool kv = false, sql = false, ann = false;
  for (auto& r : rows) {
    if (r.op == "kv_puts") kv = true;
    if (r.op == "sql_q1") sql = true;
    if (r.op == "ann_search") ann = true;
  }
  Check(kv && sql && ann, "csv/ops-kv-sql-ann");

  // 3) Klein-Bench direkt: --kv-puts 1000 liefert genau eine Zeile.
  std::string out2;
  int rc2 = -1;
  Check(RunCapture(bin + " --kv-puts 1000 2>/dev/null", out2, rc2) == 0,
        "kv1000/popen-ok");
  Check(rc2 == 0, "kv1000/exit-0");
  std::istringstream l2(out2);
  std::string h2;
  Check(!!std::getline(l2, h2), "kv1000/has-header");
  if (!h2.empty() && h2.back() == '\r') h2.pop_back();
  Check(h2 == "op,throughput,lat_p95", "kv1000/header");
  int n2 = 0;
  bool only_kv = true;
  while (std::getline(l2, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    ++n2;
    if (line.rfind("kv_puts,", 0) != 0) only_kv = false;
  }
  Check(n2 == 1, "kv1000/one-row");
  Check(only_kv, "kv1000/only-kv");

  if (g_fail == 0) {
    std::cout << "bench tests passed (smoke N=1000 + CSV parse)\n";
    return 0;
  }
  std::cout << g_fail << " BENCH TEST(S) FAILED\n";
  return 1;
}

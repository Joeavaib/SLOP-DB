// WAL-Implementierung: append (LSN), flush (fsync-Policy), replay, checkpoint,
// CRC32-lite. POSIX-Pfad (Linux): open/write/fdatasync/rename fuer Kontrolle
// ueber Crash-Safety. Ein fsync pro flush(), nicht pro Record.

#include "dbengine/storage/wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>
#include <utility>

#include <sys/stat.h>
#include <sys/uio.h>

#ifdef DBENGINE_WITH_TLS
#include <openssl/evp.h>
#endif

namespace dbengine::storage {
namespace {

// Little-endian (de)serialisierung — portabel, unabhaengig von Host-Endianness.
void put_u32le(char* p, uint32_t v) {
  p[0] = static_cast<char>(v & 0xFF);
  p[1] = static_cast<char>((v >> 8) & 0xFF);
  p[2] = static_cast<char>((v >> 16) & 0xFF);
  p[3] = static_cast<char>((v >> 24) & 0xFF);
}
void put_u64le(char* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}
uint32_t get_u32le(const char* p) {
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}
uint64_t get_u64le(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  return v;
}

bool read_full(int fd, void* buf, size_t n) {
  char* p = static_cast<char*>(buf);
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, p + got, n - got);
    if (r == 0) return false;  // EOF -> torn
    if (r < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("WAL read: ") + std::strerror(errno));
    }
    got += static_cast<size_t>(r);
  }
  return true;
}

void fsync_file(int fd) {
  // fdatasync reicht (keine Metadaten-Aenderung ausser Size; Size ist kritisch,
  // fdatasync synced sie). Fallback fsync.
#ifdef __linux__
  if (::fdatasync(fd) == 0) return;
#endif
  if (::fsync(fd) != 0)
    throw std::runtime_error(std::string("WAL fsync: ") + std::strerror(errno));
}

void fsync_dir_of(const std::string& path) {
  // Nach rename() Directory fsyncen, damit der Rename selbst Kill--9-sicher ist.
  auto slash = path.find_last_of('/');
  std::string dir = (slash == std::string::npos) ? "." : path.substr(0, slash);
  if (dir.empty()) dir = ".";
  int dfd = ::open(dir.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                                 | O_DIRECTORY
#endif
  );
  if (dfd < 0) return;  // best effort
  (void)::fsync(dfd);
  ::close(dfd);
}

// Ein writev()-Syscall pro Record (Header+Payload atomar im Syscall-Sinn,
// weiterhin ohne fsync). Loop nur fuer Partial-Writes/EINTR; Bytes/Format/CRC
// identisch zu vorher zwei write().
void writev_all(int fd, struct iovec* iov, int iovcnt) {
  while (iovcnt > 0) {
    ssize_t w = ::writev(fd, iov, iovcnt);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("WAL writev: ") + std::strerror(errno));
    }
    if (w == 0) continue;  // dürfte bei len>0 nicht passieren; erneut versuchen
    ssize_t left = w;
    while (left > 0 && iovcnt > 0) {
      size_t cur = iov[0].iov_len;
      if (static_cast<size_t>(left) >= cur) {
        left -= static_cast<ssize_t>(cur);
        ++iov;
        --iovcnt;
      } else {
        iov[0].iov_base = static_cast<char*>(iov[0].iov_base) + left;
        iov[0].iov_len = cur - static_cast<size_t>(left);
        left = 0;
      }
    }
  }
}

inline void write_record(int fd, const char hdr[20], const char* payload,
                         uint32_t len) {
  struct iovec iov[2];
  iov[0].iov_base = const_cast<char*>(hdr);
  iov[0].iov_len = 20;
  int cnt = 1;
  if (len) {
    iov[1].iov_base = const_cast<char*>(payload);
    iov[1].iov_len = len;
    cnt = 2;
  }
  writev_all(fd, iov, cnt);
}

// GCM-Record: Header(20) + Nonce(12) + Blob(Cipher+Tag). Ein writev()-Syscall,
// selbe fsync-Policy wie plain (flush macht Dauerhaftigkeit).
// [[maybe_unused]]: ohne DBENGINE_WITH_TLS ist der GCM-Pfad inaktiv.
[[maybe_unused]] inline void write_record_enc(int fd, const char hdr[20],
                             const unsigned char nonce[12], const char* blob,
                             uint32_t blob_len) {
  struct iovec iov[3];
  iov[0].iov_base = const_cast<char*>(hdr);
  iov[0].iov_len = 20;
  iov[1].iov_base = const_cast<unsigned char*>(nonce);
  iov[1].iov_len = 12;
  int cnt = 2;
  if (blob_len) {
    iov[2].iov_base = const_cast<char*>(blob);
    iov[2].iov_len = blob_len;
    cnt = 3;
  }
  writev_all(fd, iov, cnt);
}

// Nonce pro Record: 8B LSN-LE + 4B frischer Zufalls-Salt (random_device,
// kein OpenSSL noetig, damit Nonce-Erzeugung auch ohne TLS kompiliert;
// Verschluesselung selbst bleibt TLS-gated).
[[maybe_unused]] void make_nonce(uint64_t lsn, unsigned char nonce[12]) {
  put_u64le(reinterpret_cast<char*>(nonce), lsn);
  std::random_device rd;
  for (int i = 8; i < 12; ++i) nonce[i] = static_cast<unsigned char>(rd() & 0xFF);
}

// AAD = magic + lsn + raw_len (16 Header-Bytes vor crc), bindet Position.
[[maybe_unused]] void make_aad(uint32_t magic, uint64_t lsn, uint32_t raw_len,
              unsigned char aad[16]) {
  put_u32le(reinterpret_cast<char*>(aad), magic);
  put_u64le(reinterpret_cast<char*>(aad) + 4, lsn);
  put_u32le(reinterpret_cast<char*>(aad) + 12, raw_len);
}

#ifdef DBENGINE_WITH_TLS
// AES-256-GCM via OpenSSL EVP. out = Cipher(in_len) + Tag(16).
bool gcm_encrypt(const Wal::Key32& key, const unsigned char nonce[12],
                 const unsigned char* aad, size_t aad_len, const char* in,
                 size_t in_len, std::string& out) {
  out.assign(in_len + Wal::kTagLen, '\0');
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) return false;
  bool ok = false;
  int outl = 0;
  size_t produced = 0;
  do {
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) !=
        1)
      break;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(Wal::kNonceLen),
                            nullptr) != 1)
      break;
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1) break;
    if (aad_len > 0 &&
        EVP_EncryptUpdate(ctx, nullptr, &outl, aad,
                          static_cast<int>(aad_len)) != 1)
      break;
    if (in_len > 0 &&
        EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()),
                          &outl, reinterpret_cast<const unsigned char*>(in),
                          static_cast<int>(in_len)) != 1)
      break;
    produced = static_cast<size_t>(outl);
    if (produced != in_len) break;  // GCM stroemt (kein Padding/Buffering)
    if (EVP_EncryptFinal_ex(
            ctx, reinterpret_cast<unsigned char*>(out.data()) + produced,
            &outl) != 1)
      break;
    produced += static_cast<size_t>(outl);  // GCM-Final liefert 0
    if (produced != in_len) break;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                            static_cast<int>(Wal::kTagLen),
                            reinterpret_cast<unsigned char*>(out.data()) +
                                in_len) != 1)
      break;
    ok = true;
  } while (false);
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) out.clear();
  return ok;
}

// Rueckgabe false bei Tag-Mismatch (falscher Schluessel/Tamper) oder Fehler.
bool gcm_decrypt(const Wal::Key32& key, const unsigned char nonce[12],
                 const unsigned char* aad, size_t aad_len, const char* blob,
                 size_t blob_len, std::string& out_plain) {
  if (blob_len < Wal::kTagLen) return false;
  const size_t in_len = blob_len - Wal::kTagLen;
  out_plain.assign(in_len, '\0');
  EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
  if (ctx == nullptr) return false;
  bool ok = false;
  int outl = 0;
  size_t produced = 0;
  do {
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) !=
        1)
      break;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                            static_cast<int>(Wal::kNonceLen),
                            nullptr) != 1)
      break;
    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce) != 1) break;
    if (aad_len > 0 &&
        EVP_DecryptUpdate(ctx, nullptr, &outl, aad,
                          static_cast<int>(aad_len)) != 1)
      break;
    if (in_len > 0 &&
        EVP_DecryptUpdate(
            ctx, reinterpret_cast<unsigned char*>(out_plain.data()), &outl,
            reinterpret_cast<const unsigned char*>(blob),
            static_cast<int>(in_len)) != 1)
      break;
    produced = static_cast<size_t>(outl);
    if (produced != in_len) break;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                            static_cast<int>(Wal::kTagLen),
                            const_cast<char*>(blob) + in_len) != 1)
      break;
    if (EVP_DecryptFinal_ex(
            ctx, reinterpret_cast<unsigned char*>(out_plain.data()) + produced,
            &outl) != 1)
      break;  // Tag-Mismatch -> ok=false
    produced += static_cast<size_t>(outl);
    if (produced != in_len) break;
    ok = true;
  } while (false);
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) out_plain.clear();
  return ok;
}
#endif  // DBENGINE_WITH_TLS

// Audit-Konvention (s. wal.h): Escaping pro Feld.
std::string audit_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\') {
      out.push_back('\\');
      out.push_back('\\');
    } else if (c == '\n') {
      out.push_back('\\');
      out.push_back('n');
    } else {
      out.push_back(c);
    }
  }
  return out;
}

// Rueckgabe false bei ungueltiger Escape-Sequenz (inkl. einsamer '\\' am
// Ende oder rohem '\n' im Feld — Felder duerfen nach Split kein rohes '\n'
// mehr enthalten).
bool audit_unescape(std::string_view in, std::string& out) {
  out.clear();
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '\\') {
      if (i + 1 >= in.size()) return false;
      char n = in[i + 1];
      if (n == '\\')
        out.push_back('\\');
      else if (n == 'n')
        out.push_back('\n');
      else
        return false;  // unbekannte Escape-Sequenz -> strikt ablehnen
      ++i;
    } else if (c == '\n') {
      return false;  // rohes '\n' gehoert nicht in ein Feld
    } else {
      out.push_back(c);
    }
  }
  return true;
}

}  // namespace

Wal::Wal(std::string path) : path_(std::move(path)) {}
Wal::~Wal() { close(); }

void Wal::crc32_table_init(uint32_t t[256]) {
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
}

uint32_t Wal::crc32(const void* data, size_t n, uint32_t seed) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    crc32_table_init(table);
    init = true;
  }
  uint32_t c = ~seed;
  const auto* p = static_cast<const unsigned char*>(data);
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

uint32_t Wal::record_crc(uint64_t lsn, uint32_t len, const char* payload) {
  char hdr[12];
  put_u64le(hdr, lsn);
  put_u32le(hdr + 8, len);
  uint32_t c = crc32(hdr, sizeof(hdr), 0);
  if (len && payload) {
    // Weiterketten: CRC ueber Payload mit Seed = bisheriger CRC.
    // crc32(seed) erwartet ~seed intern, daher Rekombination ueber raw-Form:
    // einfacher & korrekt: CRC ueber Konkatenation via zwei Schritte geht mit
    // dieser Implementierung nicht direkt — berechne CRC ueber hdr+payload
    // sequentiell neu: wir nutzen internen Loop ueber beide Teile.
    // Pragmatisch: CRC(hdr || payload) = f(f(hdr), payload). Da unsere
    // crc32(seed) als ~seed startet und ~c endet, gilt die Verkettung nicht
    // exakt — deshalb berechnen wir hier explizit ueber kopierten Puffer nur
    // fuer kleine Records direkt; fuer grosse chunkweise mit raw-State.
    // Einfachste korrekte Variante: CRC ueber hdr, dann weiter mit payload
    // im selben ~c-Raum:
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
      crc32_table_init(table);
      init = true;
    }
    uint32_t raw = ~c;  // zurueck in Arbeitsraum
    const auto* p = reinterpret_cast<const unsigned char*>(payload);
    for (uint32_t i = 0; i < len; ++i) raw = table[(raw ^ p[i]) & 0xFF] ^ (raw >> 8);
    return ~raw;
  }
  return c;
}

uint32_t Wal::enc_record_crc(uint64_t lsn, uint32_t raw_len,
                             const unsigned char* nonce, const char* blob,
                             uint32_t blob_len) {
  // CRC ueber lsn-LE + raw_len-LE (inkl. Marker-Bit) + nonce + blob.
  // Verkettung im selben ~c-Arbeitsraum wie record_crc (s. dort).
  char pre[8 + 4 + 12];
  put_u64le(pre, lsn);
  put_u32le(pre + 8, raw_len);
  std::memcpy(pre + 12, nonce, kNonceLen);
  uint32_t c = crc32(pre, sizeof(pre), 0);
  if (blob_len && blob) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
      crc32_table_init(table);
      init = true;
    }
    uint32_t raw = ~c;
    const auto* p = reinterpret_cast<const unsigned char*>(blob);
    for (uint32_t i = 0; i < blob_len; ++i)
      raw = table[(raw ^ p[i]) & 0xFF] ^ (raw >> 8);
    return ~raw;
  }
  return c;
}

void Wal::setEncryptionKey(const Key32& key) {
#ifdef DBENGINE_WITH_TLS
  std::lock_guard<std::mutex> g(mu_);
  enc_key_ = key;
  enc_enabled_ = true;
#else
  (void)key;
  throw std::logic_error(
      "WAL encryption requires a DBENGINE_WITH_TLS build (OpenSSL "
      "AES-256-GCM); refusing fake crypto");
#endif
}

void Wal::setEncryptionKey(std::string_view raw32) {
#ifdef DBENGINE_WITH_TLS
  if (raw32.size() != kKeyLen)
    throw std::invalid_argument("WAL encryption key must be exactly 32 bytes");
  Key32 k{};
  std::memcpy(k.data(), raw32.data(), kKeyLen);
  setEncryptionKey(k);
  std::memset(k.data(), 0, kKeyLen);  // Kopie auf dem Stack wischen
#else
  (void)raw32;
  throw std::logic_error(
      "WAL encryption requires a DBENGINE_WITH_TLS build (OpenSSL "
      "AES-256-GCM); refusing fake crypto");
#endif
}

void Wal::clearEncryptionKey() {
  std::lock_guard<std::mutex> g(mu_);
  std::memset(enc_key_.data(), 0, enc_key_.size());
  enc_enabled_ = false;
}

bool Wal::encryption_enabled() const {
  std::lock_guard<std::mutex> g(mu_);
  return enc_enabled_;
}

void Wal::write_all(int fd, const void* buf, size_t n) {
  const char* p = static_cast<const char*>(buf);
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::write(fd, p + done, n - done);
    if (w < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("WAL write: ") + std::strerror(errno));
    }
    done += static_cast<size_t>(w);
  }
}

int64_t Wal::file_size(int fd) {
  struct stat st {};
  if (::fstat(fd, &st) != 0)
    throw std::runtime_error(std::string("WAL fstat: ") + std::strerror(errno));
  return static_cast<int64_t>(st.st_size);
}

Wal::ScanResult Wal::scan(int fd) { return scan(fd, nullptr); }

Wal::ScanResult Wal::scan(int fd, const Key32* key) {
  ScanResult out;
  if (::lseek(fd, 0, SEEK_SET) < 0)
    throw std::runtime_error(std::string("WAL lseek: ") + std::strerror(errno));
  int64_t pos = 0;
  for (;;) {
    char hdr[4 + 8 + 4 + 4];
    if (!read_full(fd, hdr, sizeof(hdr))) break;  // EOF / torn header -> stop
    if (get_u32le(hdr) != kMagic) break;          // Korrupt -> torn tail, stop
    uint64_t lsn = get_u64le(hdr + 4);
    uint32_t len = get_u32le(hdr + 12);
    uint32_t want = get_u32le(hdr + 16);
    const bool enc = (len & kEncFlag) != 0;
    uint32_t stored = len & ~kEncFlag;
    if (!enc) {
      // Plain-Pfad: byte-identisch zu V1 (unveraendert).
      if (len > kMaxPayload) break;  // Korrupt -> stop
      if (lsn == 0 || lsn < out.max_lsn) break;  // LSN muss steigen
      std::string payload;
      payload.resize(len);
      if (len && !read_full(fd, payload.data(), len)) break;  // torn payload
      if (record_crc(lsn, len, payload.data()) != want) break;  // CRC -> stop
      out.max_lsn = lsn;
      pos += static_cast<int64_t>(sizeof(hdr) + len);
      out.valid_bytes = pos;
      out.records.push_back(WalRecord{lsn, std::move(payload)});
      continue;
    }
    // GCM-Pfad: stored = Cipher+Tag-Bytes, Nonce folgt dem Header.
    if (stored < kTagLen || stored - kTagLen > kMaxPayload) break;  // korrupt
    if (lsn == 0 || lsn < out.max_lsn) break;  // LSN muss steigen
    unsigned char nonce[kNonceLen];
    if (!read_full(fd, nonce, sizeof(nonce))) break;  // torn nonce -> stop
    std::string blob;
    blob.resize(stored);
    if (!read_full(fd, blob.data(), stored)) break;  // torn blob -> stop
    if (enc_record_crc(lsn, len, nonce, blob.data(), stored) != want)
      break;  // CRC ueber Chiffre -> stop (Torn-Erkennung)
    if (key == nullptr) {
      out.need_key = true;  // Prefix liefern, Aufrufer entscheidet (fail-closed)
      break;
    }
#ifdef DBENGINE_WITH_TLS
    unsigned char aad[16];
    make_aad(kMagic, lsn, len, aad);
    std::string plain;
    if (!gcm_decrypt(*key, nonce, aad, sizeof(aad), blob.data(), stored,
                     plain)) {
      out.auth_failed = true;  // falscher Schluessel/Tamper -> Prefix, fail-closed
      break;
    }
    out.max_lsn = lsn;
    pos += static_cast<int64_t>(sizeof(hdr) + sizeof(nonce) + stored);
    out.valid_bytes = pos;
    out.records.push_back(WalRecord{lsn, std::move(plain)});
#else
    out.need_key = true;  // ohne TLS nie entschluesselbar
    break;
#endif
  }
  return out;
}

void Wal::ensure_open() {
  if (fd_ < 0) throw std::runtime_error("WAL not open: " + path_);
}

void Wal::open() {
  std::unique_lock<std::mutex> g(mu_);
  if (fd_ >= 0) return;
  int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0)
    throw std::runtime_error(std::string("WAL open: ") + std::strerror(errno));
  // Recovery beim Start: scanne, setze next_lsn, kappe torn tail.
  const Key32* key = enc_enabled_ ? &enc_key_ : nullptr;
  ScanResult s = scan(fd, key);
  // Fail-closed statt Datenverlust: GCM-Records ohne Schluessel oder mit
  // falschem Schluessel duerfen NICHT gekappt werden (kein Truncate).
  if (s.need_key) {
    ::close(fd);
    throw std::runtime_error(
        "WAL open: encrypted records present but no key set "
        "(call setEncryptionKey before open)");
  }
  if (s.auth_failed) {
    ::close(fd);
    throw std::runtime_error(
        "WAL open: GCM auth failed (wrong key or tampered tail); not "
        "truncating");
  }
  int64_t sz = file_size(fd);
  if (sz > s.valid_bytes) {
    // Torn tail kappen (abgerissener write nach Crash).
    if (::ftruncate(fd, s.valid_bytes) != 0) {
      ::close(fd);
      throw std::runtime_error(std::string("WAL ftruncate: ") + std::strerror(errno));
    }
    fsync_file(fd);
  }
  next_lsn_ = s.max_lsn + 1;
  durable_lsn_ = s.max_lsn;  // nach Truncate+fsync ist Dateiinhalt dauerhaft
  // Append-Position ans Ende.
  if (::lseek(fd, 0, SEEK_END) < 0) {
    ::close(fd);
    throw std::runtime_error(std::string("WAL lseek-end: ") + std::strerror(errno));
  }
  fd_ = fd;
  g.unlock();
  cv_.notify_all();
}

void Wal::append_encrypted(int fd, const Key32& key, uint64_t lsn,
                           const char* data, size_t n) {
#ifdef DBENGINE_WITH_TLS
  unsigned char nonce[kNonceLen];
  make_nonce(lsn, nonce);
  const uint32_t stored =
      static_cast<uint32_t>(n) + static_cast<uint32_t>(kTagLen);
  const uint32_t raw = stored | kEncFlag;
  unsigned char aad[16];
  make_aad(kMagic, lsn, raw, aad);
  std::string blob;
  if (!gcm_encrypt(key, nonce, aad, sizeof(aad), data, n, blob))
    throw std::runtime_error("WAL GCM encrypt failed");
  char hdr[4 + 8 + 4 + 4];
  put_u32le(hdr, kMagic);
  put_u64le(hdr + 4, lsn);
  put_u32le(hdr + 12, raw);
  put_u32le(hdr + 16, enc_record_crc(lsn, raw, nonce, blob.data(), stored));
  write_record_enc(fd, hdr, nonce, blob.data(), stored);
#else
  (void)fd;
  (void)key;
  (void)lsn;
  (void)data;
  (void)n;
  throw std::logic_error(
      "WAL encryption requires a DBENGINE_WITH_TLS build (OpenSSL "
      "AES-256-GCM); refusing fake crypto");
#endif
}

uint64_t Wal::append(std::string_view payload) {
  std::lock_guard<std::mutex> g(mu_);
  ensure_open();
  if (payload.size() > kMaxPayload) throw std::runtime_error("WAL payload too large");
  uint64_t lsn = next_lsn_++;
  if (enc_enabled_) {
    append_encrypted(fd_, enc_key_, lsn, payload.data(), payload.size());
    ++appends_;
    return lsn;
  }
  uint32_t len = static_cast<uint32_t>(payload.size());
  char hdr[4 + 8 + 4 + 4];
  put_u32le(hdr, kMagic);
  put_u64le(hdr + 4, lsn);
  put_u32le(hdr + 12, len);
  put_u32le(hdr + 16, record_crc(lsn, len, payload.data()));
  write_record(fd_, hdr, payload.data(), len);
  // KEIN fsync hier — flush() macht Group-Commit (Performance).
  ++appends_;
  return lsn;
}

std::vector<uint64_t> Wal::append_many(
    const std::vector<std::string>& payloads) {
  std::lock_guard<std::mutex> g(mu_);
  ensure_open();
  std::vector<uint64_t> out;
  out.reserve(payloads.size());
  for (const auto& p : payloads) {
    if (p.size() > kMaxPayload) throw std::runtime_error("WAL payload too large");
    uint64_t lsn = next_lsn_++;
    if (enc_enabled_) {
      append_encrypted(fd_, enc_key_, lsn, p.data(), p.size());
      ++appends_;
      out.push_back(lsn);
      continue;
    }
    uint32_t len = static_cast<uint32_t>(p.size());
    char hdr[4 + 8 + 4 + 4];
    put_u32le(hdr, kMagic);
    put_u64le(hdr + 4, lsn);
    put_u32le(hdr + 12, len);
    put_u32le(hdr + 16, record_crc(lsn, len, p.data()));
    write_record(fd_, hdr, p.data(), len);
    ++appends_;
    out.push_back(lsn);
  }
  return out;
}

void Wal::flush() {
  std::unique_lock<std::mutex> g(mu_);
  ensure_open();
  fsync_file(fd_);
  durable_lsn_ = (next_lsn_ == 0) ? 0 : next_lsn_ - 1;
  ++flushes_;
  g.unlock();
  cv_.notify_all();
}

std::vector<WalRecord> Wal::replay() {
  std::lock_guard<std::mutex> g(mu_);
  ensure_open();
  // Verlaufsgarantie: was der Aufrufer lesen will, muss vorher flush() sein.
  // Wir lesen vom Dateianfang; scan() toleriert torn tail.
  // Lesepfad: Prefix bis zum ersten GCM-Record ohne Schluessel (kein Throw).
  const Key32* key = enc_enabled_ ? &enc_key_ : nullptr;
  ScanResult s = scan(fd_, key);
  if (::lseek(fd_, 0, SEEK_END) < 0)
    throw std::runtime_error(std::string("WAL lseek-end: ") + std::strerror(errno));
  return s.records;
}

std::vector<WalRecord> Wal::replay_file(const std::string& path) {
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) return {};  // keine Datei -> leeres Log
    throw std::runtime_error(std::string("WAL replay open: ") + std::strerror(errno));
  }
  ScanResult s = scan(fd);  // statisch: ohne Schluessel -> Prefix bis GCM, kappt nie
  ::close(fd);
  return s.records;
}

void Wal::checkpoint(uint64_t checkpoint_lsn) {
  std::unique_lock<std::mutex> g(mu_);
  ensure_open();
  const Key32* key = enc_enabled_ ? &enc_key_ : nullptr;
  ScanResult s = scan(fd_, key);
  // Fail-closed: ohne passenden Schluessel nichts verwerfen/umschreiben.
  if (s.need_key)
    throw std::runtime_error(
        "WAL checkpoint: encrypted records present but no key set "
        "(call setEncryptionKey before checkpoint)");
  if (s.auth_failed)
    throw std::runtime_error(
        "WAL checkpoint: GCM auth failed (wrong key or tampered tail)");
  std::string tmp = path_ + ".chkpt.tmp";
  int tfd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (tfd < 0)
    throw std::runtime_error(std::string("WAL checkpoint open: ") + std::strerror(errno));
  uint64_t max_kept = 0;
  for (const auto& r : s.records) {
    if (r.lsn <= checkpoint_lsn) continue;
    // scan() liefert Klartext; bei aktivem Key frisch verschluesselt
    // zurueckschreiben (neue Nonce), sonst plain (byte-identisch zu V1).
    if (enc_enabled_) {
      append_encrypted(tfd, enc_key_, r.lsn, r.data.data(), r.data.size());
      max_kept = r.lsn;
      continue;
    }
    uint32_t len = static_cast<uint32_t>(r.data.size());
    char hdr[4 + 8 + 4 + 4];
    put_u32le(hdr, kMagic);
    put_u64le(hdr + 4, r.lsn);
    put_u32le(hdr + 12, len);
    put_u32le(hdr + 16, record_crc(r.lsn, len, r.data.data()));
    write_record(tfd, hdr, r.data.data(), len);
    max_kept = r.lsn;
  }
  fsync_file(tfd);
  ::close(tfd);
  if (::rename(tmp.c_str(), path_.c_str()) != 0)
    throw std::runtime_error(std::string("WAL checkpoint rename: ") + std::strerror(errno));
  fsync_dir_of(path_);
  // Altes fd zeigt noch auf unlinked inode -> schliessen + neu oeffnen.
  ::close(fd_);
  int fd = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0)
    throw std::runtime_error(std::string("WAL reopen: ") + std::strerror(errno));
  if (::lseek(fd, 0, SEEK_END) < 0) {
    ::close(fd);
    throw std::runtime_error(std::string("WAL lseek-end: ") + std::strerror(errno));
  }
  fd_ = fd;
  next_lsn_ = max_kept + 1;
  if (next_lsn_ == 0) next_lsn_ = 1;
  if (max_kept + 1 == next_lsn_) durable_lsn_ = max_kept;
  g.unlock();
  cv_.notify_all();
}

void Wal::close() {
  std::lock_guard<std::mutex> g(mu_);
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

uint64_t Wal::next_lsn() const {
  std::lock_guard<std::mutex> g(mu_);
  return next_lsn_;
}

uint64_t Wal::durable_lsn() const {
  std::lock_guard<std::mutex> g(mu_);
  return durable_lsn_;
}

Wal::GroupStats Wal::group_stats() const {
  std::lock_guard<std::mutex> g(mu_);
  return GroupStats{appends_, flushes_};
}

bool Wal::wait_for_lsn(uint64_t target, int timeout_ms) const {
  std::unique_lock<std::mutex> g(mu_);
  auto pred = [&] { return durable_lsn_ >= target; };
  if (pred()) return true;
  if (timeout_ms < 0) {
    cv_.wait(g, pred);
    return true;
  }
  if (timeout_ms == 0) return false;
  return cv_.wait_for(g, std::chrono::milliseconds(timeout_ms), pred);
}

std::vector<WalRecord> Wal::read_from(uint64_t from_lsn, size_t max_records) {
  // replay() lockt intern; hier nicht halten (kein Double-Lock).
  std::vector<WalRecord> all = replay();
  std::vector<WalRecord> out;
  out.reserve(all.size());
  for (auto& r : all) {
    if (r.lsn < from_lsn) continue;
    out.push_back(std::move(r));
    if (max_records && out.size() >= max_records) break;
  }
  return out;
}

uint64_t Wal::append_audit(std::string_view actor, std::string_view action,
                           std::string_view detail) {
  // Ausserhalb des Locks encodieren; append() lockt + prueft kMaxPayload.
  std::string payload;
  payload.reserve(5 + actor.size() + action.size() + detail.size() + 2);
  payload += "AUD1\n";
  payload += audit_escape(actor);
  payload.push_back('\n');
  payload += audit_escape(action);
  payload.push_back('\n');
  payload += audit_escape(detail);
  return append(payload);
}

std::optional<AuditEvent> parse_audit(const WalRecord& rec) {
  constexpr std::string_view kPrefix = "AUD1\n";
  std::string_view data(rec.data);
  if (data.size() < kPrefix.size() || data.substr(0, kPrefix.size()) != kPrefix)
    return std::nullopt;  // strikter Prefix-Match, sonst skip
  std::string_view rest = data.substr(kPrefix.size());
  // Trennzeichen: unescapte '\n' (auf '\\' folgt immer genau ein Zeichen).
  size_t sep1 = std::string_view::npos;
  size_t sep2 = std::string_view::npos;
  for (size_t i = 0; i < rest.size(); ++i) {
    char c = rest[i];
    if (c == '\\') {
      if (i + 1 >= rest.size()) return std::nullopt;  // einsamer '\\'
      ++i;  // escaptes Zeichen ueberspringen (Gueltigkeit prueft unescape)
      continue;
    }
    if (c == '\n') {
      if (sep1 == std::string_view::npos)
        sep1 = i;
      else if (sep2 == std::string_view::npos)
        sep2 = i;
      else
        return std::nullopt;  // mehr als 2 Trennzeichen -> kein AUD1-Format
    }
  }
  if (sep1 == std::string_view::npos || sep2 == std::string_view::npos)
    return std::nullopt;  // zu wenige Felder
  std::string_view f_actor = rest.substr(0, sep1);
  std::string_view f_action = rest.substr(sep1 + 1, sep2 - sep1 - 1);
  std::string_view f_detail = rest.substr(sep2 + 1);
  AuditEvent ev;
  ev.lsn = rec.lsn;
  if (!audit_unescape(f_actor, ev.actor)) return std::nullopt;
  if (!audit_unescape(f_action, ev.action)) return std::nullopt;
  if (!audit_unescape(f_detail, ev.detail)) return std::nullopt;
  return ev;
}

std::vector<AuditEvent> Wal::read_audit(uint64_t from_lsn, size_t max_records) {
  // read_from()/replay() locken intern; hier nicht halten.
  std::vector<WalRecord> all = read_from(from_lsn, 0);
  std::vector<AuditEvent> out;
  out.reserve(all.size());
  for (const auto& r : all) {
    auto ev = parse_audit(r);
    if (!ev) continue;  // Nicht-AUD1 (strikter Prefix-Match) -> skip
    out.push_back(std::move(*ev));
    if (max_records && out.size() >= max_records) break;
  }
  return out;
}

std::vector<WalRecord> WalCdcSlot::poll(size_t max_records) {
  if (!wal_) return {};
  auto recs = wal_->read_from(cursor_, max_records);
  if (!recs.empty()) cursor_ = recs.back().lsn + 1;
  return recs;
}

}  // namespace dbengine::storage

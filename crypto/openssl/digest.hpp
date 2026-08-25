/*
    This file is part of TON Blockchain Library.

    TON Blockchain Library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    TON Blockchain Library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with TON Blockchain Library.  If not, see <http://www.gnu.org/licenses/>.

    Copyright 2017-2020 Telegram Systems LLP
*/
#pragma once
#include <assert.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/sha.h>
#include <type_traits>
#include <utility>

#include "td/utils/Slice.h"

namespace digest {
struct OpensslEVP_SHA1 {
  enum { digest_bytes = 20 };
  static const EVP_MD *get_evp() {
    return EVP_sha1();
  }
};

struct OpensslEVP_SHA256 {
  enum { digest_bytes = 32 };
  static const EVP_MD *get_evp() {
    return EVP_sha256();
  }
};

struct OpensslEVP_SHA512 {
  enum { digest_bytes = 64 };
  static const EVP_MD *get_evp() {
    return EVP_sha512();
  }
};

template <typename H>
class HashCtx {
  EVP_MD_CTX *base_ctx{nullptr};
  EVP_MD_CTX *ctx{nullptr};
  void init();
  void clear();

 public:
  enum { digest_bytes = H::digest_bytes };
  HashCtx() {
    init();
  }
  HashCtx(const void *data, std::size_t len) {
    init();
    feed(data, len);
  }
  ~HashCtx() {
    clear();
  }
  void reset();
  void feed(const void *data, std::size_t len);
  void feed(td::Slice slice) {
    feed(slice.data(), slice.size());
  }
  std::size_t extract(unsigned char buffer[digest_bytes]);
  std::size_t extract(td::MutableSlice slice);
  std::string extract();
};

template <typename H>
void HashCtx<H>::init() {
  ctx = EVP_MD_CTX_create();
  base_ctx = EVP_MD_CTX_create();
  EVP_DigestInit_ex(base_ctx, H::get_evp(), 0);
  reset();
}

template <typename H>
void HashCtx<H>::reset() {
  EVP_MD_CTX_copy_ex(ctx, base_ctx);
}

template <typename H>
void HashCtx<H>::clear() {
  EVP_MD_CTX_destroy(base_ctx);
  base_ctx = nullptr;
  EVP_MD_CTX_destroy(ctx);
  ctx = nullptr;
}

template <typename H>
void HashCtx<H>::feed(const void *data, std::size_t len) {
  EVP_DigestUpdate(ctx, data, len);
}

template <typename H>
std::size_t HashCtx<H>::extract(unsigned char buffer[digest_bytes]) {
  unsigned olen = 0;
  EVP_DigestFinal_ex(ctx, buffer, &olen);
  assert(olen == digest_bytes);
  return olen;
}

template <typename H>
std::size_t HashCtx<H>::extract(td::MutableSlice slice) {
  return extract(slice.ubegin());
}

template <typename H>
std::string HashCtx<H>::extract() {
  unsigned char buffer[digest_bytes];
  unsigned olen = 0;
  EVP_DigestFinal_ex(ctx, buffer, &olen);
  assert(olen == digest_bytes);
  return std::string((char *)buffer, olen);
}

typedef HashCtx<OpensslEVP_SHA1> SHA1;
typedef HashCtx<OpensslEVP_SHA512> SHA512;

struct SHA256Tag {};

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

template <>
struct HashCtx<SHA256Tag> {
 public:
  enum { digest_bytes = 32 };

  HashCtx() {
    SHA256_Init(&ctx_);
  }

  HashCtx(const void *data, std::size_t len) : HashCtx() {
    feed(data, len);
  }

  void reset() {
    SHA256_Init(&ctx_);
  }

  void feed(const void *data, std::size_t len) {
    SHA256_Update(&ctx_, data, len);
  }

  void feed(td::Slice slice) {
    feed(slice.data(), slice.size());
  }

  std::size_t extract(unsigned char buffer[digest_bytes]) {
    SHA256_Final(buffer, &ctx_);
    return digest_bytes;
  }

  std::size_t extract(td::MutableSlice slice) {
    CHECK(slice.size() == digest_bytes);
    SHA256_Final(slice.ubegin(), &ctx_);
    return digest_bytes;
  }

  std::string extract() {
    unsigned char buffer[digest_bytes];
    extract(buffer);
    return std::string((char *)buffer, digest_bytes);
  }

 private:
  SHA256_CTX ctx_;
};

namespace detail {

// SHA256_CTX is a public structure in the OpenSSL, LibreSSL and BoringSSL
// versions supported by this tree, but its fields are not a stable API. Keep
// the optimization conditional so an implementation with a different layout
// retains the regular SHA256_{Init,Update,Final} path.
template <class Context, class = void>
struct HasSha256StateWords : std::false_type {};

template <class Context>
struct HasSha256StateWords<Context,
                           std::void_t<decltype(std::declval<Context &>().h), decltype(std::declval<Context &>().h[0])>>
    : std::integral_constant<
          bool, std::is_array_v<std::remove_reference_t<decltype(std::declval<Context &>().h)>> &&
                    std::extent_v<std::remove_reference_t<decltype(std::declval<Context &>().h)>> == 8 &&
                    sizeof(std::declval<Context &>().h[0]) == 4 &&
                    std::is_unsigned_v<std::remove_reference_t<decltype(std::declval<Context &>().h[0])>>> {};

template <class Context, std::enable_if_t<HasSha256StateWords<Context>::value, int> = 0>
void sha256_digest_padded_blocks_impl(unsigned char output[SHA256_DIGEST_LENGTH], const void *data,
                                      std::size_t padded_size, std::size_t) {
  Context ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, data, padded_size);
  static_assert(SHA256_DIGEST_LENGTH == 8 * 4);
  for (std::size_t i = 0; i != SHA256_DIGEST_LENGTH / 4; ++i) {
    // SHA-256 serializes its numeric state words most-significant byte first;
    // shifts make this independent of the host's byte order.
    auto word = ctx.h[i];
    output[i * 4] = static_cast<unsigned char>(word >> 24);
    output[i * 4 + 1] = static_cast<unsigned char>(word >> 16);
    output[i * 4 + 2] = static_cast<unsigned char>(word >> 8);
    output[i * 4 + 3] = static_cast<unsigned char>(word);
  }
}

template <class Context, std::enable_if_t<!HasSha256StateWords<Context>::value, int> = 0>
void sha256_digest_padded_blocks_impl(unsigned char output[SHA256_DIGEST_LENGTH], const void *data, std::size_t,
                                      std::size_t message_size) {
  Context ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, data, message_size);
  SHA256_Final(output, &ctx);
}

}  // namespace detail

// Hash an input for which the caller already appended canonical SHA-256
// padding. On known SHA256_CTX layouts this avoids SHA256_Final's second
// padding pass and directly serializes the state after the whole blocks were
// consumed. Other layouts transparently use the regular finalization path.
inline void sha256_digest_padded_blocks(unsigned char output[SHA256_DIGEST_LENGTH], const void *data,
                                        std::size_t padded_size, std::size_t message_size) {
  assert(padded_size != 0 && padded_size % SHA256_CBLOCK == 0);
  assert(message_size + 1 + 8 <= padded_size);
  assert((message_size + 1 + 8 + SHA256_CBLOCK - 1) / SHA256_CBLOCK * SHA256_CBLOCK == padded_size);
  detail::sha256_digest_padded_blocks_impl<SHA256_CTX>(output, data, padded_size, message_size);
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

typedef HashCtx<SHA256Tag> SHA256;

template <typename T>
std::size_t hash_str(unsigned char buffer[T::digest_bytes], const void *data, std::size_t size) {
  T hasher(data, size);
  return hasher.extract(buffer);
}

template <typename T>
std::size_t hash_two_str(unsigned char buffer[T::digest_bytes], const void *data1, std::size_t size1, const void *data2,
                         std::size_t size2) {
  T hasher(data1, size1);
  hasher.feed(data2, size2);
  return hasher.extract(buffer);
}

template <typename T>
std::string hash_str(const void *data, std::size_t size) {
  T hasher(data, size);
  return hasher.extract();
}

template <typename T>
std::string hash_two_str(const void *data1, std::size_t size1, const void *data2, std::size_t size2) {
  T hasher(data1, size1);
  hasher.feed(data2, size2);
  return hasher.extract();
}
}  // namespace digest

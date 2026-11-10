#include <string>

#include <catch2/catch_test_macros.hpp>

#include "recs/sha256.hpp"

using recs::sha256_hex;

TEST_CASE("SHA-256 matches NIST test vectors", "[spine][sha256]") {
  CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(sha256_hex(std::string(1000000, 'a')) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 incremental updates equal one-shot", "[spine][sha256]") {
  const std::string msg(1000, 'x');
  for (std::size_t cut : {0u, 1u, 55u, 56u, 63u, 64u, 65u, 999u}) {
    recs::Sha256 h;
    h.update(std::string_view(msg).substr(0, cut));
    h.update(std::string_view(msg).substr(cut));
    CHECK(h.hex_digest() == sha256_hex(msg));
  }
}

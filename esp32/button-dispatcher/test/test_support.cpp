// Tests for the test helpers themselves (the SHA-256/HMAC/PBKDF2 used by the fake platform) and for util.cpp.

#include <string>

#include "dispatcher/util.hpp"
#include "fake_platform.hpp"
#include "minitest.hpp"
#include "sha256.hpp"

using dispatcher::hex_encode;

TEST(support_sha256_known_vectors) {
  CHECK_EQ(hex_encode(test_crypto::sha256("abc")),
           std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK_EQ(hex_encode(test_crypto::sha256("")),
           std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  // Two blocks of input exercise the block boundary.
  CHECK_EQ(hex_encode(test_crypto::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
           std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST(support_hmac_sha256_rfc4231_vectors) {
  CHECK_EQ(hex_encode(test_crypto::hmac_sha256(std::string(20, '\x0b'), "Hi There")),
           std::string("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
  CHECK_EQ(hex_encode(test_crypto::hmac_sha256("Jefe", "what do ya want for nothing?")),
           std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));
}

TEST(support_pbkdf2_known_vectors) {
  CHECK_EQ(hex_encode(test_crypto::pbkdf2_hmac_sha256("password", "salt", 1, 32)),
           std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
  CHECK_EQ(hex_encode(test_crypto::pbkdf2_hmac_sha256("password", "salt", 2, 32)),
           std::string("ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
}

TEST(util_base64) {
  CHECK_EQ(dispatcher::base64_encode(""), std::string(""));
  CHECK_EQ(dispatcher::base64_encode("f"), std::string("Zg=="));
  CHECK_EQ(dispatcher::base64_encode("fo"), std::string("Zm8="));
  CHECK_EQ(dispatcher::base64_encode("foo"), std::string("Zm9v"));
  CHECK_EQ(dispatcher::base64_encode("foobar"), std::string("Zm9vYmFy"));
  // The URL-safe form has no padding and uses - and _ in place of + and /.
  CHECK_EQ(dispatcher::base64url_encode("fo"), std::string("Zm8"));
  CHECK_EQ(dispatcher::base64_encode(std::string("\xfb\xff\xfe", 3)), std::string("+//+"));
  CHECK_EQ(dispatcher::base64url_encode(std::string("\xfb\xff\xfe", 3)), std::string("-__-"));
}

TEST(util_hex_round_trip) {
  CHECK_EQ(hex_encode(std::string("\x00\x01\xab\xff", 4)), std::string("0001abff"));
  CHECK(dispatcher::hex_decode("0001abFF").value() == std::string("\x00\x01\xab\xff", 4));
  CHECK(!dispatcher::hex_decode("abc").has_value());
  CHECK(!dispatcher::hex_decode("zz").has_value());
}

TEST(util_constant_time_equals) {
  CHECK(dispatcher::constant_time_equals("abc", "abc"));
  CHECK(!dispatcher::constant_time_equals("abc", "abd"));
  CHECK(!dispatcher::constant_time_equals("abc", "abcd"));
  CHECK(dispatcher::constant_time_equals("", ""));
}

TEST(util_host_of_hides_paths_queries_and_credentials) {
  CHECK_EQ(dispatcher::host_of("https://maker.ifttt.com/trigger/x/with/key/K?q=1"), std::string("https://maker.ifttt.com"));
  CHECK_EQ(dispatcher::host_of("http://wol.local:8080/wake"), std::string("http://wol.local:8080"));
  CHECK_EQ(dispatcher::host_of("HTTPS://Example.com"), std::string("https://Example.com"));
  CHECK_EQ(dispatcher::host_of("http://user:secret@host.example/path"), std::string("http://host.example"));
  CHECK_EQ(dispatcher::host_of("http://host?x=1"), std::string("http://host"));
  CHECK_EQ(dispatcher::host_of("garbage"), std::string("<url>"));
  CHECK_EQ(dispatcher::host_of("http:///path"), std::string("<url>"));
}

TEST(util_uuid4_format) {
  uint8_t bytes[16];
  for (int i = 0; i < 16; ++i) bytes[i] = static_cast<uint8_t>(i * 17);
  const std::string uuid = dispatcher::format_uuid4(bytes);
  CHECK_EQ(uuid.size(), static_cast<size_t>(36));
  CHECK_EQ(uuid[8], '-');
  CHECK_EQ(uuid[13], '-');
  CHECK_EQ(uuid[14], '4');  // version
  CHECK(uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b');  // variant
}

// Tests for auth.cpp (password storage, session tokens, login throttling) over the fake platform.

#include <string>

#include "dispatcher/auth.hpp"
#include "fake_platform.hpp"
#include "minitest.hpp"

using namespace dispatcher;

namespace {

constexpr int kFastIterations = 2;  // real PBKDF2 would only slow the tests down

struct Fixture {
  test_support::FakePlatform platform;
  AuthService auth{platform, kFastIterations};
  Fixture() { auth.load(); }
};

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST(auth_starts_unconfigured) {
  Fixture f;
  CHECK(!f.auth.is_configured());
  CHECK(!f.auth.verify_password("anything goes"));
  CHECK(!f.auth.verify_token(f.auth.make_token()));
}

TEST(auth_set_and_verify_password) {
  Fixture f;
  CHECK_EQ(f.auth.set_password("a good password"), std::string(""));
  CHECK(f.auth.is_configured());
  CHECK(f.auth.verify_password("a good password"));
  CHECK(!f.auth.verify_password("a bad password"));
  CHECK(!f.auth.verify_password(""));
}

TEST(auth_password_length_rules) {
  Fixture f;
  CHECK(contains(f.auth.set_password("short"), "password must be 8-256 characters"));
  CHECK(!f.auth.set_password(std::string(kMaxPasswordLength + 1, 'x')).empty());
  CHECK(!f.auth.is_configured());
  CHECK_EQ(f.auth.set_password(std::string(kMaxPasswordLength, 'x')), std::string(""));
}

TEST(auth_salt_makes_hashes_differ) {
  Fixture f;
  f.auth.set_password("same password");
  const std::string first = f.platform.documents["auth.json"];
  f.auth.set_password("same password");
  CHECK(f.platform.documents["auth.json"] != first);
}

TEST(auth_stored_document_has_no_plain_password) {
  Fixture f;
  f.auth.set_password("a good password");
  const std::string& document = f.platform.documents["auth.json"];
  CHECK(!document.empty());
  CHECK(!contains(document, "a good password"));
  CHECK(contains(document, "pbkdf2-sha256$2$"));
}

TEST(auth_save_failure_is_reported_and_changes_nothing) {
  Fixture f;
  f.platform.fail_saves = true;
  CHECK(!f.auth.set_password("a good password").empty());
  CHECK(!f.auth.is_configured());
}

TEST(auth_password_survives_a_restart_but_sessions_do_not) {
  Fixture f;
  f.auth.set_password("a good password");
  const std::string token = f.auth.make_token();
  CHECK(f.auth.verify_token(token));

  AuthService restarted(f.platform, kFastIterations);
  restarted.load();
  CHECK(restarted.is_configured());
  CHECK(restarted.verify_password("a good password"));
  CHECK(!restarted.verify_token(token));              // a restart signs everyone out
  CHECK(restarted.verify_token(restarted.make_token()));
}

TEST(auth_token_expires) {
  Fixture f;
  f.auth.set_password("a good password");
  const std::string token = f.auth.make_token();
  f.platform.mono += kSessionTtlS - 1;
  CHECK(f.auth.verify_token(token));
  f.platform.mono += 2;
  CHECK(!f.auth.verify_token(token));
}

TEST(auth_tampered_or_garbage_tokens_are_rejected) {
  Fixture f;
  f.auth.set_password("a good password");
  const std::string token = f.auth.make_token();
  const size_t first_dot = token.find('.');
  const size_t last_dot = token.rfind('.');
  const std::string expiry = token.substr(0, first_dot);
  const std::string nonce = token.substr(first_dot + 1, last_dot - first_dot - 1);
  const std::string signature = token.substr(last_dot + 1);
  const std::string later_expiry = std::to_string(std::stol(expiry) + 999);
  for (const std::string& bad : {std::string(""), std::string("a.b"), std::string("a.b.c"), std::string("..."),
                                 later_expiry + "." + nonce + "." + signature,
                                 expiry + "." + nonce + "." + signature.substr(0, signature.size() - 2) + "xx",
                                 token + "x", token + ".extra"}) {
    CHECK(!f.auth.verify_token(bad));
  }
}

TEST(auth_changing_the_password_ends_old_sessions) {
  Fixture f;
  f.auth.set_password("a good password");
  const std::string token = f.auth.make_token();
  f.auth.set_password("another password");
  CHECK(!f.auth.verify_token(token));
  CHECK(f.auth.verify_token(f.auth.make_token()));
  CHECK(!f.auth.verify_password("a good password"));
  CHECK(f.auth.verify_password("another password"));
}

TEST(auth_corrupt_or_foreign_stored_hashes_never_verify) {
  for (const char* document : {"{}", "{\"password\": \"\"}", "{\"password\": \"x\"}", "not json",
                               "{\"password\": \"md5$1$ab$cd\"}", "{\"password\": \"pbkdf2-sha256$0$ab$cd\"}",
                               "{\"password\": \"pbkdf2-sha256$2$zz$zz\"}", "{\"password\": 5}"}) {
    test_support::FakePlatform platform;
    platform.documents["auth.json"] = document;
    AuthService auth(platform, kFastIterations);
    auth.load();
    CHECK(!auth.verify_password("anything"));
  }
}

TEST(auth_stored_iteration_count_wins_over_the_current_setting) {
  test_support::FakePlatform platform;
  AuthService first(platform, 3);
  first.load();
  first.set_password("a good password");
  AuthService second(platform, 7);  // a newer firmware would use more iterations
  second.load();
  CHECK(second.verify_password("a good password"));
}

// ---- login throttling ----

namespace {
struct LimiterFixture {
  double now = 0.0;
  LoginLimiter limiter{[this] { return now; }};
  void fail(const std::string& client, int times) {
    for (int i = 0; i < times; ++i) {
      limiter.record_failure(client);
      now += 1;
    }
  }
};
}  // namespace

TEST(limiter_allows_a_few_failures) {
  LimiterFixture f;
  f.fail("1.2.3.4", LoginLimiter::kMaxFailures - 1);
  CHECK_EQ(f.limiter.retry_after("1.2.3.4"), 0.0);
}

TEST(limiter_blocks_after_too_many_failures_then_recovers) {
  LimiterFixture f;
  f.fail("1.2.3.4", LoginLimiter::kMaxFailures);
  CHECK(f.limiter.retry_after("1.2.3.4") > 0.0);
  f.now += LoginLimiter::kBlockS + 1;
  CHECK_EQ(f.limiter.retry_after("1.2.3.4"), 0.0);
}

TEST(limiter_clients_are_independent) {
  LimiterFixture f;
  f.fail("1.2.3.4", LoginLimiter::kMaxFailures);
  CHECK_EQ(f.limiter.retry_after("5.6.7.8"), 0.0);
}

TEST(limiter_success_clears_the_failures) {
  LimiterFixture f;
  f.fail("1.2.3.4", LoginLimiter::kMaxFailures - 1);
  f.limiter.record_success("1.2.3.4");
  f.fail("1.2.3.4", LoginLimiter::kMaxFailures - 1);
  CHECK_EQ(f.limiter.retry_after("1.2.3.4"), 0.0);
}

TEST(limiter_tracked_clients_are_bounded) {
  LimiterFixture f;
  for (size_t i = 0; i < LoginLimiter::kMaxClients + 40; ++i) f.limiter.record_failure("10.0.0." + std::to_string(i));
  CHECK(f.limiter.tracked_clients() <= LoginLimiter::kMaxClients);
}

TEST(limiter_old_failures_age_out) {
  LimiterFixture f;
  f.fail("1.2.3.4", LoginLimiter::kMaxFailures - 1);
  f.now += LoginLimiter::kWindowS + 1;
  // The old failures no longer count, so one more does not reach the limit.
  f.limiter.record_failure("1.2.3.4");
  CHECK_EQ(f.limiter.retry_after("1.2.3.4"), 0.0);
  // And the emptied bookkeeping of an idle client is released.
  f.now += LoginLimiter::kWindowS + 1;
  CHECK_EQ(f.limiter.retry_after("1.2.3.4"), 0.0);
  CHECK_EQ(f.limiter.tracked_clients(), static_cast<size_t>(0));
}

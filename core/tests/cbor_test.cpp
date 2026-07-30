// Tests for the CBOR subset.
//
// The vectors that matter most are the ones lifted verbatim from the Swift suite in
// ios/RemoteCamTests/CBORTests.swift. They pin the two implementations to each other
// rather than each to its author's reading of the spec, which is the only property that
// decides whether a phone can talk to a PC.
//
// Same harness as transform_test.cpp: plain main(), non-zero on failure, no framework.

#include "rc/cbor.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

std::string hex(const std::vector<uint8_t>& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (uint8_t b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0x0F]);
    out.push_back(' ');
  }
  if (!out.empty()) out.pop_back();
  return out;
}

void checkBytes(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want,
                const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    std::printf("  FAIL: %s\n    got  %s\n    want %s\n", what.c_str(), hex(got).c_str(),
                hex(want).c_str());
  }
}

void checkError(rc::cbor::Error got, rc::cbor::Error want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    std::printf("  FAIL: %s (got %s, want %s)\n", what.c_str(), rc::cbor::errorText(got),
                rc::cbor::errorText(want));
  }
}

using rc::cbor::Value;

// ---------------------------------------------------------------------------

void testSwiftByteVectors() {
  std::printf("Byte vectors shared with the Swift suite\n");

  // From CBORTests.testCanonicalMapEncoding: the map encodes identically whichever
  // order the keys were inserted in, and "t" precedes "codec" because it is shorter.
  // This single vector is what proves the length-first rule is implemented.
  rc::cbor::Map ready;
  ready.emplace("codec", Value::text("hevc"));
  ready.emplace("t", Value::text("ready"));
  checkBytes(rc::cbor::encode(Value::map(ready)),
             {0xa2, 0x61, 0x74, 0x65, 0x72, 0x65, 0x61, 0x64, 0x79, 0x65, 0x63, 0x6f, 0x64,
              0x65, 0x63, 0x64, 0x68, 0x65, 0x76, 0x63},
             "{t:\"ready\", codec:\"hevc\"} matches the Swift encoding");

  rc::cbor::Map reversed;
  reversed.emplace("t", Value::text("ready"));
  reversed.emplace("codec", Value::text("hevc"));
  checkBytes(rc::cbor::encode(Value::map(reversed)), rc::cbor::encode(Value::map(ready)),
             "insertion order does not affect the encoding");

  Value decoded;
  checkError(rc::cbor::decode({0xf9, 0x3e, 0x00}, decoded), rc::cbor::Error::None,
             "binary16 decodes");
  double d = 0.0;
  check(decoded.asDouble(d) && d == 1.5, "0xf9 0x3e 0x00 is 1.5");

  checkError(rc::cbor::decode({0xfa, 0x42, 0x48, 0x00, 0x00}, decoded), rc::cbor::Error::None,
             "binary32 decodes");
  check(decoded.asDouble(d) && d == 50.0, "0xfa 0x42 0x48 0x00 0x00 is 50.0");

  checkError(rc::cbor::decode({0x01, 0x02}, decoded), rc::cbor::Error::TrailingBytes,
             "trailing bytes are rejected");
  checkError(rc::cbor::decode({0xa2, 0x61, 0x74, 0x01, 0x61, 0x74, 0x02}, decoded),
             rc::cbor::Error::DuplicateMapKey, "a duplicate map key is rejected");
}

void testCanonicalKeyOrder() {
  std::printf("Canonical key order is length-first, not bytewise\n");

  // The case where RFC 7049 and RFC 8949 disagree. Under the modern rule "aaa" would
  // come first; the phone expects "zz". Getting this backwards would produce a codec
  // that round-trips perfectly against itself and fails against every real device.
  rc::cbor::Map m;
  m.emplace("aaa", Value::unsignedInt(1));
  m.emplace("zz", Value::unsignedInt(2));
  const std::vector<uint8_t> encoded = rc::cbor::encode(Value::map(m));
  checkBytes(encoded, {0xa2, 0x62, 0x7a, 0x7a, 0x02, 0x63, 0x61, 0x61, 0x61, 0x01},
             "shorter key sorts first even when it is bytewise greater");

  // Within one length, plain unsigned bytewise order.
  rc::cbor::Map same;
  same.emplace("bb", Value::unsignedInt(2));
  same.emplace("aa", Value::unsignedInt(1));
  checkBytes(rc::cbor::encode(Value::map(same)),
             {0xa2, 0x62, 0x61, 0x61, 0x01, 0x62, 0x62, 0x62, 0x02},
             "equal-length keys sort bytewise");

  // High bytes must compare unsigned. A signed char comparison would put "\xff" before
  // "a", and the bug would only appear for non-ASCII keys.
  rc::cbor::Map wide;
  wide.emplace("\x7f", Value::unsignedInt(1));
  wide.emplace("\x01", Value::unsignedInt(2));
  const rc::cbor::Map* readBack = nullptr;
  Value out;
  rc::cbor::decode(rc::cbor::encode(Value::map(wide)), out);
  check(out.asMap(readBack) && readBack->begin()->first == std::string("\x01"),
        "byte comparison is unsigned");
}

void testIntegers() {
  std::printf("Integers\n");

  struct Case {
    uint64_t value;
    std::vector<uint8_t> bytes;
  };
  // Shortest-form heads at every boundary, which is where an off-by-one lives.
  const Case cases[] = {
      {0, {0x00}},
      {23, {0x17}},
      {24, {0x18, 0x18}},
      {255, {0x18, 0xff}},
      {256, {0x19, 0x01, 0x00}},
      {65535, {0x19, 0xff, 0xff}},
      {65536, {0x1a, 0x00, 0x01, 0x00, 0x00}},
      {4294967295ull, {0x1a, 0xff, 0xff, 0xff, 0xff}},
      {4294967296ull, {0x1b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}},
      {UINT64_MAX, {0x1b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
  };
  for (const Case& c : cases) {
    checkBytes(rc::cbor::encode(Value::unsignedInt(c.value)), c.bytes,
               "unsigned " + std::to_string(c.value) + " uses the shortest head");
    Value decoded;
    uint64_t got = 0;
    checkError(rc::cbor::decode(c.bytes, decoded), rc::cbor::Error::None, "decodes");
    check(decoded.asUnsigned(got) && got == c.value,
          "unsigned " + std::to_string(c.value) + " round-trips");
  }

  // Negatives, including the extremes where -1-n arithmetic overflows if written the
  // obvious way.
  const struct {
    int64_t value;
    std::vector<uint8_t> bytes;
  } negatives[] = {
      {-1, {0x20}},
      {-24, {0x37}},
      {-25, {0x38, 0x18}},
      {-42, {0x38, 0x29}},
      {INT64_MIN, {0x3b, 0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}},
  };
  for (const auto& c : negatives) {
    checkBytes(rc::cbor::encode(Value::negativeInt(c.value)), c.bytes,
               "negative " + std::to_string(c.value) + " encodes");
    Value decoded;
    int64_t got = 0;
    checkError(rc::cbor::decode(c.bytes, decoded), rc::cbor::Error::None, "decodes");
    check(decoded.asNegative(got) && got == c.value,
          "negative " + std::to_string(c.value) + " round-trips");
  }

  // A CBOR negative whose magnitude exceeds int64 is representable on the wire but not
  // in our model, and must be refused rather than wrapped.
  Value decoded;
  checkError(rc::cbor::decode({0x3b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, decoded),
             rc::cbor::Error::IntegerOverflow, "an out-of-range negative is refused");

  // Non-shortest heads are accepted on decode, matching Swift. Re-encoding normalises
  // them, which is exactly why decode-then-encode is not byte-preserving.
  checkError(rc::cbor::decode({0x18, 0x01}, decoded), rc::cbor::Error::None,
             "a non-shortest head is accepted");
  checkBytes(rc::cbor::encode(decoded), {0x01}, "re-encoding normalises it");
}

void testStringsAndUtf8() {
  std::printf("Strings and UTF-8 validation\n");

  Value decoded;
  checkError(rc::cbor::decode({0x63, 0xe2, 0x82, 0xac}, decoded), rc::cbor::Error::None,
             "a euro sign decodes");
  const std::string* text = nullptr;
  check(decoded.asText(text) && text->size() == 3, "multi-byte text keeps its bytes");

  // Each of these is a classic way to slip a byte sequence past a naive validator.
  const struct {
    std::vector<uint8_t> bytes;
    const char* what;
  } invalid[] = {
      {{0x62, 0xc3, 0x28}, "a bad continuation byte"},
      {{0x62, 0xc0, 0xaf}, "an overlong two-byte form"},
      {{0x63, 0xe0, 0x80, 0xaf}, "an overlong three-byte form"},
      {{0x63, 0xed, 0xa0, 0x80}, "a UTF-16 surrogate"},
      {{0x64, 0xf5, 0x80, 0x80, 0x80}, "a code point above U+10FFFF"},
      {{0x61, 0x80}, "a lone continuation byte"},
  };
  for (const auto& c : invalid) {
    checkError(rc::cbor::decode(c.bytes, decoded), rc::cbor::Error::InvalidUtf8,
               std::string(c.what) + " is rejected");
  }

  // Byte strings are not text and are not validated.
  checkError(rc::cbor::decode({0x42, 0xc3, 0x28}, decoded), rc::cbor::Error::None,
             "the same bytes are fine in a byte string");
}

void testRejections() {
  std::printf("Bounded and hostile input\n");

  Value decoded;
  checkError(rc::cbor::decode({0xc0, 0x00}, decoded),
             rc::cbor::Error::InvalidAdditionalInformation, "a tag is rejected");
  checkError(rc::cbor::decode({0x9f, 0x01, 0xff}, decoded),
             rc::cbor::Error::InvalidAdditionalInformation, "an indefinite array is rejected");
  checkError(rc::cbor::decode({0xbf, 0x61, 0x74, 0x01, 0xff}, decoded),
             rc::cbor::Error::InvalidAdditionalInformation, "an indefinite map is rejected");
  checkError(rc::cbor::decode({0xf7}, decoded), rc::cbor::Error::UnsupportedSimpleValue,
             "undefined is rejected");
  checkError(rc::cbor::decode({0xf8, 0x20}, decoded), rc::cbor::Error::UnsupportedSimpleValue,
             "a one-byte simple value is rejected");
  checkError(rc::cbor::decode({0xa1, 0x01, 0x02}, decoded), rc::cbor::Error::UnsupportedMapKey,
             "an integer map key is rejected");
  checkError(rc::cbor::decode({}, decoded), rc::cbor::Error::Truncated, "empty input");
  checkError(rc::cbor::decode({0x19, 0x01}, decoded), rc::cbor::Error::Truncated,
             "a head with a missing argument byte");

  // A declared length far beyond the buffer must fail on the length check, not by
  // trying to allocate it.
  checkError(rc::cbor::decode({0x5b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, decoded),
             rc::cbor::Error::Truncated, "a huge byte-string length allocates nothing");
  checkError(rc::cbor::decode({0x7b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, decoded),
             rc::cbor::Error::Truncated, "a huge text length allocates nothing");

  // Collection counts are checked before any element is read, so this is O(1) and not
  // an invitation to spin.
  checkError(rc::cbor::decode({0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, decoded),
             rc::cbor::Error::CollectionTooLarge, "an absurd array count is refused up front");
  checkError(rc::cbor::decode({0xbb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, decoded),
             rc::cbor::Error::CollectionTooLarge, "an absurd map count is refused up front");
  checkError(rc::cbor::decode({0x9a, 0x00, 0x01, 0x86, 0xa1}, decoded),
             rc::cbor::Error::CollectionTooLarge, "100,001 elements is one too many");

  // Nesting is bounded so a deeply nested payload cannot exhaust the stack. Depth 32 is
  // allowed and 33 is not, matching Swift.
  auto nestedArrays = [](int depth) {
    std::vector<uint8_t> bytes(static_cast<size_t>(depth), 0x81);
    bytes.push_back(0x00);
    return bytes;
  };
  checkError(rc::cbor::decode(nestedArrays(32), decoded), rc::cbor::Error::None,
             "32 levels of nesting is allowed");
  checkError(rc::cbor::decode(nestedArrays(33), decoded),
             rc::cbor::Error::NestingLimitExceeded, "33 levels is refused");
}

void testRoundTrip() {
  std::printf("Round-trip over every type\n");

  rc::cbor::Map inner;
  inner.emplace("flag", Value::boolean(true));
  inner.emplace("nothing", Value::null());

  rc::cbor::Map root;
  root.emplace("t", Value::text("caps"));
  root.emplace("n", Value::unsignedInt(65536));
  root.emplace("neg", Value::negativeInt(-42));
  root.emplace("raw", Value::bytes({0x00, 0x01, 0x02, 0xff}));
  root.emplace("emoji", Value::text("hello \xF0\x9F\x93\xB7"));
  root.emplace("real", Value::real(1.0 / 3.0));
  root.emplace("list", Value::array({Value::unsignedInt(1), Value::boolean(false),
                                     Value::real(-0.5), Value::map(inner)}));

  const Value original = Value::map(root);
  const std::vector<uint8_t> bytes = rc::cbor::encode(original);

  Value decoded;
  checkError(rc::cbor::decode(bytes, decoded), rc::cbor::Error::None, "the payload decodes");
  check(decoded == original, "every type survives a round trip");
  checkBytes(rc::cbor::encode(decoded), bytes, "re-encoding is stable for canonical input");

  // Sweep: every representable double must survive binary64 exactly, including the ones
  // that a shortest-float encoder would quietly reshape.
  const double doubles[] = {0.0,   -0.0,      1.0,      -1.0,    0.5,     1e-300,
                            1e300, 3.14159265358979, 5600.0,  125.0,   1.0 / 60.0};
  for (double d : doubles) {
    Value back;
    const std::vector<uint8_t> encoded = rc::cbor::encode(Value::real(d));
    check(encoded.size() == 9 && encoded[0] == 0xfb, "doubles always encode as binary64");
    checkError(rc::cbor::decode(encoded, back), rc::cbor::Error::None, "double decodes");
    double got = 0.0;
    check(back.asDouble(got) && got == d, "double survives exactly");
  }
}

void testAccessorStrictness() {
  std::printf("Accessor strictness\n");

  // The strict readers are what reject a phone sending a width as a double. Loosening
  // them would silently accept configurations the validator was meant to catch.
  uint64_t u = 0;
  check(!Value::real(1080.0).asUnsigned(u), "a double is not an unsigned");
  check(!Value::negativeInt(-1).asUnsigned(u), "a negative is not an unsigned");
  check(Value::unsignedInt(1080).asUnsigned(u) && u == 1080, "an unsigned is");

  // numericDouble is the one lenient reader, and it still refuses non-finite values.
  double d = 0.0;
  check(Value::unsignedInt(3).numericDouble(d) && d == 3.0, "numericDouble takes unsigned");
  check(Value::negativeInt(-3).numericDouble(d) && d == -3.0, "numericDouble takes negative");
  check(Value::real(2.5).numericDouble(d) && d == 2.5, "numericDouble takes double");
  check(!Value::text("3").numericDouble(d), "numericDouble does not parse strings");

  Value nan;
  rc::cbor::decode({0xf9, 0x7e, 0x00}, nan);
  check(!nan.numericDouble(d), "NaN is refused");
  Value inf;
  rc::cbor::decode({0xf9, 0x7c, 0x00}, inf);
  check(!inf.numericDouble(d), "infinity is refused");
  double raw = 0.0;
  check(inf.asDouble(raw) && std::isinf(raw) && raw > 0,
        "binary16 infinity widens to double infinity");
}

}  // namespace

int main() {
  testSwiftByteVectors();
  testCanonicalKeyOrder();
  testIntegers();
  testStringsAndUtf8();
  testRejections();
  testRoundTrip();
  testAccessorStrictness();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

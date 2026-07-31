// RemoteCam — the CBOR subset the wire protocol uses.
//
// This is not a general CBOR library and should not become one. It is the C++ half of a
// codec whose other half already ships in `ios/RemoteCam/Sources/Wire/CBOR.swift`, and
// its job is to produce and accept exactly the same bytes. Where this file and
// docs/protocol.md appear to disagree, the Swift source is the tiebreaker: it is what
// is running on real phones.
//
// THREE ASYMMETRIES THAT LOOK LIKE BUGS AND ARE NOT
//
// 1. Map keys sort by encoded LENGTH FIRST, then bytewise -- the RFC 7049 canonical
//    rule, not RFC 8949 §4.2.1's plain bytewise order. `{"zz":…,"aaa":…}` puts "zz"
//    first here and "aaa" first under the modern rule. docs/ios-backend-handoff.md
//    describes this incorrectly; CBOR.swift's canonicalKeyOrder is authoritative.
//    Ordering lives in the map's comparator rather than in a sort step at encode time,
//    so determinism is structural and cannot be forgotten at a call site.
//
// 2. Encoding emits binary64 for every double; decoding also accepts binary16 and
//    binary32. Integer and length heads must use their shortest form on both sides;
//    this removes alternate authenticated encodings for the same v1 value.
//
// 3. Everything is bounded, because this parses bytes from the network before anything
//    has authenticated them. Depth, element counts and total size are all capped, and
//    a declared length is checked against the remaining input before a single byte is
//    allocated.
//
// No exceptions and no allocation-on-error: failures come back as `Error`.

#ifndef RC_CBOR_H
#define RC_CBOR_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rc::cbor {

// Mirrors CBORError in CBOR.swift one-for-one, so a failure can be compared across the
// two implementations when they disagree about a stream.
enum class Error {
  None = 0,
  Truncated,
  InvalidAdditionalInformation,
  NonMinimalInteger,
  InvalidUtf8,
  UnsupportedMapKey,
  UnsupportedSimpleValue,
  NestingLimitExceeded,
  TrailingBytes,
  IntegerOverflow,
  DuplicateMapKey,
  CollectionTooLarge,
};

const char* errorText(Error error);

// Matches CBOR.swift: nesting through depth 32, and a map key costs a level just as a
// value does.
inline constexpr int kMaxNestingDepth = 32;
inline constexpr uint64_t kMaxCollectionElements = 100000;

// Length-first, then unsigned bytewise. See asymmetry 1 above.
struct CanonicalKeyLess {
  using is_transparent = void;
  bool operator()(const std::string& lhs, const std::string& rhs) const {
    if (lhs.size() != rhs.size()) return lhs.size() < rhs.size();
    return lhs.compare(rhs) < 0;
  }
};

class Value;
using Map = std::map<std::string, Value, CanonicalKeyLess>;
using Array = std::vector<Value>;

class Value {
 public:
  enum class Type { Unsigned, Negative, Bytes, Text, Array, Map, Boolean, Null, Double };

  Value() : type_(Type::Null) {}

  static Value unsignedInt(uint64_t v);
  // `v` is the value itself, e.g. -42. Encoding writes the CBOR representation
  // (-1 - v); callers never see that form.
  static Value negativeInt(int64_t v);
  static Value bytes(std::vector<uint8_t> v);
  static Value text(std::string v);
  static Value array(Array v);
  static Value map(Map v);
  static Value boolean(bool v);
  static Value null();
  static Value real(double v);

  Type type() const { return type_; }

  // Accessors return false when the type does not match. Deliberately strict, matching
  // Swift's `unsignedValue` -- a double-encoded width is a protocol violation, not a
  // number to coerce. `numericDouble` is the one lenient reader, mirroring AppModel's,
  // for fields the phone may send as either.
  bool asUnsigned(uint64_t& out) const;
  bool asNegative(int64_t& out) const;
  bool asBytes(const std::vector<uint8_t>*& out) const;
  bool asText(const std::string*& out) const;
  bool asArray(const Array*& out) const;
  bool asMap(const Map*& out) const;
  bool asBoolean(bool& out) const;
  bool asDouble(double& out) const;
  bool numericDouble(double& out) const;

  bool isNull() const { return type_ == Type::Null; }
  bool operator==(const Value& rhs) const;
  bool operator!=(const Value& rhs) const { return !(*this == rhs); }

 private:
  Type type_;
  uint64_t unsigned_ = 0;
  int64_t negative_ = 0;
  double double_ = 0.0;
  bool boolean_ = false;
  std::vector<uint8_t> bytes_;
  std::string text_;
  Array array_;
  Map map_;
};

// Encoding cannot fail: every representable Value has an encoding. Bytes are appended,
// so a caller can build a payload in place.
void encode(const Value& value, std::vector<uint8_t>& out);
std::vector<uint8_t> encode(const Value& value);

// Decodes exactly one top-level item and requires the input to end there -- trailing
// bytes are an error, as they are in Swift. Returns Error::None on success.
Error decode(const uint8_t* data, size_t size, Value& out);
Error decode(const std::vector<uint8_t>& data, Value& out);

}  // namespace rc::cbor

#endif  // RC_CBOR_H

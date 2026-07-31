#include "rc/cbor.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace rc::cbor {
namespace {

constexpr uint8_t kMajorShift = 5;
constexpr uint8_t kAdditionalMask = 0x1F;

constexpr uint8_t kMajorUnsigned = 0;
constexpr uint8_t kMajorNegative = 1;
constexpr uint8_t kMajorBytes = 2;
constexpr uint8_t kMajorText = 3;
constexpr uint8_t kMajorArray = 4;
constexpr uint8_t kMajorMap = 5;
constexpr uint8_t kMajorTag = 6;
constexpr uint8_t kMajorSimple = 7;

void appendBigEndian(std::vector<uint8_t>& out, uint64_t value, int bytes) {
  for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
  }
}

// Shortest-form head, matching Swift's appendMajor. Decoding is deliberately more
// permissive; see the header.
void appendHead(std::vector<uint8_t>& out, uint8_t major, uint64_t argument) {
  const uint8_t prefix = static_cast<uint8_t>(major << kMajorShift);
  if (argument < 24) {
    out.push_back(static_cast<uint8_t>(prefix | argument));
  } else if (argument <= 0xFFull) {
    out.push_back(static_cast<uint8_t>(prefix | 24));
    out.push_back(static_cast<uint8_t>(argument));
  } else if (argument <= 0xFFFFull) {
    out.push_back(static_cast<uint8_t>(prefix | 25));
    appendBigEndian(out, argument, 2);
  } else if (argument <= 0xFFFFFFFFull) {
    out.push_back(static_cast<uint8_t>(prefix | 26));
    appendBigEndian(out, argument, 4);
  } else {
    out.push_back(static_cast<uint8_t>(prefix | 27));
    appendBigEndian(out, argument, 8);
  }
}

// IEEE binary16 -> double. Handles subnormals, infinities and NaN; a naive
// implementation gets subnormals wrong and they do occur in encoders that pick the
// shortest float that round-trips.
double halfToDouble(uint16_t half) {
  const uint16_t sign = static_cast<uint16_t>((half >> 15) & 0x1u);
  const uint16_t exponent = static_cast<uint16_t>((half >> 10) & 0x1Fu);
  const uint16_t mantissa = static_cast<uint16_t>(half & 0x3FFu);

  double magnitude = 0.0;
  if (exponent == 0) {
    magnitude = static_cast<double>(mantissa) * 5.9604644775390625e-8;  // 2^-24
  } else if (exponent == 0x1F) {
    // Any quiet NaN will do; the payload is not preserved across widths anyway.
    magnitude = mantissa == 0 ? std::numeric_limits<double>::infinity()
                              : std::numeric_limits<double>::quiet_NaN();
  } else {
    double scale = 1.0;
    int power = static_cast<int>(exponent) - 15;
    for (; power > 0; --power) scale *= 2.0;
    for (; power < 0; ++power) scale *= 0.5;
    magnitude = (1.0 + static_cast<double>(mantissa) / 1024.0) * scale;
  }
  return sign ? -magnitude : magnitude;
}

float bitsToFloat(uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

double bitsToDouble(uint64_t bits) {
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

uint64_t doubleToBits(double value) {
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  size_t remaining() const { return size_ - offset_; }
  size_t offset() const { return offset_; }

  Error readByte(uint8_t& out) {
    if (remaining() < 1) return Error::Truncated;
    out = data_[offset_++];
    return Error::None;
  }

  Error readBigEndian(int bytes, uint64_t& out) {
    if (remaining() < static_cast<size_t>(bytes)) return Error::Truncated;
    uint64_t value = 0;
    for (int i = 0; i < bytes; ++i) value = (value << 8) | data_[offset_ + static_cast<size_t>(i)];
    offset_ += static_cast<size_t>(bytes);
    out = value;
    return Error::None;
  }

  // Checked against the input that is actually present before anything is sized from
  // it, so a hostile declared length cannot turn into a huge allocation.
  Error readBytes(uint64_t count, const uint8_t*& out) {
    if (count > remaining()) return Error::Truncated;
    out = data_ + offset_;
    offset_ += static_cast<size_t>(count);
    return Error::None;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t offset_ = 0;
};

// Reads the argument that follows a major type. V1 is deterministic: integer and
// length arguments must use the shortest possible head. Indefinite lengths (31) and
// the reserved 28-30 are rejected as well.
Error readArgument(Reader& reader, uint8_t additional, uint64_t& out) {
  if (additional < 24) {
    out = additional;
    return Error::None;
  }
  switch (additional) {
    case 24: {
      uint8_t byte = 0;
      const Error err = reader.readByte(byte);
      if (err != Error::None) return err;
      out = byte;
      return out >= 24 ? Error::None : Error::NonMinimalInteger;
    }
    case 25: {
      const Error err = reader.readBigEndian(2, out);
      if (err != Error::None) return err;
      return out > 0xffu ? Error::None : Error::NonMinimalInteger;
    }
    case 26: {
      const Error err = reader.readBigEndian(4, out);
      if (err != Error::None) return err;
      return out > 0xffffu ? Error::None : Error::NonMinimalInteger;
    }
    case 27: {
      const Error err = reader.readBigEndian(8, out);
      if (err != Error::None) return err;
      return out > 0xffffffffull ? Error::None : Error::NonMinimalInteger;
    }
    default: return Error::InvalidAdditionalInformation;
  }
}

Error decodeValue(Reader& reader, int depth, Value& out);

Error decodeCollectionCount(Reader& reader, uint8_t additional, uint64_t& count) {
  const Error err = readArgument(reader, additional, count);
  if (err != Error::None) return err;
  if (count > kMaxCollectionElements) return Error::CollectionTooLarge;
  return Error::None;
}

// Text strings are validated here rather than trusted. An invalid sequence that reaches
// a std::string is a problem for whatever logs or forwards it later, and by then the
// origin is lost.
bool isValidUtf8(const uint8_t* data, size_t size) {
  size_t i = 0;
  while (i < size) {
    const uint8_t lead = data[i];
    size_t extra = 0;
    uint32_t code = 0;
    if (lead < 0x80) {
      i += 1;
      continue;
    } else if ((lead & 0xE0) == 0xC0) {
      extra = 1;
      code = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
      extra = 2;
      code = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
      extra = 3;
      code = lead & 0x07u;
    } else {
      return false;
    }
    if (i + extra >= size) return false;
    for (size_t k = 1; k <= extra; ++k) {
      const uint8_t cont = data[i + k];
      if ((cont & 0xC0) != 0x80) return false;
      code = (code << 6) | (cont & 0x3Fu);
    }
    // Overlong forms, surrogates and out-of-range code points are all invalid, and all
    // three are classic ways to smuggle a byte sequence past a naive validator.
    if (extra == 1 && code < 0x80) return false;
    if (extra == 2 && code < 0x800) return false;
    if (extra == 3 && code < 0x10000) return false;
    if (code > 0x10FFFF) return false;
    if (code >= 0xD800 && code <= 0xDFFF) return false;
    i += extra + 1;
  }
  return true;
}

Error decodeValue(Reader& reader, int depth, Value& out) {
  if (depth > kMaxNestingDepth) return Error::NestingLimitExceeded;

  uint8_t initial = 0;
  Error err = reader.readByte(initial);
  if (err != Error::None) return err;

  const uint8_t major = static_cast<uint8_t>(initial >> kMajorShift);
  const uint8_t additional = static_cast<uint8_t>(initial & kAdditionalMask);

  switch (major) {
    case kMajorUnsigned: {
      uint64_t value = 0;
      err = readArgument(reader, additional, value);
      if (err != Error::None) return err;
      out = Value::unsignedInt(value);
      return Error::None;
    }
    case kMajorNegative: {
      uint64_t value = 0;
      err = readArgument(reader, additional, value);
      if (err != Error::None) return err;
      // CBOR stores -1-n; anything past int64 range cannot be represented.
      if (value > static_cast<uint64_t>(INT64_MAX)) return Error::IntegerOverflow;
      out = Value::negativeInt(-1 - static_cast<int64_t>(value));
      return Error::None;
    }
    case kMajorBytes: {
      uint64_t count = 0;
      err = readArgument(reader, additional, count);
      if (err != Error::None) return err;
      const uint8_t* start = nullptr;
      err = reader.readBytes(count, start);
      if (err != Error::None) return err;
      out = Value::bytes(std::vector<uint8_t>(start, start + count));
      return Error::None;
    }
    case kMajorText: {
      uint64_t count = 0;
      err = readArgument(reader, additional, count);
      if (err != Error::None) return err;
      const uint8_t* start = nullptr;
      err = reader.readBytes(count, start);
      if (err != Error::None) return err;
      if (!isValidUtf8(start, static_cast<size_t>(count))) return Error::InvalidUtf8;
      out = Value::text(std::string(reinterpret_cast<const char*>(start),
                                    static_cast<size_t>(count)));
      return Error::None;
    }
    case kMajorArray: {
      uint64_t count = 0;
      err = decodeCollectionCount(reader, additional, count);
      if (err != Error::None) return err;
      Array items;
      items.reserve(static_cast<size_t>(count < 1024 ? count : 1024));
      for (uint64_t i = 0; i < count; ++i) {
        Value item;
        err = decodeValue(reader, depth + 1, item);
        if (err != Error::None) return err;
        items.push_back(std::move(item));
      }
      out = Value::array(std::move(items));
      return Error::None;
    }
    case kMajorMap: {
      uint64_t count = 0;
      err = decodeCollectionCount(reader, additional, count);
      if (err != Error::None) return err;
      Map entries;
      for (uint64_t i = 0; i < count; ++i) {
        Value key;
        err = decodeValue(reader, depth + 1, key);
        if (err != Error::None) return err;
        const std::string* keyText = nullptr;
        if (!key.asText(keyText)) return Error::UnsupportedMapKey;
        Value item;
        err = decodeValue(reader, depth + 1, item);
        if (err != Error::None) return err;
        // Duplicates are rejected rather than last-wins: two values for one key means
        // the sender and receiver already disagree about the message.
        if (!entries.emplace(*keyText, std::move(item)).second) return Error::DuplicateMapKey;
      }
      out = Value::map(std::move(entries));
      return Error::None;
    }
    case kMajorTag:
      // No tag is part of this protocol, and accepting one would mean deciding what it
      // means. Swift reports the additional-information byte here too.
      return Error::InvalidAdditionalInformation;
    case kMajorSimple:
      switch (additional) {
        case 20: out = Value::boolean(false); return Error::None;
        case 21: out = Value::boolean(true); return Error::None;
        case 22: out = Value::null(); return Error::None;
        case 25: {
          uint64_t bits = 0;
          err = reader.readBigEndian(2, bits);
          if (err != Error::None) return err;
          out = Value::real(halfToDouble(static_cast<uint16_t>(bits)));
          return Error::None;
        }
        case 26: {
          uint64_t bits = 0;
          err = reader.readBigEndian(4, bits);
          if (err != Error::None) return err;
          out = Value::real(static_cast<double>(bitsToFloat(static_cast<uint32_t>(bits))));
          return Error::None;
        }
        case 27: {
          uint64_t bits = 0;
          err = reader.readBigEndian(8, bits);
          if (err != Error::None) return err;
          out = Value::real(bitsToDouble(bits));
          return Error::None;
        }
        default:
          // Covers undefined (23), the one-byte simple form (24) and break (31).
          return Error::UnsupportedSimpleValue;
      }
    default:
      return Error::InvalidAdditionalInformation;
  }
}

}  // namespace

const char* errorText(Error error) {
  switch (error) {
    case Error::None: return "none";
    case Error::Truncated: return "truncated";
    case Error::InvalidAdditionalInformation: return "invalid additional information";
    case Error::NonMinimalInteger: return "non-minimal integer or length";
    case Error::InvalidUtf8: return "invalid utf-8";
    case Error::UnsupportedMapKey: return "unsupported map key";
    case Error::UnsupportedSimpleValue: return "unsupported simple value";
    case Error::NestingLimitExceeded: return "nesting limit exceeded";
    case Error::TrailingBytes: return "trailing bytes";
    case Error::IntegerOverflow: return "integer overflow";
    case Error::DuplicateMapKey: return "duplicate map key";
    case Error::CollectionTooLarge: return "collection too large";
  }
  return "unknown";
}

Value Value::unsignedInt(uint64_t v) {
  Value value;
  value.type_ = Type::Unsigned;
  value.unsigned_ = v;
  return value;
}

Value Value::negativeInt(int64_t v) {
  Value value;
  value.type_ = Type::Negative;
  value.negative_ = v;
  return value;
}

Value Value::bytes(std::vector<uint8_t> v) {
  Value value;
  value.type_ = Type::Bytes;
  value.bytes_ = std::move(v);
  return value;
}

Value Value::text(std::string v) {
  Value value;
  value.type_ = Type::Text;
  value.text_ = std::move(v);
  return value;
}

Value Value::array(Array v) {
  Value value;
  value.type_ = Type::Array;
  value.array_ = std::move(v);
  return value;
}

Value Value::map(Map v) {
  Value value;
  value.type_ = Type::Map;
  value.map_ = std::move(v);
  return value;
}

Value Value::boolean(bool v) {
  Value value;
  value.type_ = Type::Boolean;
  value.boolean_ = v;
  return value;
}

Value Value::null() { return Value(); }

Value Value::real(double v) {
  Value value;
  value.type_ = Type::Double;
  value.double_ = v;
  return value;
}

bool Value::asUnsigned(uint64_t& out) const {
  if (type_ != Type::Unsigned) return false;
  out = unsigned_;
  return true;
}

bool Value::asNegative(int64_t& out) const {
  if (type_ != Type::Negative) return false;
  out = negative_;
  return true;
}

bool Value::asBytes(const std::vector<uint8_t>*& out) const {
  if (type_ != Type::Bytes) return false;
  out = &bytes_;
  return true;
}

bool Value::asText(const std::string*& out) const {
  if (type_ != Type::Text) return false;
  out = &text_;
  return true;
}

bool Value::asArray(const Array*& out) const {
  if (type_ != Type::Array) return false;
  out = &array_;
  return true;
}

bool Value::asMap(const Map*& out) const {
  if (type_ != Type::Map) return false;
  out = &map_;
  return true;
}

bool Value::asBoolean(bool& out) const {
  if (type_ != Type::Boolean) return false;
  out = boolean_;
  return true;
}

bool Value::asDouble(double& out) const {
  if (type_ != Type::Double) return false;
  out = double_;
  return true;
}

bool Value::numericDouble(double& out) const {
  switch (type_) {
    case Type::Double:
      // Non-finite values are rejected, matching AppModel.numericDouble. A NaN zoom
      // would propagate into the transform matrix and blank the output.
      if (!std::isfinite(double_)) return false;
      out = double_;
      return true;
    case Type::Unsigned: out = static_cast<double>(unsigned_); return true;
    case Type::Negative: out = static_cast<double>(negative_); return true;
    default: return false;
  }
}

bool Value::operator==(const Value& rhs) const {
  if (type_ != rhs.type_) return false;
  switch (type_) {
    case Type::Unsigned: return unsigned_ == rhs.unsigned_;
    case Type::Negative: return negative_ == rhs.negative_;
    case Type::Bytes: return bytes_ == rhs.bytes_;
    case Type::Text: return text_ == rhs.text_;
    case Type::Array: return array_ == rhs.array_;
    case Type::Map: return map_ == rhs.map_;
    case Type::Boolean: return boolean_ == rhs.boolean_;
    case Type::Null: return true;
    case Type::Double: return double_ == rhs.double_;
  }
  return false;
}

void encode(const Value& value, std::vector<uint8_t>& out) {
  switch (value.type()) {
    case Value::Type::Unsigned: {
      uint64_t v = 0;
      value.asUnsigned(v);
      appendHead(out, kMajorUnsigned, v);
      return;
    }
    case Value::Type::Negative: {
      int64_t v = 0;
      value.asNegative(v);
      // -1-v as an unsigned, computed so that v == INT64_MIN does not overflow.
      const uint64_t encoded = static_cast<uint64_t>(-(v + 1));
      appendHead(out, kMajorNegative, encoded);
      return;
    }
    case Value::Type::Bytes: {
      const std::vector<uint8_t>* v = nullptr;
      value.asBytes(v);
      appendHead(out, kMajorBytes, v->size());
      out.insert(out.end(), v->begin(), v->end());
      return;
    }
    case Value::Type::Text: {
      const std::string* v = nullptr;
      value.asText(v);
      appendHead(out, kMajorText, v->size());
      out.insert(out.end(), v->begin(), v->end());
      return;
    }
    case Value::Type::Array: {
      const Array* v = nullptr;
      value.asArray(v);
      appendHead(out, kMajorArray, v->size());
      for (const Value& item : *v) encode(item, out);
      return;
    }
    case Value::Type::Map: {
      const Map* v = nullptr;
      value.asMap(v);
      appendHead(out, kMajorMap, v->size());
      // std::map iterates in comparator order, which is the canonical order. No sort
      // step exists to be forgotten.
      for (const auto& entry : *v) {
        appendHead(out, kMajorText, entry.first.size());
        out.insert(out.end(), entry.first.begin(), entry.first.end());
        encode(entry.second, out);
      }
      return;
    }
    case Value::Type::Boolean: {
      bool v = false;
      value.asBoolean(v);
      out.push_back(static_cast<uint8_t>((kMajorSimple << kMajorShift) | (v ? 21u : 20u)));
      return;
    }
    case Value::Type::Null:
      out.push_back(static_cast<uint8_t>((kMajorSimple << kMajorShift) | 22u));
      return;
    case Value::Type::Double: {
      double v = 0.0;
      value.asDouble(v);
      // Always binary64. Swift does the same, and picking the shortest float that
      // round-trips would change bytes the other side already expects.
      out.push_back(static_cast<uint8_t>((kMajorSimple << kMajorShift) | 27u));
      appendBigEndian(out, doubleToBits(v), 8);
      return;
    }
  }
}

std::vector<uint8_t> encode(const Value& value) {
  std::vector<uint8_t> out;
  encode(value, out);
  return out;
}

Error decode(const uint8_t* data, size_t size, Value& out) {
  if (data == nullptr && size != 0) return Error::Truncated;
  Reader reader(data, size);
  Value value;
  const Error err = decodeValue(reader, 0, value);
  if (err != Error::None) return err;
  if (reader.remaining() != 0) return Error::TrailingBytes;
  out = std::move(value);
  return Error::None;
}

Error decode(const std::vector<uint8_t>& data, Value& out) {
  return decode(data.data(), data.size(), out);
}

}  // namespace rc::cbor

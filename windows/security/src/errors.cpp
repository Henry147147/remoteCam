#include "rcsecurity/security.h"

namespace rcsecurity {

const char* errorText(Error error) {
  switch (error) {
    case Error::None: return "none";
    case Error::Unavailable: return "required cryptography is unavailable";
    case Error::InvalidArgument: return "invalid argument";
    case Error::CryptoFailure: return "cryptographic operation failed";
    case Error::InvalidPoint: return "invalid group element";
    case Error::AuthenticationFailed: return "authentication failed";
    case Error::SequenceMismatch: return "unexpected sequence number";
    case Error::SequenceExhausted: return "sequence space exhausted";
    case Error::Expired: return "attempt expired";
    case Error::RateLimited: return "attempt rate limited";
    case Error::NotFound: return "record not found";
    case Error::StorageFailure: return "secure storage failed";
    case Error::CorruptRecord: return "secure record is corrupt";
  }
  return "unknown";
}

}  // namespace rcsecurity

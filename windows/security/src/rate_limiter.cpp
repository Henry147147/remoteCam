#include "rcsecurity/security.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace rcsecurity {

AttemptPolicy::AttemptPolicy(AttemptPolicyConfig config) : config_(std::move(config)) {}

size_t AttemptPolicy::peerAttempts(std::string_view peer) const {
  return static_cast<size_t>(std::count_if(
      attempts_.begin(), attempts_.end(),
      [&](const Attempt& attempt) { return attempt.peer == peer; }));
}

void AttemptPolicy::prune(TimePoint now) const {
  // A completed global cooldown starts a clean window. This avoids a five-minute
  // cooldown immediately retriggering from the same ten-minute history.
  if (globalBlockedUntil_.has_value() && now >= *globalBlockedUntil_) {
    attempts_.clear();
    peerBlockedUntil_.clear();
    globalBlockedUntil_.reset();
    return;
  }

  for (auto it = peerBlockedUntil_.begin(); it != peerBlockedUntil_.end();) {
    if (now < it->second) {
      ++it;
      continue;
    }
    const std::string peer = it->first;
    std::erase_if(attempts_, [&](const Attempt& attempt) { return attempt.peer == peer; });
    it = peerBlockedUntil_.erase(it);
  }

  const TimePoint cutoff = now - config_.failureWindow;
  std::erase_if(attempts_, [&](const Attempt& attempt) { return attempt.started < cutoff; });
}

bool AttemptPolicy::allowed(std::string_view peer, TimePoint now) const {
  if (peer.empty() || config_.maxAttemptsPerSource == 0 ||
      config_.maxAttemptsGlobal == 0) {
    return false;
  }
  prune(now);
  if (globalBlockedUntil_.has_value() ||
      peerBlockedUntil_.contains(std::string(peer))) {
    return false;
  }
  return attempts_.size() < config_.maxAttemptsGlobal &&
         peerAttempts(peer) < config_.maxAttemptsPerSource;
}

Error AttemptPolicy::beginAttempt(std::string peer, TimePoint now, AttemptId& id) {
  if (!allowed(peer, now) || nextAttemptId_ == std::numeric_limits<AttemptId>::max()) {
    return Error::RateLimited;
  }
  id = nextAttemptId_++;
  attempts_.push_back(Attempt{id, std::move(peer), now});
  const std::string& chargedPeer = attempts_.back().peer;
  if (peerAttempts(chargedPeer) >= config_.maxAttemptsPerSource) {
    peerBlockedUntil_.insert_or_assign(chargedPeer, now + config_.cooldown);
  }
  if (attempts_.size() >= config_.maxAttemptsGlobal) {
    globalBlockedUntil_ = now + config_.cooldown;
  }
  return Error::None;
}

void AttemptPolicy::finishAttempt(AttemptId id, bool succeeded, TimePoint now) {
  prune(now);
  const auto it = std::find_if(attempts_.begin(), attempts_.end(),
                               [&](const Attempt& attempt) { return attempt.id == id; });
  if (it == attempts_.end() || !succeeded) return;

  const std::string peer = it->peer;
  attempts_.erase(it);
  if (peerAttempts(peer) < config_.maxAttemptsPerSource) {
    peerBlockedUntil_.erase(peer);
  }
  if (attempts_.size() < config_.maxAttemptsGlobal) {
    globalBlockedUntil_.reset();
  }
}

bool AttemptPolicy::pairingExpired(TimePoint started, TimePoint now) const {
  return now - started >= config_.pairingLifetime;
}

bool AttemptPolicy::authenticationExpired(TimePoint started, TimePoint now) const {
  return now - started >= config_.authenticationLifetime;
}

}  // namespace rcsecurity

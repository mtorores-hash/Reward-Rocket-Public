#pragma once

#include <stdint.h>

// Portable one-Hall controller contract. Hardware, network and storage are
// supplied by the ESP adapter; this header performs no I/O itself.
namespace rr334 {

static const uint32_t kStepIntervalUs = 5000;
static const uint32_t kInputDebounceMs = 20;
static const uint32_t kDefaultDwellMs = 1000;
static const uint16_t kMaximumCountedJog = 256;
static const uint32_t kDefaultSeekLimitSteps = 12000;
static const uint32_t kDefaultSeekTimeoutMs = 90000;
static const uint32_t kDefaultJogLeaseMs = 500;
static const uint32_t kMaximumJogLeaseMs = 1000;
static const uint32_t kMaximumMotionGapUs = 250000;

enum class Mode : uint8_t { Recovery, Locked, Armed, Busy };
enum class Credit : uint8_t { None, Pending, Consumed };
enum class Result : uint8_t {
  Ok, Conflict, NotReady, Invalid, StorageFailure, Busy, Limit, HallFault
};
enum class Motion : uint8_t {
  None, StepPositive, StepNegative, ReleaseCoils, RewardComplete, Fault
};
enum class Run : uint8_t {
  Idle, SeekHall, StartupOffset, JogCounted, JogLease, RewardToDelivery,
  HallCheck, DeliveryCheck, DeliveryOffset, DeliveryDwell, RewardToPickup, PickupCheck,
  PickupDwell, Fault
};

struct Config {
  int32_t pickupSteps;       // Signed halfsteps relative to Hall datum (0).
  int32_t dropoffSteps;     // Signed halfsteps relative to Hall datum (0).
  uint32_t stepIntervalUs;   // Validated 3000..20000; default 5000.
  uint32_t deliveryDwellMs; // 0..60000; default 1000.
  uint32_t pickupDwellMs;   // 0..60000; default 1000.
  uint32_t seekLimitSteps;  // Explicit bounded homing configuration.
  uint32_t seekTimeoutMs;
  bool pickupTaught;
  bool dropoffTaught;
  bool commissioned;
};

struct Snapshot {
  uint32_t revision; // Incremented for every durable mutation; never zero.
  Config config;
  Mode mode;
  Credit credit;
  bool positionValid;
  int32_t positionSteps; // Signed offset from Hall datum.
  bool rewardInProgress; // Durable consumed/busy marker, written before motion.
};

struct Status {
  Snapshot saved;
  Run run;
  bool rawHallActive;
  bool stableHallActive;
  bool armed;
  uint32_t remainingSteps;
  int8_t direction;
  uint32_t bootNonce; // Runtime only; never persisted or treated as a secret.
  bool storageGood;
};

// The adapter must atomically write, read back, and validate the full record.
// Returning false gates motion and leaves the controller in Recovery.
using Persist = bool (*)(const Snapshot& next, void* context);

class Controller {
 public:
  Controller(Persist persist, void* context);

  // Invalid records and persisted Busy become Recovery. Valid records are
  // automatically referenced at the one Hall datum; no reward is dispensed.
  // bootNonce changes on every boot and is included in every phone mutation.
  void boot(bool recordValid, const Snapshot& stored, uint32_t bootNonce,
            bool rawHallActive, bool buttonDown, uint32_t nowMs);
  const Status& status() const;

  Result setPendingCredit(uint32_t nonce, uint32_t expectedRevision, bool parentAuthorized);
  Result arm(uint32_t nonce, uint32_t expectedRevision, uint32_t nowMs,
             bool rawHallActive, bool stableButtonReleased);
  Result cancelPending(uint32_t nonce, uint32_t expectedRevision);
  Result reconcile(uint32_t nonce, uint32_t expectedRevision,
                   bool parentConfirmsHallDatum, bool rawHallActive);

  Result configure(uint32_t nonce, uint32_t expectedRevision, const Config& config);
  Result startCountedJog(uint32_t nonce, uint32_t expectedRevision, int8_t direction,
                         uint16_t steps, bool hallActive, uint32_t nowUs, uint32_t nowMs);
  Result goPickup(uint32_t nonce, uint32_t expectedRevision, uint32_t nowUs,
                  uint32_t nowMs, bool hallActive);
  Result renewJog(uint32_t nonce, uint32_t expectedRevision, int8_t direction,
                  uint16_t leaseMs, uint32_t nowMs, bool hallActive);
  Result startSeek(uint32_t nonce, uint32_t expectedRevision, int8_t direction,
                   uint32_t nowUs, uint32_t nowMs, bool hallActive);
  Result teachPickup(uint32_t nonce, uint32_t expectedRevision, bool parentConfirmed,
                     bool hallActive);
  Result teachDropoff(uint32_t nonce, uint32_t expectedRevision, bool parentConfirmed,
                      bool hallActive);
  Result startReward(uint32_t nonce, uint32_t expectedRevision, uint32_t nowUs,
                     uint32_t nowMs, bool hallActive, bool buttonFreshPress);
  Result stop(uint32_t nonce, uint32_t expectedRevision);
  // Used only by guarded OTA preparation when no run is active.
  Result releaseOutputs(uint32_t nonce, uint32_t expectedRevision);

  // Called frequently by the main loop. Hall is checked before every emitted
  // step. Returned step action is applied once; reportStepWritten() commits
  // the in-RAM open-loop count/position only after the GPIO phase write.
  Motion tick(uint32_t nowUs, uint32_t nowMs, bool rawHallActive,
              bool stableHallActive);
  void reportStepWritten();
  void reportStepWriteFailure();
  bool updateButton(bool down, bool stableReleased, uint32_t nowMs);

 private:
  bool persist(const Snapshot& next);
  bool validMutation(uint32_t nonce, uint32_t expectedRevision) const;
  void enterFault();
  void refreshStatus(bool rawHallActive, bool stableHallActive);

  Persist persist_;
  void* context_;
  Snapshot saved_;
  Status status_;
  Run run_;
  uint32_t bootNonce_;
  uint32_t armStartedMs_;
  uint32_t lastTickUs_;
  uint32_t lastStepUs_;
  uint32_t deadlineMs_;
  uint32_t dwellStartedMs_;
  uint32_t jogLeaseUntilMs_;
  uint32_t seekSteps_;
  uint32_t legSteps_;
  uint32_t legTargetSteps_;
  int8_t direction_;
  bool stepPending_;
  bool buttonReleasedSeen_;
  bool freshPressPending_;
  bool hallWindowHandled_;
  bool applyOffsetAfterSeek_;
  bool rewardSeekActive_;
  bool pickupAfterHallCheck_;
  bool storageGood_;
  bool legPositionWasKnown_;
  bool pickupMoveActive_;
};

inline Controller::Controller(Persist persistFn, void* context)
    : persist_(persistFn), context_(context), bootNonce_(0), armStartedMs_(0),
      lastTickUs_(0), lastStepUs_(0), deadlineMs_(0), dwellStartedMs_(0),
      jogLeaseUntilMs_(0), seekSteps_(0), legSteps_(0), legTargetSteps_(0),
      direction_(0), stepPending_(false), buttonReleasedSeen_(false),
      freshPressPending_(false), hallWindowHandled_(false),
      applyOffsetAfterSeek_(false), rewardSeekActive_(false),
      pickupAfterHallCheck_(false), storageGood_(false),
      legPositionWasKnown_(false), pickupMoveActive_(false) {
  saved_ = {};
  saved_.mode = Mode::Recovery;
  saved_.credit = Credit::None;
  status_ = {};
  run_ = Run::Idle;
}

inline void Controller::boot(bool recordValid, const Snapshot& stored, uint32_t bootNonce,
                             bool rawHallActive, bool buttonDown, uint32_t nowMs) {
  bootNonce_ = bootNonce ? bootNonce : 1;
  rewardSeekActive_ = false;
  pickupAfterHallCheck_ = false;
  applyOffsetAfterSeek_ = false;
  storageGood_ = recordValid;
  saved_ = recordValid ? stored : Snapshot{};
  if (!recordValid) {
    saved_.mode = Mode::Recovery;
    saved_.credit = Credit::None;
    saved_.revision = 0;
    saved_.config.stepIntervalUs = kStepIntervalUs;
    saved_.config.deliveryDwellMs = kDefaultDwellMs;
    saved_.config.pickupDwellMs = kDefaultDwellMs;
    saved_.config.seekLimitSteps = kDefaultSeekLimitSteps;
    saved_.config.seekTimeoutMs = kDefaultSeekTimeoutMs;
  }
  // Boot never moves the mechanism. Only the commissioned, durable pickup
  // coordinate is trusted across power loss. Every move first durably clears
  // positionValid, so a reset during motion stays in Recovery.
  const bool interrupted = recordValid &&
      (stored.rewardInProgress || stored.credit == Credit::Consumed || stored.mode == Mode::Busy);
  const bool learnedPickup = recordValid && stored.config.commissioned &&
      stored.config.pickupTaught && stored.positionValid &&
      stored.positionSteps == stored.config.pickupSteps;
  const bool cleanSavedPosition = learnedPickup && !interrupted &&
      stored.mode != Mode::Recovery;
  const bool hallDatumCanRestore = recordValid && rawHallActive && !interrupted &&
      stored.mode != Mode::Recovery && stored.credit != Credit::Consumed &&
      (!stored.positionValid || stored.positionSteps == 0);
  saved_.rewardInProgress = recordValid && stored.rewardInProgress;
  saved_.mode = (!recordValid || !storageGood_ || interrupted ||
                 stored.mode == Mode::Recovery ||
                 (!cleanSavedPosition && !hallDatumCanRestore))
                    ? Mode::Recovery : Mode::Locked;
  saved_.positionValid = false;
  run_ = Run::Idle;
  direction_ = 0;
  legSteps_ = legTargetSteps_ = seekSteps_ = 0;
  stepPending_ = false;
  legPositionWasKnown_ = false;
  pickupMoveActive_ = false;
  rewardSeekActive_ = false;
  pickupAfterHallCheck_ = false;
  applyOffsetAfterSeek_ = false;
  hallWindowHandled_ = rawHallActive;
  if (cleanSavedPosition) {
    if (rawHallActive && stored.positionSteps != 0) {
      // Hall-active contradicts the saved nonzero coordinate; stay still.
      saved_.mode = Mode::Recovery;
    } else {
      saved_.positionValid = true;
      saved_.positionSteps = stored.positionSteps;
    }
  } else if (hallDatumCanRestore) {
    // An active Hall may establish the datum at boot, but never triggers an
    // offset move or a reward.
    saved_.positionValid = true;
    saved_.positionSteps = 0;
  }
  armStartedMs_ = nowMs;
  lastTickUs_ = lastStepUs_ = 0;
  dwellStartedMs_ = jogLeaseUntilMs_ = 0;
  buttonReleasedSeen_ = !buttonDown;
  freshPressPending_ = false;
  refreshStatus(rawHallActive, rawHallActive);
  status_.storageGood = storageGood_;
}

inline const Status& Controller::status() const { return status_; }

inline bool Controller::validMutation(uint32_t nonce, uint32_t expectedRevision) const {
  return nonce == bootNonce_ && expectedRevision == saved_.revision && storageGood_;
}

inline bool Controller::persist(const Snapshot& next) {
  Snapshot candidate = next;
  candidate.revision = saved_.revision + 1;
  if (candidate.revision == 0) candidate.revision = 1;
  if (!persist_ || !persist_(candidate, context_)) {
    storageGood_ = false;
    saved_.mode = Mode::Recovery;
    run_ = Run::Fault;
    stepPending_ = false;
    status_.storageGood = false;
    status_.saved = saved_;
    status_.run = run_;
    return false;
  }
  saved_ = candidate;
  storageGood_ = true;
  refreshStatus(status_.rawHallActive, status_.stableHallActive);
  return true;
}

inline void Controller::refreshStatus(bool rawHallActive, bool stableHallActive) {
  status_.saved = saved_;
  status_.run = run_;
  status_.rawHallActive = rawHallActive;
  status_.stableHallActive = stableHallActive;
  status_.armed = saved_.mode == Mode::Armed && saved_.credit == Credit::Pending;
  status_.remainingSteps = legTargetSteps_ > legSteps_ ? legTargetSteps_ - legSteps_ : 0;
  status_.direction = direction_;
  status_.bootNonce = bootNonce_;
  status_.storageGood = storageGood_;
}

inline Result Controller::setPendingCredit(uint32_t nonce, uint32_t expectedRevision,
                                           bool parentAuthorized) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (!parentAuthorized) return Result::NotReady;
  if (saved_.mode != Mode::Locked || saved_.credit != Credit::None ||
      saved_.rewardInProgress) return Result::Conflict;
  Snapshot next = saved_;
  next.credit = Credit::Pending;
  next.mode = Mode::Locked;
  return persist(next) ? Result::Ok : Result::StorageFailure;
}

inline Result Controller::arm(uint32_t nonce, uint32_t expectedRevision, uint32_t nowMs,
                              bool rawHallActive, bool stableButtonReleased) {
  (void)nowMs;
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (saved_.mode != Mode::Locked || saved_.credit != Credit::Pending ||
      saved_.rewardInProgress) return Result::Conflict;
  if (!saved_.config.commissioned || !saved_.positionValid ||
      !saved_.config.pickupTaught || !saved_.config.dropoffTaught ||
      saved_.positionSteps != saved_.config.pickupSteps) return Result::NotReady;
  if (rawHallActive && saved_.positionSteps != 0) return Result::HallFault;
  buttonReleasedSeen_ = stableButtonReleased;
  Snapshot next = saved_;
  next.mode = Mode::Armed;
  return persist(next) ? Result::Ok : Result::StorageFailure;
}

inline Result Controller::cancelPending(uint32_t nonce, uint32_t expectedRevision) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if ((saved_.mode != Mode::Locked && saved_.mode != Mode::Armed) ||
      saved_.credit != Credit::Pending || saved_.rewardInProgress) return Result::Conflict;
  Snapshot next = saved_;
  next.mode = Mode::Locked;
  next.credit = Credit::None;
  return persist(next) ? Result::Ok : Result::StorageFailure;
}

inline Result Controller::reconcile(uint32_t nonce, uint32_t expectedRevision,
                                    bool parentConfirmedHallDatum, bool rawHallActive) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (saved_.mode != Mode::Recovery || !parentConfirmedHallDatum || !rawHallActive)
    return Result::NotReady;
  Snapshot next = saved_;
  next.positionValid = true;
  next.positionSteps = 0;
  next.rewardInProgress = false; // Consumed credit remains consumed after recovery.
  next.mode = Mode::Locked;
  if (next.credit == Credit::Consumed) next.credit = Credit::None;
  if (!persist(next)) return Result::StorageFailure;
  run_ = Run::Idle;
  refreshStatus(rawHallActive, rawHallActive);
  return Result::Ok;
}

inline Result Controller::configure(uint32_t nonce, uint32_t expectedRevision,
                                   const Config& config) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (run_ != Run::Idle || saved_.rewardInProgress ||
      (saved_.credit == Credit::Pending && saved_.mode == Mode::Armed)) return Result::Busy;
  if (config.stepIntervalUs < 3000 || config.stepIntervalUs > 20000 ||
      config.deliveryDwellMs > 60000 || config.pickupDwellMs > 60000 ||
      config.seekLimitSteps == 0 || config.seekLimitSteps > 100000 ||
      config.seekTimeoutMs < 1000 || config.seekTimeoutMs > 600000 ||
      (config.pickupTaught && config.pickupSteps == 0) ||
      (config.pickupTaught && (static_cast<int64_t>(config.pickupSteps) > config.seekLimitSteps ||
                               static_cast<int64_t>(config.pickupSteps) < -static_cast<int64_t>(config.seekLimitSteps))) ||
      (config.dropoffTaught && (static_cast<int64_t>(config.dropoffSteps) > config.seekLimitSteps ||
                                static_cast<int64_t>(config.dropoffSteps) < -static_cast<int64_t>(config.seekLimitSteps))) ||
      (config.pickupTaught && config.dropoffTaught &&
       config.pickupSteps == config.dropoffSteps) ||
      (config.commissioned && (!config.pickupTaught || !config.dropoffTaught)))
    return Result::Invalid;
  Snapshot next = saved_;
  next.config = config;
  // A changed setup invalidates a previously unused authorization. Boot
  // positioning and goPickup are the only setup exceptions.
  if (next.credit == Credit::Pending) next.credit = Credit::None;
  next.mode = Mode::Locked;
  return persist(next) ? Result::Ok : Result::StorageFailure;
}

inline Result Controller::startCountedJog(uint32_t nonce, uint32_t expectedRevision,
                                          int8_t direction, uint16_t steps,
                                          bool hallActive, uint32_t nowUs, uint32_t nowMs) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (direction != 1 && direction != -1) return Result::Invalid;
  if (!steps || steps > kMaximumCountedJog) return Result::Limit;
  if (run_ != Run::Idle || saved_.rewardInProgress || saved_.mode == Mode::Busy)
    return Result::Busy;
  if (hallActive && saved_.positionValid && saved_.positionSteps != 0)
    return Result::HallFault;
  legPositionWasKnown_ = saved_.positionValid;
  Snapshot next = saved_;
  next.positionValid = false;
  if (next.credit == Credit::Pending) { next.mode = Mode::Locked; next.credit = Credit::None; }
  if (!persist(next)) { legPositionWasKnown_ = false; return Result::StorageFailure; }
  pickupMoveActive_ = false;
  run_ = Run::JogCounted; direction_ = direction; legSteps_ = 0;
  legTargetSteps_ = steps; lastStepUs_ = nowUs; lastTickUs_ = nowUs;
  deadlineMs_ = nowMs + (static_cast<uint32_t>(steps) * saved_.config.stepIntervalUs / 1000u + 5000u);
  stepPending_ = false;
  hallWindowHandled_ = hallActive;
  refreshStatus(hallActive, hallActive);
  return Result::Ok;
}

inline Result Controller::goPickup(uint32_t nonce, uint32_t expectedRevision,
                                   uint32_t nowUs, uint32_t nowMs, bool hallActive) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (run_ != Run::Idle || !saved_.config.pickupTaught || saved_.rewardInProgress)
    return Result::NotReady;
  if (hallActive && saved_.positionValid && saved_.positionSteps != 0 && !hallWindowHandled_)
    return Result::HallFault;
  if (!saved_.positionValid) return Result::NotReady;
  const int64_t delta = static_cast<int64_t>(saved_.config.pickupSteps) - saved_.positionSteps;
  const uint32_t target = static_cast<uint32_t>(delta > 0 ? delta : -delta);
  if (target > saved_.config.seekLimitSteps) return Result::Limit;
  Snapshot next = saved_;
  next.positionValid = false;
  if (next.mode == Mode::Armed && next.credit == Credit::Pending) {
    next.mode = Mode::Locked; next.credit = Credit::None;
  }
  if (!delta) {
    next.positionValid = true;
    next.positionSteps = next.config.pickupSteps;
    return persist(next) ? Result::Ok : Result::StorageFailure;
  }
  if (!persist(next)) return Result::StorageFailure;
  legPositionWasKnown_ = true;
  pickupMoveActive_ = true;
  direction_ = delta > 0 ? 1 : -1;
  run_ = Run::JogCounted; legSteps_ = 0;
  legTargetSteps_ = target;
  lastStepUs_ = lastTickUs_ = nowUs;
  deadlineMs_ = nowMs + saved_.config.seekTimeoutMs;
  stepPending_ = false;
  refreshStatus(hallActive, hallActive);
  return Result::Ok;
}

inline Result Controller::renewJog(uint32_t nonce, uint32_t expectedRevision,
                                   int8_t direction, uint16_t leaseMs,
                                   uint32_t nowMs, bool hallActive) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if ((direction != 1 && direction != -1) || leaseMs == 0 || leaseMs > kMaximumJogLeaseMs)
    return Result::Invalid;
  if (saved_.rewardInProgress || saved_.mode == Mode::Busy) return Result::Busy;
  if (hallActive && saved_.positionValid && saved_.positionSteps != 0 && !hallWindowHandled_)
    return Result::HallFault;
  if (run_ != Run::JogLease) {
    if (run_ != Run::Idle) return Result::Busy;
    legPositionWasKnown_ = saved_.positionValid;
    Snapshot next = saved_;
    next.positionValid = false;
    if (next.credit == Credit::Pending) { next.mode = Mode::Locked; next.credit = Credit::None; }
    if (!persist(next)) { legPositionWasKnown_ = false; return Result::StorageFailure; }
    pickupMoveActive_ = false;
    run_ = Run::JogLease; legSteps_ = 0; lastTickUs_ = lastStepUs_ = 0;
  }
  direction_ = direction;
  jogLeaseUntilMs_ = nowMs + leaseMs;
  deadlineMs_ = jogLeaseUntilMs_;
  hallWindowHandled_ = hallActive;
  refreshStatus(hallActive, hallActive);
  return Result::Ok;
}

inline Result Controller::startSeek(uint32_t nonce, uint32_t expectedRevision,
                                    int8_t direction, uint32_t nowUs, uint32_t nowMs,
                                    bool hallActive) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (direction != 1 && direction != -1) return Result::Invalid;
  if (run_ != Run::Idle || saved_.rewardInProgress || saved_.mode == Mode::Busy)
    return Result::Busy;
  if (hallActive) return Result::HallFault;
  Snapshot next = saved_;
  next.positionValid = false;
  if (next.credit == Credit::Pending) { next.mode = Mode::Locked; next.credit = Credit::None; }
  if (!persist(next)) return Result::StorageFailure;
  legPositionWasKnown_ = false;
  pickupMoveActive_ = false;
  run_ = Run::SeekHall; direction_ = direction; seekSteps_ = 0;
  rewardSeekActive_ = false;
  pickupAfterHallCheck_ = false;
  applyOffsetAfterSeek_ = false;
  lastStepUs_ = lastTickUs_ = nowUs;
  deadlineMs_ = nowMs + saved_.config.seekTimeoutMs;
  legTargetSteps_ = saved_.config.seekLimitSteps;
  legSteps_ = 0; stepPending_ = false;
  hallWindowHandled_ = false;
  refreshStatus(hallActive, hallActive);
  return Result::Ok;
}

inline Result Controller::teachPickup(uint32_t nonce, uint32_t expectedRevision,
                                      bool parentConfirmed, bool hallActive) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (!parentConfirmed || run_ != Run::Idle || saved_.rewardInProgress ||
      !saved_.positionValid || (hallActive && saved_.positionSteps != 0)) return Result::NotReady;
  Snapshot next = saved_;
  next.positionValid = true;
  next.positionSteps = saved_.positionSteps;
  next.config.pickupSteps = saved_.positionSteps;
  if (next.config.dropoffTaught && next.config.dropoffSteps == next.config.pickupSteps)
    return Result::Invalid;
  next.config.pickupTaught = true;
  next.config.commissioned = next.config.dropoffTaught;
  next.credit = next.credit == Credit::Pending ? Credit::None : next.credit;
  next.mode = Mode::Locked;
  return persist(next) ? Result::Ok : Result::StorageFailure;
}

inline Result Controller::teachDropoff(uint32_t nonce, uint32_t expectedRevision,
                                       bool parentConfirmed, bool hallActive) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (!parentConfirmed || run_ != Run::Idle || saved_.rewardInProgress ||
      !saved_.positionValid || (hallActive && saved_.positionSteps != 0)) return Result::NotReady;
  Snapshot next = saved_;
  next.config.dropoffSteps = saved_.positionSteps;
  if (next.config.pickupTaught && next.config.pickupSteps == next.config.dropoffSteps)
    return Result::Invalid;
  next.config.dropoffTaught = true;
  next.config.commissioned = next.config.pickupTaught;
  next.credit = next.credit == Credit::Pending ? Credit::None : next.credit;
  next.mode = Mode::Locked;
  return persist(next) ? Result::Ok : Result::StorageFailure;
}

inline Result Controller::startReward(uint32_t nonce, uint32_t expectedRevision,
                                      uint32_t nowUs, uint32_t nowMs, bool hallActive,
                                      bool buttonFreshPress) {
  (void)nowMs;
  // Physical button event is local; network nonce/revision do not authorize it.
  (void)nonce; (void)expectedRevision;
  if (!buttonFreshPress || !freshPressPending_ || run_ != Run::Idle || saved_.mode != Mode::Armed ||
      saved_.credit != Credit::Pending || !saved_.config.commissioned ||
      !saved_.config.pickupTaught || !saved_.config.dropoffTaught ||
      !saved_.positionValid || saved_.positionSteps != saved_.config.pickupSteps ||
      saved_.rewardInProgress) return Result::NotReady;
  freshPressPending_ = false;
  applyOffsetAfterSeek_ = false;
  Snapshot next = saved_;
  next.mode = Mode::Busy;
  next.credit = Credit::Consumed;
  next.rewardInProgress = true;
  next.positionValid = false;
  if (!persist(next)) return Result::StorageFailure;
  // Every reward performs a bounded physical Hall search. Counted distance
  // alone cannot compensate bearing drift or validate the delivery datum.
  run_ = Run::SeekHall;
  rewardSeekActive_ = true;
  hallWindowHandled_ = hallActive;
  direction_ = saved_.positionSteps < 0 ? 1 : -1;
  seekSteps_ = 0;
  legSteps_ = 0;
  legTargetSteps_ = saved_.config.seekLimitSteps;
  lastTickUs_ = lastStepUs_ = nowUs;
  dwellStartedMs_ = nowMs;
  deadlineMs_ = nowMs + saved_.config.seekTimeoutMs;
  stepPending_ = false;
  refreshStatus(hallActive, hallActive);
  return Result::Ok;
}

inline Result Controller::stop(uint32_t nonce, uint32_t expectedRevision) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (saved_.rewardInProgress && saved_.credit == Credit::Consumed) {
    Snapshot next = saved_; next.mode = Mode::Recovery; next.positionValid = false;
    if (!persist(next)) return Result::StorageFailure;
  } else {
    Snapshot next = saved_; next.mode = Mode::Recovery; next.positionValid = false;
    if (next.credit == Credit::Pending) next.credit = Credit::None;
    if (!persist(next)) return Result::StorageFailure;
  }
  run_ = Run::Idle; stepPending_ = false; direction_ = 0;
  rewardSeekActive_ = false;
  pickupAfterHallCheck_ = false;
  legPositionWasKnown_ = false;
  pickupMoveActive_ = false;
  legSteps_ = legTargetSteps_ = 0;
  refreshStatus(status_.rawHallActive, status_.stableHallActive);
  return Result::Ok;
}

inline Result Controller::releaseOutputs(uint32_t nonce, uint32_t expectedRevision) {
  if (!validMutation(nonce, expectedRevision)) return Result::Conflict;
  if (run_ != Run::Idle || saved_.mode != Mode::Locked ||
      saved_.credit != Credit::None || saved_.rewardInProgress) return Result::Busy;
  Snapshot next = saved_;
  next.mode = Mode::Recovery;
  next.positionValid = false;
  return persist(next) ? Result::Ok : Result::StorageFailure;
}

inline Motion Controller::tick(uint32_t nowUs, uint32_t nowMs, bool rawHallActive,
                               bool stableHallActive) {
  refreshStatus(rawHallActive, stableHallActive);
  if (run_ == Run::Idle || run_ == Run::Fault) return Motion::None;
  if (stepPending_) return Motion::None; // Adapter must report the previous GPIO write.
  if (lastTickUs_ && static_cast<uint32_t>(nowUs - lastTickUs_) > kMaximumMotionGapUs) {
    enterFault(); return Motion::Fault;
  }
  lastTickUs_ = nowUs;
  if (run_ == Run::JogLease && static_cast<int32_t>(nowMs - jogLeaseUntilMs_) >= 0) {
    run_ = Run::Idle; direction_ = 0; legSteps_ = 0;
    saved_.positionValid = legPositionWasKnown_;
    legPositionWasKnown_ = false;
    refreshStatus(rawHallActive, stableHallActive);
    return Motion::ReleaseCoils;
  }
  if (run_ != Run::JogLease && static_cast<int32_t>(nowMs - deadlineMs_) >= 0 &&
      run_ != Run::DeliveryDwell && run_ != Run::PickupDwell &&
      run_ != Run::DeliveryCheck && run_ != Run::PickupCheck) {
    enterFault(); return Motion::Fault;
  }

  if (run_ == Run::DeliveryCheck) {
    if (!stableHallActive) {
      if (static_cast<uint32_t>(nowMs - dwellStartedMs_) >= 100) {
        enterFault(); return Motion::Fault;
      }
      return Motion::None;
    }
    const int32_t correction = saved_.config.dropoffTaught ? saved_.config.dropoffSteps : 0;
    if (correction == 0) {
      run_ = Run::DeliveryDwell;
      dwellStartedMs_ = nowMs;
    } else {
      run_ = Run::DeliveryOffset;
      direction_ = correction > 0 ? 1 : -1;
      legSteps_ = 0;
      legTargetSteps_ = correction > 0 ? static_cast<uint32_t>(correction)
                                       : static_cast<uint32_t>(-static_cast<int64_t>(correction));
      lastStepUs_ = nowUs;
      deadlineMs_ = nowMs + saved_.config.seekTimeoutMs;
    }
    return Motion::None;
  }
  if (run_ == Run::PickupCheck) {
    const bool expectedHall = saved_.config.pickupSteps == 0;
    if (stableHallActive != expectedHall) {
      if (static_cast<uint32_t>(nowMs - dwellStartedMs_) >= 100) {
        enterFault(); return Motion::Fault;
      }
      return Motion::None;
    }
    run_ = Run::PickupDwell;
    dwellStartedMs_ = nowMs;
    return Motion::None;
  }

  if (run_ == Run::DeliveryDwell || run_ == Run::PickupDwell) {
    const uint32_t dwell = run_ == Run::DeliveryDwell ? saved_.config.deliveryDwellMs : saved_.config.pickupDwellMs;
    if (static_cast<uint32_t>(nowMs - dwellStartedMs_) < dwell) return Motion::None;
    if (run_ == Run::DeliveryDwell) {
      const int64_t delta = static_cast<int64_t>(saved_.config.pickupSteps) - saved_.positionSteps;
      run_ = delta ? Run::RewardToPickup : Run::PickupDwell;
      direction_ = delta > 0 ? 1 : (delta < 0 ? -1 : 0);
      legSteps_ = 0; legTargetSteps_ = static_cast<uint32_t>(delta > 0 ? delta : -delta);
      lastStepUs_ = nowUs; dwellStartedMs_ = nowMs;
      deadlineMs_ = nowMs + saved_.config.seekTimeoutMs;
      return Motion::None;
    }
    Snapshot next = saved_; next.mode = Mode::Locked; next.credit = Credit::None;
    next.rewardInProgress = false; next.positionValid = true;
    next.positionSteps = saved_.config.pickupSteps;
    if (!persist(next)) { enterFault(); return Motion::Fault; }
    run_ = Run::Idle; direction_ = 0; rewardSeekActive_ = false;
    return Motion::RewardComplete;
  }

  if (run_ == Run::HallCheck) {
    if (!stableHallActive) {
      if (static_cast<uint32_t>(nowMs - dwellStartedMs_) >= 100) {
        enterFault(); return Motion::Fault;
      }
      return Motion::None;
    }
    Snapshot next = saved_; next.positionValid = true; next.positionSteps = 0;
    if (!persist(next)) { enterFault(); return Motion::Fault; }
    hallWindowHandled_ = true;
    if (pickupAfterHallCheck_) {
      pickupAfterHallCheck_ = false;
      const int32_t target = saved_.config.pickupSteps;
      run_ = target == 0 ? Run::PickupCheck : Run::RewardToPickup;
      direction_ = target > 0 ? 1 : (target < 0 ? -1 : 0);
      legSteps_ = 0;
      legTargetSteps_ = target > 0 ? static_cast<uint32_t>(target)
                                   : static_cast<uint32_t>(-static_cast<int64_t>(target));
      lastStepUs_ = nowUs;
      dwellStartedMs_ = nowMs;
      deadlineMs_ = nowMs + saved_.config.seekTimeoutMs;
      refreshStatus(rawHallActive, stableHallActive);
      return Motion::None;
    }
    const int32_t correction = (applyOffsetAfterSeek_ || rewardSeekActive_) && saved_.config.dropoffTaught
                                  ? saved_.config.dropoffSteps : 0;
    applyOffsetAfterSeek_ = false;
    if (correction != 0) {
      run_ = rewardSeekActive_ ? Run::DeliveryOffset : Run::StartupOffset;
      direction_ = correction > 0 ? 1 : -1;
      legSteps_ = 0;
      legTargetSteps_ = correction > 0 ? static_cast<uint32_t>(correction)
                                       : static_cast<uint32_t>(-static_cast<int64_t>(correction));
      lastStepUs_ = nowUs;
      deadlineMs_ = nowMs + saved_.config.seekTimeoutMs;
      refreshStatus(rawHallActive, stableHallActive);
      return Motion::None;
    }
    if (rewardSeekActive_) {
      run_ = Run::DeliveryDwell;
      dwellStartedMs_ = nowMs;
      direction_ = 0;
      legSteps_ = legTargetSteps_ = 0;
      refreshStatus(rawHallActive, stableHallActive);
      return Motion::None;
    }
    run_ = Run::Idle; direction_ = 0; legSteps_ = legTargetSteps_ = 0;
    refreshStatus(rawHallActive, stableHallActive);
    return Motion::ReleaseCoils;
  }
  if (run_ == Run::SeekHall && rawHallActive) {
    run_ = Run::HallCheck;
    dwellStartedMs_ = nowMs;
    hallWindowHandled_ = true;
    refreshStatus(rawHallActive, stableHallActive);
    return Motion::None;
  }
  if (run_ == Run::SeekHall && seekSteps_ >= saved_.config.seekLimitSteps) {
    enterFault(); return Motion::Fault;
  }
  if ((run_ == Run::JogCounted || run_ == Run::StartupOffset || run_ == Run::DeliveryOffset ||
       run_ == Run::RewardToDelivery || run_ == Run::RewardToPickup) &&
      legSteps_ >= legTargetSteps_) {
    if (run_ == Run::StartupOffset) {
      Snapshot next = saved_;
      next.positionValid = true;
      next.positionSteps = saved_.config.dropoffTaught ? saved_.config.dropoffSteps : 0;
      if (!persist(next)) { enterFault(); return Motion::Fault; }
      run_ = Run::Idle; direction_ = 0; legSteps_ = legTargetSteps_ = 0;
      refreshStatus(rawHallActive, stableHallActive);
      return Motion::ReleaseCoils;
    }
    if (run_ == Run::DeliveryOffset) {
      if (saved_.positionSteps != saved_.config.dropoffSteps) { enterFault(); return Motion::Fault; }
      run_ = Run::DeliveryDwell; dwellStartedMs_ = nowMs;
      return Motion::None;
    }
    if (run_ == Run::RewardToDelivery) {
      run_ = Run::DeliveryCheck;
      dwellStartedMs_ = nowMs; return Motion::None;
    }
    if (run_ == Run::RewardToPickup) {
      if (saved_.positionSteps != saved_.config.pickupSteps) { enterFault(); return Motion::Fault; }
      run_ = saved_.config.pickupSteps == 0 ? Run::PickupCheck : Run::PickupDwell;
      dwellStartedMs_ = nowMs; return Motion::None;
    }
    if (run_ == Run::JogCounted && pickupMoveActive_) {
      Snapshot next = saved_;
      next.positionValid = true;
      next.positionSteps = next.config.pickupSteps;
      if (!persist(next)) { enterFault(); return Motion::Fault; }
      pickupMoveActive_ = false;
      legPositionWasKnown_ = false;
    } else if (run_ == Run::JogCounted) {
      saved_.positionValid = legPositionWasKnown_;
      legPositionWasKnown_ = false;
    }
    run_ = Run::Idle; direction_ = 0; legSteps_ = legTargetSteps_ = 0;
    refreshStatus(rawHallActive, stableHallActive);
    return Motion::ReleaseCoils;
  }
  if (lastStepUs_ && static_cast<uint32_t>(nowUs - lastStepUs_) < saved_.config.stepIntervalUs)
    return Motion::None;

  if (!rawHallActive) hallWindowHandled_ = false;
  if (rawHallActive && !hallWindowHandled_) {
    const bool movingTowardDatum = (direction_ > 0 && saved_.positionSteps <= 0) ||
                                   (direction_ < 0 && saved_.positionSteps >= 0);
    if (run_ != Run::SeekHall && !movingTowardDatum) {
      enterFault(); return Motion::Fault;
    }
    saved_.positionValid = true;
    saved_.positionSteps = 0;
    hallWindowHandled_ = true; // Ignore remaining active-window samples until clear.
    if (run_ == Run::RewardToDelivery) {
      run_ = Run::DeliveryCheck;
      direction_ = 0;
      legSteps_ = legTargetSteps_ = 0;
      dwellStartedMs_ = nowMs;
      refreshStatus(rawHallActive, stableHallActive);
      return Motion::None;
    }
    if (run_ == Run::RewardToPickup) {
      run_ = Run::HallCheck;
      pickupAfterHallCheck_ = true;
      direction_ = 0;
      legSteps_ = legTargetSteps_ = 0;
      dwellStartedMs_ = nowMs;
      refreshStatus(rawHallActive, stableHallActive);
      return Motion::None;
    }
  }
  if ((run_ == Run::RewardToDelivery || run_ == Run::RewardToPickup) && !saved_.positionValid) {
    enterFault(); return Motion::Fault;
  }
  stepPending_ = true;
  return direction_ > 0 ? Motion::StepPositive : Motion::StepNegative;
}

inline void Controller::reportStepWritten() {
  if (!stepPending_) return;
  stepPending_ = false;
  lastStepUs_ = lastTickUs_;
  if (direction_ > 0 && saved_.positionSteps < INT32_MAX) ++saved_.positionSteps;
  else if (direction_ < 0 && saved_.positionSteps > INT32_MIN) --saved_.positionSteps;
  ++legSteps_;
  if (run_ == Run::SeekHall) ++seekSteps_;
  refreshStatus(status_.rawHallActive, status_.stableHallActive);
}

inline void Controller::reportStepWriteFailure() { if (stepPending_) enterFault(); }

inline bool Controller::updateButton(bool down, bool stableReleased, uint32_t nowMs) {
  (void)nowMs;
  if (!down && stableReleased) buttonReleasedSeen_ = true;
  if (down && buttonReleasedSeen_) {
    buttonReleasedSeen_ = false;
    freshPressPending_ = true;
    return true;
  }
  return false;
}

inline void Controller::enterFault() {
  stepPending_ = false; direction_ = 0; run_ = Run::Fault;
  rewardSeekActive_ = false; pickupAfterHallCheck_ = false;
  legPositionWasKnown_ = false; pickupMoveActive_ = false;
  Snapshot next = saved_;
  next.mode = Mode::Recovery; next.positionValid = false;
  if (!persist(next)) {
    saved_.mode = Mode::Recovery; storageGood_ = false;
  }
  refreshStatus(status_.rawHallActive, status_.stableHallActive);
}

}  // namespace rr334

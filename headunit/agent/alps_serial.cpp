// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Bluetooth serial on EntryEvo, over the Alps stack: see alps_serial.hpp. QNX 6.5 / GCC 4.4: no lambdas, nullptr
// or NSDMI.

#include "alps_serial.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif

#include "fsutil.hpp"
#include "protocol_constants.hpp"

namespace nbt {

namespace alps {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string uuidPair(const std::string& uuid) {
    if (uuid.size() != 36) return std::string();
    std::string bytes;
    for (size_t i = 0; i < uuid.size();) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') return std::string();
            ++i;
            continue;
        }
        int high = hexDigit(uuid[i]), low = i + 1 < uuid.size() ? hexDigit(uuid[i + 1]) : -1;
        if (high < 0 || low < 0) return std::string();
        bytes += (char)(high * 16 + low);
        i += 2;
    }
    if (bytes.size() != 16) return std::string();
    return std::string(1, '\x03') + bytes + std::string(1, '\x03') + bytes;
}

bool validPair(const std::string& payload) {
    static const size_t sizes[] = {0, 2, 4, 16};
    size_t at = 0;
    for (int i = 0; i < 2; ++i) {
        if (at >= payload.size()) return false;
        unsigned char type = (unsigned char)payload[at];
        if (type < 1 || type > 3) return false;
        at += 1 + sizes[type];
    }
    return at == payload.size();
}

bool knownBaseline(const std::string& payload) {
    const std::string hex = hexOf(payload);
    // As EntryAad's platform/spp_baseline.hpp has them: the generic serial port (0x1101) twice, and the pair seen on
    // the car, the UUID links are opened from and the record's.
    return hex == "011101011101" ||
           hex == "0300000000decafadedecadeafdecacaff0300000000decafadedecadeafdecacafe";
}

std::string hexOf(const std::string& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        unsigned char b = (unsigned char)bytes[i];
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

bool bytesOf(const std::string& hex, std::string* out) {
    if (hex.size() % 2 != 0) return false;
    std::string bytes;
    for (size_t i = 0; i < hex.size(); i += 2) {
        int high = hexDigit(hex[i]), low = hexDigit(hex[i + 1]);
        if (high < 0 || low < 0) return false;
        bytes += (char)(high * 16 + low);
    }
    *out = bytes;
    return true;
}

std::string colonAddress(const std::string& six) {
    if (six.size() != 6) return std::string();
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (size_t i = 0; i < 6; ++i) {
        unsigned char b = (unsigned char)six[i];
        if (i != 0) out += ':';
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

bool flockHeld(const std::string& text, unsigned major, unsigned minor, unsigned long inode) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        // "1: FLOCK  ADVISORY  WRITE 1234 00:0e:4711 0 EOF". A line with "->" is one waiting for the lock.
        if (line.find(" FLOCK ") == std::string::npos || line.find("->") != std::string::npos) continue;
        size_t from = 0;
        while (from < line.size()) {
            size_t space = line.find(' ', from);
            if (space == std::string::npos) space = line.size();
            std::string word = line.substr(from, space - from);
            from = space + 1;
            unsigned a = 0, b = 0;
            unsigned long c = 0;
            char tail = 0;
            if (!word.empty() && sscanf(word.c_str(), "%x:%x:%lu%c", &a, &b, &c, &tail) == 3 && a == major && b == minor &&
                c == inode) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace alps

namespace {

int64_t monoMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

std::string number(long long n) {
    char text[32];
    snprintf(text, sizeof text, "%lld", n);
    return text;
}

const char kJournalMagic[] = "NBTAPP_ALPS_V1";

class Lock {
public:
    explicit Lock(pthread_mutex_t* m) : m_(m) { pthread_mutex_lock(m_); }
    ~Lock() { pthread_mutex_unlock(m_); }

private:
    pthread_mutex_t* m_;
};

// The agent's end of a link: a socket like any other, with the phone's address.
class AlpsLink : public SocketLink {
public:
    AlpsLink(int fd, const std::string& address) : SocketLink(fd, "serial"), address_(address) {}
    virtual std::string phoneAddress() const { return address_; }

private:
    std::string address_;
};

bool setFlags(int fd, bool nonBlocking) {
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) return false;
    if (!nonBlocking) return true;
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

// Our end of the pair is non-blocking whatever the flags say; MSG_DONTWAIT and MSG_NOSIGNAL where the system has them.
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifdef MSG_NOSIGNAL
const int kSendFlags = MSG_DONTWAIT | MSG_NOSIGNAL;
#else
const int kSendFlags = MSG_DONTWAIT;
#endif

}  // namespace

// ---- EntryAad ------------------------------------------------------------------------------------------------------

bool EntryAadWatch::holdsInstance() {
    struct stat st;
    return stat(journal_.c_str(), &st) == 0;
}

bool EntryAadWatch::active(std::string* who) {
    struct stat st;
    if (stat(journal_.c_str(), &st) == 0) {
        if (who != 0) *who = "EntryAad's journal of the instance it borrowed is there (" + journal_ + ")";
        return true;
    }
#ifdef __linux__
    if (stat(lock_.c_str(), &st) != 0) return false;
    std::string locks;
    if (!fs::readFile(locks_, 1u << 20, &locks, 0)) return false;
    if (alps::flockHeld(locks, major(st.st_dev), minor(st.st_dev), (unsigned long)st.st_ino)) {
        if (who != 0) *who = "EntryAad's supervisor holds its lock (" + lock_ + ")";
        return true;
    }
#endif
    return false;
}

// ---- the configuration ---------------------------------------------------------------------------------------------

AlpsConfig::AlpsConfig()
    : instance(0), uuid(kSerialServiceUuid), confirmMs(3000), sendMs(5000), lingerMs((unsigned)kSerialLingerMs),
      retryMs(30000), yieldQuietMs(10000), foreignBackoffMs(60000), resetArmedMs(60000), othersEveryMs(1000) {
    refreshMs[0] = 15000;
    refreshMs[1] = 45000;
    refreshMs[2] = 90000;
}

// ---- the listener --------------------------------------------------------------------------------------------------

AlpsSerial::AlpsSerial(AlpsBus* bus, AlpsOthers* others, SerialLog* log, const AlpsConfig& config)
    : bus_(bus), others_(others), log_(log), config_(config), state_(kOff), deadline_(0), notBefore_(0),
      lastOthers_(-1), othersSeen_(0), advertisedAt_(0), refreshed_(0), stopping_(false), canBorrow_(false),
      journaled_(false),
      savedInstance_(0), savedInterface_(0), linkReady_(false), phoneEnded_(false), agentEnded_(false),
      disconnectAsked_(false), agentEndedAt_(0), txPending_(false), txAllowed_(true), txSince_(0),
      resetArmedUntil_(0), lastNow_(0), wakeRead_(-1), wakeWrite_(-1), threadStarted_(false), threadStop_(false),
      registeredAt_(-1), links_(0), yields_(0), foreign_(0), failures_(0) {
    pair_[0] = pair_[1] = -1;
    pthread_mutex_init(&mu_, 0);
    pthread_mutex_init(&queueMu_, 0);
}

AlpsSerial::~AlpsSerial() {
    if (threadStarted_) stop(0);
    {
        Lock lock(&mu_);
        endLink(0);
    }
    if (wakeRead_ >= 0) close(wakeRead_);
    if (wakeWrite_ >= 0) close(wakeWrite_);
    pthread_mutex_destroy(&mu_);
    pthread_mutex_destroy(&queueMu_);
}

const char* AlpsSerial::stateName(State state) {
    switch (state) {
        case kOff: return "off";
        case kIdle: return "idle";
        case kReadInterface: return "reading 0x6203";
        case kWriteInterface: return "setting 0x6203";
        case kCheckInterface: return "checking 0x6203";
        case kSetUuid: return "setting the UUID";
        case kAdvertised: return "advertised";
        case kLinkUp: return "link up";
        case kEnding: return "ending the link";
        case kRestoreUuid: return "putting the UUIDs back";
        case kRestoreInterface: return "putting 0x6203 back";
        case kCheckRestore: return "checking 0x6203 is back";
    }
    return "?";
}

bool AlpsSerial::open(std::string* why) {
    Lock lock(&mu_);
    if (state_ != kOff || wakeRead_ >= 0) {
        if (why != 0) *why = "already open";
        return false;
    }
    if (alps::uuidPair(config_.uuid).empty()) {
        if (why != 0) *why = "the service UUID " + config_.uuid + " is not a UUID of 128 bits";
        return false;
    }
    canBorrow_ = alps::validPair(config_.baseline) && alps::knownBaseline(config_.baseline);
    // A journal is what an agent changed and did not put back: it is put back first, whatever else is set.
    std::string text, error;
    if (fs::kindOf(config_.journalPath) != fs::kNone) {
        if (!fs::readFile(config_.journalPath, 4096, &text, &error)) {
            if (why != 0) *why = "the journal " + config_.journalPath + " cannot be read: " + error;
            return false;
        }
        char magic[32] = {0}, pair[160] = {0};
        unsigned instance = 256, interface = 256;
        std::string pairBytes;
        if (sscanf(text.c_str(), "%31s %u %u %159s", magic, &instance, &interface, pair) != 4 ||
            strcmp(magic, kJournalMagic) != 0 || instance > 255 || interface > 1 || !alps::bytesOf(pair, &pairBytes) ||
            !alps::validPair(pairBytes) || !alps::knownBaseline(pairBytes)) {
            if (why != 0) {
                *why = "the journal " + config_.journalPath + " is not one this agent wrote, so nothing is put back from it: "
                       "look at it, put instance 0 back by hand, and remove it";
            }
            return false;
        }
        journaled_ = true;
        savedInstance_ = (uint8_t)instance;
        savedInterface_ = (uint8_t)interface;
        savedPair_ = pairBytes;
    } else if (!canBorrow_) {
        if (why != 0) {
            *why = config_.baseline.empty()
                       ? std::string("the UUIDs that are on the instance, to be put back, are not known")
                       : "the UUIDs given to be put back (" + alps::hexOf(config_.baseline) +
                             ") are not a pair this agent knows to put back";
        }
        return false;
    }
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        if (why != 0) *why = std::string("no pipe: ") + strerror(errno);
        return false;
    }
    wakeRead_ = pipeFds[0];
    wakeWrite_ = pipeFds[1];
    setFlags(wakeRead_, true);
    setFlags(wakeWrite_, true);
    state_ = kIdle;
    if (journaled_) {
        say("a journal of an agent that ended early is there: instance " + number(savedInstance_) +
            " gets its UUIDs (" + alps::hexOf(savedPair_) + ") and 0x6203 = " + number(savedInterface_) + " back first");
        if (!canBorrow_) say("no UUIDs are given to put back after that, so the instance is put back and not taken again");
    }
    return true;
}

void* AlpsSerial::run(void* self) {
    AlpsSerial* s = static_cast<AlpsSerial*>(self);
    for (;;) {
        {
            Lock lock(&s->mu_);
            if (s->threadStop_) break;
        }
        s->step(monoMs());
        usleep(20000);
    }
    return 0;
}

bool AlpsSerial::start(std::string* why) {
    {
        Lock lock(&mu_);
        if (state_ == kOff || threadStarted_) {
            if (why != 0) *why = state_ == kOff ? "not open" : "already started";
            return false;
        }
        threadStop_ = false;
    }
    if (pthread_create(&thread_, 0, &AlpsSerial::run, this) != 0) {
        if (why != 0) *why = std::string("no thread: ") + strerror(errno);
        return false;
    }
    threadStarted_ = true;
    return true;
}

bool AlpsSerial::stop(int waitMs) {
    {
        Lock lock(&mu_);
        stopping_ = true;
    }
    if (threadStarted_) {
        const int64_t until = monoMs() + (waitMs > 0 ? waitMs : 0);
        for (;;) {
            {
                Lock lock(&mu_);
                if (state_ == kOff || monoMs() >= until) {
                    threadStop_ = true;
                    break;
                }
            }
            usleep(20000);
        }
        pthread_join(thread_, 0);
        threadStarted_ = false;
    }
    Lock lock(&mu_);
    if (journaled_) say("stopped with instance " + number(savedInstance_) + " not put back: the journal stays, and the next start puts it back");
    return !journaled_;
}

void AlpsSerial::post(const AlpsSignal& signal) {
    Lock lock(&queueMu_);
    queue_.push_back(signal);
}

AlpsSerial::State AlpsSerial::state() {
    Lock lock(&mu_);
    return state_;
}

bool AlpsSerial::borrowed() {
    Lock lock(&mu_);
    return journaled_;
}

bool AlpsSerial::linkUp() {
    Lock lock(&mu_);
    return pair_[0] >= 0;
}

void AlpsSerial::say(const std::string& line) {
    if (log_ != 0) log_->line("alps: " + line);
}

void AlpsSerial::enter(State next, int64_t now, unsigned timeoutMs) {
    state_ = next;
    deadline_ = timeoutMs != 0 ? now + timeoutMs : 0;
}

void AlpsSerial::fail(const std::string& why, int64_t now, unsigned backoffMs) {
    say(why + "; next try in " + number(backoffMs / 1000) + " s");
    ++failures_;
    enter(kIdle, now);
    notBefore_ = now + backoffMs;
}

bool AlpsSerial::writeJournal(std::string* why) {
    char text[256];
    snprintf(text, sizeof text, "%s %u %u %s\n", kJournalMagic, (unsigned)savedInstance_, (unsigned)savedInterface_,
             alps::hexOf(savedPair_).c_str());
    return fs::writeFile(config_.journalPath, text, 0600, why);
}

// ---- borrowing and giving back -------------------------------------------------------------------------------------

void AlpsSerial::borrow(int64_t now) {
    refreshed_ = 0;
    if (!bus_->readConfig(config_.instance, alps::kDataInterfaceId)) {
        fail("0x6203 of instance " + number(config_.instance) + " cannot be asked for: the bus took no request", now,
             config_.retryMs);
        return;
    }
    enter(kReadInterface, now, config_.confirmMs);
}

void AlpsSerial::giveBack(const std::string& why, int64_t now, unsigned backoffMs) {
    say(why + ": instance " + number(journaled_ ? savedInstance_ : config_.instance) + " goes back");
    // After a failure the next borrow waits; after Android Auto, EntryAad's quiet time does the same.
    if (backoffMs != 0) {
        ++failures_;
        notBefore_ = now + backoffMs;
    }
    if (pair_[0] >= 0 && !phoneEnded_) {
        if (!disconnectAsked_) {
            bus_->disconnect(config_.instance);
            disconnectAsked_ = true;
        }
        enter(kEnding, now, 5000);
        return;
    }
    endLink(0);
    restoreNext(now);
}

void AlpsSerial::restoreNext(int64_t now) {
    if (!journaled_) {
        enter(stopping_ ? kOff : kIdle, now);
        return;
    }
    if (!bus_->setUuid(savedInstance_, savedPair_)) {
        fail("the UUIDs cannot be put back: the bus took no request; the journal stays", now, config_.retryMs);
        return;
    }
    enter(kRestoreUuid, now, config_.confirmMs);
}

// ---- the link ------------------------------------------------------------------------------------------------------

void AlpsSerial::openLink(const std::string& address, int64_t now) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        say(std::string("a phone opened a link, and there is no socket pair for it: ") + strerror(errno));
        bus_->disconnect(config_.instance);
        return;
    }
    setFlags(fds[0], true);
    setFlags(fds[1], false);
    pair_[0] = fds[0];
    pair_[1] = fds[1];
    linkReady_ = true;
    phoneEnded_ = agentEnded_ = disconnectAsked_ = false;
    txPending_ = false;
    txAllowed_ = true;
    rx_.clear();
    address_ = alps::colonAddress(address);
    ++links_;
    enter(kLinkUp, now);
    say("a phone opened a link on instance " + number(config_.instance));
    char wake = 1;
    ssize_t n = write(wakeWrite_, &wake, 1);
    (void)n;
}

void AlpsSerial::endLink(const char* why) {
    if (pair_[0] < 0 && pair_[1] < 0) return;
    if (pair_[1] >= 0) close(pair_[1]);   // not handed over yet
    if (pair_[0] >= 0) close(pair_[0]);
    pair_[0] = pair_[1] = -1;
    linkReady_ = false;
    rx_.clear();
    txPending_ = false;
    if (why != 0) say(std::string("the link ended: ") + why);
}

Link* AlpsSerial::accept() {
    Lock lock(&mu_);
    char drain[16];
    while (wakeRead_ >= 0 && read(wakeRead_, drain, sizeof drain) > 0) {
    }
    if (!linkReady_ || pair_[1] < 0) return 0;
    Link* link = new AlpsLink(pair_[1], address_);
    pair_[1] = -1;
    linkReady_ = false;
    return link;
}

void AlpsSerial::pump(int64_t now) {
    if (pair_[0] < 0) return;
    // What the phone sent, to the agent.
    while (!rx_.empty()) {
        ssize_t n = send(pair_[0], rx_.data(), rx_.size(), kSendFlags);
        if (n > 0) {
            rx_.erase(0, (size_t)n);
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;
        } else {
            // The agent's end is gone: what was coming is dropped.
            rx_.clear();
            if (!agentEnded_) {
                agentEnded_ = true;
                agentEndedAt_ = now;
            }
            break;
        }
    }
    // What the agent wrote, to the phone: one chunk at a time, the next once the stack says the last one is sent.
    if (!agentEnded_ && !phoneEnded_ && !txPending_ && txAllowed_) {
        char buffer[alps::kChunk];
        ssize_t n = recv(pair_[0], buffer, sizeof buffer, MSG_DONTWAIT);
        if (n > 0) {
            if (bus_->sendData(config_.instance, std::string(buffer, (size_t)n))) {
                txPending_ = true;
                txSince_ = now;
            } else {
                say("bytes for the phone cannot be sent: the bus took no request; the link is ended");
                agentEnded_ = true;
                agentEndedAt_ = now - config_.lingerMs;
            }
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            agentEnded_ = true;
            agentEndedAt_ = now;
        }
    }
    if (txPending_ && now - txSince_ >= (int64_t)config_.sendMs) {
        say("bytes for the phone were not sent within " + number(config_.sendMs) + " ms: the link is ended");
        txPending_ = false;
        agentEnded_ = true;
        agentEndedAt_ = now - config_.lingerMs;
    }
    // The agent's end closed and the phone's is still up: the phone ends it, or the stack does once lingerMs are gone.
    if (agentEnded_ && !phoneEnded_ && !disconnectAsked_ && now - agentEndedAt_ >= (int64_t)config_.lingerMs) {
        bus_->disconnect(config_.instance);
        disconnectAsked_ = true;
        say("the agent's end of the link closed and the phone's is still up: the stack is asked to end it");
    }
    if (phoneEnded_ && (rx_.empty() || agentEnded_)) {
        endLink("the phone's end is down");
        if (state_ == kLinkUp) {
            enter(kAdvertised, now);
        } else if (state_ == kEnding) {
            restoreNext(now);
        }
    }
}

// ---- the stack's signals -------------------------------------------------------------------------------------------

void AlpsSerial::handle(const AlpsSignal& s, int64_t now) {
    if (state_ == kOff) return;
    if (s.kind == AlpsSignal::kBusLost) {
        say("the stack or the bus went away");
        endLink(0);
        if (state_ == kLinkUp || state_ == kEnding) say("the link ended with it");
        // Whatever was out is lost. With a journal, the instance is put back once the stack is there again.
        fail("lost the stack", now, config_.retryMs);
        return;
    }
    const uint8_t ours = journaled_ ? savedInstance_ : config_.instance;
    if (s.instance != ours) return;

    switch (s.kind) {
        case AlpsSignal::kReadConfigCfm: {
            if (s.id != alps::kDataInterfaceId) return;
            const bool readable = s.status == 0 && s.bytes.size() == 1 && (unsigned char)s.bytes[0] <= 1;
            const unsigned value = readable ? (unsigned char)s.bytes[0] : 256;
            if (state_ == kReadInterface) {
                if (!readable) return fail("0x6203 of instance " + number(ours) + " is not readable, or not 0 or 1", now, config_.retryMs);
                savedInstance_ = config_.instance;
                savedInterface_ = (uint8_t)value;
                savedPair_ = config_.baseline;
                std::string why;
                if (!writeJournal(&why)) return fail("the journal cannot be written, so nothing is changed: " + why, now, config_.retryMs);
                journaled_ = true;
                if (value == 1) {
                    if (!bus_->setUuid(ours, alps::uuidPair(config_.uuid))) return giveBack("the UUID cannot be set", now, config_.retryMs);
                    enter(kSetUuid, now, config_.confirmMs);
                } else {
                    if (!bus_->writeConfig(ours, alps::kDataInterfaceId, std::string(1, '\x01'))) return giveBack("0x6203 cannot be set", now, config_.retryMs);
                    enter(kWriteInterface, now, config_.confirmMs);
                }
            } else if (state_ == kCheckInterface) {
                if (value != 1) return giveBack("0x6203 did not become 1", now, config_.retryMs);
                if (!bus_->setUuid(ours, alps::uuidPair(config_.uuid))) return giveBack("the UUID cannot be set", now, config_.retryMs);
                enter(kSetUuid, now, config_.confirmMs);
            } else if (state_ == kCheckRestore) {
                if (value != savedInterface_) {
                    return fail("0x6203 did not come back to " + number(savedInterface_) + "; the journal stays", now, config_.retryMs);
                }
                std::string why;
                if (!fs::removeFile(config_.journalPath, &why)) say("the journal cannot be removed: " + why);
                journaled_ = false;
                registeredAt_ = -1;
                say("instance " + number(ours) + " is back as it was: its UUIDs and 0x6203 = " + number(savedInterface_));
                enter(stopping_ ? kOff : kIdle, now);
            }
            return;
        }
        case AlpsSignal::kSetConfigCfm: {
            if (s.id != alps::kDataInterfaceId) return;
            if (state_ == kWriteInterface) {
                if (s.status != 0) return giveBack("0x6203 was not set (status " + number(s.status) + ")", now, config_.retryMs);
                if (!bus_->readConfig(ours, alps::kDataInterfaceId)) return giveBack("0x6203 cannot be asked for", now, config_.retryMs);
                enter(kCheckInterface, now, config_.confirmMs);
            } else if (state_ == kRestoreInterface) {
                if (s.status != 0) return fail("0x6203 was not put back (status " + number(s.status) + "); the journal stays", now, config_.retryMs);
                if (!bus_->readConfig(ours, alps::kDataInterfaceId)) return fail("0x6203 cannot be asked for; the journal stays", now, config_.retryMs);
                enter(kCheckRestore, now, config_.confirmMs);
            }
            return;
        }
        case AlpsSignal::kSetUuidCfm: {
            if (state_ == kSetUuid) {
                if (s.status != 0) return giveBack("the stack did not take the agent's UUID (status " + number(s.status) + ")", now, config_.retryMs);
                if (refreshed_ == 0) {
                    advertisedAt_ = now;
                    registeredAt_ = now;
                    say("instance " + number(ours) + " is the agent's: service " + config_.uuid +
                        ". While it is, the car maker's phone app on it does not work over Bluetooth");
                }
                enter(kAdvertised, now);
                if (stopping_) giveBack("the agent stops", now);
            } else if (state_ == kRestoreUuid) {
                if (s.status != 0) return fail("the UUIDs were not put back (status " + number(s.status) + "); the journal stays", now, config_.retryMs);
                if (savedInterface_ != 1) {
                    if (!bus_->writeConfig(ours, alps::kDataInterfaceId, std::string(1, (char)savedInterface_))) {
                        return fail("0x6203 cannot be put back; the journal stays", now, config_.retryMs);
                    }
                    enter(kRestoreInterface, now, config_.confirmMs);
                } else {
                    if (!bus_->readConfig(ours, alps::kDataInterfaceId)) return fail("0x6203 cannot be asked for; the journal stays", now, config_.retryMs);
                    enter(kCheckRestore, now, config_.confirmMs);
                }
            }
            return;
        }
        case AlpsSignal::kConnectInd:
        case AlpsSignal::kDisconnectInd: {
            if (s.function != alps::kFunctionSpp) return;
            const bool up = s.kind == AlpsSignal::kConnectInd;
            if (up) {
                if ((s.status & 0x7f) != 0) return;
                const bool advertised = state_ == kAdvertised || (state_ == kSetUuid && refreshed_ > 0);
                if (!advertised || pair_[0] >= 0) {
                    say(std::string("a link opened on instance ") + number(ours) + " while it is " + stateName(state_) +
                        ": not the agent's, and left alone");
                    return;
                }
                if (resetArmedUntil_ != 0 && now < resetArmedUntil_) {
                    resetArmedUntil_ = 0;
                    bus_->disconnect(ours);
                    say("a link opened within the time serial.reset gave: the stack is asked to end it at once");
                    return;
                }
                openLink(s.address, now);
            } else if (pair_[0] >= 0) {
                phoneEnded_ = true;
                say("the phone's end of the link went down (reason " + number(s.reason) + ")");
                pump(now);
            }
            return;
        }
        case AlpsSignal::kDataInd: {
            if (s.bytes.empty()) return;
            if ((state_ == kLinkUp || state_ == kEnding) && pair_[0] >= 0) {
                if (phoneEnded_ || agentEnded_) return;   // the link is going: nobody reads what comes
                if (rx_.size() + s.bytes.size() > alps::kMaxQueued) {
                    say("the agent has not read " + number((long long)rx_.size()) + " bytes from the phone: the link is ended");
                    agentEnded_ = true;
                    agentEndedAt_ = now - config_.lingerMs;
                    rx_.clear();
                    return;
                }
                rx_ += s.bytes;
                return;
            }
            if (journaled_ && state_ != kRestoreUuid && state_ != kRestoreInterface && state_ != kCheckRestore) {
                ++foreign_;
                giveBack("bytes came on the instance with no link of the agent's: a phone's link to the car maker's "
                         "service on it, which the agent hands to nobody",
                         now);
                notBefore_ = now + config_.foreignBackoffMs;
            }
            return;
        }
        case AlpsSignal::kDataCfm:
            if (pair_[0] >= 0 && s.status != 0) {
                say("the stack could not send bytes for the phone (status " + number(s.status) + "): the link is ended");
                txPending_ = false;
                agentEnded_ = true;
                agentEndedAt_ = now - config_.lingerMs;
            }
            return;
        case AlpsSignal::kSendInd:
            txPending_ = false;
            return;
        case AlpsSignal::kFlowControl:
            txAllowed_ = s.status == 0;
            return;
        case AlpsSignal::kBusLost:
            return;
    }
}

void AlpsSerial::step(int64_t now) {
    std::deque<AlpsSignal> signals;
    {
        Lock lock(&queueMu_);
        signals.swap(queue_);
    }
    Lock lock(&mu_);
    lastNow_ = now;
    for (size_t i = 0; i < signals.size(); ++i) handle(signals[i], now);
    if (state_ == kOff) return;

    // Android Auto comes first.
    if (others_ != 0 && (lastOthers_ < 0 || now - lastOthers_ >= (int64_t)config_.othersEveryMs)) {
        lastOthers_ = now;
        std::string who;
        if (others_->active(&who)) {
            if (othersSeen_ == 0 || now - othersSeen_ > (int64_t)config_.yieldQuietMs) say("another program is at work: " + who);
            othersSeen_ = now;
            if (state_ == kReadInterface && !journaled_) {
                // Nothing is changed yet: the question is dropped, and an answer that comes late finds another state.
                enter(kIdle, now);
            } else if (state_ != kIdle && state_ != kEnding && state_ != kRestoreUuid && state_ != kRestoreInterface &&
                       state_ != kCheckRestore && journaled_) {
                ++yields_;
                deadline_ = 0;
                giveBack("Android Auto comes first", now);
            }
        }
    }

    if (deadline_ != 0 && now >= deadline_) {
        deadline_ = 0;
        switch (state_) {
            case kReadInterface:
                if (!journaled_) fail("no answer to the question for 0x6203", now, config_.retryMs);
                else giveBack("no answer to the question for 0x6203", now, config_.retryMs);
                break;
            case kWriteInterface:
            case kCheckInterface:
            case kSetUuid:
                giveBack(std::string("no confirmation while ") + stateName(state_), now, config_.retryMs);
                break;
            case kEnding:
                endLink("the phone did not end the link within 5 s");
                restoreNext(now);
                break;
            case kRestoreUuid:
            case kRestoreInterface:
            case kCheckRestore:
                fail(std::string("no confirmation while ") + stateName(state_) + "; the journal stays", now, config_.retryMs);
                break;
            default:
                break;
        }
    }

    if (state_ == kIdle && now >= notBefore_) {
        if (journaled_) {
            // What an agent changed goes back first, also while EntryAad is at work, whose start waits for it: unless
            // EntryAad has a journal of its own for the instance, which is then its to put back.
            if (others_ == 0 || !others_->holdsInstance()) {
                restoreNext(now);
            } else {
                notBefore_ = now + config_.retryMs;
            }
        } else if (stopping_) {
            enter(kOff, now);
            return;
        } else if (canBorrow_ && (othersSeen_ == 0 || now - othersSeen_ >= (int64_t)config_.yieldQuietMs)) {
            borrow(now);
        }
    }

    if (state_ == kAdvertised && pair_[0] < 0) {
        if (stopping_) {
            giveBack("the agent stops", now);
        } else if (refreshed_ < 3 && now - advertisedAt_ >= (int64_t)config_.refreshMs[refreshed_]) {
            // As EntryAad does: the UUID set again a few times while no link is up.
            ++refreshed_;
            if (bus_->setUuid(config_.instance, alps::uuidPair(config_.uuid))) enter(kSetUuid, now, config_.confirmMs);
        }
    }
    if (state_ == kLinkUp && stopping_ && !phoneEnded_) giveBack("the agent stops", now);
    if (state_ == kLinkUp || state_ == kEnding) pump(now);
}

// ---- what the agent asks -------------------------------------------------------------------------------------------

SerialReset::Outcome AlpsSerial::resetSerialLink(const std::string&, const std::string&, const std::string&,
                                                 int64_t* forMs, std::string* why) {
    Lock lock(&mu_);
    if (state_ == kLinkUp && pair_[0] >= 0 && !phoneEnded_) {
        bus_->disconnect(config_.instance);
        disconnectAsked_ = true;
        say("serial.reset: the stack is asked to end the link");
        return kResetSent;
    }
    if (state_ == kAdvertised || state_ == kSetUuid) {
        resetArmedUntil_ = lastNow_ + config_.resetArmedMs;
        if (forMs != 0) *forMs = config_.resetArmedMs;
        say("serial.reset: the next link within " + number(config_.resetArmedMs / 1000) + " s is ended at once");
        return kResetArmed;
    }
    if (why != 0) *why = std::string("the Bluetooth instance is not the agent's now (") + stateName(state_) + ")";
    return kResetFailed;
}

unsigned AlpsSerial::dialNow(std::string* why, const char** code) {
    if (why != 0) *why = "the car does not dial phones on EntryEvo";
    if (code != 0) *code = "unsupported";
    return 0;
}

SerialStatus AlpsSerial::serialStatus() {
    Lock lock(&mu_);
    SerialStatus st;
    st.registered = state_ == kAdvertised || state_ == kLinkUp || (state_ == kSetUuid && refreshed_ > 0);
    st.checkedMsAgo = registeredAt_ < 0 ? -1 : lastNow_ - registeredAt_;
    st.stack.push_back(std::string("Alps Evolution over D-Bus, instance ") + number(journaled_ ? savedInstance_ : config_.instance));
    st.dialOn = false;
    st.link = pair_[0] >= 0 ? "phone-opened" : "none";
    st.checks = yields_;
    st.refusals = foreign_;
    st.problemsCounted = failures_;
    return st;
}

}  // namespace nbt

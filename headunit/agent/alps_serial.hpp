// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_ALPS_SERIAL_HPP
#define NBT_AGENT_ALPS_SERIAL_HPP

// Bluetooth serial on EntryEvo (EntryNav2): the agent's links from phone apps over the car's Alps "Evolution"
// Bluetooth stack, which a program asks through D-Bus. NBT Evo's listener (serial_link.hpp) is built on that car's
// own Bluetooth service and QNX's path space, and none of it is there.
//
// What the stack offers, as the EntryEvo Android Auto project (EntryAad) drives it (its platform/alps_spp), on the
// system bus, service org.alps.evolution, object /org/alps/evolution/hli. Every request is a method call with no
// reply; what came of it is a signal, which carries no serial, so one request of each kind is out at a time:
//   org.alps.evo.SPP  BtApplSppSetUuidReq(y instance, q length, ay data)   -> BtApplSppSetUuidCfm(y instance, y status)
//                     BtApplSppDataReq(y instance, q length, ay data)      -> BtApplSppSendInd(y instance): sent;
//                                                                             BtApplSppDataCfm(y instance, y status):
//                                                                             a status other than 0 is a failure
//                     BtApplSppDataSendFcInd(y instance, y status)            flow control: 0 go on, other pause
//                     BtApplSppDataInd(y instance, q length, ay data)         bytes from the phone
//   org.alps.evo.DM   BtApplDeviceConnectInd(y status, ay address, t function, y instance)
//                     BtApplDeviceDisconnectInd(y status, ay address, y reason, t function, y instance)
//                                                                             function 1 is SPP; status & 0x7f 0 is OK
//                     BtApplReadConfigurationReq(y instance, q id)         -> BtApplReadConfigurationCfm(y status,
//                                                                             y instance, q id, q length, ay data)
//                     BtApplSetConfigurationReq(y instance, q id, q length, ay data)
//                                                                          -> BtApplSetConfigurationCfm(y status,
//                                                                             y instance, q id)
//                     BtApplDeviceDisconnectReq(t function, y instance)       ends the SPP link of that instance
//   SetUuid's data: [type][UUID the link is opened from][type][UUID of the SDP record], type 1, 2 or 3 for a UUID of
//   16, 32 or 128 bits. Configuration 0x6203 of an instance is where its data goes: 1, through D-Bus as above.
//
// The car has no SPP instance to spare. Its instances are the car maker's (BMW's phone apps), and EntryAad borrows
// instance 0: it saves the UUIDs on it and the value of 0x6203, puts its own UUID there, and puts both back when it
// is done, from a journal if it ended early. The agent does the same with the instance it is given:
//
//   1. It reads 0x6203, and writes the journal (AlpsConfig::journalPath) before it changes anything: the instance,
//      that value, and the UUIDs to put back.
//   2. It sets 0x6203 to 1 if it is not, reads it back, and sets the agent's UUID (kSerialServiceUuid) as both
//      UUIDs. The instance is then the agent's: its SDP record is the agent's service, which a phone app connects to.
//   3. A link is a BtApplDeviceConnectInd for SPP on the instance. It becomes a Link of kind "serial" (link.hpp),
//      one end of a socket pair whose other end this class serves: what BtApplSppDataInd brings goes in, and what the
//      agent writes goes out in BtApplSppDataReq of kChunk bytes at most, one at a time, each sent once SendInd says
//      so, and none while flow control pauses. One that is not sent within sendMs ends the link.
//   4. To give the instance back it ends a link that is up (BtApplDeviceDisconnectReq), sets the saved UUIDs and the
//      saved 0x6203, reads 0x6203 back, and only then removes the journal.
//   5. A journal it finds when it starts is from an agent that ended early: step 4 comes first.
//
// The UUIDs that are on the instance cannot be read over D-Bus (EntryAad reads them out of the stack's memory). So
// they are given (AlpsConfig::baseline: the file alps-baseline in the agent's state folder, one line of hex, as
// EntryAad's supervisor logs it: "preflight: SPP 0: ... UUID=<hex>"), and only the two pairs EntryAad knows are taken
// (knownBaseline). Without one, Bluetooth serial stays off and says why.
//
// Alongside EntryAad. Android Auto comes first: while EntryAad's supervisor runs (its lock, /tmp/entry-aad-wireless
// .lock, is held, which /proc/locks tells without taking it) or its journal of instance 0 is there
// (/tmp/entry-aa-service/bluetooth-retour.txt), the agent does not borrow, and gives back what it has, ending its
// link: the supervisor takes its lock before it looks at instance 0, and looks again every 5 seconds for two
// minutes. The agent borrows again once EntryAad has been gone for yieldQuietMs.
//
// A link that is not the agent's. A phone that was connected to the car maker's service on the instance before the
// agent took it has a link that the stack goes on serving, now through D-Bus. Bytes that come with no
// BtApplDeviceConnectInd of the agent's time are that link's: the agent hands them to nobody, gives the instance
// back, and tries again foreignBackoffMs later. What to do about such a link before it sends anything is a question
// for a car (README, "Bluetooth serial on EntryEvo").
//
// Threads. The bus's own thread hands signals over with post(), which only queues them. Everything else happens in
// step(), on this class's thread (start()) or a test's: the requests, the journal, the pump. The agent's thread
// calls accept(), resetSerialLink() and serialStatus(). All of it under one lock, with no call into the bus's
// thread. Never a phone's address in the log.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI. Built everywhere, so that the bench runs its tests; only the
// EntryEvo build has a bus for it (alps_dbus.hpp).

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <string>
#include <vector>

#include "link.hpp"
#include "serial_dial.hpp"
#include "serial_log.hpp"
#include "serial_reset.hpp"

namespace nbt {
namespace alps {

const char kService[] = "org.alps.evolution";
const char kPath[] = "/org/alps/evolution/hli";
const char kSppInterface[] = "org.alps.evo.SPP";
const char kDmInterface[] = "org.alps.evo.DM";
const uint64_t kFunctionSpp = 1;
const uint16_t kDataInterfaceId = 0x6203;   // where an instance's data goes: 1 through D-Bus
const size_t kChunk = 1024;                  // bytes in one BtApplSppDataReq, as EntryAad sends them
const size_t kMaxQueued = 256 * 1024;        // bytes from the phone the agent has not read: past this, the link ends

// "58876F35-7F3F-..." as SetUuid's data for one 128-bit UUID as both: [3][16 bytes][3][16 bytes]. Empty if `uuid`
// is not a UUID of 128 bits.
std::string uuidPair(const std::string& uuid);
// Whether SetUuid's data is well formed: two UUIDs of 16, 32 or 128 bits, each after its type, and nothing more.
bool validPair(const std::string& payload);
// The two pairs of UUIDs EntryAad finds on instance 0 and knows to put back: the generic serial port's
// (0x1101 twice) and the car maker's own pair.
bool knownBaseline(const std::string& payload);
// Bytes as lower-case hex, and back; false for text that is not an even number of hex digits.
std::string hexOf(const std::string& bytes);
bool bytesOf(const std::string& hex, std::string* out);
// Six bytes of an address as 00:11:22:33:44:55, in the order the stack gives them (phones.hpp's form).
std::string colonAddress(const std::string& six);

// What /proc/locks says of a file: whether an flock() is held on the file with this device and inode. `text` is
// the file's content; `major`, `minor` and `inode` are the file's (stat).
bool flockHeld(const std::string& text, unsigned major, unsigned minor, unsigned long inode);

}  // namespace alps

// A signal of the stack's, as the bus hands it over.
struct AlpsSignal {
    enum Kind {
        kSetUuidCfm, kDataInd, kDataCfm, kSendInd, kFlowControl, kConnectInd, kDisconnectInd, kReadConfigCfm,
        kSetConfigCfm,
        kBusLost   // the bus or the stack went away: the stack's owner on the bus changed, or the connection ended
    };
    Kind kind;
    uint8_t instance;
    uint8_t status;
    uint8_t reason;
    uint16_t id;           // a configuration's
    uint64_t function;
    std::string bytes;     // data, or a configuration's value
    std::string address;   // six bytes
    AlpsSignal() : kind(kBusLost), instance(0), status(0), reason(0), id(0), function(0) {}
};

// The requests, each sent with no reply. True if it went onto the bus.
class AlpsBus {
public:
    virtual ~AlpsBus() {}
    virtual bool setUuid(uint8_t instance, const std::string& payload) = 0;
    virtual bool sendData(uint8_t instance, const std::string& bytes) = 0;
    virtual bool readConfig(uint8_t instance, uint16_t id) = 0;
    virtual bool writeConfig(uint8_t instance, uint16_t id, const std::string& value) = 0;
    virtual bool disconnect(uint8_t instance) = 0;
};

// Whether another program has the Bluetooth instance's use now, which the agent then leaves it to.
class AlpsOthers {
public:
    virtual ~AlpsOthers() {}
    // True with *who saying which, and why it is taken to be at work.
    virtual bool active(std::string* who) = 0;
    // True if the other program has borrowed the instance itself, and has yet to put it back: then nothing of it is
    // touched, not even to put back what this agent changed.
    virtual bool holdsInstance() { return false; }
};

// EntryAad: its supervisor's lock, held (in /proc/locks, which is read and nothing taken), or its journal of the
// instance it borrowed.
class EntryAadWatch : public AlpsOthers {
public:
    EntryAadWatch()
        : lock_("/tmp/entry-aad-wireless.lock"), journal_("/tmp/entry-aa-service/bluetooth-retour.txt"),
          locks_("/proc/locks") {}
    EntryAadWatch(const std::string& lock, const std::string& journal, const std::string& locks)
        : lock_(lock), journal_(journal), locks_(locks) {}
    virtual bool active(std::string* who);
    virtual bool holdsInstance();

private:
    std::string lock_, journal_, locks_;
};

struct AlpsConfig {
    uint8_t instance;            // the instance to borrow
    std::string baseline;        // SetUuid's data to put back: what is on the instance before the agent takes it
    std::string uuid;            // the agent's service UUID
    std::string journalPath;     // what the agent changed, until it is put back: in its state folder
    unsigned confirmMs;          // a request with no confirmation for this long has failed
    unsigned sendMs;             // a chunk not sent for this long ends the link
    unsigned lingerMs;           // the agent's end closed and the phone's still up this long: the stack ends it
    unsigned retryMs;            // after a failure, the next try
    unsigned yieldQuietMs;       // after another program was at work, this long with none before the next borrow
    unsigned foreignBackoffMs;   // after a link that was not the agent's
    unsigned refreshMs[3];       // the UUID set again at these times after it was first set, while no link is up
    unsigned resetArmedMs;       // serial.reset with no link: the next link within this long is ended
    unsigned othersEveryMs;      // how often AlpsOthers is asked

    AlpsConfig();
};

class AlpsSerial : public Listener, public SerialReset, public SerialDial {
public:
    enum State {
        kOff,              // not opened, or nothing more is done (no baseline, stopped)
        kIdle,             // nothing borrowed: waiting to borrow
        kReadInterface,    // 0x6203 asked for
        kWriteInterface,   // 0x6203 set to 1
        kCheckInterface,   // and asked for again
        kSetUuid,          // the agent's UUID set
        kAdvertised,       // the instance is the agent's, and no link is up
        kLinkUp,           // a phone's link is up
        kEnding,           // a link is being ended before the instance goes back
        kRestoreUuid,      // the saved UUIDs set again
        kRestoreInterface, // the saved 0x6203 set again
        kCheckRestore      // and asked for again
    };

    AlpsSerial(AlpsBus* bus, AlpsOthers* others, SerialLog* log, const AlpsConfig& config);
    virtual ~AlpsSerial();

    // Checks the configuration, reads a journal that is there, and makes the listener's wake-up pipe. False with
    // *why (and nothing borrowed, ever) if the baseline is missing or not one that is known. Nothing goes on the bus.
    bool open(std::string* why);
    // A thread of this object's that calls step() every 20 ms, until stop().
    bool start(std::string* why);
    // Gives the instance back: ends a link that is up, and puts back what was saved, for waitMs at most. Then the
    // thread ends. True if nothing is left borrowed (and the journal is gone).
    bool stop(int waitMs);

    // From the bus's thread.
    void post(const AlpsSignal& signal);
    // Does what is due at `nowMs` (a monotonic clock in milliseconds): the signals posted, the requests, the pump.
    void step(int64_t nowMs);

    // Listener: the next link that is up, once; the pipe is readable while there is one.
    virtual int fd() const { return wakeRead_; }
    virtual Link* accept();

    // SerialReset: ends the link that is up; with none, the next one within resetArmedMs.
    virtual Outcome resetSerialLink(const std::string& device, const std::string& instance, const std::string& service,
                                    int64_t* forMs, std::string* why);
    // SerialDial: the car does not dial on EntryEvo.
    virtual void pairingWindow(bool) {}
    virtual bool dialOn() { return false; }
    virtual unsigned dialNow(std::string* why, const char** code);
    virtual SerialStatus serialStatus();

    State state();
    static const char* stateName(State state);
    bool borrowed();      // something of the instance is the agent's: the journal is there
    bool linkUp();

private:
    AlpsSerial(const AlpsSerial&);
    AlpsSerial& operator=(const AlpsSerial&);

    // All of these with mu_ held.
    void handle(const AlpsSignal& s, int64_t now);
    void enter(State next, int64_t now, unsigned timeoutMs = 0);
    void fail(const std::string& why, int64_t now, unsigned backoffMs);
    void borrow(int64_t now);
    void giveBack(const std::string& why, int64_t now, unsigned backoffMs = 0);
    void restoreNext(int64_t now);
    bool writeJournal(std::string* why);
    void openLink(const std::string& address, int64_t now);
    void endLink(const char* why);
    void pump(int64_t now);
    void say(const std::string& line);
    bool waitingCfm() const;

    static void* run(void* self);

    AlpsBus* bus_;
    AlpsOthers* others_;
    SerialLog* log_;
    AlpsConfig config_;

    pthread_mutex_t mu_;
    pthread_mutex_t queueMu_;
    std::deque<AlpsSignal> queue_;

    State state_;
    int64_t deadline_;        // of the request out, or of the wait, in the state; 0: none
    int64_t notBefore_;       // no borrow before this
    int64_t lastOthers_;      // when AlpsOthers was last asked
    int64_t othersSeen_;      // when another program was last seen at work; 0: never
    int64_t advertisedAt_;
    unsigned refreshed_;
    bool stopping_;
    bool canBorrow_;          // the baseline is one to put back: without it, a journal is put back and nothing taken

    // The journal: what goes back. Valid while journaled_.
    bool journaled_;
    uint8_t savedInstance_;
    uint8_t savedInterface_;
    std::string savedPair_;

    // The link.
    int pair_[2];             // [0] ours, [1] the agent's, until accept() hands it over
    bool linkReady_;          // pair_[1] waits for accept()
    bool phoneEnded_, agentEnded_, disconnectAsked_;
    int64_t agentEndedAt_;
    std::string address_;
    std::string rx_;          // from the phone, not yet in the socket
    bool txPending_, txAllowed_;
    int64_t txSince_;
    int64_t resetArmedUntil_;
    int64_t lastNow_;         // the clock of the last step(), for what the agent's thread asks in between

    int wakeRead_, wakeWrite_;

    pthread_t thread_;
    bool threadStarted_;
    bool threadStop_;

    // For serialStatus.
    int64_t registeredAt_;
    unsigned links_, yields_, foreign_, failures_;
};

}  // namespace nbt

#endif

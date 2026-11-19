// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// Host test for agent/alps_serial.cpp: EntryEvo's Bluetooth serial over the Alps stack, against a bus that stands in
// for D-Bus and the stack. It records each request; the test answers with the stack's signals, and drives the
// listener's step() with a clock of its own. No thread of the listener's runs.
//
// usage: test_alps_serial [work folder]     (default build/agent-host/work)

#include "../../agent/alps_serial.hpp"

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../../agent/fsutil.hpp"
#include "../../agent/protocol_constants.hpp"
#include "test_util.hpp"

using namespace nbt;

namespace {

class FakeBus : public AlpsBus {
public:
    FakeBus() : refuse(false) {}
    virtual bool setUuid(uint8_t instance, const std::string& payload) { return add(say("uuid", instance) + " " + alps::hexOf(payload)); }
    virtual bool sendData(uint8_t instance, const std::string& bytes) {
        sent += bytes;
        char n[16];
        snprintf(n, sizeof n, " %u", (unsigned)bytes.size());
        return add(say("data", instance) + n);
    }
    virtual bool readConfig(uint8_t instance, uint16_t id) { return add(say("read", instance) + idText(id)); }
    virtual bool writeConfig(uint8_t instance, uint16_t id, const std::string& value) {
        return add(say("write", instance) + idText(id) + " " + alps::hexOf(value));
    }
    virtual bool disconnect(uint8_t instance) { return add(say("disconnect", instance)); }

    // The requests since the last look, one to a line.
    std::string take() {
        std::string text;
        for (size_t i = 0; i < calls.size(); ++i) text += calls[i] + "\n";
        calls.clear();
        return text;
    }

    std::vector<std::string> calls;
    std::string sent;
    bool refuse;

private:
    bool add(const std::string& call) {
        calls.push_back(call);
        return !refuse;
    }
    static std::string say(const char* what, uint8_t instance) {
        char text[32];
        snprintf(text, sizeof text, "%s %u", what, (unsigned)instance);
        return text;
    }
    static std::string idText(uint16_t id) {
        char text[16];
        snprintf(text, sizeof text, " %04x", (unsigned)id);
        return text;
    }
};

class FakeOthers : public AlpsOthers {
public:
    FakeOthers() : on(false), holds(false) {}
    virtual bool active(std::string* who) {
        if (on && who != 0) *who = "EntryAad (the test's)";
        return on;
    }
    virtual bool holdsInstance() { return holds; }
    bool on, holds;
};

class Lines : public SerialLog {
public:
    virtual void line(const std::string& line) { text += line + "\n"; }
    bool has(const std::string& part) const { return text.find(part) != std::string::npos; }
    std::string text;
};

AlpsSignal sig(AlpsSignal::Kind kind, uint8_t status = 0) {
    AlpsSignal s;
    s.kind = kind;
    s.instance = 0;
    s.status = status;
    return s;
}

AlpsSignal config(AlpsSignal::Kind kind, unsigned value, uint8_t status = 0) {
    AlpsSignal s = sig(kind, status);
    s.id = alps::kDataInterfaceId;
    if (kind == AlpsSignal::kReadConfigCfm) s.bytes = std::string(1, (char)value);
    return s;
}

AlpsSignal phoneLink(bool up) {
    AlpsSignal s = sig(up ? AlpsSignal::kConnectInd : AlpsSignal::kDisconnectInd);
    s.function = alps::kFunctionSpp;
    s.address = std::string("\x01\x02\x03\x04\x05\x06", 6);
    return s;
}

AlpsSignal data(const std::string& bytes) {
    AlpsSignal s = sig(AlpsSignal::kDataInd);
    s.bytes = bytes;
    return s;
}

const std::string kGeneric = std::string("\x01\x11\x01\x01\x11\x01", 6);   // 0x1101 twice

std::string ourPair() { return alps::hexOf(alps::uuidPair(kSerialServiceUuid)); }

// Reads what the agent's end of a link has now: everything, until nothing more comes.
std::string drain(Link* l, bool* ended) {
    std::string got;
    char buffer[8192];
    *ended = false;
    for (;;) {
        long n = l->read(buffer, sizeof buffer);
        if (n > 0) {
            got.append(buffer, (size_t)n);
        } else {
            *ended = n < 0;
            return got;
        }
    }
}

// From idle to advertised, as a car with 0x6203 = 0 answers. Returns the clock after it.
int64_t borrow(AlpsSerial& serial, FakeBus& bus, int64_t now, const std::string& journal) {
    serial.step(now);
    std::string got = bus.take();
    CHECK_IN(got, got == "read 0 6203\n" && serial.state() == AlpsSerial::kReadInterface);
    CHECK(fs::kindOf(journal) == fs::kNone);
    serial.post(config(AlpsSignal::kReadConfigCfm, 0));
    serial.step(now + 10);
    got = bus.take();
    CHECK_IN(got, got == "write 0 6203 01\n" && serial.state() == AlpsSerial::kWriteInterface);
    // The journal is written before the first change: the instance, 0x6203 as it was, and the UUIDs to put back.
    CHECK_IN(testutil::readFile(journal), testutil::readFile(journal) == "NBTAPP_ALPS_V1 0 0 011101011101\n" && serial.borrowed());
    serial.post(config(AlpsSignal::kSetConfigCfm, 0));
    serial.step(now + 20);
    got = bus.take();
    CHECK_IN(got, got == "read 0 6203\n" && serial.state() == AlpsSerial::kCheckInterface);
    serial.post(config(AlpsSignal::kReadConfigCfm, 1));
    serial.step(now + 30);
    got = bus.take();
    CHECK_IN(got, got == "uuid 0 " + ourPair() + "\n" && serial.state() == AlpsSerial::kSetUuid);
    serial.post(sig(AlpsSignal::kSetUuidCfm));
    serial.step(now + 40);
    CHECK(serial.state() == AlpsSerial::kAdvertised && bus.take().empty());
    return now + 40;
}

// From a borrow to idle: the UUIDs, then 0x6203 = 0, then 0x6203 read back; the journal goes.
int64_t giveBack(AlpsSerial& serial, FakeBus& bus, int64_t now, const std::string& journal) {
    std::string got = bus.take();
    CHECK_IN(got, got == "uuid 0 011101011101\n" && serial.state() == AlpsSerial::kRestoreUuid);
    serial.post(sig(AlpsSignal::kSetUuidCfm));
    serial.step(now + 10);
    got = bus.take();
    CHECK_IN(got, got == "write 0 6203 00\n" && serial.state() == AlpsSerial::kRestoreInterface);
    serial.post(config(AlpsSignal::kSetConfigCfm, 0));
    serial.step(now + 20);
    got = bus.take();
    CHECK_IN(got, got == "read 0 6203\n" && serial.state() == AlpsSerial::kCheckRestore);
    CHECK(fs::kindOf(journal) == fs::kFile);
    serial.post(config(AlpsSignal::kReadConfigCfm, 0));
    serial.step(now + 30);
    CHECK(fs::kindOf(journal) == fs::kNone && !serial.borrowed());
    return now + 30;
}

}  // namespace

int main(int argc, char** argv) {
    std::string work = std::string(argc > 1 ? argv[1] : "build/agent-host/work") + "/alps-serial";
    std::string why;
    CHECK_IN(why, fs::removeTree(work, &why) && fs::makeDirs(work, &why));
    const std::string journal = work + "/alps-journal";

    // 1. The pieces.
    const std::string pair = alps::uuidPair(kSerialServiceUuid);
    CHECK(pair.size() == 34 && pair[0] == '\x03' && pair[17] == '\x03' && alps::validPair(pair));
    CHECK(alps::hexOf(pair).compare(0, 10, "0358876f35") == 0);
    CHECK(alps::uuidPair("58876F35-7F3F-4592-B65D-9912589A4F5").empty() && alps::uuidPair("58876F357F3F4592B65D9912589A4F51xxxx").empty());
    CHECK(alps::validPair(kGeneric) && alps::knownBaseline(kGeneric) && !alps::knownBaseline(pair));
    CHECK(!alps::validPair(std::string("\x01\x11", 2)) && !alps::validPair(kGeneric + "x") && !alps::validPair(std::string("\x04\x11\x01", 3)));
    std::string bytes;
    CHECK(alps::bytesOf("0300000000decafadedecadeafdecacaff0300000000decafadedecadeafdecacafe", &bytes) &&
          alps::knownBaseline(bytes) && alps::validPair(bytes));
    CHECK(!alps::bytesOf("0g", &bytes) && !alps::bytesOf("012", &bytes));
    CHECK(alps::colonAddress(std::string("\x00\x1a\xff\x10\x02\xab", 6)) == "00:1A:FF:10:02:AB" && alps::colonAddress("abc").empty());
    const std::string locks =
        "1: POSIX  ADVISORY  WRITE 812 00:0e:1234 0 EOF\n"
        "2: FLOCK  ADVISORY  WRITE 977 00:0e:4711 0 EOF\n"
        "2: -> FLOCK  ADVISORY  WRITE 978 00:0f:4712 0 EOF\n";
    CHECK(alps::flockHeld(locks, 0, 0x0e, 4711) && !alps::flockHeld(locks, 0, 0x0e, 1234));
    CHECK(!alps::flockHeld(locks, 0, 0x0f, 4712) && !alps::flockHeld(locks, 0, 0x0e, 47110) && !alps::flockHeld("", 0, 0x0e, 4711));
    {
        // EntryAad's journal, and its lock as /proc/locks has it.
        const std::string lockFile = work + "/entry-aad-wireless.lock", aaJournal = work + "/bluetooth-retour.txt";
        EntryAadWatch none(lockFile, aaJournal, work + "/locks");
        CHECK(!none.active(&why) && !none.holdsInstance());
        CHECK(fs::writeFile(aaJournal, "ENTRY_AA_TRIAL_V1\n", 0600, &why));
        CHECK(none.active(&why) && none.holdsInstance() && why.find("journal") != std::string::npos);
        CHECK(fs::removeFile(aaJournal, &why));
    }

    // 2. Nothing is borrowed with no UUIDs to put back, or with ones that are not known.
    {
        FakeBus bus;
        Lines log;
        AlpsConfig c;
        c.journalPath = journal;
        AlpsSerial none(&bus, 0, &log, c);
        CHECK_IN(why, !none.open(&why) && why.find("not known") != std::string::npos && none.state() == AlpsSerial::kOff);
        c.baseline = pair;
        AlpsSerial strange(&bus, 0, &log, c);
        CHECK_IN(why, !strange.open(&why) && why.find("not a pair") != std::string::npos);
        strange.step(0);
        CHECK(bus.take().empty());
    }

    FakeBus bus;
    FakeOthers others;
    Lines log;
    AlpsConfig c;
    c.baseline = kGeneric;
    c.journalPath = journal;
    AlpsSerial serial(&bus, &others, &log, c);
    CHECK_IN(why, serial.open(&why) && serial.state() == AlpsSerial::kIdle && serial.fd() >= 0);
    CHECK(serial.accept() == 0 && !serial.dialOn());

    // 3. The borrow.
    int64_t now = borrow(serial, bus, 1000, journal);
    CHECK(log.has("is the agent's: service " + std::string(kSerialServiceUuid)));
    SerialStatus st = serial.serialStatus();
    CHECK(st.registered && st.link == "none" && !st.dialOn);

    // 4. A link: from the phone to the agent, and back in chunks, one at a time, with flow control.
    serial.post(phoneLink(true));
    serial.step(now += 10);
    CHECK(serial.state() == AlpsSerial::kLinkUp && serial.linkUp());
    Link* l = serial.accept();
    CHECK(l != 0 && std::string(l->kind()) == "serial" && l->phoneAddress() == "01:02:03:04:05:06" && serial.accept() == 0);
    CHECK(!log.has("01:02:03"));   // never an address in the log
    serial.post(data("NBTA"));
    serial.post(data("hello"));
    serial.step(now += 10);
    bool ended = false;
    CHECK(drain(l, &ended) == "NBTAhello" && !ended);
    std::string out(3000, 'x');
    out[0] = 'a';
    out[2999] = 'z';
    CHECK(l->write(out.data(), out.size()) == (long)out.size());
    serial.step(now += 10);
    CHECK_IN(bus.calls.size() == 1 ? bus.calls[0] : "", bus.take() == "data 0 1024\n");
    serial.step(now += 10);
    CHECK(bus.take().empty());                        // nothing more until SendInd
    serial.post(sig(AlpsSignal::kSendInd));
    serial.step(now += 10);
    CHECK(bus.take() == "data 0 1024\n");
    serial.post(sig(AlpsSignal::kFlowControl, 1));    // paused
    serial.post(sig(AlpsSignal::kSendInd));
    serial.step(now += 10);
    CHECK(bus.take().empty());
    serial.post(sig(AlpsSignal::kFlowControl, 0));    // and on again
    serial.step(now += 10);
    CHECK(bus.take() == "data 0 952\n");
    serial.post(sig(AlpsSignal::kSendInd));
    serial.step(now += 10);
    CHECK(bus.sent == out && bus.take().empty());
    st = serial.serialStatus();
    CHECK(st.link == "phone-opened");

    // 5. The phone ends the link: the agent's end reads to its end, and the instance stays the agent's.
    serial.post(data("bye"));
    serial.post(phoneLink(false));
    serial.step(now += 10);
    CHECK(drain(l, &ended) == "bye" && ended);
    CHECK(serial.state() == AlpsSerial::kAdvertised && !serial.linkUp() && serial.borrowed());
    delete l;

    // 6. The agent's end closes first: the phone has lingerMs to end the link, then the stack is asked to.
    serial.post(phoneLink(true));
    serial.step(now += 10);
    l = serial.accept();
    CHECK(l != 0);
    l->close();
    serial.step(now += 10);
    CHECK(bus.take().empty());
    serial.step(now += c.lingerMs);
    CHECK(bus.take() == "disconnect 0\n");
    serial.post(phoneLink(false));
    serial.step(now += 10);
    CHECK(serial.state() == AlpsSerial::kAdvertised && !serial.linkUp());
    delete l;

    // 7. serial.reset: with a link up, the stack is asked to end it; with none, the next one is ended at once.
    int64_t forMs = 0;
    CHECK(serial.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetArmed && forMs == (int64_t)c.resetArmedMs);
    serial.post(phoneLink(true));
    serial.step(now += 10);
    CHECK(bus.take() == "disconnect 0\n" && serial.accept() == 0 && !serial.linkUp());
    serial.post(phoneLink(false));
    serial.step(now += 10);
    serial.post(phoneLink(true));
    serial.step(now += 10);
    l = serial.accept();
    CHECK(l != 0 && serial.resetSerialLink("", "", "", &forMs, &why) == SerialReset::kResetSent && bus.take() == "disconnect 0\n");
    serial.post(phoneLink(false));
    serial.step(now += 10);
    delete l;

    // 8. Android Auto starts: the link is ended, the instance goes back, and nothing is borrowed while EntryAad is
    // at work, nor for yieldQuietMs after.
    serial.post(phoneLink(true));
    serial.step(now += 10);
    l = serial.accept();
    others.on = true;
    serial.step(now += c.othersEveryMs);
    CHECK(serial.state() == AlpsSerial::kEnding && bus.take() == "disconnect 0\n" && log.has("Android Auto comes first"));
    serial.post(phoneLink(false));
    serial.step(now += 10);
    CHECK(drain(l, &ended).empty() && ended);
    delete l;
    now = giveBack(serial, bus, now, journal);
    CHECK(serial.state() == AlpsSerial::kIdle);
    serial.step(now += c.othersEveryMs);
    serial.step(now += 60000);
    CHECK(bus.take().empty() && serial.state() == AlpsSerial::kIdle);
    others.on = false;
    serial.step(now += c.othersEveryMs);
    CHECK(bus.take().empty());                        // not yet: EntryAad was seen less than yieldQuietMs ago
    now += c.yieldQuietMs;
    now = borrow(serial, bus, now, journal);

    // 9. Bytes with no link of the agent's: a link of the car maker's service, which goes to nobody. The instance
    // goes back, and is not taken again for foreignBackoffMs.
    serial.post(data("not ours"));
    serial.step(now += 10);
    now = giveBack(serial, bus, now, journal);
    CHECK(log.has("hands to nobody") && serial.serialStatus().refusals == 1);
    serial.step(now += c.foreignBackoffMs / 2);
    CHECK(bus.take().empty());
    now = borrow(serial, bus, now + c.foreignBackoffMs, journal);

    // 10. No confirmation of the UUID that is set again: the instance goes back.
    serial.step(now += c.refreshMs[0]);
    CHECK(bus.take() == "uuid 0 " + ourPair() + "\n" && serial.state() == AlpsSerial::kSetUuid);
    serial.step(now += c.confirmMs);
    now = giveBack(serial, bus, now, journal);
    CHECK(log.has("no confirmation while setting the UUID"));
    now = borrow(serial, bus, now + c.retryMs, journal);

    // 11. Stopping with a link up: the link is ended, the instance goes back, and then nothing more is done.
    serial.post(phoneLink(true));
    serial.step(now += 10);
    l = serial.accept();
    CHECK(!serial.stop(0));                           // no thread here: the steps below do the work
    serial.step(now += 10);
    CHECK(bus.take() == "disconnect 0\n" && serial.state() == AlpsSerial::kEnding);
    serial.post(phoneLink(false));
    serial.step(now += 10);
    now = giveBack(serial, bus, now, journal);
    CHECK(serial.state() == AlpsSerial::kOff);
    serial.step(now += 100000);
    CHECK(bus.take().empty());
    delete l;

    // 12. A journal an agent left: put back first. With no UUIDs given beside it, nothing is borrowed after.
    {
        CHECK(fs::writeFile(journal, "NBTAPP_ALPS_V1 0 0 011101011101\n", 0600, &why));
        FakeBus bus2;
        Lines log2;
        AlpsConfig c2;
        c2.journalPath = journal;
        AlpsSerial again(&bus2, &others, &log2, c2);
        CHECK_IN(why, again.open(&why) && again.borrowed() && log2.has("put back and not taken again"));
        again.step(0);
        giveBack(again, bus2, 0, journal);
        again.step(100000);
        CHECK(bus2.take().empty() && again.state() == AlpsSerial::kIdle);
        // A journal while EntryAad has one of its own: left alone.
        CHECK(fs::writeFile(journal, "NBTAPP_ALPS_V1 0 0 011101011101\n", 0600, &why));
        others.holds = true;
        AlpsSerial held(&bus2, &others, &log2, c2);
        CHECK(held.open(&why));
        held.step(0);
        CHECK(bus2.take().empty() && held.borrowed());
        others.holds = false;
        held.step(c2.retryMs);
        giveBack(held, bus2, c2.retryMs, journal);
        // A journal that is not one of the agent's is refused whole.
        CHECK(fs::writeFile(journal, "something else\n", 0600, &why));
        AlpsSerial odd(&bus2, &others, &log2, c2);
        CHECK_IN(why, !odd.open(&why) && why.find("not one this agent wrote") != std::string::npos);
        CHECK(fs::removeFile(journal, &why));
    }

    // 13. The stack goes away while the instance is borrowed: the journal stays, and the instance is put back once
    // the stack is there again.
    {
        FakeBus bus3;
        Lines log3;
        AlpsSerial lost(&bus3, 0, &log3, c);
        CHECK(lost.open(&why));
        int64_t t = borrow(lost, bus3, 0, journal);
        lost.post(sig(AlpsSignal::kBusLost));
        lost.step(t += 10);
        CHECK(lost.state() == AlpsSerial::kIdle && lost.borrowed() && bus3.take().empty());
        lost.step(t += c.retryMs);
        giveBack(lost, bus3, t, journal);
    }

    CHECK(fs::removeTree(work, &why));
    printf("test_alps_serial: ok\n");
    return 0;
}

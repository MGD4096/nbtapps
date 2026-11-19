// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
// The system D-Bus, for the Alps stack: see alps_dbus.hpp. Only the EntryEvo build has libdbus's headers; every
// other build gets a bus that is never there.

#include "alps_dbus.hpp"

#ifdef NBT_ENTRYEVO

#include <dbus/dbus.h>
#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace nbt {

namespace {

// The functions of libdbus this file calls, each found in the library by its name: the program has no link to it.
#define NBT_DBUS_FUNCTIONS(X)                                                                                         \
    X(dbus_threads_init_default) X(dbus_error_init) X(dbus_error_free) X(dbus_error_is_set) X(dbus_bus_get_private)  \
    X(dbus_bus_add_match) X(dbus_connection_set_exit_on_disconnect) X(dbus_connection_close)                         \
    X(dbus_connection_unref) X(dbus_connection_add_filter) X(dbus_connection_remove_filter)                          \
    X(dbus_connection_read_write_dispatch) X(dbus_connection_send) X(dbus_connection_send_with_reply_and_block)      \
    X(dbus_message_new_method_call) X(dbus_message_append_args) X(dbus_message_get_args) X(dbus_message_unref)       \
    X(dbus_message_set_no_reply) X(dbus_message_is_signal) X(dbus_message_get_type) X(dbus_message_has_path)         \
    X(dbus_message_has_sender) X(dbus_message_has_interface) X(dbus_message_get_member)                              \
    X(dbus_message_has_signature) X(dbus_message_iter_init) X(dbus_message_iter_next)                                \
    X(dbus_message_iter_get_arg_type) X(dbus_message_iter_get_element_type) X(dbus_message_iter_get_basic)           \
    X(dbus_message_iter_recurse) X(dbus_message_iter_get_fixed_array) X(dbus_message_iter_init_append)               \
    X(dbus_message_iter_append_basic) X(dbus_message_iter_open_container) X(dbus_message_iter_append_fixed_array)    \
    X(dbus_message_iter_close_container) X(dbus_message_iter_abandon_container)

struct Api {
#define NBT_DBUS_DECLARE(name) __typeof__(&::name) name;
    NBT_DBUS_FUNCTIONS(NBT_DBUS_DECLARE)
#undef NBT_DBUS_DECLARE
};

// Loaded once per process, and never unloaded.
pthread_mutex_t gApiLock = PTHREAD_MUTEX_INITIALIZER;
Api gApi;
bool gApiLoaded = false;
bool gThreadsReady = false;

bool loadApi(std::string* why) {
    pthread_mutex_lock(&gApiLock);
    if (!gApiLoaded) {
        void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (lib == 0) {
            const char* e = dlerror();
            if (why != 0) *why = std::string("libdbus-1.so.3 cannot be loaded: ") + (e != 0 ? e : "?");
            pthread_mutex_unlock(&gApiLock);
            return false;
        }
        std::string missing;
#define NBT_DBUS_RESOLVE(name)                                                         \
    gApi.name = reinterpret_cast<__typeof__(&::name)>(dlsym(lib, #name));               \
    if (gApi.name == 0) missing += std::string(missing.empty() ? "" : " ") + #name;
        NBT_DBUS_FUNCTIONS(NBT_DBUS_RESOLVE)
#undef NBT_DBUS_RESOLVE
        if (!missing.empty()) {
            if (why != 0) *why = "libdbus-1.so.3 has no " + missing;
            pthread_mutex_unlock(&gApiLock);
            return false;
        }
        gThreadsReady = gApi.dbus_threads_init_default() != 0;
        gApiLoaded = true;
    }
    bool ready = gThreadsReady;
    pthread_mutex_unlock(&gApiLock);
    if (!ready && why != 0) *why = "libdbus cannot be made safe for threads";
    return ready;
}

int64_t monoMs() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// The basic value of the iterator's argument, of the type asked for; false if it is another.
template <typename T>
bool basic(DBusMessageIter* it, int type, T* out) {
    if (gApi.dbus_message_iter_get_arg_type(it) != type) return false;
    gApi.dbus_message_iter_get_basic(it, out);
    return true;
}

bool byteArray(DBusMessageIter* it, std::string* out) {
    if (gApi.dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY ||
        gApi.dbus_message_iter_get_element_type(it) != DBUS_TYPE_BYTE) {
        return false;
    }
    DBusMessageIter inner;
    gApi.dbus_message_iter_recurse(it, &inner);
    const unsigned char* data = 0;
    int n = 0;
    gApi.dbus_message_iter_get_fixed_array(&inner, &data, &n);
    if (n < 0) return false;
    out->assign(reinterpret_cast<const char*>(data), (size_t)n);
    return true;
}

}  // namespace

struct DbusAlpsBus::Impl {
    SerialLog* log;
    AlpsSerial* sink;
    pthread_mutex_t mu;          // conn and owner, for the threads that send
    DBusConnection* conn;
    std::string owner;
    bool filtered;
    bool stop;
    bool started;
    pthread_t thread;
    int64_t lastTry;
    bool saidNoBus;

    Impl() : log(0), sink(0), conn(0), filtered(false), stop(false), started(false), lastTry(0), saidNoBus(false) {
        pthread_mutex_init(&mu, 0);
    }
    ~Impl() { pthread_mutex_destroy(&mu); }

    void say(const std::string& line) {
        if (log != 0) log->line("alps bus: " + line);
    }

    void lost() {
        AlpsSignal s;
        s.kind = AlpsSignal::kBusLost;
        if (sink != 0) sink->post(s);
    }

    // Who owns the stack's name now; empty if nobody.
    std::string askOwner(DBusConnection* c) {
        DBusError e;
        gApi.dbus_error_init(&e);
        DBusMessage* q = gApi.dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "GetNameOwner");
        const char* name = alps::kService;
        std::string found;
        if (q != 0 && gApi.dbus_message_append_args(q, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
            DBusMessage* reply = gApi.dbus_connection_send_with_reply_and_block(c, q, 2000, &e);
            const char* owner = 0;
            if (reply != 0) {
                if (gApi.dbus_message_get_args(reply, 0, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) && owner != 0) found = owner;
                gApi.dbus_message_unref(reply);
            }
        }
        if (q != 0) gApi.dbus_message_unref(q);
        gApi.dbus_error_free(&e);
        return found;
    }

    bool connect(std::string* why) {
        DBusError e;
        gApi.dbus_error_init(&e);
        // A private connection: closing it closes nothing of another component's.
        DBusConnection* c = gApi.dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
        if (c == 0) {
            if (why != 0) *why = std::string("the system bus is not there: ") + (gApi.dbus_error_is_set(&e) && e.message ? e.message : "?");
            gApi.dbus_error_free(&e);
            return false;
        }
        gApi.dbus_connection_set_exit_on_disconnect(c, FALSE);
        if (!gApi.dbus_connection_add_filter(c, &Impl::filter, this, 0)) {
            if (why != 0) *why = "no filter on the bus";
            gApi.dbus_connection_close(c);
            gApi.dbus_connection_unref(c);
            return false;
        }
        const std::string rules[] = {
            std::string("type='signal',sender='") + alps::kService + "',path='" + alps::kPath + "',interface='" + alps::kSppInterface + "'",
            std::string("type='signal',sender='") + alps::kService + "',path='" + alps::kPath + "',interface='" + alps::kDmInterface + "'",
            std::string("type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='") +
                alps::kService + "'"};
        for (size_t i = 0; i < sizeof rules / sizeof rules[0]; ++i) {
            gApi.dbus_bus_add_match(c, rules[i].c_str(), &e);
            if (gApi.dbus_error_is_set(&e)) {
                if (why != 0) *why = std::string("the bus takes no match: ") + (e.message ? e.message : "?");
                gApi.dbus_error_free(&e);
                gApi.dbus_connection_remove_filter(c, &Impl::filter, this);
                gApi.dbus_connection_close(c);
                gApi.dbus_connection_unref(c);
                return false;
            }
        }
        std::string found = askOwner(c);
        pthread_mutex_lock(&mu);
        conn = c;
        owner = found;
        filtered = true;
        pthread_mutex_unlock(&mu);
        saidNoBus = false;
        say(found.empty() ? "connected; the Alps stack (org.alps.evolution) is not on the bus yet" : "connected to the Alps stack");
        return true;
    }

    void drop() {
        pthread_mutex_lock(&mu);
        DBusConnection* c = conn;
        conn = 0;
        owner.clear();
        bool f = filtered;
        filtered = false;
        pthread_mutex_unlock(&mu);
        if (c == 0) return;
        if (f) gApi.dbus_connection_remove_filter(c, &Impl::filter, this);
        gApi.dbus_connection_close(c);
        gApi.dbus_connection_unref(c);
    }

    static void* run(void* arg) {
        Impl* p = static_cast<Impl*>(arg);
        for (;;) {
            pthread_mutex_lock(&p->mu);
            bool stopNow = p->stop;
            DBusConnection* c = p->conn;
            pthread_mutex_unlock(&p->mu);
            if (stopNow) break;
            if (c == 0) {
                if (monoMs() - p->lastTry >= 5000) {
                    p->lastTry = monoMs();
                    std::string why;
                    if (!p->connect(&why) && !p->saidNoBus) {
                        p->saidNoBus = true;
                        p->say(why + "; trying again every 5 s");
                    }
                }
                usleep(100000);
                continue;
            }
            if (!gApi.dbus_connection_read_write_dispatch(c, 50)) {
                p->say("the connection to the bus ended");
                p->drop();
                p->lost();
                continue;
            }
            usleep(1000);   // the threads that send get libdbus's lock in between
        }
        p->drop();
        return 0;
    }

    static DBusHandlerResult filter(DBusConnection*, DBusMessage* m, void* arg) {
        static_cast<Impl*>(arg)->onMessage(m);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    void onMessage(DBusMessage* m) {
        if (gApi.dbus_message_is_signal(m, DBUS_INTERFACE_DBUS, "NameOwnerChanged")) {
            const char *name = 0, *before = 0, *after = 0;
            if (gApi.dbus_message_has_sender(m, DBUS_SERVICE_DBUS) &&
                gApi.dbus_message_get_args(m, 0, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &before, DBUS_TYPE_STRING,
                                           &after, DBUS_TYPE_INVALID) &&
                name != 0 && strcmp(name, alps::kService) == 0) {
                pthread_mutex_lock(&mu);
                owner = after != 0 ? after : "";
                bool there = !owner.empty();
                pthread_mutex_unlock(&mu);
                say(there ? "the Alps stack has a new owner on the bus: it was started again" : "the Alps stack left the bus");
                lost();
            }
            return;
        }
        if (gApi.dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_SIGNAL || !gApi.dbus_message_has_path(m, alps::kPath)) return;
        pthread_mutex_lock(&mu);
        std::string from = owner;
        pthread_mutex_unlock(&mu);
        if (from.empty() || !gApi.dbus_message_has_sender(m, from.c_str())) return;
        const char* member = gApi.dbus_message_get_member(m);
        if (member == 0) return;
        AlpsSignal s;
        DBusMessageIter it;
        if (!gApi.dbus_message_iter_init(m, &it)) {
            // Only SendInd has a single argument; every signal has one at least.
            return;
        }
        bool ok = false;
        if (gApi.dbus_message_has_interface(m, alps::kSppInterface)) {
            if (!basic(&it, DBUS_TYPE_BYTE, &s.instance)) return;
            if (strcmp(member, "BtApplSppSetUuidCfm") == 0 && gApi.dbus_message_has_signature(m, "yy")) {
                s.kind = AlpsSignal::kSetUuidCfm;
                ok = gApi.dbus_message_iter_next(&it) && basic(&it, DBUS_TYPE_BYTE, &s.status);
            } else if (strcmp(member, "BtApplSppDataInd") == 0 && gApi.dbus_message_has_signature(m, "yqay")) {
                dbus_uint16_t len = 0;
                s.kind = AlpsSignal::kDataInd;
                ok = gApi.dbus_message_iter_next(&it) && basic(&it, DBUS_TYPE_UINT16, &len) && gApi.dbus_message_iter_next(&it) &&
                     byteArray(&it, &s.bytes) && s.bytes.size() == len;
            } else if (strcmp(member, "BtApplSppDataCfm") == 0 && gApi.dbus_message_has_signature(m, "yy")) {
                s.kind = AlpsSignal::kDataCfm;
                ok = gApi.dbus_message_iter_next(&it) && basic(&it, DBUS_TYPE_BYTE, &s.status);
            } else if (strcmp(member, "BtApplSppSendInd") == 0 && gApi.dbus_message_has_signature(m, "y")) {
                s.kind = AlpsSignal::kSendInd;
                ok = true;
            } else if (strcmp(member, "BtApplSppDataSendFcInd") == 0 && gApi.dbus_message_has_signature(m, "yy")) {
                s.kind = AlpsSignal::kFlowControl;
                ok = gApi.dbus_message_iter_next(&it) && basic(&it, DBUS_TYPE_BYTE, &s.status);
            }
        } else if (gApi.dbus_message_has_interface(m, alps::kDmInterface)) {
            const bool up = strcmp(member, "BtApplDeviceConnectInd") == 0;
            const bool down = strcmp(member, "BtApplDeviceDisconnectInd") == 0;
            if ((up && gApi.dbus_message_has_signature(m, "yayty")) || (down && gApi.dbus_message_has_signature(m, "yayyty"))) {
                dbus_uint64_t function = 0;
                s.kind = up ? AlpsSignal::kConnectInd : AlpsSignal::kDisconnectInd;
                ok = basic(&it, DBUS_TYPE_BYTE, &s.status) && gApi.dbus_message_iter_next(&it) && byteArray(&it, &s.address) &&
                     s.address.size() == 6 && gApi.dbus_message_iter_next(&it);
                if (ok && down) ok = basic(&it, DBUS_TYPE_BYTE, &s.reason) && gApi.dbus_message_iter_next(&it);
                ok = ok && basic(&it, DBUS_TYPE_UINT64, &function) && gApi.dbus_message_iter_next(&it) &&
                     basic(&it, DBUS_TYPE_BYTE, &s.instance);
                s.function = function;
            } else if (strcmp(member, "BtApplReadConfigurationCfm") == 0 && gApi.dbus_message_has_signature(m, "yyqqay")) {
                dbus_uint16_t id = 0, len = 0;
                s.kind = AlpsSignal::kReadConfigCfm;
                ok = basic(&it, DBUS_TYPE_BYTE, &s.status) && gApi.dbus_message_iter_next(&it) &&
                     basic(&it, DBUS_TYPE_BYTE, &s.instance) && gApi.dbus_message_iter_next(&it) &&
                     basic(&it, DBUS_TYPE_UINT16, &id) && gApi.dbus_message_iter_next(&it) &&
                     basic(&it, DBUS_TYPE_UINT16, &len) && gApi.dbus_message_iter_next(&it) && byteArray(&it, &s.bytes) &&
                     s.bytes.size() == len;
                s.id = id;
            } else if (strcmp(member, "BtApplSetConfigurationCfm") == 0 && gApi.dbus_message_has_signature(m, "yyq")) {
                dbus_uint16_t id = 0;
                s.kind = AlpsSignal::kSetConfigCfm;
                ok = basic(&it, DBUS_TYPE_BYTE, &s.status) && gApi.dbus_message_iter_next(&it) &&
                     basic(&it, DBUS_TYPE_BYTE, &s.instance) && gApi.dbus_message_iter_next(&it) &&
                     basic(&it, DBUS_TYPE_UINT16, &id);
                s.id = id;
            }
        }
        if (ok && sink != 0) sink->post(s);
    }

    // A request with no reply, to the stack as it is now. False if there is no connection or no stack, or the
    // message cannot be made or sent.
    DBusMessage* newRequest(const char* interface, const char* member, std::string* to) {
        if (conn == 0 || owner.empty()) return 0;
        *to = owner;
        DBusMessage* m = gApi.dbus_message_new_method_call(to->c_str(), alps::kPath, interface, member);
        if (m != 0) gApi.dbus_message_set_no_reply(m, TRUE);
        return m;
    }

    bool send(DBusMessage* m, bool built) {
        bool sent = built && gApi.dbus_connection_send(conn, m, 0);
        gApi.dbus_message_unref(m);
        return sent;
    }

    // (y instance, [q id,] q length, ay data)
    bool appendBytes(DBusMessage* m, uint8_t instance, const std::string& data, const uint16_t* id) {
        if (data.size() > 65535) return false;
        DBusMessageIter it, array;
        gApi.dbus_message_iter_init_append(m, &it);
        dbus_uint16_t len = (dbus_uint16_t)data.size();
        if (!gApi.dbus_message_iter_append_basic(&it, DBUS_TYPE_BYTE, &instance)) return false;
        if (id != 0) {
            dbus_uint16_t value = *id;
            if (!gApi.dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT16, &value)) return false;
        }
        if (!gApi.dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT16, &len) ||
            !gApi.dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "y", &array)) {
            return false;
        }
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data.data());
        if (!data.empty() && !gApi.dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE, &bytes, (int)data.size())) {
            gApi.dbus_message_iter_abandon_container(&it, &array);
            return false;
        }
        return gApi.dbus_message_iter_close_container(&it, &array) != 0;
    }

    bool bytesRequest(const char* interface, const char* member, uint8_t instance, const std::string& data, const uint16_t* id) {
        pthread_mutex_lock(&mu);
        std::string to;
        DBusMessage* m = newRequest(interface, member, &to);
        bool sent = m != 0 && send(m, appendBytes(m, instance, data, id));
        pthread_mutex_unlock(&mu);
        return sent;
    }
};

DbusAlpsBus::DbusAlpsBus(SerialLog* log) : p_(new Impl) { p_->log = log; }

DbusAlpsBus::~DbusAlpsBus() {
    close();
    delete p_;
}

bool DbusAlpsBus::open(AlpsSerial* sink, std::string* why) {
    if (p_->started) {
        if (why != 0) *why = "already open";
        return false;
    }
    if (!loadApi(why)) return false;
    p_->sink = sink;
    p_->stop = false;
    p_->lastTry = monoMs();
    if (!p_->connect(why)) return false;
    if (pthread_create(&p_->thread, 0, &Impl::run, p_) != 0) {
        if (why != 0) *why = "no thread for the bus";
        p_->drop();
        return false;
    }
    p_->started = true;
    return true;
}

void DbusAlpsBus::close() {
    if (!p_->started) return;
    pthread_mutex_lock(&p_->mu);
    p_->stop = true;
    pthread_mutex_unlock(&p_->mu);
    pthread_join(p_->thread, 0);
    p_->started = false;
}

bool DbusAlpsBus::setUuid(uint8_t instance, const std::string& payload) {
    return p_->bytesRequest(alps::kSppInterface, "BtApplSppSetUuidReq", instance, payload, 0);
}

bool DbusAlpsBus::sendData(uint8_t instance, const std::string& bytes) {
    return p_->bytesRequest(alps::kSppInterface, "BtApplSppDataReq", instance, bytes, 0);
}

bool DbusAlpsBus::writeConfig(uint8_t instance, uint16_t id, const std::string& value) {
    return p_->bytesRequest(alps::kDmInterface, "BtApplSetConfigurationReq", instance, value, &id);
}

bool DbusAlpsBus::readConfig(uint8_t instance, uint16_t id) {
    pthread_mutex_lock(&p_->mu);
    std::string to;
    DBusMessage* m = p_->newRequest(alps::kDmInterface, "BtApplReadConfigurationReq", &to);
    dbus_uint16_t value = id;
    bool sent = m != 0 &&
                p_->send(m, gApi.dbus_message_append_args(m, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_UINT16, &value, DBUS_TYPE_INVALID));
    pthread_mutex_unlock(&p_->mu);
    return sent;
}

bool DbusAlpsBus::disconnect(uint8_t instance) {
    pthread_mutex_lock(&p_->mu);
    std::string to;
    DBusMessage* m = p_->newRequest(alps::kDmInterface, "BtApplDeviceDisconnectReq", &to);
    dbus_uint64_t function = alps::kFunctionSpp;
    bool sent = m != 0 &&
                p_->send(m, gApi.dbus_message_append_args(m, DBUS_TYPE_UINT64, &function, DBUS_TYPE_BYTE, &instance, DBUS_TYPE_INVALID));
    pthread_mutex_unlock(&p_->mu);
    return sent;
}

}  // namespace nbt

#else  // not the EntryEvo build: no bus

namespace nbt {

struct DbusAlpsBus::Impl {};

DbusAlpsBus::DbusAlpsBus(SerialLog*) : p_(0) {}
DbusAlpsBus::~DbusAlpsBus() {}
bool DbusAlpsBus::open(AlpsSerial*, std::string* why) {
    if (why != 0) *why = "this build has no D-Bus for the Alps stack: it is EntryEvo's (NBT_ENTRYEVO)";
    return false;
}
void DbusAlpsBus::close() {}
bool DbusAlpsBus::setUuid(uint8_t, const std::string&) { return false; }
bool DbusAlpsBus::sendData(uint8_t, const std::string&) { return false; }
bool DbusAlpsBus::readConfig(uint8_t, uint16_t) { return false; }
bool DbusAlpsBus::writeConfig(uint8_t, uint16_t, const std::string&) { return false; }
bool DbusAlpsBus::disconnect(uint8_t) { return false; }

}  // namespace nbt

#endif

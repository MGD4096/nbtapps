// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_ALPS_DBUS_HPP
#define NBT_AGENT_ALPS_DBUS_HPP

// The bus that AlpsSerial (alps_serial.hpp) speaks to the car's Alps stack over: the system D-Bus, through the car's
// own libdbus-1.so.3, which is loaded when open() is called (dlopen), so that the agent's program asks the car for
// glibc's libraries only and builds with no copy of the car's system. The EntryEvo build only (NBT_ENTRYEVO); in
// any other the class is there and open() says that there is no bus.
//
// open() makes a private connection to the system bus (another component's shared connection is never touched),
// asks who owns org.alps.evolution, listens for the stack's signals on org.alps.evo.SPP and org.alps.evo.DM and for
// a change of owner, and starts a thread that dispatches them: each becomes an AlpsSignal posted to the sink. When
// the stack goes or comes back, or the connection ends, the sink gets kBusLost; the thread connects again every
// 5 seconds while there is none, and a request in the meantime is refused (false).
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <pthread.h>
#include <stdint.h>

#include <string>

#include "alps_serial.hpp"
#include "serial_log.hpp"

namespace nbt {

class DbusAlpsBus : public AlpsBus {
public:
    explicit DbusAlpsBus(SerialLog* log);
    virtual ~DbusAlpsBus();

    // Loads libdbus and connects. False with *why if libdbus or a function of it is missing, or the bus is not
    // there. The stack need not be there yet: its signals are listened for, and it is waited for.
    bool open(AlpsSerial* sink, std::string* why);
    void close();

    virtual bool setUuid(uint8_t instance, const std::string& payload);
    virtual bool sendData(uint8_t instance, const std::string& bytes);
    virtual bool readConfig(uint8_t instance, uint16_t id);
    virtual bool writeConfig(uint8_t instance, uint16_t id, const std::string& value);
    virtual bool disconnect(uint8_t instance);

    struct Impl;

private:
    DbusAlpsBus(const DbusAlpsBus&);
    DbusAlpsBus& operator=(const DbusAlpsBus&);
    Impl* p_;
};

}  // namespace nbt

#endif

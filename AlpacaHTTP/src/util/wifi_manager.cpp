#include "alpacahttp/wifi_manager.h"

#include <systemd/sd-bus.h>

#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace alpacahttp::util {

namespace {

constexpr const char* kNmService = "org.freedesktop.NetworkManager";
constexpr const char* kNmPath = "/org/freedesktop/NetworkManager";
constexpr const char* kNmIface = "org.freedesktop.NetworkManager";
constexpr const char* kNmDeviceIface = "org.freedesktop.NetworkManager.Device";
constexpr const char* kNmWirelessIface = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* kNmApIface = "org.freedesktop.NetworkManager.AccessPoint";
constexpr const char* kNmSettingsPath = "/org/freedesktop/NetworkManager/Settings";
constexpr const char* kNmSettingsIface = "org.freedesktop.NetworkManager.Settings";
constexpr const char* kNmConnIface = "org.freedesktop.NetworkManager.Settings.Connection";
constexpr const char* kNmActiveIface = "org.freedesktop.NetworkManager.Connection.Active";
constexpr const char* kNmIp4Iface = "org.freedesktop.NetworkManager.IP4Config";

// NMDeviceType
constexpr std::uint32_t kDeviceTypeWifi = 2;
// NMDeviceWifiCapabilities
constexpr std::uint32_t kCapAp = 0x40;
constexpr std::uint32_t kCapFreq2 = 0x200;
constexpr std::uint32_t kCapFreq5 = 0x400;

void throw_bus(const char* what, int r, const sd_bus_error* err = nullptr) {
    std::string msg = std::string(what) + ": " +
                      (err && err->message ? err->message : std::strerror(-r));
    throw WifiError(msg);
}

// A tagged value for building a{sa{sv}} connection settings.
struct SVal {
    char type;  // 's', 'b', 'i', 'u', 'y' = byte-array
    std::string s;
    std::int64_t i = 0;
    bool b = false;
    std::vector<std::uint8_t> bytes;

    static SVal str(std::string v) { SVal x; x.type = 's'; x.s = std::move(v); return x; }
    static SVal boolean(bool v) { SVal x; x.type = 'b'; x.b = v; return x; }
    static SVal i32(std::int32_t v) { SVal x; x.type = 'i'; x.i = v; return x; }
    static SVal u32(std::uint32_t v) { SVal x; x.type = 'u'; x.i = v; return x; }
    static SVal bytearr(const std::string& v) {
        SVal x; x.type = 'y';
        x.bytes.assign(v.begin(), v.end());
        return x;
    }
};
using Section = std::vector<std::pair<std::string, SVal>>;
using SettingsSpec = std::vector<std::pair<std::string, Section>>;

}  // namespace

struct WifiManager::BusHandle {
    sd_bus* bus = nullptr;
    ~BusHandle() {
        if (bus) sd_bus_unref(bus);
    }

    // ---- property helpers -------------------------------------------------

    std::string prop_string(const char* path, const char* iface, const char* prop) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        char* value = nullptr;
        int r = sd_bus_get_property_string(bus, kNmService, path, iface, prop, &err, &value);
        if (r < 0) {
            sd_bus_error_free(&err);
            return {};
        }
        std::string out = value ? value : "";
        free(value);
        return out;
    }

    std::uint64_t prop_trivial(const char* path, const char* iface, const char* prop,
                               char type, std::uint64_t fallback = 0) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        std::uint64_t storage = 0;
        int r = sd_bus_get_property_trivial(bus, kNmService, path, iface, prop, &err, type,
                                            &storage);
        if (r < 0) {
            sd_bus_error_free(&err);
            return fallback;
        }
        // sd-bus writes only sizeof(actual type) bytes; storage was zeroed.
        return storage;
    }

    std::vector<std::string> prop_objpaths(const char* path, const char* iface,
                                           const char* prop) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* m = nullptr;
        std::vector<std::string> out;
        int r = sd_bus_get_property(bus, kNmService, path, iface, prop, &err, &m, "ao");
        if (r < 0) {
            sd_bus_error_free(&err);
            return out;
        }
        r = sd_bus_message_enter_container(m, 'a', "o");
        if (r >= 0) {
            const char* p = nullptr;
            while (sd_bus_message_read(m, "o", &p) > 0) out.emplace_back(p);
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_unref(m);
        return out;
    }

    // Byte-array property (AP Ssid) as a string.
    std::string prop_bytes(const char* path, const char* iface, const char* prop) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* m = nullptr;
        std::string out;
        int r = sd_bus_get_property(bus, kNmService, path, iface, prop, &err, &m, "ay");
        if (r < 0) {
            sd_bus_error_free(&err);
            return out;
        }
        const void* data = nullptr;
        size_t n = 0;
        if (sd_bus_message_read_array(m, 'y', &data, &n) >= 0 && data && n > 0) {
            out.assign(static_cast<const char*>(data), n);
        }
        sd_bus_message_unref(m);
        return out;
    }

    // Call a method with no in-args that returns "ao".
    std::vector<std::string> call_objpaths(const char* path, const char* iface,
                                           const char* method) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* m = nullptr;
        std::vector<std::string> out;
        int r = sd_bus_call_method(bus, kNmService, path, iface, method, &err, &m, "");
        if (r < 0) {
            sd_bus_error_free(&err);
            return out;
        }
        r = sd_bus_message_enter_container(m, 'a', "o");
        if (r >= 0) {
            const char* p = nullptr;
            while (sd_bus_message_read(m, "o", &p) > 0) out.emplace_back(p);
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_unref(m);
        return out;
    }

    // Parse a connection's GetSettings() a{sa{sv}} into json (scalars and
    // ssid byte-arrays only; nested containers we don't need are skipped).
    nlohmann::json get_settings(const std::string& conn_path) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* m = nullptr;
        nlohmann::json out = nlohmann::json::object();
        int r = sd_bus_call_method(bus, kNmService, conn_path.c_str(), kNmConnIface,
                                   "GetSettings", &err, &m, "");
        if (r < 0) {
            std::string msg = err.message ? err.message : std::strerror(-r);
            sd_bus_error_free(&err);
            throw WifiError("GetSettings failed: " + msg);
        }
        r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}");
        if (r >= 0) {
            while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
                const char* section = nullptr;
                sd_bus_message_read(m, "s", &section);
                nlohmann::json sec = nlohmann::json::object();
                if (sd_bus_message_enter_container(m, 'a', "{sv}") >= 0) {
                    while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
                        const char* key = nullptr;
                        sd_bus_message_read(m, "s", &key);
                        const char* contents = nullptr;
                        char type = 0;
                        sd_bus_message_peek_type(m, &type, &contents);
                        if (sd_bus_message_enter_container(m, 'v', contents) >= 0) {
                            std::string ct = contents ? contents : "";
                            if (ct == "s") {
                                const char* v = nullptr;
                                sd_bus_message_read(m, "s", &v);
                                sec[key] = v ? v : "";
                            } else if (ct == "b") {
                                int v = 0;
                                sd_bus_message_read(m, "b", &v);
                                sec[key] = (v != 0);
                            } else if (ct == "i") {
                                std::int32_t v = 0;
                                sd_bus_message_read(m, "i", &v);
                                sec[key] = v;
                            } else if (ct == "u") {
                                std::uint32_t v = 0;
                                sd_bus_message_read(m, "u", &v);
                                sec[key] = v;
                            } else if (ct == "ay") {
                                const void* data = nullptr;
                                size_t n = 0;
                                sd_bus_message_read_array(m, 'y', &data, &n);
                                sec[key] = data && n
                                               ? std::string(static_cast<const char*>(data), n)
                                               : std::string();
                            } else {
                                sd_bus_message_skip(m, ct.c_str());
                            }
                            sd_bus_message_exit_container(m);  // v
                        }
                        sd_bus_message_exit_container(m);  // e
                    }
                    sd_bus_message_exit_container(m);  // a{sv}
                }
                if (section) out[section] = std::move(sec);
                sd_bus_message_exit_container(m);  // e
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_unref(m);
        return out;
    }

    // Append a SettingsSpec as a{sa{sv}} to an open message. pin_shared_ip4:
    // when true, an "ipv4" section with method=shared and address-data
    // 172.24.1.1/24 is appended (the OpenAstro fleet-wide AP subnet).
    static void append_settings(sd_bus_message* m, const SettingsSpec& spec,
                                bool pin_shared_ip4) {
        sd_bus_message_open_container(m, 'a', "{sa{sv}}");
        for (const auto& [section, entries] : spec) {
            sd_bus_message_open_container(m, 'e', "sa{sv}");
            sd_bus_message_append(m, "s", section.c_str());
            sd_bus_message_open_container(m, 'a', "{sv}");
            for (const auto& [key, val] : entries) {
                sd_bus_message_open_container(m, 'e', "sv");
                sd_bus_message_append(m, "s", key.c_str());
                switch (val.type) {
                    case 's':
                        sd_bus_message_open_container(m, 'v', "s");
                        sd_bus_message_append(m, "s", val.s.c_str());
                        break;
                    case 'b':
                        sd_bus_message_open_container(m, 'v', "b");
                        sd_bus_message_append(m, "b", val.b ? 1 : 0);
                        break;
                    case 'i':
                        sd_bus_message_open_container(m, 'v', "i");
                        sd_bus_message_append(m, "i", static_cast<std::int32_t>(val.i));
                        break;
                    case 'u':
                        sd_bus_message_open_container(m, 'v', "u");
                        sd_bus_message_append(m, "u", static_cast<std::uint32_t>(val.i));
                        break;
                    case 'y':
                        sd_bus_message_open_container(m, 'v', "ay");
                        sd_bus_message_append_array(m, 'y', val.bytes.data(), val.bytes.size());
                        break;
                    default:
                        break;
                }
                sd_bus_message_close_container(m);  // v
                sd_bus_message_close_container(m);  // e
            }
            sd_bus_message_close_container(m);  // a{sv}
            sd_bus_message_close_container(m);  // e
        }
        if (pin_shared_ip4) {
            sd_bus_message_open_container(m, 'e', "sa{sv}");
            sd_bus_message_append(m, "s", "ipv4");
            sd_bus_message_open_container(m, 'a', "{sv}");
            // method = shared
            sd_bus_message_open_container(m, 'e', "sv");
            sd_bus_message_append(m, "s", "method");
            sd_bus_message_open_container(m, 'v', "s");
            sd_bus_message_append(m, "s", "shared");
            sd_bus_message_close_container(m);
            sd_bus_message_close_container(m);
            // address-data = [{address: 172.24.1.1, prefix: 24}]
            sd_bus_message_open_container(m, 'e', "sv");
            sd_bus_message_append(m, "s", "address-data");
            sd_bus_message_open_container(m, 'v', "aa{sv}");
            sd_bus_message_open_container(m, 'a', "a{sv}");
            sd_bus_message_open_container(m, 'a', "{sv}");
            sd_bus_message_open_container(m, 'e', "sv");
            sd_bus_message_append(m, "s", "address");
            sd_bus_message_open_container(m, 'v', "s");
            sd_bus_message_append(m, "s", "172.24.1.1");
            sd_bus_message_close_container(m);
            sd_bus_message_close_container(m);
            sd_bus_message_open_container(m, 'e', "sv");
            sd_bus_message_append(m, "s", "prefix");
            sd_bus_message_open_container(m, 'v', "u");
            sd_bus_message_append(m, "u", 24u);
            sd_bus_message_close_container(m);
            sd_bus_message_close_container(m);
            sd_bus_message_close_container(m);  // a{sv}
            sd_bus_message_close_container(m);  // a
            sd_bus_message_close_container(m);  // v
            sd_bus_message_close_container(m);  // e address-data
            sd_bus_message_close_container(m);  // a{sv} of ipv4
            sd_bus_message_close_container(m);  // e ipv4
        }
        sd_bus_message_close_container(m);  // a{sa{sv}}
    }

    void add_connection(const SettingsSpec& spec, bool pin_shared_ip4) {
        sd_bus_message* m = nullptr;
        int r = sd_bus_message_new_method_call(bus, &m, kNmService, kNmSettingsPath,
                                               kNmSettingsIface, "AddConnection");
        if (r < 0) throw_bus("AddConnection new_method_call", r);
        append_settings(m, spec, pin_shared_ip4);
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        r = sd_bus_call(bus, m, 0, &err, &reply);
        sd_bus_message_unref(m);
        if (r < 0) {
            std::string msg = err.message ? err.message : std::strerror(-r);
            sd_bus_error_free(&err);
            throw WifiError("AddConnection failed: " + msg);
        }
        sd_bus_message_unref(reply);
    }

    void update_connection(const std::string& conn_path, const SettingsSpec& spec,
                           bool pin_shared_ip4) {
        sd_bus_message* m = nullptr;
        int r = sd_bus_message_new_method_call(bus, &m, kNmService, conn_path.c_str(),
                                               kNmConnIface, "Update");
        if (r < 0) throw_bus("Update new_method_call", r);
        append_settings(m, spec, pin_shared_ip4);
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        r = sd_bus_call(bus, m, 0, &err, &reply);
        sd_bus_message_unref(m);
        if (r < 0) {
            std::string msg = err.message ? err.message : std::strerror(-r);
            sd_bus_error_free(&err);
            throw WifiError("Update failed: " + msg);
        }
        sd_bus_message_unref(reply);
    }

    void simple_call(const char* path, const char* iface, const char* method) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int r = sd_bus_call_method(bus, kNmService, path, iface, method, &err, &reply, "");
        if (r < 0) {
            std::string msg = err.message ? err.message : std::strerror(-r);
            sd_bus_error_free(&err);
            throw WifiError(std::string(method) + " failed: " + msg);
        }
        sd_bus_message_unref(reply);
    }

    void activate(const std::string& conn_path, const std::string& device_path) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int r = sd_bus_call_method(bus, kNmService, kNmPath, kNmIface, "ActivateConnection",
                                   &err, &reply, "ooo", conn_path.c_str(),
                                   device_path.c_str(), "/");
        if (r < 0) {
            std::string msg = err.message ? err.message : std::strerror(-r);
            sd_bus_error_free(&err);
            throw WifiError("ActivateConnection failed: " + msg);
        }
        sd_bus_message_unref(reply);
    }

    // First wifi device (path, interface-name); empty path if none.
    std::pair<std::string, std::string> wifi_device() {
        for (const auto& dev : call_objpaths(kNmPath, kNmIface, "GetDevices")) {
            auto type = prop_trivial(dev.c_str(), kNmDeviceIface, "DeviceType", 'u');
            if (type == kDeviceTypeWifi) {
                return {dev, prop_string(dev.c_str(), kNmDeviceIface, "Interface")};
            }
        }
        return {"", ""};
    }

    // All saved wifi connections: (path, settings-json).
    std::vector<std::pair<std::string, nlohmann::json>> wifi_connections() {
        std::vector<std::pair<std::string, nlohmann::json>> out;
        for (const auto& c : call_objpaths(kNmSettingsPath, kNmSettingsIface,
                                           "ListConnections")) {
            try {
                auto s = get_settings(c);
                if (s.contains("connection") &&
                    s["connection"].value("type", "") == "802-11-wireless") {
                    out.emplace_back(c, std::move(s));
                }
            } catch (const WifiError&) {
                // Skip connections we cannot read (e.g. permission-scoped).
            }
        }
        return out;
    }

    // Uuid of the active connection on a device ("" if none).
    std::string active_uuid(const std::string& device_path) {
        auto active = prop_string(device_path.c_str(), kNmDeviceIface, "ActiveConnection");
        if (active.empty() || active == "/") return "";
        return prop_string(active.c_str(), kNmActiveIface, "Uuid");
    }

    std::string device_ip4(const std::string& device_path) {
        auto cfg = prop_string(device_path.c_str(), kNmDeviceIface, "Ip4Config");
        if (cfg.empty() || cfg == "/") return "";
        // AddressData is aa{sv}; read the first "address" string.
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* m = nullptr;
        std::string out;
        int r = sd_bus_get_property(bus, kNmService, cfg.c_str(), kNmIp4Iface, "AddressData",
                                    &err, &m, "aa{sv}");
        if (r < 0) {
            sd_bus_error_free(&err);
            return out;
        }
        if (sd_bus_message_enter_container(m, 'a', "a{sv}") >= 0) {
            if (sd_bus_message_enter_container(m, 'a', "{sv}") >= 0) {
                while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
                    const char* key = nullptr;
                    sd_bus_message_read(m, "s", &key);
                    const char* contents = nullptr;
                    char type = 0;
                    sd_bus_message_peek_type(m, &type, &contents);
                    if (sd_bus_message_enter_container(m, 'v', contents) >= 0) {
                        if (key && std::string(key) == "address" &&
                            contents && std::string(contents) == "s") {
                            const char* v = nullptr;
                            sd_bus_message_read(m, "s", &v);
                            if (v) out = v;
                        } else {
                            sd_bus_message_skip(m, contents);
                        }
                        sd_bus_message_exit_container(m);
                    }
                    sd_bus_message_exit_container(m);
                }
                sd_bus_message_exit_container(m);
            }
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_unref(m);
        return out;
    }
};

// ---- nl80211 regulatory domain ---------------------------------------------

namespace {

// Equivalent of `iw reg set <alpha2>` — a single generic-netlink request.
// Needs CAP_NET_ADMIN (granted to the service as an ambient capability).
void nl80211_set_regdom(const std::string& alpha2) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) throw WifiError(std::string("netlink socket: ") + std::strerror(errno));

    struct Guard {
        int fd;
        ~Guard() { close(fd); }
    } guard{fd};

    auto send_and_recv = [&](std::vector<char>& msg, std::vector<char>& reply) {
        if (send(fd, msg.data(), msg.size(), 0) < 0) {
            throw WifiError(std::string("netlink send: ") + std::strerror(errno));
        }
        reply.resize(8192);
        ssize_t n = recv(fd, reply.data(), reply.size(), 0);
        if (n < 0) throw WifiError(std::string("netlink recv: ") + std::strerror(errno));
        reply.resize(static_cast<size_t>(n));
    };

    auto build = [](std::uint16_t nl_type, std::uint8_t genl_cmd,
                    std::uint16_t attr_type, const void* attr_data,
                    std::uint16_t attr_len) {
        size_t attr_space = NLA_HDRLEN + NLA_ALIGN(attr_len);
        size_t total = NLMSG_SPACE(GENL_HDRLEN) + attr_space;
        std::vector<char> buf(total, 0);
        auto* nlh = reinterpret_cast<nlmsghdr*>(buf.data());
        nlh->nlmsg_len = static_cast<std::uint32_t>(total);
        nlh->nlmsg_type = nl_type;
        nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        nlh->nlmsg_seq = 1;
        auto* genl = reinterpret_cast<genlmsghdr*>(NLMSG_DATA(nlh));
        genl->cmd = genl_cmd;
        genl->version = 1;
        auto* attr = reinterpret_cast<nlattr*>(reinterpret_cast<char*>(genl) + GENL_HDRLEN);
        attr->nla_type = attr_type;
        attr->nla_len = static_cast<std::uint16_t>(NLA_HDRLEN + attr_len);
        std::memcpy(reinterpret_cast<char*>(attr) + NLA_HDRLEN, attr_data, attr_len);
        return buf;
    };

    // Resolve the nl80211 family id.
    const char family_name[] = "nl80211";
    auto req = build(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, CTRL_ATTR_FAMILY_NAME, family_name,
                     sizeof(family_name));
    std::vector<char> reply;
    send_and_recv(req, reply);

    std::uint16_t family_id = 0;
    size_t remaining = reply.size();
    for (auto* nlh = reinterpret_cast<nlmsghdr*>(reply.data()); NLMSG_OK(nlh, remaining);
         nlh = NLMSG_NEXT(nlh, remaining)) {
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            auto* e = static_cast<nlmsgerr*>(NLMSG_DATA(nlh));
            if (e->error != 0) {
                throw WifiError(std::string("nl80211 family lookup: ") +
                                std::strerror(-e->error));
            }
            continue;
        }
        auto* genl = static_cast<genlmsghdr*>(NLMSG_DATA(nlh));
        int len = static_cast<int>(nlh->nlmsg_len) - NLMSG_LENGTH(GENL_HDRLEN);
        auto* attr = reinterpret_cast<nlattr*>(reinterpret_cast<char*>(genl) + GENL_HDRLEN);
        while (len > 0 && attr->nla_len >= NLA_HDRLEN &&
               static_cast<int>(attr->nla_len) <= len) {
            if (attr->nla_type == CTRL_ATTR_FAMILY_ID) {
                family_id = *reinterpret_cast<std::uint16_t*>(
                    reinterpret_cast<char*>(attr) + NLA_HDRLEN);
            }
            int step = NLA_ALIGN(attr->nla_len);
            len -= step;
            attr = reinterpret_cast<nlattr*>(reinterpret_cast<char*>(attr) + step);
        }
    }
    if (family_id == 0) throw WifiError("nl80211 family not found (no wireless stack?)");

    // Send REQ_SET_REG with the alpha2 (NUL-terminated).
    char cc[3] = {alpha2[0], alpha2[1], 0};
    auto set_req = build(family_id, NL80211_CMD_REQ_SET_REG, NL80211_ATTR_REG_ALPHA2, cc,
                         sizeof(cc));
    std::vector<char> ack;
    send_and_recv(set_req, ack);
    size_t ack_remaining = ack.size();
    for (auto* nlh = reinterpret_cast<nlmsghdr*>(ack.data()); NLMSG_OK(nlh, ack_remaining);
         nlh = NLMSG_NEXT(nlh, ack_remaining)) {
        if (nlh->nlmsg_type == NLMSG_ERROR) {
            auto* e = static_cast<nlmsgerr*>(NLMSG_DATA(nlh));
            if (e->error != 0) {
                throw WifiError(std::string("REQ_SET_REG failed (needs CAP_NET_ADMIN): ") +
                                std::strerror(-e->error));
            }
        }
    }
}

std::string security_label(std::uint32_t wpa_flags, std::uint32_t rsn_flags) {
    if (rsn_flags != 0) return "WPA2";
    if (wpa_flags != 0) return "WPA";
    return "Open";
}

}  // namespace

// ---- WifiManager ------------------------------------------------------------

WifiManager::WifiManager(std::string state_dir) : state_dir_(std::move(state_dir)) {}

WifiManager::~WifiManager() { delete bus_; }

void WifiManager::ensure_bus_locked() {
    if (bus_ && bus_->bus) return;
    delete bus_;
    bus_ = new BusHandle();
    int r = sd_bus_default_system(&bus_->bus);
    if (r < 0) {
        delete bus_;
        bus_ = nullptr;
        throw WifiError(std::string("cannot connect to system bus: ") + std::strerror(-r));
    }
}

std::string WifiManager::country_file() const { return state_dir_ + "/wifi_country"; }

bool WifiManager::available() {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        ensure_bus_locked();
        return !bus_->wifi_device().first.empty();
    } catch (const WifiError&) {
        return false;
    }
}

nlohmann::json WifiManager::status() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_bus_locked();
    nlohmann::json out;
    auto [dev, ifname] = bus_->wifi_device();
    out["Available"] = !dev.empty();
    if (dev.empty()) return out;

    out["Device"] = ifname;
    out["WirelessEnabled"] =
        bus_->prop_trivial(kNmPath, kNmIface, "WirelessEnabled", 'b') != 0;

    auto caps = static_cast<std::uint32_t>(
        bus_->prop_trivial(dev.c_str(), kNmWirelessIface, "WirelessCapabilities", 'u'));
    out["Capabilities"] = {{"Freq2GHz", (caps & kCapFreq2) != 0},
                           {"Freq5GHz", (caps & kCapFreq5) != 0},
                           {"Ap", (caps & kCapAp) != 0}};

    auto state = bus_->prop_trivial(dev.c_str(), kNmDeviceIface, "State", 'u');
    std::string state_str = state == 100  ? "connected"
                            : state == 30 ? "disconnected"
                            : state < 30  ? "unavailable"
                            : state < 100 ? "connecting"
                                          : "deactivating";
    out["State"] = state_str;

    // Active connection details (client association or our own AP).
    auto active = bus_->prop_string(dev.c_str(), kNmDeviceIface, "ActiveConnection");
    if (!active.empty() && active != "/") {
        out["ConnectionId"] = bus_->prop_string(active.c_str(), kNmActiveIface, "Id");
        out["ConnectionUuid"] = bus_->prop_string(active.c_str(), kNmActiveIface, "Uuid");
    }
    auto ap = bus_->prop_string(dev.c_str(), kNmWirelessIface, "ActiveAccessPoint");
    if (!ap.empty() && ap != "/") {
        out["Ssid"] = bus_->prop_bytes(ap.c_str(), kNmApIface, "Ssid");
        out["FrequencyMhz"] = static_cast<std::uint32_t>(
            bus_->prop_trivial(ap.c_str(), kNmApIface, "Frequency", 'u'));
        out["SignalPercent"] = static_cast<std::uint32_t>(
            bus_->prop_trivial(ap.c_str(), kNmApIface, "Strength", 'y'));
    }
    out["Ip4Address"] = bus_->device_ip4(dev);

    // 5 GHz sanity cross-check: some vendor drivers (bcmdhd) under-report
    // WirelessCapabilities while 5 GHz works — if the last scan saw any BSS
    // above 5000 MHz the band is usable regardless of the flag.
    bool sees5 = false;
    for (const auto& p :
         bus_->call_objpaths(dev.c_str(), kNmWirelessIface, "GetAllAccessPoints")) {
        if (bus_->prop_trivial(p.c_str(), kNmApIface, "Frequency", 'u') > 5000) {
            sees5 = true;
            break;
        }
    }
    out["ScanSees5GHz"] = sees5;

    std::ifstream f(country_file());
    std::string cc;
    if (f && std::getline(f, cc)) out["Country"] = cc;
    else out["Country"] = "";
    return out;
}

void WifiManager::set_wireless_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_bus_locked();
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_set_property(bus_->bus, kNmService, kNmPath, kNmIface, "WirelessEnabled",
                                &err, "b", enabled ? 1 : 0);
    if (r < 0) {
        std::string msg = err.message ? err.message : std::strerror(-r);
        sd_bus_error_free(&err);
        throw WifiError("set WirelessEnabled failed: " + msg);
    }
}

nlohmann::json WifiManager::scan() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_bus_locked();
    auto [dev, ifname] = bus_->wifi_device();
    if (dev.empty()) throw WifiError("no wifi device");

    // Best-effort scan request; NM throttles ("Scanning not allowed...") and
    // refuses while the AP is up — in both cases the cached BSS list below is
    // still the freshest data available, so ignore the error.
    {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int r = sd_bus_call_method(bus_->bus, kNmService, dev.c_str(), kNmWirelessIface,
                                   "RequestScan", &err, &reply, "a{sv}", 0);
        if (r >= 0) {
            sd_bus_message_unref(reply);
            // Give the driver a moment; NM has no synchronous scan API.
            struct timespec ts {1, 500000000};  // 1.5 s
            nanosleep(&ts, nullptr);
        }
        sd_bus_error_free(&err);
    }

    // Deduplicate by SSID keeping the strongest signal.
    std::map<std::string, nlohmann::json> best;
    for (const auto& p :
         bus_->call_objpaths(dev.c_str(), kNmWirelessIface, "GetAllAccessPoints")) {
        auto ssid = bus_->prop_bytes(p.c_str(), kNmApIface, "Ssid");
        if (ssid.empty()) continue;  // hidden
        auto freq = static_cast<std::uint32_t>(
            bus_->prop_trivial(p.c_str(), kNmApIface, "Frequency", 'u'));
        auto strength = static_cast<std::uint32_t>(
            bus_->prop_trivial(p.c_str(), kNmApIface, "Strength", 'y'));
        auto wpa = static_cast<std::uint32_t>(
            bus_->prop_trivial(p.c_str(), kNmApIface, "WpaFlags", 'u'));
        auto rsn = static_cast<std::uint32_t>(
            bus_->prop_trivial(p.c_str(), kNmApIface, "RsnFlags", 'u'));
        auto it = best.find(ssid);
        if (it == best.end() || it->second["SignalPercent"].get<std::uint32_t>() < strength) {
            best[ssid] = {{"Ssid", ssid},
                          {"FrequencyMhz", freq},
                          {"SignalPercent", strength},
                          {"Security", security_label(wpa, rsn)}};
        }
    }
    std::vector<nlohmann::json> list;
    list.reserve(best.size());
    for (auto& [k, v] : best) list.push_back(std::move(v));
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return a["SignalPercent"].template get<std::uint32_t>() >
               b["SignalPercent"].template get<std::uint32_t>();
    });
    return nlohmann::json(list);
}

nlohmann::json WifiManager::profiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_bus_locked();
    auto [dev, ifname] = bus_->wifi_device();
    std::string active = dev.empty() ? "" : bus_->active_uuid(dev);

    nlohmann::json list = nlohmann::json::array();
    for (const auto& [path, s] : bus_->wifi_connections()) {
        const auto& conn = s["connection"];
        std::string mode = "infrastructure";
        std::string ssid;
        if (s.contains("802-11-wireless")) {
            mode = s["802-11-wireless"].value("mode", "infrastructure");
            ssid = s["802-11-wireless"].value("ssid", "");
        }
        std::string uuid = conn.value("uuid", "");
        list.push_back({{"Id", conn.value("id", "")},
                        {"Uuid", uuid},
                        {"Ssid", ssid},
                        {"Mode", mode},
                        {"Autoconnect", conn.value("autoconnect", true)},
                        {"Priority", conn.value("autoconnect-priority", 0)},
                        {"Active", !active.empty() && uuid == active}});
    }
    return list;
}

nlohmann::json WifiManager::save_profile(const std::string& ssid,
                                         const std::string& passphrase, bool autoconnect,
                                         int priority) {
    if (ssid.empty() || ssid.size() > 32) throw WifiError("Ssid must be 1-32 bytes");
    if (!passphrase.empty() && (passphrase.size() < 8 || passphrase.size() > 63)) {
        throw WifiError("Passphrase must be 8-63 characters (or empty for open networks)");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_bus_locked();

    // Existing client profile for this SSID → update in place.
    std::string existing_path;
    nlohmann::json existing;
    for (const auto& [path, s] : bus_->wifi_connections()) {
        if (s.contains("802-11-wireless") &&
            s["802-11-wireless"].value("ssid", "") == ssid &&
            s["802-11-wireless"].value("mode", "infrastructure") == "infrastructure") {
            existing_path = path;
            existing = s;
            break;
        }
    }

    Section conn{{"id", SVal::str(ssid)},
                 {"type", SVal::str("802-11-wireless")},
                 {"autoconnect", SVal::boolean(autoconnect)},
                 {"autoconnect-priority", SVal::i32(priority)}};
    if (!existing_path.empty()) {
        conn.push_back({"uuid", SVal::str(existing["connection"].value("uuid", ""))});
    }
    Section wifi{{"ssid", SVal::bytearr(ssid)}, {"mode", SVal::str("infrastructure")}};
    SettingsSpec spec{{"connection", conn}, {"802-11-wireless", wifi}};

    bool had_security =
        !existing.empty() && existing.contains("802-11-wireless-security");
    if (!passphrase.empty()) {
        spec.push_back({"802-11-wireless-security",
                        {{"key-mgmt", SVal::str("wpa-psk")}, {"psk", SVal::str(passphrase)}}});
    } else if (had_security) {
        // Keep WPA with the stored secret: declare key-mgmt but omit psk —
        // NM retains secrets that are not part of an Update payload.
        spec.push_back({"802-11-wireless-security", {{"key-mgmt", SVal::str("wpa-psk")}}});
    }

    if (existing_path.empty()) {
        if (passphrase.empty() && !had_security) {
            // New open-network profile: nothing to add — spec as built.
        }
        bus_->add_connection(spec, /*pin_shared_ip4=*/false);
    } else {
        bus_->update_connection(existing_path, spec, /*pin_shared_ip4=*/false);
    }

    return {{"Id", ssid}, {"Ssid", ssid}, {"Autoconnect", autoconnect},
            {"Priority", priority}};
}

void WifiManager::delete_profile(const std::string& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_bus_locked();
    for (const auto& [path, s] : bus_->wifi_connections()) {
        if (s["connection"].value("uuid", "") == uuid) {
            if (s["connection"].value("id", "") == kApProfileId) {
                throw WifiError("refusing to delete the hotspot profile; use the AP settings");
            }
            bus_->simple_call(path.c_str(), kNmConnIface, "Delete");
            return;
        }
    }
    throw WifiError("no profile with that Uuid");
}

void WifiManager::connect_profile(const std::string& uuid) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_bus_locked();
    auto [dev, ifname] = bus_->wifi_device();
    if (dev.empty()) throw WifiError("no wifi device");
    for (const auto& [path, s] : bus_->wifi_connections()) {
        if (s["connection"].value("uuid", "") == uuid) {
            bus_->activate(path, dev);
            return;
        }
    }
    throw WifiError("no profile with that Uuid");
}

nlohmann::json WifiManager::get_ap() {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_bus_locked();
    auto [dev, ifname] = bus_->wifi_device();
    std::string active = dev.empty() ? "" : bus_->active_uuid(dev);

    for (const auto& [path, s] : bus_->wifi_connections()) {
        if (!s.contains("802-11-wireless")) continue;
        if (s["802-11-wireless"].value("mode", "") != "ap") continue;
        const auto& wifi = s["802-11-wireless"];
        std::string uuid = s["connection"].value("uuid", "");
        return {{"Configured", true},
                {"Ssid", wifi.value("ssid", "")},
                {"Band", wifi.value("band", "")},
                {"Channel", wifi.value("channel", 0u)},
                {"Autoconnect", s["connection"].value("autoconnect", true)},
                {"Active", !active.empty() && uuid == active},
                {"Ip4Address", "172.24.1.1"}};
    }
    return {{"Configured", false}};
}

nlohmann::json WifiManager::set_ap(const std::string& ssid, const std::string& passphrase,
                                   const std::string& band, std::uint32_t channel,
                                   bool enabled) {
    if (ssid.empty() || ssid.size() > 32) throw WifiError("Ssid must be 1-32 bytes");
    if (!passphrase.empty() && (passphrase.size() < 8 || passphrase.size() > 63)) {
        throw WifiError("Passphrase must be 8-63 characters");
    }
    if (band != "a" && band != "bg") throw WifiError("Band must be \"a\" (5 GHz) or \"bg\" (2.4 GHz)");
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_bus_locked();
    auto [dev, ifname] = bus_->wifi_device();
    if (dev.empty()) throw WifiError("no wifi device");

    std::string existing_path;
    nlohmann::json existing;
    for (const auto& [path, s] : bus_->wifi_connections()) {
        if (s.contains("802-11-wireless") && s["802-11-wireless"].value("mode", "") == "ap") {
            existing_path = path;
            existing = s;
            break;
        }
    }

    Section conn{{"id", SVal::str(kApProfileId)},
                 {"type", SVal::str("802-11-wireless")},
                 {"autoconnect", SVal::boolean(enabled)}};
    if (!existing_path.empty()) {
        conn.push_back({"uuid", SVal::str(existing["connection"].value("uuid", ""))});
    }
    Section wifi{{"ssid", SVal::bytearr(ssid)},
                 {"mode", SVal::str("ap")},
                 {"band", SVal::str(band)}};
    if (channel != 0) wifi.push_back({"channel", SVal::u32(channel)});
    SettingsSpec spec{{"connection", conn}, {"802-11-wireless", wifi}};
    if (!passphrase.empty()) {
        spec.push_back({"802-11-wireless-security",
                        {{"key-mgmt", SVal::str("wpa-psk")}, {"psk", SVal::str(passphrase)}}});
    } else if (!existing.empty() && existing.contains("802-11-wireless-security")) {
        spec.push_back({"802-11-wireless-security", {{"key-mgmt", SVal::str("wpa-psk")}}});
    }

    if (existing_path.empty()) {
        bus_->add_connection(spec, /*pin_shared_ip4=*/true);
    } else {
        bus_->update_connection(existing_path, spec, /*pin_shared_ip4=*/true);
    }

    // Re-resolve (AddConnection does not return the path through our helper)
    // and apply the enable state immediately.
    for (const auto& [path, s] : bus_->wifi_connections()) {
        if (s.contains("802-11-wireless") && s["802-11-wireless"].value("mode", "") == "ap") {
            if (enabled) {
                bus_->activate(path, dev);
            } else {
                auto active = bus_->prop_string(dev.c_str(), kNmDeviceIface,
                                                "ActiveConnection");
                if (!active.empty() && active != "/" &&
                    bus_->prop_string(active.c_str(), kNmActiveIface, "Id") ==
                        kApProfileId) {
                    sd_bus_error err = SD_BUS_ERROR_NULL;
                    sd_bus_message* reply = nullptr;
                    int r = sd_bus_call_method(bus_->bus, kNmService, kNmPath, kNmIface,
                                               "DeactivateConnection", &err, &reply, "o",
                                               active.c_str());
                    if (r >= 0) sd_bus_message_unref(reply);
                    sd_bus_error_free(&err);
                }
            }
            break;
        }
    }

    return {{"Ssid", ssid}, {"Band", band}, {"Channel", channel}, {"Enabled", enabled}};
}

nlohmann::json WifiManager::get_country() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream f(country_file());
    std::string cc;
    if (f) std::getline(f, cc);
    return {{"Alpha2", cc}};
}

void WifiManager::set_country(const std::string& alpha2) {
    if (alpha2.size() != 2 || !std::isupper(static_cast<unsigned char>(alpha2[0])) ||
        !std::isupper(static_cast<unsigned char>(alpha2[1]))) {
        throw WifiError("Alpha2 must be a two-letter uppercase ISO country code");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    nl80211_set_regdom(alpha2);
    std::error_code ec;
    std::filesystem::create_directories(state_dir_, ec);
    std::ofstream f(country_file(), std::ios::trunc);
    if (!f) throw WifiError("country applied but could not persist to " + country_file());
    f << alpha2 << "\n";
}

void WifiManager::apply_persisted_country() {
    try {
        std::string cc;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::ifstream f(country_file());
            if (!f || !std::getline(f, cc) || cc.size() != 2) return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        nl80211_set_regdom(cc);
    } catch (...) {
        // Best-effort at startup; the user can re-apply from the UI.
    }
}

}  // namespace alpacahttp::util

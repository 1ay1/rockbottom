// platform/linux/nl80211.hpp — the wifi link as `iw dev <if> link` reports it.
//
// /proc/net/wireless gives only the signal. The network name, frequency and
// link speed live behind nl80211, the kernel's wifi netlink family. Two
// requests on one generic-netlink socket: GET_INTERFACE (ssid, frequency)
// and a GET_STATION dump (signal, tx/rx bitrate of the access point). No
// fork, no library. Fails quietly: a sandbox that refuses netlink (Android's
// shell) just leaves the fields empty.

#pragma once

#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rockbottom::nl80211 {

struct Link {
    std::string ssid;
    int freq_mhz = 0, signal_dbm = 0, tx_mbps = 0, rx_mbps = 0;
};

namespace detail {

class Sock {
public:
    Sock() : fd_(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC)) {
        if (fd_ < 0) return;
        timeval tv{0, 300000};   // never hang the sampler
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    ~Sock() { if (fd_ >= 0) ::close(fd_); }
    Sock(const Sock&) = delete;
    Sock& operator=(const Sock&) = delete;
    [[nodiscard]] bool ok() const { return fd_ >= 0; }

    // Send one generic-netlink request; attrs are pre-encoded.
    bool send(std::uint16_t family, std::uint8_t cmd, std::uint16_t flags,
              const std::vector<std::uint8_t>& attrs) {
        std::vector<std::uint8_t> m(NLMSG_HDRLEN + GENL_HDRLEN + attrs.size());
        auto* nh = reinterpret_cast<nlmsghdr*>(m.data());
        nh->nlmsg_len = static_cast<std::uint32_t>(m.size());
        nh->nlmsg_type = family;
        nh->nlmsg_flags = static_cast<std::uint16_t>(NLM_F_REQUEST | flags);
        nh->nlmsg_seq = ++seq_;
        auto* gh = reinterpret_cast<genlmsghdr*>(m.data() + NLMSG_HDRLEN);
        gh->cmd = cmd;
        gh->version = 1;
        if (!attrs.empty()) std::memcpy(m.data() + NLMSG_HDRLEN + GENL_HDRLEN, attrs.data(), attrs.size());
        sockaddr_nl to{};
        to.nl_family = AF_NETLINK;
        return ::sendto(fd_, m.data(), m.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof to) ==
               static_cast<ssize_t>(m.size());
    }

    // Read replies until DONE/ERROR (or the one reply of a non-dump); call
    // `each` with every message's attribute block.
    template <class F>
    bool recv(bool dump, F&& each) {
        std::vector<std::uint8_t> buf(32768);
        for (;;) {
            const ssize_t n = ::recv(fd_, buf.data(), buf.size(), 0);
            if (n <= 0) return false;
            int len = static_cast<int>(n);
            for (auto* nh = reinterpret_cast<nlmsghdr*>(buf.data()); NLMSG_OK(nh, len);
                 nh = NLMSG_NEXT(nh, len)) {
                if (nh->nlmsg_type == NLMSG_DONE) return true;
                if (nh->nlmsg_type == NLMSG_ERROR) {
                    const auto* e = static_cast<const nlmsgerr*>(NLMSG_DATA(nh));
                    return e->error == 0;
                }
                const auto* p = static_cast<const std::uint8_t*>(NLMSG_DATA(nh)) + GENL_HDRLEN;
                const int alen = static_cast<int>(nh->nlmsg_len) - NLMSG_HDRLEN - GENL_HDRLEN;
                if (alen > 0) each(p, alen);
                if (!dump) return true;
            }
        }
    }

private:
    int fd_;
    std::uint32_t seq_ = 0;
};

inline void put_attr(std::vector<std::uint8_t>& v, std::uint16_t type, const void* data, std::size_t len) {
    nlattr a{};
    a.nla_len = static_cast<std::uint16_t>(NLA_HDRLEN + len);
    a.nla_type = type;
    const std::size_t at = v.size();
    v.resize(at + NLA_ALIGN(a.nla_len));
    std::memcpy(v.data() + at, &a, sizeof a);
    std::memcpy(v.data() + at + NLA_HDRLEN, data, len);
}

// Walk attributes, calling f(type, payload, len).
template <class F>
void each_attr(const std::uint8_t* p, int len, F&& f) {
    while (len >= static_cast<int>(NLA_HDRLEN)) {
        nlattr a{};
        std::memcpy(&a, p, sizeof a);
        if (a.nla_len < NLA_HDRLEN || a.nla_len > len) return;
        f(static_cast<std::uint16_t>(a.nla_type & NLA_TYPE_MASK), p + NLA_HDRLEN,
          static_cast<int>(a.nla_len - NLA_HDRLEN));
        const int step = static_cast<int>(NLA_ALIGN(a.nla_len));
        p += step;
        len -= step;
    }
}

template <class T>
T get(const std::uint8_t* p, int len) {
    T v{};
    if (len >= static_cast<int>(sizeof v)) std::memcpy(&v, p, sizeof v);
    return v;
}

// A bitrate nest, in Mbps (the kernel's unit is 100 kbit/s).
inline int bitrate_mbps(const std::uint8_t* p, int len) {
    std::uint32_t r32 = 0, r16 = 0;
    each_attr(p, len, [&](std::uint16_t t, const std::uint8_t* d, int l) {
        if (t == NL80211_RATE_INFO_BITRATE32) r32 = get<std::uint32_t>(d, l);
        else if (t == NL80211_RATE_INFO_BITRATE) r16 = get<std::uint16_t>(d, l);
    });
    return static_cast<int>((r32 ? r32 : r16) / 10);
}

}  // namespace detail

// The interface's current link, or false when nl80211 is unreachable or the
// interface isn't associated.
inline bool link(const std::string& iface, Link& out) {
    using namespace detail;
    const unsigned idx = ::if_nametoindex(iface.c_str());
    if (!idx) return false;
    Sock s;
    if (!s.ok()) return false;

    // resolve the nl80211 family id
    std::vector<std::uint8_t> a;
    put_attr(a, CTRL_ATTR_FAMILY_NAME, "nl80211", 8);
    if (!s.send(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 0, a)) return false;
    std::uint16_t fam = 0;
    s.recv(false, [&](const std::uint8_t* p, int len) {
        each_attr(p, len, [&](std::uint16_t t, const std::uint8_t* d, int l) {
            if (t == CTRL_ATTR_FAMILY_ID) fam = get<std::uint16_t>(d, l);
        });
    });
    if (!fam) return false;

    const std::uint32_t ifx = idx;
    a.clear();
    put_attr(a, NL80211_ATTR_IFINDEX, &ifx, sizeof ifx);

    // ssid and frequency
    if (s.send(fam, NL80211_CMD_GET_INTERFACE, 0, a))
        s.recv(false, [&](const std::uint8_t* p, int len) {
            each_attr(p, len, [&](std::uint16_t t, const std::uint8_t* d, int l) {
                if (t == NL80211_ATTR_SSID && l > 0 && l <= 32)
                    out.ssid.assign(reinterpret_cast<const char*>(d), static_cast<std::size_t>(l));
                else if (t == NL80211_ATTR_WIPHY_FREQ)
                    out.freq_mhz = static_cast<int>(get<std::uint32_t>(d, l));
            });
        });

    // the access point as a station: signal and bitrates
    if (s.send(fam, NL80211_CMD_GET_STATION, NLM_F_DUMP, a))
        s.recv(true, [&](const std::uint8_t* p, int len) {
            each_attr(p, len, [&](std::uint16_t t, const std::uint8_t* d, int l) {
                if (t != NL80211_ATTR_STA_INFO) return;
                each_attr(d, l, [&](std::uint16_t st, const std::uint8_t* sd, int sl) {
                    if (st == NL80211_STA_INFO_SIGNAL) out.signal_dbm = get<std::int8_t>(sd, sl);
                    else if (st == NL80211_STA_INFO_TX_BITRATE) out.tx_mbps = bitrate_mbps(sd, sl);
                    else if (st == NL80211_STA_INFO_RX_BITRATE) out.rx_mbps = bitrate_mbps(sd, sl);
                });
            });
        });
    // Newer kernels leave the ssid out of GET_INTERFACE; `iw link` takes it
    // from the scan result marked associated, in its SSID element (id 0).
    if (out.ssid.empty() && s.send(fam, NL80211_CMD_GET_SCAN, NLM_F_DUMP, a))
        s.recv(true, [&](const std::uint8_t* p, int len) {
            each_attr(p, len, [&](std::uint16_t t, const std::uint8_t* d, int l) {
                if (t != NL80211_ATTR_BSS) return;
                bool assoc = false;
                const std::uint8_t* ies = nullptr;
                int ies_len = 0;
                each_attr(d, l, [&](std::uint16_t bt, const std::uint8_t* bd, int bl) {
                    if (bt == NL80211_BSS_STATUS)
                        assoc = get<std::uint32_t>(bd, bl) == NL80211_BSS_STATUS_ASSOCIATED;
                    else if (bt == NL80211_BSS_INFORMATION_ELEMENTS) { ies = bd; ies_len = bl; }
                });
                if (!assoc || !ies) return;
                for (int i = 0; i + 2 <= ies_len;) {
                    const int id = ies[i], el = ies[i + 1];
                    if (i + 2 + el > ies_len) break;
                    if (id == 0 && el > 0 && el <= 32) {
                        out.ssid.assign(reinterpret_cast<const char*>(ies + i + 2),
                                        static_cast<std::size_t>(el));
                        break;
                    }
                    i += 2 + el;
                }
            });
        });
    return !out.ssid.empty() || out.signal_dbm != 0;
}

}  // namespace rockbottom::nl80211

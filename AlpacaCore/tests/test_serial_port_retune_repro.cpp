// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// Regression cases for open-astro#1202: a serial port that a connected driver
// held could be opened and retuned (baud, VMIN, VTIME) by a second driver, and
// a retune to a blocking read (VMIN=1) parked the rotator's move monitor in
// read(), so Disconnect never returned. Both cases assert the fixed behaviour
// (util::SerialLink: registry claim, non-blocking poll()-bounded reads).

#include <alpacacore/vendor/gemini/gemini_protocol_wrapper.h>
#include <alpacacore/vendor/wandererastro/wandererastro_rotator_protocol_wrapper.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

#include "catch2_compat.h"
#include "fake_pty_write.h"

namespace {

namespace gemini = alpacacore::vendor::gemini;
namespace wanderer = alpacacore::vendor::wandererastro;

// One pty that answers as a WandererRotator Mini (bare 7-digit commands) and
// as a Gemini focuser (":NN#" commands), so either wrapper can connect to it.
// Move commands get no reply: the rotator monitor stays in read().
class DualProtocolPty {
public:
    DualProtocolPty() : pty_("DualProtocolPty") {
        th_ = std::thread([this] { run(); });
    }
    ~DualProtocolPty() {
        stop_ = true;
        th_.join();
    }
    std::string slave_path() const { return pty_.slave_path(); }
    int master_fd() const { return pty_.master_fd(); }
    int keepalive_fd() const { return pty_.keepalive_fd(); }

private:
    void reply(const std::string& text) {
        alpacacore::test::pty_write_bounded(pty_.master_fd(), text.data(), text.size(), stop_);
    }
    void run() {
        std::string acc;
        while (!stop_) {
            pollfd p{pty_.master_fd(), POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) {
                continue;
            }
            char buf[64];
            const ssize_t n = ::read(pty_.master_fd(), buf, sizeof(buf));
            for (ssize_t i = 0; i < n; ++i) {
                acc += buf[i];
            }
            for (;;) {
                if (acc.rfind("1500001", 0) == 0) {
                    acc.erase(0, 7);
                    reply("WandererRotatorMiniA20250101A0A0A0A");
                } else if (acc.rfind(":03#", 0) == 0) {
                    acc.erase(0, 4);
                    reply("F123#");
                } else if (!acc.empty() && acc[0] == ':' && acc.find('#') != std::string::npos) {
                    acc.erase(0, acc.find('#') + 1);  // other Gemini command: no reply
                } else if (!acc.empty() && acc[0] != ':' && acc.rfind("1500001", 0) != 0 && acc.size() >= 7) {
                    acc.clear();  // rotator move/stop: no reply
                } else {
                    break;
                }
            }
        }
    }
    alpacacore::test::PtyPair pty_;
    std::thread th_;
    std::atomic<bool> stop_{false};
};

struct Tuning {
    speed_t baud = 0;
    cc_t vmin = 0;
    cc_t vtime = 0;
};

Tuning read_tuning(int fd) {
    struct termios tty {};
    REQUIRE(tcgetattr(fd, &tty) == 0);
    return {cfgetospeed(&tty), tty.c_cc[VMIN], tty.c_cc[VTIME]};
}

std::string describe(const Tuning& t) {
    return "baud=" + std::to_string(t.baud == B19200 ? 19200 : (t.baud == B9600 ? 9600 : -1)) +
           " VMIN=" + std::to_string(t.vmin) + " VTIME=" + std::to_string(t.vtime);
}

}  // namespace

TEST_CASE("Serial port retune - second driver cannot retune a connected rotator's port",
          "[serial][wandererastro][gemini][issue1202]") {
    DualProtocolPty fake;
    wanderer::WandererRotatorProtocolWrapper rotator;
    wanderer::RotatorConnectionConfig rcfg;
    rcfg.serial_port = fake.slave_path();
    REQUIRE(rotator.connect(rcfg) == "WandererRotatorMini");

    const Tuning before = read_tuning(fake.keepalive_fd());
    WARN("rotator link before the second open: " << describe(before));
    REQUIRE(before.baud == B19200);
    // SerialLink keeps the fd non-blocking and zeroes VMIN/VTIME; reads are
    // bounded by poll(), not by VTIME.
    REQUIRE(before.vmin == 0);
    REQUIRE(before.vtime == 0);

    bool second_connect_succeeded = false;
    std::string refusal;
    gemini::GeminiProtocolWrapper focuser;
    gemini::ConnectionConfig gcfg;  // default 9600 baud, VTIME 10
    gcfg.serial_port = fake.slave_path();
    try {
        focuser.connect(gcfg);
        second_connect_succeeded = true;
    } catch (const std::exception& e) {
        refusal = e.what();
        WARN("second driver connect threw: " << e.what());
    }
    const Tuning after = read_tuning(fake.keepalive_fd());
    WARN("second driver open succeeded: " << (second_connect_succeeded ? "yes" : "no"));
    WARN("rotator link after the second open: " << describe(after));
    focuser.disconnect();

    // Fixed behaviour: the second driver is refused and the rotator's link is untouched.
    CHECK_FALSE(second_connect_succeeded);
    // The operator learns which device holds the port (the registry claim), not
    // only that the open was busy (TIOCEXCL).
    CHECK(refusal.find("WandererRotator") != std::string::npos);
    CHECK(after.baud == before.baud);
    CHECK(after.vmin == before.vmin);
    CHECK(after.vtime == before.vtime);
    rotator.disconnect();
}

TEST_CASE("Serial port retune - rotator disconnect returns after the link is retuned to VMIN=1",
          "[serial][wandererastro][issue1202]") {
    DualProtocolPty fake;
    wanderer::WandererRotatorProtocolWrapper rotator;
    wanderer::RotatorConnectionConfig rcfg;
    rcfg.serial_port = fake.slave_path();
    REQUIRE(rotator.connect(rcfg) == "WandererRotatorMini");

    // 30 degrees: the monitor budget is about 19 s, so its own deadline cannot end it.
    rotator.move_relative(30.0);
    REQUIRE(rotator.get_state().moving);

    struct termios tty {};
    REQUIRE(tcgetattr(fake.keepalive_fd(), &tty) == 0);
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    REQUIRE(tcsetattr(fake.keepalive_fd(), TCSANOW, &tty) == 0);
    // Outlast the 0.5 s VTIME window so the monitor's next read() runs under VMIN=1.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // The wrapper's disconnect() is the synchronous one: it joins the monitor.
    std::promise<void> done;
    auto done_future = done.get_future();
    const auto start = std::chrono::steady_clock::now();
    std::thread disconnecter([&] {
        rotator.disconnect();
        done.set_value();
    });
    const bool returned = done_future.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    WARN("disconnect returned within 3 s: " << (returned ? "yes" : "no") << " (waited " << elapsed_ms << " ms)");
    if (!returned) {
        // Release the parked read so the thread (and the wrapper destructor) can finish.
        const char release = 'A';
        REQUIRE(::write(fake.master_fd(), &release, 1) == 1);
    }
    disconnecter.join();

    CHECK(returned);
}

// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// Test-only server for the hardware-free ConformU job (scripts/run_fake_conformu.sh, issue #2078).
// It serves one device: the row of scripts/conformu_fake_runs.tsv named on its command line, through a test
// catalog descriptor ("<vendor>-fake") whose factory builds the real driver over that row's fake. The hosts are
// the table below; a row the table does not know is refused, so the run list cannot ask for a host this server
// cannot build. The run script registers device 0 with POST /management/v1/configuredevice (the router's own
// path). Not installed or packaged, and not linked into alpacabridge or alpacahttp_server. A ConformU run against
// it is fake evidence only: it says nothing about hardware.
//
// The descriptor reaches the catalog through Server::router_for_test(), the same seam the catalog tests use.
// Everything the Schema borrows has static storage (schema.h): the display name, the build option and the vendor
// key are string literals in the host table, and the field span is empty.
//
//   fake_conformu_server <http-port> <vendor> <deviceType> <fakeHeader>
//   fake_conformu_server --describe <vendor> <deviceType> <fakeHeader>
//
// --describe prints the fake's description, which the run script puts in the report's FAKE REPORT line.

#include <alpacacore/alpaca_defs.h>
#include <alpacacore/catalog/device_catalog.h>
#include <alpacahttp/config.h>
#include <alpacahttp/server.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "conformu_camera_factories.h"
#include "conformu_covercalibrator_factories.h"
#include "conformu_filterwheel_factories.h"
#include "conformu_focuser_factories.h"
#include "conformu_rotator_factories.h"
#ifdef FAKE_CONFORMU_SKYWATCHER
#include "conformu_skywatcher_factory.h"
#endif

namespace {

namespace catalog = alpacacore::catalog;

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

// One ConformU host: a run-list row (vendor, deviceType, fakeHeader), the descriptor it registers and how to build
// its driver over the fake. `ready` reports whether the fake came up (a bound port, an open pty).
struct ConformuHost {
    std::string_view vendor;
    std::string_view device_type;
    std::string_view fake_header;
    alpacacore::DeviceType type;
    const char* descriptor_vendor;  // the catalog vendor key the run script registers
    const char* display_name;
    const char* build_option;
    const char* description;  // the FAKE REPORT line's "ConformU against ..." text
    std::function<bool()> ready;
    std::function<std::unique_ptr<alpacacore::AlpacaDriver>(int device_number)> make;
};

// Static storage: the fake outlives every driver the factory builds. Each fake is built only when its row is served.
#ifdef FAKE_CONFORMU_SKYWATCHER
alpacacore::test::FakeSkyWatcherMount& skywatcher_mount() {
    static alpacacore::test::FakeSkyWatcherMount mount;
    return mount;
}

alpacacore::test::FakeSkyWatcherSerialBoard& skywatcher_serial_board() {
    static alpacacore::test::FakeSkyWatcherSerialBoard board;
    return board;
}
#endif

namespace cu = alpacacore::test::conformu;
// A host's factory: a typed factory of conformu_<type>_factories.h, as the generic driver the descriptor builds.
template <typename Make>
std::function<std::unique_ptr<alpacacore::AlpacaDriver>(int)> driver_of(Make make) {
    return [make](int device_number) -> std::unique_ptr<alpacacore::AlpacaDriver> { return make(device_number); };
}

// Each host is compiled only with its vendor (the FAKE_CONFORMU_<VENDOR> macros, AlpacaHTTP/CMakeLists.txt).
const std::vector<ConformuHost> kHosts = {
#ifdef FAKE_CONFORMU_SKYWATCHER
    {"skywatcher", "telescope", "fake_skywatcher_mount.h", alpacacore::DeviceType::Telescope, "skywatcher-fake",
     "Sky-Watcher (fake mount, test only)", "ALPACACORE_ENABLE_SKYWATCHER",
     "FakeSkyWatcherMount (simulated mount over loopback UDP)", [] { return skywatcher_mount().ok(); },
     [](int device_number) -> std::unique_ptr<alpacacore::AlpacaDriver> {
         return alpacacore::test::make_fake_skywatcher_telescope(device_number, skywatcher_mount());
     }},
    {"skywatcher", "telescope", "fake_skywatcher_serial_board.h", alpacacore::DeviceType::Telescope, "skywatcher-fake",
     "Sky-Watcher (fake serial motor board, test only)", "ALPACACORE_ENABLE_SKYWATCHER",
     "FakeSkyWatcherSerialBoard (simulated motor board over a pseudo-terminal)",
     [] { return !skywatcher_serial_board().slave_path().empty(); },
     [](int device_number) -> std::unique_ptr<alpacacore::AlpacaDriver> {
         return alpacacore::test::make_fake_skywatcher_serial_telescope(device_number, skywatcher_serial_board());
     }},
#endif
#ifdef FAKE_CONFORMU_ZWO
    {"zwo", "focuser", "fake_zwo_eaf_sdk.h", alpacacore::DeviceType::Focuser, "zwo-fake",
     "ZWO EAF (fake SDK, test only)", "ALPACACORE_ENABLE_ZWO", "FakeZWOEAFSDK (simulated EAF SDK, in process)",
     [] { return true; }, driver_of(cu::make_zwo_focuser)},
#endif
#ifdef FAKE_CONFORMU_CELESTRON
    {"celestron", "focuser", "fake_celestron_focuser.h", alpacacore::DeviceType::Focuser, "celestron-fake",
     "Celestron focus motor (fake AUX bus, test only)", "ALPACACORE_ENABLE_CELESTRON",
     "FakeCelestronFocuser (simulated focus motor over a pseudo-terminal)",
     [] { return !cu::celestron_focuser().slave_path().empty(); }, driver_of(cu::make_celestron_focuser)},
#endif
#ifdef FAKE_CONFORMU_ONSTEP
    {"onstep", "focuser", "fake_mount_server.h", alpacacore::DeviceType::Focuser, "onstep-fake",
     "OnStepX focuser (fake controller, test only)", "ALPACACORE_ENABLE_ONSTEP",
     "the OnStepX controller model of onstep_controller_model.h (focuser 1, over loopback TCP)",
     [] { return cu::onstep_controller_host().server.ok(); }, driver_of(cu::make_onstep_focuser)},
#endif
#ifdef FAKE_CONFORMU_IOPTRON
    {"ioptron", "focuser", "fake_ioptron_ieaf.h", alpacacore::DeviceType::Focuser, "ioptron-fake",
     "iOptron iEAF (fake, test only)", "ALPACACORE_ENABLE_IOPTRON",
     "FakeIoptronIeaf (simulated iEAF over a pseudo-terminal)", [] { return !cu::ioptron_ieaf().slave_path().empty(); },
     driver_of(cu::make_ioptron_ieaf_focuser)},
    {"ifr", "focuser", "fake_ioptron_ifr.h", alpacacore::DeviceType::Focuser, "ifr-fake",
     "iOptron iFR focuser (fake, test only)", "ALPACACORE_ENABLE_IOPTRON",
     "FakeIoptronIfr (simulated iFR over a pseudo-terminal)", [] { return !cu::ioptron_ifr().slave_path().empty(); },
     driver_of(cu::make_ifr_focuser)},
#endif
#ifdef FAKE_CONFORMU_ASTROASIS
    {"astroasis", "focuser", "fake_astroasis_hid.h", alpacacore::DeviceType::Focuser, "astroasis-fake",
     "Astroasis Oasis focuser (fake HID, test only)", "ALPACACORE_ENABLE_ASTROASIS",
     "FakeHidTransport (simulated Oasis focuser HID, in process)", [] { return true; },
     driver_of(cu::make_astroasis_focuser)},
#endif
#ifdef FAKE_CONFORMU_GEMINI
    {"gemini", "focuser", "fake_gemini_focuser.h", alpacacore::DeviceType::Focuser, "gemini-fake",
     "Gemini focuser (fake, test only)", "ALPACACORE_ENABLE_GEMINI",
     "FakeGeminiFocuser (simulated focuser over a pseudo-terminal)",
     [] { return !cu::gemini_focuser().slave_path().empty(); }, driver_of(cu::make_gemini_focuser)},
#endif
#ifdef FAKE_CONFORMU_QHY
    {"qhy", "focuser", "fake_qhy_qfocuser.h", alpacacore::DeviceType::Focuser, "qhy-fake",
     "QHY Q-Focuser (fake, test only)", "ALPACACORE_ENABLE_QHY",
     "FakeQhyQFocuser (simulated Q-Focuser over a pseudo-terminal)",
     [] { return !cu::qhy_qfocuser().slave_path().empty(); }, driver_of(cu::make_qhy_focuser)},
#endif
#ifdef FAKE_CONFORMU_ZWO
    {"zwo", "filterwheel", "fake_zwo_efw_sdk.h", alpacacore::DeviceType::FilterWheel, "zwo-fake",
     "ZWO EFW (fake SDK, test only)", "ALPACACORE_ENABLE_ZWO", "FakeZWOEFWSDK (simulated EFW SDK, in process)",
     [] { return true; }, driver_of(cu::make_zwo_filterwheel)},
#endif
#ifdef FAKE_CONFORMU_QHY
    {"qhy", "filterwheel", "fake_qhy_cfw3.h", alpacacore::DeviceType::FilterWheel, "qhy-fake",
     "QHYCFW3 (fake, test only)", "ALPACACORE_ENABLE_QHY", "FakeQhyCfw3 (simulated CFW3 over a pseudo-terminal)",
     [] { return !cu::qhy_cfw3().slave_path().empty(); }, driver_of(cu::make_qhy_cfw3_filterwheel)},
#endif
#ifdef FAKE_CONFORMU_PLAYERONE
    {"playerone", "filterwheel", "fake_playerone_pw_sdk.h", alpacacore::DeviceType::FilterWheel, "playerone-fake",
     "Player One Phoenix (fake SDK, test only)", "ALPACACORE_ENABLE_PLAYERONE",
     "FakePlayerOnePWSDK (simulated Phoenix wheel SDK, in process)", [] { return true; },
     driver_of(cu::make_playerone_filterwheel)},
#endif
#ifdef FAKE_CONFORMU_WANDERERASTRO
    {"wandererastro", "filterwheel", "fake_serial_streamer.h", alpacacore::DeviceType::FilterWheel,
     "wandererastro-fake", "WandererAstro SFW (fake, test only)", "ALPACACORE_ENABLE_WANDERERASTRO",
     "FakeSerialStreamer (a streamed SFW status frame over a pseudo-terminal)",
     [] { return !cu::wandererastro_sfw().slave_path().empty(); }, driver_of(cu::make_wandererastro_filterwheel)},
#endif
#ifdef FAKE_CONFORMU_ZWO
    {"zwo", "rotator", "fake_zwo_caa_sdk.h", alpacacore::DeviceType::Rotator, "zwo-fake",
     "ZWO CAA (fake SDK, test only)", "ALPACACORE_ENABLE_ZWO", "FakeZWOCAASDK (simulated CAA SDK, in process)",
     [] { return true; }, driver_of(cu::make_zwo_rotator)},
#endif
#ifdef FAKE_CONFORMU_ONSTEP
    {"onstep", "rotator", "fake_mount_server.h", alpacacore::DeviceType::Rotator, "onstep-fake",
     "OnStepX rotator (fake controller, test only)", "ALPACACORE_ENABLE_ONSTEP",
     "the OnStepX controller model of onstep_controller_model.h (rotator, over loopback TCP)",
     [] { return cu::onstep_controller_host().server.ok(); }, driver_of(cu::make_onstep_rotator)},
#endif
#ifdef FAKE_CONFORMU_IOPTRON
    {"ifr", "rotator", "fake_ioptron_ifr.h", alpacacore::DeviceType::Rotator, "ifr-fake",
     "iOptron iFR rotator (fake, test only)", "ALPACACORE_ENABLE_IOPTRON",
     "FakeIoptronIfr (simulated iFR over a pseudo-terminal)", [] { return !cu::ioptron_ifr().slave_path().empty(); },
     driver_of(cu::make_ifr_rotator)},
#endif
#ifdef FAKE_CONFORMU_GEMINI
    {"gemini", "covercalibrator", "fake_gemini_flatpanel.h", alpacacore::DeviceType::CoverCalibrator, "gemini-fake",
     "Gemini Flat Panel Pro (fake, test only)", "ALPACACORE_ENABLE_GEMINI",
     "FakeGeminiFlatPanel (simulated Motorized Flat Panel V3 firmware over a pseudo-terminal)",
     [] { return !cu::gemini_flatpanel().slave_path().empty(); }, driver_of(cu::make_gemini_flatpanel_pro)},
#endif
#ifdef FAKE_CONFORMU_ZWO
    {"zwo", "camera", "fake_zwo_sdk.h", alpacacore::DeviceType::Camera, "zwo-fake",
     "ZWO ASI camera (fake SDK, test only)", "ALPACACORE_ENABLE_ZWO",
     "FakeZWOSDK (simulated ASI camera SDK, in process)", [] { return true; }, driver_of(cu::make_zwo_camera)},
#endif
#ifdef FAKE_CONFORMU_IOPTRON
    {"iguider", "camera", "fake_iguider_v4l2.h", alpacacore::DeviceType::Camera, "iguider-fake",
     "iOptron iGuider (fake V4L2, test only)", "ALPACACORE_ENABLE_IOPTRON",
     "FakeIGuiderV4L2 (simulated iGuider UVC camera, in process)", [] { return true; },
     driver_of(cu::make_iguider_camera)},
#endif
#ifdef FAKE_CONFORMU_QHY
    {"qhy", "camera", "fake_qhy_sdk.h", alpacacore::DeviceType::Camera, "qhy-fake", "QHY camera (fake SDK, test only)",
     "ALPACACORE_ENABLE_QHY", "FakeQHYSDK (simulated QHYCCD SDK, in process)", [] { return true; },
     driver_of(cu::make_qhy_camera)},
#endif
#ifdef FAKE_CONFORMU_PLAYERONE
    {"playerone", "camera", "fake_playerone_sdk.h", alpacacore::DeviceType::Camera, "playerone-fake",
     "Player One camera (fake SDK, test only)", "ALPACACORE_ENABLE_PLAYERONE",
     "FakePlayerOneSDK (simulated Player One camera SDK, in process)", [] { return true; },
     driver_of(cu::make_playerone_camera)},
#endif
#ifdef FAKE_CONFORMU_TOUPTEK
    {"touptek", "camera", "fake_touptek_sdk.h", alpacacore::DeviceType::Camera, "touptek-fake",
     "ToupTek camera (fake SDK, test only)", "ALPACACORE_ENABLE_TOUPTEK",
     "FakeToupTekSDK (simulated ToupTek camera SDK, in process)", [] { return true; },
     driver_of(cu::make_touptek_camera)},
#endif
#ifdef FAKE_CONFORMU_ALTAIR
    {"altair", "camera", "fake_touptek_sdk.h", alpacacore::DeviceType::Camera, "altair-fake",
     "Altair camera (fake SDK, test only)", "ALPACACORE_ENABLE_ALTAIR",
     "FakeToupTekSDK (simulated ToupTek-family SDK under the Altair driver, in process)", [] { return true; },
     driver_of(cu::make_altair_camera)},
#endif
#ifdef FAKE_CONFORMU_SVBONY
    {"svbony", "camera", "fake_svbony_sdk.h", alpacacore::DeviceType::Camera, "svbony-fake",
     "SVBONY camera (fake SDK, test only)", "ALPACACORE_ENABLE_SVBONY",
     "FakeSVBSDK (simulated SVBONY camera SDK, in process)", [] { return true; }, driver_of(cu::make_svbony_camera)},
#endif
#ifdef FAKE_CONFORMU_GPHOTO
    {"gphoto", "camera", "fake_gphoto_sdk.h", alpacacore::DeviceType::Camera, "gphoto-fake",
     "GPhoto camera (fake libgphoto2, test only)", "ALPACACORE_ENABLE_GPHOTO",
     "FakeGPhotoSDK and FakeRawDecoder (a simulated Nikon D5300, in process)", [] { return true; },
     driver_of(cu::make_gphoto_camera)},
#endif
};

const ConformuHost* find_host(std::string_view vendor, std::string_view device_type, std::string_view fake_header) {
    for (const auto& host : kHosts) {
        if (host.vendor == vendor && host.device_type == device_type && host.fake_header == fake_header) return &host;
    }
    return nullptr;
}

catalog::Schema make_schema(const ConformuHost& host) {
    catalog::Schema schema;
    schema.key = catalog::DeviceKey{host.descriptor_vendor, host.type};
    schema.display_name = host.display_name;
    schema.build_option = host.build_option;
    return schema;  // fields: empty span
}

catalog::Factory make_factory(const ConformuHost& host) {
    catalog::Factory factory;
    factory.key = catalog::DeviceKey{host.descriptor_vendor, host.type};
    factory.create = [&host](const catalog::DeviceConfig&,
                             int device_number) -> std::unique_ptr<alpacacore::AlpacaDriver> {
        return host.make(device_number);
    };
    return factory;
}

int usage() {
    std::cerr << "usage: fake_conformu_server <http-port> <vendor> <deviceType> <fakeHeader>\n"
                 "       fake_conformu_server --describe <vendor> <deviceType> <fakeHeader>\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) return usage();
    const std::string_view first = argv[1];
    const ConformuHost* host = find_host(argv[2], argv[3], argv[4]);
    if (host == nullptr) {
        std::cerr << "no ConformU host for {" << argv[2] << ", " << argv[3] << ", " << argv[4]
                  << "}: add one to kHosts in AlpacaHTTP/tests/fake_conformu_server.cpp\n";
        return 2;
    }
    if (first == "--describe") {
        std::cout << host->description << std::endl;
        return 0;
    }
    const int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) return usage();
    if (!host->ready()) {
        std::cerr << "the fake for {" << argv[2] << ", " << argv[3] << ", " << argv[4] << "} did not start\n";
        return 1;
    }

    alpacahttp::Config config;
    config.set_http_port(static_cast<std::uint16_t>(port));
    config.set_discovery_enabled(false);
    config.set_server_name("FakeConformU");

    alpacahttp::Server server(config);
    auto& router = server.router_for_test();
    router.catalog().add(make_schema(*host));
    router.catalog().add(make_factory(*host));

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    server.start_async();
    std::cout << "fake ConformU server for " << argv[2] << " " << argv[3] << " on port " << port << std::endl;
    while (g_stop == 0 && server.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.stop();
    return 0;
}

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

// The Telescope method dispatch (issue #2081): one source per device type, moved out of
// router.cpp. Router::dispatch_device_method in router.cpp stays the common path.

#include <alpacacore/telescope_driver.h>
#include <alpacahttp/request.h>
#include <alpacahttp/response.h>
#include <alpacahttp/router.h>
#include <alpacahttp/util/error_mapping.h>
#include <alpacahttp/util/log_text.h>
#include <alpacahttp/util/logging_adapter.h>

#include <chrono>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

#include "../device_call.h"
#include "../router_helpers.h"

namespace alpacahttp {

// Moved verbatim from router.cpp (issue #2081), which predates the format gate for these lines;
// formatting the body is its own change, so the move stays byte-identical.
// clang-format off
Response Router::dispatch_telescope_method(std::shared_ptr<alpacacore::TelescopeDriver> telescope,
                                           const std::string& method_name, const DeviceCall& call) {
    const Request& request = call.request();

    return call.run("telescope method '" + method_name + "'", [&]() -> Response {
        // GET-only boolean properties
        if (request.method() == HttpMethod::GET) {
            if (method_name == "cansetrightascensionrate") {
                return call.ok(telescope->get_can_set_right_ascension_rate());
            }
            else if (method_name == "cansetdeclinationrate") {
                return call.ok(telescope->get_can_set_declination_rate());
            }
            else if (method_name == "cansetguiderates") {
                return call.ok(telescope->get_can_set_guide_rates());
            }
            else if (method_name == "cansettracking") {
                return call.ok(telescope->get_can_set_tracking());
            }
            else if (method_name == "cansetpark") {
                return call.ok(telescope->get_can_set_park());
            }
            else if (method_name == "cansetpierside") {
                return call.ok(telescope->get_can_set_pier_side());
            }
            else if (method_name == "canfindhome") {
                return call.ok(telescope->get_can_find_home());
            }
            else if (method_name == "canpark") {
                return call.ok(telescope->get_can_park());
            }
            else if (method_name == "canpulseguide") {
                return call.ok(telescope->get_can_pulse_guide());
            }
            else if (method_name == "canslew") {
                return call.ok(telescope->get_can_slew());  // #1660: the driver's value again
            }
            else if (method_name == "canslewasync") {
                return call.ok(telescope->get_can_slew_async());
            }
            else if (method_name == "cansync") {
                return call.ok(telescope->get_can_sync());
            }
            else if (method_name == "canunpark") {
                return call.ok(telescope->get_can_unpark());
            }
            else if (method_name == "canslewaltaz") {
                return call.ok(false);  // #775: synchronous slews are not served
            }
            else if (method_name == "canslewaltazasync") {
                return call.ok(telescope->get_can_slew_alt_az_async());
            }
            else if (method_name == "cansyncaltaz") {
                return call.ok(telescope->get_can_sync_alt_az());
            }
            else if (method_name == "alignmentmode") {
                int mode = static_cast<int>(telescope->get_alignment_mode());
                return call.ok(mode);
            }
            else if (method_name == "equatorialsystem") {
                return call.ok(static_cast<int>(telescope->get_equatorial_system()));
            }
            else if (method_name == "doesrefraction") {
                return call.ok(telescope->get_does_refraction());
            }
            else if (method_name == "ispulseguiding") {
                return call.ok(telescope->get_is_pulse_guiding());
            }
            else if (method_name == "slewsettletime") {
                return call.ok(telescope->get_slew_settle_time());
            } else if (method_name == "utcdate") {
                return call.ok(alpacacore::device_state_timestamp(telescope->get_utc_date()));
            } else if (method_name == "axisrates") {
                int axis = call.get_int("Axis");
                // AxisRates must return an array of rate objects, even if only one range.
                auto ranges = telescope->get_axis_rate_ranges(axis);
                nlohmann::json rates_array = nlohmann::json::array();
                for (const auto& range : ranges) {
                    nlohmann::json rate_obj;
                    rate_obj["Minimum"] = range.first;
                    rate_obj["Maximum"] = range.second;
                    rates_array.push_back(rate_obj);
                }
                return call.ok(rates_array);
            } else if (method_name == "canmoveaxis") {
                int axis = call.get_int("Axis");
                bool can_move = telescope->get_can_move_axis(axis);
                return call.ok(can_move);
            } else if (method_name == "destinationsideofpier") {
                if (!request.has_query_param("RightAscension")) {
                    throw_bad_request("Missing parameter: RightAscension");
                }
                if (!request.has_query_param("Declination")) {
                    throw_bad_request("Missing parameter: Declination");
                }
                double ra = parse_double_value(request.get_query_param("RightAscension"), "RightAscension");
                double dec = parse_double_value(request.get_query_param("Declination"), "Declination");
                int side = telescope->get_destination_side_of_pier(ra, dec);
                return call.ok(side);
            } else if (method_name == "athome") {
                return call.ok(telescope->get_at_home());
            } else if (method_name == "atpark") {
                return call.ok(telescope->get_at_park());
            } else if (method_name == "slewing") {
                return call.ok(telescope->get_slewing());
            } else if (method_name == "tracking") {
                return call.ok(telescope->get_tracking());
            } else if (method_name == "altitude") {
                return call.ok(telescope->get_altitude());
            } else if (method_name == "azimuth") {
                return call.ok(telescope->get_azimuth());
            } else if (method_name == "declination") {
                return call.ok(telescope->get_declination());
            } else if (method_name == "rightascension") {
                return call.ok(telescope->get_right_ascension());
            } else if (method_name == "declinationrate") {
                return call.ok(telescope->get_declination_rate());
            } else if (method_name == "rightascensionrate") {
                return call.ok(telescope->get_right_ascension_rate());
            } else if (method_name == "trackingrate") {
                int rate = telescope->get_tracking_rate();
                return call.ok(rate);
            } else if (method_name == "trackingrates") {
                auto rates = telescope->get_tracking_rates();
                nlohmann::json rates_array = nlohmann::json::array();
                for (int rate : rates) {
                    rates_array.push_back(rate);
                }
                return call.ok(rates_array);
            } else if (method_name == "targetdeclination") {
                return call.ok(telescope->get_target_declination());
            } else if (method_name == "targetrightascension") {
                return call.ok(telescope->get_target_right_ascension());
            } else if (method_name == "siderealtime") {
                return call.ok(telescope->get_sidereal_time());
            } else if (method_name == "siteelevation") {
                return call.ok(telescope->get_site_elevation());
            } else if (method_name == "sitelatitude") {
                return call.ok(telescope->get_site_latitude());
            } else if (method_name == "sitelongitude") {
                return call.ok(telescope->get_site_longitude());
            } else if (method_name == "focallength") {
                return call.ok(telescope->get_focal_length());
            } else if (method_name == "aperturediameter") {
                return call.ok(telescope->get_aperture_diameter());
            } else if (method_name == "aperturearea") {
                return call.ok(telescope->get_aperture_area());
            } else if (method_name == "sideofpier") {
                return call.ok(telescope->get_side_of_pier());
            } else if (method_name == "guideratedeclination") {
                auto guide_rate = telescope->get_guide_rate();
                return call.ok(guide_rate.dec);
            } else if (method_name == "guideraterightascension") {
                auto guide_rate = telescope->get_guide_rate();
                return call.ok(guide_rate.ra);
            }
        }
        
        // GET/PUT properties
        if (method_name == "doesrefraction") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_does_refraction());
            }
            else if (request.method() == HttpMethod::PUT) {
                bool value = call.get_bool("DoesRefraction");
                telescope->set_does_refraction(value);
                return call.ok_void();
            }
        }
        else if (method_name == "slewsettletime") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_slew_settle_time());
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = call.get_int("SlewSettleTime");
                telescope->set_slew_settle_time(value);
                return call.ok_void();
            }
        }
        else if (method_name == "utcdate") {
            // open-astro#401: a UTCDate write can step the host clock (#289)
            // and latch ClockSource, the same host-level effect the synctime
            // endpoint guards. A browser page on another origin must not be
            // able to fire it; native Alpaca clients send no Origin and pass.
            if (auto rejected =
                    reject_cross_origin_request(request, call.client_tx_id(), call.server_tx_id(), "UTCDate")) {
                return *rejected;
            }
            if (request.method() == HttpMethod::GET) {
                auto utc = telescope->get_utc_date();
                auto time_t = std::chrono::system_clock::to_time_t(utc);
                std::ostringstream oss;
                oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    utc.time_since_epoch()) % 1000;
                oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
                return call.ok(oss.str());
            }
            else if (request.method() == HttpMethod::PUT) {
                const std::string date_str = call.find_string("UTCDate").value_or("");
                if (date_str.empty()) {
                    throw_bad_request("Missing parameter: UTCDate or Value");
                }
                std::string cleaned = date_str;
                if (!cleaned.empty() && (cleaned.back() == 'Z' || cleaned.back() == 'z')) {
                    cleaned.pop_back();
                }
                std::string base = cleaned;
                int millis = 0;
                auto dot_pos = cleaned.find('.');
                if (dot_pos != std::string::npos) {
                    base = cleaned.substr(0, dot_pos);
                    std::string frac = cleaned.substr(dot_pos + 1);
                    if (frac.size() > 3) {
                        frac.resize(3);
                    }
                    while (frac.size() < 3) {
                        frac.push_back('0');
                    }
                    try {
                        millis = std::stoi(frac);
                    } catch (const std::exception&) {
                        throw_bad_request("Invalid UTC date format: " + date_str);
                    }
                }

                std::tm tm = {};
                std::istringstream ss(base);
                ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
                if (ss.fail()) {
                    throw_bad_request("Invalid UTC date format: " + date_str);
                }
                auto utc_time = timegm(&tm);
                if (utc_time == static_cast<std::time_t>(-1)) {
                    throw_bad_request("Failed to convert UTC date: " + date_str);
                }
                auto time_point = std::chrono::system_clock::from_time_t(utc_time) +
                    std::chrono::milliseconds(millis);
                // open-astro#289: on an SBC with no NTP and no RTC this write is
                // the only correct time the host will ever see. Step the system
                // clock from it when the kernel reports the clock undisciplined
                // (never over NTP/chrony/GPS), then hand the driver the same value.
                // The step does not depend on the driver's result: it runs before
                // set_utc_date and stays if the driver throws (Bisque refuses every
                // write; most drivers throw NotConnected before connect, though the
                // ZWO AM mount caches an offline write and succeeds), because a
                // rejection says nothing about whether the client's time is right.
                // The driver's own result goes back to the client unchanged.
                {
                    const auto step = host_clock_.step_from_client(time_point);
                    using Outcome = alpacacore::util::HostClock::Outcome;
                    const std::string what = "UTCDate from " + request.remote_address() + " for telescope/" +
                                             std::to_string(telescope->get_device_number()) + ": host clock " +
                                             alpacacore::util::HostClock::outcome_name(step.outcome) +
                                             " (client - host = " + std::to_string(step.delta.count()) + " ms)";
                    if (step.outcome == Outcome::Stepped) {
                        util::log_info(what);
                    } else if (step.outcome == Outcome::Failed) {
                        util::log_warning(what + ": " + step.error);
                    } else if (step.outcome == Outcome::SkippedSynchronized &&
                               (step.delta > alpacacore::util::HostClock::kClientDisagreementWarn ||
                                step.delta < -alpacacore::util::HostClock::kClientDisagreementWarn)) {
                        // Rendered in ms, not seconds: the threshold is a
                        // millisecond constant, and dividing by 1000 would
                        // log "2 s" for a future 2500 ms value -- which
                        // defeats the point of having one shared constant.
                        util::log_warning(what + "; NTP-disciplined host and client disagree by more than " +
                                          std::to_string(alpacacore::util::HostClock::kClientDisagreementWarn.count()) +
                                          " ms");
                    } else {
                        util::log_debug(what);
                    }
                }
                telescope->set_utc_date(time_point);
                return call.ok_void();
            }
        }
        else if (method_name == "tracking") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_tracking());
            }
            else if (request.method() == HttpMethod::PUT) {
                bool value = call.get_bool("Tracking");
                telescope->set_tracking(value);
                return call.ok_void();
            }
        }
        else if (method_name == "declinationrate") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_declination_rate());
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("DeclinationRate");
                telescope->set_declination_rate(value);
                return call.ok_void();
            }
        }
        else if (method_name == "rightascensionrate") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_right_ascension_rate());
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("RightAscensionRate");
                telescope->set_right_ascension_rate(value);
                return call.ok_void();
            }
        }
        else if (method_name == "trackingrate") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_tracking_rate());
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = call.get_int("TrackingRate");
                telescope->set_tracking_rate(value);
                return call.ok_void();
            }
        }
        else if (method_name == "targetdeclination") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_target_declination());
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("TargetDeclination");
                telescope->set_target_declination(value);
                return call.ok_void();
            }
        }
        else if (method_name == "targetrightascension") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_target_right_ascension());
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("TargetRightAscension");
                telescope->set_target_right_ascension(value);
                return call.ok_void();
            }
        }
        else if (method_name == "siteelevation") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_site_elevation());
            }
            else if (request.method() == HttpMethod::PUT) {
                // open-astro#444: this PUT rewrites persisted device config,
                // the effect configuredevice already guards (#401 precedent).
                if (auto rejected = reject_cross_origin_request(request, call.client_tx_id(), call.server_tx_id(),
                                                                "SiteElevation")) {
                    return *rejected;
                }
                double value = call.get_double("SiteElevation");
                telescope->set_site_elevation(value);
                persist_client_site(*telescope, "siteElevation", value);  // open-astro#444
                return call.ok_void();
            }
        }
        else if (method_name == "sitelatitude") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_site_latitude());
            }
            else if (request.method() == HttpMethod::PUT) {
                // open-astro#444: this PUT rewrites persisted device config,
                // the effect configuredevice already guards (#401 precedent).
                if (auto rejected = reject_cross_origin_request(request, call.client_tx_id(), call.server_tx_id(),
                                                                "SiteLatitude")) {
                    return *rejected;
                }
                double value = call.get_double("SiteLatitude");
                telescope->set_site_latitude(value);
                persist_client_site(*telescope, "siteLatitude", value);  // open-astro#444
                return call.ok_void();
            }
        }
        else if (method_name == "sitelongitude") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_site_longitude());
            }
            else if (request.method() == HttpMethod::PUT) {
                // open-astro#444: this PUT rewrites persisted device config,
                // the effect configuredevice already guards (#401 precedent).
                if (auto rejected = reject_cross_origin_request(request, call.client_tx_id(), call.server_tx_id(),
                                                                "SiteLongitude")) {
                    return *rejected;
                }
                double value = call.get_double("SiteLongitude");
                telescope->set_site_longitude(value);
                persist_client_site(*telescope, "siteLongitude", value);  // open-astro#444
                return call.ok_void();
            }
        }
        else if (method_name == "sideofpier") {
            if (request.method() == HttpMethod::GET) {
                return call.ok(telescope->get_side_of_pier());
            }
            else if (request.method() == HttpMethod::PUT) {
                int value = call.get_int("SideOfPier");
                telescope->set_side_of_pier(value);
                return call.ok_void();
            }
        }
        else if (method_name == "guideratedeclination") {
            if (request.method() == HttpMethod::GET) {
                auto guide_rate = telescope->get_guide_rate();
                return call.ok(guide_rate.dec);
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("GuideRateDeclination");
                telescope->set_guide_rate_declination(value);
                return call.ok_void();
            }
        }
        else if (method_name == "guideraterightascension") {
            if (request.method() == HttpMethod::GET) {
                auto guide_rate = telescope->get_guide_rate();
                return call.ok(guide_rate.ra);
            }
            else if (request.method() == HttpMethod::PUT) {
                double value = call.get_double("GuideRateRightAscension");
                telescope->set_guide_rate_right_ascension(value);
                return call.ok_void();
            }
        }
        
        // PUT-only methods (actions)
        if (request.method() == HttpMethod::PUT) {
            if (method_name == "abortslew") {
                telescope->abort_slew();
                return call.ok_void();
            }
            else if (method_name == "moveaxis") {
                // Debug logging
                if (!request.body().empty()) {
                    util::log_info("moveaxis body: " + util::escape_for_log(request.body()));
                }
                int axis = call.get_int("Axis");
                double rate = call.get_double("Rate");
                util::log_info("moveaxis parsed: Axis=" + std::to_string(axis) + ", Rate=" + std::to_string(rate));
                telescope->move_axis(axis, rate);
                return call.ok_void();
            }
            else if (method_name == "slewtoaltazasync") {
                double altitude = call.get_double("Altitude");
                double azimuth = call.get_double("Azimuth");
                telescope->slew_to_alt_az_async(altitude, azimuth);
                return call.ok_void();
            }
            else if (method_name == "slewtoaltaz") {
                // #775: ASCOM's slew FAQ steers clients to the async forms; refuse before
                // any parameter parse, target write or driver call.
                throw alpacacore::AlpacaException("Synchronous slews are not implemented; use the async form",
                                                  alpacacore::AlpacaError::MethodNotImplemented);
            }
            else if (method_name == "synctoaltaz") {
                double altitude = call.get_double("Altitude");
                double azimuth = call.get_double("Azimuth");
                telescope->sync_to_alt_az(altitude, azimuth);
                return call.ok_void();
            }
            else if (method_name == "park") {
                telescope->park();
                return call.ok_void();
            }
            else if (method_name == "unpark") {
                telescope->unpark();
                return call.ok_void();
            }
            else if (method_name == "findhome") {
                telescope->find_home();
                return call.ok_void();
            }
            else if (method_name == "setpark") {
                telescope->set_park();
                return call.ok_void();
            }
            else if (method_name == "slewtotarget") {
                // #1660: refused untouched when the driver cannot slew; else the
                // async initiator, then a bounded wait for Slewing to clear.
                require_can_slew(*telescope);
                telescope->slew_to_target_async();
                wait_for_slew_end(*telescope, sync_slew_wait_limit(), stopping_);
                return call.ok_void();
            }
            else if (method_name == "slewtotargetasync") {
                telescope->slew_to_target_async();
                return call.ok_void();
            }
            else if (method_name == "slewtocoordinates") {
                require_can_slew(*telescope);
                double ra = call.get_double("RightAscension");
                double dec = call.get_double("Declination");
                telescope->slew_to_coordinates_async(ra, dec);
                wait_for_slew_end(*telescope, sync_slew_wait_limit(), stopping_);
                return call.ok_void();
            }
            else if (method_name == "slewtocoordinatesasync") {
                double ra = call.get_double("RightAscension");
                double dec = call.get_double("Declination");
                // slew_to_coordinates_async sets targets internally; skip
                // redundant set_target calls to avoid extra mutex round-trips.
                telescope->slew_to_coordinates_async(ra, dec);
                return call.ok_void();
            }
            else if (method_name == "synctotarget") {
                telescope->sync_to_target();
                return call.ok_void();
            }
            else if (method_name == "synctocoordinates") {
                double ra = call.get_double("RightAscension");
                double dec = call.get_double("Declination");
                telescope->sync_to_coordinates(ra, dec);
                return call.ok_void();
            }
            else if (method_name == "pulseguide") {
                int direction = call.get_int("Direction");
                int duration = call.get_int("Duration");
                telescope->pulse_guide(direction, duration);
                return call.ok_void();
            }
        }
        
        // Method not found
        return call.error(util::ErrorCode::NOT_IMPLEMENTED,
                          "Method '" + method_name + "' not yet implemented for Telescope");
    });
}
// clang-format on

}  // namespace alpacahttp

# Reads the workspace VERSION file for AlpacaCore/CMakeLists.txt and
# AlpacaHTTP/CMakeLists.txt, so the two cannot parse it differently.
#
# alpacabridge_read_version(<VERSION file>) sets, in the caller's scope:
#   ALPACABRIDGE_VERSION       the full string (5.0.0~beta1), for the version
#                              defines that DriverVersion and the web UI report
#   ALPACABRIDGE_BASE_VERSION  its numeric part (5.0.0), for project(), which
#                              takes integer components only
# A beta VERSION spells the Debian pre-release suffix ~betaN
# (docs/beta-channel.md). scripts/check_beta_configure.sh runs this file in
# script mode (cmake -P) over sample values; include() it before project().
function(alpacabridge_read_version version_file)
    file(STRINGS "${version_file}" full LIMIT_COUNT 1)
    string(STRIP "${full}" full)
    string(REGEX REPLACE "~.*$" "" base "${full}")
    if(NOT base MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "${version_file} holds '${full}', which is not X.Y.Z or X.Y.Z~betaN")
    endif()
    set(ALPACABRIDGE_VERSION "${full}" PARENT_SCOPE)
    set(ALPACABRIDGE_BASE_VERSION "${base}" PARENT_SCOPE)
endfunction()

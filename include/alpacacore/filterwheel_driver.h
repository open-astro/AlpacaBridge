// AlpacaCore
// Copyright (c) 2025 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the Server Side Public License, version 1 (SSPL v1).
// https://github.com/open-astro/AlpacaCore/blob/main/LICENSE
//
// If you use this library to provide a network-accessible service, you must comply
// with the SSPL v1 requirements.

#pragma once

#include <alpacacore/alpacadriver.h>
#include <alpacacore/util/error_handling.h>
#include <vector>
#include <string>

namespace alpacacore {

/**
 * @brief Pure virtual interface for Alpaca FilterWheel drivers.
 *
 * Follows ASCOM Alpaca FilterWheel API specification.
 * All filter wheel drivers must implement this interface.
 */
class FilterWheelDriver : public AlpacaDriver {
public:
    virtual ~FilterWheelDriver() = default;

    // FilterWheel-specific properties

    /**
     * @brief Get the current filter position (0-based).
     */
    virtual int get_position() const = 0;

    /**
     * @brief Set the filter position (0-based).
     */
    virtual void set_position(int position) = 0;

    /**
     * @brief Get the number of filter positions.
     */
    virtual int get_focus_offsets_size() const = 0;

    /**
     * @brief Get the focus offset for a filter position.
     *
     * @param position Filter position (0-based)
     * @return Focus offset in steps
     */
    virtual int get_focus_offset(int position) const = 0;

    /**
     * @brief Set the focus offset for a filter position.
     *
     * @param position Filter position (0-based)
     * @param offset Focus offset in steps
     */
    virtual void set_focus_offset(int position, int offset) = 0;

    /**
     * @brief Get the names of all filters.
     */
    virtual std::vector<std::string> get_names() const = 0;

    /**
     * @brief Set the names of all filters.
     */
    virtual void set_names(const std::vector<std::string>& names) = 0;
};

} // namespace alpacacore


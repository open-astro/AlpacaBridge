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

#include <alpacahttp/util/yaml_comment.h>

#include <string>

#include "test_assert.h"

using alpacahttp::util::strip_yaml_comment;

int main() {
    // Trailing comments and full-line comments.
    EXPECT(strip_yaml_comment("port: 11111 # http") == "port: 11111 ");
    EXPECT(strip_yaml_comment("# only a comment") == "");
    EXPECT(strip_yaml_comment("port: 11111") == "port: 11111");

    // '#' inside a quoted value is data, for both quote styles.
    EXPECT(strip_yaml_comment("location: \"Obs #2\"") == "location: \"Obs #2\"");
    EXPECT(strip_yaml_comment("location: 'Obs #2'") == "location: 'Obs #2'");
    EXPECT(strip_yaml_comment("location: \"Obs #2\" # note") == "location: \"Obs #2\" ");
    EXPECT(strip_yaml_comment("location: 'Obs #2' # note") == "location: 'Obs #2' ");

    // Backslash escapes inside double quotes keep the quote open.
    EXPECT(strip_yaml_comment("name: \"a\\\"#b\" # c") == "name: \"a\\\"#b\" ");

    // An apostrophe or quote inside a plain value is a literal.
    EXPECT(strip_yaml_comment("name: it's #x") == "name: it's ");
    EXPECT(strip_yaml_comment("name: a\"b #x") == "name: a\"b ");

    // A quote after the first colon of a plain value does not open a string.
    EXPECT(strip_yaml_comment("url: http://h:1 #x") == "url: http://h:1 ");
    return 0;
}

# AlpacaHTTP
# Copyright (c) 2025-2026 Joey Troy and contributors
#
# This file is part of AlpacaHTTP.
#
# AlpacaHTTP is licensed under the GNU Affero General Public License,
# version 3 or (at your option) any later version (AGPL-3.0-or-later),
# with an additional permission allowing combination with proprietary
# device-vendor SDKs. See the LICENSE file in this repository for the full
# license text and the vendor-SDK linking exception, or the license online at:
# https://www.gnu.org/licenses/agpl-3.0.html
"""The server exits 1 when it stops without a shutdown request (#2088, #713).

A first bind that fails is that case: the port is held by this script, the
server cannot listen, its loop ends with nobody having asked, and systemd's
Restart=on-failure needs a non-zero status to respawn it.

Usage: server_exit_status.py <alpacahttp_server binary>
"""
import os
import socket
import subprocess
import sys
import tempfile


def main(server):
    holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    holder.bind(("0.0.0.0", 0))
    holder.listen(1)
    port = holder.getsockname()[1]
    env = dict(os.environ, ALPACAHTTP_PORT=str(port), ALPACAHTTP_DISCOVERY="false",
               ALPACAHTTP_FILE_LOGGING="false")
    with tempfile.TemporaryDirectory() as cwd:
        try:
            proc = subprocess.run([server], cwd=cwd, env=env, capture_output=True, text=True, timeout=20)
        finally:
            holder.close()
    err = proc.stderr + proc.stdout
    if proc.returncode != 1:
        print("expected exit status 1, got %d\n%s" % (proc.returncode, err))
        return 1
    if "first bind failed" not in err:
        print("the ERROR line does not say that a failed first bind also exits 1:\n%s" % err)
        return 1
    print("server exited 1 on a port it could not bind")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

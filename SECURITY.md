# Security policy

## Reporting a vulnerability

Do not open a public issue for a security problem.

Report it privately through GitHub: open the repository's **Security** tab and
select **Report a vulnerability**, or go to
<https://github.com/open-astro/AlpacaBridge/security/advisories/new>.
Only the maintainers can read the report.

Include:

- the AlpacaBridge version (`alpacabridge --version`, or the `.deb` version) and the board;
- what an attacker can do, and from where (same network, the web UI, a device on USB or serial);
- the steps to reproduce it, or a proof of concept.

The maintainers confirm the report, agree a fix and a disclosure date with you,
and publish a GitHub security advisory with the release that fixes it.

## Supported versions

| Version | Security fixes |
| --- | --- |
| `main` and the newest 6.0 beta | Yes |
| 5.0.x (the newest stable release) | Yes |
| Older releases | No: update to a supported version |

## Scope

AlpacaBridge serves the ASCOM Alpaca API, the web UI and Alpaca discovery on the
local network, without authentication, as the Alpaca standard expects. Run it
on a network you trust. A report that an unauthenticated client on that network
can control the devices is expected behavior; a report that it can do more (run
code, read or write files outside the configuration, crash the server, or reach
the host from outside the local network) is in scope.

Vendor SDK binaries under `AlpacaCore/external/` come from the vendors. Report a
problem inside one of them to that vendor; tell us too, so we can track the fix.

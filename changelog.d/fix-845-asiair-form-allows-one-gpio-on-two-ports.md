### Fixed

- **ASIAIR device form reports a GPIO line chosen for two ports before saving** (AlpacaHTTP web UI): the form now shows a message naming the repeated line and stops the submit, including when a saved port list loads with a repeated default. The server check is unchanged.

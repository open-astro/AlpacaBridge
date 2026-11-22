### Fixed (tests)

- **CGX re-home case waits for the newer command to take the axes** (Celestron CGX tests, issue #2466): "a newer FindHome or Park or a disconnect during a re-home ends its homes" gave the newer command 2 s of real time and then stepped the fake clock, so on a slow runner the old body could end its still-wait and send its second home first. The case now waits for the Dec command that is sent only after the old body was superseded or joined: a newer FindHome or Park's Dec home, or the disconnect's Dec stop.

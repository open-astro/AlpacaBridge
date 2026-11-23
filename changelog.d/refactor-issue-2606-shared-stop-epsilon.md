### Changed

- **Command gate: one shared constant for the just-below-1e-9 MoveAxis stop epsilon** (AlpacaCore telescope command gate, ZWO and Bisque telescope drivers, issue #2606): ZWO and Bisque each hand-wrote the largest double under 1e-9 as their `move_axis_stop_epsilon`, each with its own `static_assert`. The gate header now holds it once as `util::kMoveAxisStopBelow1e9`, with one `static_assert` that pins it one ulp under 1e-9, and both drivers use it. No behaviour change: 1e-9 is still a move for both.

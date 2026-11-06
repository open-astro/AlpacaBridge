### Fixed (tests)
- **ZWO camera stress cases: the download coverage check no longer depends on scheduling** (issue #2099): both connected-SDK camera stress cases first run one undisturbed 1 ms exposure, so the `get_data_after_exposure` check holds on a starved host. The EAF staleness-loss check is unchanged.

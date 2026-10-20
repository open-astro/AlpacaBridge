### Added (docs)
- **SDK seams decision record** (docs, `docs/decisions/0015-sdk-seams.md`, accepted): records why each vendor SDK is reached through one abstract interface with a real wrapper, a scripted fake and a test-only `Locked<Vendor>SDK` decorator, the destructor shape, the harsher-fake rule and the seams in place. The testing instructions link it.

### Changed
- **Designs to avoid** (docs, `docs/decisions/0013-designs-to-avoid.md`): a seventh rejected design, a production `ByteChannel` between the serial and socket wrappers and their file descriptors, with the condition that would reopen it.

# Fork Local Message State Storage

## Goal

Move Forkgram-only message revision and locally-hidden state out of the monolithic prefs blob into a separate incremental store. The store must keep UI work small, encrypt data with libsodium AEGIS-128L, and avoid expanding crypto changes to unrelated app storage.

## Success Criteria

- Existing legacy prefs keys are read once for migration, then cleared.
- Current state is stored under the account tdata directory outside `lskPrefs`.
- Revisions and hidden markers are partitioned by UTC month.
- Writes rewrite only dirty partitions plus the compact index; serialization, encryption, and disk writes run off the UI thread.
- Old revision-only entries can be pruned by partition-friendly retention, while hidden/deleted correctness state is preserved.
- The Release target builds on this macOS checkout; if the new dependency changes the bundle, use the full packaging path rather than a fast executable swap.

## Plan

1. Add a small Forkgram local message state module with pure partition/retention helpers and AEGIS-backed read/write functions.
2. Add libsodium as a packaged external CMake dependency and link it only into the Forkgram object target.
3. Replace the anonymous `history_item.cpp` store cache with the new module types, legacy migration, dirty partition tracking, and async write scheduling for revisions and hidden markers.
4. Reconfigure/build Release and run the narrowest available checks for the touched code.
5. Package/install the macOS app because libsodium adds a new runtime dylib.

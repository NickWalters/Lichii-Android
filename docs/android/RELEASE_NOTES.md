# Android boot fix (after 0.4.0)

- Fixes an Ocarina of Time startup hang with SDL audio. The imported microphone path
  could wait indefinitely for an Android recording-permission response. Android now
  uses a silent microphone input while keeping SDL playback enabled; desktop capture
  is unchanged. Real Android microphone capture is not supported yet.
- Device check: nubia NP06J / Adreno 840 / Turnip, SDL enabled, reached the title menu
  at approximately 60 fps. All 999 host test cases passed, including silent-input
  open, engage, pump, purge, close and reopen coverage.
- Gameplay validation is incomplete. A separate Kokiri Forest run with audio disabled
  showed magenta lighting and approximately 14–15 fps. The original white-flash issue
  has not been confirmed resolved on this device.

# Lichii for Android 0.4.0

Merges the latest Lichii (Zortos) `nx2` code, up to `bf48fe92cc` (8 Nov 2026), into the Android fork. Not yet validated on a device.

## Highlights

- **The Legend of Zelda: Ocarina of Time**
  - Fixes for the white flashes from Lichii: the GPU now waits for pending query results before macro DMA reads (#215). Also imported from Frizti's Fritchii v4: the water flicker fix and staging buffer reuse (#778).
  - Drawing stays in the render pass that a deferred clear began (#699), and the cap on bank-bound occlusion query resets is raised to 4 banks (#718).
  - Fixes the red line in the tomb, because noperspective varyings are now interpolated perspective-correct (#820).
  - The movie network is replaced by an input copy (#721). The FPS Unlock is built in, and a Switch 2 frame rate limit slider is added (#655).
  - Hyrule Field grass and near foliage bind their storage buffers correctly (#551, #410).
  - **Adreno (Turnip):** software matrix B gathers in the end-of-frame network are faster on guarded 32-lane Turnip compute pipelines.
- **Mario Kart World:** the karts stay on the water and the waves come back (#690).
- **Mario Tennis Fever:** Switch 2 controller button icons are drawn with the firmware fonts (#691, #821).
- **Textures:** ASTC uploads are cheaper (#296). New images no longer get stale guest memory uploaded into them (#818). Uploads that a join copy overwrites completely are skipped (#817).
- **Pipeline cache:** only damaged, stale or duplicate records are dropped (#815).
- **GPU thread:** the pending-channel poll timer resets when a channel makes progress (#621). A channel can wait for a GPU-written command list without blocking the others (#752).
- All fork changes from 0.3.0 are kept: macro DMA read snapshots, the Ocarina of Time preflush, and the memory caps while scenes load.

## Known issues

- On-device testing of this release is still pending. Please report whether the white flashes in Ocarina of Time are gone on your phone.
- **Recommended:** a phone with **16 GB of RAM**.

## For testers

- Install the APK, add your games folder, and set up keys and firmware as usual. None are included.
- Diagnostic switches can be set in `Android/data/dev.lichii.android/files/env.txt` (see the README).
- Report problems at https://github.com/NickWalters/Lichii-Android/issues with your phone, driver and `eden_log.txt`.

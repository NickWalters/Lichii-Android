# OoT / Adreno 840 matrix optimization candidate

Status: 2026-11-08. This is a focused matrix candidate, **not yet a production
merge recommendation**. No game assets, keys, shader binaries, frame captures,
driver binaries or APKs accompany this branch. Measurements are author-local.

## Scope

- Software matrix B gather: replace variable-lane shuffles with constant-lane
  broadcasts on supported Turnip compute pipelines explicitly requiring 32 lanes.
  Float arithmetic, FMA ordering and rounding are unchanged.
- Shared-B staging is selected automatically when all safety checks pass.
  Requires SPIR-V 1.6, explicit workgroup layout, full 32-lane subgroups, a
  conservative reconverged top-level call-flow check and a 16 KiB shared budget.
  Separate aliased explicit-layout scratch follows guest shared memory. Subgroup
  barriers protect publication and reuse. Broader divergent callers are refused.
- Broadcast remains the fallback when staging is ineligible; the original shuffle
  path remains for other drivers, subgroup sizes and stages. No new runtime
  experiment-selection variables are included.

Bank-reupload experiments, Accurate command-list copies, WFI skip, decoder pacing
and the gameplay bilinear stand-in are excluded. They did not establish sufficient
benefit or correctness for this patch. Existing movie interception and the native
gameplay network are unchanged.

## Evidence and limits

Android 16, Adreno 840, Accurate GPU, 0.5x, frame generation off, plain Turnip.
Historical samples below used the earlier experimental APKs, not this final tree.
Repeated samples are four 5-second windows in the same treehouse scene.

| Comparison | Mean FPS | Interpretation |
|---|---:|---|
| Same APK, broadcast / shared-B | 6.54 / 8.08 | Supports a shared-B gain in this run |
| Shared-B plus WFI skip | 8.03 | No clear gain; left disabled |
| Shared-B plus Accurate list-copy, early clean run | 8.63 | Not interleaved; modest apparent gain |
| DD-Turnip v0.5.0 | 2.99 | Slower in this test; reverted |
| Return to plain Turnip, existing Accurate-copy APK | 14.52, repeat 14.50 | Unexplained runtime change; no new code installed |
| New stand-in-capable APK, flag unset | 14.35 | Faster control survives update |
| Flag enabled, but contract refused / native fallback | 14.70 | **Not** a stand-in speedup |

The owner independently observed approximately 15 FPS. This is not 30 FPS and
the unexplained 8.63 -> 14.5 change must not be attributed to a new source fix.
Fast mode still flashes. Accurate was reported to have only one barely noticeable
flash in one run; this is not a flash-free correctness proof.

Later controls reached approximately 17 FPS on the existing APK. A reboot alone
recovered 4.69 to 17.74 FPS without a source change. These observations reinforce
the need for interleaved controls; they are not additional matrix speedup evidence.

Historical matrix checks: 865,962 assertions / 14 host cases; 90 emitted modules passed
SPIR-V validation. 56 paired GPU fixture comparisons and seven full-grid cases
were byte-identical, including guest shared-memory sentinels. Fixture median
40.77 -> 15.03 ms is **not** game frame time. Fresh game-generated SPIR-V admitted
shared-B, validated, and retained its arithmetic/rounding opcode stream.

These results and the earlier successful Android compile/link predate the flag-free
cleanup. They do not substitute for testing this tree. The synthetic fixture tests
now select broadcast through a missing explicit-layout capability and shared-B
through supported capabilities, rather than setting an environment variable.
Both paths check their distinct emitted instructions and retain guest shared-memory
sentinels.

Fresh flag-free checks on 2026-11-08:

- Standalone MSVC source harness: `[tensor]` passed 933,842 assertions / 14 cases;
  `[ampere]` passed 1,346,560 assertions / 16 cases. These filters overlap and must
  not be added together. The harness builds real SPIR-V/IR sources but substitutes
  logging/settings/assertion plumbing; it is not the production test executable.
- All 86 emitted synthetic modules passed `spirv-val --target-env vulkan1.3`.
  Eight shared-B GPU fixtures differ from historical fixtures only in the order
  of capability/extension declarations.
- Fresh device replay of the final-tree synthetic modules passed 63 paired output
  comparisons: 56 cases across K8/K16, one/192 calls and both rounding paths, plus
  seven cases at 1,296 workgroups. Broadcast and shared-B outputs were byte-identical,
  including the guest shared-memory sentinels. Device: Adreno 840, Android 16,
  Turnip Mesa 27.0.0-devel (`79cb04951e`), required subgroup size 32. This checks
  synthetic output equivalence, not gameplay correctness or a game FPS gain.
- Android NDK Clang 19 syntax checks passed for the final matrix emitter and Vulkan
  pipeline-cache translation units using the previous build's dependencies and
  generated headers. This is not a fresh configure, link or package result.
- The final flag-free tree completed Android compile, link and packaging with
  `assembleMainlineRelWithDebInfo`. A local-only download adapter fetched the same
  release checksum assets through GitHub's API after direct URLs returned HTTP 504;
  asset digests and archive hash verification remained enabled. No tracked build
   files or dependency versions were changed. No complete desktop/backend or
   all-game regression suite was run.
- A bounded run of that APK booted OoT into the treehouse scene and completed
  short movement inputs. Local before/after screenshots showed gameplay and a
  changed character/view orientation. Only the separate test package was updated;
  its temporary diagnostic environment was restored and the run was stopped.
  This is a boot/input smoke test, not a sustained FPS, flash-free rendering or
  multi-scene correctness result. Game captures remain local and are not part of
  this patch.
  The run still logged unmapped device reads during loading; reaching gameplay
  does not establish an error-free log or resolve those memory reports.

Profiling correction: Accurate-copy reduced buffer Finish waits, but roughly
563–575 ms/s reappeared in WaitSubmittedTick. Much of the wait moved, rather than
vanished. Genuine GPU dependencies and CPU-write preservation remain necessary.

## Before production consideration

1. Complete production Linux GCC 13/Werror and ctest on this exact tree. The fresh
   source-harness, SPIR-V, paired GPU and Android compile/link/package checks above
   passed, but do not replace the complete production test runner.
2. Reproduce interleaved controls and investigate driver/runtime/clock variance.
   GPU frequency was not readable through the tested sysfs path; no clock floor
   was changed by this emulator patch.
3. Validate multiple scenes, camera motion, videos, transitions, cold/warm caches,
   other games/drivers/subgroup sizes and desktop backends.
4. Obtain human code review and direct testing, and follow the destination's
   contribution and AI-disclosure policies. A clean merge or retarget check is
   not build, gameplay or correctness evidence.

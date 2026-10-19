# Decoder selection and measurement

Select an encoded result or an imported compressed source, then open **Decoder**
in the right inspector. Choose an implementation and use **Decode once** or
**Measure**. Discovery is asynchronous and independent of the encoder. Successful
choices replace that image's preview and invalidate its lazy quality metrics.
Errors leave a usable preview intact. Color interpretation and import assumptions
are preserved when changing implementations. Compressed imports remain available
for repeated decoding after their preview changes.

Vulkan Video, VA-API and native software libraries run in process. Hardware choices
require an actual successful decode; selected hardware cannot fall back to CPU.
Candidates must preserve dimensions, depth and chroma. Video samples are checked
against the retained preview. VVC requires exact visible integer samples; other
video paths permit at most one code value of rounding difference.
This verifies the selected image; it does not certify every part of a codec spec.

The selector prefers a verified Vulkan implementation, then VA-API, then software.
Normal encoding continues to publish its initial decoded preview immediately;
opening the Decoder tab probes alternatives without holding that preview.

## Timing and statistics

Each attempt creates a fresh decoder. Total time covers decoder initialization,
decoding, GPU completion, CPU readback, pixel packing and decoder teardown.
Libavcodec adapters also report an inner codec interval: send/receive plus a
VA surface sync or Vulkan timeline wait. That interval excludes device/context
setup, CPU readback and pixel packing. Native adapters currently report total time.
Metrics, correctness checking, final display-context copying and rendering are
outside the measured intervals. Energy covers total time, not the inner timer.

Every attempt is retained in per-image session memory, including the first run,
failures and attempts completed before cancellation. No warmup samples, medians,
trimming or outlier rejection are used. Successful decode times have arithmetic
means, unbiased sample standard deviations and standard errors (SD / sqrt(n)).
Failure times and errors are listed explicitly; they are not presented as successful
decode performance. Cancellation takes effect between calls and preserves history.

VVdeC uses native SIMD and automatic threads; dav1d uses automatic threads.
Native libde265/libvpx/AVM adapters retain their existing single-thread policy,
which is named in the selector. Libavcodec software adapters use library defaults.

## Energy

RAPL discovery prefers a readable psys counter, otherwise one readable package
counter. Overlapping domains are not summed. Raw joules and midpoint read intervals
are retained per attempt; there is no idle subtraction. Domain average power is
sum(joules) / sum(interval seconds). Single counter wraps are corrected. Multiple
wraps during a single attempt cannot be identified.

The UI names the domain. Coverage is firmware-dependent and includes unrelated
activity; this is not wall power or decoder-attributable energy. Counter permission
failure leaves timing usable.

The Decoder panel has **Enable energy...**, **Cancel energy request**, and
**Disable energy** controls. Authorization uses `pkexec` and the desktop's polkit
agent. Only the small native `codec_vis_energy_helper` elevates. It opens fixed
sysfs attributes read-only, sends one counter FD over an authenticated abstract
Unix socket, then exits. The GUI verifies peer UID/PID, protocol metadata,
read-only FD flags and an actual counter read before accepting access. Both
socket endpoints and the descriptor are close-on-exec. No socket files, chmod,
permanent permission changes, root GUI, or privileged sampling daemon are used.
Access lasts for this application session; repeated measurements read the counter
in process. Disable releases access for subsequent measurements. An active run
retains its own lease until completion. Old runs retain their original energy
availability and samples.

Real psys FD transfer, reads after the helper exits, disable with an active lease,
and missing-helper/model cancellation/error/retry paths are covered by
`build/test_energy_access` (`--authorize` additionally tests real elevation).
The measurements include concurrent system activity; they are not decoder-only
energy.

Bundled native decoder support is described in
[third_party/native_video_decode/NOTICE](../third_party/native_video_decode/NOTICE).

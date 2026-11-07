# WAV import policy

`fl_live_media` decodes WAV files into independent `AudioClip` copies on a control or worker thread. It is built with the renderer option because it shares the pinned libsndfile 1.2.2 baseline. The engine, controller and backend libraries do not link it, so no decoding or file access can run in the render callback.

## Admission rules

| Property | Policy | Refusal |
|---|---|---|
| Container | RIFF/WAVE or WAVE_FORMAT_EXTENSIBLE | `not_wav`, including AIFF, FLAC, text and truncated headers |
| Encoding | Unsigned 8-bit; signed 16/24/32-bit PCM; 32/64-bit float | `unsupported_encoding` (for example A-law, mu-law, ADPCM) |
| Channels | One (mono) or two (interleaved left/right) | `unsupported_channels` |
| Sample rate | Must equal the session rate | `sample_rate_mismatch`; the message names both rates |
| Length | 1 to `frame_limit` frames (default 2^24, about 5.8 minutes at 48 kHz) | `empty`, `too_long` |
| Values | Integer PCM is normalized to [-1, 1). Float values are kept as stored and must be finite; they may exceed 1 because the engine mix is unclipped | `non_finite` |

There is no resampling, warping or channel conversion. A file at another rate is refused rather than played at the wrong speed. Every refusal carries a short message that says what to change. A missing file, a directory or an unreadable path returns `cannot_open`.

Header frame counts are not trusted for the limit: the reader reserves only what the header declares within the limit and stops with `too_long` as soon as decoded frames exceed it. Decoding reads 16,384-frame chunks and checks an optional cancel flag between them.

## Worker ownership

`WavImportWorker` owns one thread. The single control owner calls `submit`, `poll` and `cancel`; `poll` never waits for decoding. Requests complete in submission order. Cancelling a pending request reports `cancelled` without opening the file; cancelling the running request interrupts it at the next chunk. Destruction cancels the running request and joins the thread.

Results never modify a document. The caller applies a successful clip to a candidate document and render preparation, and keeps its current state on any failure. The imported `AudioClip` is copied into `DocumentRenderPlan`, which then owns it for the plan lifetime.

## Verification

`wav-import` writes fixtures with its own RIFF writer, independent of libsndfile. It checks PCM16 mono/stereo, PCM24 and float32 values, a matching 44.1 kHz session policy, the inclusive frame limit and a header that under-reports its data. It checks each refusal: missing file, directory, text, truncated header, AIFF, A-law, three channels, rate mismatch, empty data, NaN, infinity, an over-limit file, invalid policies and a pre-set cancel flag. An imported stereo clip is prepared through `DocumentRenderPlan` and rendered by the engine. The worker test checks that decoding runs on a separate thread, ordered completion, pending and running cancellation, and prompt shutdown during a 20-second decode.

These are offline checks. The desktop does not offer import yet; Session UI integration, waveform display and loop ranges are tracked in #27 and #31.

# Handoff: iOS on Pixel 4 (flame)

Last updated **2026-10-22, 09:40 local** (newest work first under "Start here"). Read this first, then [NOTES.md](NOTES.md) (hardware facts,
test loop, bootloader rules) and `docs/ios-sweet/NOTES.md` (history of every kernel patch).

## Start here (next session)

### HOTDOGB: the ~16 ms per backboardd frame is ~9,000 one-row 2D blits re-copying the same shadowed textures every frame; Turnip fix 0101 written (runs k0175-sysmem-1111, k0176-surf64-1111, k0175-m2vmsg-1111, k0177-blitruns-1111, 2026-11-11)
- Observed, TU_DEBUG=sysmem (`/var/local/mtl2-tu-debug`, removed again): the gpuhist shows direct rendering
  (marker 1, 124 draws instead of ~1,700). Swipebench 38.7 / 33.0 fps, within the noise of 0175's 34.7 / 32.9.
  The heavy IB still takes 16.3 ms (17.5 with tiles). ~177k of its ~187k dwords are ~10,900 CP_BLITs
  (~1.5 us each). The second IB takes 7.3 ms.
- The code (Mesa tu_clear_blit.cc copy_buffer_to_image ~2843, copy_image_to_buffer ~3067): on A6xx a
  buffer<->image copy whose buffer start or row pitch is not a multiple of 64 bytes is issued as one CP_BLIT
  per row (A7xx has an unaligned mode). ~150 such copies of ~76 rows per frame, so smallish images, not
  full-screen ones.
- 0176 (round an explicit unaligned IOSurfaceBytesPerRow up to 64, `sweetsurfalign`, default 1): only 5
  unaligned surfaces in a whole boot (e.g. SpringBoard 230x186 LA08 bpr 480 -> 512); backboardd's framebuffers
  are already 4608. CP_BLIT is still ~11,400 per job, fps 33.4 / 32.4. Harmless, kept. So the copies do not come
  from unaligned IOSurfaces.
- Candidate sources in metal2vulkan (any of them, not yet told apart):
  - shadow images of textures over MTLBuffers (texture.rs ~2044, `Degradation::ShadowedTexture`), copied in
    and back around each encoder (shadow.rs ~826);
  - `replaceRegion` uploads, packed tightly (pitch = width x bpp, texture.rs ~5449);
  - `copyFromBuffer:toTexture` blits with the app's bytesPerRow (blit_texture.rs ~801).
- `SWEET_M2V_MESSAGES=/var/local/m2v-msg.log` in `/var/local/mtl2-env` reached backboardd, SpringBoard and
  others (console "env ..." lines), but no file appeared (k0175-m2vmsg-1111, 37.6 fps swipebench). Guess: the
  bridge could not create it. Both it and `/var/local/mtl2.log` (opt-in: written only when it exists) are now
  pre-created empty with mode 666; the env file is still on the phone.
- MEASURED with 0177 (k0177-blitruns-1111, swipebench 39.7 fps, two backboardd jobs captured mid-swipe): 8,900 /
  8,964 CP_BLITs per job, 8,814 / 8,876 of them in ~50 one-row runs. The same destinations in both jobs, so the
  same textures are copied again every frame: metal2vulkan shadow copy-ins (shadow.rs: "copy in before the first
  command of an encoder that uses the texture"; the CPU may have written the buffer). All are buffer -> tiled
  image with tightly packed rows, start aligned (x 0, once x 8):
  - 2 large RGBA8: 1105x1160 (pitch ~4420) and 1271x1823 (~5084), ~3,000 rows;
  - ~20 x 180x180 RGBA8 (pitch 720: home screen icons), ~3,600 rows;
  - ~25 A8 140-285 px wide x 62/134 rows (pitch 160/224/288: icon labels), ~2,200 rows;
  - a few tiny ones (6 px) back to buffers.
- Fix written, NOT built yet: phone/turnip/patches/0101-sweet-a6xx-unaligned-buffer-copy-repack.patch
  (tu_clear_blit.cc, A6xx 2D path only). A pitch that is a multiple of 64 with an unaligned start: one blit
  with an x offset. An unaligned pitch: with k = 64 / gcd(pitch, 64), k blits at pitch k x pitch repack the
  rows into Turnip's scratch BO at a 64-aligned pitch, then one blit into the image (Turnip's own staging
  flushes in between). Expected ~210 blits per frame instead of ~8,900. A/B without rebuilding:
  `TU_SWEET_ROW_REPACK=0` in /var/local/mtl2-env. Needs the Mac: setup-metal2-trees.sh, build-turnip-ios.sh,
  phone/metal2/build.sh, then tools/hotdogb-metal2-install.py with the new SweetMetal2.tgz. Check after:
  gpuhist "BLIT RUNS" should show few one-row runs; the heavy IB ~16 ms -> a few ms; the images must look right
  (icons, labels, wallpaper).
- Kernel 0177 (sha256 b1d97bc2..., booted in k0177-blitruns-1111) = 0176 + gpuhist groups CP_BLITs into one-row runs:
  `#### GPUHIST BLIT RUNS: N blits, M of them in R one-row runs` and up to 40 `GPUHIST run k: ROWS rows of W px
  at x X, dst y.., src A..B (step = the buffer pitch), src info/pitch, dst base/info/pitch`. x > 0 = the start
  is unaligned; step % 64 != 0 = the pitch is. A dst pitch of 0 = the image->buffer direction (copy back).
- The phone dropped off USB after k0175-m2vmsg-1111 ended (no fastboot, no network); it was back in fastboot later.

### HOTDOGB: kernel 0175 panel copy on its own thread: vsync regular, real fps unchanged (~33-35); EARLIER FPS NUMBERS WERE INFLATED (runs k0175-pthread-1111, k0175-nothread-1111, 2026-11-11)
- 0175 (WSL build, sha256 d14264d6...) = 0174 + AppleCLCD's vsync timer only signals a present thread (boot-arg
  `sweetpthread`, default 1; 0 = the copy in the timer as before). The thread copies the latest swapped frame, one
  per kick; a kick while it is busy is counted as "skipped". "SWEET FB: 5s" lines now end in "vsync late max N us,
  N ticks > 1/8 period late, N copies skipped (busy)". Milestone "#### PANEL COPY ON ITS OWN THREAD (VSYNC NEVER WAITS
  FOR THE GPU)" / "#### PANEL COPY IN THE VSYNC TIMER (sweetpthread=0)" (before the live log starts; not seen).
- Observed: the "5s" stats window is 450 vsync ticks, so with late ticks it ran long. With the copy in the timer
  (sweetpthread=0) the windows lasted 5.0-10.4 s while swiping, vsync up to 264 ms late, 25-112 ticks per window
  > 1.4 ms late. The swipebench divided by 5, so it overstated fps by up to 2x. Re-scored from the logs with the real
  time between FB lines (scratchpad rescore.py; tools/hotdogb-swipebench.py now does this):

  | run | old score | real swaps/s | panel copies/s |
  |---|---|---|---|
  | k0172-swipe-nolog-1110 (0172) | 27.0 / 22.8 | 27.0 / 22.8 | 26.9 / 22.7 |
  | k0172-swipe-withlog-1110 (0172 + junk load) | 34.0 / 36.4 | 32.8 / 30.0 | 31.8 / 25.8 |
  | k0173-swipe-big-1110 (0173) | **63.8 / 67.7** | **33.6 / 33.6** | 19.3 / 17.3 |
  | k0175-nothread-1111 (0175, sweetpthread=0) | 47.0 / 70.1 | 33.1 (both passes) | 23.1 |
  | k0175-pthread-1111 (0175, thread) | 35.7 / 32.9 | 34.7 / 32.9 | 29.7 / 19.5 |

  So 0173 gave ~+25-45% over 0172, not 2.5x. The 64-74 "fps" in the entries below are the inflated scores.
- With the thread: windows exactly 5.0 s, vsync late max 1.0-1.3 ms while swiping (a single 3.8 / 11.5 / 29 ms tick
  now and then when idle), 0-1 late ticks per window. But a panel copy still takes 8-37 ms while swiping (it
  waits in present_wait_locked for the present two back, behind backboardd's 27 ms jobs), so 55-228 kicks per
  5 s find the thread busy: new pictures on the panel stay at ~20-30 per second either way.
- Reading: 0175 makes vsync honest and regular (CA gets on-time vsyncs and correct timestamps), but the frame
  rate is set by the GPU: ~33 swaps/s, ~20-30 panel copies/s, both limited by the 27 ms backboardd job. The
  panel copies' irregular spacing (whenever the copy thread gets through the GPU queue) is still uneven.
  Whether it LOOKS smoother: not judged yet (needs eyes on the phone).
- Next: the GPU job itself (entry below: per-row 2D copies + 22-bin GMEM pass) is what limits the rate. 0175 stays
  the recommended kernel (regular vsync, no loss). Possible kernel follow-up: give the panel copy its own GPU
  priority/ring so it does not queue behind the Metal clients' jobs.

### HOTDOGB: choppy frames = one 27 ms GPU job per frame + the panel blit queued behind it (analysis of k0173-perf-1111, 2026-11-11)
- Observed in the "SWEET GPU: 5s" / "5s jobs" lines while swiping (0173, 61.8 "fps"):
  - backboardd's GPU jobs are two kinds: ~200-230 per 5 s take 1-4 ms, ~110-140 per 5 s take >= 16 ms. The long
    one is always the same shape: 16 IBs, 203286-203375 dw, 27.1-27.7 ms; IB 1 = 17.5 ms, IB 11 = 7.5 ms, IB 3 =
    1.1 ms, IB 13 = 0.8 ms (the rest < 0.2 ms). Same in k0172-swipe-nolog-nogov-1110 (GPU pinned at 499 MHz).
  - backboardd GPU time 3.0-4.0 s of every 5 s (61-82% busy); the present blits themselves 99-135 ms per 5 s.
  - So ~22-28 heavy frames per second at 27 ms each fill the GPU: the swap count (53-74 per second) is not the
    number of new pictures.
  - Panel: 178-198 panel copies per 5 s (36-40 per second) for 267-368 swaps: 30-45% of the swaps never reach the
    panel. "avg copy" 23-34 ms and "present: GPU blit 15512 / 27009 us": the blit runs on the vsync timer
    (AppleCLCD::vsyncFired -> present -> sweet_gpu_present) and present_wait_locked waits there for the blit two
    presents back, which sits in the ring behind a 27 ms job. While it waits, the vsync notifications CA paces on
    are late (the timer skips to now + period).
  - Other jobs by shape (idle/animation frames): 22516 dw = 3.9 ms, 55-60k dw = 7.2-7.6 ms, 82926 dw = 13.8 ms:
    GPU time grows with the command stream length (~0.13-0.17 us per dw).
- Reading (guess): the page-swipe frame is drawn in passes that cost 17.5 + 7.5 ms on the GPU (what they are is
  not known: wallpaper/dock/folder blur, tile load/store, sysmem vs GMEM, or per-draw overhead from metal2vulkan).
  That caps new pictures at ~37 per second, and the late vsync + dropped panel copies make them uneven.
- Kernel 0174 = 0173 + "gpuhist [MIN_DW] [JOBS]" on the command server: packet histogram of the next big v2 GPU
  jobs (per top-level IB: dwords, draws, nested IBs, events, CP_SET_MARKER modes; whole job: CP opcodes, events,
  most written registers, GRAS_SC_BIN_CNTL / RB_CNTL / RB_RESOLVE_OPERATION first/last). It maps the buffer
  holding each IB for the read (md->map). The first version read pool memory with readBytes and killed the phone
  (k0174-gpuhist-1111); the second found no buffers (k0174-gpuhist2-1111). Third works: k0174-gpuhist3-1111b.
- Observed, 3 heavy backboardd jobs while swiping (179-203k dw, 16 IBs; decoded with the msm XML in
  ~/src/wsl-kernel-6.18.54.1/drivers/gpu/drm/msm/registers/adreno/):
  - CP_BLIT (op 0x2c, the 2D engine): 9204 / 10513 / 12033 per job. GRAS_A2D_SRC_XMIN, GRAS_A2D_DEST_TL and
    TPL1_A2D_SRC_TEXTURE_INFO are each written once per blit (~9-12k), RB_A2D_BLT_CNTL / RB_A2D_DEST_BUFFER_INFO
    only 128-157 times: ~150 copy setups with ~70 blits each, a new source each time, one destination.
  - The 17.5 ms IB (137410 dw, 274-278k with nested) is a binned (GMEM) pass: markers 2 (BIN_VISIBILITY) then
    4 5 6 7 (RENDER_START, END_OF_DRAWS, RESOLVE, RENDER_END) over and over (the 64-char marker buffer filled at
    15+ bins). GRAS_SC_BIN_CNTL 0x40e12 = bins 576 x 224 -> 2 x 11 = 22 bins over 1125 x 2436. 1702-1748 draws
    counted with the per-bin replays (68 nested IBs = draw IBs per bin), 157-160 resolves (RB_RESOLVE_*, BLIT
    event 0x1e) = ~7 per bin.
  - Second big IB (39-63k dw, 93-154k with nested): 736-1288 draws, 68 nested IBs, 281-371 events.
  - Per job: CP_SET_DRAW_STATE 6166-8685 (45-61k groups), CP_DRAW_INDX_OFFSET 2092-3015, CACHE_INVALIDATE 160-194.
- Reading (inferred from the packet pattern, Turnip source not on this PC): one destination + a new 2D source per
  blit is Turnip's vkCmdCopyBufferToImage fallback when the buffer address or row pitch is not 64-byte aligned:
  it copies ONE ROW PER BLIT. 9-12k rows per frame ~ 4-5 surfaces of 2436 rows. A 1125-pixel BGRA row is 4500 bytes
  (4500 % 64 = 20). So: full-screen CPU/IOSurface images are re-uploaded every frame, row by row; the likely
  source is metal2vulkan's buffer/IOSurface-backed textures (a VkImage shadow refreshed by a copy before use).
  Next to that, the binned pass with 22 small bins replays every draw 22 times (bin size points at ~8 bytes per
  pixel of attachments).
- Next (userland; needs the Turnip / metal2vulkan trees from the Mac build: tools/build-turnip-ios.sh,
  tools/build-m2v-bridge.sh): (1) confirm in metal2vulkan where buffer-backed / IOSurface textures get copied
  and skip the copy when the surface did not change, or sample the linear memory directly; (2) make the copy
  aligned (64-byte bytesPerRow for the surfaces CA creates, IOSurfaceAlignProperty) so Turnip takes one blit per
  image; (3) see whether the main pass can render direct (sysmem) instead of 22 GMEM bins (TU_DEBUG=sysmem test).
  Kernel side, independent: move the panel blit off the vsync timer so vsync stays regular.

### HOTDOGB: 0173 idle warms up with the CPUs nearly idle: not a CPU-load problem any more (run k0173-perf-1111, 2026-11-11)
- Kernel 0173, usual args + sweettop=30, 1500 s. Unlocked at the start, then untouched on the home screen for
  ~17 min, display on the whole time (no auto-lock while unlocked; there is no remote power key, so NO screen-off
  reading in this run).
- Observed idle (22 reports from 250 s on): 0.27-0.42 cores busy. Summed top: audiod (one pri 4 thread, mostly
  sys, ~6-9% all the time), wland 4-6%, wifid, kernel_task, SpringBoard, btd9. Governor at the floor: pwrcl
  ~300-500 MHz, perfcl 710, prime 825; GPU 257 MHz, 0-5% busy.
- Battery gauge temp over the run, USB in, charging at a constant 480 mA: 23.6 C (120 s) -> 26.8 (280) -> 28.9
  (480) -> 30.2 (680) -> 31.2 (900) -> 31.9 (1100), flattening. Yesterday's idle run sat at ~33 C.
- Reading (guess): ~9-10 C over the start temperature at 0.3 cores busy means the power goes somewhere other than
  CPU load. Not measured, candidates: charging itself (480 mA), cores only in WFI (no PSCI CPU_SUSPEND / power
  collapse), GPU rail + GMU always on, Wi-Fi without power save (wland), Bluetooth, SLPI, UFS never hibernating,
  panel at 90 Hz, and whether "display off" stops the DSI/MDSS clocks or only darkens the panel. The system's own
  draw cannot be read while USB feeds it (the input current is unknown).
- In use, swipebench 30 s + top 20 + sample: 61.8 fps (53.4 58.4 73.6), GPU busy 74% at 499 MHz (governor at the
  top, busiest 100 ms 100%), CPU 1.57 cores; coreanimation thread 63% on cpu 4. New observation:
  "SWEET FB: 5s: 292 swaps (58.4 fps), 190 panel copies, avg copy 23927 us" and "368 swaps, 198 copies, avg copy
  34485 us" - a panel copy takes 24-34 ms while swiping (1-2 ms when idle), against 11 ms per frame at 90 Hz.
  Not looked at yet what the copy waits for. backboardd's hot user PCs are 0x1c5150ec8-0x1c51532a0 (shared cache,
  not symbolised), under 0x1c5162b18 / 0x1d46da72c.
- Next: (1) screen-off reading: boot, press the power key by hand, leave 10 min, read CPU TOP + temp; same again
  with sweetcharge=0 to take the charging heat out. (2) Why panel copies take 24-34 ms under load. (3) Symbolise
  the backboardd hot spot. (4) Idle power states (CPU_SUSPEND, GPU off when idle, Wi-Fi power save).

### HOTDOGB: kernel 0173 big cores first: swipebench 64 / 68 fps (was 27 / 23) (run k0173-swipe-big-1110, 2026-11-10) - CORRECTED: real 33.6 / 33.6 fps, see the 0175 entry
- Cause found in the source: the 8 cores are one processor set (MAX_CPU_CLUSTERS 1, clutch scheduler, no AMP) and
  choose_processor took the lowest-numbered idle core = cpu0-3, the A55s.
- 0173 (WSL build, sha256 fd933acb...): a timeshare thread at priority >= `sweetbig` (default 37; 0 = off) takes an
  idle core of `sweetbigmask` (default 0xf0), a lower one an idle small core; none of its kind idle = old choice.
  Realtime threads untouched. TOP thread lines end in "cpu N" (core the thread ran on last, at the end of the interval).
- Observed, same args and same procedure as the 0172 runs (oslog job + vmd off, unlock, home screen checked,
  `hotdogb-swipebench.py 30 --no-unlock` twice, `top 15 12` during each):
  - **63.8 fps** (windows 54.6 75.4 61.4) and **67.7 fps** (78.8 79.2 45.0); GPU busy 75% / 78%; backboardd 1533 /
    1612 submits (0172 without the load: 27.0 / 22.8 fps; with the load 34.0 / 36.4).
  - CPU TOP: 1.73 / 1.63 cores busy, per cpu 30 21 16 15 | 43 42 34 24 and 36 25 17 17 | 45 39 34 23.
    backboardd "com.apple.coreanimation" (pri 54) 63% on cpu 7 / cpu 5, SpringBoard main (pri 47) 17% on cpu 6,
    metal2vulkan-queue-wait (pri 46) 13% on cpu 4 / 6; metal2vulkan-handlers (pri 31) 9% on cpu 4 / cpu 0.
  - The "#### BIG CORES FIRST ..." line comes before the live log starts (not seen in live.txt; the kernel string
    check passed and the placement shows it is on).
- Not measured yet: heat and battery current in use against 0172; an idle run on 0173 (expect the same as
  k0172-nolog-1110, the idle work is below priority 37 - not checked); sweetbig=0 on the same kernel as the A/B;
  other thresholds (31 would move default-priority app work to the A76s too).
- During this work the WSL disk went read-only (ext4 "emergency_ro"); `wsl --shutdown` brought it back, but the
  kernel log says "mounting fs with errors, running e2fsck is recommended". That shutdown cut the live recording
  of the user's own run k0172-top-1110-1801 at 18:10:48 (the phone ran on to its timer).

### HOTDOGB: swipebench is SLOWER without the background load: the render thread sits on the A55s (2026-11-10)
- Three runs of kernel 0172, same args, unlock ~22 s after the live log starts (home screen checked: centre pixel
  72b2d2ff), then `hotdogb-swipebench.py 30 --no-unlock` twice. fps / GPU busy / CPU TOP during the swipes:
  - k0172-swipe-nolog-1110 (oslog job + vmd off): 27.0 and 22.8 fps, GPU 41% / 48%; 1.80 cores busy,
    per cpu 49 45 43 33 | 23 19 20 13.
  - k0172-swipe-withlog-1110 (both back on): **34.0 and 36.4 fps**, GPU 54% / 69%; 4.69 cores busy,
    per cpu 93 88 91 90 | 74 69 66 62.
  - k0172-swipe-nolog-nogov-1110 (off, plus sweetcpugov=0 sweetgpugov=0): 28.8 and 25.4 fps, GPU 44% / 53%;
    1.58 cores busy, per cpu 41 50 43 42 | 17 14 8 7.
- So the governors are not what costs the frames (27.0/22.8 -> 28.8/25.4 with them off). In all three the work is the
  same: backboardd "com.apple.coreanimation" (pri 54) 61-73% of a core, SpringBoard main thread 16-21%.
- Reading (inferred from the per-cpu figures; CPU TOP does not say which core a thread ran on): with the phone
  otherwise idle the render thread runs on cpu0-3 (A55); with the junk load filling cpu0-3 it gets pushed to
  cpu4-7 (A76) and paging is ~8 fps faster. The scheduler does not prefer the fast cores.
- Next: (1) CPU TOP: add each thread's time per cluster (or last cpu) to prove it; (2) make the scheduler put
  high-priority / UI threads on cpu4-7 first (how this tree picks a processor is not looked at yet); then repeat
  the no-log swipebench: expect >= the 34-36 fps of the loaded run.
- Phone state after these runs: oslog-reportcrash removed, vmd disabled (as after k0172-nolog-1110).
- The first run's kernel timestamps started at ~11670 s instead of ~35 s (cause not checked; flamereboot still
  worked relative to boot).

### HOTDOGB: HEAT - A/B without the log stream and vmd: 0.45 cores busy instead of 5.1 (run k0172-nolog-1110, 2026-11-10)
- Changed on the phone (setup boot k0172-setup-1110): boot job org.iosonpixel.oslog-reportcrash removed (the only
  oslog job left), com.apple.voicemail.vmd disabled (plist kept in /var/local/launchd-disabled; undo:
  `hotdogb-launchd-job.py enable com.apple.voicemail.vmd`). Same kernel 0172 and args, 660 s, phone untouched.
- Observed, 11 reports: 0.31-0.78 cores busy, mean 0.45 (before: 4.0-6.3, mean 5.1). Top: audiod 10.7% (sys),
  wland 6.8%, kernel_task 4.5%, btd9 3.6%, wifid 2.0%, SpringBoard 1.7%; diagnosticd, oslog, vmd, coreduetd absent.
- The governor now drops: last 30 s pwrcl avg 451 MHz (load 13%), perfcl 724, prime 826 (before: 1785 / 2400 / up to 2956).
- Battery: 32.9 -> 33.3 C and +187..287 mA (charging) over the run; the old run was at 42.9 C and -382 mA by 650 s.
- coreduetd made 122 AKS calls in the whole run (before 7521) and never showed in the top: its loop came with the
  log stream or with use of the phone (the old run was used by hand, this one was not) - not separated yet.
- Still there: respawn churn (285 xpcproxy, BlueTool 21, the three TTS AUs 12 each = 36 CRASH lines each).
- Not measured: the phone in use (scrolling, apps) without the log stream; fps against the old 35 fps figure.
- Trap: `fastboot boot` answered "Failed to load/authenticate boot image: Load Error" for every image, also
  yesterday's: slot a was unbootable with retry count 0. `fastboot set_active a` (retry 7 again) fixed it.

### HOTDOGB: HEAT - CPU top result: our own log stream + vmd + coreduetd keep ~4 cores busy (run k0172-top-1109-2354, 2026-11-10)
- Observed, 15 reports over 900 s (5 s each, every 60 s): 4.0-6.3 cores busy (mean 5.1); cpu0-3 at 60-100% always.
  Average per process over all reports (100% = one core):
  diagnosticd 140% (user 67 sys 73), oslog[76] 99% (one thread, user 46 sys 53), vmd 97% (two pri-4 threads),
  coreduetd 67% (one thread ~84% from ~250 s on), backboardd 31% (10-16% untouched, 70-85% while used),
  launchd 17% (sys 10), biomed 9.5%, SpringBoard 6.7%, kernel_task 4.3%, everything else < 3%.
  The listed processes add up to the per-cpu total within ~0.3 core: interrupt time is not a large hidden part.
- The kernel also flagged them: "caught burning CPU" for oslog (128 s), vmd (343 s), coreduetd (436 s).
- oslog is our boot job (`oslog -l NAME`): -l asks diagnosticd for info+debug of EVERY process and oslog filters by
  name only after receiving each event (phone/tools/oslog.m), so the pair costs ~2.4 cores whatever is filtered.
- coreduetd: 7521 of the log's 10151 "SWEET AKS" lines, all `sel 17 ... out 0/4096 -> 0x0`, two per ~150 ms.
- Respawns in 900 s: 340 xpcproxy, wifip2pd 77, BlueTool 30 (bluetoothd's 31 s retry), SiriTTS/Kona/MacinTalk AUs
  12 each (36 CRASH lines each), 11 Spotlight extensions 6 each, assetsd 8.
- Not known (guesses): why vmd spins (voicemail daemon with no CommCenter: a reconnect loop?), what coreduetd's
  sel 17 loop is, whether launchd's 17% is those respawns or failed service lookups.
- Next: (1) remove the oslog jobs + disable vmd, reboot, compare CPU TOP and the gauge mA/temperature;
  (2) `flame-cmd.py sample vmd 200 20` and `sample coreduetd 200 20` + sweet-profile.py for their stacks;
  (3) if oslog is needed again: start it by hand with -t SECS, never as a boot job with -l.

### HOTDOGB: HEAT - the governor is not the cause; CPU top to find the busy threads, kernel 0172 (2026-11-09) - RUN, see above
- Observed with 0170 (run k0170-gov-1109-2238, 900 s) and 0171 (k0171-bohandle-1109-2258): the CPU governor hardly
  ever lowers anything - 30 s averages pwrcl 1784 MHz (top 1785), perfcl 2311/2405 (top 2419), prime 2748/2935 (top
  2956), the busiest core of each cluster at 85-100% in nearly every report. The GPU governor works (average 273/298
  MHz, bursts to 499, no "GOVERNOR STOPPED"). Battery gauge 23.45 C -> 45.15 C over the 0170 run.
  So at least one core per cluster is busy almost all the time; which threads is not known.
- 0172 (`xnu/patches/0172-hotdogb-cpu-top.patch`, WSL build work/boot/kernel-0172, sha256 cceaadef...): CPU top.
  Every 60 s (`sweettop=N`, 0 = off) a 5 s measurement: "#### CPU TOP: 5000 ms, N threads, X cores busy; per cpu %: ..."
  then the 12 busiest processes ("TOP proc name[pid] N% (user N sys N) N threads") and threads ("TOP thread name[pid]
  tid N "thread name" N% (user N sys N) pri N [cont 0x...]"), "TOP --- done". 100% = one core. On demand:
  `python tools\flame-cmd.py top [SECS] [N]`. cont = unslid continuation of a blocked kernel thread.
- Next, depending on what tops the list: a user process -> `flame-cmd.py sample NAME 200 20` + tools/sweet-profile.py
  (stacks; high "sys" = time in the kernel on its behalf, e.g. the GPU fence wait spin); kernel_task threads without
  a name -> name the sweet_* driver threads, or add a per-CPU pc sampler.
- Limits: a thread that never leaves its core is charged at interrupts/switches, so a 5 s figure can be a little off;
  time in interrupt handlers is not attributed to a thread ("per cpu" higher than the threads' sum = that, or stale
  figures); idle threads are left out.
- Trap met while building: mac-build.sh tries a forward dry-run first, and a patch that only adds code applies a
  second time to a tree that already has it (0172 doubled; fixed by `patch -R` once). Generate the patch, then
  revert the tree before building, or check for doubles.

### HOTDOGB: DISPLAY FREEZE (two frames blinking, OS alive) - kernel 0171 (2026-11-09) - BUILT, NOT RUN YET
- Seen: k0170-gov-1109-2238 at 534 s (also k0165-bt-1, k0165-bt-11, k0165-gps-1109-1901; same as the 0160/0161
  freezes of the 0162 entry). The picture alternates between two old frames; touch, brightness, the log go on.
- In the log: `SWEET MTL: v2 backboardd submit refused: IB 12 of 14 iova 0x15570000 ... (outside the window)` +
  `method 5 -> 0xe00002c2`. From then on "SWEET GPU: 5s" shows only `present` jobs (no backboardd IBs) while
  "SWEET FB" still counts swaps: Turnip called the device lost, backboardd keeps swapping its last two surfaces.
- Likely cause (read from UniXNU's turnip-darwin-kmd.patch, NOT proven on the phone): the bad "iova" is
  0 + the low 32 bits of a CPU pointer (0x1_15570000), i.e. a command-buffer BO whose `tu_bo` struct is zero
  (iova 0, map NULL) while in use. darwin_bo_finish zeroes the handle-indexed struct AFTER the kernel freed the
  handle; the kernel handed out the first free slot, so another thread's BoAlloc got the same handle and filled
  the struct, which the first thread then zeroed. Every case came in a burst of allocs/frees (pool MB jumping).
- 0171 (`xnu/patches/0171-hotdogb-gpu-bo-handle-rotate.patch`): handles go round the table (no immediate reuse);
  milestone `#### GPU BUFFER HANDLES ROTATE (NO IMMEDIATE REUSE)`. A refusal now also logs
  `refused IB: handle N is live/not a buffer, kernel iova ...`: "live" with a window iova confirms the cause.
- The real fix is in Turnip (zero the struct before the handle is released): needs the Mesa tree (not on the PC).
- Once frozen: nothing recovers backboardd's device; restart backboardd or reboot.

### HOTDOGB: HEAT - CPU and GPU frequency governors, kernel 0170 (2026-11-09) - BUILT, NOT RUN YET
- Why the phone gets warm (from the code): 0086 pins all three OSM clusters at their top all-core frequency and
  0107 keeps the GPU at 499 MHz. Idle CPUs do sit in WFI (idle_enable, wfi=1), but nothing deeper, and the GPU
  never powers down. Not measured: how much each part contributes.
- 0170 (`xnu/patches/0170-hotdogb-cpu-gpu-governor.patch`, WSL build work/boot/kernel-0170, sha256 cd614cc4...):
  - CPU (bsd/dev/sweet_cpufreq.c + sweet_processor_idle_time() in osfmk/kern/processor.c): a thread every 40 ms;
    cluster load = its busiest CPU (idle time from recount); wanted frequency = current * load / 70, lowest table
    entry at or above it; 90%+ load -> top at once; down after 3 intervals. Boot starts at the top as before.
    `sweetcpugov=0` = 0086 behaviour; `flamecpufreq=N` still caps at N MHz, `=0` leaves ABL's setting.
  - GPU (bsd/dev/sweet_gpu.c, a6_set_perf() in sweet_a6xx.c): a thread every 100 ms; load = GPU time of the submits
    that landed; levels 1 (257 MHz) .. sweetgpufreq (default 4 = 499); wanted = current * load / 40, 50%+ -> top;
    down after 5 intervals with nothing queued. The run-time vote is the power-up HFI message 30 again;
    a vote without ack stops the governor ("#### GPU GOVERNOR STOPPED"). `sweetgpugov=0` = fixed level.
- Log: "#### CPU GOVERNOR RUNNING", "#### GPU GOVERNOR RUNNING", every 30 s "SWEET CPUFREQ: 30s: ..." and
  "SWEET GPU: gov 30s: ...".
- Unverified: that the GMU acks perf votes while the GPU is working (only ever sent once, at power-up); that the
  OSM takes frequent index writes without trouble; whether 100 ms is quick enough for the first frames of a scroll
  (GPU jobs of 16-37 ms at 499 MHz were already common in k0169 runs).
- Not done, would save more: GPU power collapse when idle (GMU IFPC/slumber), CPU core power-down through PSCI
  CPU_SUSPEND, sleeping instead of spinning in the GPU fence wait (sweetgpusleep, off since 0161).

### BUILD: kernels build in WSL, no Mac (2026-11-09) - WSL KERNEL BOOTS
- `wsl.exe -- bash /mnt/c/Users/Peketr/ios-flame/src/tools/wsl-build.sh` (PowerShell) = tools/mac-build.sh on
  ~/xnu-linux: applied 0166-0169, all mac-build-check strings ok, work/out/kernel-0169.sweet (sha256 e53c4bc2...).
  Setup and details: HOTDOGB.md "Building in WSL".
- Observed vs the Mac builds of 0165 and 0169: identical kalloc_type views (1554), kalloc_var views (465), global
  symbols (24431 for 0169) and MIG sources (79 files); __TEXT_EXEC,__text 3.8% smaller (upstream clang 21.1.8).
- Booted (run k0169-wsl-1109-2208, work/boot/kernel-0169-wsl, same args as the Mac run k0169-charge-1109-2146):
  the same 18 milestone lines in the same order with the same spacing (HID opened -> TOUCHSCREEN RUNNING 16.6 s in
  both), battery gauge BQ27411 81%, no panic; process crashes are a subset of the Mac run's (driverkitd, assetsd,
  FitnessWidget, Spotlight extensions). Touch works too (FIRST TOUCH: FINGER 0 AT 574,2397, sent to iOS HID).
- work/boot/kernel-0169 is still the Mac build; tools/hotdogb-run-current.sh boots kernel-0169-wsl.

### HOTDOGB: MOTION SENSORS step 1 - sensord: SLPI boot, FastRPC file server, SSC client (2026-11-09) - NOT RUN YET
- New userland daemon phone/sensors/sensord (build: phone/sensors/build.sh on the Mac; install:
  `python tools/hotdogb-sensors-install.py`, boot job org.iosonpixel.sensord = `sensord auto`, log /var/local/sensord.log).
  No kernel change: runs on 0167. Covers plan steps 1-3 of "Accel/gyro plan" below; the iOS side (step 4) is next.
- From the stock DT (dtb_idx 8) and dtbo overlay 3 (observed): qcom,ssc@5c00000 PAS id 12, firmware "slpi", LCX/LMX
  (pm8150_l8/l4 level) votes, signal-aop; smp2p-dsps qcom,smem <481 430> (in/out), APCS IPC bit 26, entries
  master-kernel / slave-kernel / sleepstate; glink dsps remote-pid 3, IPC bit 24, channels IPCRTR,
  fastrpcglink-apps-dsp, glink_ssr; sdsprpc compute banks SMMU 0x5a1-0x5a3; pil_slpi_mem 0x98100000 + 0x1400000
  (overlay fragment@87; base DTB 0x97300000 overlaps the moved modem region). slpi.mdt is linked at 0x97300000-0x98700000
  and relocatable (flag bit 27): loaded at 0x98100000. slpir.jsn/slpius.jsn: msm/slpi/root_pd + sensor_pd, instance 90.
- Sensor configs: the 7T's vendor_a.img /etc/sensors (extracted to ../hotdogb/sensors/etc, not in git): lsm6dsm_0,
  msmnile_lsm6dsm (bus_type 1, bus_instance 2, irq 132, rails /pmic/client/sensor_vdd(io)), mmc5603nj_0,
  msmnile_mmc5603nj, sx932x, tcs3701, ...; sns_reg_config (version 8) selects on /sys/devices/soc0 hw_platform +
  soc_id and writes its registry to /mnt/vendor/persist/sensors/registry/registry. The installer serves soc_id 339 +
  hw_platform MTP (guess: what OnePlus' SM8150 reports; every msmnile_* file accepts that pair).
- sensord: PAS boot as audiod (+ relocation); SMP2P out with sleepstate bit 12 = apps awake (downstream
  smp2p_sleepstate.c PROC_AWAKE_ID, from memory); GLINK host 3; QRTR node learned from the SLPI's HELLO
  (link.h QRTR_NODE_REMOTE_VAR); pd-mapper (pdmapper.c PDMAPPER_SLPI); servreg listener for msm/slpi/sensor_pd.
- FastRPC (phone/sensors/fastrpc.c) without a kernel driver, after Linux drivers/misc/fastrpc.c + linux-msm/hexagonrpc:
  40-byte fastrpc_msg on fastrpcglink-apps-dsp, payload at 0x9e200000 (+1 MB, top of cont_splash below audiod's block;
  0xe1000000+ is the GPU pool) with DSP addresses 1:PA (context bank 1 in bits 32+, as downstream adsprpc), streams
  0x5a0/mask 3 on a translation-off bank. Attach = handle 1 method 0 (pd 2 = sensors PD in ctx), then remotectl open
  "adsp_default_listener" + register, adsp_listener (handle 3) init2 + next2 loop. Reverse calls: remotectl (0),
  apps_std (1): fopen/fopen_with_env/fread/fwrite/fseek/ftell/flen/fclose/fflush/fsync/fileExists/fremove/
  opendir/readdir/closedir/mkdir/stat/ftrunc/frename, writes allowed (the registry); apps_mem (2) is not implemented
  (logged as "#### FASTRPC REVERSE CALL NOT IMPLEMENTED"). Paths: /mnt/vendor/persist/sensors -> /var/local/sensors/persist,
  /vendor/etc/sensors -> .../etc, /sys/devices/soc0 -> .../socinfo, anything else -> .../other (logged).
- SSC client (phone/sensors/ssc.c): QMI 400 request 0x20 (TLV 1 = u16 length + protobuf, 0x10 jumbo), reports 0x21/0x22;
  libssc's protobuf layout; SUID lookup (suid 0xabab...) of the default "accel", "gyro", "mag", enable at 50/50/25 Hz
  (msg 513), samples (msg 1025) -> /var/local/imu.txt (`t= accel=x,y,z gyro=... mag=... n=`, 5 Hz). Units (libssc,
  unverified): m/s^2, rad/s, uT; gyro/mag uncalibrated (no gyro_cal/mag_cal yet).
- Host-tested (phone/sensors/test_sensord.c, WSL cc): listener blob encode/decode, invoke payload layout, the SSC
  request bytes ("0a 12 ..." as libssc's notes show). Nothing of it ran on the phone yet.
- Unverified guesses to check first in the log: the SLPI's QRTR node, whether attach needs pid 0, the fileExists
  out size (1 byte), whether the registry wants apps_mem, the sleepstate bit.

### HOTDOGB: BATTERY FROM THE REAL FUEL GAUGE - kernels 0166-0167 (2026-11-09) - WORKS with 0167
- Before: SweetBattery on the 7T was the ios-sweet (Redmi Note 10, PM6150 QG) driver. Without `sweetbattery=1` it
  publishes a fixed 100% / AC; with it, it reads PM6150 registers the 7T does not have -> the same fallback.
  CurrentCapacity/MaxCapacity were mAh of a 5020 mAh battery.
- The 7T's SOC source (stock Android, observed): the OPLUS charger stack uses an external TI gauge,
  `bq27541-battery 3-0055: DEVICE_TYPE is 0x421` = **bq27411** family, on `/soc/i2c@a80000` (= qupv3_se8_i2c,
  QUPv3 1 SE0; stock dtbo fragment@69) at 0x55. PM8150B's FG gen4 is `oem,use_external_fg`. Design 3800 mAh
  (`qcom,batt_capacity_mah`); stock log: 100%, 4359 mV, 29.9 C, RM/FCC 3017 mAh.
- 0166: GENI I2C FIFO mode on SE8 (same init as the S3706 driver), clocks voted (GCC 0x5200c bits 18-22, RCG
  0x18148 XO/1, branches 0x18004/0x18008/0x18144), Control(DEVICE_TYPE) then standard commands SOC 0x1c, voltage
  0x04, AverageCurrent 0x10, Flags 0x06, RM 0x0c, FCC 0x0e, temperature 0x02 (bq27541 map if the type is 0x0541).
  Charger: PM8150B SMB5 at SPMI SID 2 (USBIN 0x1310, path 0x110b, CHGR 0x1006/0x1007/0x100d, the PM6150 decoding).
  Guesses to confirm on the first run: XBL left SE8 with I2C firmware and FIFO mode allowed; pins 88/89 (stock DT:
  function qup8, 2 mA, no pull in the active state) already muxed (the driver does not touch TLMM); the SID 2 SMB5 APIDs are visible to the observer.
- **Run k0166-batt-1109-2018**: clocks and SE fine - GCC vote 0x40243c0 -> 0x47e43c0 (wrap1 bits 18-22 were OFF:
  XBL never used SE8), RCG DFS on (0x21, level 0 = XO/1), SE fw 0x303 (I2C), FIFO allowed, 32-bit x16 FIFOs, status 0.
  Then `#### BATTERY GAUGE FAILED: NO ANSWER AT 0x55 (TIMEOUT)`: the first write never finished. Likely cause: pins
  88/89 still GPIOs (nobody muxed them; Linux's pinctrl does it at probe).
- **0167**: muxes GPIO 88/89 to qup8 before the first transfer (pinctrl-sm8150.c: NORTH tile 0x03900000, funcs
  {gpio, tsif1, qup8, ...} -> mux 2; 2 mA; pull-up), logs their cfg and SDA/SCL levels before and after
  (`#### BATTERY I2C PINS 88/89 SET TO QUP8 (cfg, SDA, SCL)`: both should read 1 = idle bus with pull-ups), and
  logs irq/status/FIFO counts on an I2C timeout.
- **Run k0167-batt-1109-2024: WORKS.** gpio88/89 were cfg 0x1 (GPIO, pull-down) reading 1 (board pull-ups); after
  the mux 0xb/0xb, SDA 1 SCL 1. `#### BATTERY GAUGE UP: BQ27411 (DEVICE TYPE 0x421)` 2 ms later, then
  `#### FIRST BATTERY READING: 37% 3796 MV -150 MA 43.2 C 1069/2916 MAH` (RM/FCC = 36.7%, consistent).
  SMB5 over SID 2 answers: usb=0x10 (plugged in), path=0x95, **chg=0x47 -> state 7 = charging DISABLED**: on USB but
  draining 150 mA, gauge flags 0x109 (DSG, BAT_DET, CHG allowed). iOS gets external=1 charging=0 (correct). Open:
  why SMB5 disables charging under iOS (guess: the SMB5 charger watchdog / no software enabling it, or the 43 C
  battery near the 45 C warm limit) - the phone does not charge on iOS. Not checked yet: the status bar number.
- **0168** (not run yet): read-only SMB5 register dump (`#### CHARGER REGISTER DUMP (...)` + `SWEET SMB5 xxyy: 16 bytes`) at
  the first reading and on each plug/unplug, to decode the state-7 cause against smb5-reg.h.
- **Run k0168-sensors-chg-1109-2031 (dump at the first reading; decoded with LineageOS sm8150 smb5-reg.h)**:
  37%, 3794 mV, -105 mA, **47.3 C**. Charging is off **by software command**: CHARGING_ENABLE_CMD 0x1042 = 0x00
  (CHGR_CFG2 0x1051 = 0x2c: enable source = command, inhibit off). Everything else would allow charging: USB
  SDP (0x1308 = 0x01, APSD done), not suspended (0x1340 = 0, power path 0x95 = USBIN valid, not suspended), ICL
  0x20 (1.6 A cfg) and AICL done, no JEITA/temperature limit (0x100d = 0, MISC 0x1606 = 0x02), charger watchdog
  off (WD_CFG 0x1651 = 0, bite does not disable charging). STATUS_5 0x100b = 0x80 (trickle/pre/full-on all off),
  STATUS_2 0x1007 = 0x08 (bit 3 not in the header). FCC 0x1061 = 0x0a (500 mA), FV 0x1070 = 0x4d.
  Under Android the OPLUS driver sets the enable bit and does the thermal policy (warm > 45 C: 4.13 V / 1050 mA,
  hot > 50 C: stop). Fix needs one SPMI **write** (0x1042 bit 0) plus a temperature gate; not done yet.
- **0169**: charging on through the one PMIC write (0x1042 bit 0, apps-owned channel, read back),
  gated on the gauge temperature (on 3-43 C, off > 45 C / < 0 C / no data); FCC (500 mA) and FV untouched.
  `sweetcharge=0` = no writes. Same args as 0168, USB to the PC.
  - k0169-charge-1109-2059 (44.2 C, 34%): `#### CHARGING HELD OFF: 43-45 C BAND` - the gate works as written.
  - k0169-charge-1109-2143 / -2146 (29-33 C, 82%): **the write works**: `charge enable 1 -> APID 265 status 0x1
    readback 0x1 rc 0`, `#### CHARGING ENABLED`, charger state 0x47 -> 0x03 (FULLON), iOS gets charging=1.
    But the battery still discharges: gauge -354 mA (2143), -301 / -296 mA (2146), 2417 -> 2414 mAh in 40 s.
  - Open: why the net current stays negative on the PC port (APSD = SDP; USBIN_ICL_OPTIONS 0x1366 = 0x03,
    USBIN_CURRENT_LIMIT_CFG 0x1370 = 0x20, ICL status 0x1106-0x1109 = 0x20; the dump is from before the enable).
    Guess, not checked: the real input is ~500 mA (SDP) and the load with the GUI is above it. Next: same run on a
    wall charger (DCP), and a dump after the enable. Both 0169 GUI runs' live logs end at 100-136 s (cause not checked).
- Milestones: `#### STARTING BATTERY GAUGE`, `#### BATTERY GAUGE UP`, `#### FIRST BATTERY READING`, `#### CHARGER
  CONNECTED/DISCONNECTED`, `#### BATTERY GAUGE FAILED: <why>` (stops cleanly; static 100% stays).

### HOTDOGB: CELLULAR - how iOS reaches a baseband, and step 1 (CommCenter back) (2026-11-09)
- iOS stack (20H392): CommCenter (CoreTelephony/Support) speaks **QMI** itself (qmi::Client, QMIClientPool, "Unified
  QMUX v2"), over QMUX channels qmux1..5 + qmux_control. AppleBasebandManager (in CommCenter/abm-helper) powers and
  boots the baseband through the AppleBaseband / AppleBasebandPCIControl kernel drivers, BBUpdater loads its firmware;
  transports in the cache: TelephonyBasebandPCITransport (iPhone X: MobileGestalt baseband 'mav17' = Qualcomm over
  PCIe), TelephonyBasebandUSBTransport (HSIC), UART, plain FD/file (TelephonyUtilTransportCreateWithFD).
- Our modem already answers QMI over QRTR (wland: WDS 0x1, DMS 0x2, NAS 0x3, WMS 0x5, VOICE 0x9, UIM 0xb, LOC 0x10, ...).
  Plan: (1) see what CommCenter does here; (2) give it a transport (kernel IOService it opens, or an FD/UART path)
  and a userland bridge QMUX <-> QRTR (QMUX CTL client ids <-> QRTR ports; the QMI SDU is the same); (3) skip/fake
  the boot (the modem is already up from wland: AppleBasebandManager's power/boot/BBUpdater steps must be answered,
  not done); (4) answer Apple vendor QMI messages the OnePlus firmware lacks (as btd does for Broadcom HCI);
  (5) SIM/registration (UIM/NAS), then data (WDS + the IPA data path: another driver), SMS, voice last (audio).
- launchd picks the CommCenter job by device tree: com.apple.CommCenter.plist `_LimitLoadToDeviceTree baseband`,
  com.apple.CommCenter-noncellular.plist `_LimitLoadFromDeviceTree baseband` (same binary). Our ADT has no baseband
  node; sweet-root-tweaks removes both (label com.apple.CommCenter) since it blocked Setup.
- **Step 1 (on the phone, not run yet)**: the non-cellular job restored (`hotdogb-launchd-job.py enable
  com.apple.CommCenter`, entry from the root's plist in /var/local/launchd-disabled) + job oslog-commcenter
  (`oslog -l CommCenter` -> /var/local/oslog-commcenter.log). Expect: CoreTelephony answers (Settings > Privacy shows
  Location Services, the getSubscriptionInfo check below) - or the old crash loop. Undo:
  `hotdogb-launchd-job.py disable com.apple.CommCenter` + `remove oslog-commcenter`.
- **Step 1 result (run k0165-cc-1109-1919)**: CommCenter starts in **cellular** mode even under the non-cellular job
  (MobileGestalt says iPhone with a 'mav17' baseband): "Using Baseband Command Driver", "Could not find AppleBaseband
  service", "TelephonyBasebandController: Failed to find AppleBaseband service, registering for matching notification",
  "Baseband Ready: False", publishes a slot-one subscription, then is gone ~6 s after start - 12 times in 2 min, no exit
  or error line (last line always "(ApplePDPHelper) Could not find AppleIPAppender service"). Not jetsam as far as the
  live log shows (4 ReportMemoryException in the whole boot; D22 limits: 36 MB active soft, 20 MB inactive hard).
  Cause unknown: job oslog-reportcrash added (`oslog -l ReportCrash`) for the next CommCenter run. The jobs were
  removed again afterwards (undo). What cellular needs first is clear: an `AppleBaseband` IOService (kernel) for
  TelephonyBasebandController to match, then the PCI transport/QMUX behind it.
- **GPS run 2**: same as run 1 - no engine-state or fix-session indication at all, the $PQWP1 sentence frozen (same
  time every time, 9999 km uncertainty): the engine never starts. gnss.c now also reads + clears the **engine lock**
  (QMI_LOC_GET/SET_ENGINE_LOCK 0x3b/0x3a, none = 1; Android clears it at start), sets the NMEA types (0x3e: GGA RMC GSV
  GSA VTG GLGSV GNGSA) before the operation mode, logs those answers and a position-report status histogram.
- **GPS run 3 (k0165-cc-1109-1931): THE GPS ENGINE RUNS.** Engine lock was **4 (ALL locked)**; cleared (status 0), NMEA
  types + standalone ok, "fix session STARTED", "engine ON", standard NMEA every second (GGA/RMC/GSA/GSV/VTG). In ~110 s
  (indoors) it tracked one GPS satellite (PRN 21, C/N0 24-29 dB-Hz), no GLONASS, no fix (GGA quality 0, RMC V).
  The "#### FIRST GPS FIX" it printed was wrong: in-progress reports (status 1) carry lat/lon 0/0 without an
  accuracy TLV; gnss.c now ignores 0/0 and status-1 reports without a horizontal uncertainty (host-tested). The
  gps.fix left on the phone from that run holds 0/0. Next: a longer run outdoors; if it stays at 0-1 satellites,
  inject UTC time (0x38) and check the GNSS antenna/LNA path.
- **GPS run 4 (k0165-cc-1109-1942, 900 s, partly outdoors): FIRST REAL GPS FIX.** Engine lock read back as 1 (none:
  the clear persists in the modem). ~12 min with 2-5 satellites in view (GPS + GLONASS, none used), then a coarse
  status-1 position (+-10 km, tech 0x1), then 21 in view (GPS 11, GLONASS 10), 4 used, **17 status-0 fixes, +-8.8 m**
  horizontal/vertical, altitude 87 m (gps.fix written). Cold start without time/XTRA: ~13 min. The modem ends and
  restarts the fix session every few seconds (FINISHED/STARTED pairs, harmless so far). Logs pulled with a short
  `sweetwifi=0` boot (wland stays off, so nothing is overwritten; it keeps the previous log as wland.log.prev).
  Next: GPS step 2 (feed locationd); later time injection + XTRA for a faster first fix.
- **GPS step 2 (written, not built/run yet)**: phone/tools/gpsfeed.m (+ gpsfeed-ents.plist:
  com.apple.locationd.simulation, which locationd checks: "unentitled client attempting to simulate location").
  Reads /var/local/gps.fix each second; a fix is new only when its utc_ms advances (wland rewrites the file every
  second with the last fix); good = not 0/0 and status 0, or accuracy 0-5 km. Each new fix: CLSimulationManager
  clearSimulatedLocations / appendSimulatedLocation: (CLLocation, timestamp = phone clock now) / flush, and
  startLocationSimulation once; 15 s without a new fix -> stopLocationSimulation. Milestones `#### GPS FEED STARTED`,
  `#### FIRST LOCATION SENT TO LOCATIOND`, `#### GPS FEED STALE`; never logs coordinates. Parser host-tested.
  Install: flame-files put out/gpsfeed /var/local/bin/gpsfeed 755 + `hotdogb-launchd-job.py add gpsfeed --keep --
  /var/local/bin/gpsfeed` (log /var/local/gpsfeed.log). Apps see it as a simulated location (iOS 15+ flags it).
- ReportCrash log works (oslog-reportcrash): 74 corpses that boot, mostly speech synthesizer AUs (SiriTTS, Kona,
  MacinTalk ~12 each), Spotlight index extensions, driverkitd x4. CommCenter was off.

### HOTDOGB: GPS, accelerometer, gyro - plan, and GPS step 1 (QMI LOC client in wland) (2026-11-09)
- **Hardware** (strings in ../hotdogb/fw/modem-image/image/slpi.b*): IMU = ST **LSM6DSM** (accel + gyro), magnetometer
  MEMSIC **MMC5603**, SAR sx932x; all on the **SLPI** (sensor DSP, sensor_pd per slpius.jsn), not reachable from apps.
  GNSS is in the **modem**, which wland already boots: it announces QMI LOC (service 0x10, instance 2, k0148-wifi-1).
- **GPS step 1 (untested on the phone)**: phone/wifi/gnss.c. 20 s after LOC appears: inform client revision 2, register
  events (position, NMEA, engine, session state), operation mode standalone, START session 1 (periodic 1 s, high
  accuracy, intermediate reports). Writes /var/local/gps.fix (one key=value line: utc_ms lat lon hacc alt vacc speed
  course sats_used sats_view) and /var/local/gps.nmea (last 32 sentences), 1/s. Milestones `#### STARTING GPS`,
  `#### GPS SESSION STARTED`, `#### FIRST GPS NMEA SENTENCE`, `#### FIRST GPS FIX`; a "gnss:" summary every 15 s.
  `WLAND_NO_GPS=1` turns it off. Parsing host-tested with fake indications. Test outdoors: no time/XTRA injection, so
  a cold start takes minutes. If the engine never turns on: check the START response error and the engine indication.
- **GPS run 1 (2026-11-09)**: all four requests ok, "#### GPS SESSION STARTED"; ~1 position report/s without lat/lon,
  and only one proprietary sentence ($PQWP1, about every 10 s), no GSV/GGA (not tested outdoors yet). If no fix
  outdoors: QMI_LOC_SET_NMEA_TYPES (0x3e) for GGA/RMC/GSV, log the report status, inject time (the iOS clock is in 1971).
- **Location Services were off**: /var/root/Library/Preferences/com.apple.locationd.plist had no
  LocationServicesEnabledIn7.0 (Setup Assistant sets it; our roots skip it), so locationd authorizes no client
  (none in clients.plist) and Settings has no per-app Location rows. tools/sweet-root-tweaks.py now sets it (and
  LocationServicesEnabled) on new roots; on the running phone put the edited plist + reboot (backup in /var/local).
- **Settings hides the Location Services row anyway** (after the fix above; disassembly of the 20H392 shared cache,
  extracted on the PC with a small reader + llvm-mc): -[PUIPrivacyController updateLocationServicesVisibility] removes
  the LOCATION specifier unless _cachedLocationServicesAvailableValue is YES, and refreshLocationServicesAvailable sets
  it only after CoreTelephonyClient getSubscriptionInfoWithError: succeeds (then per SIM: carrier bundle value). On an
  error it sets nothing -> nil -> NO. Our roots remove com.apple.CommCenter (no baseband), so the call fails. The
  locationd switch itself is on; apps can still ask. Fix options: a stub CommCenter XPC server that answers "no SIMs",
  or bring CommCenter back now that Setup is done.
- **GPS step 2 (iOS side)**: an injector daemon that reads gps.fix and feeds locationd (candidate: CoreLocation's
  private CLSimulationManager, the path Xcode's location simulation uses - to be checked against this root's locationd).
- **Accel/gyro plan**: (1) SLPI PAS boot (like the ADSP's in audiod: pil_slpi carve-out from dtbo overlay 3, its
  SMP2P/IPC bits, QRTR node of its own); (2) a FastRPC apps_std file server: the SLPI's sns_registry reads
  /vendor/etc/sensors/config/*.json and /mnt/vendor/persist/sensors/registry over FastRPC (what Android's sscrpcd
  serves) - **the same gap that is blocking audio** (audio PD, fastrpcglink-apps-dsp); (3) an SSC client: QMI service
  400 (SNS_CLIENT) carrying sns_client.proto protobuf: SUID lookup for "accel"/"gyro"/"mag", then std sensor config
  (postmarketOS libssc does this); (4) the iOS side: kernel/HID accelerometer + gyro events for backboardd (rotation)
  and CoreMotion. Needs from Android first: /vendor/etc/sensors and /mnt/vendor/persist/sensors (pull with root).

### HOTDOGB: Bluetooth - bluetoothd runs its HCI init on the WCN3998; one refused command left (k0165-bt-7/8, 2026-11-09)
- The MobileGestalt shim never loaded into BlueTool (DYLD_PRINT_LIBRARIES worked, DYLD_INSERT_LIBRARIES was
  dropped silently; also from world-readable /usr/local/lib; k0165-bt-4..6). Instead com.apple.BlueTool runs a
  patched copy /usr/local/bin/BlueTool: `mov w25, #1` at 0x10001607c and 0x1000160b0 (w25 = "ReleaseType is
  Vendor"), re-signed ad hoc on the Mac with --preserve-metadata=entitlements. Kept as
  ../hotdogb/bt/BlueTool.vendor-signed (Apple's file, outside git); hotdogb-bt-install.py installs it. Shim removed.
- k0165-bt-7: BlueTool opens the bridge ("Opening com.apple.uart.bluetooth @ 3000000 baud") and reads the chip
  (LMP 0x0A, subversion 0x02BE, its address); "#### FIRST BLUETOOTH H4 BYTES FROM/TO THE HOST" at 89.9 s.
  bluetoothd then connects ("Booting chipset", "Powering on chipset", "Stack: STACK_OFF -> STACK_BUSY",
  "Searching for transport: H4 / driver: POSIX", "DeviceTree speed = 3000000") and ~30 HCI commands all get status 0.
- It stops at `OI_HCICMD_WriteDefaultErroneousDataReporting (status=628)` -> "HCI initialization failed",
  "Bluetooth error - restarting { reason=108 }"; bluetoothd restarts every ~31 s.
- k0165-bt-9: answering 0x0c5b was not enough - bluetoothd never sends it: it checks the chip's Read Local
  Supported Commands mask first (octet 18 = 0x83, bit 3 clear) and fails locally. btd now also sets the mask bits of
  the commands it answers in the 0x1002 reply (patch_supported_commands; logs "<ctlr supported commands: set ...").
- k0165-bt-10: **"Stack: STACK_BUSY -> STACK_READY", "Stack did start"**. Then Broadcom vendor commands: most refused
  non-fatally (status 701: LE_META_ENA_FEATURES 0xfce9, CHANGE_LE_SETTINGS / SET_MIN_AFH_LE_CHANNELS 0xfce2,
  ENABLE_EXTENDED_DUPLICATE_FILTER 0xfd47, 0xfd48), but BD_VSC_OLYMPIC_ANTENNA_PREF_B (0xfce2, 10 bytes) got no
  return data -> "Invalid data size 0", timeout, restart (reason 3602). btd now answers every host vendor command
  (OGF 0x3f) itself: status 0 + first parameter byte echoed (answer_vendor in btd.c); none reach the chip.
- k0165-bt-11: vendor answers work (LE_META_ENA_FEATURES etc. "completed with status 0"); ANTENNA_PREF_B still
  "Invalid data size 1": _BCM_ByteStreamEventParser wants >1 byte after the status (0x10012617c) -> btd now echoes all
  parameter bytes. Later bluetoothd restarts failed HCI init again: the 0x1002 reply came split across reads, so the
  mask patch missed it -> chip-to-host bytes now go through a packet reassembler (ctlr_feed) and are patched whole.
  Once, between restarts, a bluetoothd ran with STACK_READY and no restart (PowerState 2, manager not powered on).
- k0165-bt-12: ANTENNA_PREF_B passes; next BD_VSC_FACTORY_CAL_READ_TABLE (1 parameter byte) -> "Invalid data size 1",
  restart. Vendor answers are now padded with zeros to >= 32 return bytes.
- k0165-bt-13: calibration passes; locationd/sharingd/FindMy register scans; then LE Set Scan Parameters 0x200b
  -> 0xc (Command Disallowed), "Could not start LE scan ... STATUS 712", restart. Cause: bluetoothd's init sends
  0x204a (an extended-advertising-family command), after which the chip refuses legacy LE commands. btd now answers
  0x204a itself (list size 8).
- Fix: btd answers 0x0c5b itself (local_answers[] in btd.c: Command Complete, status 0; the chip
  never sees it) and logs every refused command ("REFUSED") regardless of the 400-packet limit.
- bluetoothd's own log: job org.iosonpixel.oslog-bluetoothd (`oslog -l bluetoothd`, /var/local/oslog-bluetoothd.log).
- The k0165-bt-2 "freeze": SpringBoard started a shutdown at 298 s (after touches); halt is not implemented, so
  "Halt/Restart Timed Out" panic at 332 s. Not Bluetooth.
- Next: new btd at /var/local/bin/btd8 (installer path bumped each build; never replace a running KeepAlive binary), reboot, read REFUSED lines and bluetoothd's
  log; extend local_answers until "Stack: STACK_BUSY -> STACK_READY". Then Settings > Bluetooth.

### HOTDOGB: Bluetooth stage 2b - plain kernel bridge instead of Skywalk (0165, 2026-11-09)
- k0164-bt-1 (kernel 0164): "bluetoothd skywalk user pipe privilege granted" at 22 s, then "bluetoothd
  privilege 1002 denied" (PRIV_GLOBAL_PROC_INFO); btd claimed the pipe at 71 s; bluetoothd never connected to
  com.apple.uart.sk.bluetooth at all; 0 packets.
- The Galaxy S9 port (Scion-Project PR 171, iOS 16.5, Settings > Bluetooth scanning) needs no Skywalk: bluetoothd's
  launchd job gets BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,4000000 (bare "H4" crashes it), and the
  kernel control "com.apple.uart.bluetooth" is a plain stream socket (termios as sockopts, H4 by read/write; a new
  connect takes over). Their BlueTool runs first with a MobileGestalt ReleaseType="Vendor" shim and a 2-line boot
  script (`device -D -S`, `hci info`); not done here yet - try without it first.
- 0165: sweet_btuart.c is now that control, a bridge: btd connects and claims the chip side (BTUART_CLAIM_CHIP,
  root); any other connect is the host; bytes are copied between the two. btd (pipe.c) is a socket client now; its
  10 s stats show the host unit/pid and drops. The Skywalk pipe code and the .sk. control are gone; 0164 stays
  (harmless).
- On the phone (already set during k0164-bt-1): `hotdogb-launchd-job.py env com.apple.bluetoothd
  BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3000000` (hotdogb-bt-install.py now does it too).
- k0165-bt-2: btd claimed the chip side ("#### BTUART CHIP SIDE UNIT 1 (pid 30)" at 77.6 s); bluetoothd never
  connected: it launches the launchd job com.apple.BlueTool every 31 s (XPC "script" request) and waits for its
  result. BlueTool never connected to the bridge.
- BlueTool (iOS 16.7 disassembly): the override /etc/bluetool/<HWModel>.<boot|init|deepsleep>.script runs only
  when MGCopyAnswer("ReleaseType") is "Vendor" or "Internal" (0x100016054); HWModel is gestalt key
  /YYygAofPDbhrwToVsXdeA = D22AP here (ADT target-sub-type). bluetoothd's HWRepair/BlueToolBTResult override only
  forces a nonzero (failed) result, so it cannot skip BlueTool.
- Done (not run yet): phone/bt/bluetool-shim.c (dylib interposing MGCopyAnswer: ReleaseType = Vendor),
  phone/bt/bluetool/D22AP.{boot,init}.script (`device -D -S`, `hci info`); hotdogb-bt-install.py puts them in
  /var/local/lib and /private/etc/bluetool and sets DYLD_INSERT_LIBRARIES on com.apple.BlueTool (sweet AMFI allows
  DYLD_* for unrestricted processes). btd now lives at /var/local/bin/btd2; the installer leaves it alone if
  unchanged.
- Gotcha: from Git Bash, run hotdogb-launchd-job.py with MSYS_NO_PATHCONV=1 or program paths become
  "C:/Program Files/Git/var/..." (k0165-bt-1 lost btd that way).
- Next: kernel 0165 + new btd (install, THEN reboot), look for "SWEET BTUART: connect unit N (pid <bluetoothd>)",
  "#### FIRST BLUETOOTH H4 BYTES FROM THE HOST" and the HCI log in /var/local/btd.log. If bluetoothd still waits:
  BlueTool job + script, then stage 3 (Broadcom vendor commands).

### HOTDOGB: Bluetooth stage 2 - the pipe to bluetoothd is up, bluetoothd not connected yet (k0163-bt-*, 2026-11-09)
- How iOS 16 bluetoothd finds its controller (usr/sbin/bluetoothd disassembly): it exits at start unless an
  IOService named "bluetooth" (or marconi-bt) exists; transport from that node: transport-encoding (0 USB, 1 H4,
  2 H4DS, 3 H4BC = iPhone X, 4 H5, 6 APPLEBT, 7 PCIe), transport-speed. H4 is NOT a tty on iOS 16: a
  SYSPROTO_CONTROL socket to "com.apple.uart.sk.bluetooth", setsockopt 0x800c5414 (channel spec {1024, 63, 63}),
  getsockopt 0x40105412 (channel UUID), termios as sockopts 0x40487413/0x80487414, then os_channel_create(uuid, 0)
  and H4 bytes in slots. AppleBluetoothModule is only an interest notification on that path (PCIe uses more).
- Done: ADT /arm-io/bluetooth (tools/adt-add-bluetooth.py, applied to work/boot/hotdogb.adt; original
  hotdogb.adt.pre-bluetooth) - bluetoothd now stays up and SpringBoard/backboardd find the node. Kernel 0163
  control; btd `serve` registers an anonymous Skywalk user pipe (the kernel cannot create user pipes) and hands
  over its UUID: k0163-bt-4 `#### BTUART PIPE 7394...`, firmware + reset OK, then idle (0 packets).
- Pitfalls hit: (1) k0163-bt-2 panic `nxctl->nxctl_cred != proc_ucred(kernproc)` (nexus.c:1453, DEVELOPMENT
  assert): launchd's root jobs share the kernel's interned credential; btd now setgroups({0,1}) first.
  (2) k0163-bt-3 launchd panic "unexpected exec of non-platform binary: /var/local/bin/btd": never replace a
  KeepAlive job's binary during the boot that runs it; install, then reboot. (3) One serve bring-up stalled
  mid-rampatch (UART TX held off 10 s); serve now retries once.
- bluetoothd never connected in k0163-bt-4: "SWEET Sandbox: bluetoothd privilege 12000 denied" =
  PRIV_SKYWALK_REGISTER_USER_PIPE (its entitlement). 0164 grants it. Next: kernel 0164 run; expect bluetoothd's
  connect / channel UUID lines in the kernel log and its HCI commands in /var/local/btd.log, then stage 3
  (it knows only Broadcom chipsets; this one is manufacturer 29).

### HOTDOGB: BLUETOOTH RADIO WORKS - btd scans 23 LE + 1 classic device (k0162-bt-3, 2026-11-08)
Stage 1 of Bluetooth (no iOS stack yet): phone/bt/btd (build: phone/bt/build.sh), installed with
`python tools/hotdogb-bt-install.py` (btd -> /var/local/bin, firmware -> /var/local/fw/bt from the bluetooth_a
partition, pulled read-only into ../hotdogb/fw/bluetooth/image, not in git). Boot jobs org.iosonpixel.btd
(`btd auto`: 40 s wait, then scan, power off; log /var/local/btd.log; skip: /var/local/btd.off) and
org.iosonpixel.btd-regs (read-only dump, /var/local/btd-regs.log; can be removed). Kernel 0162, no boot args.
- Hardware (uart.c, userland via /dev/pmem): QUPv3 wrapper 2 SE3 at 0xc8c000 (DT qupv3_se13_4uart), GPIO 43
  CTS / 44 RTS / 45 TX / 46 RX = qup13 (function 1, EAST tile). GCC vote 0x52014 bit 7, RCG 0x1e4d8 (not DFS:
  btd programs M/N/D from the Linux table), QUP hw 0x10030000 -> 32x oversampling, SE fw 0x20b (UART, loaded
  by XBL), FIFO 16/32 words, polled. 3 Mbaud = 96 MHz / 1; 115200 = 7.3728 MHz / 2; 2400 = 7.3728 MHz / 96.
- Supplies ldoa7 1.8 V, ldoc2 1.304 V, ldoc11 3.312 V + rfclka2 (wland votes the same rails).
- WCN3998: pulse 0xC0 at 2400, 0xFC at 115200 -> EDL version 0.1 s later: product 0xa, soc 0x40010224, rom
  0x1001 (btqca's formula would pick "01": btd maps soc ...224 -> crbtfw21/crnv21) -> 3 Mbaud -> rampatch
  229812 bytes in 2.1 s -> NVM (in-band sleep bit and deep sleep cleared, baud byte 14) -> reset ->
  HCI/LMP 10 (5.1), subversion 0x2be, manufacturer 29 (Qualcomm). 0 junk bytes, no UART errors.
- 10 s passive LE scan: 23 devices; 10 s inquiry: 1 classic device. Then power-off pulse.
- Next: stage 2 (kernel: an IOKit `bluetooth` node + Apple BT module stand-in + /dev/uart.bluetooth bridged
  to btd), stage 3 (btd answers bluetoothd's Broadcom vendor commands and reports a BCM chip). Open: BD
  address (the NVM default), IBS/power saving, coexistence with Wi-Fi under load.

### HOTDOGB: GPU variants traced on k0162 (k0162-gt-*, 2026-11-08)
Same setup as the Turnip trace entry below (backboardd only, print_csv + TU_DEBUG perf), 20-swipe benchmark each.
Big full-screen pass (~40 draws) / fps: sweetgpufreq=1 19.5 ms / 16.0; sweetgpufreq=5 9.4 ms / 17.5;
TU_DEBUG=sysmem (default clock) 8.6 ms / **22.4**; TU_DEBUG=noubwc 11.2 ms / 14.5 (keep UBWC on).
Earlier 0161 default-clock GMEM: 10.7 ms / 17. Uploads (copy_buffer_to_image, ~210 us each, 600-850/s) are
15-18% of GPU time in every variant. No v2 submit refusal and no "IB is empty" line in any of the four runs,
so 0162's skip path never ran: the 0160/0161 freezes did not recur here but their cause is not confirmed.
Settings restored afterwards. Next: sysmem without tracing, against the 35 fps untraced baseline.

### HOTDOGB: Turnip trace of backboardd - where a frame's GPU time goes (k0161-1108-trace2, 2026-11-08)
How (one run; undo afterwards): empty /var/local/gputrace-bb.csv and /var/local/turnip-backboardd.log with mode 666
(backboardd's sandbox cannot create files there), /var/local/mtl2-env = `MESA_GPU_TRACES=print_csv` +
`MESA_GPU_TRACEFILE=/var/local/gputrace-bb.csv`, /var/local/mtl2-tu-debug = `perf`, /var/local/mtl2-clients =
`backboardd` (Mesa opens the trace "w": other clients would overwrite it; the normal list is `*` / `-chronod`).
Reboot, swipebench, fetch the CSV, `python tools/hotdogb-gputrace.py CSV 25`. (Tracing + CPU-drawn SpringBoard:
17 fps, GPU 61%.) All three settings were removed and the client list restored afterwards.
- **Render passes 6.1 s of 20 s**: almost all full-screen 1125x2436 (iPhone X size; 22 GMEM bins of 576x224,
  tiled, no depth, LRZ off), ~2 per frame: 304 x 10.7 ms (~40+ draws), 345 x 3.3 ms (<10 draws), 181 x 5.5 ms.
  Inside them **draw_ib_gmem is ~95%**; GMEM loads/stores/binning are small. So the draws themselves are slow:
  ~0.25-1 ms per draw for UI quads.
- **Turnip perf log: `single_prim_mode due to pipeline settings` for every pipeline (122)**. That is Turnip's
  per-primitive flush mode for raster-order attachment access / feedback loops (programmable blending, which CA
  uses and metal2vulkan emulates). Prime suspect for the slow draws; to check in metal2vulkan/Turnip source
  (which pipelines ask for it, and whether CA's shaders really read the framebuffer).
- **Copies 3.5 s of 20 s**: copy_buffer_to_image 2.8 s (13201 = ~660 per second: texture uploads / shadow
  copies of buffer- or IOSurface-backed textures), copy_image_to_buffer 0.67 s (917 = ~45/s: the frame copied out
  to the IOSurface, see "Tried, reverted" requiresDedicatedAllocation above).

### HOTDOGB: graphics profiling - the home screen is GPU-bound, 24 ms per frame (kernels 0159-0161, 2026-11-08)
- **Correction**: the first swipebench (k0158: "23 fps, GPU 6%") measured the wrong screen. The unlock swipe does
  not always work; check with `tools/flame-shot X.png --h 1200 --stride 4352` (home screen centre pixel 72b2d2ff)
  before `tools/hotdogb-swipebench.py 30 --no-unlock`.
- **0159** sample bursts: USB command `sample NAMES COUNT MS`, `swipebench --sample`, `tools/sweet-profile.py`
  (busy threads, hot pcs; symbolicate on the Mac with tools/sweet-crash.py + research/dsc). SpringBoard is mostly
  idle; backboardd's render thread runs in ~45% of samples, under SweetMetal2 (sys_dcache_flush, IOConnect calls).
  SweetMetal2's own offsets: llvm-symbolizer on work/metal2/SweetMetal2.bundle/SweetMetal2 (the
  "MTL2[pid name]: images:" line gives its load address).
- **0160** fence waits sleep after 100 us spin: **both k0160 boots froze 15-20 s after backboardd's first GPU work**
  (Turnip DeviceLost in /var/local/mtl2.log, ~160000 refused command buffers, no kernel fault; brightness still
  worked). k0160 with sweetgpusleep=0: fine. **0161** spins by default again; its sweetgpusleep=200 run
  (k0161-1108-sleep) did NOT freeze (35.9 fps, ~21000 sleeps/5 s, avg 233 us, max ~400 us) and the render thread
  still ran in 272/600 samples: the fence spin is not where its CPU goes. Why 0160 froze is open (Turnip only calls
  the device lost on a v2 error, a kernel fault, a 5 s wait or 64 busy submits: 0161 logs v2 errors, none seen).
- **0161 job timing** (`SWEET GPU: 5s jobs:` line), home-screen paging at 90 Hz (k0161-1108-spin): **35.0 fps, GPU
  65-70% busy**; per 5 s ~130 backboardd frames take >= 16 ms: one submit of 16 IBs, ~111600 dwords, ~24 ms; of
  that **one IB ~15.5 ms, one ~6 ms, one ~1.1 ms**, the other 13 are 3-120 us. Ring max ~1650/8192 dw, 0 busy
  submits. So the frame rate is the GPU's: the next step is what that 15.5 ms IB renders (Turnip's view: GMEM or
  sysmem, blur passes, target sizes) - Turnip debug/perf output in backboardd.

### HOTDOGB: 90 Hz panel (kernels 0157/0158) and a hands-free swipe benchmark (2026-11-08)
- **0157** `sweet90=1`: the panel's 90 Hz timing switch over DSI0 DCS (F0 5A 5A, 60 10, F0 A5 A5; dtbo sofef03f_m
  90 Hz timing, same DSI clock) + vsync every 11.1 ms. Result (k0157-1108-1927): **the picture froze** (touch and
  brightness still worked, iOS kept presenting): INTF_1's tear check still counted for 60 Hz.
- **0158**: before the switch, INTF_1 (0xae6b800) SYNC_CONFIG_VSYNC vsync_count (0x284 bits 18:0) x 60/90 (only
  if 50..1000); `sweet90` is a bit mask: 1 = panel + tear check, 2 = vsync timer, 3 = both (run-current: 3).
  k0158-1108-1946: the picture updates; the user: "hard to tell, probably yes, but still sluggish". The tear-check
  log lines come before the live log starts (read them from the rawdump if needed).
- **Do not poke 0xae70000**: reading it (wrong ping-pong guess) reset the phone. The tear check is in INTF_1.
- **tools/hotdogb-swipebench.py [SECS]**: tap + swipe-up unlock, then home pages 1 <-> 2 through the kernel's
  "swipe" command, averaging the 5 s SWEET FB / SWEET GPU lines inside the window. k0158 at 90 Hz, 30 s:
  **23.0 fps** (18.6-24.8), **GPU busy 6%**, backboardd 715 submits at 2.1 ms. So home-screen paging is not
  GPU-bound (CPUs are at their top clocks since 0086, DDR/SH0 voted at top by sweet_gpu.c): the time goes on the
  CPU side / frame pacing (backboardd's and SpringBoard's threads). Settings-type screens did reach 57-59% GPU.
- Next: on-demand thread samples during the benchmark (bsd/dev/sweet_sample.c only samples every 120 s) and
  symbolication with tools/sweet-crash.py on the Mac.

### HOTDOGB: iOS PLAYS THROUGH THE SPEAKERS, NO CRACKLE (2026-11-08) - SweetAudio.driver + audiod, kernel 0156
The user played ringtones / Safari audio: clean from both speakers (k0156-jit-1108).
- **SweetAudio.driver** (phone/audio/hal, build hal/build.sh; install tools/hotdogb-audio-install.py X.tgz, then
  reboot): ios-topaz's RedwoodAudio HAL plug-in (MIT, from HackiOS user space, notices kept). In mediaserverd it
  presents "Speaker" (+ "Hawking", "Actuator", "Codec", which VirtualAudio's routing needs) and writes the
  speaker's 48 kHz 16-bit stereo into **/var/local/audio.fifo**: O_NONBLOCK, F_SETNOSIGPIPE, 512-byte (PIPE_BUF)
  pieces so a full FIFO never splits a frame, reopened when audiod comes back. Its log file
  (/var/local/sweetaudio.log) never appears: mediaserverd's sandbox does not allow writing there.
- **VirtualAudio.plugin** is needed (it makes the speaker route): tools/sweet-root-tweaks.py's "audio" step moves
  it to /Library/Audio/Plug-Ins/HAL.disabled; the installer links /Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin
  -> ../HAL.disabled/VirtualAudio.plugin. mediaserverd did not crash with it (with SweetAudio's devices).
- **audiod** after the stream opens (`#### SPEAKER READY FOR IOS`): makes the FIFO (0666), keeps the ASM session
  running, drains the FIFO into a 1 s ring every 2 ms, and gives the DSP **whole 25 ms buffers only**, 6 queued;
  silence when iOS plays nothing or the DSP is about to run dry. Logs `#### IOS AUDIO PLAYING` and, 2 s after the
  last data, "idle (N frames from iOS, M buffers short, K frames dropped)".
- Crackle history: (1) reading the FIFO only when the DSP returned a buffer: the pipe (16 KB, 85 ms) overflowed
  on the HAL's bursts, ~38000 of 48000 frames/s arrived; (2) with the ring, partial buffers padded with silence:
  85 of 280 buffers short. Both fixed.
- The boot test tone is now opt-in: /var/local/audiod.tone. Volume: iOS scales the samples (software volume);
  the amps have no speaker protection - keep it moderate.
- Open: the amps stay unmuted and the stream runs all the time (power); microphone; haptics; Bluetooth.

### HOTDOGB: SOUND - A TEST TONE FROM BOTH SPEAKERS (2026-11-08) - audiod, kernel 0156
The user heard the 3 s 440 Hz tone from both speakers (k0156-tone2-1108). All from userland, phone/audio/audiod
(boot job org.iosonpixel.audiod, log /var/local/audiod.log), about 65 s after boot:
- ADSP PAS boot -> GLINK apr_audio_svc + IPCRTR (QRTR, pd-mapper with the ADSP's domains, servreg listener:
  msm/adsp/audio_pd UP). APR headers must be SEQ_CMD (2): with 1 the audio PD ignored everything.
- AFE QUAT_MI2S_RX 0x1006: GPIO 137-140 = qua_mi2s, clock 0x106 at 3.072 MHz, I2S 48 kHz **32-bit** stereo SD1
  (the amps' container: 32-bit slots), start.
- TFA9874 x2 (tfa.c): container /var/local/fw/tfa98xx.cnt "speaker" profile, PLLS/CLKS/AMPS/SWS 1, MANSTATE 9.
- apps SMMU stream 0x1b21 -> a translation-off bank (CB6 / SMR8). Shared buffers: 0x9e380000 + 512 KB (top of
  ABL's display carve-out, below 4 GiB), ADSP address msw = SID offset 1.
- ASM: shared memory map (handle) -> open write v3 (session 1 stream 1, port 0x0101, multi-channel PCM v2,
  16-bit, no post-processing) -> media format 48 kHz stereo -> **then** ADM device open (NULL topology, 32-bit,
  COPP 0) and matrix map session 1 -> COPP (mapping before the ASM open: 0x2 EBADPARAM) -> run -> 120 x 25 ms
  writes, all done -> pause.
- Volume: the tone is at about -26 dBFS. The amps run without speaker protection (no NXP algorithm on the ADSP):
  keep levels moderate.
- Skip files: /var/local/audiod.{off,noport,noamp,notone}.
- Next: a PCM path from iOS: a CoreAudio HAL plug-in (ios-topaz uses HackiOS's RedwoodAudio.driver writing
  48 kHz 16-bit stereo to /dev/redwood-audio) feeding audiod's ASM ring; volume; stop the amps when idle.

### HOTDOGB: AUDIO STEP 1 - the ADSP boots and its audio services answer (2026-11-08) - audiod, kernel 0156
- phone/audio/audiod (build: phone/audio/build.sh; phone/wifi's SMEM/GLINK with -DGLINK_HOST=2 -DIPC_GLINK=8
  -DLINK_CHAN='"apr_audio_svc"' -DLINK_RAW). Boot job org.iosonpixel.audiod = `audiod auto` (`up 30`: waits for
  wland's AOSS messages, one QMP mailbox), log /var/local/audiod.log; off: /var/local/audiod.off.
- Firmware /var/local/fw/adsp/adsp.mdt + b00-b13 from ../hotdogb/fw/modem-image/image (not in git). Carve-out
  0x8be00000 + 0x1e00000 (dtbo overlay 3; exactly adsp.mdt's span). PAS id 1, SMP2P out 429 / in 443, APCS IPC
  bit 10 (SMP2P) / 8 (GLINK), SMEM host 2, cx.lvl top vote, QMP load_state adsp.
- k0156-audio2-1108: init image, mem setup, 13.5 MB of segments, auth and reset -> SMP2P "ready handover" after
  100 ms; GLINK opens glink_ssr, fastrpcglink-apps-dsp, IPCRTR, LOOPBACK_CTL_LPASS, apr_audio_svc, apr_apps2;
  AVCS state 1 = ready; services core/AFE/ASM/ADM/MVM/CVS/CVP/USM/LSM (legacy APR, not AudioReach). No pd-mapper
  or QRTR needed so far.
- First try: jetsam killed audiod (reason ns 1 code 7 = per-process limit) holding the 9.4 MB segment in one
  malloc; segments are now streamed in 64 KB pieces.
- **Speaker amps answer** (k0156-audio3-1108, `audiod tfa`, also run first by up/auto): phone/audio/i2c.c = GENI
  I2C FIFO mode on QUP0 SE4 0x890000 (XBL/ABL leave it clocked from XO with SER_M_CLK_CFG 0x21, FIFO mode allowed:
  GENI_IF_DISABLE_RO 0, firmware 0x303; pins 51/52 already qup4 + pull-up). 0x34 (right) and 0x35 (left) both:
  reg 03 = 0x0c74 = **TFA9874** (has a CoolFlux DSP), 00=0001 02=25c8 05=1000 08=0002 10=0017 13=010f. Reset GPIOs
  37 (north tile 0x3900000) / 100 (south 0x3d00000) are inputs pulled low = out of reset. MI2S pins 137-140 (south)
  are still GPIOs (function 1 = qua_mi2s).
- No tfa98xx.cnt in vendor_a (only "audio_tfa9894" / oplus_audio_check_tfa98xx_driver_ready strings): looking in
  odm_a (super + 0x100000, ext4, 51740 x 4 KB; read with `flame-files.py part super`), kept in ../hotdogb/fw/odm.
- tfa98xx.cnt found in odm_a /firmware (3575 bytes, ../hotdogb/fw/odm, not in git): "PM3_01" container, "stereo"
  9871N1C, devices 0x34/0x35, 9874_dummy.patch (DSP code in ROM), profiles receiver / speaker / calibrate.cal,
  SetExcursionFilters.msg, SetAlgoParams vstep, SetChipTempSelector.
- k0156-afe-1108: MI2S pins muxed (0x1 -> 0xc4), but the AFE never answered AFE_SVC_CMD_SET_PARAM (clock 0x106):
  no reply, no error, no crash. Core (AVCS, root PD) answers; AFE/ASM/ADM live in **msm/adsp/audio_pd**
  (adspua.jsn: avs/audio, instance 74). Now audiod also serves QRTR on IPCRTR (the ADSP is node 5) with a
  pd-mapper of the ADSP's domains (phone/wifi link.c has a per-channel table now; pdmapper.c -DPDMAPPER_ADSP),
  then retries the AFE for 18 s.
- **Where audio is stuck (2026-11-08, k0156-sr/apps2/probe-1108)**: the pd-mapper works (the ADSP asks for avs/audio
  -> msm/adsp/audio_pd, tms/pdr_enabled, tms/pddump_disabled) and the servreg notifier (QMI 0x42 at 5:3) says
  msm/adsp/audio_pd is **UP**, but nothing in the audio PD answers: AFE stop, an empty ADM routing map and the AFE
  clock set get no reply and no error, on apr_audio_svc and on apr_apps2 alike. The core (AVCS, root PD) answers
  every time. QRTR services the ADSP announces: 0x301, 0x42 (servreg), 0x422, 0x2b (ssctl), 0xf x2, 0x33, 0x18.
  Best guess (not verified): the audio PD waits for FastRPC - Android's audioadsprpcd serves its file reads
  (adsp_avs_config.acdb from vendor/etc/acdbdata) over the "fastrpcglink-apps-dsp" GLINK channel, which nobody
  opens here. Next: a minimal FastRPC (adsprpc) apps_std file server in audiod, or find what the audio PD waits for.
- **Found the cause in our code** (downstream techpack/audio from LineageOS/android_kernel_oneplus_sm8150
  lineage-20: ipc/apr.h, dsp/q6afe.c, include/dsp/apr_audio-v2.h): APR_MSG_TYPE_SEQ_CMD is **2** (audiod used 1 =
  CMD_RSP; the root PD's core answered anyway, the audio PD dropped them), and AFE_SVC_CMD_SET_PARAM is
  **0x000100f3** (v1 layout: payload_size + mem_mapping_hdr + param data, as audiod builds it), not 0x100f2.
  The FastRPC guess above is not needed for that. Downstream order otherwise matches audiod: q6core ready
  (AVCS_CMD_ADSP_EVENT_GET_STATE), then audio_notifier waits for servreg UP on the ADSP domain.
- For the PCM path later: ios-topaz streams a 1 s ring of 48 kHz 16-bit stereo from shared memory, with an SMMU
  identity mapping for the audio DMA stream and the SID in the address msw. Ours (stock DT qcom,msm-audio-ion):
  apps SMMU **0x1b21**, qcom,smmu-sid-mask 0xf.
- **k0156-seqcmd-1108: THE AUDIO PD ANSWERS, QUAT MI2S RUNS.** With SEQ_CMD headers: AFE stop -> 0, ADM empty
  routing map -> 0, AFE_SVC_CMD_SET_PARAM clock 0x106 1.536 MHz -> 0, I2S config (48 kHz 16-bit stereo SD1,
  internal WS) -> 0, QUAT_MI2S_RX start -> 0: `#### QUAT MI2S RUNNING`. The amps' registers do not change
  (00=0001, 10=0016): they are still powered down (TFA init from tfa98xx.cnt not written yet).
- **k0156-amps-1108: BOTH SPEAKER AMPS RUN** (phone/audio/tfa.c, the vendor driver's CF-bypass path: the TFA9874
  has no CoolFlux DSP). I2C reset, keys, the 0x0c74 V1.16 settings, the container's 55 device items + the
  "speaker" profile (6 items; also "receiver", "calibrate.cal"), MANSCONF 1, PWDN 0, then AMPE 1: both amps
  PLLS 1 CLKS 1 AREFS 1 AMPS 1 SWS 1, MANSTATE 9 (operating), NOCLK 0, `#### SPEAKER AMPS RUNNING (2 of 2)`.
  Silence so far: the I2S line carries zeros (no stream).
- The container's TDM setup: TDMSLLN / TDMSSIZE = 31 (**32-bit slots**), TDMNBCK 2, 0x34 speaker on slot 0
  (TDMSPKS 0), 0x35 on slot 1. QUAT_MI2S_RX runs 16-bit now: for audio switch it to 32-bit (bit clock
  48000 * 32 * 2 = 3.072 MHz).
- Next: TFA9874 bring-up (power up, clock lock on the bit clock, the container's register settings; the DSP code
  is in ROM: 9874_dummy.patch), then ADM COPP + ASM stream + shared memory through SMMU stream 0x1b21, a test tone.
- Next: AFE QUATERNARY_MI2S_RX (port 0x1006) clocks + config + start, ADM COPP, ASM stream + shared memory, a test
  tone; the two NXP TFA98xx amps (I2C 0x34 right / 0x35 left, reset GPIO 37 / 100, dtbo fragment@96) need their
  I2C bus (base DT) and probably tfa98xx.cnt from vendor. iOS side: a CoreAudio HAL plug-in feeding PCM
  (ios-topaz: HackiOS's RedwoodAudio.driver -> /dev/redwood-audio, 48 kHz 16-bit stereo).

### HOTDOGB: button combos - screenshot works, power-off screen never appears (2026-11-08)
- Volume keys were right in 0156 (GPIO6 = up, GPIO7 = down). 0157 swapped them on a misread (the user had pressed
  Android's power + volume DOWN); 0157 and its undo 0158 were dropped. iOS screenshot = side + volume UP: works.
- Holding side + either volume: SpringBoard's SBSOSClawGestureObserver triggers SOS after ~1.1 s, sosd opens
  InCallService with telSOS:, InCallService logs "showing SOS controller" / PHSOSViewController sosMode 2 and asks
  for a full-screen-overlay scene, then its main thread goes silent right after loading com.apple.avfoundation
  prefs (05:52:36 in logs/runs/k0156-poweroff2-1108-0120/hold.txt, read from /var/local/oslog-ui.log at
  70-82 MB). 20 s later SpringBoard: "Watchdog scene-create provision violated" (ignored: BKNoWatchdogs), so the
  power-off/SOS sliders never draw. Suspect: a synchronous audio/AVFoundation call (no audio on this phone).
- Next: pstack -w InCallService (phone/tools/pstack.c, new) as launchd job org.iosonpixel.pstack-ics ->
  /var/local/pstack-ics.log; symbolicate with tools/pstack-sym.py on the Mac.
- oslog-ui now logs InCallService + SpringBoard (~80 MB per boot; the file was 134 MB on 2026-11-08). Remove the
  job (or the file) when done.

### HOTDOGB: Settings > Date & Time froze Settings - fairplayd.H2 removed (2026-11-07)
Same as on the flame (2026-10-21): fairplayd.H2 exits right after starting and launchd relaunches it every 10 s
(13 `exec fairplayd.H2` in the first 150 s of k0156-1107), so requests to com.apple.fairplayd.versioned queue
forever; locationd blocks on one at startup and every synchronous CoreLocation caller hangs with it (Date & Time's
automatic time zone). `python tools/hotdogb-launchd-job.py disable com.apple.fairplayd.H2` (new disable/enable
commands; the job is kept in /var/local/launchd-disabled/ on the phone): in k0156-datetime-1107 Date & Time opens.
tools/sweet-root-tweaks.py now removes the job from new roots too. Phone jobs now: wland, oslog-wifid, oslog-ui
(`oslog -l Preferences locationd timed` -> /var/local/oslog-ui.log; remove it when no longer needed).

### HOTDOGB: BRIGHTNESS SLIDER WORKS (2026-11-07) - kernel 0156
Run k0156-1107 (`work/boot/kernel-0156` SHA-256 54472cd2...01fd, hotdogb-run-current.sh defaults: -q -G, sweetmtl=2;
the backlight service is now on by default). The user: "It works perfectly". Control Center's slider moves the panel
(`panel level 0x3ef ... 0x29d` while dragging, 34.8 fps), display off = dark, on = the last level.
How it got there (0155, 0156, after the 0152-0154 notes below):
- backboardd's unified log (job org.iosonpixel.oslog-bb: `oslog -l backboardd` -> /var/local/oslog-bb.log) showed
  CoreBrightness finding AppleARMBacklight, then `failed to create DisplayRef` / `failed to create a display module`.
  Disassembly of CoreBrightness (extracted on the Mac: `ipsw dyld extract research/dsc/dyld_shared_cache_arm64
  CoreBrightness`, read with llvm-objdump; research/ stays out of git): DisplayCreateWithID returns NULL when
  IORegistryEntryCreateCFProperty(backlight, "IODisplayParameters") is NULL. With no display module, libEDR's server
  never started, and CA's EDRClient (pstack: render thread in IOMFBServer::set_enabled -> register_libedr_callbacks
  -> EDRClient::client, a worker in EDRGetDefaultMinScalingFactor's mach_msg) waited on it every frame = the
  0.2-0.4 fps.
- 0155: IODisplayParameters {brightness {min 0, max 65536, value}} + the ADT /backlight properties on the service ->
  `load_libEDR`, `EDR service kick off`, `CBEDRServerAddMainDisplay: main display: 1`, normal frame rate.
  CoreBrightness still reads no calibration it recognises ("hardware max: 1.000000"): it works in fractions of 1.
- 0156: per slider step it sets "backlight-level" (= "brightness") 0..65536 of that maximum (slider 0.505 -> 10709,
  perceptual curve): mapped linearly to DBV 1..0x3ff; IOMFBBrightnessLevel on the framebuffer (it sets 0) ignored.
Open: real calibration (nits) so iOS knows the panel's range; auto-brightness has no ambient light sensor
("timeout waiting for ALS arrival" at every boot).

### HOTDOGB: panel brightness commands work; iOS's brightness slider does not (2026-11-07) - kernels 0152-0154
- **DSI DCS 0x51 works** (0152): `panel level 0: done` on display off, `0x199: done` (200 nits) on, every time;
  the user confirmed the panel goes fully dark with the power button (k0154-1107). Packet in ABL's display
  carve-out (0x9e300000), DSI0 command DMA, software trigger, between autorefresh frames: no glitch seen.
- **The AppleARMBacklight service stalls backboardd**: with it (0152/0153, sweetbl=2 from 0154) CoreBrightness
  matches it (`backlight-control -> AppleARMBacklight`) but never sets brightness-nits or IOMFBBrightnessLevel
  (0153 logs every setProperties key: none at all), and backboardd drops to 0.2-0.4 fps with the GPU idle.
  Same with the iPhone X /backlight node in the ADT (tools/adt-add-backlight.py; work/boot/hotdogb.adt has it now,
  the original is hotdogb.adt.pre-backlight). Without the service (k0152-nobl, k0154): 25-31 fps.
- pstack (phone/tools, job org.iosonpixel.pstack-bb samples PID 58 every 10 s into /var/local/pstack-bb.log;
  remove with hotdogb-launchd-job.py remove pstack-bb) in k0153-pstack-1107: every backboardd thread parked in
  the kernel; the **main thread** blocked for 2+ minutes in pthread_cond_wait called from 0x1c7579d20 <- 0x1c75752e0
  <- 0x1c7574148 (shared cache, slide 0x40718000) <- backboardd 0x104561e40 <- start. Not symbolicated yet: the
  Mac needs ipsw and research/dsc (`ipsw extract --dyld --remote <20H392 IPSW URL>`), then tools/pstack-sym.py
  on logs/runs/k0153-pstack-1107/pstack-bb-last.txt.
- Next for the slider: name that wait (likely CoreBrightness waiting for something our backlight never sends).

### HOTDOGB: kernel 0151 - full-screen GPU present; a client's first GPU resets forgiven (2026-11-07)
Run k0151-fill-1107 (`work/boot/kernel-0151` SHA-256 978b9910...7cc0, hotdogb-run-current.sh: -q -G, sweetmtl=2):
`present: fill (crop the overhang)`; panel captures have content in rows 0 and 2399 and columns 0 and 1079
(0150's fit left rows 0-29 and 2369-2399 black); Settings > Wallpaper drawn edge to edge. Why 0151's second
half: in k0150-metal2-safari-1107 a GPU fault at 508 s (backboardd's own work, read at 0x12952c2e0, fsr 2)
was reported to backboardd as a fault -> Turnip DeviceLost -> ~7000 refused command buffers, frozen panel.
Now the first 3 (`sweetgpuforgive`) lost-work resets per client log `lost its work (N of 3 forgiven, device
kept)` instead. The fault's cause is still open (0x308 crash and this fault both after app switches).

### HOTDOGB: Metal on the Adreno 640 - backboardd + SpringBoard draw through SweetMetal2 (2026-11-07)
SweetMetal2.bundle built on the Mac (tools/setup-metal2-trees.sh: work/turnip, work/metal2vulkan, work/UniXNU;
then build-turnip-ios.sh, build-m2v-bridge.sh, phone/metal2/build.sh; tgz SHA-256 569d91f0...72b6), installed
with the new `tools/hotdogb-metal2-install.py` (file server, no ssh). Kernel 0150, `hotdogb-run-current.sh`
(-q -G, sweetmtl=2). Three runs:
- k0150-metal2-1107-1427: both refused the device: `ShaderCache ... /var/local/m2v-cache/<proc>: Permission
  denied` (they run as mobile, the folder did not exist). The installer now makes /var/local/m2v-cache/<proc> 777.
- k0150-metal2-1107-1429: `device Turnip Adreno (TM) 640` in backboardd (1.4 s) and SpringBoard (1.9 s), 47
  render pipelines compiled, lock screen drawn - then at 57.5 s `GPU hang or fault (fence 44, done 35, smmu fsr 2
  far 0 fsynr0 70002)`, 1.7 s after CollectionsPoster made the wallpaper's surfaces (1105x1160 420f = the Head
  layer). Recovery works, but the lost fences raise the client's `faults` count, Turnip calls the device lost,
  backboardd never submits again: the panel kept the first fade-in frame (max pixel value 1 - looked black).
- k0150-metal2png-1107-1505: the default wallpaper's 5 HEIC layers (WWDC 2022, 375w-812h) replaced with PNG
  data (`tools/hotdogb-wallpaper-png.py`; originals in /var/local/wallpaper-heic-backup and ../hotdogb/wallpaper/heic;
  the flame's 2026-10-21 workaround). **No GPU fault in 954 s**; lock screen, Settings, Safari chrome drawn by
  Metal; up to 53.8 fps; pool peak 422 MB; backboardd 195-212 submits per 5 s while in use.
- k0150-metal2-safari-1107: `/var/local/mtl2-clients` = `*`, `-chronod` (the flame's list; installer default,
  `hotdogb-metal2-install.py --clients-only` writes just the list). com.apple.WebKit.GPU and MobileSafari get
  devices; **Safari renders pages** (telex.hu: header, text, buttons, photo; panel5-small.png), home screen with
  blurred dock and widget. Captures during scroll/load show unpainted tiles holding older pixels (wallpaper),
  gone once painted. One backboardd SIGSEGV at 81.6 s (far 0x308, pc 0x1c1deb0c0 in the shared cache, right
  after it made a new 1125x2436 surface; restarted with Metal, not repeated) - symbolicate on the Mac
  (tools/pstack-sym.py needs ipsw + research/dsc). Up to 58.6 fps while scrolling.
Open: backboardd's 0x308 crash if it repeats; the fault itself (a 420f/HEIC surface makes the GPU read address 0 - should fail cleanly, not fault); a Metal
device that survives a GPU reset; settings list rows looked torn in one capture mid-scroll.

### HOTDOGB: double home indicator = stale CPU-copy margins; 0150 clears them (2026-11-07)
User saw edges cut and a double navbar in run k0149-gpu-1107-1336. Panel memory read over the file server
(`flame-files.py read /dev/pmem 0x9c000000 4352*2400`, logs/runs/k0149-gpu-1107-1336/panel-*.png): the GPU
frames are right (1080x2339 at y 31, full width, corners), but a second home indicator sits at row ~2384
(and status-bar icons in the top band) - outside the GPU rectangle. Log: `GPU present not used: not exactly
one layer` (46.9 s): multi-layer frames (transitions) take the CPU copy, which crops 1125x2436 to the panel
(the cut edges while it shows) and writes the bands the GPU frames never repaint. 0150: the next GPU frame
clears outside its rectangle; `CPU fallbacks` per 5 s in the SWEET FB stats line. The real fix for the cropped
frames themselves: multi-layer frames on the GPU (later).
Kernel 0150 (`work/boot/kernel-0150` SHA-256 a94174de...0b61), run k0150-gpu-1107-1346: 4 CPU fallbacks in
the first minute (start-up), then GPU only; panel capture: rows 0-29 and 2369-2399 all black (the frame is
rows 30-2368), one home indicator.

### HOTDOGB: kernel 0149 - the Adreno 640 runs and presents every frame (2026-11-07)
Run k0149-gpu-1107-1336 (`work/boot/kernel-0149` SHA-256 3fc5bbde...ea48, `hotdogb-run-current.sh`: -q -G,
900 s, root rw). GPU firmware from the 7T's vendor_a (/firmware: a640_gmu.bin 37680, a630_sqe.fw 32304,
a640_zap.mdt 6860 + b00/b01/b02; zap code segment relocatable, 0x7b0 bytes) in ../hotdogb/fw/gpu.
Observed: `panel stride 4352`, iOS's surfaces mapped (slots 0/1), 300 frames presented by GPU blit
(17 us blit, 622 us cache clean), 35-106 submits per 5 s, 0 missed, no panic. The bring-up itself (GMU,
zap) runs before the USB log starts. Not yet: Metal on the GPU - SweetMetal2.bundle (phone/metal2 +
tools/build-turnip-ios.sh, Mac) is not on this root; no Metal client opened the accelerator.

### HOTDOGB: why wifid does not auto-join; GPU bring-up started (2026-11-07)
- **Auto-join**: run k0148-oslog-wifid with `oslog -l wifid` as a launchd job (tools/hotdogb-launchd-job.py,
  oslog from phone/tools; log /var/local/oslog-wifid.log). wifid has the network in "List of known networks",
  scans every few s, finds it and decides `AUTO-JOIN: Join candidate ... is not allowed (error=(1 'Below
  minimum RSSI threshold'))` for both of its BSSs at -38..-43 dBm (13 times) - so its minimum is ~0, not a
  real dBm value. The password lookup itself runs (WiFiNetworkCopyPasswordWithTimeout). Our kernel side
  (redwood_apple80211.c): scan results carry rssi but noise = snr = noise_known = 0; NOISE (17) is refused,
  RSSI (16) errors when not joined, RSSI_BOUNDS (106, 16 bytes) is refused. itlwm (macOS) answers RSSI/NOISE
  with 52-byte apple80211_rssi_data/noise_data (version, num_radios, unit, [4], aggregate, ext[4], ext agg)
  and has no RSSI_BOUNDS. Next try: noise -95 + snr in scan results, NOISE answered; RSSI_BOUNDS layout
  unknown. Setting console logging for wifid's job (launchd.plist.pre-wifidlog on the phone) gave nothing:
  os_log is not mirrored for daemons; use oslog.
- **GPU**: the A640 driver (sweet_gpu.c/sweet_a6xx.c) only starts with `sweetgpu=1` + firmware in the ADT.
  Only flame-specific value: ZAP_REGION 0x9a915000 -> 0149 makes it 0x99515000 on hotdogb (dtbo overlay 3,
  fragment@90). `hotdogb-run.sh -G` = sweetgpu=1 + `--gpufw ../hotdogb/fw/gpu` (a640_gmu.bin, a630_sqe.fw,
  a640_zap.mdt/.b02 from the 7T's vendor_a: super partition, LP metadata v10.0, vendor_a one extent at
  sector 8690688, 603 MiB, ext4; read with `flame-files.py part super` + debugfs).

### HOTDOGB: Wi-Fi works end to end - joined from Settings, DHCP, real traffic (2026-11-07)
Run k0148-wifi-3 (kernel 0148, wland job, root rw): the user picked the network in Settings, typed the
password and pressed Join; wifid ASSOCIATE -> wland WPA2 4-way handshake -> link up (422.6 s) -> configd's
DHCP on en0: 10.0.0.120, default route 10.0.0.1 (425.98 s, 3.4 s after the link); 90 s later en0 had sent
25303 frames (2.2 MB) and received 36882 (52 MB), nothing dropped.
- The fix that made DHCP happen: the root image's /var/preferences/SystemConfiguration/preferences.plist had
  host names only and no network services (as on flame, HANDOFF "WiFi IN SETTINGS"), so configd never ran
  DHCP on en0 (run k0148-wifi-1: joined, 0 frames sent). Removing it is not enough on hotdogb: configd starts
  at ~29 s, en0 only appears at ~69 s (after wland), and configd recreated host names only. Now: a hand-made
  preferences.plist with one Wi-Fi service (en0, AirPort, DHCP, IPv6 automatic) in an "Automatic" set
  (copy: ../hotdogb/fw/sc-preferences-wifi.plist; original on the phone as preferences.plist.pre-wifi).
- Open: (1) wifid did not rejoin by itself after a reboot (run k0148-wifi-2/3 needed the manual join; no
  com.apple.wifi.known-networks.plist was written before) - check on the next boot. (2) wlan.conf as the
  boot-time fallback is not set (the user writes it). (3) Done: sweet-root-tweaks.py `network()` writes
  the same preferences.plist into new root images (tested against the phone's file: identical).

### HOTDOGB: Wi-Fi radio works - wland boots the OnePlus modem, WLFW, scan finds 29 networks (2026-11-07)
Run k0148-wifi-1 (kernel 0148, root rw, launchd job `wland auto`; wland-hotdogb with MPSS 0x8dc00000+0xa000000):
wland starts at 32.8 s; SMMU 0x640 -> CB5 via SMR4; WLAN regulators + rfclka2 + QDSS; PAS init/mem setup/
auth-and-reset all 0; modem ready at once; GLINK + QRTR + rmtfs (modemst1/2, fsg) + pd-mapper + TFTP;
wlanmdsp.mbn over TFTP (~7 s); WLFW: chip 0x30224, fw WLAN.HL.3.0.c8-00232, board 0 -> bdwlan.bin (26328
bytes) accepted, FW READY, mission mode; CE rings at 0xe0000000, HTC/WMI/HTT ready, passive scan: 29
networks (2.4 + 5 GHz) about 24 s after wland started. `bridge: hello: the kernel makes en0`. Not served
(harmless so far): rmtfs `/oem/nvbk/static`, `/oem/nvbk/dynamic`, `/oppo/oem_partion` (OnePlus NV backups,
LUN 0 oem_stanvbk/oem_dycnvbk), TFTP mcfg mbn_hw.dig/mbn_sw.dig. Next: join (/var/local/wlan.conf, written
by the user) and DHCP; then whether wifid/Settings shows the scan list.

### HOTDOGB: kernel 0148 works; Wi-Fi firmware pulled and installed (2026-11-07)
Run k0148-root-1107-1211 (kernel-0148 SHA-256 6ddd1584...0ec6, 900 s, root rw, -q): PARTS lists 118 partitions
on LUN 0-5; PART read modem_a (LUN 4, 300 MB in ~4 min) and modemst1/modemst2/fsg/fsc (LUN 5), plus dtbo_a.
No panic, no refused writes. Found: the 7T's real modem region is 0x8dc00000 + 0xa000000 (dtbo overlay 3,
HOTDOGB.md "Wi-Fi"); board.h fixed, so the first wland-hotdogb build (0x8d800000) must not run - rebuild.
Firmware + EFS images are on the iOS root (/var/local/fw), no launchd job yet. Next: rebuilt wland,
`python tools/hotdogb-wifi-install.py` (adds the job), reboot, then a run and /var/local/wland.log.

### HOTDOGB: Wi-Fi port started; runs without TWRP; kernel 0148 raw partition reads (2026-11-07)
- TWRP stopped working in the user's runs, so `tools/hotdogb-run.sh` no longer uses it by default: it records the
  live log into logs/runs/NAME/live.txt (Windows Python through WSL interop) and leaves the phone in fastboot
  (`-T` = the TWRP rawdump pull, `-L` = no recorder). Earlier: one TWRP pull by hand worked.
- Wi-Fi: same WLAN hardware as flame (0x18800000, SMMU stream 0x640, MSA 0x8bc00000 1 MB, regulators
  pm8150 l1/l7 + pm8150l l2/l11 at the same voltages); the modem region differs (0x8d800000 + 0x9600000).
  `phone/wifi/board.h` (BOARD=hotdogb default in build.sh). The kernel side already runs on hotdogb
  (`/dev/wlan`, wifid starts). Missing: OnePlus firmware + EFS partitions, a Mac build of wland, install.
- 0148 (not built yet): file server `PARTS`/`PART` read any partition read-only, so
  `python tools/hotdogb-fw-pull.py` gets the firmware during a run. Then `python tools/hotdogb-wifi-install.py`
  (wland + firmware + launchd job), reboot, boot again. `tools/hotdogb-run-current.sh` = kernel 0148, 900 s.

### HOTDOGB: kernel 0147 with the iOS root - SpringBoard up, backboardd gets touches (2026-11-07)
Run k0147-root (`work/boot/kernel-0147`, 600 s, `-a "rd=ufsroot0 ufsrw=1 rootrw=1 sweetusb=2 sweetmtl=2"`,
log through TWRP): the root mounted read-write, and launchd started SpringBoard and backboardd (103 s on the log clock).
`#### HID SweetHIDTouch OPENED BY IOHIDEventServiceUserClient` at 108.9 s (backboardd, plus SweetHIDButtons).
Touch running at 120.6 s, `#### FIRST TOUCH EVENT SENT TO IOS HID (1 CLIENTS)` at 128.7 s, and 220
touch-downs up to the end. SpringBoard (pid 26) still alive at 722 s and redrawing: SWEET FB 6-16 fps
while touched, 4000+ swaps. 484 `CRASH` lines, all background processes (TTS synthesizers, assetsd,
Spotlight importers; bluetoothd and fairplayd.H2 respawned 62 times each). No panic. The kernel sent
ARP for 192.168.7.1 (no IP set on the Windows side yet). Open: what the screen showed (user report);
the respawn loops; the live log over NCM.

### HOTDOGB: iOS root written to userdata (sda22); Android must not boot (2026-11-07)
`ios16-root-20H392.img` (SHA-256 d699e211...3502, checked before writing) went to userdata as a sparse
image with `fastboot -S 512M flash userdata` (details and the check in HOTDOGB.md "iOS root image").
Afterwards the device matched the image in every 16 MiB window that holds data, except the last
block (backup volume header), which I wrote by hand from TWRP and then verified. Android's data is gone.
Android must not boot now: its /data mount would fail and recovery offers "factory data reset", which
would wipe the root. `tools/hotdogb-run.sh` therefore reads rawdump through TWRP 3.7.0_11-0-hotdog
(`fastboot boot` only; `hotdogb/downloads/twrp/`, SHA-256 matches twrp.me) and goes back to fastboot;
`-A` is the old Android path. Tested by hand: TWRP adb is root, logpull.sh `/tmp/shimlog` pulled the
k0147 log, identical. Not run yet: a kernel with the root mounted (`-a "rd=ufsroot0 ufsrw=1 rootrw=1
sweetusb=2 sweetmtl=2"`), and the USB NCM live log, which needs 192.168.7.1/24 on the phone's network
adapter in Windows (the user sets it).

### HOTDOGB: kernel 0147 - touch up at 32 s, 10-finger multi-touch works (2026-11-07)
Kernel 0147 (`work/boot/kernel-0147`, SHA-256 e6f8f46e...126c, logs/runs/k0147-first, ran 300 s; fetched
from peketr@truenas.local:~/ios-kernels): `#### TOUCHSCREEN RUNNING` at 31.97 s (0.17 s after the 20 s
start delay; was 132 s). `#### MULTI-TOUCH: 10 FINGERS DOWN AT ONCE` at 40.9 s; 3907 reports over all 10
objects, 158 HID down/up events, no read errors or GPI timeouts. `FIRST TOUCH EVENT SENT TO IOS HID
(0 CLIENTS)`: no userland, as expected. Next: a run with the iOS root so backboardd opens SweetHIDTouch.
- First 0147 attempt: `fastboot boot` itself failed (USB "no link"/"Too many links"); fixed by rebooting
  the phone into the bootloader by hand.

### HOTDOGB: 0147 - fast touch start-up, multi-touch and HID milestones (2026-11-07)
- Input path already exists: s3_report -> sweet_hid_touch -> SweetHIDTouch (SweetHID.cpp), panel pixels;
  the S3706 reports panel pixels too (DT touchpanel,panel-coords 1079x2399). In k0146-first SweetHID
  logged 24 touch down/up events; no userland was running (no root), so no HID client yet.
- **0147** (written, syntax-checked, NOT built/run): the 2.5 s pauses after GPI debug lines only with
  `hdgpidbg=1` (touch up ~22 s after boot instead of ~132 s; `hdtouchlog=1` still logs); milestones
  `#### FIRST TOUCH`, `#### MULTI-TOUCH: N FINGERS DOWN AT ONCE`, `#### HID ... OPENED BY ... (IOS IS
  LISTENING)`, `#### FIRST TOUCH EVENT SENT TO IOS HID (N CLIENTS)`.
- Proving iOS receives touches needs a userland root (backboardd opening SweetHIDTouch); the rawdump
  log setup has none.

### HOTDOGB: kernel 0146 - TOUCHSCREEN WORKS over GPI DMA (2026-11-07)
Kernel 0146 (`work/boot/kernel-0146`, SHA-256 afd2d225...72e7, logs/runs/k0146-first, normal path with
`hdtouchlog=1`): mapping the QUP wrapper 2 data stream 0x7a3 fixed the reset. Transfers 0-2 complete with
events, PDT scan finds F34/F01/F12 on page 0 and F54 on page 1, F01 status 0x81 (post-reset, then
configured: ctrl0 0 -> 0x84), irq enable 0x7f, F12 10 objects, `#### TOUCHSCREEN RUNNING` at 132 s.
443 touch reports between 193 s and 214 s (one finger, x 60-1066, y 867-2092), no read errors, ran 300 s.
- Next: drop the 2.5 s GPIDBG waits on the normal path (start-up takes ~100 s with hdtouchlog=1), check
  multi-touch, and feed the reports into the HID/input side.

### HOTDOGB: kernel 0145 - probe 5 works, probe 4 resets: unmapped QUP2 data stream 0x7a3; 0146 (2026-11-07)
Kernel 0145 (`work/boot/kernel-0145`, SHA-256 abba2ec7...6d27; logs/runs/k0145-probe4, k0145-probe5):
- probe 4 (CONFIG0 + GO + DMA): reset right after the synced "ringing" line (73 s), no dump made it.
- probe 5 (CONFIG0 + GO, no DMA): ran 300 s. TX RP 0xf0001010 (CONFIG0 consumed), GPI PC 0x4d -> 0x3df,
  QSB log 0xf000101c (TRE fetch via stream 0x7b6 works), SE status 0x41, M_CMD0 0x08004000 (I2C write to
  0x20 started, waiting for data). So TRE fetch and SE command work; the DMA TRE resets.
- Cause (DT): the touch SE's wrapper `qcom,qupv3_2_geni_se@cc0000` has its own context bank,
  iommus = <&apps_smmu 0x7a3 0>, qcom,iommu-s1-bypass; Linux maps I2C buffers for the wrapper device.
  0x7a3 had no SMR; sCR0 0x270406 has USFCFG set -> unmatched-stream fault -> reset.
- **0146** (written, syntax-checked, NOT built/run): also share stream 0x7a3 onto the USB translation-off
  bank (`hdqupsid=1` default, 0 = old behaviour); `#### SMMU: QUP WRAPPER 2 DATA STREAM 0x7A3 MAPPED`.
  Run the normal path first (no hdgpiprobe); then probe 4 if it still resets.

### HOTDOGB: kernel 0144 probes 1-3 all survive; GPI never consumes a TRE; 0145 fast trace (2026-11-07)
Kernel 0144 (`work/boot/kernel-0144`, SHA-256 4d1eed36...91e8; logs/runs/k0144-probe1..3, all ran 300 s):
- probe 1 (empty TX doorbell), 2 (RX DMA TRE alone), 3 (CONFIG0 alone, IEOT): no reset, no SMMU global
  fault (sCR0 0x2f0406, sGFSR 0). WP registers follow the doorbells (tx wp 0xf0001010 / rx wp 0xf0001410)
  but RP never moves and no event arrives: the GPI does not consume even a lone CONFIG0.
- So the reset needs the full chain (CONFIG0 + GO + DMA). probe 1's first try ran fine; only the rescue
  boot afterwards failed (`fastboot: error: Command failed`, phone left on the bootloader screen); retried.
- **0145** (written, syntax-checked, NOT built/run): `sweet_log_sync()` (sweet_ufs.c) writes the log to disk
  synchronously; probe dumps add GPI PC/SW_RF/QSB log, raw GPII IRQ status and SE status/M_CMD/IRQ
  registers. New probes: 4 = the real first write with a synced log before the doorbell and 40 synced
  dumps after it; 5 = CONFIG0 + GO without DMA. Also `#### ALL CAPS` milestone lines in the touch flow
  (user request). Run order: probe 4, then 5.

### HOTDOGB: kernel 0143 - same reset at the first TX doorbell with the block at 0xf0000000; 0144 probes (2026-11-07)
Kernel 0143 (`work/boot/kernel-0143`, SHA-256 c6a8bfcd...6883, logs/runs/k0143-first; the first upload was
truncated at 13 MB, the Downloads copy is the full 17.4 MB): `GPI DMA block at fixed 0xf0000000`, setup and
START fine, last line again `txn write TREs queued, ringing TX doorbell`, back in fastboot after 80 s, no
panic. So address width was not (alone) the cause.
- Checked against OnePlus i2c-qcom-geni.c/gpi.c: TRE words (CONFIG0/GO/DMA, chain/IEOT/link_rx), channel
  and event CNTXT, SCRATCH_0, QOS, INTSET, ev ring priming all match. GCC 0x1e008 = 0x80000002 is s_ahb
  under HW gating (bit 1), not a clock left off.
- **0144** (written, syntax-checked, NOT built/run): `hdgpiprobe=N` runs one doorbell experiment after
  channel START and then stops the touch thread: 1 = TX doorbell with no new TREs (the register write
  itself), 2 = an RX DMA TRE alone (TRE fetch through the SMMU, no SE command), 3 = CONFIG0 alone (GPI
  programs the SE). Each prints ch/event RP/WP and the apps SMMU global fault state
  (`sweet_usb_smmu_status`: sCR0, sGFSR, sGFSYNR0/1, sGFAR, shared cb4 FSR/FAR). Default 0 = unchanged.
  Run order: probe 1, 2, 3, adding `hdgpiprobe=N` to the run's -a boot-args.

### HOTDOGB: kernel 0142 - no RAM below 4 GiB for XNU; 0143 uses a fixed low block (2026-11-07)
Kernel 0142 (`work/boot/kernel-0142`, SHA-256 91702962...fec0, logs/runs/k0142-first): no reset, ran the
full 300 s - but the touch thread stopped at `GPI DMA alloc failed (3, below 4 GiB 1)`: on hotdogb XNU only
manages RAM from 0x1_00000000 up (mkadt.py --highmem), so vm_page_find_contiguous below 4 GiB finds nothing.
- Second reason the high block was wrong: the channel/event doorbells take only the low 32 bits of a ring
  pointer, so with the block at 0x1_049xxxxx the first TX doorbell pointed outside the ring.
- **0143** (written, syntax-checked, NOT built/run): fixed DDR block below 4 GiB that XNU does not manage
  (`hdgpilowpa`, default 0xf0000000; fixed carve-outs end at 0xa8800000), mapped write-combining like
  sweet_usb.c's sweetusbdma path; cache flushes become a dsb for it (`s3_dma_sync`). `hdgpilow=0`: old
  kernel-memory block. Look for `GPI DMA block at fixed 0xf0000000`, then whether the run gets past
  "ringing TX doorbell" to "txn write rung", the wait/event lines and "txn 0 write done".

### HOTDOGB: kernel 0141 - reset within ms of the first TX doorbell (0140 just flushed one line more); 0142 (2026-11-06, 23:30)
Kernel 0141 (`work/boot/kernel-0141`, SHA-256 2145ce08...01f9, logs/runs/k0141-first): last line again
`txn write TREs queued, ringing TX doorbell` (back at 80 s), no "txn write rung". So 0140's extra line was
flush timing: the reset comes within milliseconds of the doorbell, while the GPI fetches/executes the first
TREs - the event-ring handling (0141) is never reached.
- Suspect: address width. The DMA block (rings + buffers) is at 0x1_049xxxxx; Linux only gives the GPI IOVAs
  in 0x100000..0x200000 (qcom,iova-range); the DWC3 that works >4 GiB is a 64-bit master. A 32-bit GPI
  would fetch from 0x049xxxxx - someone else's memory -> XPU reset.
- **0142** (written, syntax-checked, NOT built/run): kmem_alloc_contig with max_pnum = 4 GiB (`hdgpilow=1`
  default, 0 = anywhere); refuses to continue if the block still lands above 4 GiB. Check the
  `GPIDBG dma_pa` line is < 0x100000000.

### HOTDOGB: kernel 0140 - bus votes fix the doorbell reset; reset now while waiting for the event; 0141 (2026-11-06, 23:20)
Kernel 0140 (`work/boot/kernel-0140`, SHA-256 3d6e71cb...870c, logs/runs/k0140-first): BCM QUP0 + SN11 votes
-> 0, and the first TX doorbell no longer resets: `txn write rung: ch0 0x1020001a ... glob 0 gpii 0`. Reset
after that (back at 81 s): inside the 2.5 s marker wait or in s3_gpi_wait (event poll / doorbell / IEOB).
- s3_gpi_wait differed from Linux gpi_process_events: we wrote the event doorbell with the RP (WP == RP,
  "no space") and did so plus IEOB clear on every poll, even with no events. Linux advances a WP (primed at
  base+len-16) per recycled event and writes doorbell + IEOB clear only after consuming events.
- **0141** (written, syntax-checked, NOT built/run): Linux event-ring WP handling (s3.ev_wp); trace for the
  first 3 transfers: "wait ch N: hw rp .. ev_rp .. ev_wp ..", each raw event, "events consumed, writing
  event doorbell (wp)", "event doorbell + IEOB clear done". Timeout line shows hw rp.

### HOTDOGB: kernel 0139 - reset on the first GPI TX doorbell; 0140 adds the QUP2 bus votes (2026-11-06, 23:10)
Kernel 0139 (`work/boot/kernel-0139`, SHA-256 7d3efc78...24fb, logs/runs/k0139-first): GPI DMA up, touch reset
released, then `txn 0: write 2 (ff 00) read 0` and `txn write TREs queued, ringing TX doorbell` is the last
line; no "txn write rung" dump, no panic -> hardware reset the moment the GPI first masters memory (TRE fetch
from 0x1_04978000) / drives SE17. (USB DMA is also >4 GiB through the same cb4 and works.)
- Lead: the qupv3_2 wrapper node has `qcom,bus-mas-id = <MSM_BUS_MASTER_QUP_2>` -> EBI; Linux votes that path
  (se_geni_resources_on) before any transfer. sm8150-bus.dtsi: mas-qhm-qup2 -> BCM QUP0 (also the QUP core
  clock BCM), aggre2_noc -> snoc -> BCM SN11, then SN0/SH0/MC0 (CPU path, already on). An unvoted QUP2 NoC
  path would hang/abort the first master access -> reset.
- **0140** (written, syntax-checked, NOT built/run): before the clocks, ACTIVE BCM votes for QUP0 and SN11
  via sweet_rpmh_write (cmd = commit|valid|AB<<14|IB, `hdgpibcm=N`, default 1; 0 = skip). Logs
  `BCM QUP0 vote ... -> err` and marker "bus votes sent". Next if it still resets: DMA block below 4 GiB.

### HOTDOGB: kernel 0138 - GPI channels START fine; reset is a hardware reset in the first transfer; 0139 (2026-11-06, 23:00)
Kernel 0138 (`work/boot/kernel-0138`, SHA-256 40d7824c...fb18):
- k0138-first: `before start: ch0 0x1010001a ch1 0x10100112 ev 0x10110012 glob 0 gpii 0`, `tx started: ch0
  0x1020001a`, `rx started: ch1 0x10200112` (CHSTATE 2 = started, no GPII errors), then reset (71 s). Next
  in the code: "GPI DMA up", touch reset release, 150 ms, first I2C transfer (page select) over GPI.
- k0138-panictest (`hdgpipanic=1`): **0137 panic capture works** - the full panic (string, boot-args,
  paniclog, backtrace, stackshot) is in xnu.txt after "SWEET LOG: panic flush". So the real resets in
  0137/0138 (no panic on disk) are hardware resets (NoC/XPU-type), not kernel panics.
- Linux geni_i2c_gsi_xfer sends one I2C message per transfer: write = [CONFIG0 chain] GO(write, stretch if
  a read follows, chain) DMA(IEOT); read = GO(read, len, link_rx|IEOB) on TX + DMA(IEOT) on RX. Ours chained
  the read GO onto the write with no link_rx/IEOB and no IEOT on the TX DMA.
- **0139** (written, syntax-checked, NOT built/run): Linux TRE layout; markers "se init done, releasing
  touch reset" / "touch reset released, first I2C transfer next"; first 3 transfers traced ("txn N: write
  .. read ..", "txn write TREs queued, ringing TX doorbell", GPII dump "txn write rung" / "txn read rung",
  "txn N write/read done err"). Panic-flush marker now once, on its own line.

### HOTDOGB: kernel 0137 - SMMU share works, reset now at GPI channel START; 0138 (2026-11-06, 22:45)
Kernel 0137 (0136+0137, `work/boot/kernel-0137`, SHA-256 f35e0a3a...43ba, logs/runs/k0137-first):
`SWEET USB: SMMU share smr4 800007b6 s2cr 4 -> cb4 (translation off)`, then every GPI setup marker through
"ch ring" (53.9 s), then reset (back in fastboot at 63 s). No "SWEET LOG: panic flush" line: either a
hardware reset (likely) or a panic path the 0137 hooks miss. Same point as k0135-nosmmu, so the SMMU is
not the cause any more. Next step in the code is channel START (TX then RX). Our ALLOC/ring/scratch/QoS/
doorbell-MSB sequence matches Linux gpi.c (gpi_alloc_ev_chan / gpi_alloc_chan / gpi_start_chan).
- **0138** (written, syntax-checked, NOT built/run): GPII register dump (ch0/ch1/ev CNTXT_0, GLOB/GPII IRQ
  status, ERROR_LOG) "before start", "tx started"/"rx started" (or "... start failed"), each flushed.
  `hdgpistart=0` stops cleanly before START (should run the full 300 s); `hdgpipanic=1` panics there on
  purpose, to prove 0137 gets a panic onto disk.

### HOTDOGB: panics were on screen but never on disk; 0137 flushes the log at panic time (2026-11-06)
The user saw a kernel panic on screen at the end of the recent runs; none of it is in logs/runs/*/xnu.txt or
xnu.raw. 0098 already puts the panic string and backtraces into the persistent ring (consdebug_putc), but
the ring only reaches UFS from the log thread, which never runs again after a panic, and the 7T's RAM rescue
does not survive the reset. So the "resets" in 0131-0135 may be panics - the panic text is the real evidence.
- **0137** (written, syntax-checked with stubs, NOT built/run): sweet_ufs.c `sweet_log_panic_flush()` -
  polled UFS write of the unflushed ring + header + SYNC CACHE, no mutex (a stopped CPU may hold it), waits
  for / clears a command left in slot 0, UFS error printfs quiet. Hooked from osfmk/arm/model_dep.c (stock
  file, first patch to touch it): top of SavePanicInfo (panic string; before the panic_info wait that
  spins forever without a panic region) and top of paniclog_flush (after print_all_backtraces). The log
  thread's flush is now the shared sweet_log_flush_locked(). Expect "SWEET LOG: panic flush" then the panic.
- Build 0136 + 0137 together; run with `hdtouchlog=1`.

### HOTDOGB: kernel 0135 pins the reset to the touch driver's SMMU map; GPI setup works without it; 0136 (2026-11-06, 22:30)
Kernel 0135 (`work/boot/kernel-0135`, SHA-256 d1a336e1...b0b4), with the log kick:
- k0135-first (default): last marker `GPIDBG smmu enter` (33.9 s), never `smmu mapped` -> the reset is at
  `ml_io_map(0x15000000)` in s3_gpi_smmu (or within 2.5 s of it), before any SMMU read/write. sweet_usb.c
  maps the same block at 11 s fine.
- k0135-nosmmu (`hdgpismmu=0`): GPI setup runs all the way - smmu skipped, se gsi, irq masked, ev alloc,
  ev ring, ch alloc, ch ring (51.9 s) - then reset at channel START / first DMA: stream 0x7b6 unmatched in
  the SMMU (the same global-fault reset USB had before it got a bank). So the GPI engine is clocked and
  its registers, commands and rings work.
- (The recurring "SWEET USB: busERR ... fsr 400" line and "mount(N) failed" retries are in every run,
  including 300 s ones: unrelated.)
- **0136** (written, syntax-checked, NOT built/run): `sweet_usb_smmu_share(sid)` in sweet_usb.c adds an SMR
  for 0x7b6 -> USB's translation-off bank through USB's existing mapping; s3_gpi_smmu uses it by default
  (`hdgpismmu=3`; 1/2/0 keep the older paths). Markers: "smmu share via usb", "smmu shared"/"smmu share
  failed", then the setup markers. Run with `hdtouchlog=1`.

### HOTDOGB: S3706 cross-checked against OnePlus/Linux sources; 0135 fixes the GPI SE index (2026-11-06)
OnePlusOSS/android_kernel_oneplus_sm8150 (branch oneplus/SM8150_R_10.0) has the DT and the GPI/GENI drivers,
but its touch driver (`CONFIG_TOUCHPANEL_ONEPLUS` -> drivers/input/oneplus_touchscreen/) is not published on
any branch; LineageOS/android_kernel_oneplus_sm8150 carries it as
drivers/input/touchscreen/oplus_touchscreen/Synaptics/S3706/. Findings (details in HOTDOGB.md):
- **Bug: GPI SE index.** Touch is qupv3_se17_i2c @0xc80000, `dmas = <&gpi_dma2 {0,1} 0 3 64 0>` = SE index 0,
  proto 3 (I2C). Our GPI_SEID 3 came from the gpidump of i2c@c8c000 (the dump missed i2c@0xc80000; live FDT
  re-read via adb confirms 0). Fixed in **0135**.
- I2C timing: Linux 400 kHz = div 2, t_high 5, t_low 12, t_cycle 24 (we had 5/11/22). Fixed in 0135 (CONFIG0
  TRE 0x0318050c and SE_I2C_SCL). No qcom,clk-freq-out on the node -> Linux default 400 kHz.
- GPII 1 is right (Linux takes the lowest free GPII in gpii-mask 0xfa); stream 0x7b6 is right; Linux runs it
  with smmu-cfg 1 (S1 translation, IOVA 0x100000+1 MB), not a bypass bank.
- RMI4 side matches OnePlus: reset low 10 ms / high + 80 ms, L17A 3.008 V, reset GPIO80, attn GPIO122 active
  low, F01 ctrl0 (|0x80 configured), int-enable, F12 data1 8-byte objects. 0135 masks x/y to 12 bits like
  OnePlus and reads F01 data0 (device status) with the IRQ status, logging non-zero status.
- 0135 does not address the reset inside s3_gpi_smmu (0134's markers are still the way to find it).

### HOTDOGB: kernel 0133 - same reset, nothing from inside s3_gpi_smmu reached disk; 0134 flushes per marker (2026-11-06, 22:05)
Kernel 0133 (`work/boot/kernel-0133`, SHA-256 247c3e81...923b, logs/runs/k0133-first): identical end - last
line `GPIDBG dma_pa 0x104914000`, back in fastboot at 68 s. Not even the SMR dump / idr line (printed before
any SMMU write, then a 2.1 s wait) landed. Either the very first SMMU reads/mapping kill it, or the markers
are lost: the log thread sleeps 2 s *then* flushes, so its period is 2 s + flush time and a 2.1 s wait can
miss. 0130 (bailed just before s3_gpi_smmu, same DMA PA) ran 300 s, so the cause is after dma_pa.
- **0134** (written, syntax-checked, NOT built/run): `sweet_log_kick()` in sweet_ufs.c wakes the log thread;
  GPIDBG prints, kicks, waits 2.5 s. New markers: "smmu enter", "smmu mapped", idr0/idr1 + "smmu idr read",
  "smr read N" every 16 SMRs, "smr scan done", then 0133's "smmu own bank written"/"smmu s2cr written".

### HOTDOGB: kernel 0132 - the reset is inside the touch SMMU setup; 0133 shares USB's bank (2026-11-06, 21:50)
Kernel 0132 (`work/boot/kernel-0132`, SHA-256 7c814889...b6f1) ran with `hdtouchlog=1` (logs/runs/k0132-first).
Last line on disk: `GPIDBG dma_pa 0x1048f8000` - the marker before `s3_gpi_smmu`; "smmu ok" never landed,
back in fastboot at 68 s (same wall time as 0131, so 0131 died at the same place). The GPI engine itself
is not touched yet. s3_gpi_smmu is a copy of sweet_usb's working setup (USB: smr3 -> cb4, translation off),
but touch must take a *fresh* SMR and a *fresh* context bank (cb5+); a reset on writing one of those points
at a bank/SMR the hypervisor or secure world owns (reads as free, RAZ).
- **0133** (written, syntax-checked, NOT built/run): `hdgpismmu=1` (default) points 0x7b6's new SMR at the
  bank USB already configured (looked up via the SMR matching 0x140), so no fresh bank is written;
  `hdgpismmu=2` = old fresh-bank path, `0` = skip the SMMU. With hdtouchlog it logs every live SMR/S2CR,
  idr0/idr1, the chosen smr/cb, and markers "smmu own bank written" / "smmu s2cr written" before the SMR
  write. Run with `hdtouchlog=1`; if it still resets, the line before the last marker names the write.

### HOTDOGB: kernel 0131 - GPI setup resets the phone at ~55 s; 0132 adds step logging to localize it
Kernel 0131 (GPI 64-bit fix) ran, and now `s3_gpi_setup` actually executes (0130 bailed before it). The phone
reset ~55 s after boot - back in fastboot at 68 s - during GPI setup (logs/runs/k0131-first). Proof it is the
GPI setup: 0130, which bailed before it, ran the full 300 s; 0131 resets within ~2 s of entering it. The
console log flushes to rawdump only every 2 s, so every print inside setup (se fw, SMMU, GPI DMA up) is lost
before the reset, and the 7T's RAM rescue does not survive - the last line on disk is the RPMH L17A vote, just
before the GPI work. So we cannot yet see which step resets it. (A transient USB "no link" also hit `fastboot
boot` twice here; reseating the cable fixed it - unrelated to the kernel.)
- **0132** (diagnostic, written/verified, NOT built/run): a `GPIDBG` marker after each setup milestone
  (dma_pa, smmu ok, se gsi, irq masked, ev alloc, ev ring, ch alloc, ch ring) that, with `hdtouchlog=1`,
  prints and waits past one 2 s flush. The last marker on disk then names the step that resets the phone.
  Costs nothing without hdtouchlog. Build 0132 on the Mac (0131 tree needs only 0132), run with
  `hdtouchlog=1`, and read the last `GPIDBG` line. Likely suspects by marker: stops at "se gsi" = the first
  GPI 0xc00000 access hung (GPI engine unclocked - may need a gcc clock beyond the wrap2 votes); stops at
  "irq masked"/"ev alloc" = the event-ring ALLOCATE command hung/faulted; stops at "ch alloc" = SMMU/DMA fault
  on first ring access. hdtouchgpi=0 still gives the clean 0129 stop.

### HOTDOGB: kernel 0130 GPI touch reached DMA setup; 0131 fixes >4 GiB addressing (2026-11-06, 16:18)
Kernel 0130 (`work/boot/kernel-0130`, SHA-256 da65d473...b00c5) ran the full 300 s, no reset - the GPI path
touches nothing TrustZone-protected. Log (logs/runs/k0130-first/xnu.txt): the touch driver read the SE
(`se fw=0x303 proto=3`), then stopped at my own guard: `GPI DMA above 4 GB (0x104914000)`. The contiguous
DMA block landed at PA 0x1_04914000; I was only programming the low 32 bits of the TRE buffer addresses.
- **0131** (written, host-tested, NOT built/run): drop the >4 GiB bail (the GPI engine is 64-bit, like the
  UFS driver's DMA) and put the high word in the DMA TREs' dword1. The ring CNTXT already carried BASE_MSB
  and DOORBELL_1; an 8 KiB block never straddles a 4 GiB boundary, so the low-32 doorbell/event-RP math is
  safe. Host test now covers a PA above 4 GiB (TRE high word = 1); aarch64 syntax-checked.
- Next: build 0131 on the Mac (needs only 0131 on the 0130 tree; HOTDOGB.md "Kernel 0131 on the Mac"), run,
  touch the screen. Expect past the 4 GiB point: `GPI DMA up (GPII 1, dma_pa 0x1...)`, then ALLOCATE/START
  (watch for `GPI cmd ...` timeouts = clock/SMMU), a PDT scan, `F12 data1 at ...`, `N objects, polling
  every 8 ms`, and with hdtouchlog `obj N type 1 x .. y ..` as the screen is touched.

### HOTDOGB: S3706 touch over GPI DMA - patch 0130 (2026-11-06, written + host-tested, NOT built/run)
The FIFO switch is out (TrustZone), so touch I2C now goes the way Android does: the QUPv3 GPI DMA
engine. Patch 0130 adds a polled GPI/GSI client to `bsd/dev/sweet_s3706.c` (no new file):
- From the phone's live FDT: GPI DMA at `qcom,gpi-dma@0xc00000`, this SE's channel is
  `gpi_dmac00000_gpii1` (so GPII 1), `dmas` seid 3, IOMMU stream `<&apps_smmu 0x7b6 0>`.
- Setup (`s3_gpi_setup`): map the GPII window, one 8 KiB contiguous DMA block (event ring 4 KiB +
  TX/RX channel rings 1 KiB each + I2C buffers), an apps-SMMU stage-1 bypass bank for stream 0x7b6
  (same translation-off bank sweet_usb.c uses), SE into GSI mode (DMA_MODE_EN, GSI_EVENT_EN - all
  below SE+0x2000, never the forbidden region), then ALLOCATE/START the event ring + TX(0)/RX(1)
  channels, polling CNTXT_0 state (no GPII interrupt).
- Transfers (`s3_gpi_txn`): I2C config0 (400 kHz) once, then GO(write)+DMA for a write, or
  GO(write,stretch)+DMA+GO(read) on TX with an RX DMA for a read; doorbell, then poll the event ring
  for XFER_COMPLETE (EOT/SUCCESS) on the channel. `s3_read`/`s3_write`/`s3_page` dispatch to it when
  `s3.gpi`. Register + command model from Linux `drivers/dma/qcom/gpi.c` and `i2c-qcom-geni.c`.
- Reconstructed the post-0129 source tree from patches 0001-0129 (the Mac has the only real tree);
  0130 applies on it and reproduces the edit. Host-tested the TRE bit layout, ring WP wrap, and the
  event parser (EOT/SUCCESS/error/other-channel/wrap) under ASan; syntax-checked the register-poke
  functions against stubs for aarch64. Not built or run on the phone.
- Boot-args: `hdtouchgpi=0` forces the 0129 stop (to isolate a GPI problem); `hdtouchlog=1` logs per
  object. In the log expect `GPI DMA up (GPII 1, ...)`, a PDT scan (`PDT page 0 F01 ...`, `F12 ...`),
  `F12 data1 at ...`, then `N objects, polling every 8 ms`; touch the screen and, with hdtouchlog,
  `obj N type 1 x .. y ..`. Risks to watch: GPI/QUP clock may need enabling (ALLOCATE/START timeout);
  SMMU bypass may fault (no event, timeout); event-ring RP semantics (CNTXT_4) assumed per gpi.c.

### HOTDOGB: kernel 0128 - USB NCM works on Windows; touch FIFO switch resets the phone (2026-11-06, 15:30)
`work/boot/kernel-0128` (0001-0128, SHA-256 6a145776...37bd7d0). Boot-args as 0126 (`rd=ufsroot0
ufsroot=rawdump sweetusb=2 sweetmtl=2`). Runs k0128-first, k0128-nofifo (`hdtouchfifo=0`), k0128-notouch (`hdtouch=0`).
- **USB (0128) works**: Windows loads usbccgp + **UsbNcm.sys** ("UsbNcm Host Device") for the NCM function
  (USB\VID_1209&PID_0001&MI_00), adapter "Ethernet 2" Up at 480 Mbps, MAC 02-00-5E-77-EE-01. Answered:
  MS OS compat ID (`c0 01 0000 0004`, 16 then 40 bytes), GET_NTB_PARAMETERS (`a1 80`). The phone parses
  Windows' NTBs (DHCP, IPv6, mDNS, ARP probes); Windows counts the phone's ARP broadcasts. Windows has only
  169.254.24.133: give "Ethernet 2" 192.168.7.1/24 for ue0 (needs the user's OK; not done).
- **Touch (0127) resets the phone**: k0128-first and k0128-nofifo were back in fastboot after 38-39 s; the
  log ends at the L17A vote (~31.4 s, lines after the last 2 s flush are lost). k0128-notouch ran the full
  120 s. 0127 reads SE + 0x2000/0x2004/0x2008 before checking hdtouchfifo, so any access to that region
  resets the SoC: TrustZone-protected. FIFO mode cannot be selected from the kernel.
- **0129**: no access to SE + 0x2000.., a GSI-only SE is a logged stop again (as 0126). Kernel 0129
  (`work/boot/kernel-0129`, SHA-256 f1f4129d...316a3d), run k0129-first, same boot-args, no workaround:
  full 300 s (flamereboot at 311.951), "SE is GSI-only (FIFO interface off); touch needs GPI DMA; stopping"
  at 31.976, NCM configured and 48 frames/ARPs from Windows parsed. Current stable hotdogb kernel.
- Open: touch needs GSI/GPI DMA (QUPv3 2 GPI: channel/event rings, TREs for I2C config/go/DMA, the QUP's
  SMMU stream); a driver of its own.

### HOTDOGB: kernel 0126 boots on the OnePlus 7T (2026-11-06, 14:20) - kernel 0126
First XNU on hotdogb. `work/boot/kernel-0126` (SHA-256 d72b4823...61ea6ee, plain arm64), run from WSL with
`tools/hotdogb-run.sh` (new; shim/build-hotdogb.sh + mkboot_hotdogb.py, logs from rawdump, `fastboot boot`
only). Run k0126-rawdumplog, boot-args: hotdogb-run.sh base + `flamereboot=300 rd=ufsroot0 ufsroot=rawdump
sweetusb=2 sweetmtl=2` (rawdump as a read-only "root" so the UFS driver and console log start; userdata untouched).
Observed (logs/runs/k0126-rawdumplog/xnu.txt, 731 lines):
- Shim loads the payload at 0x1_00000000 and jumps; kernel up to the root mount, scheduler alive for 300 s
  (FLAME WDT pets until flamereboot, then resets into fastboot).
- 8/8 CPUs via PSCI, cpufreq on all three clusters (prime 2956 MHz), PM8150 RTC, AppleCLCD 1080x2400 at
  0x9c000000, UFS "rawdump" + console log to its end: all work.
- PM8150 GPIO7 (vol-) reads as an enabled input (en 0x80, mode 0): assumption confirmed. Run k0126-screen
  (same args): vol- (key 2, HID 0xea) and vol+ (key 1, 0xe9) give clean down/up pairs. On-screen console
  (no sweetfb_noconsole) seen by the user: readable, upright, fills the 1080x2400 panel, no flicker or freeze.
- S3706 SE: fw 0x303, protocol 3 (I2C) and 32-bit FIFOs as assumed, but **SE_FIFO_OFF bit 0 = 1** (FIFO
  interface disabled: the SE is set up for GSI DMA), so the driver stops. Needs GSI or SE DMA, or clearing it.
- USB: the host (a Windows PC) reads every descriptor, including the MS OS string 0xEE, and never
  sends SET_CONFIGURATION: Windows lists "Sweet iOS (USB Ethernet)" with no driver (it has none for
  CDC-ECM). The periodic `busERR addr 0 ... SMMU cb fsr 400` line is a status print, not a fault
  (busERR addr 0 = no bus error). Kernel side works; fixed for Windows by 0128 (NCM).
- Root mount fails as expected on rawdump (no HFS volume header), retried every 5 s, no panic.
- RAM log rescue: mailbox at 0x1ffe00000 does not survive the 7T's reset ("no mailbox"); the UFS log is enough.
Fixed: `tools/mkpayload.py` used GNU nm on Linux/WSL (no Mach-O support) -> ring addresses 0, shim never armed
the RAM log; now llvm-nm. Without `rd=ufsroot0` the UFS driver never starts, so nothing reaches rawdump.
Open: writing the iOS root to sda22 for a real root mount.
Next kernel (not built or run yet): 0127 S3706 SE to FIFO mode, 0128 USB CDC-NCM for Windows hosts
(HOTDOGB.md "Kernel 0129 on the Mac"). Check in its log: "fifo-if-disable now 0, fifo-off 0" then a PDT scan;
"function CDC-NCM", "configured (1)", "NCM endpoints configured" and a Windows network adapter.

### WiFi IN SETTINGS: wifid, en0, iOS's own DHCP (2026-10-22, 11:15) - kernels 0123/0124
Settings -> Wi-Fi: switch on, the home network ticked, nearby networks listed (from a
background scan while joined), Wi-Fi icon in the status bar; configd's DHCP on **en0** is the primary
interface, Safari/nettest go over it. All from a cold boot, nothing by hand. Screens: work/wifi-ui/.
- **How**: ios-topaz published `0900-en0-Apple80211-for-wifid` (from "darwin-redwood"): bsd/dev/redwood_net.c
  (en0 via the ifnet KPI, SIOCGIFMEDIA says 802.11), redwood_apple80211.c (wifid's Apple80211 requests as
  SIOCGA80211/SIOCSA80211 ioctls; the IOKit service has no user client on purpose -> "Falling back to IOCTL
  path"), redwood_sta.c (one station thread), iokit RedwoodWLAN.cpp (IO80211Controller + IONetworkInterface
  registry entries), MiniIONetworkStack.cpp (configd's InterfaceNamer waits for it). Applied unchanged
  (kernel 0123) plus topaz 0128 (IOKit quiesce) and HackiOS's keystore wrap/unwrap (keychain items).
  Their glue topaz_sta.c -> **bsd/dev/flame_sta.c**, on **bsd/dev/flame_wlan.c** = the same C API over
  **/dev/wlan** to the radio in userland: protocol phone/wifi/flamewlan.h (= bsd/sys/flame_wlan.h), one
  message per read/write: kernel -> wland SCAN/JOIN/LEAVE/TX frame, wland -> kernel HELLO/BSS list/SCAN_DONE/
  JOIN_DONE/LEFT/LINK(+ssid, bssid)/RX frame. Station calls block on the answer (scan 20 s, join 40 s).
- **wland** (phone/wifi/bridge.c): opens /dev/wlan after radio start; then **no utun/ipnet** (iOS does IP on
  en0); HELLO after the first scan (kernel makes en0); wlan.conf auto-join stays as the fallback until wifid
  asks for a network (bridge_joins). SCAN while joined = a **background scan** (radio_scan_now: the firmware's
  min/max rest on the home channel; at most every 15 s, else the cached list; list = heard in the last 2 min).
  No /dev/wlan (older kernel or boot-arg `sweetwifid=0`) -> the old utun path.
- **ADT**: /arm-io/wlan node (tools/mkadt.py; launchd starts wifid only with it: `_LimitLoadToDeviceTree`).
  work/boot/flame-hi.adt rebuilt (old one: flame-hi.adt.pre-wlan).
- **configd had no network services** (preferences.plist held host names only, dated 1969): moved to
  /var/preferences/SystemConfiguration/preferences.plist.pre-wifi (copy /var/local/sc-preferences.plist.bak),
  configd restarted -> default configuration with a Wi-Fi service for en0 (persists).
- **Kernel 0124**: HackiOS's "10-bit surfaces ('w30r', 'l10r', 'w30R', 'b3a8') as one-plane BGRA" only for
  com.apple.WebKit.WebContent (`sweetsurf10=0` never, `=2` always). For WebContent it **fixed Safari** (page
  bodies were white: its CG-drawn 'w30r' tiles); for everyone it broke chronod's CPU-path widgets (white) and
  NewsToday2's Metal images (garbled colours). Also flame_wlan keeps the joined network's scan record.
- **Open**: (1) Settings labels the network "Unsecured": wifid never joined it itself (wland's wlan.conf join
  came first) and has no known network for it. Pick it in Settings once and type the password: wifid stores
  it (keychain wrap now works) and joins by itself; then wlan.conf is only the fallback. (2) Joining another
  network from Settings is untested (path: ASSOCIATE -> sta_join -> FW_JOIN -> radio_join with the PMK as hex).
  (3) wifid sets 383 (4104 bytes) and 419, gets 96/97 RATE/MCS/NOISE...: REFUSED, harmless so far.
  (4) `wland shutdown; wland up` (radio restart) still untested: reboot the phone instead.
- Safari now renders whole pages (text, images, tables) with Metal v2; Today view widgets render with chronod on
  the CPU path (`/var/local/mtl2-clients` = `* -chronod`; with a Metal device they are still white).
- **ios-topaz pulled** (now at its 0950): besides 0900 it has **audio** (0144 ADSP, 0145 codec, 0146 stream,
  0935 /dev/redwood-audio), 0148 charger, 0140 4K pages, display/touch fixes and many GPU-pool patches (0901-0947)
  for their Adreno 610: the next things to read before writing our audio driver.

### WiFi at boot, rejoin, frame pacing, DDR vote (2026-10-22, 10:40) - kernel 0122, sweetmtl=2
Boot: `tools/flame-fresh-run.sh NAME 1800 sweetmtl=2` (boots **kernel-0122**). WiFi now comes up by itself.
- **WiFi boot job**: launchd job `org.iosonpixel.wland` (in /System/Library/xpc/launchd.plist; work/userland/
  launchd.plist.cur = what is on the phone, .pre-wland on the phone = before) runs `wland auto`: once per phone
  boot (stamp /var/local/wland.boot = kern.boottime), off with /var/local/wland.off or boot-arg `sweetwifi=0`,
  log /var/local/wland.log (previous boot: .prev). Joined + DHCP ~23 s after start. It must lift its own
  jetsam limit (memorystatus_control SET_MEMLIMIT_PROPERTIES 2048 MB): launchd's per-process limit killed it
  while the modem firmware loaded (reason 7); `JetsamProperties` in the launchd cache did not help.
- **Rejoin** (radio.c `w_link_lost`, wland.c `join_poll`): deauth/disassoc, peer kickout and WMI roam event
  0xb001 reason 2 (beacon miss) end the link with ENOTCONN; wland joins again after 3 s (rescan first);
  failed joins retry 4 x 5 s, then every 30 s with a rescan. On link loss ipnet removes the lease from the utun
  and runs `netsvc down`. Test: `kill -USR1 $(psx wland | cut -c1-6)` drops the link: rejoined + new lease in
  ~6 s, HTTPS 200. wland idle CPU: ~1.6% of a core (psx now prints CPU seconds).
- **Kernel 0121: vsync notifications carry the refresh period.** CA's
  `IOMFBServer::vsync_callback(fb, a1, a2, a3, refcon)` takes a2 = vsync host time, a2 + a3 = the next one;
  we sent a3 = 0 (deadlines always past). Swipe benchmark 15.8 -> 21 fps.
- **Kernel 0120**: GPU stats per process, summed on every submit (the old line capped at 256 submits/5 s, so its
  "busy %" undercounted): `SWEET GPU: 5s: N submits (M missed), GPU busy ...; backboardd n/ms present n/ms ...`.
  RPMh lookups accept BCM resources: `wland bcm MC0 0x3fff` votes a bus from userland.
- **Kernel 0122**: DDR (BCM MC0) + memory NoC (SH0) voted to the top at GPU start (`sweetddr=0` off): 8-thread
  CPU bandwidth 13.7 -> 18-19.6 GB/s (`membw` in phone/tools), present blit 1.65 -> 1.1 ms, backboardd submits
  ~20% faster. GPU pool cap: 256 MB per process (`sweetgpucap`, 0 = none) and the last 160 MB
  (`sweetgpureserve`) only for backboardd/SpringBoard; refusals logged once per client ("SWEET MTL: v2 X: ... refused").
- **Metal v2 fixes**: (1) NBTexture adopts Metal's private `MTLTextureImplementation` protocol: Metal NSLogged
  "texture is not a MTLTextureImplementation." ~600 lines/s from backboardd while scrolling (flooded the live log).
  (2) `/var/local/mtl2-clients` takes `-NAME` (exclude); now `* -chronod`: **chronod's widgets (Photos,
  Calendar, Weather in the Today view) are white with a Metal device and render right on CA's CPU path**
  (root cause open; its RenderBox pipelines all compile). News widget (NewsToday2) renders with Metal.
  (3) bridge: `SWEET_M2V_MESSAGES=PATH` (via /var/local/mtl2-env) logs every distinct runtime message
  (degradations: why a texture is shadowed etc.). (4) bridge patch 0101: padded textures that render alias too.
  (5) tools/flame-metal2-install.sh restarts backboardd with `psx -k` (killall does not exist: it never did).
- **Tried, reverted**: Turnip `requiresDedicatedAllocation` off for host allocations
  (work/m2v/turnip-host-alloc-not-dedicated.patch.tried): CA's 1125x2436 frame then renders straight into the
  IOSurface (no shadow copies) but comes out in horizontal bands with text missing, and GPU time went up.
  The frame is still a shadow image copied in/out each frame (~1 ms + uploads); fixing the linear render is next.
- **Frame time now**: swipe benchmark `work/m2v/swipebench.sh RUN` (home pages 1<->2, averages the FB 5 s
  windows) ~19-26 fps; backboardd ~1.8 submits and ~10-15 ms GPU per frame, run in series with CA's CPU work.
  Turnip trace: `printf "MESA_GPU_TRACES=print_csv\nMESA_GPU_TRACEFILE=/var/local/gputrace-bb.csv\n" >
  /var/local/mtl2-env`, restart backboardd, remove the file; `python3 -I work/m2v/trace-sum.py CSV`.
- **Safari**: WebKit2GPUProcessForDOMRendering prefs deleted (iOS 16 draws DOM in WebContent anyway). Pages now
  show their top tiles (Wikipedia header/title) but the body below the first tile row stays white; WebContent
  without Metal draws nothing (black).

### Metal at user quality, step 3: memory, timing, async present, every process on Turnip (2026-10-22, 04:50) - kernel 0119
Written up afterwards (the session hit its usage limit at 04:51). Tour with `mtl2-clients` = `*`:
`work/scenes/m2v-1/sheet.png` - lock, home, Settings, General, Control Center (sliders right now), Spotlight,
Clock, Calendar, Notes all correct. Wrong: Today widgets (white panels, `work/m2v/today5.png`), Safari page white,
Photos crashed once.
- **Kernels**: 0117 pool accounting (whole pool + per v2 client, logged per 32 MB), 0118 per-submit CP_ALWAYS_ON
  stamps + "SWEET GPU: 5s: N submits, GPU busy X ms" line, **0119** present not waited for (two IB slots; waits
  only for the present two back; surface remaps wait for queued presents). flame-fresh-run.sh boots 0119.
- **Bridge patch 0100** (phone/metal2/bridge/patches/0100-sweet-8mb-memory-blocks.patch): runtime memory blocks
  64 MB -> 8 MB. backboardd ~69-96 MB of pool, SpringBoard ~32 MB.
- **SweetMetal2.mm**: 10 s telemetry line in mtl2.log, logs image bases (atos), `/var/local/mtl2-env` KEY=VALUE
  lines go into the process env before the device opens (e.g. `MESA_GPU_TRACES=print_csv`, TU_DEBUG).
- **Frontend**: newTextureLayoutWithDescriptor:isHeapOrBufferBacked: (SweetTextureLayout, per-format texel
  table; row/width was wrong for narrow textures), newTextureWithBytesNoCopy:... (linear texture over a
  no-copy buffer). AegirPoster's abort came from layer_private_check_pixel_format returning nil; no abort
  seen after these.
- **Turnip**: `TU_FETCH_GMEM_MIN_DRAWS` env in tu_autotune.cc (single-prim sysmem passes go GMEM only from N
  draws; no fps effect).
- **Frame rate (open)**: swipe benchmark 9.6-17.6 swaps/s; GPU busy 29-47%, mean submit 7-9 ms, longest ~25 ms.
  0119 cut the present from 15-30 ms to ~1 ms with no fps change. `tools/flame-profile.py PROC N` (pstack
  sampling + symbolication): backboardd mostly in semwait; SpringBoard idle; touches arrive at 100 Hz.
  Full-screen pass interval median 21.5 ms (p90 67 ms) in the GPU trace.

### Metal at user quality, step 2: THE HOME SCREEN RENDERS THROUGH metal2vulkan + TURNIP (2026-10-22, 04:00) - kernel 0116, sweetmtl=2
`work/m2v/home-metal2-first.png`: lock screen, unlock, home screen (icons, labels, Batteries widget, blurred dock,
badges) composited by backboardd through **SweetMetal2.bundle** = UniXNU's frontend (phone/metal2/
NativeBridgeMetalFrontend.mm, patched for iOS 16) + the metal2vulkan bridge (phone/metal2/bridge, static lib from
`tools/build-m2v-bridge.sh`) + Turnip (inside the bundle). CA's shaders are translated and compiled **on the phone**
(no host templates): ~270 render pipelines on a cold cache, 3-30 ms each; cache in /var/local/m2v-cache/<process>.
- Boot: `tools/flame-fresh-run.sh NAME 1800 sweetmtl=2` with FLAME_KERNEL=work/boot/kernel-0116 (**kernel 0116**:
  sweetmtl=2 names the SweetMetal2 plugin/class, 1 keeps SweetMetal v1). Build `phone/metal2/build.sh`, install
  `tools/flame-metal2-install.sh` (restarts backboardd). Log: /var/local/mtl2.log + console ("MTL2[pid name]:");
  pipeline journal /var/local/m2v-cache/<proc>/pipelines.log (S = specialisation, R = render pipeline).
- Who gets a device: backboardd always, others listed in /var/local/mtl2-clients ("*" = all). **SpringBoard must be
  listed** or the unlock swipe does nothing (now: `SpringBoard`).
- iOS 16 fixes to the frontend: `[super init]` before returning the frontend's device (else -[_MTLDevice dealloc]
  crashes on unset queues); 3-wide vector function constants (CA's fixed_frag uses uchar3 fc_color_opcodes0/1:
  "Metal failed to specialize fragment function"); no 536-byte _MTLDevice check (iOS 16: 528); iPadOS-offset
  diagnostics removed; newPipelineLibraryWithFilePath: -> nil; sys_dcache_flush overridden with dc civac.
- Open: ~60-130 MB of GPU pool per process (393 MB with backboardd + SpringBoard: pool is 944 MB); posters
  (AegirPoster crash-loops without a device); apps; fps (17 during the unlock animation); missing
  newTextureLayoutWithDescriptor:isHeapOrBufferBacked: (SpringBoard), newTextureWithBytesNoCopy:... (backboardd).

### Metal at user quality, step 1: Mesa's Turnip runs on the phone (2026-10-22, 03:40) - kernel 0115
Plan and the reasons for it: **METAL-NEXT.md** (port UniXNU's on-device stack: Metal frontend -> metal2vulkan
(AIR -> SPIR-V, Metal API on Vulkan) -> Turnip with a darwin kernel backend -> our kernel; host-made templates
cannot cover apps). Done so far:
- **Turnip for iOS 16** (`tools/build-turnip-ios.sh`): work/turnip/mesa = Mesa 85fda3b3 + UniXNU's 8 patches/mesa
  (all apply), venv in work/turnip/venv (meson/mako/pyyaml), Homebrew glslang/bison/flex. Output
  work/turnip/build-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib (18 MB, min iOS 16.0); on the phone in
  /var/local/lib. It opens IOAcceleratorES with type 0x4147 and tunnels the ABI through selector 0.
- **Kernel 0115** (`xnu/patches/0115-flame-gpu-v2-client-turnip.patch`; flame-fresh-run.sh boots it; FLAME_KERNEL=
  overrides): SweetAccelerator v2 user client = NativeAdreno ABI v1 (iokit/Kernel/arm/SweetGPUABI.h, copy of
  phone/turnip/NativeAdrenoABI.h): 4 GB GPU VA window per connection from 0x10_0000_0000, client-chosen IOVAs,
  WC CPU maps, BoImport (client pages; client does `dc civac`, EL0 cache ops are enabled), up to 16 IBs per submit
  wrapped like msm's a6xx_submit (CCU invalidate, fence by CACHE_FLUSH_TS; v1 async submits now too), ns fence
  waits, per-client fault count from a ring of reset "lost fence" ranges, CP_ALWAYS_ON timestamp.
  Hangs: progress-based (no fence progress for 1 s with work queued; present: 500 ms) instead of fixed timeouts;
  after a reset the fence memory jumps to the last submitted fence; the log shows rb rptr, IB1/IB2 base+rem.
- **The big trap (cost an hour):** SWEET has no PPL, hence no `__ARM_PTE_PHYSMAP__`: the physmap maps all managed
  RAM with cacheable blocks that pmap_set_cache_attributes cannot change. Any WC/NC view of a managed page is a
  mismatched alias: CPU writes through it got lost / reads went stale (Turnip's command buffer read back 15 of
  410 dwords; `v2test` showed 2496 of 16384 words wrong after 200 ms). Fix: v2 buffers come from a **pool of RAM
  XNU never manages**: 0xc4000000-0xe0000000 + 0xe1000000-0x100000000 (944 MB; WiFi radio keeps 0xe0000000+16 MB),
  sweet_gpu_pool_alloc/free, zeroed through a WC kernel view; freed buffers still mapped by a task wait in a
  zombie list. (Also fixed, but not the cause: pmap_sync_page_attributes_phys shifted a 32-bit ppnum_t; and asking
  IOKit for a WC mapping of IOMalloc'd kernel memory makes vm_object_change_wimg_mode walk kernel_object: never.)
  Note SweetSurface maps IOSurfaces WC in the kernel (kernelMap/mapCache): the same mismatched-alias hazard.
- **Tests** (phone/turnip, `build.sh`): `v2test` (ABI without Turnip: CPU/GPU coherence, CP_MEM_WRITE, import) and
  `vktest` (Turnip: device, 1 MB fill+copy, compute shader compiled on the phone 2.6-10 ms, triangle pipeline
  5-7 ms, readback exact): **ALL PASS** with UBWC on, sysmem and forced GMEM. `TU_DEBUG=noubwc|sysmem|gmem|startup`,
  `FD_RD_DUMP=enable,full FD_RD_DUMP_PATH=/var/local/rd` writes .rd captures (`tools/fd cffdump`).
- metal2vulkan check on the Mac (scratchpad build, see METAL-NEXT.md): reads iOS 16.7 AIR; 230 of 242 logged CA
  pipeline keys translate (the 12 others are RenderBox/SiriUICore keys logged without their constants).
- **Next:** metal2vulkan bridge for iOS (UniXNU scripts/build-native-metal-bridge.py: pin 720ef1ce + 5 patches),
  a phone test that draws a QuartzCore pipeline through it, then the Metal frontend for backboardd.

### WiFi works: scan, WPA2 join, DHCP, internet (2026-10-21, 23:10) - userland only, no kernel change
After a fresh boot: `wland up 0 > /var/local/wland.log 2>&1 &` (0 = run forever; `wland up SECS` stops after
SECS and the utun goes with it). With `/var/local/wlan.conf` (`ssid=NAME`, `psk=PASS` lines; **phone only,
never a repo**) it scans, joins and gets a lease in ~25 s; without it, it only scans. Run wifi12 (kernel 0114):
joined the home network (2447 MHz, -41 dBm), DHCP lease /24 from the router, DNS 1.1.1.2/1.0.0.2, **utun3 is
the PrimaryInterface**, `nettest` DNS/TCP/HTTPS 200 over WiFi, 5 MB in 7.1 s (~5.7 Mbit/s). Not in the UI yet.
- **phone/wifi/radio.c**: port of ios-topaz TopazModem 0123/0125/0126/0129/0130 (ath10k snoc/ce/htc/wmi-tlv/htt
  for WCN3990) to /dev/pmem: copy engines, HTC, WMI INIT, HTT rx ring, passive scan (37 channels), station
  (auth/assoc via WMI mgmt tx, PEER_ASSOC, VDEV_UP), WPA2-PSK 4-way handshake (CommonCrypto: PBKDF2, HMAC-SHA1,
  RFC 3394 unwrap; self-test at the first join), HTT data TX/RX as native wifi <-> Ethernet.
  **phone/wifi/ipnet.c**: utun (PF_SYSTEM control socket; kern_control structs declared by hand), ARP both
  ways, DHCP client (T1 renew), lease on the utun (p2p to the router) + `netsvc up utunN ...`.
  wland.c: SMMU step first in `up`, radio after mission mode, `wland smmu`, wlan.conf, join retries (4 x 5 s),
  stats every 30 s. wlfw.c: shadow register table now 12 entries (was 9; Linux sends 12).
  **phone/tools/netsvc**: `netsvc up IF ADDR MASK ROUTER DNS [DNS2]` / `down IF` / `show IF` (no args = ue0
  as before; one service ID per interface name). Build: `ios-cc -fobjc-arc -framework Foundation -framework
  SystemConfiguration`, sign with netsvc-ents.plist.
- **Flame facts that made it work first time:** WLAN DMA = apps SMMU (0x15000000) stream **0x640/0x1** -> its own
  bank with translation off (CBAR 0x1f000, SCTLR 0xe0, as ABL's banks and our USB stream): came out as CB5 via
  SMR5; must be set before the WLAN touches host memory (`wland up` does it before the modem boots). CE regs
  0x18A40000 (+0x1000 per CE, RRI-in-DDR wrapper +0xC004), shadow regs 0x18832000 + 4*ce. Rings/buffers in RAM
  XNU does not manage below 4 GB at **0xE0000000** (pmem maps it as device memory: no cache maintenance; WMI
  host-memory chunks only take 32-bit addresses unless service 179; this firmware asked for none). Without the
  HTT host rx ring no beacon reaches the scan (topaz's finding). Firmware: WLAN.HL.3.2.0.c9-00554, chip 0x30224.
- Timeline (wifi12): WLFW mission at 18.0 s, HTC READY at once, WMI READY +170 ms, scan 3 s, auth+assoc+4-way
  170 ms, DHCP 4 s (the first DISCOVER is lost, the retry 2 s later works).
- **Next, in order:** (1) a launchd job for `wland up 0` (and stop relying on a fresh boot: `wland shutdown;
  wland up` restarts the modem, untested with the radio); (2) link loss: deauth/kickout tear down but nothing
  rejoins yet; (3) CPU: wland polls every ~1-2 ms through pread/pwrite syscalls - measure, then batch or
  sleep longer when idle; (4) **Settings/wifid** (see below); (5) the radio in the kernel for speed (topaz's
  code is kernel code already; keep wland for the modem services).
- **wifid / Settings notes** (from IO80211.framework, wifid, configd; nothing built yet): launchd starts wifid
  only with an ADT node named `wlan` (`_LimitLoadToDeviceTree` wlan, marconi-wifi) - add to mkadt.py; we also
  lack topaz 0128 (IOKit never quiesces; wifid hung their boot). wifid matches IOKit class
  **IO80211SkywalkInterface** and waits for APPLE80211_M_DRIVER_AVAILABLE. Apple80211 finds the driver by
  getifaddrs + SIOCGIFMEDIA (802.11 media) + IOService with `IOInterfaceName`=en0 and
  `IO80211InterfaceRole`="Infrastructure", then IOServiceOpen type 0: selector 0 = ioctl wrapper (0x3C30-byte
  struct, data first, `apple80211req` {ifname[16], type, value, len, data} at 0x3C08; first call type 43
  DRIVER_VERSION), selectors 1/5 = 1-byte flags (use IOUC / IOUC event pipe), 3 = notification port,
  IOConnectMapMemory type 1 = event ring; with the flags off it falls back to ioctls SIOCGA80211/SIOCSA80211
  and kernel events (vendor 1, class 6, subclass 1; data = ifname[16] + payload). Ioctl type numbers = the
  classic macOS apple80211_ioctl.h (SSID 1, SCAN_REQ 10 with 0x1550-byte request, SCAN_RESULT 11 = one
  0x48C-byte result per GET until errno 5/0xE0820446: RSSI @0x16, cap @0x1A, BSSID @0x1C, rates @0x22/0x24,
  SSID len/bytes @0x60/0x61, IE len @0x8A, IEs inline @0x8C; ASSOCIATE 20 with the PMK from
  Apple80211ParseWPAPassword; events SCAN_DONE 10, LINK_CHANGED 4, RSN_HANDSHAKE_DONE 20). configd/SC needs a
  registry chain bus -> controller conforming to `IO80211Controller` (IOMACAddress, IOSelfNamed=true so
  InterfaceNamer skips it) -> interface conforming to `IONetworkInterface` (IOInterfaceType 6, BSD Name,
  IOInterfaceNamePrefix/Unit, IOBuiltin, IOPrimaryInterface). The real kexts (IO80211Family 1200.13, classes
  IO80211APIUserClient, IO80211AsyncEventUserClient, IO80211IOUCEventPipe) are in
  downloads/ipsw/kernelcache.release.iphone10b (old prelinked format: `ipsw kernel extract` refuses it).

### First third-party app: YouTube (YouTubePlus 5.2b4 IPA) runs (2026-10-21, 22:45) - kernel 0114
YouTube 21.13.6 (decrypted, ad-hoc signed, iOS 26.2 SDK, min iOS 16.0; YouTubePlus tweak dylibs + CydiaSubstrate
in Frameworks/) launches with Metal on (`sweetmtl=1`), shows the welcome screen, "Use YouTube signed out" goes to
the home tab, and `openapp ... youtube://www.youtube.com/watch?v=...` opens a watch page (title, channel, likes,
comments, related thumbnails all render). **Video does not play yet** (black player, see below).
- **Install recipe (no installd):** unpacked IPA in `ytapp/Payload`; stream it gzipped to a staging dir, then
  rename into /Applications (same filesystem) and rebuild LaunchServices:
  `COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 --uname root --gname wheel -czf - -C
  ytapp/Payload YouTube.app | tools/flame-ssh 'toybox zcat | tar -xpf - -C /var/local/tmp/yt'` (~2 min for 308 MB;
  zcat is a toybox applet without a symlink), `mv /var/local/tmp/yt/YouTube.app /Applications/`, then
  **`lsreg -r`** (registers it as a System app; `lsreg APP` alone returns 0 and logs nothing). Keep it
  root-owned: MobileInstallation could not validate the signature and tried to chmod -x the executable.
- **Kernel 0114** (`xnu/patches/0114-flame-amfi-dyld-policy.patch`): every @rpath framework failed with
  "security policy does not allow @ path expansion". dyld asks `__sandbox_ms("AMFI", 0x5a, {u64 in, u64 *out})`
  (amfi_check_dyld_policy_self, checked in the phone's dyld) and treats an error as "allow nothing"; our
  policy was named "SWEET AMFI", so the call got ENOPOLICY. The policy is now named "AMFI" and answers 0x5a:
  @ paths always; DYLD_* vars, fallback paths, interposing, insert failures, embedded vars unless the process
  is restricted/encrypted (so `DYLD_PRINT_LIBRARIES=1`, `DYLD_INSERT_LIBRARIES` work now). Other AMFI calls still
  get ENOPOLICY (lsd uses 96). flame-fresh-run.sh boots **kernel-0114**.
- **fairplayd.H2 removed from launchd** (`/System/Library/xpc/launchd.plist`, key
  /System/Library/LaunchDaemons/com.apple.fairplayd.H2.plist; backups `launchd.plist.pre-fairplay` on the phone
  and in work/userland, work/userland/launchd.plist.cur = new). It exits silently right after a container lookup
  and launchd relaunched it every 10 s, so MIG requests to com.apple.fairplayd.versioned queued forever.
  locationd's `-[CLMescalSigner initInSilo:]` (FairPlay SAPInit) blocked on one at startup, which deadlocked
  locationd (main thread dispatch_sync on that silo), and every app making a synchronous CoreLocation call
  hung with it (YouTube's main thread: NSXPC to locationd, taps ignored). Without the job the lookup fails
  fast; no new crash loops seen.
- **New tool `openapp BUNDLE_ID [URL]`** (phone/tools/openapp.m, entitlement com.apple.frontboard.launchapplications;
  in /var/local/bin): launches through FBSOpenApplicationService, URL as __PayloadURL. Unlock first
  (`flame-cmd.py swipe 540 2270 540 1100 300`). Tap "Use YouTube signed out": `flame-cmd.py tap 538 2095`.
- **Playback (next):** mediaserverd crashes when the player starts: (1) SIGBUS in H264SW.videocodec (software
  H.264 decoder) writing to 0x210670040, which is inside another dylib's __TEXT, so the decoder writes through a
  wrong pointer. My guess is the output CVPixelBuffer/IOSurface plane base (biplanar 420v/420f) is wrong:
  check IOSurfaceGetBaseAddressOfPlane vs our kernel's planar layout (see 0104). (2) SIGABRT (uncaught C++
  exception on a dispatch source) after it restarts. There is no audio device either (CoreAudio "no object
  with given ID 0", AQMEIO -66680), so the audio session has nothing to route to.
- Rendering glitches in YouTube (grey dim over the top of the home tab, black lower half, garbled skeleton
  placeholders): run the template loop (mtl-keys.log -> tools/mtl-templates.py -> flame-metal-swap.sh) for
  the pipelines YouTube's views introduced.
- Optional: MobileInstallation logs "Code signing identifier (YouTube) does not match bundle identifier", so LS
  has no entitlements for it. A copy re-signed with `-i com.google.ios.youtube` and the same entitlements
  removes that if it matters.
- USB Ethernet moves only ~1.2 MB/s (ssh and the kernel file server alike), far below USB 2.0. Worth a look:
  it slows every push (templates, apps).

### Metal glitches, stage 11 (2026-10-21, 19:45) - kernel 0113, SweetMetal + host template fixes
**Method:** `tools/flame-scenes.sh OUTDIR [SCENE...]` drives a fixed tour (lock home settings general control
notifications today spotlight clock calendar notes photos safari) with flame-cmd taps/swipes and grabs each with
flame-shot; `tools/scene-sheet.sh DIR` makes a contact sheet. Reference set from the CPU renderer (boot with
-G, no sweetmtl): `work/scenes/cpu/`. Latest Metal set: `work/scenes/mtl-0113/`. Note: CA's software
renderer has no blur, so CPU Today/Spotlight backgrounds are black where Metal's blurred wallpaper is right.
**Fixed today:**
- **attachment_clear** (`path_blit_vert + attachment_clear_frag`, att0 wm 0): was "writes nothing" and skipped,
  so CA's memoryless attachment 1 was never cleared -> garbled glyphs (Wi-Fi-off icon, play button, Spotlight
  "Show More"). Rule now (mtl-templates.py and SweetMetal): att0 masked + att1 written => dest 1.
- **Non-standard blend factors** were captured as premultiplied source-over: the key now carries
  ` bf s,d,op,sa,da,aop` (Metal enums) per attachment only when not One/OneMinusSrcAlpha/Add (old keys
  unchanged); mtlkeys parses it, mtl-templates maps to Vulkan, vkdraw takes "s,d,op,sa,da,aop" as blend.
  CA uses dest-out (0,5), additive (1,1), (4,1) and source-atop (8,5): masks (lock-screen Head mask) and
  vibrant text (Spotlight "Search" placeholder) now right.
- **mtlkeys.py**: (1) parsed by (pid, pipeline#) across concatenated logs of many boots -> keys/libs crossed
  and lost; now in log order. (2) empty `| vfc  | ffc ` keys (RenderBox) were dropped as "old format".
- **air2glsl**: icmp `samesign`, dynamic sub-word loads (i8/i16/half at index*stride: RenderBox per-instance
  records), sign extension of narrow ints (sext, air.convert from s.i16), llvm.fmuladd/fma/fabs/minnum/...,
  llvm.copysign, air.get_width/height_texture_2d (GL_EXT_samplerless_texture_functions), vectors wider than
  4 as arrays; airspec inlines everything (-inline-threshold=100000: variable_blur's bicubic_upsample).
  ~60 new templates (chronod/RenderBox plane/mask/primitive_gradient/image, variable_blur, blend-factor keys).
- **No Metal device where the GPU cannot be opened** (SweetMetal initWithAcceleratorPort: returns nil;
  MTLRegisterDevices skips nil): sandboxed extensions got a device that drew nothing, so CoreImage/VT there
  never fell back to the CPU. Took away the dark box behind the lock-screen clock.
- **Lock screen black block = HEIC**: the default wallpaper (CollectionsPoster, "WWDC 2022" CA package) is 5
  HEIC layers and **HEIC decodes to nothing on this phone** (no HEVC hardware; ImageIO returns an image with
  no data: `imgtest` in phone/tools shows it; JPEG/PNG fine). CA's Metal path uploads the zeros and draws the
  opaque "Head" layer black; the CPU renderer skips it. Workaround applied on the phone: the layers converted
  to PNG on the Mac (sips) and written over the .HEIC files (ImageIO sniffs content), originals in
  /var/local/wallpaper-heic-backup. Lock screen now shows the real iOS 16 wallpaper. Real fix: a software
  HEVC decoder (Photos, other HEIC wallpapers still blank).
- **Kernel 0113**: SweetAcceleratorUserClient::free waited for nothing before unmapping (comment said
  submissions are synchronous - false since 0109): now waits for the client's last fence (crash-looping Metal
  processes like MusicWidgets exit with work queued). flame-fresh-run.sh boots 0113.
**Open (in order of visibility):**
- **Control Center sliders**: upper part black and/or green battery-ring arcs inside; nondeterministic across
  boots (black, battery-widget fragments). Readbacks show the slider's offscreen pass (207x459: backdrop copy
  aa4cd065362b -> a429968600a5 (att0 over att1, a no-op as expected) -> filter 35179d026854 -> 538215004f47)
  produces the right material, and the composite 2899e76dd60b right after it puts it on screen correctly
  during the opening animation; the static final frame is wrong. Not luma_boost, not async (mtl-sync), not
  the 1127x2438 copy-back. Suspects: what CA draws in the settled state (damage-tracked frames; dumps of the
  last frames show only glyph draws over the slider), and cross-surface aliasing (battery widget pixels).
- **Widgets' text/content** (Today view panels empty, calendar widget sometimes black): RenderBox's text and
  AA go through att1 = RG16Float (fmt 65) accumulator pipelines (mask_accumulate, accumulator_color: att0
  wm 0 or both attachments written at once) -> needs real MRT (RB_MRT[1] + capture with 2 attachments) and an
  RG16F scratch format (RB_MRT[0].BUF_INFO for dest-1 draws, fbfetch tex 14 format), plus RenderBox's
  depth/stencil (setDepthStencilState/setStencilReferenceValue are "no signature" = dropped).
- Siri chip (air.sample_texture_1d), MapKit snapshots (VectorKit shaders: no templates; maps stay blank).
- Batteries widget glyph occasionally garbled (chronod; same aliasing suspicion).
**New debug switches (files in /var/local, read by SweetMetal):** `mtl-skip` (template hashes not drawn;
read once per process: restart backboardd), `mtl-readback` ("WxH" pass end, "WxH MIN" pass of >= MIN draws,
"WxH @N" after draw N, "WxH #HASH [TEXW]" after the first draw with that template [and texture 3 width];
one-shot; GPU CP_MEMCPY of att0 + the att1 scratch -> rb-att0.raw/rb-att1.raw/rb-info.txt), `mtl-dump` now
takes a draw count (`echo 1500 > /var/local/mtl-dump`; pass lines show the clear colour), `mtl-sync`
(synchronous submission), `mtl-passflush` (UCHE flush after every pass). Helpers:
tools/mtl-drawsin.py DUMP X0 Y0 X1 Y1 (draws in a rect from a dump), tools/mtl-tregs.py HASH... (a template's RB_MRT0/blend registers; run from the repo root).
- Also: a pmap_assert_free panic once (run cm3, 395 s) while MusicWidgets crash-looped ("Too many corpses");
  page freed with a user mapping in another pmap. Not seen again yet. FlameBridge crashes on a flame-files
  PUT whose path has spaces/'@' (put to a simple name and mv over ssh). The scratch for CA's attachment 1 is
  ONE shared growing buffer (a6draw.m scratchTargetVA:), not one per size as stage 7 said.
- Reference repos: pulled (all ios-sweet forks current; UniXNU submodules drivers/NativeAdreno,
  drivers/NativeMetal, kits/AirNir, kits/AdrenoPM4 checked out). For WiFi, port from ios-topaz TopazModem
  (patches 0113-0130) instead of writing CE/HTC/WMI/WPA2 again.

### Rounded display corners (2026-10-21, 18:30) - kernel 0112, default on
The iOS image (1053x2280 fit, 13 px black bars) now has **Apple's continuous corners** (UIBezierPath roundedRect
since iOS 7: 3 cubics + a short line per corner, 1.52866483 r along each edge), anchored to the *image* rect, so
the outline runs from the panel's top edge into the image's side edge without a step. Radius = the iPhone X's
39 pt (117 px) scaled to the fit = **110 panel px** (extent 169 px). `sweetfb_corner_radius=N` (0 off, max 160).
- History: a Codex session (14:46-15:40, in parallel with stage 9/10) wrote **0110** (CPU mask after the copy
  -> flicker) and **0111** (GPU: whole frame into a pool staging image, 2D-engine blend of mattes, full copy to
  the panel; 4th-power superellipse at the panel corners). 0111 worked but the curve was nearly square and
  mostly hidden under the glass, crossed the side bar at an angle, and doubled the present (GPU blit 2.44 ms vs
  1.26 ms). 0111 was uncommitted; committed as-is, then 0112 on top.
- **0112**: `iokit/Kernel/arm/SweetDisplayMask.h` (integer flattening of the curve, per-row coverage with 16
  sub-rows, exact horizontally; within 0.035 px of a float reference), `sweet_gpu.c` (same scaled blit three
  times with the **2D scissor** - GRAS_A2D_SCISSOR_TL/BR 0x840a/b + BLT_CNTL bit 16 - middle rows straight to
  the panel, corner bands to the stage; 4 mattes blended (RB_A2D_PIXEL_CNTL 0x3809c181 = SRC_ALPHA /
  ONE_MINUS_SRC_ALPHA); bands copied to the panel; GPU blit **1.73 ms**), SweetFramebuffer CPU copy masks
  each band row while copying (from the source, no read-back), SweetHID drops contacts that start in a masked
  corner but no longer drops contacts starting in the side bars (Codex's 0111 did: edge swipes).
- Checked (run cm3): `tools/flame-shot` rows are symmetric on all four corners; camera crops look clean, the
  dock is concentric with the bottom corners. Fit vs glass: Google's flame outline (`config_mainDisplayShape`
  in device/google/coral flame overlay, 176 px footprint) is outside our curve below row 3.
- `tools/flame-fresh-run.sh` boots **kernel-0112** now. Note `tools/build-xnu.sh` needs an absolute LOG path.
- Possible trim: every a6_blit_ib re-emits blit_init + flushes (9 blits per present now); one init per IB
  would save most of the +0.47 ms.
- First boot attempt of this session fell back to fastboot within ~10 s (no XNU log); the identical retry
  worked. The phone had been sitting at ABL's "Press power button to resume" (Power pressed during the
  unlocked warning).

### Reference repos fetched (2026-10-21, 18:30)
- New forks of ios-sweet: **ios-topaz** (Redmi Note 12 4G, SM6225): **Wi-Fi works** - TopazModem, patches
  0113-0130: modem PAS boot, GLINK/QRTR, rmtfs/pd-mapper/tftp, WLFW, then **copy engines, HTC/WMI/HTT, scan,
  association, in-kernel WPA2-PSK, Ethernet data path, iOS sees it through wifid**. Our WiFi is parked at
  exactly the step before (WLFW up, next CE/HTC/WMI): read topaz first. **iPadOS-nabu** (Xiaomi Pad 5,
  SM8150-AC: same SoC family and Adreno 640 as flame; no GPU/WiFi yet). **ios-lemonadep** (lemonadep = OnePlus 9 Pro; early, kernel CI stage).
- UniXNU (56 new commits): native Metal on the Adreno 740 (sheng) - submodules drivers/NativeAdreno,
  drivers/NativeMetal, kits/AirNir (AIR -> NIR), kits/AdrenoPM4 on github.com/UniXNU (not checked out).
- ios-sweet: +site docs, mkadt panic region (we have our own since 0098). Only fetched; local branches of
  ios-sweet (3 behind) and UniXNU (56 behind) not fast-forwarded.

### Metal driver, stage 10 (2026-10-21, 15:10) - asynchronous submission (kernel 0109)
- **Kernel 0109**: user client sel 6 = submit without waiting (returns the fence; ring space checked against
  RPTR), sel 7 = wait for a fence (polls without the GPU lock; on timeout logs + `a6_recover`). File server PUT now
  truncates (`vnode_setsize` after open): flame-files put of a smaller file is safe again.
- SweetMetal: the queue submits each command buffer with `submitAsync` and completes it in order on a serial
  dispatch queue after `retire:` (fence wait, then release of kept resources + surface sweep). Blit encoders still
  submit earlier work synchronously. Stream pool and surface cache are @synchronized.
- Result (run mtl16): no faults, all draws on the GPU, but scrolling stays ~21-24 fps: backboardd's threads sample
  idle (pstack: all in mach_msg), so the limit is upstream (app commits / frame pacing via SweetFB vsync), not the
  driver. Next perf look: SweetFramebuffer vsync/swap pacing and CA's frame scheduling.
- **Phone state at stop (17:25):** not on USB at all (no fastboot/adb/USB device); camera shows it moved, screen
  off, cable apparently unplugged. Reconnect, then `tools/flame-fresh-run.sh NAME 1800 sweetmtl=1`.

### Metal driver, stage 9 (2026-10-21, 15:00) - clears, write masks, buffer textures; lock screen still off
- **Load action Clear** was ignored (offscreen targets kept stale contents): `gpuClear:` draws one quad over the
  target with the hand-made fixed_color template (clip-space corners, identity MVP, clear colour as vertex colour).
- **Write masks**: keys now carry ` wm N` when a colour attachment's mask is not RGBA (most `att1` have wm 0, so
  most keys changed once; templates were regenerated). Pipelines whose drawn attachment has mask 0 are not drawn
  (attachment_clear clears only the memoryless attachment). vkdraw takes the mask (8th arg).
- Buffer-backed textures (`newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:`,
  `newTextureWithDescriptor:offset:bytesPerRow:`): address = buffer + offset, shadow copy when not 64-aligned.
  getBytes:...slice: implemented; getBytes invalidates the CPU cache first.
- Tracing: `touch /var/local/mtl-trace` before (re)starting a process logs creation/uploads/blits/passes of
  textures >= 512 px; the draw dump also lists render passes ("== pass WxH fmt load store draws").
- **Lock screen (open):** the upper wallpaper area is black and the clock sits in a dark box (the box is the
  blurred backdrop of that black area). The wallpaper textures (1271x1823 BGRA8, 1105x1160 RGBA8) are uploaded
  by replaceRegion with all-zero data, i.e. the poster snapshot images CA gets are empty. Suspect PosterBoard /
  PridePosterExtension (Metal pipelines from their own metallibs: ribbon/shadow; no templates until their keys
  show up in mtl-keys.log) or snapshot files rendered in an earlier boot. Home-screen wallpaper is fine.

### Metal driver, stage 8 (2026-10-21, 14:35) - apps render, GPU recovery, 26 fps scrolling
Run mtl15, **kernel 0108** (flame-fresh-run.sh boots it). Home screen, widgets, Settings and Safari render on the
GPU; scrolling Settings ~26 fps (CPU path ~6). Every backboardd draw is on the GPU unless "5s:" lists it.
- **GPU recovery (kernel 0108)**: `a6_recover()` = RBBM_SW_RESET_CMD pulse + CP init again (hw_init registers, SQE,
  ME_INIT) with the zap shader **resumed** (SCM boot svc 0x0a set_remote_state(0, 13); rc 0) instead of reloaded;
  page tables/SMMU kept. Called when a user submit or a present fails. Tested: `mtlk fault` (CP_MEM_WRITE to an
  unmapped VA) -> fault, timeout, "recover: soft reset + CP init -> 0", later submits and the UI fine.
- Kernel 0108 also: completion polling with 20 us sleeps (was 1 ms: every submit took whole ms); 0107: GPU perf
  vote from boot-arg `sweetgpufreq=1..5` (default 4 = 499 MHz; was 257 MHz) - little effect, GPU ~6 ms/frame.
- Lines/points: CA draws edge antialiasing as 1-px lines (stride-16 layout pipelines). Line draws use a separate
  capture with line topology (`<hash>-l.json`, mtl-templates.py makes them for vertex_layout=1 keys; DI_PT 2/3;
  points DI_PT 9 have no templates yet). Non-indexed and instanced draws (CP_DRAW_INDX_OFFSET auto-index) work;
  first vertex != 0 is skipped. air2glsl: dynamic GEPs (offset expressions), powr/tan/... + exp10/min3 macros.
- Other render-target formats: pass prologue rt.info/rt.spfmt/rt.lrzfmt patched from the target's FMT6
  (BGRA8, RGBA8, R8, RGBA16F, BGR5A1, B5G6R5); 16-bit packed texture formats added (Metal 40/43).
- **Per-function metallib paths**: SweetMetal logs `pipeline N lib: VPATH | FPATH`; libraries from memory are
  saved in /var/local/mtl-libs (777). mtl-templates.py fetches each metallib once (work/mtl/libs) and specialises
  from it: SpringBoard's SiriUICore pipelines have templates now.
- **Keys log**: pipeline keys also go to append-only **/var/local/mtl-keys.log** (never truncate it). Loop:
  `tools/flame-ssh 'cat /var/local/mtl-keys.log' > work/mtl/keys-phone.log; cat work/mtl/mtl-*.log
  work/mtl/keys-phone.log > work/mtl/all.log; tools/mtl-templates.py work/mtl/all.log; phone/metal/build.sh;
  tools/flame-metal-swap.sh` (now pushes only templates whose md5 differs; ~240 templates, 18 MB).
- Perf counters in "5s:": encode (our CPU time per 5 s), cache (bytes cleaned), GPU (waiting in submit).
  Cache cleaning now skips IOSurfaces whose seed did not change and BO textures without CPU writes (348 -> 18 ms).
  Next perf steps: asynchronous submission (we wait ~6 ms per frame), cheaper encode (~40 us/draw: ObjC
  dictionaries, 3 KB descriptor sets memset per draw), kernel present (~2-3 ms).
- Gotchas: **flame-files PUT does not truncate** (a smaller file keeps the old tail: rm first or put NAME.new +
  mv; to fix in bsd/dev/sweet_files.c). Restarting backboardd locks the phone (`swipe 540 2270 540 1100 300`).
  mtlk is built separately: `cd phone/metal; ../../work/userland/bin/ios-cc -O2 -o out/mtlk mtlk.c sweetgpu.c
  -framework IOKit; codesign -f -s - out/mtlk`.
- Not done: lock-screen poster (black area + boxed clock seen once after a restart), constexpr sampler decode,
  first-vertex draws, points, posters/widgets (chronod/PosterBoard metallibs: keys appear when they render).

### Metal driver, stage 7 (2026-10-21, 13:55) - complete home screen on the GPU (labels, widget, dock, badges)
`work/gpu/shot-metal-home2.png` / `cam-metal-home2.jpg` (run mtl12, kernel 0106, sweetmtl=1): every backboardd
draw runs on the A640 ("5s: ... 561 draws (561 on the GPU), no template:"), no faults.
- **Framebuffer fetch**: air2glsl reads attachment 0 as texture 15 / attachment 1 as texture 14 (`texelFetch` at
  gl_FragCoord, sampler 23); before such a draw SweetMetal emits CCU colour flush + CACHE_INVALIDATE + WFI.
- **CA's memoryless attachment 1** (load/store DontCare; `dest_index=1` pipelines write it, others fetch it): one
  scratch BGRA8 buffer per target size (SweetGPU scratchTargetVA:); a dest=1 draw retargets RB_MRT[0].BASE
  (0x8825/6) to it, with a CCU flush when switching back. Templates for dest=1 use attachment 1's blend setting.
- **Resource lifetime**: SweetCmdStream `keep:`s every texture/buffer a draw uses until submit (CA releases them
  after encoding; freed BOs were unmapped -> GPU read fault far 200ad7840, which also kills the GPU for the boot).
- flame-metal-swap.sh: the mv's run in a loop on the phone (a long command line closed the SSH connection).
- mtlkeys.py skips old-format log lines; `cat work/mtl/mtl-*.log > work/mtl/all.log` + mtl-templates.py is safe.
- Still to do: GPU fault recovery (soft reset + CP re-init + zap resume SMC; now a fault means a reboot), target
  format 43 (CA probe passes), path_blit (dynamic GEP), non-BGRA8 render targets, constexpr sampler decode, other
  metallibs (SpringBoard Siri/shadow, posters, apps), performance (10 swaps/s idle; measure per pass).

### Metal driver, stage 6 (2026-10-21, 13:35) - THE HOME SCREEN RENDERS ON THE GPU
Icons, the gradient wallpaper and the dock icons are drawn by the A640 through SweetMetal (run mtl11, kernel 0106,
`sweetmtl=1`; `work/gpu/shot-metal-home1.png`, camera `work/gpu/cam-metal-home1.jpg`). Missing: icon labels and
status-bar text, the dock backdrop, badge circles (pipelines with framebuffer fetch or the memoryless attachment
`dest_index=1`, and luma_boost) - see "next" below.
- **Template pipeline, generic** (host): `tools/mtl-templates.py LOG...` reads SweetMetal's log (pipeline keys,
  `tools/mtlkeys.py`), specialises both AIR functions (`airspec.py`, cached in work/mtl/spec), translates them to
  GLSL with **`tools/air2glsl.py`** (new: LLVM IR -> GLSL; constant-offset UBO loads as uvec4 words, phis, acyclic
  CFGs via reach flags, ~40 air.* intrinsics, textures/samplers, struct values), captures one draw on turnip
  (`work/mesa/draw/capture.sh VS FS OUT blend layout vkformat`; vkdraw.c now has sets 2/3, vertex layouts, target
  formats) and exports `phone/metal/pipelines/<sha1(key)[:12]>.json` with the resource map under "res". ~50
  templates so far. Loop: use the phone, `tools/flame-ssh 'cat /var/local/mtl.log' > work/mtl/X.log`,
  `tools/mtl-templates.py work/mtl/X.log`, `tools/flame-metal-swap.sh`.
- **Phone:** templates looked up by key hash at pipeline creation ("no template H for pipeline N" logged once);
  per draw it writes A6XX texture descriptors (set 2, 64-byte slots: FMT6/swap/swizzle/pitch/base, formats in
  sweet_fmt6) and sampler descriptors (set 3; constexpr samplers 16+ = linear clamp for now); textures live in GPU
  memory (BOs; IOSurfaces imported, unaligned-pitch surfaces get a shadow copy refreshed by IOSurface seed);
  blits (copyFromTexture/Buffer, fillBuffer) run on the CPU after submitting earlier GPU work;
  newBufferWithBytesNoCopy = GPU copy synced before use.
- Bugs found on the way: (1) Metal's library returns **one function object per name**, so the constants attached
  to it were overwritten by later specialisations -> wrong keys (vertex shader without texcoords: icons
  transparent, gradient flat). Now each specialisation is a `SweetFnProxy`. (2) flame-files PUT does not truncate:
  flame-metal-swap.sh now writes NAME.new and mv's. (3) Template load took ~1 s (128 KB shader hex): trimmed blobs +
  fast hex decode. (4) Page tables: kernel **0106** adds 8 MB (2048 tables) via `a6_pt_extra()`; SweetMetal releases
  surface imports when only it still holds the surface.
- Debug aids: `touch /var/local/mtl-dump` logs the next 60 GPU draws (template, viewport/scissor, 4 vertices,
  textures with checksums) to mtl.log; the "5s:" line gives GPU vs total draws and the top untemplated pipelines.
- **Next:** framebuffer fetch (fs reads the destination: luma_boost, CA blend modes) and the memoryless second
  attachment (dest_index=1: intermediate in tile memory) -> probably a real second colour attachment in memory
  plus reading it as a texture; non-BGRA8 render targets (R8/RGBA16F blur passes); constexpr sampler decoding;
  dynamic GEP (path_blit). Then other metallibs (SpringBoard/PosterBoard/Weather pipelines).

### Metal driver, stage 5 (2026-10-21, 13:20) - THE A640 DRAWS CA's FRAMES (first GPU-rendered pixels via Metal)
- With `/var/local/mtl-tint` present, SweetMetal recolours templated quads red: the panel showed CA's full-screen
  background quad in red, drawn by the GPU into CA's IOSurface and presented (camera-confirmed, run mtl10). Without
  the flag that quad is opaque black, so the screen is black: only `fixed_color` draws (no texture) run so far.
- Fixes that got here:
  1. rdtemplate.py relocates every capture-time global-buffer address: CP_EVENT_WRITE/CP_MEM_WRITE/CP_REG_TO_MEM
     bodies (`pb` patches) and *every* write of a BASE/ADDR register pair (border colours were set twice; only the
     last was patched). Warns on any other capture VA left in packet bodies.
  2. **SweetMTLCommandBuffer -device** was missing (nil): backboardd's CA logged "commandBuffer is associated with
     a different device" for every frame and never submitted (only SpringBoard did). Now returns the queue's device.
  3. setVertex/FragmentBufferOffset:atIndex: implemented (CA rebinds offsets), bytes vs buffer: last binding wins.
  4. **Kernel 0105** (`work/boot/kernel-0105`, patch 0105-flame-gpu-va-reuse): GPU user VAs come from a free list
     (64 KB granules, freed ranges reused so the ~250 page tables in the arena are reused), `a6_unmap()` clears PTEs,
     the user client unmaps buffers on free and on close and reuses handle slots. Before, every backboardd restart
     leaked all its mappings: after two `flame-metal-swap.sh` the arena was full ("map: no page table").
     flame-fresh-run.sh boots 0105.
- **Logging:** the live UDP log drops lines in bursts (most `pipeline N:` lines never arrived). SweetMetal now also
  appends to **/var/local/mtl.log** when it exists (create with `: > /var/local/mtl.log; chmod 666 ...`).
- CA's compositor uses ~40 specialisations of fixed_vert/fixed_frag (pipeline keys in mtl.log: texcoord_count,
  image_function, blend_function, dest_count, coord_function, vertex_layout ...). airspec.py reduces e.g. the icon
  pipeline (image_function0=29, RG8 texture -> .rrrg) to 59 instructions. Plan (in progress): tools/air2glsl.py
  (specialised AIR -> GLSL, generic), capture with textures/samplers in sets 2/3, phone writes A6xx texture/sampler
  descriptors per draw, templates looked up by a hash of the pipeline key, missing keys logged for a host batch job.

### Metal driver, stage 4 (2026-10-21, 12:40) - first GPU draws submitted, two faults fixed/found
**Where it stopped:** CA's compositor draws (pipeline `fixed_color`) reach the A640 as real command streams
(559 dwords per pass, 1 draw) and fault in the SMMU; CA still completes frames on the CPU side, so nothing drawn
yet. Phone at stop: boot **mtl8** (kernel-0104, `sweetmtl=1`, flamereboot 1800 s from 12:22), screen black as
always with sweetmtl=1. Default boots (no sweetmtl) are unaffected.
- **Next fix (not landed, clear):** the pass template's `CP_EVENT_WRITE` packets (pkt7 0x46, prologue/epilogue
  of pipelines/pass.json) still carry turnip's capture-time timestamp address **0x1_0000_4000** (dw1/dw2, not
  relocated by rdtemplate.py). The GPU writes there -> fault `far 100004000 fsynr0 30011` (bit 4 = write). Fix in
  tools/rdtemplate.py (emit a patch name, e.g. `scratch.lo/hi`, for CP_EVENT_WRITE / CP_MEM_WRITE addresses)
  or in a6draw.m emitItems (point k==0x46 bodies with >=3 dwords at a per-stream scratch dword). Look for other
  capture VAs (0x1_xxxx_xxxx) in pass.json/fixed_color.json the same way (scan dws for value hi == 0x1).
- **Fixed (12:20, in the phone's bundle now):** RB_MRT_PITCH / RB_MRT_ARRAY_PITCH are in 64-byte units
  (capture: 0x48 = 4608 >> 6); a6draw.m wrote bytes -> render-target writes 64x too far (`fsr 2 far 200d28f40`).
  beginGPUPass also refuses targets whose pitch/base is not 64-byte aligned.
- **A GPU fault kills the GPU for the rest of the boot:** after a user-submit fault the next kernel present
  times out, sets `gpu_ready = false` (bsd/dev/sweet_gpu.c), and SweetAccelerator then reports not ready
  (`mtlk` and new backboardd: "no SweetAccelerator"). No GPU reset/recovery yet -> reboot per test
  (`tools/flame-fresh-run.sh NAME 1800 sweetmtl=1`). Worth adding: a6 soft reset + re-init on fault.
- **Logging gotcha:** only one `tools/flame-live.py` can hold the UDP listener. A stale one from an older run
  silently captures the new boot's log into the *old* run's live.txt (I lost time reading a stale mtl6 log).
  `pkill -f flame-live.py` before a new run if anything is left over.
- How the GPU path is built (phone/metal; build.sh compiles SweetMetal.m a6draw.m sweetgpu.c, copies
  pipelines/*.json into the bundle; `tools/flame-metal-swap.sh` pushes binary + pipelines and restarts backboardd):
  - a6draw.m: `SweetGPU` (shared user-client connection, BO alloc, IOSurface import cached per surface ID,
    turnip's global buffer relocated once, pool of 1 MB IB + 1 MB scratch streams), `SweetPipelineTemplate`
    (`pipelines/NAME.json`: shader BO copied, static draw-state groups pre-patched into one BO, dynamic groups
    patched per draw), `SweetCmdStream` (pass prologue/epilogue from `pass.json`, per draw: bindless UBO
    descriptor sets 0/1 in scratch [dw0 = addr lo, dw1 = hi | size16 << 17], dynamic groups,
    CP_SET_DRAW_STATE, CP_DRAW_INDX_OFFSET; `submit` cleans caches and submits synchronously, 1 s timeout).
  - SweetMetal.m: SweetMTLBuffer is GPU memory (contents = BO map, gpuAddress = BO VA, so CA's embedded
    pointers work); render encoder tracks buffers/bytes/viewport/scissor, draws indexed quads with the template
    when the pipeline matches `fixed_color` (fixed_vert/fixed_frag, texcoord_count0/1 = 0, image_count = 0,
    BGRA8); queue submit runs the stream before completing the buffers.
  - Templates come from the host: `work/mesa/draw/capture.sh VS FS OUT [blend]` (turnip + drm-shim, sysmem)
    then `tools/rdtemplate.py` export -> JSON (groups with patch points: shader/global/vbN/bindlessN/vp/scissor/
    rt.*/window.br/a2d.br). Current: work/mesa/draw/cap2/pipe.json = pipelines/fixed_color.json = pass.json.
- After the event-write fix, the order of work: see a solid-colour quad (`tools/flame-shot --stride 4352`),
  then tools/air2glsl.py (AIR -> GLSL for CA's specialisations, incl. framebuffer fetch), textures/samplers
  (sets 2/3), clears, non-indexed draws, blit encoder, templates for every CA pipeline.
- WiFi is parked at "firmware up, no scan" (see WiFi part 1); internet works over USB (below).

### Done overnight (2026-10-21, 01:00-02:00): items 1-3 of the ranked plan
- **Panic backtraces** (kernel 0098): `/pram` + `embedded-panic-log-size` in the ADT (mkadt.py, 512 KB at
  0x1fd000000) un-stuck SavePanicInfo (it spun forever without a panic region); unbuffered panic output now goes
  into the persistent log ring (osfmk/kern/printf.c). `tools/flame-panic.sh RUN` symbolicates (atos, run's kernel).
  `tools/flame-cmd.py panic` panics on purpose.
- **Real time** (kernel 0099): SweetPlatformExpert::getGMTTimeOfDay = PM8150 RTC (SPMI 0x6048, read-only) +
  boot-arg `sweetrtcoff`, from `work/rtc-offset` (`tools/flame-rtc.py` calibrates against the Mac; flame-run.sh adds
  it). Publishing IORTC also removed a **30 s wait on every boot** (IOKitInitializeTime). Phone TZ is Pacific.
- **Stock apps on the home screen**: 32 removable apps were staged but never installed. installd failed three ways:
  clonefile unsupported on HFS+ (kernel 0100: copy fallback in clonefile_internal), extension-point lookups, and
  code signature validation (0xe8008001, no AMFI/trust cache). Workaround that works: the apps live in
  **/Applications** (root:wheel, from `work/staged_system_apps.tar`; backup on the phone in
  /var/local/staged-backup), placeholders removed, LaunchServices DB deleted and rebuilt by running
  `runmigrator /System/Library/DataClassMigrators/00LaunchServicesMigrator.migrator` (lsd itself waits for
  "seeding" forever otherwise). Home screen = Apple's default layout, two pages; Weather launches.
- **New phone tools** (phone/tools, in /var/local/bin): `psx [NAME]` / `psx -k NAME` (processes, kill),
  `pstack PID` (thread backtraces + shared cache slide; symbolicate with `ipsw dyld a2s` on addr - slide),
  `runmigrator BUNDLE` (DataMigration plugin by hand, wrapper entitlements), `expt` (extension point catalog),
  `lsreg` (LSApplicationWorkspace; its calls return nothing so far).
- **Remote UI** (kernel 0101): `tools/flame-cmd.py tap X Y` / `swipe X1 Y1 X2 Y2 MS` (panel pixels) inject touches
  through SweetHID. Unlock: `swipe 540 2270 540 1100 300`.

### Internet over the USB link (2026-10-21, 08:00) - Safari browses
- **Mac:** `tools/mac-share.sh` (root): IP forwarding + one pf NAT rule in anchor com.apple/sweet-share,
  192.168.7.0/24 out the default interface. Not macOS Internet Sharing (it would re-address the link). From
  Claude: `osascript -e 'do shell script "<abs path>/tools/mac-share.sh" with administrator privileges'`
  (the `!` prefix cannot prompt for sudo). Rerun after a Mac reboot/network change; `... off` undoes it.
- **Phone:** `netsvc` (phone/tools, SCDynamicStore write entitlement; SC functions declared by hand, the iOS SDK
  marks them unavailable) publishes ue0 as a service: IPMonitor makes it primary (router 192.168.7.1), DNS
  1.1.1.1/8.8.8.8. **Rerun `netsvc` after every boot** (nothing persists it yet; next: a launchd job).
- `nettest [HOST] [URL]`: DNS, TCP, HTTPS via NSURLSession (HTTP 200 from apple.com's success page).
- `ping 192.168.7.2` from the terminal fails: Local Network privacy, not the link (forwarded traffic is fine).
- **Safari pages stay blank** (loads fine: URL bar, HTTP 200). Found/fixed on the way: kernel **0104** (planar
  IOSurfaces report summed bytes-per-row/element; CA logged "unexpected bytes-per-row" for 'b3a8'), and WebKit's
  GPU process (DOM rendering needs Metal) is off: `prefs -g|com.apple.mobilesafari WebKit2GPUProcessForDOMRendering
  bool NO` (phone/tools/prefs = `defaults` for iOS). Still blank with WebContent drawing 'w30r' surfaces itself
  (SpringBoard's w30r surfaces display fine, so format/IPC are OK): likely accelerated CG drawing needs Metal.
  Untested: `...AcceleratedDrawingEnabled bool NO` (set under WebKit*/WebKit2*/com.apple.Safari.ContentPage
  GroupIdentifier.WebKit2* keys, global + mobilesafari) - the relaunch tap missed Safari. Decision: go to Metal.

### Black wallpaper fixed (2026-10-21, 07:50): app extensions now register (kernel 0103 + ADT MACs)
Wallpaper and widgets (Batteries widget on the home screen) work. The chain, each found with the new tools:
- PosterBoard asserted `!_stateLock_switcherConfiguration` (-[PBFPosterExtensionDataStore
  _stateLock_setupSwitcherConfigurationForFirstTime], source line 3431): its first extension update came minutes
  late, after a client had already built the switcher configuration through the lazy getter.
- It came late/empty because LaunchServices had 189 apps but 1 extension record. Two causes:
  1. **installd took 5 s per bundle**: MobileGestalt's WifiAddress (old-style UniqueDeviceID) reads
     `IODeviceTree:/chosen` `mac-address-wifi0`, else waits 5 s for an IONetworkController (sdio/wlan/
     marconi-wifi). Fix: tools/mkadt.py adds mac-address-wifi0/bluetooth0/ethernet0 (locally administered
     02:50:49:58:45:4c/4d/4e; wifi = the one wland gives the WLAN firmware). Rebuild work/boot/flame-hi.adt.
  2. **Every .appex was rejected** by MobileInstallation inside lsd ("Extension cache does not have an entry
     for <point>"): ExtensionFoundation reads the signed extension point cache only after asking AMFI
     (IOService "AppleMobileFileIntegrity", selector 6, cdhash) whether it is in the trust cache. No AMFI in
     our kernel -> no cache in sandboxed lsd. Fix: **kernel 0103** `iokit/Kernel/arm/SweetAMFI.cpp`, a stand-in
     service answering "trusted" (other selectors logged + unsupported; boot-arg sweetamfi=0 disables).
- After both: `lsreg -r` (once; the database persists) -> 515 extension records, all 10 poster providers.
- New tools (phone/tools, /var/local/bin): **`oslog [-t SECS] [-l] [PROC...]`** streams the unified log
  (there is no `log` on iOS; private data is on via /Library/Preferences/Logging/com.apple.system.logging.plist),
  `exq` (EXQ_POINTS=1: extension records per point; EXQ_LIST=<point>), `lsreg -p APPEX` (plugin registration
  status code). `tools/flame-fresh-run.sh NAME [SECS]` (was flame-wifi-run.sh) boots kernel-0103.
- Screenshots with GPU present need `tools/flame-shot --stride 4352 OUT.png`.

### WiFi, part 1: the modem runs the WLAN firmware (2026-10-21, 05:30) - `wland up` on the phone
`phone/wifi` (build.sh -> /var/local/bin/wland, all from userland over /dev/pmem; kernel 0102 for PMEM_RPMH).
`wland up [SECS]` (0 = daemon) from a fresh boot: SMP2P item, modem boot, GLINK, QRTR, rmtfs, pd-mapper, TFTP.
Result: the modem boots, stays up (no err-fatal), loads wlanmdsp.mbn and publishes **WLFW (QMI 0x45)**.
- Files on the phone: /var/local/fw/modem = radio modem.mdt/bNN + modemr.jsn + from vendor.img (factory image,
  `debugfs -R "dump /firmware/X"`, work/vendor/fw): wlanmdsp.mbn, modemuw.jsn, bdwlan-flame.bin.
  /var/local/fw/rmtfs = backup/{modemst1,modemst2,fsg,fsc}.img (IMEI etc.: phone only, never a repo).
- Layers (Linux file each one follows): smem.c (smem.c), SMP2P in wland.c (smp2p.c), boot_modem (q6v5_pas.c),
  glink.c + link.c (qcom_glink_smem.c/native.c), QRTR in link.c (af_qrtr.c + ns), qmi.c (qmi_encdec wire format),
  rmtfs.c (andersson/rmtfs + downstream msm_sharedmem: 2 MB, hyp_assign HLOS+MSS), pdmapper.c (andersson/pd-mapper,
  table from the .jsn files), tftp.c (andersson/tqftpserv).
- What each fix taught (all were needed):
  1. Our SMP2P out item (428) must exist **before** the modem boots, else it never posts "ready". SMP2P in =
     435; the modem's entry is "slave-kernel": bit 0 err-fatal, 1 ready, 2 handover. Clear it before a restart
     (wland does), or the old "ready" fools the wait.
  2. SSR ack: mirror the modem's RESTART_DONE (in flags bit 0) into our RESTART_ACK (bit 1) after every restart.
  3. Without **rmtfs** the modem err-fatals ~40 s after boot (no SFR string in SMEM 421 - none at all).
  4. QRTR control packets are always 20 bytes; RESUME_TX = {cmd, client.node, client.port} - a wrong layout
     stalls every modem client after ~8 packets (QRTR flow control).
  5. GLINK intent ids are per channel: only keep IPCRTR's. Big packets need one-shot intents (~30 ms per
     RX_INTENT_REQ round trip), so TFTP blocks are capped at 1024 (fit the reusable 1152-byte intents).
- Restart the modem: `wland shutdown; wland up`. Logs: `wland up 60 > /var/local/wland.log`; WLAND_VERBOSE=2
  traces every intent/TX/RX_DONE.
- **WLFW works (06:55): FW READY, wlan config + mission mode accepted** (wlfw.c, ath10k qmi.c order). Chip
  0x30224, fw WLAN.HL.3.2.0.c9-00554. What it took:
  6. WLAN power before the modem boots (`wland wlan-power`): RPMh VRM ldoa1 752 mV, ldoa7 1800, ldoc2 1304,
     ldoc11 3104 (HPM, enable), rfclka2 on, and **the QDSS clock: QMP "{class: clock, res: qdss, val: 1}"**
     (mainline sm8150 wifi clocks). Without QDSS the firmware takes an exception when the cal report starts
     calibration: PFR "EX:wlan_process:0x1:WLAN RT:0x78:PC=0xb01976e8" (pd-mapper prints PD failure reasons).
  7. qmi_buf was 4 KB: the 6 KB BDF segments silently lost their data TLV (error 48). Now 8 KB, overflow loud.
  8. MSA (0x8bc00000, 1 MB) stays assigned across modem restarts: never take it back while WLAN runs (the SMC
     never returns). Same for rmtfs: one 2 MB buffer per phone boot (/var/local/fw/rmtfs/.buffer), else after a
     few restarts hyp_assign hangs in TZ. Both hangs wedge a CPU: reboot the phone (tools/flame-fresh-run.sh).
  9. BDF = vendor /firmware/bdwlan-flame.bin in 6144-byte segments; MAC address (0x33) sent before the cal report
     (locally administered 02:50:49:58:45:4c).
- `tools/flame-fresh-run.sh NAME [SECS]`: fresh run (kernel-0102, -G), waits for SSH. Use 1800 s: each modem
  restart costs ~70 s (most of it the 3.7 MB wlanmdsp.mbn over TFTP).
- Next: copy engines at 0x18800000 + HTC + WMI scan (ath10k snoc.c/ce.c/htc.c/wmi-tlv.c), from userland,
  polled; CE rings in pm_alloc memory (WLAN DMA is apps SMMU stream 0x640: check whether it is bypassed).

### Ranked plan (2026-10-21, 01:00) - work top-down, update as items land
State: iOS 16.7.16 boots to the home screen on flame; touch, 8 cores, USB Ethernet + SSH + file server; GPU
up in the kernel (296 ms), every frame scaled to fit by the A640 (kernel 0096/0097, `flame-run.sh -G`);
Metal loads our SweetMetal.bundle; the kernel GPU user client works from userland (`mtlk`: submit 0.03 ms).
Missing: real time, WiFi/BT/cellular, audio, sensors, camera, most stock apps, GPU-rendered UI.

Ranked by leverage (value to the user and to later work / effort):
1. ~~**Panic backtraces in the RAM log**~~ DONE (tooling, small): the last 6 panics printed no backtrace, so each cost a
   guess. Every later driver benefits.
2. ~~**Real time from the PM8150 RTC**~~ DONE (small): the date is 1969; TLS (any internet over WiFi), logs, lock screen,
   certificate checks and many daemons need it. SPMI access already exists (sweet_spmi.c).
3. ~~**Install the 32 staged stock apps**~~ DONE (small-medium, very visible): /private/var/staged_system_apps has
   Calculator, Clock, Notes, Weather, Maps, Calendar, Mail, Music, Files, Health, ... never installed (no
   /var/containers/Bundle/Application): the first-boot data migration that installs them did not run.
4. **WiFi** (large; the user's top wish: scan nearby APs, then join): WCN3990-class WLAN in the SoC (Linux
   ath10k_snoc, "qcom,wcn3990-wifi"), firmware runs on the modem DSP: boot MSS (PAS/MBA, like the zap shader),
   QRTR/QMI over SMEM+GLINK to the WLFW service, then ath10k copy engines + WMI for scans. iOS side: wifid
   expects IO80211Family; first a scan tool, then a minimal IO80211 interface for Settings.
5. **Metal driver for Core Animation** (large, performance): queue/command buffer/encoders over the user
   client, AIR->NIR->ir3 for CA's shaders, render passes as A6xx streams.
6. Stock-app hardware, as apps need it: audio (WCD9340 via ADSP/SLIMbus), sensors (SLPI), camera, BT,
   battery/charging accuracy, brightness, vibration, GPS, cellular.
Tooling items folded in as needed: faster kernel iteration, panic symbolication (`ipsw` + kernel nm).

### Metal driver, stage 2 (2026-10-21, 08:30) - command queue works, next: library/pipelines
- SweetMetal.m: the logging forwarder is generic (`route_class(cls, "BaseClass", protocols)` + `ROUTED("Base")`
  per class: unimplemented base/protocol selectors log first use as `MTL[pid name]: Class -sel (base|MISSING)`).
  SweetMTLCommandQueue (_MTLCommandQueue; device/completionQueue are the driver's, the base lacks them) and
  SweetMTLCommandBuffer (_MTLCommandBuffer): submitCommandBuffers:count: -> didSchedule..., then the base's
  completeCommandBuffers:count: (buffers complete at once, nothing encoded yet).
- Loop without rebooting: `tools/flame-metal-swap.sh` (build, swap binary, restart backboardd); boot with
  `tools/flame-fresh-run.sh NAME 1800 sweetmtl=1` (third arg = extra boot-args). Log: logs/runs/NAME/live.txt.
- **Stage 3 (09:20): CA renders frames through SweetMetal** (nothing drawn into pixels yet: black screen with
  sweetmtl=1). Needed: device factories for textures (IOSurface-backed or CPU), buffers, depth-stencil/sampler
  states, pipeline states (record the descriptor), render/blit encoders (_MTLCommandEncoder), no pipeline
  archives (`newPipelineLibraryWithFilePath:` -> nil), `MTLFailureTypeSetErrorModeType(t, 5 nslog)` for t 0..4
  in the bundle's constructor (MPS "device is not supported" aborted SpringBoard), and two CA preferences
  (global, `prefs -g KEY bool NO`): **CAEnableDeepFramebuffer** (else the framebuffer is 'b3a8' = BGRA10_XR,
  "Unexpected framebuffer format b3a8", and CA renders nothing) and CAEnableFramebufferCompression/
  CAEnableIOSurfaceCompression (probably not needed). CA's probes then real passes: 1125x2436 BGRA8 iosurface
  targets, ~3-10 draws per pass.
- **Shader set for the home screen** (`pipeline N:` lines): fixed_vert_lph_spc + fixed_frag_lph_cph (blend on/
  off) = the compositor, luma_boost_lph, home_affordance_base_lph, attachment_clear_frag_lph, path_blit_vert,
  std_vert1, and the downsample/narrow_blur family. fixed_frag_lph_cph is an uber shader (104 KB AIR)
  specialised by function constants (fc_image_function0/1, fc_coord_function0/1, fc_filter0/1,
  fc_color_opcodes0/1, fc_color_inputs0/1, fc_plane_count, fc_texcoord_count, fc_blend_function,
  fc_destination_function, fc_image_count, fc_extended_range, ...). `tools/metallib.py LIB [NAME OUT.bc]` lists/
  extracts functions (research/gpu/QuartzCore.metallib), `/opt/homebrew/opt/llvm/bin/llvm-dis` reads them.
- Function constants of CA's fixed_* pipelines are **empty**: it uses the unspecialised uber shader, whose
  configuration comes per draw at runtime (setFragmentBytes/buffers). So: one compositor shader + a few small ones.
- Per-draw arguments (first 12 draws logged as `draw N:` + indented lines): every draw is an indexed quad
  (6 u16 indices 0,1,2,2,1,3); state persists between draws (Metal semantics). fixed_*: vertex buffers 1-3 and
  fragment buffers 1-2 are offsets into shared 256 KB MTLBuffers, fragment texture 3 (icons: 96x96 fmt 30;
  luma_boost: 128x1 R8 LUT); blend One / OneMinusSourceAlpha (premultiplied). The data holds float positions
  (512.0, 1152.0, ...), half colours (3c00 = 1.0) and **64-bit buffer gpuAddress values** (0x1_6fb08b10 = our
  current CPU pointer): CA's shaders follow pointers, so GPU VAs must equal what -gpuAddress returns.
- **Design (decided 2026-10-21, 10:30)** - SweetMetal draws through turnip-generated state:
  1. CA specialises with function constants (descriptor.constantValues); Metal's library cannot without a
     compiler backend, so SweetMetal's library wrapper returns the plain function with the values attached
     (associated object), for sync and async calls (else "Metal failed to specialize fragment function").
     Present as an Apple family (Metal's default answers): non-Apple families made CA ask for
     fixed_frag_lpf_cpf, absent on iOS. Consequence: fc_framebuffer_fetch=1 (blending in the shader).
  2. Host: `tools/airspec.py LIB FUNC OUT.ll name=value...` specialises + optimises (fixed_frag 13k lines ->
     ~40 instructions). AIR metadata (!air.vertex/!air.fragment) gives each argument's role and index.
  3. Host: translate the small IR to GLSL (todo: tools/air2glsl.py; generic CFG via a switch-in-loop),
     binding convention: set 0 binding N = vertex Metal buffer N (UBO), set 1 = fragment buffers, set 2 =
     textures, set 3 = samplers; Metal y-up clip space -> flip.
  4. VM: `work/mesa/draw/capture.sh VS FS OUT [blend]` = turnip (TU_DEBUG=sysmem, noop drm-shim) draws one
     quad with work/mesa/draw/vkdraw.c (vertex layout = CA's: binding 1 stride 48, loc0 float4 @0, loc1/2
     float2 @16/24, loc3 half4 @32); `tools/fd cffdump` decodes. First capture: work/mesa/draw/cap1 (fixed_vert/
     fixed_frag by hand: mvp * position, colour).
  5. What the capture shows: one CP_SET_DRAW_STATE with ~30 groups. Address-bearing: group 1 (SP_VS_BASE +
     CP_LOAD_STATE6 of the VS), 8 (SP_PS_BASE + FS), 9 (VFD_VERTEX_BUFFER[n].BASE/SIZE), 11 (SP_GFX_/
     HLSQ_BINDLESS_BASE[set] -> descriptor memory, 64-byte slots), 12 (CP_LOAD_STATE6 ST6_UBO SS6_BINDLESS:
     UBO base/size per stage). The rest is per pipeline, address-free -> lift verbatim. Before draws: preamble
     (CP_SET_AMBLE), sysmem pass setup (RB_MRT[0] format/pitch/base, scissors, window offsets, MSAA off,
     RB_RENDER_CNTL...), after: flushes.
  6. Phone (todo): MTLBuffers/IOSurface textures in GPU memory (user client sel 1/3; -gpuAddress = GPU VA, so
     CA's embedded pointers work), command stream per command buffer: preamble + per pass setup + per draw
     CP_SET_DRAW_STATE (static template groups + generated 9/11/12, shader code copied to a GPU buffer) +
     CP_DRAW_INDX_OFFSET, submit at commit (sel 4), complete. Templates per pipeline from the host.
- Older note on the shader path choice: (a) generic AIR (LLVM IR) -> SPIR-V
  (rename air.* intrinsics, llvm-spirv) -> Mesa spirv_to_nir -> ir3, or (b) hand-written GLSL for the few
  specialisations CA uses, compiled by Mesa in the VM. Then textures/buffers in GPU memory (user client),
  render encoder -> A6xx draw streams (from turnip captures), present.
- Earlier, where CA stopped (backboardd crash-looped with sweetmtl=1): newDepthStencilStateWithDescriptor: (nil),
  newLibraryWithURL:error: (QuartzCore.metallib via _MTLDevice), newPipelineLibraryWithFilePath:error:,
  `-targetDeviceInfo` -> crash (Apple's compiler path). Next: our own SweetMTLLibrary/Function (_MTLLibrary/
  _MTLFunction: keep the AIR bitcode per function), depth-stencil/sampler state objects, render pipeline state
  (AIR -> NIR -> ir3 via Mesa), then textures from IOSurfaces, buffers, render encoder -> A6xx draw streams.

### Metal driver: first contact (2026-10-21, 00:15)
- **GPU present works** (kernel **0096** = 0095 + SweetAccelerator; built with `-mcpu=cortex-a55+crypto`, see
  below): bring-up 296 ms at boot, every frame is the whole iOS frame scaled to 1053x2280 (status bar and home
  indicator visible), ~3.0 ms per present (surface cache clean + blit at 257 MHz). Run: `flame-run.sh NAME -k
  work/boot/kernel-0096 -G`. Touch follows the fit (not yet tried by the user).
- **How Metal finds a driver** (Metal.framework, disassembled with `ipsw dyld disass`): `MTLRegisterDevices` iterates
  `IOServiceMatching("IOAcceleratorES")`; `getMetalPluginClassForService` loads
  `/System/Library/Extensions/<MetalPluginName>.bundle`, takes class `MetalPluginClassName` (or the principal class),
  requires a subclass of `_MTLDevice`; `[[cls alloc] initWithAcceleratorPort:service]`; `MTLAddDevice` asserts
  conformsToProtocol MTLDevice **and MTLDeviceSPI**, then initLimits / initFeatureQueries.
- **Kernel:** `iokit/Kernel/arm/SweetAccelerator.cpp` (class IOAcceleratorES + SweetAccelerator, IOResources
  personality in KernelConfigTables.cpp), published only with boot-arg **`sweetmtl=1`**.
- **Userland:** `phone/metal/SweetMetal.m` -> `/System/Library/Extensions/SweetMetal.bundle`
  (`tools/flame-metal-install.sh`; root-fs binaries are platform binaries, so backboardd loads it). Stage 1 is a
  logging device: all 585 selectors of _MTLDevice + MTLDevice + MTLDeviceSPI go through forwardInvocation:, first use
  logged to /dev/console as `MTL[pid name]: -selector (_MTLDevice | MISSING)`.
- **Result (run mtl3):** backboardd -> CAMetalContextCreate -> MTLCreateSystemDefaultDevice -> our device; device
  registration passes (featureProfile and maxBufferLength missing, return 0); then `-newCommandQueue` ->
  `_MTLDevice newCommandQueueWithDescriptor:` crashes (NULL): a driver must supply its own _MTLCommandQueue subclass
  (base: initWithDevice:/initWithDevice:maxCommandBufferCount:, commandBufferWithDescriptor:, submitCommandBuffers:count:,
  completeCommandBuffers:count:). backboardd crash-loops with sweetmtl=1: don't use it for normal boots.
- **Shaders:** QuartzCore.metallib's 141 functions are LLVM bitcode modules (wrapper magic 0x0b17c0de) that Homebrew
  `llvm-dis` reads as plain LLVM IR (triple air64-apple-ios16.7.0), e.g. std_vert0_lpf = mat4 * position. So the
  translation path is AIR (LLVM IR) -> NIR -> ir3 (Mesa, in the VM), for the functions/constants CA requests.
- Faster loop than a reboot: replace the bundle (put to a temp name, `mv` over ssh) and `killall backboardd`.
- **Plan:** (1) kernel user client for SweetAccelerator: buffers (IOBufferMemoryDescriptor mapped to the GPU and the
  task), IOSurface import, submit + fence; (2) queue/command buffer classes; (3) textures/buffers; (4) AIR->NIR->ir3
  for the requested functions; (5) render encoder -> A6xx draw streams (from turnip captures of equivalent draws).

### GPU in the kernel: frames scaled to fit by the A640 (2026-10-20, 23:30, in progress)
- **Shared core** `bsd/dev/sweet_a6xx.c/.h` (kernel patch 0095): bring-up, SMMU banks (TCR2 SEP fix, 39-bit VA),
  ring/IB submission, 4K page mapping, and `a6_blit_ib()` = turnip's 2D blit rebuilt from the capture (A640 init
  registers + RB/GRAS/TPL1 A2D registers + CP_BLIT SCALE). The userland tool builds the same file
  (`phone/gpu/build.sh` compiles it from the xnu tree); `gpu test blit` checks it against a bilinear reference
  (worst 1.0), `gpu test fb` puts the fitted test frame on the panel (photo `work/gpu/cam-fb.jpg`). The blit takes
  ~5.9 ms on the GPU at 257 MHz (`GPU_TIME=1`).
- **Kernel service** `bsd/dev/sweet_gpu.c` (boot-arg `sweetgpu=1`; `flame-run.sh -G` adds it and the firmware):
  firmware in the ADT (`mkpayload.py --gpufw research/gpu/fw` -> /chosen `gpufw,<file>`), bring-up in a thread
  (**296 ms**, works), per-surface GPU mappings (4 slots at 4 GB + n*16 MB, pages from getPhysicalSegment after
  prepare()), cache clean of the surface, blit into the panel memory.
- **SweetFramebuffer** `presentGPU()`: one full-frame BGRA layer -> GPU blit fitted (1053x2280 at x 13); first time
  switches the panel to stride 4352 (`sweet_gpu_panel_stride`: VIG0/VIG1 YSTRIDE0 + CTL0 flush, live under
  autorefresh) and clears it. Falls back to the CPU copy (reasons logged once: "GPU present not used: ...").
  Touch follows the fit (`sweet_display_fit_x/w`, SweetHID.cpp).
- **Found:** the kernel's `-march=armv8.2-a+crypto` let clang emit **EOR3 (SHA3)**, undefined on A76/A55: panic
  "Undefined kernel instruction ... instr=ce1044c6" in a6_blit_ib (gpu9). makedefs/MakeInc.def now says
  `+crypto+nosha3+nosm4`; full rebuild (old objects in build/xnu.obj/DEVELOPMENT_ARM64_SWEET.old-0095b). Check new
  kernels with `xcrun llvm-objdump -d K | grep -E "\s(eor3|bcax|rax1|xar)"`.
- iOS surfaces must be prepare()d (wired) before getPhysicalSegment (gpu8: no GPU frames, silent fallback).

### GPU: Mesa command streams replay on the A640 (2026-10-20, 21:25)
- **Capture:** turnip (Mesa Vulkan) + the noop drm-shim in the Lima VM (`work/mesa/build-vk.sh` -> `~/mesa-vk-install`;
  local patch `work/mesa/drm-shim-a640.patch` adds the A640 to the shim). `limactl shell --workdir $PWD iosroot sh
  work/mesa/capture.sh PROG.c OUTDIR` runs a Vulkan program with `FD_GPU_ID=640 FD_RD_DUMP=enable,full`; the .rd it
  writes is **gzip-compressed** despite the name (gunzip before use). Example: `work/mesa/vkblit.c` = scaled,
  linear-filtered blit 1125x2436 -> 1053x2280 at x 13 in 1080x2280 (iOS frame -> panel, aspect kept),
  capture `work/gpu/cap-blit/blit.rd` (6 buffers from iova 0x100000000, one 184-dword stream with CP_SET_DRAW_STATE,
  CP_SET_AMBLE, CP_BLIT).
- **`gpu replay FILE.rd`**: each buffer gets its own /dev/pmem allocation mapped at its iova (4K pages, tables from an
  arena in the pool), then the streams are submitted. **Result: the blit ran on the phone; 60000 sampled channel
  values within 0.9 of a bilinear reference, side bars untouched** (`work/gpu/blit-result.png`).
- **SMMU fix:** TCR2.SEP must be 7 (upstream); at 0 VA bit 31 selects TTBR1 (off), so every GPU VA >= 2 GB faulted
  at level 0 (found with `gpu vatest [VA...]`). The GPU bank now uses T0SZ 25 (39-bit VA) too.
- Init now also has the rest of a6xx hw_init for the A640 (PC_DBG_ECO_CNTL 0x180000, UBWC/NC mode regs for highest
  bank bit 15, CP_PERFCTR_CP_SEL0). An IB *fetch* fault wedges the CP (needs a reboot; no GPU reset path yet); a
  faulting data write does not.
- **Next:** turn the captured blit into a parameterized stream (`tools/fd rddecompiler`, or read the register
  writes out of cffdump), then the kernel service: bring-up in XNU + per-frame GPU blit of the swapped IOSurface
  into the panel framebuffer (replacing SweetFramebuffer's CPU copy and its crop).

### GPU debug tools (2026-10-20, 21:00) - the GPU now runs from an SSH shell
Kernel **0094** (`xnu/patches/0094-flame-pmem-files-read.patch`, `work/boot/kernel-0094d`) + `phone/gpu/`:
- **`/dev/pmem`** (`bsd/dev/sweet_pmem.c`, root 0600, boot-arg `sweetpmem=0` removes it): pread/pwrite at
  offset = PA (managed DRAM via the physmap with cache clean+invalidate around each access; MMIO and
  carve-outs through 2 MB device windows, 32-bit accesses only), ioctls ALLOC (contiguous, zeroed, stays
  until FREE), FREE, LIST, SMC (resumes QCOM_SCM_INTERRUPTED like Linux; without that the zap shader's
  INIT_IMAGE returned rc 1). Two traps found: ml_io_unmap of an ml_io_map mapping panics (permanent entry),
  and a 2 MB ml_io_map panics (io_submap too small); windows use ml_io_map_unmappable.
- **File server `READ path off len`** (any file or device): `tools/flame-files.py read /dev/pmem PA LEN OUT`.
- **`pmem`** on the phone: `r/w/m/dump/load/alloc/free/list/smc` (`phone/gpu/pmem_tool.c`).
- **`gpu`** on the phone (`phone/gpu/gpu.c`, the shim's gpuprobe.c on a 16 MB pool from /dev/pmem):
  `gpu up [-p]` = power, GMU boot, HFI, GX, SMMU banks, SQE, ME_INIT, zap, CP_MEM_WRITE test (once per boot,
  ~2 s); `gpu test paint` (GPU draws the band into a buffer: every pixel checked, "0 wrong pixels (exact)";
  then on the panel); `gpu status`; `gpu dump [FILE]`; `gpu alloc NAME SIZE` (prints the GPU VA);
  `gpu read/write NAME`; `gpu submit FILE.bin` (IB from raw dwords; writes `/var/local/gpu/last.rd`; on a
  hang/fault writes `hang-N.txt` and clears the SMMU fault); `gpu clear`. Every log line also goes to
  /dev/console ("GPU: ..."), so the RAM rescue keeps it if the SoC hangs. State: /var/local/gpu/state.
- Mac side: `tools/flame-gpu-install.sh` (builds, installs pmem/gpu + firmware, syncs);
  `tools/pm4.py IN.txt OUT.bin` (PM4 assembler, opcode/register names from Mesa's XML: `pkt7 CP_MEM_WRITE
  lo(@out) hi(@out) 1`, `pkt4 CP_SCRATCH[2] 5`); `tools/fd cffdump X.rd` and `tools/fd crashdec -f
  hang-N.txt` (Mesa freedreno tools built in the Lima VM, `work/mesa/build.sh`, source
  `reference-repos/mesa`; crashdec names the fault address, finds the ring/IB and walks to the packet);
  `tools/flame-shot OUT.png` (framebuffer as an exact PNG, 1.2 MB/s, 8 s per shot; `--pa/--w/--h/--stride`
  for any surface).
- Loop: `tools/flame-run.sh NAME -k work/boot/kernel-0094d -s 900 -a "sweetusb=2 sweetusbframes=0"`, wait
  for `tools/flame-ssh`, `tools/flame-gpu-install.sh`, `tools/flame-ssh 'gpu up'`. Files written through the
  file server are lost on a panic unless synced (the install script syncs).
- Findings: a translation fault makes the GPU drop later writes (fence never lands) until the context
  bank's FSR is cleared; FSR 0x400 alone is just the format field, not a fault. The panel framebuffer is
  redrawn by the kernel's 60 Hz copy of the iOS frame (early in boot it holds the console text; once
  SpringBoard runs, `tools/flame-shot` shows the home screen: `work/gpu/shot-now.png`), so GPU output must
  be checked in its own buffers.
- Example: `work/gpu/t1.txt` (a write to unmapped VA 0x30000000), `work/gpu/hang-6.txt` and its decode.
- Next: port the bring-up into a kernel service (IOAccelerator-shaped), first real draw (2D blit engine or
  an ir3 shader), and the logging AGX stub to learn backboardd's user-kernel interface.

### Tools (2026-10-20 evening) - use these
- **`tools/flame-run.sh NAME [-k KERNEL] [-s SECS] [-a "ARGS"] [-p POKE] [-g REGEX] [-f FILM]`**: one experiment
  end to end (payload + image, fastboot boot, wait, rescue boot, TWRP, pull + decode, summary). Output in
  `logs/runs/NAME/` (xnu.txt, shim.txt, rescue.txt, cmdline, kernel). Leaves the phone in TWRP.
- **Timestamps**: every kernel log line in the ring starts `[sss.mmm]` (19.2 MHz timer).
- **RAM log rescue**: XNU mirrors its console ring uncached at PA 0x1fe000000 (from SweetPlatformExpert start);
  the next shim boot copies the tail to UFS (`tools/flame-log-pull.sh rescue OUT`). flame-run.sh does a rescue
  boot (`work/boot/shim-probe.img`) automatically. Recovers the lines a crash never flushed.
- **Poke scripts** (`bsd/dev/sweet_poke.c`, `mkpayload.py --poke FILE` -> ADT /chosen/sweet-poke): `@SECS`,
  `r ADDR [N]`, `w ADDR VAL`, `m ADDR MASK VAL`, `p ADDR MASK VAL MS`, `s MS`; output lines `POKE:`. Example:
  `tools/poke/apps-smmu-dump.txt`.
- **USB Ethernet + live log** (boot-arg `sweetusb=2`, kernel 0089+): Mac interface "Sweet iOS (USB Ethernet)"
  (en17), configure once: `networksetup -setmanual "Sweet iOS (USB Ethernet)" 192.168.7.1 255.255.255.0`
  (no sudo needed). `tools/flame-live.py [OUT] [--grep RE]` prints the kernel console live (UDP 5140).
- **Command server** (kernel 0090): `tools/flame-cmd.py 'r 0x... N'`, `-f script`, `reboot` (UDP 5141 on
  192.168.7.2; results come back in the live log). The Mac never gets an ARP reply from the phone (open
  issue), so flame-cmd falls back to the subnet broadcast 192.168.7.255, which works. `reboot` = PSCI reset ->
  fastboot in ~2 s; flame-run.sh then rescues and pulls as usual. With `-a sweetusb=2` flame-run.sh also
  records the live log to `logs/runs/NAME/live.txt` (stop it before using flame-cmd: one UDP listener).
- The shim's start.S uses adrp+add now (the image passed 1 MB with the GPU firmware + rescue buffer).

### SSH into iOS works (2026-10-20, 20:00)
- **`tools/flame-ssh ['cmd']`**: root shell in iOS (dropbear 22 via FlameBridge 127.0.0.1:2222, key
  `work/userland/ssh/id_ed25519`). `uname -a` = `Darwin iPhone 22.6.0 ... iPhone10,3`.
- **FlameBridge** (`tools/flamebridge/FlameBridge.app`, start with `open`): macOS Local Network privacy blocks
  unicast to 192.168.7.2 from this terminal (Ghostty has no NSLocalNetworkUsageDescription, so macOS never
  asks; "No route to host" even for the home router). FlameBridge has the key and the user allowed it; it
  relays TCP 127.0.0.1:L -> 192.168.7.2:R from `tools/flamebridge/ports.txt` (2222->22, 2223->2223) and UDP
  127.0.0.1:5142 -> :5141 (commands). Rebuilding the app may reset the permission: edit ports.txt instead.
- **File server** (kernel 0092+, `bsd/dev/sweet_files.c`, TCP 2223): `tools/flame-files.py put|get|mkdir|link|rm|chmod`.
  vfs wrappers `sweet_vfs_mkdir/symlink/unlink` at the end of bsd/vfs/vfs_syscalls.c.
- **Kernel 0093**: command server `sync`, and `reboot` syncs first.
- Userland (`work/userland`, scripts `tools/flame-install-ssh.sh`): dropbear (single-user build, root only,
  pubkey only; IOSONPIXEL_SINGLE_ROOT skips its "non-multiuser kernel" check), dash (configure with
  `ac_cv_func_strchrnul=no`: the iOS 27 SDK has strchrnul, iOS 16 does not -> weak import -> PC 0 crash;
  check new binaries with `nm -m X | grep "weak external"`), toybox (macos_defconfig, GNU sed, iOS SDK lacks
  sys/disk.h: patched). Built with `work/userland/bin/ios-cc`, signed `codesign -s -`. Installed in
  /var/local/bin, /bin/sh -> dash, /var/root/.ssh/authorized_keys, /var/root/.profile.
- launchd: new jobs must be keyed under **/System/Library/LaunchDaemons/** in
  /System/Library/xpc/launchd.plist (a /Library/LaunchDaemons key was ignored). Original saved as
  launchd.plist.orig on the phone and work/userland/launchd.plist.orig.
- ARP: the phone pads frames to 60 bytes and broadcasts ARP replies (en17's lladdr 02:00:00:00:00:00 differs
  from the 02:00:5e:77:ee:01 it uses on the wire). `arp -an` shows nothing without root on this macOS.
- Known: toybox `ps` needs /proc (no output); next: Darwin ps/top, a sync before reboots in scripts.

### USB result (18:10)
The apps SMMU blocks USB DMA after ABL unless stream 0x140 is matched: S2CR bypass is refused by the
hypervisor (reads back as fault), banks with page tables gave translation faults, sharing the UFS bank
stalled the thread. **What works is ABL's own way** (found with the poke dump): a free SMR -> our own bank
with CBAR 0x1f000, CBA2R 1, **SCTLR 0xe0 (translation off)**. Then the Mac enumerates "Sweet iOS (USB
Ethernet)", the CDC-ECM driver attaches and the live log streams (~3 KB/s, 148 KB in 45 s). The "stall"
seen first was the phone backing off while nothing listened on 5140. Open: no ARP reply reaches the Mac.

- **Newest (16:10):** the Adreno 640 runs our command streams from the shim (GMU boot, HFI, zap shader,
  SQE, ring) and has drawn pixels on the panel. See "Frame rate measured, GPU probe" below. iOS itself:
  8 cores at full clock, touch works, 8-25 fps in the UI (software rendering).
- **State (older):** iOS 16.7.16 userland boots on flame, with launchd, ~400 daemons, backboardd and SpringBoard building
  the lock screen, and no panic in 20 min. The screen shows garbage (a white band, dark blue, pink noise),
  and backboardd stops presenting after 21 frames. PosterBoard (the lock-screen wallpaper) traps and is
  restarted. Details are in the dated sections below.
- **Phone:** (morning note; now it ends each flame-run in TWRP) left in TWRP, plugged in. userdata holds root image v1, modified by the rw boots. If it needs
  a reflash, use the `adb exec-in dd` path, never `fastboot flash userdata`.
- **Next tasks:** GPU (see "GPU debug tools" above). Still open from earlier: the PosterBoard trap
  (`logs/20261020/ios3.txt`, CRASH lines), no ARP reply from the phone. (cpus=8, touch, USB: done.)
- **Loop:** `tools/flame-twrp.sh` (always), `tools/flame-log-pull.sh xnu|shim OUT`, film with
  `tools/watch-boot.sh DIR SECS IMG`. Boot image to reuse: `work/boot/shim-boot-0084h.img`.
- **Gotchas found today:** `fastboot boot` dies at the "unlocked" warning after ~10 s means the Power key
  reads as held (check `klog`). TWRP bounces back to fastboot means both slots are unbootable (the helper
  fixes this). If the Bash permission classifier errors out, ask the user to run the command with `!`.

## Where things stand

The approach is ios-sweet's: a bare-metal shim booted with `fastboot boot` acts as iBoot and starts a
patched XNU (xnu-8796, "SWEET" machine config, all drivers built into the kernel). That XNU runs the
unmodified iPhone X iOS 16.7.16 userland from an HFS+ image in `userdata`.

Done and checked on the phone:
- **Kernel build:** ios-sweet patches 0001-0081 plus flame 0082-0083 build **locally** with Xcode 27.1,
  a few minutes per rebuild (`tools/build-xnu.sh`). No GitHub CI needed.
- **Shim:** boots under Google's ABL. This needed three fixes the Redmi didn't (see NOTES.md, "Google ABL boot image rules").
- **Unattended probe loop:** the shim dumps hardware state to the end of `userdata`, sets the PMIC
  reboot reason, resets into fastboot by itself, and TWRP reads the dump back.
  It worked once end to end: `logs/shim-probe1.txt`.
- **Display:** ABL leaves the command-mode panel **auto-refreshing** from a framebuffer at
  `0x9c400000`, 1080x2280, stride 4320, a8r8g8b8. That's the Redmi's pixel format, so ios-sweet's
  display path should carry over.
- **Partition backups:** per-device partitions (persist, modemst1/2, fsg, fsc, ...) and LUNs sdb-sdf are in
  `backup/`, SHA-256 verified against the phone.
- **iOS 16.7.16 (20H392) IPSW parts:** in `downloads/ipsw/` (rootfs 087-86698-044.dmg with its `.mtree`,
  SystemOS 087-86593-047.dmg, AppOS 087-86715-047.dmg, DeviceTree, kernelcache). Extracted D22AP ADT in
  `research/ipsw-x/d22ap.adt`.

Not done yet (as of 2026-10-20): a correct picture on screen, touch, USB link, SMP. The XNU log and the iOS
root image are done (see below).

## 2026-10-20, 14:30: ALL 8 CORES AT FULL CLOCK (read first)

- `cpus=8` works on flame with no code change (ios-sweet 0065/0078 + mkadt's 8 CPU nodes): PSCI CPU_ON rc=0 and
  "came alive" for MPIDR 0x000-0x700, `SWEET CPU: 8/8 CPUs initialized` (`logs/20261020/s8.txt`).
- Kernel **0086** (`xnu/patches/0086-flame-cpufreq.patch`, also carries the 0085 s_ahb halt-check fix):
  `bsd/dev/sweet_cpufreq.c` writes each OSM cluster's DCVS_PERF_STATE_DESIRED (0x920) to its top all-core
  LUT entry. ABL had left the A76 clusters at index 0 (**710 / 825 MHz**); now A55 1785, A76 2419, prime
  2841 MHz (`logs/20261020/f1.txt`). Boot-arg `flamecpufreq=0` keeps ABL's setting, `=N` caps at N MHz.
  Voltage (CPR) and thermal (LMh) stay in hardware; heat under long runs not yet checked.
- Current image for the user: `work/boot/shim-boot-0086u.img` (cpus=8, 20 min).
- GPU: backboardd matches `AGXAccelerator` / `IOAcceleratorES` and finds none, so CA renders in software.
  See the GPU section below for the plan being scoped.

### Frame rate measured, GPU probe 1 (2026-10-20, 15:45)

- Kernel **0087** (`xnu/patches/0087-flame-vsync-fps.patch`): vsync ticks re-arm from fixed deadlines before
  the panel copy (they used to re-arm 16.7 ms after it, so "60 Hz" ran slower with jitter), and every 5 s
  `SWEET FB: 5s: N swaps (X fps), M panel copies, avg copy U us`. Image `work/boot/shim-boot-0087.img`.
- Result (`logs/20261020/fps1.txt`, user swiping home pages and scrolling Settings, cpus=8 full clocks):
  **8-25 fps while interacting, peaks 38**; the panel copy costs ~2.3 ms. So the limit is Core Animation's
  software renderer; no-copy scanout is not worth doing. The GPU is the fix.
- **GPU probe 1** (`work/boot/shim-gpuprobe.img`, `SHIM_CFLAGS=-DSHIM_GPUPROBE`, `shim/gpuprobe.c`,
  log `logs/20261020/gpuprobe1.txt`): GCC gpu clocks on, GPUCC CX GDSC on (`f8222000`), gmu_clk_src on XO,
  ahb/cxo/cx_gmu on; GMU registers read (fw_version reg `4ad21cd3` = stale, rpmh_state `f`); **ITCM and DTCM
  take writes**; **Adreno SMMU registers readable** (CR0 `230406`, IDR0 `4c017e06`, IDR1 `20000005`, IDR2
  `5555`): no hypervisor fault on reads. Next: SMMU context bank (or bypass) for the GMU stream, then GMU
  firmware boot + HFI.
- **GPU probe 2: GMU BOOTED** (`work/boot/shim-gpuprobe2.img`, logs `logs/20261020/gpuprobe2-4.txt`). The
  shim programs Adreno SMMU SMR0 = `84000005` (GMU stream 5, mask 0x400) -> CB0 (S1, AArch64 descriptors,
  T0SZ 32, tables at 0x1f0000000), maps icache 0x4000 / dcache 0x44000 (AP=00: privileged, executable;
  AP=01 there gave a permission fault on instruction fetch at 0x4dc0), dummy 0x60000000, log 0x60001000,
  HFI 0x60005000. RSCC is at **0x2c6a000 + 0x23000** for the A640 (0x2c50000 is A8xx only and hangs the
  bus). Then RSCC/PDC sequencer setup, 7 firmware blocks (fw 2.0.1.5), HFI table, CM3 reset release:
  **init result 0xBABEFACE, HFI_CTRL_STATUS 1**, no SMMU fault, GMU log filling. The RSC wake handshake
  (RSCC_CONTROL_REQ bit 1) does not ACK; ignored so far. Next: HFI messages (init, fw version, perf/bw tables,
  start), GX power-up by the GMU, then read GPU registers (RBBM), SQE + zap.
- **GPU probe 3: GX POWERED, GPU REGISTERS RESPOND** (`work/boot/shim-gpuprobe3.img`, log
  `logs/20261020/gpuprobe6.txt`). After the GMU boot the shim reads cmd-db aux levels (`gfx.lvl`: 0 16 48 64
  128 192 224 256 320 384 416; `cx.lvl`; `mx.lvl`), builds arc votes like a6xx_gmu_rpmh_arc_votes_init, and
  sends over HFI (all acked, error 0; acks are matched by the seqnum in ret_header, the reply id is the
  request id): PERF_TABLE (GPU 257/345/427/499.2/585 MHz, GMU 200 MHz), BW_TABLE (a640_build_bw_table),
  CORE_FW_START, START, GX_BW_PERF_VOTE (index 1 = 257 MHz), then OOB GPU_SET (H2G bit 30, ack bit 31):
  **GX GDSC `f8222800` (on), RBBM_STATUS 1, CP_SCRATCH_REG0 write/read OK**. The RSCC wake handshake still
  never ACKs and did not matter. Next: SQE microcode (a630_sqe.fw), zap shader via SCM PAS (id 13),
  GPU stream 0 in the SMMU, ringbuffer, CP_INIT, then a CP_MEM_WRITE to prove execution.
- **GPU probe 5: THE GPU EXECUTES COMMANDS** (`work/boot/shim-gpuprobe5.img`, log `logs/20261020/gpuprobe11.txt`).
  Order (as a6xx hw_init): GPU stream (SMR1 = `84010000`, id 0 mask 0x401) -> CB1 with its own tables (AP=01,
  unprivileged), hw_init register subset, SQE microcode (a630_sqe.fw minus its first dword) at GPU VA
  0x10000000 via **CP_SQE_INSTR_BASE = 0x830/0x831** (0x816 is the A8xx offset; the register database lists
  both, check `variants=`), ring at 0x10010000, CP_RB_CNTL 0x0800020c, CP_SQE_CNTL 1, CP_ME_INIT -> idle,
  **then** the zap shader via SCM PAS (init image / mem setup / auth all 0), CP_SET_SECURE_MODE 0,
  CP_MEM_WRITE -> **target memory = 0xcafef00d**, no SMMU fault.
- **GPU probe 6: GPU PIXELS ON THE PANEL** (`work/boot/shim-gpuprobe6.img`, log `logs/20261020/gpuprobe12.txt`,
  photo `logs/20261020/photos/first-gpu-pixels.jpg`): the framebuffer (0x9c400000) mapped into the GPU as
  2 MB blocks at GPU VA 0x20000000; one gradient row by CP_MEM_WRITE, 599 more by CP_MEMCPY (0x75). The CPU
  never touches the framebuffer. Next GPU steps: port this bring-up into XNU as a kernel service, then a
  first real draw (2D blit engine or a triangle with an ir3 shader from Mesa), then the Metal driver work.
- **USB networking (kernel 0088, `sweetusb=2`), 16:15-17:20, NOT working yet.** `bsd/dev/sweet_usb.c` now has
  flame GCC offsets (master 0xf010, cfg-noc-axi 0xf078, aggre 0xf07c, clkref 0x8c008), no QUSB2 reads, femto
  HS PHY dump at **0x088e2000** (primary port; 0x88e3000 is the 2nd controller), periodic DWC3 state dumps.
  Findings: ABL leaves DWC3 3.110a in device mode (SSP), PHY powered (utmi0 01, common0 88, ctrl2 06);
  with RUN_STOP the Mac sends SOFs (DSTS SOFFN counts, HS) **but DWC3 DMA fails** (GSTS bit 4 bus error,
  no events). Cause: the apps SMMU (qcom,qsmmu-v500 at 0x15000000, 94 SMRs, 56 CBs, sCR0 USFCFG=1) has no
  SMR for the USB stream 0x140 after ABL exits -> the first TRB fetch is an unmatched-stream global fault
  -> SoC reset. Tried: (1) S2CR bypass: reads back type 2 (fault), refused by the hypervisor; (2) own S1
  bank (cb55, then cb4), identity 2 MB block, tables read back right, NC walks: **translation fault at the
  first TRB** every time; (3) pointing 0x140 at the UFS bank (cb1): SoC reset. Next: read the downstream
  qsmmu-v500 driver (coral kernel drivers/iommu/arm-smmu.c: skip-init, 3-lvl tables, TBU at 0x15182000,
  ACTLR, possibly nested S2 by the hypervisor) or capture what Linux programs for cb/SMR of 0x140 from
  TWRP (read the SMMU registers under Linux while USB is up). DMA buffer option `sweetusbdma=PA`.
- User wants networking: plan is USB device-mode CDC-ECM (port ios-sweet 0072-0077 to flame's DWC3 + SNPS
  femto PHY), which also gives a live kernel log. Wi-Fi and USB-host Ethernet are much harder.

### GPU plan (scoping, 2026-10-20)

The Adreno 640 hardware is already documented by Mesa (freedreno/turnip, ir3 compiler) and Linux msm DRM; the
hard part is iOS's side. Plug-in point: an IOAccelerator-family kernel service (what backboardd matches) whose
properties name a userland Metal driver bundle, as AGXMetalA11.bundle does for the A11. Phases:
1. Kernel: Adreno bring-up in XNU (GPUCC/GMU power via RPMh, SQE/GMU firmware, zap shader via SCM, GPU SMMU,
   ringbuffer), proven first in the shim (read the GPU ID, run a trivial command stream).
2. Find exactly which Metal API surface and which shaders QuartzCore's Metal renderer (CA::OGL::MetalContext)
   uses; CA ships a fixed shader set, so those can be translated once (AIR -> NIR -> ir3) instead of a general compiler.
3. A minimal Metal driver bundle covering CA's subset: compositing on the GPU. Apps that use Metal directly come later.
Before that: no-copy scanout (display pipe reads iOS's IOSurfaces placed in the 36 MB splash carve-out). Lower
value than first thought: CA's software renderer reads back the target, and scanout memory must be uncached.

GPU phase 1 checklist (reference: mainline `reference-repos/linux-msm/drivers/gpu/drm/msm/adreno/a6xx_gmu.c`,
`a6xx_gpu.c`, DT `arch/arm64/boot/dts/qcom/sm8150.dtsi` gpu@2c00000 / gmu@2c6a000 / gpucc@2c90000 /
adreno_smmu@2ca0000; firmware from stock vendor.img in `research/gpu/fw/`: a640_gmu.bin, a630_sqe.fw,
a640_zap.mdt/.b0x; never commit these):
1. GPUCC: CX GDSC on; clocks gpu_cc_ahb, cx_gmu (200 MHz), cxo; GCC ddrss_gpu_axi, gpu_memnoc_gfx. Read a GMU
   register back (CX domain only).
2. Adreno SMMU (MMU-500, stream 0x400 GMU, 0x401 GPU): bypass or identity context bank. Unknown: whether
   Qualcomm's hypervisor traps these registers.
3. GMU: PDC sequencer + RSC setup (a6xx_gmu_rpmh_init), firmware blocks into ITCM/DTCM, HFI queues in memory,
   boot, HFI init/perf table (GX levels from cmd-db `gfx.lvl`), GMU powers GX.
4. Zap shader through SCM PAS (id 13), SQE microcode, CP init, ringbuffer; first proof: CP_MEM_WRITE of a
   magic value to memory.

## 2026-10-20, 14:00: TOUCH WORKS

- Kernel **0085** (`xnu/patches/0085-flame-fts-touch.patch`, `work/boot/kernel-0085`, boot image
  `work/boot/shim-boot-0085c.img`): new `bsd/dev/sweet_fts.c` drives the ST FTM5 over GENI SPI SE4. It votes RPMh
  L14A 1.8 V and L17A 2.856 V (`sweet_rpmh.c` now allows them), votes the wrap0 GCC clocks (s_ahb is HW-gated:
  its halt bit is skipped like Linux), muxes GPIO 51-54 to qup4, takes the bus from the SLPI (GPIO 126 low),
  pulses reset (GPIO 127), waits for the controller-ready event, reads chip id `36 39` fw 3.0, sends sense on
  (`A0 00 FF`), then polls the FIFO (`87`, one dummy byte) on IRQ GPIO 122 low. Coordinates are panel pixels
  (0..1079, 0..2279), passed straight to `sweet_hid_touch`. SweetHID now maps for the 2280-row panel.
- The user tested: tap, swipe-up unlock, home screen, apps open, corners: "working flawlessly".
- Boot-args: `flametouch=0` off, `flametouchdelay=N` (default 20 s), `flametouchlog=1` logs every event.
- One boot (0085a, first try) reset at ABL's "unlocked" warning before the shim ran (klog: PS_HOLD, not
  KPDPWR); a plain retry worked. Probably a button press during handling.
- Logs: decode the XNU ring with `tools/flame-log-decode.py RING.bin OUT.txt`.

## 2026-10-20, 13:20: iOS LOCK SCREEN ON THE PANEL

- **The "garbage" was the shim, not iOS.** `sde_autorefresh()` wrote CTL_START while ABL's INTF1 autorefresh
  was already on. That wedges the pipe: the panel keeps a half-drawn frame with pink noise on top and never
  updates again. Proven with `work/boot/shim-disptest.img` (`SHIM_CFLAGS=-DSHIM_DISPTEST`, `shim/disptest.c`):
  phases A (leave autorefresh alone), B (autorefresh off + manual kick per frame) and C (autorefresh back on, no
  kick) all update; D (autorefresh + kick) freezes. The fix is in `shim/sde.c`: never kick in autorefresh mode.
- **Boot 0084x** (`work/boot/shim-boot-0084x.img`, kernel 0084 unchanged, `sweetfb_noconsole=1 sweetfb_hz=30
  flamereboot=300`): XNU's console scrolls, then the **iOS 16 lock screen**: black wallpaper (PosterBoard
  crashes), red date "Wednesday, December 31" (no RTC: epoch) and a ticking clock, camera button. Photo:
  `logs/20261020/photos/first-lockscreen-0084x.jpg`. backboardd keeps presenting (the clock advances).
- Older notes below about pink noise and "21 swaps" are explained by this; ignore those leads.

## 2026-10-20, 10:20: iOS USERLAND RUNS ON FLAME

Boot **0084r** (`work/boot/shim-boot-0084r.img`) on root image v1 (`work/iosroot/iosroot-v1.img`, written from
TWRP with `adb exec-in dd` and checked with SHA-256 per GiB). Log: `logs/20261020/ios2.txt`.
- The sequence was hfs mounting iOSRoot (journal initialized), launchd (libignition level 1, the shared cache
  found), the boot tasks (mount, data-protection, FIPS POST all passed, keybagd with the system keybag
  loaded, usermanagerd), then ~300 daemons, backboardd (AppleCLCD, 1125x2436 IOSurfaces, `SWEET FB: swap
  2..7`) and SpringBoard. There was no panic. The run ended at `flamereboot=300`.
- The screen showed a uniform blue with a thin white strip at the earpiece end, most likely the first iOS
  frame(s). The sampler shows SpringBoard's main thread waiting. Only 7 swaps happened in 5 minutes on 1 CPU.
- Next: **0084h** (`work/boot/shim-boot-0084h.img`) adds ios-sweet's home-screen args `sweetfb_noconsole=1
  sweetfb_hz=30` and uses `flamereboot=1200`. Later: `cpus=8` (patch 0078; flame MPIDRs are in the ADT),
  touch (ST FTS over GENI SPI), USB.
- **0084h result (10:18 boot, filmed 11 min: scratchpad `ios3/boot.mp4`, not kept):** the screen shows a white
  band at the USB end, dark blue in the middle, and **changing pink noise over the top half**. Something keeps
  writing there, perhaps CPU-rendered IOSurfaces that are not initialized, or a wrong stride/offset in the
  `sweetfb_hz=30` swap path. The phone ends at `flamereboot=1200` (about 10:39) and resets into fastboot by
  itself. **First thing next session:** `tools/flame-twrp.sh`, `tools/flame-log-pull.sh xnu
  logs/20261020/ios3.bin`, decode it (python in the 0084a/ios2 notes), and look for `SWEET FB: swap` lines,
  SpringBoard crashes, and the SMP samples.
- **0084h log** (`logs/20261020/ios3.txt`, 246 KB, ran the full 20 min): no panic. Only 2 crashes:
  driverkitd (sig 9) and **PosterBoard** (sig 5 = trap, pc 0x1d35f8268, slide 0x11e80000, so unslid about
  0x1c1778268 in the shared cache; launchd restarts it). ios-sweet's notes never mention PosterBoard. It renders
  the iOS 16 lock-screen wallpaper, so it is a prime suspect for the garbage on screen. SpringBoard builds the
  lock screen (SBCoverSheetPrimarySlidingViewController, CATransformLayers, its own small IOSurfaces) and keeps
  running. backboardd made only **21 swaps**, all in the first minutes (display off/on once in between), then
  none. Next: symbolicate PosterBoard's backtrace against the 20H392 shared cache (blacktop `ipsw dyld
  a2s`), and dump the swapped surface (or the framebuffer) to UFS to see what iOS actually drew.
- The diag shim (`work/boot/shim-diag.img`) once showed pink noise instead of its text after a Linux boot.
  Same family of problem? Check what the display pipe really scans out (the probe shim logs it).
- Lima VM `iosroot` is stopped. Start it with `limactl start iosroot`.
- The Power key looked stuck from about 09:14 to 10:00 (see below). If boots die at the "unlocked" warning
  after about 10 s, check `klog` for `reboot_longkey`.

## 2026-10-20 update

**TWRP blocker solved, and XNU 0084 boots to "mount root" on flame** (`logs/20261020/xnu0084a.txt`). The sequence
was VM, IOKit, platform expert and WDT pets, AppleCLCD 1080x2280, CPU0, BSD, network, `SWEET UFS: "userdata" ...
read-write`, keys, `BSD root: ufsroot0`. Then `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : 0`
repeated, because userdata was empty. `flamereboot=90` then stopped the pets and the WDT reset the phone into
fastboot.

What the TWRP problem actually was:
1. **Slot exhaustion.** Every boot takes a retry from the active slot and nothing marks one successful. With
   both slots unbootable, ABL still accepts `fastboot boot`, but TWRP's first-stage init cannot mount its slot
   partitions and reboots into fastboot after about 14 s. Fix: `fastboot set_active a` before each TWRP boot.
   `tools/flame-twrp.sh` does this and waits for adb.
2. **Volatile state that survived warm resets.** After the slot was reset, TWRP hung on the Google splash
   until a long Power+VolDown press, which is a PMIC hard reset. ABL left PS_HOLD at WARM_RESET (`0x85a=1`).
   The shim now sets HARD_RESET (7) like Linux does (`set_hard_reset()`; log line `ps_hold reset: ctl 1 -> 7`).
   The bootloader reboot reason still survives.
- Partitions compared with `backup/SHA256SUMS`: only expected changes (backup GPT slot attributes, modemst EFS
  sync, klog/metadata/misc/persist).
- **0084** maps the early framebuffer block as Device-GRE, not Normal-NC, so the CPU never reads into pil_slpi
  speculatively. Patch `xnu/patches/0084-flame-early-fb-device-memory.patch`. Next boots also use `sweetusb=0`.

Tools:
- **Camera:** an iPhone films the Pixel. AVFoundation device "iPhone Xs Camera".
  `tools/camsnap/camsnap OUT.jpg "iphone xs" 1` takes one frame. `tools/watch-boot.sh DIR SECS [IMG]` films a
  boot. If frames come out black or blurred, the iPhone camera session paused (lock screen); ask the user.
- `work/boot/shim-showlog.img` (`SHIM_CFLAGS=-DSHIM_SHOWLOG`) pages the stored shim log and the XNU ring tail on
  screen, so logs can be read without Linux. It is built but not yet run on the phone.
- **iOS root build:** Lima VM `iosroot` (Ubuntu 26.04, vz, project mounted read-write at the same path), with
  apfs-fuse built from source (`-include cstdint`), hfsprogs and img2simg. OrbStack's kernel has no hfsplus, so
  it can't be used. Run `limactl shell iosroot bash -c '... tools/mkiosroot.sh ...'`.
  Output in `work/iosroot/` (log `mkiosroot-v1.log`).
- Root-boot image: `work/boot/shim-boot-0084r.img` (0084, cmdline with `rootrw=1 flamereboot=300
  ufslogsecs=2`). Next: tweaks (`sweet-root-tweaks.py` in the VM), `img2simg`, `fastboot -S 512M flash
  userdata X.simg`, then boot 0084r while filming.

### Writing userdata, and the 09:15-09:45 reset cycle (2026-10-20)

- `fastboot -S 512M` fails on flame ("No enough buffer"; max-download-size is 256 MiB). `fastboot -S 240M flash
  userdata iosroot-v1.simg` sent all 50 chunks, then **ABL hung writing chunk 50** for 25+ min. That chunk is
  long zero FILLs plus the HFS+ alternate header. Don't flash userdata through ABL. Write it from TWRP instead:
  `adb exec-in 'dd of=/dev/block/sda15 bs=4194304' < IMG`, then compare against
  `work/iosroot/iosroot-v1.gib.sha256` (SHA-256 per GiB).
- After that, the phone reset every ~10-15 s, even on battery and even in fastboot. **`klog`** (the
  by-name/klog partition, readable from TWRP with `strings`) holds XBL/ABL's own log with reset forensics:
  `reset message: reboot_longkey`, `gcc_reset_status=PMIC_RESIN:PMIC_ABNORMAL_RESIN`, PON history
  `WARM RST=0x80:KPDPWR_N`, then `abnormal: warm. Force reset...`. So the PMIC saw the **Power key as held**,
  and its long-press warm reset fired over and over. It stopped after the user clicked the buttons, and the
  user reached TWRP through the Recovery menu entry. Watch for a sticky Power key. Read `klog` first after any
  unexplained reset.
- `fastboot erase userdata` did not stop the cycle (the userdata contents were not the cause).
- TWRP has no `bootctl`, so keep using `set_active a` before TWRP boots (`tools/flame-twrp.sh`, which also
  waits for ABL to re-enumerate).

## (Solved 2026-10-20) The blocker: TWRP no longer boots

**Symptom:** `fastboot boot downloads/twrp/twrp-3.7.1_12-0-flame.img` gives "Booting OKAY". The phone leaves
fastboot and comes back to fastboot 14-15 s later, with no adb. This happened 4 times in a row and is
reproducible.

**When it started:** right after the first XNU boot (`work/boot/shim-boot-0083a.img`, 22:05:49). That boot
also came back to fastboot after about 14 s. Before it, TWRP booted fine every time, including right after
probe runs.

**What the timing means:** about 15 s is simply ABL's time to come back to fastboot after an early reset; a probe
run (shim work under 1 s) takes the same 15 s. So TWRP (and probably XNU) dies within seconds of
starting, not at 14 s. Don't read meaning into the 14.

**Ruled out:**
- The watchdog: a probe build that turns the watchdog *off* didn't help.
- The corrupted f2fs on `userdata`: I erased `userdata` (`fastboot erase userdata`, about 22:18) and it didn't help either.
  Android user data is gone. It had to go for the iOS root anyway, but I did it on a guess.

**Still works:** the probe shim runs and returns to fastboot. I just can't read its logs without TWRP.

**Hypotheses, in the order to test:**
1. **Leftover volatile state.** Since the XNU run, every reset has been a warm reset, so DRAM, IMEM
   cookies and PMIC registers survive. A crash, or an XPU fault from XNU, may have left a crash or
   download-mode cookie in IMEM (`dload_type@146bf01c`, `mem_dump_table@146bf010`, `rst_info@146bfd38`)
   that changes how the next Linux boot behaves.
   **Test: cold power cycle.** In the fastboot menu, press Volume down to "Power off" and press Power, wait
   10 s, then hold Power + Volume down. Then retry `fastboot boot` of TWRP.
2. **TWRP-specific.** Try `fastboot reboot`, which boots TWRP from `boot_a` by the normal path. Also try
   `fastboot boot stock/boot.img` (stock Android kernel; Android itself can't finish booting, but USB
   enumeration shows whether the kernel lives).
3. **Something written outside `userdata`.** Unlikely: every write path is bounds-checked to `userdata`, and the
   PMIC write is only `SOFT_RB_SPARE`. Once a shell works again, compare partitions and LUNs against
   `backup/SHA256SUMS`.

**Probable cause of the XNU reset (separate from the TWRP problem):** the early console maps the whole 32 MB block
`0x9c000000-0x9e000000` as Normal-NC (`start.s` and `sweet_early_map_final` in `arm_vm_init.c`).
On flame that block also covers the tail of `pil_slpi_region` (`0x9ab00000-0x9c300000`). Speculative
reads into an XPU-protected carve-out can reset a Qualcomm SoC. On the Redmi the framebuffer *was* the block
start. **Fix before the next XNU boot:** map only the framebuffer pages (an L3 table for that block, or
Device-nGnRE outside the FB range).

**Make this class of problem impossible:** give the shim its own USB device mode (ABL leaves the DWC3 at
`0xa600000` running; ios-sweet's `bsd/dev/sweet_usb.c`, patches 0072-0075, has the ep0 and bulk code), so
logs can be read with no Linux at all. Probably worth doing right after unblocking.

## Phone state at stop

- In **fastboot**, USB to the Mac, battery 4354 mV. Leaving it in fastboot overnight is fine.
  Powering it off from the fastboot menu instead *is* test 1 above.
- `boot_a` = TWRP 3.7.1_12-0 (flashed by me 2026-10-19). `boot_b` = stock. Active slot a. `userdata` erased.
- PMIC PON reason was set to "bootloader" by the last shim run, so the next reset lands in fastboot.
- Restore stock at any time: `images/flame-tp1a.221005.002.b2/flash-all.sh` (see RECOVERY.md).

## How to run things

```sh
# kernel (incremental; first-time full build was blacktop build.sh, see work/build-try1.log)
tools/build-xnu.sh work/build-NNNN.log   # -> work/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet
# new kernel patch: edit work/dxb/xnu (a git repo, HEAD = baseline + 0082 + 0083), then
#   (cd work/dxb/xnu && git diff > ../../../xnu/patches/NNNN-flame-*.patch && git commit -qam NNNN)

# ADT, payload, boot image (build.sh needs ABSOLUTE paths: it cd's into shim/)
python3 tools/mkadt.py work/boot/flame-hi.adt --highmem
python3 tools/mkpayload.py work/boot/kernel-0083 work/boot/flame-hi.adt work/boot/payload-X.bin --cmdline "..."
shim/build.sh $PWD/work/boot/payload-X.bin $PWD/work/boot/shim-boot-X.img
shim/build.sh                               # probe-only image: shim/out/shim-boot.img

# run + read back
fastboot boot work/boot/shim-boot-X.img     # waits for the reset into fastboot
fastboot boot downloads/twrp/twrp-3.7.1_12-0-flame.img
tools/flame-log-pull.sh shim logs/X.txt     # shim log (64 blocks before the XNU ring)
tools/flame-log-pull.sh xnu logs/X.bin      # XNU persistent console ring (last 1025 blocks)
```

The 0083a cmdline: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsrw=1
unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 flamereboot=90`

Shim behaviour (current `shim/shim.c`):
1. Logs CPU, PSCI, PMIC, UFS and SDE state.
2. Sets the PON reason to bootloader.
3. In probe mode, turns the watchdog off and resets with PSCI.
4. With a payload, arms the watchdog (30 s, tick rate assumed 32765 Hz, **unverified**) and jumps to XNU.

XNU patch 0083 pets the watchdog from `SweetPlatformExpert::start`: `flamewdt=0` turns it off,
`flamereboot=N` stops the pets after N s to bound a run.

## After unblocking: road to the home screen

1. Fix the framebuffer block mapping (above), then boot XNU again until the persistent log shows the boot reaching
   "waiting for root". The XNU ring in `userdata` is written once the UFS driver is up (`ufsrw=1`).
2. **iOS root image on macOS without sudo.** Ownership matters.
   - `hdiutil create` a case-sensitive journaled HFS+ image of about 16G, attach it (owners are ignored), and
     `ditto --nopreserveHFSCompression` from the attached APFS DMGs. XNU's HFS has no decmpfs
     compressors, so files must be stored uncompressed.
   - Add ios-sweet's extra links and files (`tools/mkiosroot.sh`).
   - Rewrite owner, mode and flags in the HFS+ catalog records from `Firmware/087-86698-044.dmg.mtree`
     (cryptexes: root:wheel), then check with `fsck_hfs`.
   - Fallback: a Linux VM (lima) running ios-sweet's scripts as root.
3. Root tweaks (`tools/sweet-root-tweaks.py`), then `fastboot flash userdata`,
   then boot with `rootrw=1`.
4. **Touch:** a new driver for ST FTS (ftm5) on GENI **SPI** SE4 `0x890000`. It needs:
   - RPMh votes for L14A (1.8 V) and L17A, and TLMM GPIO 122 (irq) and 127 (reset).
   - The Redmi Goodix driver is off by default (patch 0083); never enable it here: it would put 3.2 V on L11C.
5. The display is presented as 1125x2436 and the 2280-row panel crops 78 rows at the top and bottom (the status bar and
   home indicator are partly cut). Downscale later if it matters.
6. Then a USB live log (ios-sweet 0072-0077), `cpus=8` (flame: 4x A55 + 3x A76 + 1x A76 prime), battery.

## Gotchas

- macOS asks "Allow accessory to connect?" for every new USB identity (TWRP, any future shim USB
  device). Until someone clicks Allow, `ioreg -p IOUSB` shows the phone `!registered` and adb sees nothing.
- `grep` in this shell is ugrep and `\b` patterns misbehave; use `/usr/bin/grep`.
- TWRP debugfs SDE access: `echo "2000 1e0" > /sys/kernel/debug/dri/0/debug/sde_off`
  (hex, no `0x`), then `cat .../sde_reg`. Disable idle power collapse first (`encoder27/idle_power_collapse`).
- Nothing is in git for the project itself, and nothing has been pushed anywhere. Keep `backup/`, `dtb/`, `stock/`,
  `downloads/`, `work/` and `logs/` out of any future repo: they hold serials, IMEI data and Apple files.

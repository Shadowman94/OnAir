# Voice and audio codecs

> **DMR, P25, NXDN, dPMR and D-STAR voice is silent until you install mbelib.** The calls, IDs and data are shown without it. Its vocoders are patented, so OnAir does not include it: put `libmbe` / `mbelib` in the codecs folder (the **?** button next to the sound has an **Open codecs folder** button), then press **Look again**.

OnAir never ships a patented codec. You install the ones you are allowed to use yourself, and OnAir loads them. The one open codec, **codec2** (FreeDV and M17 speech), comes with OnAir: nothing to install. **Whether you may
install or use them is your own responsibility: follow the patent and radio-listening law where you live.**

Every mode that needs a codec has a small **?** button. It shows whether the library was found and where, the exact file names OnAir
looks for, the exact folders it searches, an **Open codecs folder** button (it makes the folder if it is missing) and **Look again**
(searches again, no restart).

## Which mode needs which codec

| Mode | Codec | Library you provide |
|---|---|---|
| DMR, P25 Phase 1, NXDN, dPMR | AMBE+2 / IMBE (patented) | **mbelib** (not included: you install it yourself; voice is silent until you do) |
| D-STAR | AMBE (patented) | **mbelib** |
| FreeDV 700D / 700E / 1600, M17 | codec2 (open, LGPL) | **codec2**: comes with OnAir (the library `libcodec2` next to the program) |
| FreeDV RADE | RADE neural modem (open, BSD) | **librade** (not codec2): optional; without it the RADE slot stays dark |
| TETRA | its own speech codec | nothing from you: it is not mbelib and not codec2 |
| DAB, DRM, TV, FM | MP2, AAC, H.264 ... | nothing (FFmpeg is inside OnAir) |
| ATSC 3.0 (US, South Korea) | Dolby AC-4 (patented) | optional **libavcodec + libavutil from an FFmpeg with an AC-4 decoder** (see below); without it the AC-4 sound is left out, picture and AAC / AC-3 sound still play |

**codec2 does not help DMR, P25, NXDN or dPMR.** Putting `codec2.dll` or `libcodec2.dll` in the codecs folder only matters for FreeDV
and M17. If a digital-voice mode says "mbelib not found", you need **mbelib**, a different library.

## File names and search order (the same on every mode)

OnAir tries these file names. Folders are searched in this order; inside each folder every name is tried before the next folder.

1. The `DECT2_MBELIB_PATH` / `DECT2_CODEC2_PATH` environment variable (a full path), if set.
2. The OnAir **codecs folders** (below).
3. The libraries that ship with OnAir: the program's own folder (Windows: next to `OnAir.exe`; Linux: next to `dect2`, and `../lib/onair` and `../lib` of it, which is where the `.deb` and the portable package put codec2; macOS: inside the app, `Contents/Frameworks` and `Contents/Resources`). A copy you put in a codecs folder (step 2) wins over the shipped one.
4. The system: Windows `PATH`; macOS `/opt/homebrew/lib`, `/usr/local/lib`, `/opt/local/lib`; Linux `/usr/local/lib`, `/usr/lib`, `/usr/lib64`, the multiarch folder.

| Library | Windows | macOS | Linux |
|---|---|---|---|
| mbelib | `mbelib.dll`, `libmbe.dll`, `libmbe-1.dll`, `mbe.dll` | `libmbe.1.3.dylib`, `libmbe.1.dylib`, `libmbe.dylib` | `libmbe.so.1.3`, `libmbe.so.1`, `libmbe.so` |
| codec2 | `codec2.dll`, `libcodec2.dll` | `libcodec2.1.2.dylib` ... `libcodec2.dylib` | `libcodec2.so.1.2` ... `libcodec2.so` (any `libcodec2.so.*`) |

### The OnAir codecs folders

| OS | Next to the program | In your user folder |
|---|---|---|
| Windows | `codecs\` next to `OnAir.exe` | `%LOCALAPPDATA%\OnAir\codecs\` |
| macOS | `codecs/` next to `OnAir.app` | `~/Library/Application Support/OnAir/codecs/` |
| Linux | `codecs/` next to the `dect2` program | `~/.local/share/onair/codecs/` (or `$XDG_DATA_HOME/onair/codecs/`) |

The **?** popup prints the full paths on your computer. A DLL must match OnAir: 64-bit with 64-bit. If it needs other DLLs
(for example `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`), put them in the same folder.

## mbelib (DMR, P25, NXDN, dPMR, D-STAR voice): build it yourself

mbelib decodes AMBE/IMBE, which are patented in some countries (the project's own notice says so). OnAir does not ship, download or
link to binaries of it. Upstream project: <https://github.com/szechyjs/mbelib> (a maintained fork: <https://github.com/lwvmobile/mbelib>). OnAir loads it at run time and contains none of its source. Check your local law first.

**Windows (MSYS2 / MinGW).** Install MSYS2 (msys2.org), open the "MSYS2 MINGW64" shell, paste:

```
pacman -S --needed git mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make
git clone https://github.com/lwvmobile/mbelib
cd mbelib && mkdir build && cd build
cmake .. -G "MSYS Makefiles" -DBUILD_SHARED_LIBS=ON && make
```

Copy the resulting `libmbe.dll` (look in the `build` folder) and, if OnAir says it does not load, `libgcc_s_seh-1.dll` and
`libwinpthread-1.dll` from `C:\msys64\mingw64\bin`, into `%LOCALAPPDATA%\OnAir\codecs\`. Press **Look again**.

**macOS.**

```
brew install cmake git
git clone https://github.com/lwvmobile/mbelib
cd mbelib && mkdir build && cd build
cmake .. && make
mkdir -p "$HOME/Library/Application Support/OnAir/codecs"
cp libmbe*.dylib "$HOME/Library/Application Support/OnAir/codecs/"
```

**Linux.**

```
sudo apt install git cmake build-essential     # or your distribution's equivalent
git clone https://github.com/lwvmobile/mbelib
cd mbelib && mkdir build && cd build
cmake .. && make && sudo make install && sudo ldconfig
```

(or copy `libmbe.so*` into `~/.local/share/onair/codecs/`). If the build gives a static library only, add `-DBUILD_SHARED_LIBS=ON`.

Then open the mode and look at its **?** button: it says `mbelib found: <path>`. Encrypted P25/NXDN/DMR calls are only reported as
encrypted; OnAir never decrypts. DMR voice is not decoded in this version of OnAir, even with mbelib installed.

## codec2 (FreeDV, M17)

codec2 is open software (LGPL-2.1) and **comes with OnAir**: FreeDV and M17 speech works after you download OnAir, with nothing to install.
It is only for FreeDV and M17.

- Where it is: Windows `libcodec2.dll` next to `OnAir.exe`; macOS `OnAir.app/Contents/Frameworks/libcodec2.1.2.dylib`; Linux the `.deb` puts it in
  `/usr/lib/onair/`, the portable package in `lib/` (next to `bin/`).
- It is a **separate shared library** that OnAir loads at run time, not part of the OnAir program, as the LGPL asks for. The source is in
  `third_party/codec2` (the unmodified 1.2.0 release, see `third_party/codec2/README-OnAir.md`), and every package carries the licence text
  (`licenses/codec2/COPYING`) and a notice.
- **Use your own build instead:** put your codec2 (`libcodec2.so.1.2`, `libcodec2.1.2.dylib`, `codec2.dll` or `libcodec2.dll`) in the codecs folder
  (the **Open codecs folder** button of the **?** popup), or set `DECT2_CODEC2_PATH` to its full path. Those are searched before the shipped copy.
- If the **?** button says `codec2 not found`, this copy of OnAir is incomplete (for example only `OnAir.exe` was copied out of the zip):
  unpack or install the whole package again. Or install codec2 yourself: macOS `brew install codec2`; Linux `sudo apt install libcodec2-1.2`
  (Ubuntu 22.04: `libcodec2-1.0`) or `sudo dnf install codec2`; Windows `libcodec2.dll` from a FreeDV install or MSYS2
  (`pacman -S mingw-w64-x86_64-codec2`, with the DLLs it needs) into the codecs folder.
- Building OnAir yourself: the codec2 library is built with it (`cmake --build build`, file `libcodec2*` next to the programs in the build folder).
  `-DDECT2_BUNDLE_CODEC2=OFF` leaves it out; OnAir then uses an installed codec2 as before.

Open HF digital and pick FreeDV (or M17): the **?** button says `codec2 found: <path>`.

## HDC (HD Radio sound)

HD Radio sound uses HDC, a patented codec. OnAir shows the stations, the data and the pictures, but plays no sound. To hear HD Radio,
use nrsc5 (<https://github.com/theori-io/nrsc5>) with an RTL-SDR or an IQ recording.

## DRA+ (CDR sound)

CDR sound uses DRA+, a patented codec that is not openly described. OnAir lists the services but plays no sound. There is no free
DRA+ decoder to install at this time. You can save the raw sound with **Save stream** and play it in an FFmpeg built with a DRA patch (see the README, "Saving DRA sound").

## AVS+, AVS2, AVS3 and DRA (DTMB television in China)

DTMB channels in China send their pictures as AVS+ (stream type 0x42; some as AVS2, 0xD2, or AVS3, 0xD4) and some send their sound as
DRA. These are Chinese patented codecs, and OnAir ships none of them: it plays what the FFmpeg built into it can decode.

- AVS1 in its JiZhun profile: FFmpeg's `cavs` decoder plays it (the macOS and Windows downloads; the Linux packages carry a smaller
  FFmpeg without it).
- AVS+ (the broadcast profile, 0x48): FFmpeg's `cavs` decoder refuses it ("only support JiZhun profile", FFmpeg 8 and 9). OnAir lists
  the service as TV, plays its sound, and says in the player that there is no picture. There is no free AVS+ decoder to install at this
  time.
- AVS2 and AVS3: FFmpeg decodes them only when it is built with libdavs2 or libuavs3d, which the FFmpeg in OnAir is not. Save the stream
  (Outputs: file or UDP) and play it with an FFmpeg build that has them (ffplay).
- DRA sound: FFmpeg has no DRA decoder. The player says "DRA audio: no decoder available"; a second sound track in another format plays.

The player panel has a **?** button next to these messages. A channel that signals a CA system (a CA descriptor, free_CA_mode) but sends
its streams in the clear is not shown as scrambled: OnAir goes by the scrambling bits of the stream packets.

## AMBE (Inmarsat Aero voice)

Aero C channel voice uses an AMBE vocoder. OnAir decodes the P channel data only and does not tune voice channels. Separate programs,
for example JAERO (<https://github.com/jontio/JAERO>), work on C channels.

## AMBE variant (Iridium voice)

OnAir counts Iridium voice frames but does not decode them. It hands the data off instead: **Save for iridium-toolkit** writes a
`.bits` file. Then:

1. Get iridium-toolkit: <https://github.com/muccc/iridium-toolkit>
2. `iridium-parser.py output.bits > output.parsed`
3. Install the `ir77_ambe_decode` program from osmo-ir77 (<http://git.osmocom.org/osmo-ir77>), as the toolkit's README explains, and
   use its `stats-voc.py` or `voc-cluster.py` to play the voice.

## AAC, xHE-AAC and the other FFmpeg codecs

DAB/DAB+ (MP2, AAC), DRM (AAC, xHE-AAC) and the TV modes decode with FFmpeg, which is built into every OnAir download (FFmpeg 8.0).
Nothing to install, with one exception: the SBR and parametric stereo of DAB+ HE-AAC (HE-AAC and HE-AAC v2 stations). FFmpeg cannot apply
them to the 960-sample frames of DAB+ ("SBR with 960 frame length"), so without help those stations play the core only: band-limited, and
HE-AAC v2 in mono. OnAir uses faad2 for them when it is installed (Linux: the `libfaad2` package; macOS: `brew install faad2`; Windows: a
`libfaad.dll`, `libfaad2.dll` or `libfaad-2.dll` beside the program, for example the one that ships with QT-DAB). It is found at run time and not shipped; the DAB player panel says which decoder is working and whether SBR
and PS are applied. Some xHE-AAC tools (harmonic SBR, PVC, inter-TES, MPEG Surround 212) are not in FFmpeg yet; DRM stations that use
them stay silent and the DRM view says so.

## AC-4 (ATSC 3.0 sound)

ATSC 3.0 broadcasts in the US and South Korea (and a few DVB services) carry their sound as Dolby AC-4. FFmpeg, including the one inside
OnAir, has **no AC-4 decoder** (it knows the codec id but no decoder: `ffmpeg -h decoder=ac4` says so). OnAir therefore leaves AC-4 sound out
and says so in the ATSC 3.0 tab and the TV tab, with a **?** card. Picture and any AAC or AC-3 sound track of the service play as usual.

**Optional support.** If you provide `libavcodec` and `libavutil` (shared libraries) from an FFmpeg that contains a decoder named `ac4`, OnAir
loads them at run time from the codecs folders (or the folder in `DECT2_AC4_DIR`), decodes the AC-4 access units of the selected service,
mixes them to stereo or 5.1 and plays them (internally: decoded with your library, encoded as AC-3 by OnAir's own FFmpeg, put in the
transport stream). The libraries are used through a small, stable interface, so an FFmpeg of another major version than OnAir's works.
Put them, and the libraries they need (`libswresample` and others), together in the codecs folder, then press **Look again**.

**Where such an FFmpeg comes from.** The only public AC-4 decoder known to this project is the patch
`ffmpeg_ac4.patch` in <https://github.com/funnymanva/ffmpeg-with-ac4> (FFmpeg 6.1 plus a 5,900 line decoder, `libavcodec/ac4dec.c`, by Paul B Mahol, 2019).
Its `build-ffmpeg` script builds a **static** `ffmpeg` program with `--disable-shared`, which does not give OnAir what it needs: apply the patch
to FFmpeg 6.1 yourself and configure it with `--enable-shared --disable-static`. The patch does not build with FFmpeg 7 or newer (it uses the
removed `avctx->channels`). OnAir's test of this path used a stock FFmpeg with the AC-3 decoder standing in for the AC-4 one; **it has not been
tried on a real AC-4 broadcast**, and nobody has checked how complete the patch's decoder is (OnAir uses its first presentation, its default).

**Licences.** The patch's decoder carries the LGPL-2.1-or-later header of FFmpeg, and the build script is MIT-licensed (it is derived from
markus-perl/ffmpeg-build-script): for copyright you may build and use them yourself. Copyright is not the whole story: AC-4 is a Dolby codec covered
by patents, licensed to makers of products (and Dolby's AC-4 specification is ETSI TS 103 190, public, but implementing it is not a patent licence).
OnAir ships no AC-4 code, no FFmpeg patch and downloads nothing; the build script above also pulls in GPL and non-free libraries if you ask it to
(`--enable-gpl-and-non-free`), which would make that binary unredistributable. Whether you may use it where you live is your responsibility.

## Legal note

Some of these codecs are covered by patents in some countries. Whether you install and use them is your choice and your
responsibility; check the rules where you live. OnAir does not ship them, does not download them, and contains no code from them.

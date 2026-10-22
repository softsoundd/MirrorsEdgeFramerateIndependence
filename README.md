# Mirror's Edge Framerate Independence Mod

> [!NOTE]
> Use of this mod is currently allowed for speedrun.com submissions, under the condition that if any game-breaking issue is later discovered with it then your run will be invalidated.

This is a work in progress mod for Mirror's Edge that decouples the game's simulation tick from its rendered frames. It's intended for speedrunners who wish to play at higher framerates while maintaining expected gameplay behaviour a fixed tick rate of 62hz (matching the default 62fps cap), though its benefit can carry across to casual play too by preventing issues that occur at higher tick rates (e.g. Chapter 1 building slide becoming uncontrollable).

To achieve this, the visible frame is interpolated to the real framerate at one tick behind. Camera rotation is an exception to this system, where it is instead shown in the present the input arrives, so mouse movement does not have any latency penalty. In practice however you shouldn't notice any real latency, especially at higher framerates.

The mod is functionally complete, though requires further testing and polish before I'd consider it final.

## Install

1. Download the latest version from [releases](https://github.com/softsoundd/MirrorsEdgeFramerateIndependence/releases).
2. Copy `dinput8.dll` into the game's `Binaries` folder.
3. Set your desired framerate with [Mirror's Edge Tweaks](https://github.com/softsoundd/MirrorsEdgeTweaks), or change the `MaxSmoothedFrameRate` value in `%USERPROFILE%\Documents\EA Games\Mirror's Edge\TdGame\Config\TdEngine.ini`.

Optionally copy `framerate-mod.ini` next to `dinput8.dll` if you want to adjust the mod's settings, like setting your own desired tick/loading rate, or letting the mod own the framerate cap. Without it the defaults apply which should suit most users.

## Recommended settings

- It's generally recommended to cap around -5% under your display's max refresh rate. 
- For the mouse, leave `LateLatchMouse` and `MouseResample` on together. The latch removes the tick wait and the interpolation lag from rotation, and the resampler removes the sim's batch ripple (e.g. ripple during look-constrained moves like wallclimb side steps).
- If you're after the minimal possible latency then do not use an external frame limiter, always use the `Cap` config to leave this up to the game. External limiters hold the finished frame at the present stage, so input latency in that frame ages by the whole wait. This has always been the case even before this mod, but it's worth mentioning. If you don't mind the added latency and would rather the smoother frame pacing of an external limiter then that's fine.
- Casual players might want to set `PaceRate=0` for faster level streaming loads.

## Known limitations/issues

- Currently supports non-SecuROM 1.0.1.0 builds of the game. This shouldn't be a blocker for most users given builds outside this spec can no longer be obtained via official channels. Complete version support will be addressed in the future.
- As aforementioned, it is an expected design constraint that camera position and world meshes are one sim interval - at a tick rate of 62hz this is about 16 ms in the past. Camera rotation is exempt from this behaviour.
- If your system struggles to achieve framerates higher than 62fps to begin with, then you may notice worse latency/stuttering than without the mod. Also, in areas where the PhysX fog emitters are misconfigured to spawn even when PhysX disabled, PhysX falls back to the CPU which can dramatically bring down framerates, so you may notice latency/stuttering in these scenarios as well. Setting `EvenPacing=1` can remove the stutter, otherwise to fix the framerate dip you'd need to either turn PhysX back on, or install the lag fixes located here: https://drive.google.com/drive/folders/17zquu0oWI8kSzXEB7WeoR1cAPgEcB-dJ?usp=drive_link
- Some users have reported `MouseResample=1` not addressing the ripple effect - yet to determine why that is. To work around this you can instead set the framerate cap to a multiple of `TickRate` (124, 186, or 248 at 62hz) which gives a clean sim and render interleave.
- Some users have reported lag/stuttering when running the mod at an uncapped framerate. It's generally advised to run capped for cleaner frame times anyway, though I'll look into what causes this.
- Some users have reported audio glitches with the mod.
- PhysX cloth advances at its own timestep rate, independent of sim rate. The 'PhysX FPS' setting in [Mirror's Edge Tweaks](https://github.com/softsoundd/MirrorsEdgeTweaks) can adjust the timestep rate separately.
- Interpolation of material shaders has not yet been accounted for. Not of concern for speedrunners but for casual players this is something to keep in mind.
- Rarely certain meshes fail to interpolate for whatever reason.

## Build

Visual Studio 2019 or 2022 with the C++ desktop workload, x86 target:

```
cd framerate-mod
build.bat
```

Output is at `framerate-mod/build/dinput8.dll` and generates a copy of `framerate-mod.ini`.

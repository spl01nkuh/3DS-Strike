================================================================
 Street Fighter III: 3rd Strike - Nintendo 3DS port (homebrew)
================================================================

An unofficial port of Street Fighter III: 3rd Strike to the Nintendo 3DS,
built from the crowded-street PS2 decompilation (3s-decomp / 3sx) and the
demmis98 PSP port (3s-psp). Runs on Old 3DS and New 3DS.

This package contains ONLY the homebrew program. It contains NO game data.
You must supply your own game files from a copy you legally own.

----------------------------------------------------------------
 WHAT YOU NEED
----------------------------------------------------------------
1. A 3DS with homebrew (Luma3DS + the Homebrew Launcher).

2. SF33RD.AFS  -- the game data, extracted from your own PS2 copy of
   "Street Fighter Anniversary Collection" (or the JP 3rd Strike PS2 disc).
   This file is NOT included and cannot be shared.

3. dspfirm.cdc -- your console's DSP firmware, for SOUND. Dump it once with
   GodMode9 (it is unique to your console; do not share it). Without it the
   game still runs, just silently.

----------------------------------------------------------------
 INSTALL
----------------------------------------------------------------
On your SD card:

  /3ds/3rd-strike.3dsx                     <- this program
  /3ds/sf3/resources/SF33RD.AFS            <- your game data (step 2)
  /3ds/sf3/dspfirm.cdc                     <- your DSP firmware (step 3, for sound)

Then launch "SFIII 3rd Strike" from the Homebrew Launcher.

----------------------------------------------------------------
 CONTROLS
----------------------------------------------------------------
 Menus:  A = confirm     B = cancel
         On-screen prompts now show Nintendo buttons (A/B/X/Y/L/R/ZL/ZR).

 In a fight (default layout -- fully remappable, see below):
   D-Pad / Circle Pad ...... move
   X / Y / L ............... Light / Medium / Heavy Punch
   A / B / R ............... Light / Medium / Heavy Kick
   ZL / ZR ................. 3x Punch / 3x Kick      (New 3DS)
   START ................... start / pause
   SELECT ................. coin / insert credit

 Rebind any button in  OPTION -> BUTTON CONFIG.  ZL and ZR are now
 fully assignable too (not just fixed macros). Your layout is saved.

----------------------------------------------------------------
 NEW IN THIS BUILD
----------------------------------------------------------------
- SAVING: settings, button config, options, rankings and unlock
  progress now persist to your SD card and survive a reboot
  (stored at /3ds/sf3/SETTINGS.BIN -- created automatically).
- Nintendo button glyphs on all on-screen prompts.
- ZL / ZR are fully remappable in BUTTON CONFIG.
- Smoother pause-menu audio (the muffle is no longer harsh/lo-fi).

----------------------------------------------------------------
 NOTES / KNOWN ISSUES
----------------------------------------------------------------
- New 3DS runs best. Old 3DS is playable; very busy stages may dip below 60fps.
- Sound requires dspfirm.cdc (see above).
- One attract-screen opening effect (text dissolving into squares) still
  shows minor artifacts. Gameplay and menus are unaffected.

----------------------------------------------------------------
 CREDITS
----------------------------------------------------------------
- crowded-street : 3s-decomp / 3sx (PS2 decompilation & PC port)
- demmis98       : 3s-psp (PSP port this 3DS port builds on)
- Capcom         : Street Fighter III: 3rd Strike (original game)

This is a non-commercial fan project. Not affiliated with or endorsed by Capcom.

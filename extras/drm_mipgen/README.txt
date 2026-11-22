================================================================
  Optional: HD texture pack fix (mipmaps)
================================================================

You only need this if you use the AI-Enhanced HD texture pack for
Tomb Raider: Legend:

    https://www.nexusmods.com/tombraiderlegend/mods/97

Nothing here is needed for the VR mod itself, and this folder does not
go into the game folder.


WHY
---
That pack's textures were saved without mipmaps: the smaller copies of
each texture the game switches to at a distance. Without them, floors,
walls and foliage further away shimmer and sparkle as you move. On a
monitor it is distracting; in VR it is much worse.

The fix rebuilds the missing mipmaps from the pack's own textures, the
same way the original game had them. The pack's files are not changed;
converted copies are made beside them.


WHAT YOU NEED
-------------
  * The texture pack, downloaded and unpacked into a folder.
  * TR7-Menu-Hook installed in the game folder (it is what loads the
    game's "mods" folder, which the pack uses).
  * Free disk space: about the size of the pack (more if you keep full
    4K size). The script checks this before it starts.


HOW
---
  1. Drag the unpacked texture pack's folder onto

         Fix HD texture pack.bat

     (or double-click it and drag the folder into its window).

  2. Choose the texture size:
       1 = cap at 2048 pixels -- recommended. The game is a 32-bit
           program and can run out of memory with full 4K textures,
           especially in VR.
       2 = keep the full size.

  3. Wait. It can take from several minutes to over an hour. If it is
     interrupted, run it again with the same folder: finished files are
     skipped.

  4. When it asks, let it move the files into the game's mods folder,
     or move them yourself from the "<pack folder> - mipmapped" folder
     it made, into

         <game folder>\mods

     Steam's default game folder is
         C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider Legend

To go back to the pack as it was, put its original files in the mods
folder again.


FILES
-----
  Fix HD texture pack.bat   the step-by-step script (use this)
  drm_mipgen.exe            the converter it runs
  origtex.txt               which of the game's textures had mipmaps
                            originally, and how many (so textures the
                            game shipped without them stay that way)

drm_mipgen.exe can also be run from a command prompt; run it with no
arguments to see its options.

Game Categories Lite v1.8

The main differences in this version are the stabilization in the visibility of the
Game Categories options listed within the System Settings, as well as modifications &
enhancements to the gclite_filter.txt file, including hiding entire categories on the XMB.

------------------------------------------
Enhanced gclite_filter.txt Info:
------------------------------------------
If you want to hide certain update/game/dlc on the XMB (no ISOs), create a file named gclite_filter.txt
and add sectioned entries as outlined below. Put the file in your seplugins folder.
Sections supported:
===HIDDEN CATEGORIES=== (category names, optional "ms0," or "ef0," prefix.)
===HIDDEN APPS=== (filepaths, optional "ms0:/" or "ef0:/" prefix. ISOs not supported.)


>> Example /seplugins/gclite_filter.txt:
===HIDDEN CATEGORIES===
Emulators
ms0, PSP
ef0, Homebrew

===HIDDEN APPS===
ef0:/PSP/GAME/Emulators/s9xTYLme_mod
/PSP/GAME/UCUS98744
ms0:/PSP/GAME/PS1/Final Fantasy VII


These enhancements were made to add the ability to specify the device and/or filepath where 
the category/game/update/dlc folder you want to hide resides. The goal in adding this granularity 
in filtering was to avoid the admittedly minuscule and relatively inconsequential situation of 
inadvertently hiding folders with the same name stored across multiple devices/categories. But it 
was important for my "Homebrew Sorter Ultimate" app to have the ability to define the exact apps 
you want to hide, and on which PSP Go storage device (Memory Stick vs. Internal Storage).


Alternatively, you can still use the legacy gclite_filter.txt file format, where you simply enter the 
folder name of an update/game/dlc folder per line. But unlike past versions of the plugin, the folder 
names are case-INsensitive.


>> Example legacy-format /seplugins/gclite_filter.txt:
UCUS98744
ISO_TOOL
UCES01264


NOTE: Legacy-format gclite_filter.txt files will be auto-converted to the new "===SECTION==="
gclite_filter.txt format if you run my "Homebrew Sorter Ultimate" app. (You can hide 
update/game/dlc folders directly from the "Homebrew Sorter Ultimate app's GUI.)

------------------------------------------
Additional Info:
------------------------------------------
If you want a translation of the visible options then edit the file category_lite_en.txt
and save it using the language code that first you (e.g. "es" for spanish).

Note: you must use UTF-8 encoding for the translation files (without unicode BOM).

Languages supported: "ja", "en", "fr", "es", "de", "it", "nl", "pt", "ru", "ko", "ch1", "ch2"

Notes: make sure that this is the 1st plugin listed in vsh.txt

------------------------------------------
Known issues:
------------------------------------------
>> Unknown if fixable:
* Change of category in the PSPGo requires a VSH reset.
* Vanilla (non-ARK) Adrenaline may require enabling the "Category prefixes" setting.

>> Unrelated to gclite:
* ME & vanilla (non-ARK) Adrenaline don't merge the categories with the same name between /ISO and /PSP/GAME.

>> Folder mode limitations/bugs:
* Max categories: 8 (included uncategorized).
* Folder name + homebrew folder name: 30 character, e.g.: My homebrews/AwesomeBigHomebrew
  is valid but My homebrews/AwesomeBigHomebrews isn't (i am not counting the "/").
  Note: the japanese and other non-ascii characters are 2 bytes wide, e.g.: カラフル counts
  as 8 chars.

------------------------------------------
Changelog:
------------------------------------------
v1.8:
[+]Option to divide gclite_filter.txt into sections (Categories & Apps) with optional device-specific indicators (ms0 and ef0) and filepaths for hiding apps. (See "Enhanced gclite_filter.txt Info" section above.)
[!]Stabilize the inconsistently-displayed lists of options in the various gclite System Settings.
v1.7-js1 (October 17, 2017):
[!]Fix labels not showing on PSP go internal storage.
v1.6:
[+]Added new option to sort categories: Use CAT_XX or XXcategory_name (XX between 00 and 99).
v1.5-r4
[+]Added polish translation.
v1.5-r3
[!]Fix duplicated entries on iso category in folder mode (PRO).
v1.5:
[+]Support for categories in folder mode like Bubbletune's GCL (thx Nekmo for betatesting).
[+]Support to hide certain homebrews/games/dlc from the categories.
[+]Added subtitles to the config options.
[+]Empty categories are hidden by default.
[+]Non game folders are hidden by default on uncategorized content.
[+]Added folder mode benchmark (compile with BENCHMARK=1)
[+]Added bulgarian translation by Xian Nox.
[+]Added simple chinese translation by phoe-nix.
[+]Added Traditional-Chinese translation by Raiyou.
[+]Added Russian translation by Frostegater.
[+]Added italian translation by stevealexanderames.
[!]Force the uncategorized content to be the last item by default.
[!]Fixed UMD icon malfunction bug introduced in 1.4-r2.
v1.4:
[+]6.60 firmware support
[+]Allow the uncategorized folder to be sorted with your favorite app.
[+]Multiple language support.
[+]Added ja translation by popsdeco.
[+]Added de translation by KOlle and The Z.
v1.3:
[+]Support for categories in contextual menu.
[+]Support for plugin configuration in system settings.
[+]Added runtime detection for ME, so category games are now shown.
[!]Fixed issues with PSPGo (big thanks to raing3 to help me with the debugging).
v1.2:
[!]Fixed PSPGo categories, again (thx RUSTII for the tests).
[!]Fixed the free space display when the psp returns from sleep.
v1.1:
[!]Fixed PSPGo categories (thx RUSTII for the tests).
v1.0:
[+]First release.

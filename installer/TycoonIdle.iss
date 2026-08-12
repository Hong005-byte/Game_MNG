; Inno Setup script for Tycoon Idle -- builds a real Windows installer
; (Start Menu shortcut, optional desktop shortcut, uninstaller) from the
; same portable build the "release a new version" workflow already zips up
; into dist/TycoonIdle/ (see README.md's own build instructions). Doesn't
; touch that folder or the zip -- this is an alternative distribution
; format alongside it, not a replacement.
;
; Build with (from this installer/ folder, or any cwd -- paths below are
; relative to THIS SCRIPT's own location):
;   "C:\Program Files\Inno Setup 7\ISCC.exe" TycoonIdle.iss
; Output lands in dist\TycoonIdle-Setup-<version>.exe, next to the zip.
;
; MyAppVersion has no single source of truth to read from automatically
; (Version.h is C++, not something Inno Setup's preprocessor can include) --
; bump it here by hand alongside every Version.h bump.
#define MyAppName "Tycoon Idle"
#define MyAppVersion "1.5.1"
#define MyAppPublisher "Hong005-byte"
#define MyAppURL "https://github.com/Hong005-byte/Game_MNG"
#define MyAppExeName "Game Mng.exe"

[Setup]
; Fixed GUID -- identifies "this app" to Windows across versions/reinstalls
; (upgrades overwrite in place instead of registering as a separate app).
; Generated once for this project; never change it for a future version.
AppId={{B4E1C9E0-6F3A-4C8D-9A1E-7D2F5B3C8A91}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=TycoonIdle-Setup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; Modern 64-bit-architecture identifier (Inno Setup 6.3+) -- the old bare
; "x64" value here is deprecated now that ARM64 Windows exists too; this
; game itself is only ever built x64 (see the .vcxproj), so installing
; under 64-bit compatibility mode is the only case that makes sense.
ArchitecturesInstallIn64BitMode=x64compatible
; UninstallDisplayIcon and every [Icons] shortcut below already pick up the
; app's own icon automatically (no per-shortcut IconFilename override
; needed) since {#MyAppExeName} itself has one embedded now (see
; Game Mng/app.rc) -- this is specifically the SETUP.EXE'S own icon, shown
; in Explorer/Alt-Tab for the installer itself before anything's installed.
SetupIconFile=..\Game Mng\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
; No license file in this repo (see README) -- skip the license-acceptance
; wizard page entirely rather than pointing at something that doesn't exist.
; 2026-08-12 ("我不需要每次都要跑到github去重新下载了" -- the in-game
; one-click updater, see UpdateChecker::downloadAndRunInstaller): this
; installer can now be launched while a previous copy of the game is still
; RUNNING (the updater downloads it and starts it without the player
; closing the game first). Windows' own Restart Manager (which these two
; directives turn on) detects that {#MyAppExeName} has open file handles on
; the very files this install is about to overwrite, closes it gracefully
; (the game already saves on that close -- same code path as the player
; clicking the window's own X button, see GameWorld::run's Closed-event
; handling) before copying files, then relaunches it once done. Without
; these, overwriting a running exe's own file would just fail outright.
CloseApplications=yes
RestartApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

; Everything the portable zip already ships (exe + every runtime DLL it
; needs) -- see the "release a new version" workflow's own dist/TycoonIdle/
; staging step, this installer packages that exact same folder rather than
; duplicating its own copy of what needs bundling.
[Files]
Source: "..\dist\TycoonIdle\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

; Player data (saves/, settings.cfg) lives next to the exe (see
; Game::startSession/SaveManager) -- deliberately NOT removed on uninstall,
; same "don't delete what the player made" reasoning saves/ being gitignored
; already follows. Only the program files themselves go.
[UninstallDelete]
Type: files; Name: "{app}\*.dll"
Type: files; Name: "{app}\{#MyAppExeName}"

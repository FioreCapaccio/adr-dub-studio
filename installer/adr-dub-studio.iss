; Installer di ADR Dub Studio per Windows (Inno Setup 6). Si compila da CI: iscc /DMyAppVersion=1.0.0 installer\adr-dub-studio.iss
; La cartella "pkg" (accanto a "installer") contiene ADR Dub Studio.exe, ffmpeg.exe, ffprobe.exe, LICENSE, FFMPEG.txt.
#ifndef MyAppVersion
  #define MyAppVersion "2.2.0"
#endif
#define MyAppName "ADR Dub Studio"
#define MyAppExe "ADR Dub Studio.exe"

[Setup]
AppId={{6F1D7E0B-3C5A-4B8E-9D2A-5A7C1E9B4F31}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=Fioravante Capaccio
AppPublisherURL=https://github.com/FioreCapaccio/adr-dub-studio
AppSupportURL=https://github.com/FioreCapaccio/adr-dub-studio/issues
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
LicenseFile=..\pkg\LICENSE
OutputDir=..\installer_out
OutputBaseFilename=ADR-Dub-Studio-{#MyAppVersion}-Windows-x64-Setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequiredOverridesAllowed=dialog
UninstallDisplayIcon={app}\{#MyAppExe}
UninstallDisplayName={#MyAppName}

[Languages]
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "..\pkg\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExe}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

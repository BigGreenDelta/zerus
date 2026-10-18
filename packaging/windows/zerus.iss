; The per-user Zerus installer (docs/superpowers/specs/2026-10-18-windows-installer-design.md).
; scripts/package-windows-installer.py stages the tested portable zip's files,
; installation.json and uninstall\payload.txt in PayloadDir and compiles this
; script with Inno Setup 6. Nothing here changes a payload file: third-party
; DLLs and EXEs keep their bytes and signatures.
;
; The installer never touches the staged CLI (%USERPROFILE%\.local\bin), the
; session host images (.local\share\hgs\host), configuration, state, desktop
; settings or drafts. Restart Manager closes only processes that use files in
; the install folder; the session host runs from its staged image, so it and its
; sessions keep running through an upgrade.

#ifndef AppVersion
  #error Define AppVersion
#endif
#ifndef NumericVersion
  #error Define NumericVersion
#endif
#ifndef PayloadDir
  #error Define PayloadDir
#endif
#ifndef OutputDir
  #error Define OutputDir
#endif
#ifndef OutputName
  #error Define OutputName
#endif
#ifndef IconFile
  #error Define IconFile
#endif

[Setup]
; Never change the AppId: upgrades and the uninstaller find earlier installs by it.
AppId={{6F1B0C52-7E3A-4D8B-9C41-2A5E8F0D3B17}
AppName=Zerus
AppVersion={#AppVersion}
AppVerName=Zerus {#AppVersion}
AppPublisher=Zerus contributors
AppPublisherURL=https://github.com/ufna/zerus
AppSupportURL=https://github.com/ufna/zerus/issues
AppUpdatesURL=https://github.com/ufna/zerus/releases
VersionInfoVersion={#NumericVersion}
VersionInfoProductName=Zerus
VersionInfoProductTextVersion={#AppVersion}
; Per user, never elevated: {autopf} is %LOCALAPPDATA%\Programs.
PrivilegesRequired=lowest
DefaultDirName={autopf}\Zerus
; The folder is the installer's own: an upgrade removes files an older version
; left there, so the user does not choose an arbitrary one.
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 10 1903, as the portable zip and the winget manifest require.
MinVersion=10.0.18362
UninstallFilesDir={app}\uninstall
UninstallDisplayName=Zerus
UninstallDisplayIcon={app}\hgs-tray.exe
SetupIconFile={#IconFile}
; Restart Manager asks processes using files in {app} to close; the GUI then
; exits, its drafts already saved. Nothing outside {app} is closed.
CloseApplications=yes
RestartApplications=no
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ChangesAssociations=no
ChangesEnvironment=no

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Zerus"; Filename: "{app}\hgs-tray.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\hgs-tray.exe"; Description: "Start Zerus"; Flags: nowait postinstall skipifsilent
; A self-update runs the installer silently with /RELAUNCH after the desktop
; app has exited; the new version starts once its files are in place.
Filename: "{app}\hgs-tray.exe"; Flags: nowait; Check: Relaunch

[UninstallDelete]
Type: dirifempty; Name: "{app}"

[Code]
var
  Upgrade: Boolean;
  Payload: TStringList;

function Relaunch(): Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/RELAUNCH') = 0 then
      Result := True;
end;

{ Removes files in Folder (relative path Relative) that the new payload does
  not name, and then empty folders. uninstall\ keeps the uninstaller. }
procedure RemoveStale(const Folder, Relative: String);
var
  Find: TFindRec;
  Path, Name: String;
begin
  if not FindFirst(AddBackslash(Folder) + '*', Find) then
    Exit;
  try
    repeat
      if (Find.Name <> '.') and (Find.Name <> '..') then
      begin
        Path := AddBackslash(Folder) + Find.Name;
        if Relative = '' then
          Name := Find.Name
        else
          Name := Relative + '\' + Find.Name;
        if (Find.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
        begin
          if CompareText(Name, 'uninstall') <> 0 then
          begin
            RemoveStale(Path, Name);
            RemoveDir(Path);
          end;
        end
        else if Payload.IndexOf(Lowercase(Name)) < 0 then
        begin
          if DeleteFile(Path) then
            Log('Removed ' + Name + ', which this version no longer ships.')
          else
            Log('Could not remove ' + Name + ', which this version no longer ships.');
        end;
      end;
    until not FindNext(Find);
  finally
    FindClose(Find);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Lines: TArrayOfString;
  I: Integer;
begin
  { Only a folder an earlier installer created is cleaned up. }
  if CurStep = ssInstall then
    Upgrade := FileExists(ExpandConstant('{app}\installation.json'));
  { After every file is in place: a failed install rolls back and removes nothing. }
  if (CurStep = ssPostInstall) and Upgrade then
  begin
    if not LoadStringsFromFile(ExpandConstant('{app}\uninstall\payload.txt'), Lines) then
    begin
      Log('The payload list is missing; files of earlier versions stay.');
      Exit;
    end;
    Payload := TStringList.Create;
    try
      for I := 0 to GetArrayLength(Lines) - 1 do
        if Trim(Lines[I]) <> '' then
          Payload.Add(Lowercase(Trim(Lines[I])));
      RemoveStale(ExpandConstant('{app}'), '');
    finally
      Payload.Free;
    end;
  end;
end;

; Velox application installer. Google packages are installed by the app after acceptance.
#ifndef AppVersion
  #error AppVersion must be supplied by make-installer.ps1
#endif
#ifndef StageDir
  #error StageDir must be the verified portable release folder
#endif

[Setup]
AppId={{A626143B-6F41-4B4F-A1BB-5891AB129D81}
AppName=Velox
AppVersion={#AppVersion}
AppVerName=Velox {#AppVersion}
AppPublisher=Velox
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Velox Setup
VersionInfoProductName=Velox
DefaultDirName={localappdata}\Programs\Velox
DefaultGroupName=Velox
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.19041
DisableWelcomePage=no
DisableFinishedPage=no
DisableProgramGroupPage=yes
DisableDirPage=auto
DisableReadyPage=no
CloseApplications=yes
RestartApplications=no
UsePreviousTasks=yes
OutputDir=..\dist
OutputBaseFilename=Velox-Setup-{#AppVersion}
Compression=lzma2/normal
SolidCompression=yes
SetupIconFile=..\build\velox.ico
UninstallDisplayIcon={app}\Velox.exe
UninstallDisplayName=Velox
WizardStyle=modern dark hidebevels
WizardSizePercent=110
WizardBackColor=#0E1015
WizardImageBackColor=#0E1015
WizardImageFile=..\build\installer-art\wizard-164x360.png,..\build\installer-art\wizard-192x440.png,..\build\installer-art\wizard-246x528.png,..\build\installer-art\wizard-273x632.png,..\build\installer-art\wizard-328x720.png,..\build\installer-art\wizard-355x806.png,..\build\installer-art\wizard-410x912.png
WizardSmallImageFile=..\build\installer-art\small-55x55.png,..\build\installer-art\small-64x68.png,..\build\installer-art\small-83x80.png,..\build\installer-art\small-92x97.png,..\build\installer-art\small-110x106.png,..\build\installer-art\small-119x123.png,..\build\installer-art\small-138x140.png

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"

[Messages]
english.WelcomeLabel1=Welcome to Velox
english.WelcomeLabel2=Your games. Your phone. Your space.%n%nSetup installs Velox for your Windows account. No administrator access is required.%n%nOn first launch, Velox downloads and sets up Android automatically after you accept Google's SDK terms. Allow about 2.2 GB of downloads and 10 GB of free space. No separate SDK installation is needed.
english.FinishedHeadingLabel=Velox is ready
english.FinishedLabel=You'll find Velox in the Start menu. Open it to set up your gaming phone.
english.SelectTasksDesc=Choose your shortcuts.
german.WelcomeLabel1=Willkommen bei Velox
german.WelcomeLabel2=Deine Spiele. Dein Handy. Dein Platz.%n%nVelox wird für dein Windows-Konto installiert. Administratorrechte sind nicht erforderlich.%n%nBeim ersten Start lädt Velox Android automatisch herunter und richtet es ein, nachdem du die SDK-Bedingungen von Google akzeptiert hast. Plane ca. 2,2 GB Download und 10 GB freien Speicher ein. Eine separate SDK-Installation ist nicht nötig.
german.FinishedHeadingLabel=Velox ist bereit
german.FinishedLabel=Du findest Velox im Startmenü. Öffne es, um dein Gaming-Handy einzurichten.
german.SelectTasksDesc=Wähle deine Verknüpfungen aus.

[CustomMessages]
english.DesktopShortcut=Create a desktop shortcut
english.Shortcuts=Shortcuts:
english.OpenVelox=Open Velox
english.KeptData=Your phones, apps, saves and settings stay on this PC after uninstalling Velox.
german.DesktopShortcut=Desktop-Verknüpfung erstellen
german.Shortcuts=Verknüpfungen:
german.OpenVelox=Velox öffnen
german.KeptData=Deine Handys, Apps, Spielstände und Einstellungen bleiben nach der Deinstallation auf diesem PC erhalten.

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopShortcut}"; GroupDescription: "{cm:Shortcuts}"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Velox"; Filename: "{app}\Velox.exe"; Check: not WizardNoIcons
Name: "{autodesktop}\Velox"; Filename: "{app}\Velox.exe"; Tasks: desktopicon

[Dirs]
Name: "{app}\data"; Flags: uninsneveruninstall

[INI]
Filename: "{app}\data\settings.ini"; Section: "launcher"; Key: "language"; String: "{code:InitialLanguage}"; Check: NeedsLanguageSeed; Flags: createkeyifdoesntexist

[Run]
Filename: "{app}\Velox.exe"; Description: "{cm:OpenVelox}"; Flags: nowait postinstall skipifsilent

[Code]
function NeedsLanguageSeed: Boolean;
begin
  Result := not FileExists(ExpandConstant('{app}\data\settings.ini'));
end;

function InitialLanguage(Param: String): String;
begin
  if ActiveLanguage = 'german' then Result := '1' else Result := '0';
end;

procedure InitializeUninstallProgressForm;
begin
  UninstallProgressForm.PageDescriptionLabel.Caption := CustomMessage('KeptData');
end;

procedure FitBrandRail(PageID: Integer);
begin
  WizardForm.WizardBitmapImage.Parent := WizardForm;
  WizardForm.WizardBitmapImage.Left := 0;
  WizardForm.WizardBitmapImage.Top := 0;
  WizardForm.WizardBitmapImage.Height := WizardForm.ClientHeight;
  WizardForm.WizardBitmapImage.Stretch := True;
  WizardForm.WizardBitmapImage.Visible := PageID = wpWelcome;
  WizardForm.WizardBitmapImage.BringToFront;
  WizardForm.WizardBitmapImage2.Parent := WizardForm;
  WizardForm.WizardBitmapImage2.Left := 0;
  WizardForm.WizardBitmapImage2.Top := 0;
  WizardForm.WizardBitmapImage2.Height := WizardForm.ClientHeight;
  WizardForm.WizardBitmapImage2.Stretch := True;
  WizardForm.WizardBitmapImage2.Visible := PageID = wpFinished;
  WizardForm.WizardBitmapImage2.BringToFront;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  FitBrandRail(CurPageID);
end;

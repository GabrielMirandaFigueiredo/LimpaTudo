; Instalador do LimpaTudo (Inno Setup). Gera "LimpaTudo-Setup.exe".
; O workflow do GitHub passa /DAppVersion=... e /DVst3Dir=<pasta do LimpaTudo.vst3 compilado>.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef Vst3Dir
  #define Vst3Dir "..\build\LimpaTudo_artefacts\Release\VST3\LimpaTudo.vst3"
#endif

[Setup]
AppId={{7B1E6C8A-3D52-4F0B-9A47-5C2D8E1F6A90}
AppName=LimpaTudo
AppVersion={#AppVersion}
AppPublisher=Gabriel
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
Uninstallable=yes
UninstallDisplayName=LimpaTudo (VST3)
OutputBaseFilename=LimpaTudo-Setup
OutputDir=output
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
ShowLanguageDialog=no

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Messages]
brazilianportuguese.WelcomeLabel2=Este assistente vai instalar o plugin [name/ver] (VST3) automaticamente na pasta padrão de plugins do Windows.%n%nFeche o seu DAW antes de continuar.
brazilianportuguese.FinishedLabel=O LimpaTudo foi instalado em C:\Program Files\Common Files\VST3.%n%nAbra o seu DAW e reescaneie os plugins VST3 (se ele não aparecer sozinho).

[Files]
; copia a pasta LimpaTudo.vst3 inteira para C:\Program Files\Common Files\VST3\LimpaTudo.vst3
Source: "{#Vst3Dir}\*"; DestDir: "{commoncf64}\VST3\LimpaTudo.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs

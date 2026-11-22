Unicode True
RequestExecutionLevel user

!include "LogicLib.nsh"
!include "MUI2.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!ifndef APP_EXE
  !error "Define APP_EXE with the path to the built Windows executable."
!endif
!ifndef OUTPUT_FILE
  !error "Define OUTPUT_FILE with the desired installer path."
!endif
!ifndef VERSION
  !error "Define VERSION as major.minor.patch."
!endif
!ifndef APP_NAME
  !define APP_NAME "Shutdown Warning"
!endif
!ifndef APP_REGKEY
  !define APP_REGKEY "ShutdownWarning"
!endif
!ifndef APP_ICON
  !define APP_ICON "${__FILEDIR__}\..\..\app\windows\ShutdownWarning.ico"
!endif

!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_REGKEY}"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"
SetCompressor /SOLID lzma

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "CompanyName" "Shutdown Warning"
VIAddVersionKey /LANG=1033 "FileDescription" "${APP_NAME} Installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (c) Shutdown Warning contributors"
VIAddVersionKey /LANG=1033 "ProductName" "${APP_NAME}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"

!define MUI_ICON "${APP_ICON}"
!define MUI_UNICON "${APP_ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\ShutdownWarning.exe"
!define MUI_FINISHPAGE_RUN_PARAMETERS "--settings"
!define MUI_FINISHPAGE_RUN_TEXT "Open ${APP_NAME} settings"
!define MUI_FINISHPAGE_RUN_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!macro RunInstalledCommand ARGUMENT FAILURE
  ClearErrors
  ExecWait '"$INSTDIR\ShutdownWarning.exe" ${ARGUMENT}' $0
  ${If} ${Errors}
  ${OrIf} $0 != 0
    MessageBox MB_OK|MB_ICONSTOP "${FAILURE}" /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
!macroend

Function .onInit
  ${IfNot} ${AtLeastWin10}
  ${OrIfNot} ${AtLeastBuild} 15063
  ${OrIfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "Shutdown Warning requires 64-bit Windows 10 version 1703 or newer." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "Install"
  ReadRegStr $1 HKCU "${UNINSTALL_KEY}" "InstallLocation"
  ${If} $1 != ""
  ${AndIf} $1 != $INSTDIR
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} is already installed in $1. Choose that folder or uninstall it first." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\ShutdownWarning.exe"
    ${If} $1 == ""
      MessageBox MB_OK|MB_ICONSTOP "This folder already contains another copy of Shutdown Warning. Choose a different folder." /SD IDOK
      SetErrorLevel 2
      Abort
    ${EndIf}
    !insertmacro RunInstalledCommand "--exit" "Could not close ${APP_NAME}. Exit it from the notification area and run Setup again."
  ${EndIf}

  SetOutPath "$INSTDIR"
  File /oname=ShutdownWarning.exe "${APP_EXE}"
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  CreateShortCut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\ShutdownWarning.exe" "--settings"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\ShutdownWarning.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
SectionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "Uninstall"
  ${If} ${FileExists} "$INSTDIR\ShutdownWarning.exe"
    !insertmacro RunInstalledCommand "--exit" "Could not close ${APP_NAME}. Exit it from the notification area and run Uninstall again."
    !insertmacro RunInstalledCommand "--remove-startup" "Could not remove ${APP_NAME} from startup."
  ${EndIf}

  ClearErrors
  Delete "$INSTDIR\ShutdownWarning.exe"
  ${If} ${Errors}
    MessageBox MB_OK|MB_ICONSTOP "Could not remove ShutdownWarning.exe. Close it and run Uninstall again." /SD IDOK
    SetErrorLevel 2
    Abort
  ${EndIf}

  ReadRegStr $1 HKCU "${UNINSTALL_KEY}" "InstallLocation"
  ${If} $1 == $INSTDIR
    Delete "$SMPROGRAMS\${APP_NAME}.lnk"
    DeleteRegKey HKCU "${UNINSTALL_KEY}"
  ${EndIf}
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd

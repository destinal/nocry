; nocry per-user installer: no administrator rights needed.
; makensis /DVERSION=x.y.z /DSTAGE=<folder with the app> /DOUT=<installer.exe> installer.nsi
Unicode true
!include "MUI2.nsh"
Name "nocry"
OutFile "${OUT}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\nocry"
InstallDirRegKey HKCU "Software\nocry" "InstallDir"
!define MUI_ICON "nocry.ico"
!define MUI_UNICON "nocry.ico"
!define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\nocry"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\nocry-gui.exe"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "nocry"
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateDirectory "$SMPROGRAMS\nocry"
  CreateShortcut "$SMPROGRAMS\nocry\nocry.lnk" "$INSTDIR\nocry-gui.exe"
  CreateShortcut "$SMPROGRAMS\nocry\Uninstall nocry.lnk" "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "Software\nocry" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST}" "DisplayName" "nocry"
  WriteRegStr HKCU "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST}" "DisplayIcon" "$INSTDIR\nocry-gui.exe"
  WriteRegStr HKCU "${UNINST}" "Publisher" "nocry project"
  WriteRegStr HKCU "${UNINST}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${UNINST}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${UNINST}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  RMDir /r "$INSTDIR"
  Delete "$SMPROGRAMS\nocry\nocry.lnk"
  Delete "$SMPROGRAMS\nocry\Uninstall nocry.lnk"
  RMDir "$SMPROGRAMS\nocry"
  DeleteRegKey HKCU "${UNINST}"
  DeleteRegKey HKCU "Software\nocry"
SectionEnd

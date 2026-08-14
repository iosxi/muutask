' MuuTask をコンソール ウィンドウなしで起動する。
' このファイルをダブルクリックするか、ショートカットを作って使う。
Option Explicit

Dim shell, fso, base, launcher
Set shell = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

base = fso.GetParentFolderName(WScript.ScriptFullName)
launcher = base & "\.venv\Scripts\pythonw.exe"
If Not fso.FileExists(launcher) Then launcher = "pythonw.exe"

shell.Run """" & launcher & """ """ & base & "\app.py""", 0, False

Dim fso, shell
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("Shell.Application")

Dim scriptDir
scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)

Dim srcDll, dstDll, srcJson, dstJson
srcDll = fso.BuildPath(scriptDir, "Builds\VisualStudio2026\x64\Debug\VST3\PitchTunePlugin.vst3\Contents\x86_64-win\PitchTunePlugin.vst3")
If Not fso.FileExists(srcDll) Then
    srcDll = fso.BuildPath(scriptDir, "Builds\VisualStudio2026\x64\Release\VST3\PitchTunePlugin.vst3\Contents\x86_64-win\PitchTunePlugin.vst3")
End If

dstDir = "C:\Program Files\Common Files\VST3\Self-Pl\Contents\x86_64-win\"
srcJson = fso.BuildPath(scriptDir, "Builds\VisualStudio2026\x64\Debug\VST3\PitchTunePlugin.vst3\Contents\Resources\moduleinfo.json")
If Not fso.FileExists(srcJson) Then
    srcJson = fso.BuildPath(scriptDir, "Builds\VisualStudio2026\x64\Release\VST3\PitchTunePlugin.vst3\Contents\Resources\moduleinfo.json")
End If
dstJsonDir = "C:\Program Files\Common Files\VST3\Self-Pl\Contents\Resources\"

Dim cmd
cmd = "/c mkdir """ & dstDir & """ 2>nul & mkdir """ & dstJsonDir & """ 2>nul & copy /Y """ & srcDll & """ """ & dstDir & "PitchTunePlugin.vst3"" & copy /Y """ & srcJson & """ """ & dstJsonDir & "moduleinfo.json"" & echo [SUCCESS] Updated PitchTunePlugin.vst3! & pause"

shell.ShellExecute "cmd.exe", cmd, "", "runas", 1

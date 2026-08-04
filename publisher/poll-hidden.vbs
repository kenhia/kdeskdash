' poll-hidden.vbs — launch claude-pub.sh poll with no console window.
'
' The scheduled task runs as Interactive (registering S4U needs elevation), and
' Task Scheduler always shows a console window for a console app in that mode —
' a bash window flashing every 5 minutes. WScript.Shell.Run with intWindowStyle
' 0 suppresses it. bWaitOnReturn is True so Task Scheduler sees the real exit
' code and the task's 2-minute ExecutionTimeLimit still applies.

Option Explicit

Dim sh, fso, bash, script, cmd
Set sh  = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")

bash   = sh.ExpandEnvironmentStrings("%ProgramFiles%") & "\Git\bin\bash.exe"
script = sh.ExpandEnvironmentStrings("%USERPROFILE%") & "\.claude\kdeskdash-pub\claude-pub.sh"

If Not fso.FileExists(bash) Then WScript.Quit 0
If Not fso.FileExists(script) Then WScript.Quit 0

' -l so the git-bash profile sets HOME; the script resolves its own paths from it.
cmd = """" & bash & """ -lc ""$HOME/.claude/kdeskdash-pub/claude-pub.sh poll"""

WScript.Quit sh.Run(cmd, 0, True)

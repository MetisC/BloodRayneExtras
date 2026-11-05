# test_boss_run.ps1 — fully autonomous Boss Battle / Challenge run.
# Launches the game, posts Enter to every window (title -> main -> submenu),
# injects the menu action via memory, then watches the trace.
param([int]$Action = 0x164, [int]$TimeoutSec = 240)
$gameDir = "H:\GOG\BloodRayne Terminal Cut"
$trace = "$gameDir\br1mod_trace.log"

Add-Type -TypeDefinition @"
using System;using System.Runtime.InteropServices;using System.Text;
using System.Collections.Generic;
public class WIN{
 [DllImport("user32.dll")]public static extern bool EnumWindows(IntPtr cb,IntPtr lp);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("kernel32.dll")]public static extern IntPtr OpenProcess(uint a,bool b,int p);
 [DllImport("kernel32.dll")]public static extern bool WriteProcessMemory(IntPtr h,IntPtr a,byte[] d,int n,out int w);
 [DllImport("kernel32.dll")]public static extern bool ReadProcessMemory(IntPtr h,IntPtr a,byte[] o,int n,out int r);
 public delegate bool EnumCb(IntPtr h,IntPtr l);
 public static List<IntPtr> Wins(uint pid){
   var l=new List<IntPtr>();
   EnumWindows(Marshal.GetFunctionPointerForDelegate(new EnumCb((h,x)=>{
     uint p2;GetWindowThreadProcessId(h,out p2);
     if(p2==pid)l.Add(h);return true;})),IntPtr.Zero);
   return l;}
}
"@ -IgnoreWarnings

function PostKey([uint32]$pid2,[int]$vk){
  foreach($w in [WIN]::Wins($pid2)){
    [void][WIN]::PostMessage($w,0x0100,[IntPtr]$vk,[IntPtr]0x001C0001)
    Start-Sleep -Milliseconds 40
    [void][WIN]::PostMessage($w,0x0101,[IntPtr]$vk,[IntPtr]0xC01C0001)
    Start-Sleep -Milliseconds 80
  }
}

# ---- launch ----
$old = Get-Process rayne1 -ErrorAction SilentlyContinue
if($old){$old|Stop-Process -Force;Start-Sleep -s 2}
Start-Process "$gameDir\rayne1.exe" -WorkingDirectory $gameDir
$p=$null
for($i=0;$i -lt 40 -and -not $p;$i++){Start-Sleep -Milliseconds 500;$p=Get-Process rayne1 -ErrorAction SilentlyContinue}
if(-not $p){"launch failed";exit 1}
"pid="+$p.Id
$h=[WIN]::OpenProcess(0x3A,$false,$p.Id)
$base=[long]$p.MainModule.BaseAddress
function A([long]$va){[IntPtr]($base+$va-0x400000)}
function R32([long]$va){$b=New-Object byte[] 4;[void][WIN]::ReadProcessMemory($h,(A $va),$b,4,[ref]($r=0));[BitConverter]::ToUInt32($b,0)}
function W32([long]$va,[uint32]$v){$w=0;[void][WIN]::WriteProcessMemory($h,(A $va),[BitConverter]::GetBytes($v),4,[ref]$w)}
function Mid(){R32 0x1D27B24}

# ---- title -> main menu: keep pressing enter until mid!=0 ----
$t0=Get-Date;$mid=0
while($mid -eq 0 -and ((Get-Date)-$t0).TotalSeconds -lt 90){
  PostKey $p.Id 0x0D
  Start-Sleep -Milliseconds 700
  $mid=Mid
}
"mid=$mid (after title)"
if($mid -ne 1){"not at main menu";exit 1}

# ---- enter New Game submenu (first item is New Game -> mid=2) ----
PostKey $p.Id 0x0D
$t0=Get-Date;$mid=Mid
while($mid -eq 1 -and ((Get-Date)-$t0).TotalSeconds -lt 20){
  Start-Sleep -Milliseconds 400; $mid=Mid
}
"mid=$mid (submenu?)"
if($mid -eq 1){ "submenu did not open; posting enter again"; PostKey $p.Id 0x0D; Start-Sleep -s 2; $mid=Mid; "mid=$mid" }

# ---- cascade NOP patch so the dispatcher accepts our action ----
function SetNOP([bool]$on){
  if($on){[void][WIN]::WriteProcessMemory($h,(A 0x4C8339),[byte[]](0x90,0x90),2,[ref]($w=0))}
  else   {[void][WIN]::WriteProcessMemory($h,(A 0x4C8339),[byte[]](0x74,0x05),2,[ref]($w=0))}
}

# ---- inject action on item[0]; repeat while any menu is up ----
$t0=Get-Date;$mid=-1;$lastInj=0
while(((Get-Date)-$t0).TotalSeconds -lt 90){
  $mid=Mid
  if($mid -eq 0){break}
  if(((Get-Date)-$t0).TotalSeconds - $lastInj -gt 1.5){
    SetNOP $true
    W32 0x1D27B20 0
    W32 (0x1D20068+4) ([uint32]$Action)
    $lastInj=((Get-Date)-$t0).TotalSeconds
    "action=0x{0:X} injected at mid=$mid t={1:N1}s" -f $Action,$lastInj
  }
  Start-Sleep -Milliseconds 100
}
SetNOP $false
"mid=$mid (post cascade)"

# ---- watch trace ----
if(Test-Path $trace){$len0=(Get-Item $trace).Length}else{$len0=0}
$seen=0;$t0=Get-Date
while($true){
  $p.Refresh();if($p.HasExited){"proc died";break}
  $lines = Get-Content $trace -ErrorAction SilentlyContinue
  $w = @($lines | Select-String "CHAL wave")
  if($w.Count -gt $seen){$seen=$w.Count;$w[-1].Line}
  $ap = @($lines | Select-String "APPEAR")
  if($ap){$ap | ForEach-Object {$_.Line};$ap=@()}
  $fin = $lines | Select-String "CHAL finished" | Select-Object -Last 1
  if($fin){"==> "+$fin.Line;break}
  if(((Get-Date)-$t0).TotalSeconds -gt $TimeoutSec){"watch timeout";break}
  Start-Sleep -Seconds 2
}
"final mid="+(Mid)

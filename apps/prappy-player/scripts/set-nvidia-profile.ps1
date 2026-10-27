#Requires -Version 7.0
param([switch]$Apply)
$ErrorActionPreference='Stop'
# Explicit, machine-local opt-in. This undocumented setting is never changed by
# the player. A complete NVAPI profile export precedes any mutation.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Runtime.InteropServices;
public static class PlayerNvidiaProfile {
 [DllImport("nvapi64.dll",CallingConvention=CallingConvention.Cdecl)] static extern IntPtr nvapi_QueryInterface(uint id);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Init();
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Create(out IntPtr session);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Session(IntPtr session);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int FileOp(IntPtr session,[MarshalAs(UnmanagedType.LPWStr)] string path);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int FindApp(IntPtr session,[MarshalAs(UnmanagedType.LPWStr)] string name,out IntPtr profile,IntPtr app);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int CreateProfile(IntPtr session,IntPtr info,out IntPtr profile);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int ProfileOp(IntPtr session,IntPtr profile,IntPtr data);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetSetting(IntPtr session,IntPtr profile,uint id,IntPtr data);
 static T Api<T>(uint id) where T:Delegate { var ptr=nvapi_QueryInterface(id); if(ptr==IntPtr.Zero) throw new Exception("NVAPI function unavailable"); return Marshal.GetDelegateForFunctionPointer<T>(ptr); }
 static void Check(int result,string operation) { if(result!=0) throw new Exception(operation+": NVAPI status "+result); }
 sealed class Buffer:IDisposable {
  public IntPtr Ptr; public Buffer(int size) { Ptr=Marshal.AllocHGlobal(size); Marshal.Copy(new byte[size],0,Ptr,size); Marshal.WriteInt32(Ptr,size|0x10000); }
  public void Text(int offset,string value) { var bytes=Encoding.Unicode.GetBytes(value+"\0"); Marshal.Copy(bytes,0,IntPtr.Add(Ptr,offset),bytes.Length); }
  public void Dispose(){Marshal.FreeHGlobal(Ptr);}
 }
 public static string Run(bool apply,string backup) {
  Check(Api<Init>(0x0150e828)(),"initialize"); IntPtr session=IntPtr.Zero;
  try {
   Check(Api<Create>(0x0694d52e)(out session),"create session"); Check(Api<Session>(0x375dbd6b)(session),"load settings");
   using var app=new Buffer(12296); using var info=new Buffer(4116); using var setting=new Buffer(12320);
   IntPtr profile; int found=Api<FindApp>(0xeee566b2)(session,"PrappyPlayer.exe",out profile,app.Ptr);
   if(found!=0 && found!=-166) Check(found,"find executable");
   if(apply) {
    Check(Api<FileOp>(0x2be25df8)(session,backup),"backup all profiles");
    if(!File.Exists(backup) || new FileInfo(backup).Length==0) throw new Exception("Profile backup missing");
    if(found==-166) {
     info.Text(4,"Prappy Player"); Check(Api<CreateProfile>(0xcc176068)(session,info.Ptr,out profile),"create dedicated profile");
     app.Text(8,"PrappyPlayer.exe"); app.Text(4104,"Prappy Player"); Check(Api<ProfileOp>(0x4347a9de)(session,profile,app.Ptr),"attach executable");
    } else {
     Check(Api<ProfileOp>(0x61cd6fd6)(session,profile,info.Ptr),"inspect existing profile");
     // Never alter an existing profile shared with another executable.
     if(Marshal.ReadInt32(info.Ptr,4108)!=1 || Marshal.ReadInt32(info.Ptr,4104)!=0) throw new Exception("Existing profile is shared or predefined; refusing modification");
    }
    Marshal.WriteInt32(setting.Ptr,4100,unchecked((int)0x809D5F60));
    Marshal.WriteInt32(setting.Ptr,8220,0x10000000);
    Check(Api<ProfileOp>(0x577dd202)(session,profile,setting.Ptr),"set per-app overlay exclusion");
    Check(Api<Session>(0xfcbc7e14)(session),"save settings"); Check(Api<Session>(0x375dbd6b)(session),"reload settings");
   } else if(found==-166) return "No PrappyPlayer.exe profile exists.";
   Check(Api<GetSetting>(0x73bf8338)(session,profile,0x809D5F60,setting.Ptr),"read back exclusion");
   var value=unchecked((uint)Marshal.ReadInt32(setting.Ptr,8220));
   if(apply && value!=0x10000000) throw new Exception("Profile value did not persist");
   return "PrappyPlayer.exe: 0x809D5F60=0x"+value.ToString("X8")+(apply ? "; backup: "+backup : "");
  } finally { if(session!=IntPtr.Zero) Api<Session>(0xdad9cff8)(session); Api<Init>(0xd22bdd7e)(); }
 }
}
'@
$backup=Join-Path (Split-Path $PSScriptRoot -Parent) ('build/nvidia-profiles-before-'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'.txt')
New-Item -ItemType Directory -Force (Split-Path $backup) | Out-Null
[PlayerNvidiaProfile]::Run($Apply.IsPresent,$backup)

using System;
using System.Runtime.InteropServices;
using System.Threading;

// Test-only WASAPI loopback capture of the default multimedia output endpoint.
// Measures a known tone, not just whether the application queued PCM.
public sealed class PrappyAudioLoopback : IDisposable {
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class Enumerator {}
    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] interface IDevices {
        void EnumAudioEndpoints(int flow,uint state,out IntPtr collection);
        void GetDefaultAudioEndpoint(int flow,int role,out IDevice device);
    }
    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] interface IDevice {
        void Activate(ref Guid iid,uint context,IntPtr parameters,[MarshalAs(UnmanagedType.IUnknown)] out object result);
    }
    [ComImport, Guid("1CB9AD4C-DBFA-4c32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] interface IAudio {
        void Initialize(int mode,uint flags,long duration,long period,IntPtr format,IntPtr session);
        void GetBufferSize(out uint frames);
        void GetStreamLatency(out long latency);
        void GetCurrentPadding(out uint frames);
        [PreserveSig] int IsFormatSupported(int mode,IntPtr format,out IntPtr closest);
        void GetMixFormat(out IntPtr format);
        void GetDevicePeriod(out long normal,out long minimum);
        void Start();void Stop();void Reset();void SetEventHandle(IntPtr handle);
        void GetService(ref Guid iid,[MarshalAs(UnmanagedType.IUnknown)] out object result);
    }
    [ComImport, Guid("C8ADBD64-E71E-48a0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] interface ICapture {
        void GetBuffer(out IntPtr data,out uint frames,out uint flags,out ulong device,out ulong clock);
        void ReleaseBuffer(uint frames);void GetNextPacketSize(out uint frames);
    }
    readonly Thread worker;
    readonly ManualResetEventSlim ready=new ManualResetEventSlim(false);
    readonly object gate=new object();
    readonly double frequency;
    volatile bool stopping;
    Exception failure;
    long count;
    double[] sine,cosine,power;
    int Best(){int best=0;for(int i=1;i<sine.Length;i++)if(sine[i]*sine[i]+cosine[i]*cosine[i]>sine[best]*sine[best]+cosine[best]*cosine[best])best=i;return best;}
    public PrappyAudioLoopback(double tone){
        frequency=tone;worker=new Thread(Run){IsBackground=true};worker.SetApartmentState(ApartmentState.MTA);worker.Start();
        if(!ready.Wait(10000)){stopping=true;throw new Exception("WASAPI loopback startup timed out");}
        if(failure!=null)throw new Exception("WASAPI loopback failed",failure);
    }
    public void Clear(){lock(gate){count=0;Array.Clear(sine,0,sine.Length);Array.Clear(cosine,0,cosine.Length);Array.Clear(power,0,power.Length);}}
    public double ToneAmplitude {get{lock(gate){if(failure!=null)throw new Exception("Loopback capture failed",failure);return count==0?0:2*Math.Sqrt(sine[Best()]*sine[Best()]+cosine[Best()]*cosine[Best()])/count;}}}
    public double Rms {get{lock(gate)return count==0?0:Math.Sqrt(power[Best()]/count);}}
    public long Frames {get{lock(gate)return count;}}
    void Run(){
        object enumerator=null,device=null,audioObject=null,captureObject=null;IntPtr format=IntPtr.Zero;IAudio audio=null;bool started=false;
        try{
            enumerator=new Enumerator();IDevice endpoint;((IDevices)enumerator).GetDefaultAudioEndpoint(0,1,out endpoint);device=endpoint;
            Guid audioId=typeof(IAudio).GUID;endpoint.Activate(ref audioId,23,IntPtr.Zero,out audioObject);audio=(IAudio)audioObject;
            audio.GetMixFormat(out format);
            int tag=Marshal.ReadInt16(format,0)&65535,channels=Marshal.ReadInt16(format,2)&65535;
            int rate=Marshal.ReadInt32(format,4),stride=Marshal.ReadInt16(format,12)&65535,bits=Marshal.ReadInt16(format,14)&65535;
            if(tag==65534)tag=Marshal.ReadInt32(format,24);
            if((tag!=3||bits!=32)&&(tag!=1||(bits!=16&&bits!=24&&bits!=32)))throw new Exception("Unsupported loopback mix format");
            sine=new double[channels];cosine=new double[channels];power=new double[channels];
            audio.Initialize(0,0x20000,1000000,0,format,IntPtr.Zero);
            Guid captureId=typeof(ICapture).GUID;audio.GetService(ref captureId,out captureObject);var capture=(ICapture)captureObject;
            audio.Start();started=true;ready.Set();
            while(!stopping){
                uint available;capture.GetNextPacketSize(out available);
                if(available==0){Thread.Sleep(5);continue;}
                IntPtr data;uint frames,flags;ulong devicePosition,clock;
                capture.GetBuffer(out data,out frames,out flags,out devicePosition,out clock);
                try{
                    byte[] bytes=new byte[checked((int)frames*stride)];if((flags&2)==0)Marshal.Copy(data,bytes,0,bytes.Length);
                    lock(gate)for(int f=0;f<frames;f++){
                        double phase=2*Math.PI*frequency*count/rate;
                        for(int c=0;c<channels;c++){
                            int at=f*stride+c*(bits/8);
                            double sample=tag==3?BitConverter.ToSingle(bytes,at):bits==16?BitConverter.ToInt16(bytes,at)/32768.0:
                                bits==32?BitConverter.ToInt32(bytes,at)/2147483648.0:
                                ((bytes[at]|bytes[at+1]<<8|bytes[at+2]<<16)<<8)/2147483648.0;
                            power[c]+=sample*sample;sine[c]+=sample*Math.Sin(phase);cosine[c]+=sample*Math.Cos(phase);
                        }
                        count++;
                    }
                }finally{capture.ReleaseBuffer(frames);}
            }
        }catch(Exception e){lock(gate)failure=e;ready.Set();}
        finally{
            if(started)try{audio.Stop();}catch{}
            if(format!=IntPtr.Zero)Marshal.FreeCoTaskMem(format);
            foreach(var obj in new[]{captureObject,audioObject,device,enumerator})if(obj!=null&&Marshal.IsComObject(obj))Marshal.ReleaseComObject(obj);
        }
    }
    public void Dispose(){stopping=true;if(!worker.Join(10000))throw new Exception("Loopback worker failed to stop");ready.Dispose();}
}

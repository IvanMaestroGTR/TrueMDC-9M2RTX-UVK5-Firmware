#!/usr/bin/env python3
"""TMID / FSK audio analyzer. Python 3 + Tkinter only."""
import csv, math, os, struct, wave
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

def load_wav(path):
    with wave.open(path, "rb") as w:
        ch, width, rate, count = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(count)
    if width == 1: vals = [(v-128)/128.0 for v in raw]
    elif width == 2: vals = [v[0]/32768.0 for v in struct.iter_unpack("<h",raw)]
    elif width == 3:
        vals=[]
        for i in range(0,len(raw),3):
            v=raw[i]|raw[i+1]<<8|raw[i+2]<<16
            if v&0x800000: v-=1<<24
            vals.append(v/8388608.0)
    elif width == 4: vals=[v[0]/2147483648.0 for v in struct.iter_unpack("<i",raw)]
    else: raise ValueError("Unsupported PCM WAV sample width")
    if ch>1: vals=[sum(vals[i:i+ch])/ch for i in range(0,len(vals),ch)]
    return vals,rate,ch,width,count

def goertzel(x,rate,freq):
    n=len(x)
    if n<8 or freq<=0 or freq>=rate/2:return 0.0
    mean=sum(x)/n; s1=s2=0.0; coeff=2*math.cos(2*math.pi*freq/rate); norm=0.0
    for i,v in enumerate(x):
        win=0.5-0.5*math.cos(2*math.pi*i/(n-1))
        v=(v-mean)*win; norm+=v*v
        s=v+coeff*s1-s2; s2,s1=s1,s
    return max(0.0,s1*s1+s2*s2-coeff*s1*s2)/(norm+1e-15)

def auto_pair(samples,rate):
    # Estimate the two dominant FSK tones from a short set of overlapping audio windows.
    n=min(len(samples),int(rate*0.35)); x=samples[:n]
    if n<64: raise ValueError("Recording is too short for automatic tone detection")
    # Coarse sweep first, then refine around the two strongest separated peaks.
    scores=[]
    for f in range(500,3001,10): scores.append((goertzel(x,rate,f),f))
    peaks=[]
    for i in range(1,len(scores)-1):
        if scores[i][0]>=scores[i-1][0] and scores[i][0]>=scores[i+1][0]:
            peaks.append(scores[i])
    peaks.sort(reverse=True)
    chosen=[]
    for score,f in peaks:
        if all(abs(f-g)>=180 for _,g in chosen):
            chosen.append((score,f))
        if len(chosen)==2: break
    if len(chosen)<2: raise ValueError("Could not confidently identify two separate tones. Enter Mark and Space manually.")
    fs=sorted(f for _,f in chosen)
    return fs[0],fs[1]

def crc16_modbus(data):
    crc=0xFFFF
    for b in data:
        crc^=b
        for _ in range(8): crc=(crc>>1)^(0xA001 if crc&1 else 0)
    return crc&0xFFFF

def runs(bits):
    if not bits:return []
    out=[]; val=bits[0]; count=1
    for b in bits[1:]:
        if b==val:count+=1
        else:out.append((val,count));val=b;count=1
    out.append((val,count));return out

def uart_candidates(bits,final_stop):
    found=[]
    for phase in range(min(10,len(bits))):
        p=phase; vals=[];bad=0
        while p+9+(1 if final_stop else 0)<=len(bits):
            if bits[p]==0:
                vals.append(sum(bits[p+1+j]<<j for j in range(8)))
                if final_stop and bits[p+9]!=1:bad+=1
                p+=10 if final_stop else 9
            else:p+=1
        if len(vals)>=3:found.append((bad,-len(vals),phase,vals))
    return sorted(found)

class App(tk.Tk):
    def __init__(self):
        super().__init__();self.title("TMID / FSK Bit & Preamble Analyzer");self.geometry("1280x900");self.minsize(980,680)
        self.samples=None;self.rate=None;self.path=None;self.rows=[];self.results=[];self.meta={}
        self._ui()

    def _ui(self):
        bar=ttk.Frame(self,padding=8);bar.pack(fill="x")
        ttk.Button(bar,text="Open PCM WAV…",command=self.open).pack(side="left")
        self.file_label=ttk.Label(bar,text="No recording loaded");self.file_label.pack(side="left",padx=10)
        cfg=ttk.LabelFrame(self,text="Signal settings",padding=8);cfg.pack(fill="x",padx=8,pady=4)
        self.mode=tk.StringVar(value="Manual");self.mark=tk.StringVar(value="1200");self.space=tk.StringVar(value="2200")
        self.baud=tk.StringVar(value="1200");self.offset=tk.StringVar(value="0");self.window=tk.StringVar(value="70")
        self.polarity=tk.StringVar(value="Mark=0");self.finalstop=tk.BooleanVar(value=False)
        fields=[("Frequency mode",self.mode,["Manual","Auto"]),("Mark (Hz)",self.mark,None),("Space (Hz)",self.space,None),
                ("Baud",self.baud,None),("Start offset (ms)",self.offset,None),("Window (%)",self.window,None)]
        for i,(label,var,choices) in enumerate(fields):
            col=i*2
            ttk.Label(cfg,text=label).grid(row=0,column=col,sticky="w",padx=(3,2))
            if choices: ttk.Combobox(cfg,textvariable=var,values=choices,state="readonly",width=9).grid(row=0,column=col+1,padx=(0,8))
            else: ttk.Entry(cfg,textvariable=var,width=9).grid(row=0,column=col+1,padx=(0,8))
        ttk.Label(cfg,text="Bit mapping").grid(row=1,column=0,sticky="w",pady=7)
        ttk.Combobox(cfg,textvariable=self.polarity,values=["Mark=0","Mark=1"],state="readonly",width=9).grid(row=1,column=1,sticky="w")
        ttk.Checkbutton(cfg,text="Expect final UART stop bit",variable=self.finalstop).grid(row=1,column=2,columnspan=2,sticky="w")
        ttk.Button(cfg,text="Analyze",command=self.analyze).grid(row=1,column=9,columnspan=2,sticky="e")
        self.summary=ttk.Label(self,text="Load a WAV file. Warble/preamble is analyzed from the selected start offset.",padding=8);self.summary.pack(fill="x")
        split=ttk.Panedwindow(self,orient="vertical");split.pack(fill="both",expand=True,padx=8,pady=4)
        top=ttk.Frame(split);bottom=ttk.Frame(split);split.add(top,weight=2);split.add(bottom,weight=3)
        wave_frame=ttk.Frame(top);wave_frame.pack(fill="both",expand=True)
        ttk.Label(wave_frame,text="PCM waveform (full recording)").pack(anchor="w")
        self.wave_canvas=tk.Canvas(wave_frame,bg="#101820",height=145,highlightthickness=0)
        self.wave_canvas.pack(fill="both",expand=True)
        ttk.Label(wave_frame,text="Bit decisions").pack(anchor="w")
        self.bit_canvas=tk.Canvas(wave_frame,bg="#101820",height=125,highlightthickness=0)
        self.bit_canvas.pack(fill="both",expand=True)
        self.wave_canvas.bind("<Configure>",self._redraw_waveform)
        cols=("idx","ms","tone","bit","confidence","mark","space")
        self.table=ttk.Treeview(bottom,columns=cols,show="headings",height=10)
        for c,h,w in zip(cols,("Symbol","Time ms","Detected Hz","Bit","Confidence","Mark score","Space score"),(65,95,110,50,90,100,100)):
            self.table.heading(c,text=h);self.table.column(c,width=w,anchor="center")
        sy=ttk.Scrollbar(bottom,orient="vertical",command=self.table.yview);self.table.configure(yscrollcommand=sy.set)
        self.table.pack(side="left",fill="both",expand=True);sy.pack(side="right",fill="y")
        foot=ttk.Frame(self,padding=6);foot.pack(fill="x")
        ttk.Button(foot,text="Export FULL LOG (.txt)…",command=self.export_log).pack(side="left")
        ttk.Button(foot,text="Export bit CSV…",command=self.export_csv).pack(side="left",padx=6)
        ttk.Button(foot,text="Copy log",command=self.copy_log).pack(side="left")
        self.report=tk.Text(self,height=13,wrap="word",font=("Consolas",9));self.report.pack(fill="x",padx=8,pady=(0,8))

    def open(self):
        p=filedialog.askopenfilename(filetypes=[("PCM WAV","*.wav"),("All files","*.*")])
        if not p:return
        try:
            self.samples,self.rate,ch,width,count=load_wav(p);self.path=p
            self.meta={"channels":ch,"sample_width_bytes":width,"frames":count}
            self.file_label.config(text=f"{os.path.basename(p)} | {self.rate} Hz | {ch} ch | {len(self.samples)/self.rate:.3f} s")
            self.analyze()
        except Exception as e:messagebox.showerror("WAV error",str(e))

    def analyze(self):
        if self.samples is None:messagebox.showinfo("No WAV","Open a recording first.");return
        try:
            baud=float(self.baud.get());offset=float(self.offset.get())/1000;pct=float(self.window.get())/100
            if baud<=0 or offset<0 or not .1<=pct<=1:raise ValueError("Check baud, offset, and window (10–100%).")
            if self.mode.get()=="Auto":
                mark,space=auto_pair(self.samples,self.rate);self.mark.set(f"{mark:.0f}");self.space.set(f"{space:.0f}")
            else:mark=float(self.mark.get());space=float(self.space.get())
            if mark<=0 or space<=0 or mark==space or max(mark,space)>=self.rate/2:raise ValueError("Mark and Space must be different positive frequencies below Nyquist.")
        except Exception as e:messagebox.showerror("Settings / auto detect",str(e));return
        period=1/baud;start=offset;n=max(0,int((len(self.samples)/self.rate-start)/period));bits=[];rows=[];half=period*pct/2
        for i in range(n):
            center=start+(i+.5)*period;a=max(0,int((center-half)*self.rate));b=min(len(self.samples),int((center+half)*self.rate))
            seg=self.samples[a:b];pm=goertzel(seg,self.rate,mark);ps=goertzel(seg,self.rate,space)
            mk=pm>=ps;bit=0 if mk else 1
            if self.polarity.get()=="Mark=1":bit=1-bit
            conf=abs(pm-ps)/(pm+ps+1e-15);row=(i,center*1000,mark if mk else space,bit,conf,pm,ps)
            bits.append(bit);rows.append(row)
        self.rows=rows;self.results={"mark":mark,"space":space,"baud":baud,"offset_ms":offset*1000,"window_pct":pct*100,"bits":bits}
        self.table.delete(*self.table.get_children())
        for r in rows:self.table.insert("","end",values=(r[0],f"{r[1]:.3f}",f"{r[2]:.1f}",r[3],f"{r[4]:.3f}",f"{r[5]:.3f}",f"{r[6]:.3f}"))
        self.draw_waveform();self.draw(rows,mark,space);self.make_report()
        avg=sum(r[4] for r in rows)/max(1,len(rows))
        self.summary.config(text=f"{len(rows)} symbols | Mark {mark:g} Hz / Space {space:g} Hz | {baud:g} baud | mean confidence {avg:.3f} | preamble runs shown in log")

    def _redraw_waveform(self,event=None):
        if self.samples is not None:self.draw_waveform()

    def draw_waveform(self):
        c=self.wave_canvas;c.delete("all")
        w=max(1,c.winfo_width());h=max(1,c.winfo_height())
        if not self.samples:return
        left,right,top,bottom=48,w-12,12,h-25
        plot_w=max(1,right-left);plot_h=max(1,bottom-top)
        mid=top+plot_h/2
        # Horizontal amplitude grid and labels.
        for amp in (-1.0,-0.5,0.0,0.5,1.0):
            y=mid-amp*(plot_h/2)
            c.create_line(left,y,right,y,fill="#344552" if amp else "#718392",dash=(2,3) if amp else ())
            c.create_text(left-7,y,text=f"{amp:g}",anchor="e",fill="#c6d0d8",font=("TkDefaultFont",8))
        # Min/max decimation keeps narrow pulses visible without drawing every PCM sample.
        total=len(self.samples)
        for px in range(plot_w):
            a=int(px*total/plot_w);b=max(a+1,int((px+1)*total/plot_w))
            chunk=self.samples[a:min(total,b)]
            if not chunk:continue
            lo=min(chunk);hi=max(chunk)
            y1=mid-hi*(plot_h/2);y2=mid-lo*(plot_h/2)
            x=left+px
            c.create_line(x,y1,x,y2,fill="#40d6a2",width=1)
        duration=total/self.rate
        for tick in range(6):
            x=left+plot_w*tick/5
            c.create_line(x,top,x,bottom,fill="#263641",dash=(2,4))
            c.create_text(x,bottom+4,text=f"{duration*tick/5*1000:.0f} ms",anchor="n",fill="#c6d0d8",font=("TkDefaultFont",8))
        c.create_text(5,3,anchor="nw",fill="white",text=f"Amplitude | {duration*1000:.1f} ms total")

    def draw(self,rows,mark,space):
        self.bit_canvas.delete("all");w=max(1,self.bit_canvas.winfo_width());h=max(1,self.bit_canvas.winfo_height())
        if not rows:return
        self.bit_canvas.create_text(10,8,anchor="nw",fill="white",text="Symbol decisions from selected start offset (includes warble if offset starts at recording beginning)")
        step=max(1,(w-30)/len(rows));top=30;usable=h-55
        for r in rows:
            x=15+r[0]*step;y=top if r[3]==0 else top+usable*.55;color="#40d6a2" if r[3]==0 else "#ffb454"
            self.bit_canvas.create_rectangle(x,y,x+max(1,step-1),y+usable*.38,fill=color,outline="")
            if step>12:self.bit_canvas.create_text(x+step/2,y+usable*.19,text=str(r[3]),fill="#101820")

    def make_report(self):
        z=self.results;bits=z["bits"];rr=runs(bits);avg=sum(r[4] for r in self.rows)/max(1,len(self.rows))
        lines=["TMID / FSK ANALYSIS LOG",f"Input: {self.path}",f"Sample rate: {self.rate} Hz",f"Channels: {self.meta.get('channels')}",f"Sample width: {self.meta.get('sample_width_bytes')} bytes",
               f"Duration: {len(self.samples)/self.rate:.6f} s",f"Mark: {z['mark']} Hz",f"Space: {z['space']} Hz",f"Baud: {z['baud']}",f"Start offset: {z['offset_ms']} ms",
               f"Analysis window: {z['window_pct']}%",f"Mapping: {self.polarity.get()}",f"Mean confidence: {avg:.4f}","","BIT STREAM (chronological):","".join(map(str,bits)),"",
               "PREAMBLE / WARBLE RUN-LENGTH VIEW (bit, symbol-count):"," ".join(f"{v}:{n}" for v,n in rr),
               "RUN-LENGTH ONLY:"," ".join(str(n) for _,n in rr),"",
               "RAW 8-BIT LSB-FIRST GROUPS:"," ".join(f"{b:02X}" for b in [sum(bits[i+j]<<j for j in range(8)) for i in range(0,len(bits)-7,8)]),"",
               "PER-SYMBOL DETAIL:","index,time_ms,tone_hz,bit,confidence,mark_score,space_score"]
        lines += [f"{r[0]},{r[1]:.4f},{r[2]:.1f},{r[3]},{r[4]:.5f},{r[5]:.5f},{r[6]:.5f}" for r in self.rows]
        lines += ["","UART FRAMING CANDIDATES:"]
        cand=uart_candidates(bits,self.finalstop.get())
        if cand:
            for bad,neg,phase,vals in cand[:5]:
                lines.append(f"phase={phase}; bytes={len(vals)}; invalid_stop={bad}; hex="+" ".join(f"{v:02X}" for v in vals))
                if len(vals)>=9:
                    payload=bytes(vals[:7]);got=vals[7]|(vals[8]<<8);calc=crc16_modbus(payload)
                    lines.append(f"TMID hypothesis callsign={payload[:6]!r} SSID={payload[6:7]!r}; CRC received={got:04X} calculated={calc:04X} {'VALID' if got==calc else 'INVALID'}")
        else:lines.append("No UART candidate found.")
        lines += ["","NOTE: Audio-side analysis only. It does not emulate BK4819 sync detection, FIFO packing, or IRQ behavior."]
        self.log_text="\n".join(lines)
        self.report.delete("1.0","end");self.report.insert("end",self.log_text)

    def export_log(self):
        if not self.results:return
        p=filedialog.asksaveasfilename(defaultextension=".txt",filetypes=[("Text log","*.txt")],initialfile="tmid_fsk_analysis.txt")
        if not p:return
        try:
            with open(p,"w",encoding="utf-8",newline="\n") as f:f.write(self.log_text+"\n")
            messagebox.showinfo("Log exported",p)
        except Exception as e:messagebox.showerror("Export failed",str(e))

    def export_csv(self):
        if not self.rows:return
        p=filedialog.asksaveasfilename(defaultextension=".csv",filetypes=[("CSV","*.csv")],initialfile="tmid_fsk_bits.csv")
        if not p:return
        with open(p,"w",newline="",encoding="utf-8") as f:
            w=csv.writer(f);w.writerow(["symbol_index","time_ms","detected_tone_hz","bit","confidence","mark_score","space_score"]);w.writerows(self.rows)
        messagebox.showinfo("CSV exported",p)

    def copy_log(self):
        if not self.results:return
        self.clipboard_clear();self.clipboard_append(self.log_text);messagebox.showinfo("Copied","Full diagnostic log copied.")

if __name__=="__main__":App().mainloop()

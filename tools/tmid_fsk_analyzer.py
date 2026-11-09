#!/usr/bin/env python3
"""TMID / multi-pair FSK WAV analyzer. Python 3 + Tkinter only."""
import csv, math, os, struct, wave
import tkinter as tk
from tkinter import ttk, filedialog, messagebox

PRESETS = "TMID 1200/2200:1200:2200\nMDC 1200/1800:1200:1800\n1200/2400:1200:2400\n1200/2100:1200:2100"

def load_wav(path):
    with wave.open(path, "rb") as w:
        channels, width, rate, count = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(count)
    if width == 1:
        vals = [(v - 128) / 128.0 for v in raw]
    elif width == 2:
        vals = [v[0] / 32768.0 for v in struct.iter_unpack("<h", raw)]
    elif width == 3:
        vals = []
        for i in range(0, len(raw), 3):
            v = raw[i] | raw[i+1] << 8 | raw[i+2] << 16
            if v & 0x800000: v -= 1 << 24
            vals.append(v / 8388608.0)
    elif width == 4:
        vals = [v[0] / 2147483648.0 for v in struct.iter_unpack("<i", raw)]
    else:
        raise ValueError("Unsupported PCM WAV sample width")
    if channels > 1:
        vals = [sum(vals[i:i+channels]) / channels for i in range(0, len(vals), channels)]
    return vals, rate

def goertzel_power(x, rate, freq):
    n = len(x)
    if n < 8 or freq >= rate / 2: return 0.0
    mean = sum(x) / n
    # Hann window suppresses energy leakage from adjacent symbols.
    s1 = s2 = 0.0
    coeff = 2.0 * math.cos(2.0 * math.pi * freq / rate)
    norm = 0.0
    for i, value in enumerate(x):
        win = 0.5 - 0.5 * math.cos(2.0 * math.pi * i / (n - 1))
        sample = (value - mean) * win
        norm += sample * sample
        s = sample + coeff * s1 - s2
        s2, s1 = s1, s
    power = s1*s1 + s2*s2 - coeff*s1*s2
    return max(0.0, power) / (norm + 1e-15)

def crc16_modbus(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xA001 if crc & 1 else 0)
    return crc & 0xFFFF

def bits_to_bytes(bits, lsb=True):
    out=[]
    for i in range(0, len(bits)-7, 8):
        chunk=bits[i:i+8]
        out.append(sum(int(b) << (j if lsb else 7-j) for j,b in enumerate(chunk)))
    return out

def uart_candidates(bits, final_stop=True):
    found=[]
    for phase in range(min(10, len(bits))):
        p=phase; vals=[]; bad=0; starts=0
        while p+9+(1 if final_stop else 0) <= len(bits):
            if bits[p] == 0:
                val=sum(bits[p+1+j] << j for j in range(8))
                vals.append(val); starts += 1
                if final_stop and bits[p+9] != 1: bad += 1
                p += 10 if final_stop else 9
            else: p += 1
        if len(vals) >= 3:
            found.append((bad, -len(vals), phase, vals))
    return sorted(found)

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("TMID / Multi-pair FSK Analyzer")
        self.geometry("1260x900")
        self.minsize(980, 680)
        self.samples=None; self.rate=None; self.path=None; self.rows=[]
        self._ui()

    def _ui(self):
        bar=ttk.Frame(self,padding=8); bar.pack(fill="x")
        ttk.Button(bar,text="Open PCM WAV…",command=self.open).pack(side="left")
        self.file_label=ttk.Label(bar,text="No file loaded"); self.file_label.pack(side="left",padx=10)
        cfg=ttk.LabelFrame(self,text="Decoder settings",padding=8); cfg.pack(fill="x",padx=8,pady=4)
        self.pairs=tk.StringVar(value=PRESETS)
        ttk.Label(cfg,text="Tone pairs (one per line: name:mark:space)").grid(row=0,column=0,sticky="w")
        ttk.Entry(cfg,textvariable=self.pairs,width=66).grid(row=0,column=1,columnspan=5,sticky="ew",padx=5)
        self.baud=tk.StringVar(value="1200"); self.offset=tk.StringVar(value="0")
        self.window=tk.StringVar(value="70"); self.polarity=tk.StringVar(value="Mark=0")
        self.finalstop=tk.BooleanVar(value=True)
        for c,(label,var,width) in enumerate([("Baud",self.baud,8),("Start offset ms",self.offset,10),("Window %",self.window,8)]):
            col=0 if c==0 else 2*c
            ttk.Label(cfg,text=label).grid(row=1,column=col,sticky="w",pady=6)
            ttk.Entry(cfg,textvariable=var,width=width).grid(row=1,column=col+1,sticky="w")
        ttk.Label(cfg,text="Bit mapping").grid(row=1,column=6,sticky="e")
        ttk.Combobox(cfg,textvariable=self.polarity,values=["Mark=0","Mark=1"],state="readonly",width=10).grid(row=1,column=7,padx=4)
        ttk.Checkbutton(cfg,text="Expect stop bits (except optional final byte)",variable=self.finalstop).grid(row=2,column=0,columnspan=3,sticky="w")
        ttk.Button(cfg,text="Analyze all tone pairs",command=self.analyze).grid(row=2,column=6,columnspan=2,sticky="e")
        self.summary=ttk.Label(self,text="Load a WAV recording to begin.",padding=8); self.summary.pack(fill="x")
        split=ttk.Panedwindow(self,orient="vertical"); split.pack(fill="both",expand=True,padx=8,pady=4)
        upper=ttk.Frame(split); lower=ttk.Frame(split); split.add(upper,weight=3); split.add(lower,weight=2)
        self.canvas=tk.Canvas(upper,bg="#101820",height=260,highlightthickness=0); self.canvas.pack(fill="both",expand=True)
        cols=("n","ms","tone","bit","confidence","mark_score","space_score")
        self.table=ttk.Treeview(lower,columns=cols,show="headings",height=12)
        for c,h,w in zip(cols,("Bit #","Time ms","Chosen tone Hz","Bit","Confidence","Mark score","Space score"),(70,100,120,55,100,110,110)):
            self.table.heading(c,text=h); self.table.column(c,width=w,anchor="center")
        sy=ttk.Scrollbar(lower,orient="vertical",command=self.table.yview); self.table.configure(yscrollcommand=sy.set)
        self.table.pack(side="left",fill="both",expand=True); sy.pack(side="right",fill="y")
        foot=ttk.Frame(self,padding=6); foot.pack(fill="x")
        ttk.Button(foot,text="Export bit decisions CSV…",command=self.export).pack(side="left")
        ttk.Button(foot,text="Copy report",command=self.copy).pack(side="left",padx=6)
        self.report=tk.Text(self,height=11,wrap="word",font=("Consolas",10)); self.report.pack(fill="x",padx=8,pady=(0,8))

    def open(self):
        path=filedialog.askopenfilename(filetypes=[("WAV files","*.wav"),("All files","*.*")])
        if not path:return
        try:
            self.samples,self.rate=load_wav(path); self.path=path
            self.file_label.config(text=f"{os.path.basename(path)} | {self.rate} Hz | {len(self.samples)/self.rate:.3f} s")
            self.analyze()
        except Exception as e: messagebox.showerror("WAV load failed",str(e))

    def parse_pairs(self):
        pairs=[]
        for line in self.pairs.get().splitlines():
            line=line.strip()
            if not line: continue
            parts=line.split(":")
            if len(parts)!=3: raise ValueError("Each tone-pair line must be name:mark:space")
            name,mark,space=parts[0],float(parts[1]),float(parts[2])
            if mark<=0 or space<=0 or mark==space: raise ValueError("Tone frequencies must be positive and different")
            if self.rate and max(mark,space)>=self.rate/2: raise ValueError(f"{name}: tone must be below Nyquist ({self.rate/2:g} Hz)")
            pairs.append((name,mark,space))
        if not pairs: raise ValueError("Enter at least one tone pair")
        return pairs

    def analyze(self):
        if self.samples is None:
            messagebox.showinfo("No WAV","Open a WAV file first."); return
        try:
            baud=float(self.baud.get()); offset=float(self.offset.get())/1000
            pct=float(self.window.get())/100
            pairs=self.parse_pairs()
            if baud<=0 or not 0.1<=pct<=1: raise ValueError("Baud must be positive and window must be 10–100%")
        except Exception as e: messagebox.showerror("Settings",str(e)); return
        period=1/baud; start=max(0,offset); n=max(0,int((len(self.samples)/self.rate-start)/period))
        all_results=[]
        for name,mark,space in pairs:
            bits=[]; rows=[]; half=period*pct/2
            for i in range(n):
                center=start+(i+.5)*period
                a=max(0,int((center-half)*self.rate)); b=min(len(self.samples),int((center+half)*self.rate))
                seg=self.samples[a:b]
                pm=goertzel_power(seg,self.rate,mark); ps=goertzel_power(seg,self.rate,space)
                mk=pm>=ps; bit=(0 if mk else 1)
                if self.polarity.get()=="Mark=1": bit=1-bit
                conf=abs(pm-ps)/(pm+ps+1e-15)
                row=(i,center*1000,mark if mk else space,bit,conf,pm,ps)
                bits.append(bit); rows.append(row)
            all_results.append((name,mark,space,bits,rows))
        # Default display first tone pair, retain every candidate report.
        self.rows=all_results[0][4]
        self.table.delete(*self.table.get_children())
        for r in self.rows:
            self.table.insert("","end",values=(r[0],f"{r[1]:.3f}",f"{r[2]:.1f}",r[3],f"{r[4]:.3f}",f"{r[5]:.3f}",f"{r[6]:.3f}"))
        self.draw(all_results[0][4],pairs[0][1:])
        self.write_report(all_results,baud)
        means=[sum(r[4] for r in z[4])/max(1,len(z[4])) for z in all_results]
        self.summary.config(text=f"{len(self.rows)} bit periods | {baud:g} baud | {len(pairs)} tone-pair candidates | best mean confidence: {all_results[means.index(max(means))][0]} ({max(means):.3f})")

    def draw(self,rows,pair):
        self.canvas.delete("all")
        w=max(1,self.canvas.winfo_width()); h=max(1,self.canvas.winfo_height())
        if not rows:return
        top=20; bottom=h-32; usable=bottom-top
        self.canvas.create_text(12,8,anchor="nw",fill="white",text="Bit decisions for first tone pair — each vertical cell is one 1200-baud symbol")
        step=max(1,(w-40)/len(rows))
        for r in rows:
            x=30+r[0]*step; y=top if r[3]==0 else top+usable*.55
            color="#40d6a2" if r[3]==0 else "#ffb454"
            self.canvas.create_rectangle(x,y,x+max(1,step-1),y+usable*.38,fill=color,outline="")
            if step>12:self.canvas.create_text(x+step/2,y+usable*.19,text=str(r[3]),fill="#101820")
        self.canvas.create_text(30,bottom+15,anchor="w",fill="white",text=f"Mark {pair[0]} Hz = bit {0 if self.polarity.get()=='Mark=0' else 1}; Space {pair[1]} Hz = bit {1 if self.polarity.get()=='Mark=0' else 0}")

    def write_report(self,results,baud):
        self.report.delete("1.0","end")
        for name,mark,space,bits,rows in results:
            avg=sum(r[4] for r in rows)/max(1,len(rows))
            self.report.insert("end",f"\n=== {name}: mark {mark:g} / space {space:g} Hz | mean confidence {avg:.3f} ===\n")
            self.report.insert("end","BITS: "+"".join(map(str,bits))+"\n")
            raw=bits_to_bytes(bits)
            self.report.insert("end","RAW 8-bit LSB-first groups: "+" ".join(f"{b:02X}" for b in raw)+"\n")
            self.report.insert("end","ASCII: "+"".join(chr(b) if 32<=b<127 else "." for b in raw)+"\n")
            candidates=uart_candidates(bits,self.finalstop.get())
            if not candidates:
                self.report.insert("end","UART: no plausible start-bit framing found.\n")
            else:
                bad,neg_n,phase,vals=candidates[0]
                self.report.insert("end",f"UART best guess: phase={phase}, bytes={len(vals)}, invalid stop bits={bad}\n")
                self.report.insert("end","UART bytes: "+" ".join(f"{b:02X}" for b in vals)+"\n")
                self.report.insert("end","UART ASCII: "+"".join(chr(b) if 32<=b<127 else "." for b in vals)+"\n")
                if len(vals)>=9:
                    payload=bytes(vals[:7]); received=vals[7] | vals[8]<<8; calculated=crc16_modbus(payload)
                    self.report.insert("end",f"TMID payload guess: callsign={payload[:6]!r}, SSID={payload[6:7]!r}\n")
                    self.report.insert("end",f"CRC16/MODBUS LE: received={received:04X}, calculated={calculated:04X} => {'VALID' if received==calculated else 'INVALID'}\n")
            # Search exact raw sync in either bit orientation.
            text="".join(map(str,bits))
            for pattern,label in (("1010010110100101","A5 A5 LSB-first"),("1010010110100101"[::-1],"A5 A5 reversed")):
                pos=text.find(pattern)
                if pos>=0:self.report.insert("end",f"Sync pattern {label} found at bit {pos}\n")
        self.report.insert("end","\nImportant: this is an audio-side diagnostic. It does not emulate BK4819 REG_5A sync detection, FIFO byte packing, IRQ timing, or the chip's internal demodulator. Compare the decoded bitstream against a known-good capture and sweep Start offset in small increments (e.g. 0.1 ms). A CRC pass is strong evidence the audio framing is aligned; a CRC fail alone does not prove the transmitted packet is malformed.\n")

    def export(self):
        if not self.rows: return
        path=filedialog.asksaveasfilename(defaultextension=".csv",filetypes=[("CSV","*.csv")])
        if not path:return
        with open(path,"w",newline="",encoding="utf-8") as f:
            writer=csv.writer(f); writer.writerow(["bit_index","time_ms","detected_tone_hz","bit","confidence","mark_score","space_score"])
            writer.writerows(self.rows)
        messagebox.showinfo("Exported",path)

    def copy(self):
        text=self.report.get("1.0","end-1c")
        self.clipboard_clear(); self.clipboard_append(text)
        messagebox.showinfo("Copied","Analysis report copied to clipboard.")

if __name__=="__main__":
    App().mainloop()
